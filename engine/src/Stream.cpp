#include "pulp/Stream.h"
#include <cstdio>
#include <thread>

namespace pulp {

namespace {
std::atomic<int64> gStreamAbove{(int64)48000 * 45};     // about 45 seconds
std::atomic<int> gPageFrames{65536};
constexpr uint32 kKeepEpochs = 500;                     // a page nobody asked for in this many passes is dropped
constexpr int kPageBudget = 48;                         // pages kept per stream before the oldest go early

// One loader thread for every open stream.
struct Loader {
    std::mutex m;
    std::vector<std::weak_ptr<AudioStream>> streams;
    std::thread thread;
    std::atomic<bool> quit{false};
    void add(const std::shared_ptr<AudioStream>& s) {
        std::lock_guard<std::mutex> g(m);
        streams.push_back(s);
        if (!thread.joinable()) thread = std::thread([this] { run(); });
    }
    void run() {
        std::vector<std::shared_ptr<AudioStream>> live;
        while (!quit.load()) {
            {
                std::lock_guard<std::mutex> g(m);
                live.clear();
                size_t w = 0;
                for (size_t i = 0; i < streams.size(); ++i)
                    if (auto s = streams[i].lock()) { live.push_back(std::move(s)); streams[w++] = streams[i]; }
                streams.resize(w);
            }
            for (auto& s : live) s->service();
            live.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    }
    ~Loader() {
        quit.store(true);
        if (thread.joinable()) thread.join();
    }
};
Loader& loader() { static Loader l; return l; }
}

int64 AudioStream::streamAbove() { return gStreamAbove.load(); }
void AudioStream::setStreamAbove(int64 frames) { gStreamAbove.store(frames); }
int AudioStream::pageFrames() { return gPageFrames.load(); }
void AudioStream::setPageFrames(int frames) { gPageFrames.store(std::max(64, frames)); }

std::shared_ptr<AudioStream> AudioStream::open(const std::string& path, std::string* error) {
    WavInfo info;
    if (!readWavInfo(path, info, error)) return nullptr;
    FILE* f = (FILE*)openFileForRead(path);
    if (!f) { if (error) *error = "cannot open " + path; return nullptr; }
    std::shared_ptr<AudioStream> s(new AudioStream());
    s->info_ = info;
    s->file_ = f;
    s->path = path;
    s->sampleRate = info.rate;
    s->channels = info.channels > 1 ? 2 : 1;
    s->frames = info.frames();
    s->pageFrames_ = pageFrames();
    s->numPages_ = (int)((s->frames + s->pageFrames_ - 1) / s->pageFrames_);
    s->pages_ = std::make_unique<std::atomic<float*>[]>((size_t)std::max(1, s->numPages_));
    s->stamp_ = std::make_unique<std::atomic<uint32>[]>((size_t)std::max(1, s->numPages_));
    for (int i = 0; i < std::max(1, s->numPages_); ++i) { s->pages_[(size_t)i].store(nullptr); s->stamp_[(size_t)i].store(0); }

    // one pass over the file for the waveform overview and the peak
    const int64 bins = (s->frames + kOverviewBin - 1) / kOverviewBin;
    s->ovMin.assign((size_t)bins, 0.0f);
    s->ovMax.assign((size_t)bins, 0.0f);
    const int chunk = kOverviewBin * 64;
    std::vector<unsigned char> raw((size_t)chunk * info.blockAlign);
    std::vector<float> l((size_t)chunk), r((size_t)chunk);
    _fseeki64(f, info.dataOffset, SEEK_SET);
    for (int64 at = 0; at < s->frames;) {
        const int want = (int)std::min<int64>(chunk, s->frames - at);
        const int got = (int)(fread(raw.data(), info.blockAlign, (size_t)want, f));
        if (got <= 0) break;
        decodeWavFrames(raw.data(), info, (size_t)got, l.data(), s->channels > 1 ? r.data() : nullptr);
        for (int i = 0; i < got; ++i) {
            const size_t b = (size_t)((at + i) / kOverviewBin);
            float lo = l[(size_t)i], hi = lo;
            if (s->channels > 1) { lo = std::min(lo, r[(size_t)i]); hi = std::max(hi, r[(size_t)i]); }
            if (lo < s->ovMin[b]) s->ovMin[b] = lo;
            if (hi > s->ovMax[b]) s->ovMax[b] = hi;
            s->peak = std::max(s->peak, std::max(std::fabs(lo), std::fabs(hi)));
        }
        at += got;
    }
    loader().add(s);
    return s;
}

AudioStream::~AudioStream() {
    for (int i = 0; i < numPages_; ++i) delete[] pages_[(size_t)i].load();
    if (file_) fclose((FILE*)file_);
}

bool AudioStream::loadPage(int page) {
    if (page < 0 || page >= numPages_) return false;
    if (pages_[(size_t)page].load(std::memory_order_seq_cst) != nullptr) return true;
    const int64 first = (int64)page * pageFrames_;
    const int n = (int)std::min<int64>(pageFrames_, frames - first);
    auto* buf = new float[(size_t)pageFrames_ * (size_t)channels]();
    std::vector<unsigned char> raw((size_t)n * info_.blockAlign);
    size_t got = 0;
    {
        std::lock_guard<std::mutex> g(fileMutex_);
        _fseeki64((FILE*)file_, info_.dataOffset + first * (int64)info_.blockAlign, SEEK_SET);
        got = fread(raw.data(), info_.blockAlign, (size_t)n, (FILE*)file_);
    }
    decodeWavFrames(raw.data(), info_, got, buf, channels > 1 ? buf + pageFrames_ : nullptr);
    float* expected = nullptr;
    if (!pages_[(size_t)page].compare_exchange_strong(expected, buf)) delete[] buf;     // someone else loaded it meanwhile
    return true;
}

bool AudioStream::read(int64 start, int n, float* l, float* r, bool blocking) {
    bool complete = true;
    // While the pin is held the loader will not free a page it has taken out of the table.
    pins_.fetch_add(1, std::memory_order_seq_cst);
    const uint32 ep = epoch_.load(std::memory_order_relaxed);
    int done = 0;
    while (done < n) {
        const int64 pos = start + done;
        if (pos < 0 || pos >= frames) {
            const int run = pos < 0 ? (int)std::min<int64>(n - done, -pos) : n - done;
            std::memset(l + done, 0, sizeof(float) * (size_t)run);
            std::memset(r + done, 0, sizeof(float) * (size_t)run);
            done += run;
            continue;
        }
        const int page = (int)(pos / pageFrames_);
        const int off = (int)(pos - (int64)page * pageFrames_);
        const int run = (int)std::min<int64>(std::min(n - done, pageFrames_ - off), frames - pos);
        // this page and its neighbours are wanted: the loader reads ahead (and behind, for reversed clips)
        stamp_[(size_t)page].store(ep, std::memory_order_relaxed);
        if (page + 1 < numPages_) stamp_[(size_t)page + 1].store(ep, std::memory_order_relaxed);
        if (page > 0) stamp_[(size_t)page - 1].store(ep, std::memory_order_relaxed);
        const float* p = pages_[(size_t)page].load(std::memory_order_seq_cst);
        if (p == nullptr && blocking) {
            Rt::Suspend notRealTime;
            loadPage(page);
            p = pages_[(size_t)page].load(std::memory_order_seq_cst);
        }
        if (p != nullptr) {
            std::memcpy(l + done, p + off, sizeof(float) * (size_t)run);
            std::memcpy(r + done, (channels > 1 ? p + pageFrames_ : p) + off, sizeof(float) * (size_t)run);
        } else {
            std::memset(l + done, 0, sizeof(float) * (size_t)run);
            std::memset(r + done, 0, sizeof(float) * (size_t)run);
            complete = false;
        }
        done += run;
    }
    pins_.fetch_sub(1, std::memory_order_seq_cst);
    if (!complete) misses_.fetch_add(1, std::memory_order_relaxed);
    return complete;
}

void AudioStream::prefetch(int64 frame) {
    if (numPages_ <= 0) return;
    const int page = (int)clampT<int64>(frame / pageFrames_, 0, numPages_ - 1);
    const uint32 ep = epoch_.load(std::memory_order_relaxed);
    for (int p = std::max(0, page - 1); p <= std::min(numPages_ - 1, page + 1); ++p) stamp_[(size_t)p].store(ep, std::memory_order_relaxed);
}

void AudioStream::setHeads(std::vector<int64> headFrames) {
    std::vector<int> pages;
    for (int64 f : headFrames) {
        if (numPages_ <= 0) break;
        const int p = (int)clampT<int64>(f / pageFrames_, 0, numPages_ - 1);
        pages.push_back(p);
        if (p + 1 < numPages_) pages.push_back(p + 1);
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    std::lock_guard<std::mutex> g(serviceMutex_);
    heads_ = std::move(pages);
}

void AudioStream::service() {
    std::lock_guard<std::mutex> g(serviceMutex_);
    const uint32 ep = epoch_.load(std::memory_order_relaxed);
    auto isHead = [&](int p) { return std::binary_search(heads_.begin(), heads_.end(), p); };
    auto drop = [&](int p) {
        float* d = pages_[(size_t)p].exchange(nullptr, std::memory_order_seq_cst);
        if (!d) return;
        while (pins_.load(std::memory_order_seq_cst) != 0) std::this_thread::yield();      // a reader may still be copying from it
        delete[] d;
    };
    int loaded = 0;
    for (int p = 0; p < numPages_; ++p) {
        const uint32 age = ep - stamp_[(size_t)p].load(std::memory_order_relaxed);
        const bool head = isHead(p);
        const bool have = pages_[(size_t)p].load(std::memory_order_relaxed) != nullptr;
        if (!have && (age <= 2 || head)) { loadPage(p); ++loaded; }
        else if (have && !head && age > kKeepEpochs) drop(p);
        else if (have) ++loaded;
    }
    // over budget: let the pages go that were wanted longest ago
    while (loaded > kPageBudget) {
        int oldest = -1;
        uint32 oldestAge = 2;
        for (int p = 0; p < numPages_; ++p) {
            if (pages_[(size_t)p].load(std::memory_order_relaxed) == nullptr || isHead(p)) continue;
            const uint32 age = ep - stamp_[(size_t)p].load(std::memory_order_relaxed);
            if (age > oldestAge) { oldestAge = age; oldest = p; }
        }
        if (oldest < 0) break;
        drop(oldest);
        --loaded;
    }
    epoch_.store(ep + 1, std::memory_order_relaxed);
}

int AudioStream::pagesLoaded() const {
    int n = 0;
    for (int p = 0; p < numPages_; ++p) n += pages_[(size_t)p].load(std::memory_order_relaxed) != nullptr ? 1 : 0;
    return n;
}

std::shared_ptr<SampleData> AudioStream::loadAll(std::string* error) const {
    auto s = readWavFile(path, error);
    return s;
}

} // namespace pulp
