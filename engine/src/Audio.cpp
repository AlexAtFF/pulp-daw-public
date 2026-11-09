#include "pulp/Audio.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

namespace pulp {

namespace fs = std::filesystem;
static fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

float SampleData::peak() const {
    float p = 0.0f;
    for (int c = 0; c < channels; ++c)
        for (float v : ch[c]) p = std::max(p, std::fabs(v));
    return p;
}

bool readFileBytes(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream f(u8path(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const auto n = (size_t)f.tellg();
    f.seekg(0);
    out.resize(n);
    if (n) f.read((char*)out.data(), (std::streamsize)n);
    return (bool)f || f.eof();
}
bool writeFileBytes(const std::string& path, const void* data, size_t size) {
    std::error_code ec;
    auto p = u8path(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write((const char*)data, (std::streamsize)size);
    return (bool)f;
}
bool readTextFile(const std::string& path, std::string& out) {
    std::vector<unsigned char> b;
    if (!readFileBytes(path, b)) return false;
    out.assign(b.begin(), b.end());
    return true;
}
bool writeTextFile(const std::string& path, const std::string& text) {
    return writeFileBytes(path, text.data(), text.size());
}

namespace {
inline unsigned rd16(const unsigned char* p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
inline unsigned rd32(const unsigned char* p) { return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24); }
inline void wr16(std::vector<unsigned char>& o, unsigned v) { o.push_back((unsigned char)(v & 255)); o.push_back((unsigned char)((v >> 8) & 255)); }
inline void wr32(std::vector<unsigned char>& o, unsigned v) { wr16(o, v & 0xFFFF); wr16(o, (v >> 16) & 0xFFFF); }
}

uint64 nextAudioUid() { static std::atomic<uint64> n{1}; return n.fetch_add(1); }

void* openFileForRead(const std::string& utf8Path) { return _wfopen(u8path(utf8Path).wstring().c_str(), L"rb"); }

// Walks the chunks of a WAV file. read(offset, dst, n) returns how many bytes it could give.
template <class Read>
static bool parseWav(Read&& read, int64 size, WavInfo& info, std::string* error) {
    auto fail = [&](const char* m) { if (error) *error = m; return false; };
    unsigned char h[12];
    if (size < 12 || read(0, h, 12) != 12 || std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0) return fail("not a WAV file");
    info = WavInfo();
    bool haveData = false;
    int64 pos = 12;
    while (pos + 8 <= size) {
        unsigned char c[8];
        if (read(pos, c, 8) != 8) break;
        int64 len = rd32(c + 4);
        const int64 body = pos + 8;
        if (body + len > size) len = size - body;       // tolerate truncated files and takes still being written
        if (std::memcmp(c, "fmt ", 4) == 0 && len >= 16) {
            unsigned char f[40] = {};
            const int got = (int)read(body, f, (int)std::min<int64>(len, 40));
            if (got < 16) return fail("bad WAV format");
            info.format = rd16(f);
            info.channels = rd16(f + 2);
            info.rate = rd32(f + 4);
            info.blockAlign = rd16(f + 12);
            info.bits = rd16(f + 14);
            if (info.format == 0xFFFE && got >= 26) info.format = rd16(f + 24);   // WAVE_FORMAT_EXTENSIBLE
        } else if (std::memcmp(c, "data", 4) == 0) {
            info.dataOffset = body;
            info.dataBytes = len;
            haveData = true;
        }
        pos = body + len + (len & 1);
    }
    if (!haveData || info.channels == 0 || info.rate == 0) return fail("WAV file has no audio data");
    if (!(info.format == 1 || info.format == 3)) return fail("unsupported WAV encoding");
    const unsigned bytes = info.bits / 8;
    if (bytes == 0 || info.blockAlign < bytes * info.channels) return fail("bad WAV format");
    if (info.format == 3 && !(info.bits == 32 || info.bits == 64)) return fail("unsupported float WAV");
    if (info.format == 1 && !(info.bits == 8 || info.bits == 16 || info.bits == 24 || info.bits == 32)) return fail("unsupported PCM WAV");
    return true;
}

bool readWavInfo(const std::string& path, WavInfo& info, std::string* error) {
    FILE* f = (FILE*)openFileForRead(path);
    if (!f) { if (error) *error = "cannot open " + path; return false; }
    _fseeki64(f, 0, SEEK_END);
    const int64 size = _ftelli64(f);
    const bool ok = parseWav([&](int64 off, unsigned char* dst, int n) { _fseeki64(f, off, SEEK_SET); return (int)fread(dst, 1, (size_t)n, f); }, size, info, error);
    fclose(f);
    return ok;
}

void decodeWavFrames(const unsigned char* src, const WavInfo& info, size_t frames, float* l, float* r) {
    const unsigned bytes = info.bits / 8, bits = info.bits;
    const int outCh = info.channels > 1 ? 2 : 1;
    for (size_t i = 0; i < frames; ++i) {
        const unsigned char* fr = src + i * info.blockAlign;
        for (int c = 0; c < outCh; ++c) {
            const unsigned char* p = fr + (size_t)c * bytes;
            float v = 0.0f;
            if (info.format == 3) {
                if (bits == 32) { float f; std::memcpy(&f, p, 4); v = f; }
                else { double f; std::memcpy(&f, p, 8); v = (float)f; }
            } else if (bits == 8) v = ((int)p[0] - 128) / 128.0f;
            else if (bits == 16) v = (float)(short)rd16(p) / 32768.0f;
            else if (bits == 24) { int x = (int)(p[0] | (p[1] << 8) | (p[2] << 16)); if (x & 0x800000) x |= ~0xFFFFFF; v = (float)x / 8388608.0f; }
            else v = (float)((double)(int)rd32(p) / 2147483648.0);
            if (c == 0) l[i] = v;
            else if (r) r[i] = v;
        }
    }
}

std::shared_ptr<SampleData> readWavMemory(const void* data, size_t size, std::string* error) {
    const unsigned char* d = (const unsigned char*)data;
    WavInfo info;
    if (!parseWav([&](int64 off, unsigned char* dst, int n) {
            const int64 k = std::max<int64>(0, std::min<int64>(n, (int64)size - off));
            if (k > 0) std::memcpy(dst, d + off, (size_t)k);
            return (int)k;
        }, (int64)size, info, error)) return nullptr;
    auto s = std::make_shared<SampleData>();
    s->sampleRate = info.rate;
    s->channels = info.channels > 1 ? 2 : 1;
    const size_t frames = (size_t)info.frames();
    for (int c = 0; c < s->channels; ++c) s->ch[c].resize(frames);
    decodeWavFrames(d + info.dataOffset, info, frames, s->ch[0].data(), s->channels > 1 ? s->ch[1].data() : nullptr);
    return s;
}

std::shared_ptr<SampleData> readWavFile(const std::string& path, std::string* error) {
    std::vector<unsigned char> b;
    if (!readFileBytes(path, b)) { if (error) *error = "cannot open " + path; return nullptr; }
    auto s = readWavMemory(b.data(), b.size(), error);
    if (s) s->path = path;
    return s;
}

std::vector<unsigned char> encodeWav(const SampleData& s, WavFormat fmt, bool dither) {
    const unsigned channels = (unsigned)s.channels;
    const unsigned bytes = fmt == WavFormat::Pcm16 ? 2u : (fmt == WavFormat::Pcm24 ? 3u : 4u);
    const size_t frames = (size_t)s.frames();
    const unsigned dataBytes = (unsigned)(frames * channels * bytes);
    std::vector<unsigned char> o;
    o.reserve(44 + dataBytes);
    o.insert(o.end(), {'R', 'I', 'F', 'F'});
    wr32(o, 36 + dataBytes);
    o.insert(o.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    wr32(o, 16);
    wr16(o, fmt == WavFormat::Float32 ? 3u : 1u);
    wr16(o, channels);
    wr32(o, (unsigned)std::lround(s.sampleRate));
    wr32(o, (unsigned)std::lround(s.sampleRate) * channels * bytes);
    wr16(o, channels * bytes);
    wr16(o, bytes * 8);
    o.insert(o.end(), {'d', 'a', 't', 'a'});
    wr32(o, dataBytes);
    // TPDF dither with a fixed seed so exports are reproducible.
    std::minstd_rand rng(0x50554C50u);
    auto tpdf = [&]() { return ((double)rng() + (double)rng()) / (double)rng.max() - 1.0; };
    for (size_t i = 0; i < frames; ++i) {
        for (unsigned c = 0; c < channels; ++c) {
            const float v = s.ch[c][i];
            if (fmt == WavFormat::Float32) {
                unsigned char b[4]; std::memcpy(b, &v, 4); o.insert(o.end(), b, b + 4);
            } else {
                const double scale = fmt == WavFormat::Pcm16 ? 32768.0 : 8388608.0;
                double x = (double)v * scale + (dither ? tpdf() : 0.0);
                long long q = std::llround(x);
                const long long hi = (long long)scale - 1, lo = -(long long)scale;
                q = q > hi ? hi : (q < lo ? lo : q);
                o.push_back((unsigned char)(q & 255));
                o.push_back((unsigned char)((q >> 8) & 255));
                if (bytes == 3) o.push_back((unsigned char)((q >> 16) & 255));
            }
        }
    }
    return o;
}

bool writeWavFile(const std::string& path, const SampleData& s, WavFormat fmt, bool dither, std::string* error) {
    auto bytes = encodeWav(s, fmt, dither);
    if (!writeFileBytes(path, bytes.data(), bytes.size())) { if (error) *error = "cannot write " + path; return false; }
    return true;
}

bool WavStreamWriter::open(const std::string& path, double sampleRate, int channels, WavFormat fmt) {
    close();
    std::error_code ec;
    auto p = u8path(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    FILE* f = _wfopen(p.wstring().c_str(), L"wb");
    if (!f) return false;
    file_ = f;
    path_ = path;
    channels_ = channels;
    fmt_ = fmt;
    frames_ = 0;
    SampleData empty;
    empty.sampleRate = sampleRate;
    empty.channels = channels;
    auto head = encodeWav(empty, fmt);          // a 44-byte header for zero frames
    // until close() the sizes say "as much as there is": a reader takes whatever the file holds
    const unsigned char open[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    std::memcpy(head.data() + 4, open, 4);
    std::memcpy(head.data() + 40, open, 4);
    fwrite(head.data(), 1, head.size(), f);
    fflush(f);
    return true;
}

bool WavStreamWriter::write(const float* interleaved, int frames) {
    if (!file_ || frames <= 0) return false;
    FILE* f = (FILE*)file_;
    const size_t n = (size_t)frames * (size_t)channels_;
    if (fmt_ == WavFormat::Float32) fwrite(interleaved, sizeof(float), n, f);
    else {
        std::vector<unsigned char> b;
        const bool s16 = fmt_ == WavFormat::Pcm16;
        b.reserve(n * (s16 ? 2 : 3));
        for (size_t i = 0; i < n; ++i) {
            const double scale = s16 ? 32768.0 : 8388608.0;
            long long q = std::llround((double)interleaved[i] * scale);
            q = clampT<long long>(q, -(long long)scale, (long long)scale - 1);
            b.push_back((unsigned char)(q & 255));
            b.push_back((unsigned char)((q >> 8) & 255));
            if (!s16) b.push_back((unsigned char)((q >> 16) & 255));
        }
        fwrite(b.data(), 1, b.size(), f);
    }
    frames_ += frames;
    return true;
}

void WavStreamWriter::flush() { if (file_) fflush((FILE*)file_); }

void WavStreamWriter::close() {
    if (!file_) return;
    FILE* f = (FILE*)file_;
    const unsigned bytes = fmt_ == WavFormat::Pcm16 ? 2u : (fmt_ == WavFormat::Pcm24 ? 3u : 4u);
    const unsigned dataBytes = (unsigned)(frames_ * channels_ * bytes);
    const unsigned riff = 36 + dataBytes;
    fseek(f, 4, SEEK_SET);
    fwrite(&riff, 4, 1, f);
    fseek(f, 40, SEEK_SET);
    fwrite(&dataBytes, 4, 1, f);
    fclose(f);
    file_ = nullptr;
}

SampleCache& SampleCache::global() { static SampleCache c; return c; }

void SampleCache::setFactoryRoot(const std::string& dir) { std::lock_guard<std::mutex> g(m_); factoryRoot_ = dir; }

std::string SampleCache::resolve(const std::string& path, const std::string& projectDir) const {
    if (path.rfind("factory:", 0) == 0) return factoryRoot_ + "/" + path.substr(8);
    if (path.rfind("mem:", 0) == 0) return path;       // audio that only exists in memory so far (an unsaved recording in an editor)
    auto p = u8path(path);
    if (p.is_relative() && !projectDir.empty()) {
        auto full = (u8path(projectDir) / p).u8string();
        return std::string(full.begin(), full.end());
    }
    return path;
}

SamplePtr SampleCache::load(const std::string& path, const std::string& projectDir, std::string* error) {
    if (path.empty()) return nullptr;
    const std::string full = resolve(path, projectDir);
    {
        std::lock_guard<std::mutex> g(m_);
        auto it = map_.find(full);
        if (it != map_.end()) return it->second;
    }
    auto s = readAudioFile(full, error);
    if (!s) return nullptr;
    s->path = path;
    std::lock_guard<std::mutex> g(m_);
    map_[full] = s;
    return s;
}

void SampleCache::put(const std::string& key, SamplePtr s) { std::lock_guard<std::mutex> g(m_); map_[resolve(key)] = std::move(s); }
void SampleCache::clear() { std::lock_guard<std::mutex> g(m_); map_.clear(); }

} // namespace pulp

namespace pulp {
std::string sha256Hex(const void* data, size_t size) {
    static const uint32 k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<unsigned char> m((const unsigned char*)data, (const unsigned char*)data + size);
    const unsigned long long bits = (unsigned long long)size * 8ull;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back((unsigned char)(bits >> (i * 8)));
    auto rotr = [](uint32 x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32 w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32)m[off + i * 4] << 24) | ((uint32)m[off + i * 4 + 1] << 16) | ((uint32)m[off + i * 4 + 2] << 8) | (uint32)m[off + i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32 S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32 ch = (e & f) ^ (~e & g);
            const uint32 t1 = hh + S1 + ch + k[i] + w[i];
            const uint32 S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32 mj = (a & b) ^ (a & c) ^ (b & c);
            const uint32 t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char out[65];
    for (int i = 0; i < 8; ++i) snprintf(out + i * 8, 9, "%08x", h[i]);
    return std::string(out, 64);
}
}
