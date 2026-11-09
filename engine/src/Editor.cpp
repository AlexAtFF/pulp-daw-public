#include "pulp/Editor.h"
#include "pulp/Dsp.h"
#include "pulp/Effects.h"

namespace pulp::edit {

namespace {
void clampRange(const SampleData& s, int64& a, int64& b) {
    const int64 n = s.frames();
    if (b <= a) { a = 0; b = n; }           // nothing selected: everything
    a = clampT<int64>(a, 0, n);
    b = clampT<int64>(b, a, n);
}
std::shared_ptr<SampleData> copyOf(const SampleData& s) {
    auto o = std::make_shared<SampleData>();
    o->sampleRate = s.sampleRate;
    o->channels = s.channels;
    for (int c = 0; c < s.channels; ++c) o->ch[c] = s.ch[c];
    return o;
}
}

std::shared_ptr<SampleData> slice(const SampleData& s, int64 a, int64 b) {
    clampRange(s, a, b);
    auto o = std::make_shared<SampleData>();
    o->sampleRate = s.sampleRate;
    o->channels = s.channels;
    for (int c = 0; c < s.channels; ++c) o->ch[c].assign(s.ch[c].begin() + a, s.ch[c].begin() + b);
    return o;
}
std::shared_ptr<SampleData> trim(const SampleData& s, int64 a, int64 b) { return slice(s, a, b); }

std::shared_ptr<SampleData> cut(const SampleData& s, int64 a, int64 b) {
    clampRange(s, a, b);
    auto o = copyOf(s);
    for (int c = 0; c < s.channels; ++c) o->ch[c].erase(o->ch[c].begin() + a, o->ch[c].begin() + b);
    return o;
}

std::shared_ptr<SampleData> silence(const SampleData& s, int64 a, int64 b) {
    clampRange(s, a, b);
    auto o = copyOf(s);
    for (int c = 0; c < s.channels; ++c) std::fill(o->ch[c].begin() + a, o->ch[c].begin() + b, 0.0f);
    return o;
}

std::shared_ptr<SampleData> fade(const SampleData& s, int64 a, int64 b, bool fadeIn) {
    clampRange(s, a, b);
    auto o = copyOf(s);
    const double len = (double)std::max<int64>(1, b - a);
    for (int c = 0; c < s.channels; ++c)
        for (int64 i = a; i < b; ++i) {
            const double t = (double)(i - a) / len;
            o->ch[c][(size_t)i] *= (float)(fadeIn ? t : 1.0 - t);
        }
    return o;
}

std::shared_ptr<SampleData> gain(const SampleData& s, int64 a, int64 b, float g) {
    clampRange(s, a, b);
    auto o = copyOf(s);
    for (int c = 0; c < s.channels; ++c)
        for (int64 i = a; i < b; ++i) o->ch[c][(size_t)i] *= g;
    return o;
}

std::shared_ptr<SampleData> normalize(const SampleData& s, int64 a, int64 b, float peak) {
    clampRange(s, a, b);
    float p = 0.0f;
    for (int c = 0; c < s.channels; ++c)
        for (int64 i = a; i < b; ++i) p = std::max(p, std::fabs(s.ch[c][(size_t)i]));
    if (p < 1.0e-9f) return copyOf(s);
    return gain(s, a, b, peak / p);
}

std::shared_ptr<SampleData> reverse(const SampleData& s, int64 a, int64 b) {
    clampRange(s, a, b);
    auto o = copyOf(s);
    for (int c = 0; c < s.channels; ++c) std::reverse(o->ch[c].begin() + a, o->ch[c].begin() + b);
    return o;
}

// ---- noise removal: spectral subtraction in overlapping windows ----

namespace {
constexpr int kN = 2048, kHop = kN / 4, kBins = kN / 2 + 1;
std::vector<float> rootHann() {
    std::vector<float> w((size_t)kN);
    for (int i = 0; i < kN; ++i) w[(size_t)i] = (float)std::sqrt(0.5 - 0.5 * std::cos(kTwoPi * i / kN));
    return w;
}
}

NoiseProfile learnNoise(const SampleData& s, int64 a, int64 b) {
    clampRange(s, a, b);
    NoiseProfile p;
    if (b - a < kN) return p;
    const auto w = rootHann();
    std::vector<float> re((size_t)kN), im((size_t)kN);
    std::vector<double> sum((size_t)kBins, 0.0);
    int count = 0;
    for (int c = 0; c < s.channels; ++c)
        for (int64 at = a; at + kN <= b; at += kHop) {
            for (int i = 0; i < kN; ++i) { re[(size_t)i] = s.ch[c][(size_t)(at + i)] * w[(size_t)i]; im[(size_t)i] = 0.0f; }
            dsp::fft(re.data(), im.data(), kN, false);
            for (int k = 0; k < kBins; ++k) sum[(size_t)k] += std::sqrt((double)re[(size_t)k] * re[(size_t)k] + (double)im[(size_t)k] * im[(size_t)k]);
            ++count;
        }
    if (count == 0) return p;
    p.fftSize = kN;
    p.sampleRate = s.sampleRate;
    p.mag.resize((size_t)kBins);
    for (int k = 0; k < kBins; ++k) p.mag[(size_t)k] = (float)(sum[(size_t)k] / count);
    return p;
}

std::shared_ptr<SampleData> removeNoise(const SampleData& s, int64 a, int64 b, const NoiseProfile& profile, float amount) {
    clampRange(s, a, b);
    auto o = copyOf(s);
    if (!profile.valid() || profile.fftSize != kN || b - a < kN) return o;
    amount = clampT(amount, 0.0f, 1.0f);
    const auto w = rootHann();
    const float over = 1.0f + 2.5f * amount;                // how much more than the average noise is taken out
    const float floorGain = dbToGain(-36.0f * amount);      // what is left of a bin that is all noise
    std::vector<float> re((size_t)kN), im((size_t)kN), g((size_t)kBins), gPrev((size_t)kBins), acc;
    for (int c = 0; c < s.channels; ++c) {
        const int64 len = b - a;
        acc.assign((size_t)len, 0.0f);
        std::fill(gPrev.begin(), gPrev.end(), 1.0f);
        for (int64 at = -kN + kHop; at < len; at += kHop) {
            for (int i = 0; i < kN; ++i) {
                const int64 src = a + at + i;
                re[(size_t)i] = src >= 0 && src < s.frames() ? s.ch[c][(size_t)src] * w[(size_t)i] : 0.0f;
                im[(size_t)i] = 0.0f;
            }
            dsp::fft(re.data(), im.data(), kN, false);
            for (int k = 0; k < kBins; ++k) {
                const float m = std::sqrt(re[(size_t)k] * re[(size_t)k] + im[(size_t)k] * im[(size_t)k]);
                float gk = m > 1.0e-12f ? std::max(floorGain, 1.0f - over * profile.mag[(size_t)k] / m) : floorGain;
                gk = std::max(gk, 0.7f * gPrev[(size_t)k]);        // gains close slowly, which keeps the result from chirping
                gPrev[(size_t)k] = gk;
                g[(size_t)k] = gk;
            }
            // smooth across neighbouring bins too
            for (int k = 0; k < kBins; ++k) {
                const float gk = (g[(size_t)std::max(0, k - 1)] + 2.0f * g[(size_t)k] + g[(size_t)std::min(kBins - 1, k + 1)]) * 0.25f;
                re[(size_t)k] *= gk;
                im[(size_t)k] *= gk;
                if (k > 0 && k < kN / 2) { re[(size_t)(kN - k)] = re[(size_t)k]; im[(size_t)(kN - k)] = -im[(size_t)k]; }
            }
            dsp::fft(re.data(), im.data(), kN, true);
            for (int i = 0; i < kN; ++i) {
                const int64 dst = at + i;
                if (dst >= 0 && dst < len) acc[(size_t)dst] += re[(size_t)i] * w[(size_t)i] * 0.5f;     // four Hann windows overlap: they add up to two
            }
        }
        // a short crossfade at each end of the range, so a partial selection has no step
        const int64 xf = std::min<int64>(256, len / 2);
        for (int64 i = 0; i < len; ++i) {
            float mix = 1.0f;
            if (a > 0 && i < xf) mix = (float)i / (float)xf;
            if (b < s.frames() && len - 1 - i < xf) mix = std::min(mix, (float)(len - 1 - i) / (float)xf);
            o->ch[c][(size_t)(a + i)] = s.ch[c][(size_t)(a + i)] * (1.0f - mix) + acc[(size_t)i] * mix;
        }
    }
    return o;
}

} // namespace pulp::edit

namespace pulp {

Splice::Splice() {
    pRec = addBool("rec", "Record", false);
    pMode = addChoice("mode", "Record when", {"Now", "On play", "On input"}, 0);
}

std::vector<Preset> Splice::factoryPresets() const {
    return buildPresets(*this, {{"Record now", {{"mode", 0}}}, {"Record while playing", {{"mode", 1}}}, {"Record when sound arrives", {{"mode", 2}}}});
}

void Splice::prepare(double sr) {
    sr_ = sr;
    if (!ring_) ring_ = std::make_unique<float[]>((size_t)kRing * 2);
    w_.store(0);
    r_.store(0);
    latched_ = false;
}

void Splice::process(ProcessContext& c) {
    bool rec = pRec->getBool();
    if (!rec) latched_ = false;
    else {
        const int mode = pMode->getInt();
        if (mode == OnPlay) rec = c.playing;
        else if (mode == OnInput && !latched_) {
            float pk = 0.0f;
            for (int i = 0; i < c.numSamples; ++i) pk = std::max(pk, std::max(std::fabs(c.io[0][i]), std::fabs(c.io[1][i])));
            latched_ = pk >= 0.01f;         // -40 dB
            rec = latched_;
        }
    }
    capturing_.store(rec, std::memory_order_relaxed);
    if (!rec || !ring_) return;
    const int64 w = w_.load(std::memory_order_relaxed);
    if (w - r_.load(std::memory_order_acquire) + c.numSamples > kRing) { dropped_.fetch_add(c.numSamples, std::memory_order_relaxed); return; }
    for (int i = 0; i < c.numSamples; ++i) {
        const size_t at = (size_t)((w + i) & (kRing - 1)) * 2;
        ring_[at] = c.io[0][i];
        ring_[at + 1] = c.io[1][i];
    }
    w_.store(w + c.numSamples, std::memory_order_release);
}

void Splice::pump() {
    if (!ring_) return;
    const int64 r = r_.load(std::memory_order_relaxed), w = w_.load(std::memory_order_acquire);
    if (w == r) {
        if (live_ && !pRec->getBool()) live_ = nullptr;        // recording is over: the next one starts a new undo step
        return;
    }
    if (!live_) {
        // the first captured block of a recording: the audio so far goes on the undo stack and a copy is extended
        live_ = std::make_shared<SampleData>();
        live_->sampleRate = sr_;
        live_->channels = 2;
        if (audio_) {
            if (audio_->sampleRate == sr_) { live_->ch[0] = audio_->ch[0]; live_->ch[1] = audio_->channels > 1 ? audio_->ch[1] : audio_->ch[0]; }
            undo_.push_back(audio_);
            if (undo_.size() > 24) undo_.erase(undo_.begin());
        }
        audio_ = live_;
    }
    const int64 maxFrames = (int64)(sr_ * 60.0 * 30.0);        // half an hour is the most one recording holds
    for (int64 i = r; i < w; ++i) {
        if (live_->frames() >= maxFrames) break;
        const size_t at = (size_t)(i & (kRing - 1)) * 2;
        live_->ch[0].push_back(ring_[at]);
        live_->ch[1].push_back(ring_[at + 1]);
    }
    r_.store(w, std::memory_order_release);
    unsaved_ = true;
}

void Splice::setAudio(SamplePtr s, bool isEdit) {
    if (isEdit && audio_) {
        undo_.push_back(audio_);
        if (undo_.size() > 24) undo_.erase(undo_.begin());
    }
    audio_ = std::move(s);
    live_ = nullptr;
    if (isEdit) unsaved_ = true;
    const int64 n = audio_ ? audio_->frames() : 0;
    selA = clampT<int64>(selA, 0, n);
    selB = clampT<int64>(selB, 0, n);
}

bool Splice::undo() {
    if (undo_.empty()) return false;
    audio_ = undo_.back();
    undo_.pop_back();
    live_ = nullptr;
    unsaved_ = true;
    const int64 n = audio_ ? audio_->frames() : 0;
    selA = clampT<int64>(selA, 0, n);
    selB = clampT<int64>(selB, 0, n);
    return true;
}

void Splice::clear() {
    setAudio(nullptr, true);
    selA = selB = 0;
    file.clear();
    unsaved_ = false;
}

Json Splice::saveExtra() const {
    Json j = Json::object();
    if (audio_ && unsaved_) {
        // Not in a file yet: the state points at the audio in memory, so undo and redo can bring it back.
        // Project::save writes it to a file first.
        const std::string key = "mem:splice-" + std::to_string(audio_->uid.v);
        SampleCache::global().put(key, audio_);
        j.set("file", key);
    } else if (!file.empty()) j.set("file", file);
    return j;
}

void Splice::loadExtra(const Json& j, const std::string& projectDir) {
    file = j["file"].str("");
    audio_ = file.empty() ? nullptr : SampleCache::global().load(file, projectDir);
    unsaved_ = audio_ != nullptr && file.rfind("mem:", 0) == 0;
    if (unsaved_ || !audio_) file.clear();
    live_ = nullptr;
    undo_.clear();
    selA = selB = 0;
    pRec->set(0.0f);            // a project never opens (or comes back from undo) already recording
}

} // namespace pulp
