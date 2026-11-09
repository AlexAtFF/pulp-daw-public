#include "pulp/Ogg.h"
#include <bit>

namespace pulp {

#ifdef PULP_OGG_TRAIN
// Development only (dev/oggtool.cpp): how often the encoder wrote each entry of each of its books,
// and book lengths to use in place of the built-in ones.
std::vector<std::vector<uint64>> oggTrainCounts;
const char* const* oggTrainLengths = nullptr;
#endif

namespace {

// ------------------------------------------------------------------ Ogg pages

// A page's checksum: CRC-32 with polynomial 0x04c11db7, no bit reflection, starting at zero, taken
// over the whole page with the checksum field itself as zeros.
struct OggCrcTable {
    uint32 t[256] = {};
    constexpr OggCrcTable() {
        for (uint32 i = 0; i < 256; ++i) {
            uint32 r = i << 24;
            for (int k = 0; k < 8; ++k) r = (r & 0x80000000u) ? ((r << 1) ^ 0x04c11db7u) : (r << 1);
            t[i] = r;
        }
    }
};

constexpr OggCrcTable kOggCrc;
uint32 oggCrc(uint32 c, const unsigned char* p, size_t n) {
    while (n--) c = (c << 8) ^ kOggCrc.t[(c >> 24) ^ *p++];
    return c;
}

inline uint32 le32(const unsigned char* p) { return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24); }

struct Page {
    unsigned flags = 0;             // 1 = first packet continues one from the page before, 2 = first page of a stream, 4 = last
    int64 granule = -1;
    uint32 serial = 0, seq = 0;
    int segments = 0;
    const unsigned char* lacing = nullptr;
    const unsigned char* body = nullptr;
    size_t bytes = 0;               // the whole page
};

enum class PageRead { Ok, NotAPage, Short, Crc };
PageRead readPage(const unsigned char* p, size_t avail, Page& pg) {
    if (avail < 4) return PageRead::Short;
    if (std::memcmp(p, "OggS", 4) != 0) return PageRead::NotAPage;
    if (avail < 27) return PageRead::Short;
    if (p[4] != 0) return PageRead::NotAPage;       // the only version there is
    pg.flags = p[5];
    pg.granule = (int64)((uint64)le32(p + 6) | ((uint64)le32(p + 10) << 32));
    pg.serial = le32(p + 14);
    pg.seq = le32(p + 18);
    pg.segments = p[26];
    const size_t head = 27u + (size_t)pg.segments;
    if (avail < head) return PageRead::Short;
    pg.lacing = p + 27;
    size_t body = 0;
    for (int i = 0; i < pg.segments; ++i) body += pg.lacing[i];
    if (avail - head < body) return PageRead::Short;
    pg.body = p + head;
    pg.bytes = head + body;
    static const unsigned char zeros[4] = {0, 0, 0, 0};
    uint32 c = oggCrc(0, p, 22);
    c = oggCrc(c, zeros, 4);
    c = oggCrc(c, p + 26, pg.bytes - 26);
    return c == le32(p + 22) ? PageRead::Ok : PageRead::Crc;
}

// ------------------------------------------------------------------ bits

// Vorbis packs numbers lowest bit first. Past the end of the packet every read gives zero and eop is
// set; nothing here can read outside the packet.
struct BitReader {
    const unsigned char* p;
    const unsigned char* end;
    uint64 acc = 0;         // the next bits, lowest first; only the low n count (above them may sit bits of the byte at p)
    int n = 0;
    bool eop = false;
    BitReader(const unsigned char* from, size_t bytes) : p(from), end(from + bytes) {}
    void refill() {
        if (end - p >= 8) {         // eight bytes at once; p moves only past the whole bytes that fitted
            const uint64 v = (uint64)le32(p) | ((uint64)le32(p + 4) << 32);
            acc |= v << n;
            p += (63 - n) >> 3;
            n |= 56;
            return;
        }
        while (n <= 56 && p < end) { acc |= (uint64)*p++ << n; n += 8; }
    }
    void fail() { eop = true; acc = 0; n = 0; p = end; }
    uint32 bits(int k) {            // k is 0 to 32
        if (n < k) {
            refill();
            if (n < k) { fail(); return 0; }
        }
        if (k == 0) return 0;
        const uint32 v = (uint32)(acc & (~0ull >> (64 - k)));
        acc >>= k;
        n -= k;
        return v;
    }
};

inline int ilog(uint32 v) { return (int)std::bit_width(v); }       // how many bits v needs; 0 for 0
inline uint32 reverse32(uint32 v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
}

// ------------------------------------------------------------------ codebooks

constexpr int kFastBits = 10;

// A Huffman code over `entries` symbols, and optionally a vector of `dim` numbers for each symbol.
struct Codebook {
    int dim = 0, entries = 0, used = 0;
    int single = -1;                        // the entry of a book with only one, which is read as one bit
    bool hasVectors = false;
    std::vector<unsigned char> lens;        // by entry; 0 = not used
    std::vector<uint32> codes;              // by entry: the codeword, left-aligned (its first bit is bit 31)
    std::vector<int32_t> fast;              // by the next kFastBits bits: entry * 64 + length, or -1
    std::vector<uint32> longCode;           // the codewords longer than kFastBits, sorted, with their entries and lengths
    std::vector<int32_t> longEntry;
    std::vector<unsigned char> longLen;
    std::vector<float> vectors;             // entries * dim
    // Gives each used entry the lowest codeword still free at its length, as the specification lays out.
    // False if the lengths ask for more codewords than there are, or leave some over: either makes the
    // stream undecodable. (A book with a single entry is the exception; it is read as one bit.)
    bool assignCodes() {
        codes.assign((size_t)entries, 0);
        uint32 spare[33] = {};              // the free node at each depth, if there is one
        bool have[33] = {};
        used = 0;
        single = -1;
        int first = -1;
        for (int i = 0; i < entries; ++i)
            if (lens[(size_t)i]) { if (first < 0) first = i; ++used; }
        if (used == 0) return true;
        if (used == 1) { single = first; return true; }
        for (int l = 1; l <= lens[(size_t)first]; ++l) { spare[l] = 1u << (32 - l); have[l] = true; }
        for (int i = first + 1; i < entries; ++i) {
            const int z = lens[(size_t)i];
            if (!z) continue;
            int l = z;
            while (l > 0 && !have[l]) --l;
            if (l == 0) return false;
            const uint32 c = spare[l];
            have[l] = false;
            codes[(size_t)i] = c;
            for (int y = l + 1; y <= z; ++y) { spare[y] = c + (1u << (32 - y)); have[y] = true; }
        }
        for (int l = 1; l <= 32; ++l) if (have[l]) return false;
        return true;
    }
    void buildDecoder() {
        fast.assign((size_t)1 << kFastBits, -1);
        longCode.clear(); longEntry.clear(); longLen.clear();
        if (used == 0) return;
        if (single >= 0) { std::fill(fast.begin(), fast.end(), single * 64 + 1); return; }
        std::vector<int32_t> order;
        for (int i = 0; i < entries; ++i) {
            const int len = lens[(size_t)i];
            if (!len) continue;
            if (len <= kFastBits) {
                const uint32 rev = reverse32(codes[(size_t)i]);     // the codeword's first bit is now bit 0
                for (uint32 j = 0; j < (1u << (kFastBits - len)); ++j) fast[rev | (j << len)] = i * 64 + len;
            } else order.push_back(i);
        }
        std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) { return codes[(size_t)a] < codes[(size_t)b]; });
        for (int32_t i : order) {
            longCode.push_back(codes[(size_t)i]);
            longEntry.push_back(i);
            longLen.push_back(lens[(size_t)i]);
        }
    }

    // The next entry, or -1 at the end of the packet (or on a codeword the book does not have).
    int decode(BitReader& b) const {
        if (b.n < 32) b.refill();
        const int32_t f = fast[(size_t)((uint32)b.acc & ((1u << kFastBits) - 1u))];
        if (f >= 0) {
            const int len = f & 63;
            if (len > b.n) { b.fail(); return -1; }
            b.acc >>= len;
            b.n -= len;
            return f >> 6;
        }
        return decodeLong(b);
    }
    int decodeLong(BitReader& b) const {
        if (longCode.empty()) { b.fail(); return -1; }
        const uint32 x = reverse32((uint32)b.acc);
        const size_t at = (size_t)(std::upper_bound(longCode.begin(), longCode.end(), x) - longCode.begin());
        if (at == 0) { b.fail(); return -1; }
        const int len = longLen[at - 1];
        if (((x ^ longCode[at - 1]) >> (32 - len)) != 0 || len > b.n) { b.fail(); return -1; }
        b.acc >>= len;
        b.n -= len;
        return longEntry[at - 1];
    }
};

// The largest r with r^dim <= entries: how many values per dimension a lattice book has.
int lookup1Values(int entries, int dim) {
    if (dim <= 0) return 0;
    int r = (int)std::floor(std::pow((double)entries, 1.0 / (double)dim));
    auto fits = [&](int v) {        // v^dim <= entries
        uint64 acc = 1;
        for (int i = 0; i < dim; ++i) {
            acc *= (uint64)v;
            if (acc > (uint64)entries) return false;
        }
        return true;
    };
    while (r > 0 && !fits(r)) --r;
    while (fits(r + 1)) ++r;
    return r;
}

inline float unpackFloat(uint32 x) {
    const double mantissa = (double)(x & 0x1fffffu);
    const int exponent = (int)((x & 0x7fe00000u) >> 21);
    return (float)std::ldexp((x & 0x80000000u) ? -mantissa : mantissa, exponent - 788);
}

// Limits on what a setup header may ask for. Real streams stay far below them; they are here so that a
// few crafted bytes cannot make the decoder allocate gigabytes.
constexpr int64 kMaxBookEntries = 1 << 21;      // all the codebooks of a stream together
constexpr int64 kMaxBookValues = 1 << 24;       // all their vectors together
bool readCodebook(BitReader& b, Codebook& cb, int64& entriesLeft, int64& valuesLeft) {
    if (b.bits(24) != 0x564342u) return false;
    cb.dim = (int)b.bits(16);
    cb.entries = (int)b.bits(24);
    if (b.eop || cb.entries > entriesLeft) return false;
    entriesLeft -= cb.entries;
    cb.lens.assign((size_t)cb.entries, 0);
    if (!b.bits(1)) {
        const bool sparse = b.bits(1) != 0;
        for (int i = 0; i < cb.entries && !b.eop; ++i)
            if (!sparse || b.bits(1)) cb.lens[(size_t)i] = (unsigned char)(b.bits(5) + 1);
    } else {            // lengths in rising order: how many entries have each
        int len = (int)b.bits(5) + 1;
        for (int i = 0; i < cb.entries;) {
            const int count = (int)b.bits(ilog((uint32)(cb.entries - i)));
            if (b.eop || count > cb.entries - i || len > 32) return false;
            std::fill(cb.lens.begin() + i, cb.lens.begin() + i + count, (unsigned char)len);
            i += count;
            ++len;
        }
    }
    if (b.eop || !cb.assignCodes()) return false;
    const unsigned lookup = b.bits(4);
    if (lookup > 2) return false;
    if (lookup) {
        const float minimum = unpackFloat(b.bits(32)), delta = unpackFloat(b.bits(32));
        const int valueBits = (int)b.bits(4) + 1;
        const bool sequence = b.bits(1) != 0;
        const int64 count = lookup == 1 ? (int64)lookup1Values(cb.entries, cb.dim) : (int64)cb.entries * cb.dim;
        const int64 total = (int64)cb.entries * cb.dim;
        if (b.eop || count > valuesLeft || total > valuesLeft) return false;
        valuesLeft -= total;
        std::vector<float> mult((size_t)count);
        for (auto& m : mult) m = (float)b.bits(valueBits);
        if (b.eop) return false;
        cb.vectors.assign((size_t)total, 0.0f);
        for (int e = 0; e < cb.entries; ++e) {
            if (!cb.lens[(size_t)e]) continue;
            float* v = cb.vectors.data() + (size_t)e * (size_t)cb.dim;
            float last = 0.0f;
            if (lookup == 1) {
                int64 div = 1;
                for (int i = 0; i < cb.dim; ++i) {
                    v[i] = mult[(size_t)((e / div) % count)] * delta + minimum + last;
                    if (sequence) last = v[i];
                    div = div <= (int64)cb.entries ? div * count : div;     // past entries the quotient is zero anyway
                }
            } else {
                const float* m = mult.data() + (size_t)e * (size_t)cb.dim;
                for (int i = 0; i < cb.dim; ++i) {
                    v[i] = m[i] * delta + minimum + last;
                    if (sequence) last = v[i];
                }
            }
        }
        cb.hasVectors = true;
    }
    cb.buildDecoder();
    return !b.eop;
}

// ------------------------------------------------------------------ transform

// The MDCT of Vorbis blocks, through a DCT-IV made from a complex FFT of a quarter of the block size.
struct Mdct {
    int n = 0;                          // block size; a block has n/2 coefficients
    std::vector<float> pre, post, tw;   // cos, sin pairs
    std::vector<int> rev;
    std::vector<float> z, u;
    void init(int blockSize) {
        n = blockSize;
        const int m = n / 2, q = n / 4;
        pre.resize((size_t)q * 2); post.resize((size_t)q * 2); tw.resize((size_t)q); rev.resize((size_t)q);
        z.resize((size_t)q * 2); u.resize((size_t)m);
        for (int j = 0; j < q; ++j) {
            const double a = -kPi * j / m, c = -kPi * (4 * j + 1) / (4.0 * m);
            pre[(size_t)j * 2] = (float)std::cos(a); pre[(size_t)j * 2 + 1] = (float)std::sin(a);
            post[(size_t)j * 2] = (float)std::cos(c); post[(size_t)j * 2 + 1] = (float)std::sin(c);
        }
        for (int j = 0; j < q / 2; ++j) {
            const double a = -kTwoPi * j / q;
            tw[(size_t)j * 2] = (float)std::cos(a); tw[(size_t)j * 2 + 1] = (float)std::sin(a);
        }
        const int bitsQ = std::countr_zero((unsigned)q);
        for (int j = 0; j < q; ++j) rev[(size_t)j] = (int)(reverse32((uint32)j) >> (32 - bitsQ));
    }

    // out[k] = sum over j of in[j] * cos(pi / m * (j + 1/2) * (k + 1/2)), m = n/2 numbers each way. in and out must differ.
    void dct4(const float* in, float* out) {
        const int m = n / 2, q = n / 4;
        float* f = z.data();
        for (int j = 0; j < q; ++j) {
            const float a = in[2 * j], b = in[m - 1 - 2 * j], c = pre[(size_t)j * 2], s = pre[(size_t)j * 2 + 1];
            float* d = f + (size_t)rev[(size_t)j] * 2;
            d[0] = a * c - b * s;
            d[1] = a * s + b * c;
        }
        for (int j = 0; j < q; j += 2) {            // the first pass has no rotation
            float* d = f + (size_t)j * 2;
            const float ar = d[0], ai = d[1], br = d[2], bi = d[3];
            d[0] = ar + br; d[1] = ai + bi; d[2] = ar - br; d[3] = ai - bi;
        }
        for (int half = 2; half < q; half *= 2) {
            const int step = q / (half * 2);
            for (int start = 0; start < q; start += half * 2) {
                float* a = f + (size_t)start * 2;
                float* b = a + (size_t)half * 2;
                const float* w = tw.data();
                for (int k = 0; k < half; ++k, a += 2, b += 2, w += (size_t)step * 2) {
                    const float tr = b[0] * w[0] - b[1] * w[1], ti = b[0] * w[1] + b[1] * w[0];
                    b[0] = a[0] - tr; b[1] = a[1] - ti;
                    a[0] += tr; a[1] += ti;
                }
            }
        }
        for (int k = 0; k < q; ++k) {
            const float re = f[(size_t)k * 2], im = f[(size_t)k * 2 + 1], c = post[(size_t)k * 2], s = post[(size_t)k * 2 + 1];
            out[2 * k] = re * c - im * s;
            out[m - 1 - 2 * k] = -(re * s + im * c);
        }
    }

    // n/2 coefficients to n samples (not yet windowed).
    void inverse(const float* coef, float* y) {
        const int m = n / 2, h = n / 4;
        float* t = u.data();
        dct4(coef, t);
        for (int i = 0; i < h; ++i) y[i] = t[i + h];
        for (int i = h; i < m + h; ++i) y[i] = -t[m + h - 1 - i];
        for (int i = m + h; i < n; ++i) y[i] = -t[i - m - h];
    }

    // n windowed samples to n/2 coefficients, scaled so that inverse() and the overlap give the samples back.
    void forward(const float* x, float* coef) {
        const int m = n / 2, h = n / 4;
        float* t = u.data();
        for (int i = 0; i < h; ++i) t[i] = -x[m + h - 1 - i] - x[m + h + i];
        for (int i = h; i < m; ++i) t[i] = x[i - h] - x[m + h - 1 - i];
        dct4(t, coef);
        const float scale = 2.0f / (float)m;
        for (int i = 0; i < m; ++i) coef[i] *= scale;
    }
};

// The rising half of the Vorbis window, `half` samples long.
void makeSlope(std::vector<float>& w, int half) {
    w.resize((size_t)half);
    for (int i = 0; i < half; ++i) {
        const double s = std::sin((i + 0.5) / half * (kPi * 0.5));
        w[(size_t)i] = (float)std::sin(kPi * 0.5 * s * s);
    }
}

// Windows a block of n samples in place. A long block next to a short one takes the short block's slope
// on that side, in the middle of that half.
void applyWindow(float* x, int n, const float* slopeL, int halfL, const float* slopeR, int halfR) {
    const int l0 = n / 4 - halfL / 2, r0 = n * 3 / 4 - halfR / 2;
    std::fill(x, x + l0, 0.0f);
    for (int i = 0; i < halfL; ++i) x[l0 + i] *= slopeL[i];
    for (int i = 0; i < halfR; ++i) x[r0 + i] *= slopeR[halfR - 1 - i];
    std::fill(x + r0 + halfR, x + n, 0.0f);
}

// ------------------------------------------------------------------ floors

// Floor type 1's amplitude scale: 256 steps of 140/256 dB up to 1.0.
struct FloorScale {
    float v[256];
    FloorScale() { for (int i = 0; i < 256; ++i) v[i] = (float)std::pow(10.0, (i - 255) * (140.0 / 256.0 / 20.0)); }
};

const FloorScale kFloorScale;
constexpr int kFloorRange[4] = {256, 128, 86, 64};
struct Floor {
    int type = 1;
    // type 1: a curve drawn through points ("posts") at fixed places in the spectrum
    int partitions = 0, mult = 1, rangeBits = 0, values = 0;
    unsigned char partClass[32] = {};
    unsigned char classDim[16] = {}, classSub[16] = {}, classMaster[16] = {};
    int subBook[16][8] = {};
    int x[65] = {};
    unsigned char low[65] = {}, high[65] = {};      // each post's nearest neighbours among the posts before it
    unsigned char sorted[65] = {};                  // the posts in order of x
    // type 0: a filter given as line spectral pairs
    int order = 0, rate = 0, barkMapSize = 0, ampBits = 0, ampOffset = 0, books = 0;
    unsigned char bookList[16] = {};
    std::vector<int> map[2];                        // bin to step along the Bark scale, for each block size
    // Works out the neighbours and the drawing order. False if two posts share an x.
    bool finishPosts() {
        for (int i = 0; i < values; ++i) sorted[i] = (unsigned char)i;
        std::sort(sorted, sorted + values, [&](unsigned char a, unsigned char b) { return x[a] < x[b]; });
        for (int i = 1; i < values; ++i) if (x[sorted[i]] == x[sorted[i - 1]]) return false;
        for (int i = 2; i < values; ++i) {
            int lo = 0, hi = 1;
            for (int j = 0; j < i; ++j) {
                if (x[j] < x[i] && x[j] > x[lo]) lo = j;
                if (x[j] > x[i] && x[j] < x[hi]) hi = j;
            }
            low[i] = (unsigned char)lo;
            high[i] = (unsigned char)hi;
        }
        return true;
    }
};

// Where the straight line from (x0, y0) to (x1, y1) is at x, in the integer arithmetic the format fixes.
inline int renderPoint(int x0, int y0, int x1, int y1, int x) {
    const int dy = y1 - y0, adx = x1 - x0;
    const int off = (dy < 0 ? -dy : dy) * (x - x0) / adx;
    return dy < 0 ? y0 - off : y0 + off;
}

// Turns the values read from a packet into the height of each post: each is coded as its distance from
// the line between its two neighbours. step2[i] says whether post i takes part in the final curve.
void floor1Unwrap(const Floor& f, const int* y, int* finalY, bool* step2) {
    const int range = kFloorRange[f.mult - 1];
    step2[0] = step2[1] = true;
    finalY[0] = std::min(y[0], range - 1);
    finalY[1] = std::min(y[1], range - 1);
    for (int i = 2; i < f.values; ++i) {
        const int lo = f.low[i], hi = f.high[i];
        const int predicted = renderPoint(f.x[lo], finalY[lo], f.x[hi], finalY[hi], f.x[i]);
        const int val = y[i], highRoom = range - predicted, lowRoom = predicted;
        const int room = (highRoom < lowRoom ? highRoom : lowRoom) * 2;
        int v = predicted;
        if (val) {
            step2[lo] = step2[hi] = step2[i] = true;
            if (val >= room) v = highRoom > lowRoom ? val - lowRoom + predicted : predicted - val + highRoom - 1;
            else v = (val & 1) ? predicted - (val + 1) / 2 : predicted + val / 2;
        } else step2[i] = false;
        finalY[i] = clampT(v, 0, range - 1);
    }
}

// Draws the curve through the posts that take part, as n2 multipliers.
void floor1Render(const Floor& f, const int* finalY, const bool* step2, int n2, float* curve) {
    int lx = 0, ly = clampT(finalY[0] * f.mult, 0, 255);
    for (int s = 1; s < f.values; ++s) {
        const int i = f.sorted[s];
        if (!step2[i]) continue;
        const int hx = f.x[i], hy = clampT(finalY[i] * f.mult, 0, 255);
        if (lx < n2) {
            const int dy = hy - ly, adx = hx - lx, base = dy / adx, sy = dy < 0 ? base - 1 : base + 1;
            const int ady = (dy < 0 ? -dy : dy) - (base < 0 ? -base : base) * adx;
            const int end = hx < n2 ? hx : n2;
            int yy = ly, err = 0;
            curve[lx] = kFloorScale.v[yy];
            for (int xx = lx + 1; xx < end; ++xx) {
                err += ady;
                if (err >= adx) { err -= adx; yy += sy; } else yy += base;
                curve[xx] = kFloorScale.v[yy];
            }
        }
        lx = hx;
        ly = hy;
    }
    for (int xx = lx; xx < n2; ++xx) curve[xx] = kFloorScale.v[ly];
}

inline double bark(double hz) { return 13.1 * std::atan(0.00074 * hz) + 2.24 * std::atan(0.0000000185 * hz * hz) + 0.0001 * hz; }

// ------------------------------------------------------------------ the rest of a stream's setup

struct Residue {
    int type = 0, begin = 0, end = 0, partSize = 1, classes = 1, classBook = 0;
    unsigned passes = 0;            // bit p set if any class has a book in pass p
    int books[64][8] = {};
};

struct Mapping {
    int submaps = 1, steps = 0;
    std::vector<unsigned char> magnitude, angle, mux;
    unsigned char floor[16] = {}, residue[16] = {};
};

struct Mode {
    int blockFlag = 0, mapping = 0;
};

struct Setup {
    int channels = 0;
    uint32 rate = 0;
    int blockSize[2] = {};
    std::vector<Codebook> books;
    std::vector<Floor> floors;
    std::vector<Residue> residues;
    std::vector<Mapping> mappings;
    std::vector<Mode> modes;
};

bool isVorbisHeader(const unsigned char* p, size_t n, int type) {
    return n >= 7 && p[0] == type && std::memcmp(p + 1, "vorbis", 6) == 0;
}

bool readIdentification(const unsigned char* p, size_t n, Setup& s) {
    if (!isVorbisHeader(p, n, 1) || n < 30) return false;
    if (le32(p + 7) != 0) return false;             // the version
    s.channels = p[11];
    s.rate = le32(p + 12);
    s.blockSize[0] = 1 << (p[28] & 15);
    s.blockSize[1] = 1 << (p[28] >> 4);
    if (s.channels < 1 || s.rate < 1) return false;
    if (s.blockSize[0] < 64 || s.blockSize[1] > 8192 || s.blockSize[0] > s.blockSize[1]) return false;
    return (p[29] & 1) != 0;                        // the framing bit
}

bool readSetup(const unsigned char* p, size_t n, Setup& s) {
    if (!isVorbisHeader(p, n, 5)) return false;
    BitReader b(p + 7, n - 7);
    int64 entriesLeft = kMaxBookEntries, valuesLeft = kMaxBookValues;
    const int bookCount = (int)b.bits(8) + 1;
    s.books.resize((size_t)bookCount);
    for (auto& cb : s.books) if (!readCodebook(b, cb, entriesLeft, valuesLeft)) return false;
    auto bookOk = [&](int i) { return i >= 0 && i < bookCount; };
    auto vectorBookOk = [&](int i) { return bookOk(i) && s.books[(size_t)i].hasVectors && s.books[(size_t)i].dim > 0; };

    const int times = (int)b.bits(6) + 1;           // placeholders that must all be zero
    for (int i = 0; i < times; ++i) if (b.bits(16) != 0) return false;

    s.floors.resize((size_t)b.bits(6) + 1);
    for (auto& f : s.floors) {
        f.type = (int)b.bits(16);
        if (f.type == 0) {
            f.order = (int)b.bits(8);
            f.rate = (int)b.bits(16);
            f.barkMapSize = (int)b.bits(16);
            f.ampBits = (int)b.bits(6);
            f.ampOffset = (int)b.bits(8);
            f.books = (int)b.bits(4) + 1;
            for (int i = 0; i < f.books; ++i) {
                f.bookList[i] = (unsigned char)b.bits(8);
                if (!vectorBookOk(f.bookList[i])) return false;
            }
            if (b.eop || f.order < 1 || f.rate < 1 || f.barkMapSize < 1) return false;
            for (int w = 0; w < 2; ++w) {
                const int n2 = s.blockSize[w] / 2;
                f.map[w].resize((size_t)n2 + 1);
                const double scale = (double)f.barkMapSize / bark(0.5 * f.rate);
                for (int i = 0; i < n2; ++i)
                    f.map[w][(size_t)i] = std::min(f.barkMapSize - 1, (int)std::floor(bark((double)f.rate * i / (2.0 * n2)) * scale));
                f.map[w][(size_t)n2] = -1;
            }
        } else if (f.type == 1) {
            f.partitions = (int)b.bits(5);
            int maxClass = -1;
            for (int i = 0; i < f.partitions; ++i) {
                f.partClass[i] = (unsigned char)b.bits(4);
                maxClass = std::max(maxClass, (int)f.partClass[i]);
            }
            for (int c = 0; c <= maxClass; ++c) {
                f.classDim[c] = (unsigned char)(b.bits(3) + 1);
                f.classSub[c] = (unsigned char)b.bits(2);
                if (f.classSub[c]) {
                    f.classMaster[c] = (unsigned char)b.bits(8);
                    if (!bookOk(f.classMaster[c])) return false;
                }
                for (int j = 0; j < (1 << f.classSub[c]); ++j) {
                    f.subBook[c][j] = (int)b.bits(8) - 1;
                    if (f.subBook[c][j] >= bookCount) return false;
                }
            }
            f.mult = (int)b.bits(2) + 1;
            f.rangeBits = (int)b.bits(4);
            f.x[0] = 0;
            f.x[1] = 1 << f.rangeBits;
            f.values = 2;
            for (int i = 0; i < f.partitions; ++i)
                for (int j = 0; j < f.classDim[f.partClass[i]]; ++j) {
                    if (f.values >= 65) return false;
                    f.x[f.values++] = (int)b.bits(f.rangeBits);
                }
            if (b.eop || !f.finishPosts()) return false;
        } else return false;
    }

    s.residues.resize((size_t)b.bits(6) + 1);
    for (auto& r : s.residues) {
        r.type = (int)b.bits(16);
        if (r.type > 2) return false;
        r.begin = (int)b.bits(24);
        r.end = (int)b.bits(24);
        r.partSize = (int)b.bits(24) + 1;
        r.classes = (int)b.bits(6) + 1;
        r.classBook = (int)b.bits(8);
        if (b.eop || !bookOk(r.classBook) || s.books[(size_t)r.classBook].dim < 1) return false;
        unsigned cascade[64];
        for (int c = 0; c < r.classes; ++c) {
            const unsigned lowBits = b.bits(3);
            cascade[c] = (b.bits(1) ? b.bits(5) << 3 : 0u) | lowBits;
        }
        for (int c = 0; c < r.classes; ++c)
            for (int pass = 0; pass < 8; ++pass) {
                r.books[c][pass] = -1;
                if (!(cascade[c] & (1u << pass))) continue;
                r.books[c][pass] = (int)b.bits(8);
                if (!vectorBookOk(r.books[c][pass])) return false;
                r.passes |= 1u << pass;
            }
    }

    s.mappings.resize((size_t)b.bits(6) + 1);
    for (auto& m : s.mappings) {
        if (b.bits(16) != 0) return false;
        m.submaps = b.bits(1) ? (int)b.bits(4) + 1 : 1;
        if (b.bits(1)) {
            m.steps = (int)b.bits(8) + 1;
            const int width = ilog((uint32)(s.channels - 1));
            for (int i = 0; i < m.steps; ++i) {
                const unsigned mag = b.bits(width), ang = b.bits(width);
                if (mag == ang || mag >= (unsigned)s.channels || ang >= (unsigned)s.channels) return false;
                m.magnitude.push_back((unsigned char)mag);
                m.angle.push_back((unsigned char)ang);
            }
        }
        if (b.bits(2) != 0) return false;
        m.mux.assign((size_t)s.channels, 0);
        if (m.submaps > 1)
            for (auto& x : m.mux) {
                x = (unsigned char)b.bits(4);
                if (x >= m.submaps) return false;
            }
        for (int i = 0; i < m.submaps; ++i) {
            b.bits(8);
            m.floor[i] = (unsigned char)b.bits(8);
            m.residue[i] = (unsigned char)b.bits(8);
            if (m.floor[i] >= s.floors.size() || m.residue[i] >= s.residues.size()) return false;
        }
    }

    s.modes.resize((size_t)b.bits(6) + 1);
    for (auto& m : s.modes) {
        m.blockFlag = (int)b.bits(1);
        if (b.bits(16) != 0 || b.bits(16) != 0) return false;       // window and transform types: only zero exists
        m.mapping = (int)b.bits(8);
        if (m.mapping >= (int)s.mappings.size()) return false;
    }
    return b.bits(1) == 1 && !b.eop;
}

// ------------------------------------------------------------------ decoder

struct Decoder {
    Setup s;
    Mdct mdct[2];
    std::vector<float> slope[2];
    std::vector<std::vector<float>> spec, curve;    // by channel: the residue, then the spectrum; and the floor
    std::vector<unsigned char> unused, skip;
    std::vector<float> inter, block;
    std::vector<int> classes;
    std::vector<float*> vecs;
    std::vector<unsigned char> vecSkip;
    int outCh[2] = {0, 0}, outs = 1;
    std::vector<float> tail[2];     // the second half of the block before, windowed
    int prevN = 0;
    void start() {
        for (int w = 0; w < 2; ++w) {
            mdct[w].init(s.blockSize[w]);
            makeSlope(slope[w], s.blockSize[w] / 2);
        }
        const size_t n2 = (size_t)s.blockSize[1] / 2;
        spec.assign((size_t)s.channels, std::vector<float>(n2));
        curve.assign((size_t)s.channels, std::vector<float>(n2));
        unused.assign((size_t)s.channels, 0);
        skip.assign((size_t)s.channels, 0);
        block.resize((size_t)s.blockSize[1]);
        outs = s.channels > 1 ? 2 : 1;
        outCh[0] = 0;
        // In Vorbis's channel orders, three channels and five to eight have the centre second.
        outCh[1] = s.channels == 3 || (s.channels >= 5 && s.channels <= 8) ? 2 : 1;
        for (auto& t : tail) t.assign(n2, 0.0f);
        prevN = 0;
    }

    // False if this channel has no floor in this block (it is silent).
    bool decodeFloor(const Floor& f, BitReader& b, int w, float* out) {
        const int n2 = s.blockSize[w] / 2;
        if (f.type == 1) {
            if (!b.bits(1)) return false;
            const int range = kFloorRange[f.mult - 1];
            int y[65], finalY[65];
            bool step2[65];
            y[0] = (int)b.bits(ilog((uint32)range - 1));
            y[1] = (int)b.bits(ilog((uint32)range - 1));
            int at = 2;
            for (int i = 0; i < f.partitions; ++i) {
                const int cls = f.partClass[i], cdim = f.classDim[cls], cbits = f.classSub[cls];
                int cval = 0;
                if (cbits) {
                    cval = s.books[f.classMaster[cls]].decode(b);
                    if (cval < 0) return false;
                }
                for (int j = 0; j < cdim; ++j) {
                    const int book = f.subBook[cls][cval & ((1 << cbits) - 1)];
                    cval >>= cbits;
                    int v = 0;
                    if (book >= 0) {
                        v = s.books[(size_t)book].decode(b);
                        if (v < 0) return false;
                    }
                    y[at + j] = v;
                }
                at += cdim;
            }
            if (b.eop) return false;
            floor1Unwrap(f, y, finalY, step2);
            floor1Render(f, finalY, step2, n2, out);
            return true;
        }

        // Type 0: an amplitude, then the line spectral pairs of a filter whose response is the curve.
        uint64 amp = b.bits(std::min(f.ampBits, 32));
        if (f.ampBits > 32) amp |= (uint64)b.bits(f.ampBits - 32) << 32;
        if (amp == 0 || b.eop) return false;
        const unsigned number = b.bits(ilog((uint32)f.books));
        if (b.eop || number >= (unsigned)f.books) return false;
        const Codebook& book = s.books[f.bookList[number]];
        float coef[256 + 1];
        float last = 0.0f;
        for (int have = 0; have < f.order;) {
            const int e = book.decode(b);
            if (e < 0) return false;
            const float* v = book.vectors.data() + (size_t)e * (size_t)book.dim;
            for (int j = 0; j < book.dim && have < f.order; ++j) coef[have++] = v[j] + last;
            last = v[book.dim - 1] + last;
        }
        for (int j = 0; j < f.order; ++j) coef[j] = 2.0f * std::cos(coef[j]);
        const float scale = (float)((double)amp * f.ampOffset / (std::ldexp(1.0, f.ampBits) - 1.0));
        const std::vector<int>& map = f.map[w];
        for (int i = 0; i < n2;) {
            const int k = map[(size_t)i];
            const float wv = 2.0f * std::cos((float)kPi * (float)k / (float)f.barkMapSize);
            float p = 0.5f, q = 0.5f;
            int j = 1;
            for (; j < f.order; j += 2) { q *= wv - coef[j - 1]; p *= wv - coef[j]; }
            if (j == f.order) {         // an odd order has one more on the q side
                q *= wv - coef[j - 1];
                p *= p * (4.0f - wv * wv);
                q *= q;
            } else {
                p *= p * (2.0f - wv);
                q *= q * (2.0f + wv);
            }
            const float v = std::exp(0.11512925f * (scale / std::sqrt(p + q) - (float)f.ampOffset));
            do out[i++] = v; while (map[(size_t)i] == k);
        }
        return true;
    }

    // Adds the vectors of one residue to `count` spectra of `size` numbers each.
    void decodeResidue(const Residue& r, BitReader& b, float* const* v, const unsigned char* skipIt, int count, int size) {
        const Codebook& classBook = s.books[(size_t)r.classBook];
        const int words = classBook.dim;
        const int begin = std::min(r.begin, size), end = std::min(r.end, size);
        const int parts = (end - begin) / r.partSize;
        if (parts <= 0) return;
        const size_t stride = (size_t)parts + (size_t)words;
        classes.assign(stride * (size_t)count, 0);
        for (int pass = 0; pass < 8; ++pass) {
            if (pass && !(r.passes >> pass)) break;
            for (int part = 0; part < parts;) {
                if (pass == 0)
                    for (int j = 0; j < count; ++j) {
                        if (skipIt[j]) continue;
                        int temp = classBook.decode(b);
                        if (temp < 0) return;
                        int* c = classes.data() + stride * (size_t)j + (size_t)part;
                        for (int i = words - 1; i >= 0; --i) { c[i] = temp % r.classes; temp /= r.classes; }
                    }
                for (int i = 0; i < words && part < parts; ++i, ++part)
                    for (int j = 0; j < count; ++j) {
                        if (skipIt[j]) continue;
                        const int bookNo = r.books[classes[stride * (size_t)j + (size_t)part]][pass];
                        if (bookNo < 0) continue;
                        const Codebook& book = s.books[(size_t)bookNo];
                        const int dim = book.dim;
                        float* out = v[j] + begin + part * r.partSize;
                        if (r.type == 0) {
                            const int step = r.partSize / dim;
                            for (int k = 0; k < step; ++k) {
                                const int e = book.decode(b);
                                if (e < 0) return;
                                const float* t = book.vectors.data() + (size_t)e * (size_t)dim;
                                for (int d = 0; d < dim; ++d) out[k + d * step] += t[d];
                            }
                        } else if (dim == 2 && !(r.partSize & 1)) {
                            for (int k = 0; k < r.partSize; k += 2) {
                                const int e = book.decode(b);
                                if (e < 0) return;
                                const float* t = book.vectors.data() + (size_t)e * 2;
                                out[k] += t[0];
                                out[k + 1] += t[1];
                            }
                        } else {
                            for (int k = 0; k < r.partSize;) {
                                const int e = book.decode(b);
                                if (e < 0) return;
                                const float* t = book.vectors.data() + (size_t)e * (size_t)dim;
                                for (int d = 0; d < dim && k < r.partSize; ++d) out[k++] += t[d];
                            }
                        }
                    }
            }
        }
    }

    // Decodes one audio packet and appends what it completes to out[0..outs-1]. Returns how many
    // samples that was, or -1 for a packet that is not decodable audio (it is then left out).
    int decodeAudio(const unsigned char* data, size_t bytes, std::vector<float>* out) {
        BitReader b(data, bytes);
        if (b.bits(1) != 0 || b.eop) return -1;
        const unsigned modeNo = b.bits(ilog((uint32)s.modes.size() - 1));
        if (b.eop || modeNo >= s.modes.size()) return -1;
        const Mode& mode = s.modes[modeNo];
        const int w = mode.blockFlag, n = s.blockSize[w], n2 = n / 2;
        int wl = w, wr = w;         // the window shape each side: a long block uses a short slope beside a short block
        if (w) {
            wl = (int)b.bits(1);
            wr = (int)b.bits(1);
            if (b.eop) return -1;
        }
        const Mapping& map = s.mappings[(size_t)mode.mapping];
        const int channels = s.channels;

        for (int c = 0; c < channels; ++c) {
            unused[(size_t)c] = !decodeFloor(s.floors[map.floor[map.mux[(size_t)c]]], b, w, curve[(size_t)c].data());
            skip[(size_t)c] = unused[(size_t)c];
        }
        for (int i = 0; i < map.steps; ++i)
            if (!skip[map.magnitude[(size_t)i]] || !skip[map.angle[(size_t)i]]) skip[map.magnitude[(size_t)i]] = skip[map.angle[(size_t)i]] = 0;

        for (int c = 0; c < channels; ++c) std::fill(spec[(size_t)c].begin(), spec[(size_t)c].begin() + n2, 0.0f);
        for (int sub = 0; sub < map.submaps; ++sub) {
            vecs.clear();
            vecSkip.clear();
            bool any = false;
            for (int c = 0; c < channels; ++c)
                if (map.mux[(size_t)c] == sub) {
                    vecs.push_back(spec[(size_t)c].data());
                    vecSkip.push_back(skip[(size_t)c]);
                    any = any || !skip[(size_t)c];
                }
            if (!any) continue;
            const Residue& r = s.residues[map.residue[sub]];
            const int count = (int)vecs.size();
            if (r.type == 2) {          // the channels are coded as one vector, interleaved
                inter.assign((size_t)n2 * (size_t)count, 0.0f);
                float* one = inter.data();
                const unsigned char never = 0;
                decodeResidue(r, b, &one, &never, 1, n2 * count);
                if (count == 2) {
                    float* a = vecs[0];
                    float* c2 = vecs[1];
                    for (int i = 0; i < n2; ++i) { a[i] = one[2 * i]; c2[i] = one[2 * i + 1]; }
                } else {
                    for (int c = 0; c < count; ++c) {
                        float* a = vecs[(size_t)c];
                        for (int i = 0; i < n2; ++i) a[i] = one[(size_t)i * (size_t)count + (size_t)c];
                    }
                }
            } else decodeResidue(r, b, vecs.data(), vecSkip.data(), count, n2);
        }

        for (int i = map.steps - 1; i >= 0; --i) {
            float* m = spec[map.magnitude[(size_t)i]].data();
            float* a = spec[map.angle[(size_t)i]].data();
            for (int k = 0; k < n2; ++k) {
                const float mv = m[k], av = a[k];
                if (mv > 0.0f) {
                    if (av > 0.0f) a[k] = mv - av;
                    else { a[k] = mv; m[k] = mv + av; }
                } else {
                    if (av > 0.0f) a[k] = mv + av;
                    else { a[k] = mv; m[k] = mv - av; }
                }
            }
        }

        const int produced = prevN ? prevN / 4 + n / 4 : 0;
        const int halfL = s.blockSize[wl] / 2, halfR = s.blockSize[wr] / 2;
        for (int o = 0; o < outs; ++o) {
            const int c = outCh[o];
            float* x = block.data();
            if (unused[(size_t)c]) std::fill(x, x + n, 0.0f);
            else {
                float* sp = spec[(size_t)c].data();
                const float* fl = curve[(size_t)c].data();
                for (int k = 0; k < n2; ++k) sp[k] *= fl[k];
                mdct[w].inverse(sp, x);
                applyWindow(x, n, slope[wl].data(), halfL, slope[wr].data(), halfR);
            }
            if (produced) {
                // The second half of the block before and the first half of this one, lined up so that
                // this block's middle is the end of what is produced.
                const size_t at = out[o].size();
                out[o].resize(at + (size_t)produced);
                float* dst = out[o].data() + at;
                const int fromTail = std::min(prevN / 2, produced);
                std::copy(tail[o].begin(), tail[o].begin() + fromTail, dst);
                std::fill(dst + fromTail, dst + produced, 0.0f);
                const int shift = produced - n2;        // where this block starts in dst; negative if it starts before
                for (int i = std::max(0, -shift); i < n2; ++i) dst[i + shift] += x[i];
            }
            std::copy(x + n2, x + n, tail[o].begin());
        }
        prevN = n;
        return produced;
    }
};

std::shared_ptr<SampleData> readOgg(const unsigned char* d, size_t size, std::string* error) {
    auto fail = [&](const char* m) { if (error) *error = m; return std::shared_ptr<SampleData>(); };
    // The first page that is whole and passes its checksum; anything before it (a tag, say) is skipped.
    size_t pos = 0;
    Page pg;
    bool any = false;
    for (; size - pos >= 27; ++pos) {
        const void* hit = std::memchr(d + pos, 'O', size - pos);
        if (!hit) break;
        pos = (size_t)((const unsigned char*)hit - d);
        if (readPage(d + pos, size - pos, pg) == PageRead::Ok) { any = true; break; }
    }
    if (!any) return fail("not an Ogg file");

    Decoder dec;
    auto s = std::make_shared<SampleData>();
    bool found = false, ended = false, sawOpus = false;
    uint32 serial = 0, nextSeq = 0;
    int packets = 0;                    // of the Vorbis stream, counting its three headers
    std::vector<unsigned char> partial; // a packet that began on an earlier page
    bool inPartial = false;
    int64 position = -1;                // the granule position: samples up to the end of the last packet, once a page has said
    int64 produced = 0;                 // samples decoded
    int64 trimStart = 0, keep = -1;     // samples to drop from the front, and how many to keep in all (-1 = all)
    const char* headerError = nullptr;

    auto packet = [&](const unsigned char* p, size_t n) {
        const int index = packets++;
        if (index == 0) {
            if (!readIdentification(p, n, dec.s)) headerError = "bad Vorbis identification header";
        } else if (index == 1) {
            if (!isVorbisHeader(p, n, 3)) headerError = "bad Vorbis comment header";
        } else if (index == 2) {
            if (!readSetup(p, n, dec.s)) headerError = "bad Vorbis setup header";
            else {
                dec.start();
                s->sampleRate = dec.s.rate;
                s->channels = dec.outs;
            }
        } else {
            const int got = dec.decodeAudio(p, n, s->ch);
            if (got > 0) {
                produced += got;
                if (position >= 0) position += got;
            }
        }
    };

    bool damaged = false;
    while (pos < size) {
        const PageRead got = readPage(d + pos, size - pos, pg);
        if (got != PageRead::Ok) {          // damage, or the file ends here
            damaged = got != PageRead::Short;
            break;
        }
        pos += pg.bytes;
        if (!found) {
            if (!(pg.flags & 2) || pg.segments == 0 || pg.lacing[0] == 255) continue;
            if (pg.lacing[0] >= 8 && std::memcmp(pg.body, "OpusHead", 8) == 0) sawOpus = true;
            if (!isVorbisHeader(pg.body, pg.lacing[0], 1)) continue;
            found = true;
            serial = pg.serial;
            nextSeq = pg.seq;
        } else if (pg.serial != serial) continue;       // a page of another stream
        else if (pg.flags & 2) break;                   // the same serial number starting again
        if (pg.seq != nextSeq) { damaged = true; break; }       // pages are missing
        ++nextSeq;

        const unsigned char* body = pg.body;
        int seg = 0;
        const int packetsBefore = packets;
        if (pg.flags & 1) {
            if (!inPartial) {           // the start of this packet was never seen: skip the rest of it
                while (seg < pg.segments) {
                    const int l = pg.lacing[seg++];
                    body += l;
                    if (l < 255) break;
                }
            }
        } else if (inPartial) inPartial = false;        // the packet before was never finished
        while (seg < pg.segments && !headerError) {
            const unsigned char* start = body;
            bool whole = false;
            while (seg < pg.segments) {
                const int l = pg.lacing[seg++];
                body += l;
                if (l < 255) { whole = true; break; }
            }
            const size_t len = (size_t)(body - start);
            if (inPartial) {
                partial.insert(partial.end(), start, body);
                if (whole) { packet(partial.data(), partial.size()); inPartial = false; }
            } else if (whole) packet(start, len);
            else { partial.assign(start, body); inPartial = true; }
        }
        if (headerError) return fail(headerError);

        // The page says where its last finished packet ends. On the first page that says so, a figure
        // lower than what was decoded means the stream starts part way into its first block; on the
        // last page it means the stream ends part way through its last one.
        const bool last = (pg.flags & 4) != 0;
        if (pg.granule >= 0 && packets > std::max(packetsBefore, 3)) {
            if (position < 0) {
                if (pg.granule < produced) {
                    if (last) keep = pg.granule;
                    else trimStart = produced - pg.granule;
                }
            } else if (position > pg.granule && last) keep = std::max<int64>(trimStart, produced - (position - pg.granule));
            position = pg.granule;
        }
        if (last) { ended = true; break; }
    }

    if (!found) return fail(sawOpus ? "Ogg Opus files cannot be read" : "no Vorbis stream in this Ogg file");
    if (packets < 3) return fail(damaged ? "Ogg Vorbis headers are damaged" : "Ogg Vorbis headers are cut short");
    if (!ended && produced == 0) return fail(damaged ? "Ogg Vorbis file is damaged before any audio" : "Ogg Vorbis file is cut short");
    for (int c = 0; c < s->channels; ++c) {
        auto& v = s->ch[c];
        if (keep >= 0 && (size_t)keep < v.size()) v.resize((size_t)keep);
        if (trimStart > 0) v.erase(v.begin(), v.begin() + (std::ptrdiff_t)std::min<int64>(trimStart, (int64)v.size()));
        // A stream can be built (or damaged) so that its numbers overflow; the engine must never be handed those.
        for (float& x : v) if (!(std::fabs(x) <= 3.0e38f)) x = 0.0f;
    }
    return s;
}

// ------------------------------------------------------------------ encoder

struct BitWriter {
    std::vector<unsigned char> o;
    uint64 acc = 0;
    int n = 0;
    void put(uint32 v, int k) {         // k is 0 to 32 and v has no bits above k
        acc |= (uint64)v << n;
        n += k;
        while (n >= 8) { o.push_back((unsigned char)acc); acc >>= 8; n -= 8; }
    }
    void finish() { if (n) { o.push_back((unsigned char)acc); acc = 0; n = 0; } }
};

// The residue of a block is the spectrum divided by the floor and rounded to whole numbers. It is
// coded in partitions; each partition is put in the smallest class that holds its largest number,
// and a class codes its numbers in up to three stages, coarse to fine: each stage is a book of
// vectors whose numbers run from -half to half in steps of `step`.
struct StageShape { int dim, half, step; };
struct ClassShape { int limit, stages; StageShape stage[3]; };
constexpr int kClassCount = 11;
constexpr ClassShape kClasses[kClassCount] = {
    {0, 0, {}},
    {1, 1, {{4, 1, 1}}},
    {2, 1, {{4, 2, 1}}},
    {4, 1, {{2, 4, 1}}},
    {7, 1, {{2, 7, 1}}},
    {12, 1, {{2, 12, 1}}},
    {37, 2, {{4, 1, 25}, {2, 12, 1}}},
    {112, 2, {{2, 4, 25}, {2, 12, 1}}},
    {312, 2, {{2, 12, 25}, {2, 12, 1}}},
    {1562, 3, {{2, 2, 625}, {2, 12, 25}, {2, 12, 1}}},
    {8124, 3, {{2, 12, 625}, {2, 12, 25}, {2, 12, 1}}},
};
constexpr int kMaxResidue = 8124;

// The books of a stream, in a fixed order. For each block size: four for the floor (which of the
// three value books each post of a partition uses, then those three) and one for the residue classes
// of two partitions at a time. Then one for each stage of each residue class.
constexpr int kFloorSubEntries[3] = {4, 16, 128};
constexpr int kBooksPerSize = 5, kFirstValueBook = 2 * kBooksPerSize, kBookCount = kFirstValueBook + 17;
enum { kBookMaster = 0, kBookSub = 1, kBookClass = 4 };

struct BookShape { int entries, dim, half, step; };     // half = 0: the entry number is the value
BookShape bookShape(int book) {
    if (book < kFirstValueBook) {
        const int role = book % kBooksPerSize;
        if (role == kBookMaster) return {256, 1, 0, 0};
        if (role == kBookClass) return {kClassCount * kClassCount, 2, 0, 0};
        return {kFloorSubEntries[role - kBookSub], 1, 0, 0};
    }
    int at = kFirstValueBook;
    for (int c = 1; c < kClassCount; ++c)
        for (int p = 0; p < kClasses[c].stages; ++p, ++at)
            if (at == book) {
                const StageShape& st = kClasses[c].stage[p];
                int entries = 1;
                for (int i = 0; i < st.dim; ++i) entries *= 2 * st.half + 1;
                return {entries, st.dim, st.half, st.step};
            }
    return {0, 0, 0, 0};
}
int valueBook(int cls, int pass) {
    int at = kFirstValueBook;
    for (int c = 1; c < cls; ++c) at += kClasses[c].stages;
    return at + pass;
}

// The codeword length of every entry of every book, as letters ('A' = 1 bit): one set for mono and for
// stereo coded as left and right, one for stereo coded as magnitude and difference. Made by
// dev/oggtool.cpp from how often the encoder used each entry on a set of synthetic signals.
//@@TABLES-BEGIN
const char* const kBookLengths[2][kBookCount] = {
    {   // mono, and stereo as left and right
        "DHEIHKHKFHEHJLIJHKHKJLJMHJGJLMJLFHEHHJHJFHEGIKHIHJGIJKIKGIFHIKHIHJHKJLJLHJHJLMKLJLJLLMLMKLJLMNLMHJHJJLIKHJGIKMJJILJKLMKLJKIJK"
        "MJJFHFHIJHJFHEGJKHHHKHJJLJLHJGIKMJJFHEGHJGIEGDFIJGFGJGHHKIJFIFGIJHGJKIJLMKLIKHHKLIHKMKLMNMMJLJJLMKJIKHIKMJJHJGGJKHGKLJILMJKIK"
        "HGJLIG",
        "CCBA",
        "HHHHCCCCCDDDDDFE",
        "MMMMMMMMMMMMMMMMDCDDDDEDEDFEFEFEFEGFHFHFIFIGJGJGJGJGKGKGKHKHKHKILILILILILJLILJLJMJMJMJMJMKMJMKMKMKMKMKMLMKMKMLMLMLMMMLMMMMMMM"
        "MMM",
        "DIKKLLLMLMMFEGIKKKLMMMHFEFIJKLLMMHGEEFHJLLLMIHGEDEHKLLMIIHFEDEJLLMIIIGEDCFJLMJKJIHGEDHJMLLKKKJGFGJMLLLKLLJGIJMMMMMMMMLLMM",
        "CFEJGIGLEGEIKLJJGIHLIKJMHJHKMMLLFHFIHJHKFHEHJLIIKLKKLMLMJLIJLMJJFIGLIKIMHJGKMMKKILJMLMLMJLJLMNMMHJHKJLJLHJGJLMJJMMLMMNMMLMKKM"
        "MLKDGEIGIGKEGEHJKHIHJHKJLJLHJGJLMJJFHEHHJGJFGEGIJGGJKIJLMKKIKHHJKIHHJIKJLKLHKHIKLJIJLKLMMLMKLJKMMKJGJHIJLJKHJGHJKHGILIJLMKKJK"
        "HHJKHG",
        "CCBA",
        "HHHHBCCDCDDDEEEF",
        "MMMMMMMMMMMMMMMMDDDDDDEDEEEEFEFEFEGEGFGFGFGFHFHFIGIGIGJGJGJHKHKHKIKIKILILJLILJLJLJLKLJLKLKLKLKLKLJLKLLLKMLLLLKMJMKMLLLMLMLMLM"
        "MML",
        "CFIIIJIJLMMFBEGIIIJKLMIEDFIJIKLLMIGEDFHIJKLMIHHEEFHJKLMIIIHFEGIKLMIHIHGFEGJKMJIJJJIGFIKMKKKKKKIHHJMLLLLLLKJIIMMMMMMMMMMMM",
        "KJLJGJKJKJHJHDHJHJKJKJHJKJLJHJHEHJHJGEGEAEGDGJHJHEHJHJLJKJHJKJKJHJHDHJHJKIKJGJLJK",
        "MNNOPNLLMONLKLNOMLLNPONNNNMMMOMKJKMMJIJMMKJKMOMLMNNMKMNMKIKMLIHILMKIJMNLKLNOMMMOMKJKMMJIJMMKJKMOMLMOPONNNOMLMONLKMOOMLMOPONOP"
        "NMMMOMKJKMMJIJLMKJKMOMLMNLKKKMKIHIKKHGHKKIHIKMKJKLLJIKLJHGHJIGEGIJHGHJLJIJLMKKKMKIHIKKHGHKKIHIKMKKKMOMMMNMKJKMLJIKMMKJKMOMMMO"
        "NMLMNMKIKLKIHIKMKIJLNMKLNLJIJLJHGHJIGEGIJHGHJLJIJLKIHIKIGEGIHECEHIGEGIKIHIKLJIJLJHGHJIGEGIJHGHJLJIJLNLKMNLJIKMKIHIKLKIKMNMLMN"
        "OMMMOMKJKMMKIJLMKJKMNMMMOMKKKMKIHIKKHGHKKIHIKMKKKMLJIJLJHGHJIGEGIJHGHJLKIJLLKJKMKIHIKKHGHKKIHIKMKKKLNMLMOMKJKMLJIJMMKJKMOMMMN"
        "PONOPOMLMONMKLNOMLMONNNOPOMLMOMKJKMMJIJMMKJKMOMMMONLKLNMJIKMLIHILMKIKMNMKMNNMLMOMKJKMMJIJMMKJKMOMMMNNNNOPNLLMONLKLNOMLLNPONNM",
        "KJJIIIJKKJIHGGHHIKJHGFFFGHJIGFEDEFHIIGFDBDFGIIHFEDEFGIJHGFFFGHJKIHHGGHIJKKJIIIJJK",
        "LLLLKKKJKKKLLMMLKKKJJJIJJJKKLMLKJJIIHHHIIJKKLKKJIIHHHHHIJJKLKJIIHGGGGHHIIJKKJIHGGFFFGHHIJKKJHHGFEEEFGHIJKJIHHGFECEFGHHIJKJHHG"
        "FEEEFGHHJKKJIHHGFFFGGHIJKKJIIHHGGGGHIIJKLKJJIHHHHHIIJKKLKKJIIHHHIIJJKLMLKKJJJIJJJKKKLMMLLKKKJJKKKLLL",
        "NNNNNNMMMMMLLLMMMMNNNNOOONMMMMMMMLLLLKLLLLMMMNNNOONMMMMLLLLKKKKKKLLLLMMMNNONMMLLLLKKKKJJJKKKKLLMMMNNNMMLKKKKJJJJIJJJKKKLLMMNN"
        "NMLLKKKKJJJJJJJJJKKLLLMMNMMLLKKJJJIIIIIIJJJKKKLLMMMMLKKKJIIIIHHIIIIJJKKKLMMMLLKJJJIIHHHHHHIIIJJKKLLMMLKKJJIIHHHGGGHHIIJJJKLLM"
        "MLKKJJIIHHGGGGGHHIIJJKKLMLLKJJJIHHGGFFFGGHHIJJJKLLLKKJIIIHHGGECEGGHHIIIJKKLLLKJJJIHHGGFFFGGHHIJJJKLLMLKKJJIIHHGGGGGHHIIJJKKLM"
        "MLLKJJJIIHHGGGHHHIIJJKKLMMLLKKJJIIIHHHHHHIIJJJKLLMMMLKKKJJIIIHHHIIIIJKKKLMMMMLLKKKJJJIIIIIIJJJKKLLMMNMMLLLKKJJJJJJJJJKKKKLLMN"
        "NNMMLLKKKJJJIJJJJKKKKLMMNNNMMMLLKKKKJJJKKKKLLLLMMNONNMMMLLLLKKKKKKLLLLMMMMNOONNNMMMLLLLKLLLLMMMMMMMNOOONNNNMMMMLLLMMMMMNNNNNN",
        "JJLJGJKJJJHJHEHJHJLJKJHJLJLJHJHEHJHJGEGDADGEGJHJHEHJHJLJLJHJKJLJHJHDHJHJJJKJGJLJJ",
        "KKKKKKKKKKJJJJJKKKKKKKKKKKKKKKKKKKKJJJJJKKKKKKKKKKKKKKKKKKKJJJJJJKKKKKKKKKKKKKKKKKKJJJJJJJJKKKKKKKKKKKKKJJJJJJJJJJJJJJKKKKKKK"
        "KKKKJJJJJJJJIJJJJJJKKKKKKKKKKJJJJJJIIIIJJJJJJKKKKKKKKKJJJJIIIIIIIIJJJJJKKKKKKKJJJJIIIIIHIIIIJJJJJKKKKKJJJJJIIHHHHHHIIIJJJJKKK"
        "JJJJJJIIIHHGGGHHIIIJJJJJJJJJJJJIIIHGFFGGHIIIJJJJJJJJJJIIIIHHGECEGHHIIIIJJJJJJJJJJIIIHGGFFGHIIIJJJJJJJJJJJJIIIHHGGGHHIIIJJJJJJ"
        "KKKJJJJIIIHHHHHHIIJJJJJKKKKKJJJJJIIIIHIIIIIJJJJKKKKKKKJJJJJIIIIIIIIJJJJKKKKKKKKKJJJJJIIIIIJJJJJJKKKKKKKKKKJJJJJJIJJJJJJJJKKKK"
        "KKKKKKKJJJJJJJJJJJJJJKKKKKKKKKKKKKJJJJJJJJKKKKKKKKKKKKKKKKKKJJJJJJKKKKKKKKKKKKKKKKKKKJJJJJKKKKKKKKKKKKKKKKKKKKJJJJJKKKKKKKKKK",
        "LLKKJKKLMLJJIHIJKLKJHGFHIJKKIGFDFGIKIHFDADFHIKIGFDFGIKKJIHFGHJKLKJIHIJJLMLKKJKKKL",
        "JJKKKJJJJJJJJJJJJJJKKKKKJJJJJJJJJJJJJJJJJJJJKKKKKKKJJJJJJJJJJJJJJJJJJJKKKKKKJJJJJJJJJJJIJJJJJJJJKKKKJKJJJJJJJJJJIJJJJJJJJKKKK"
        "JJJJJJJJJJJJIJJJJJJJJJJKKJJJJJJJJJJJIIIJJJJJJJJJJJJJJJJJJJJIIIIIIJJJJJJJJJJJJJJJJJJIIIIHIIIJJJJJJJJJJJJJJJJIIIIHHHIIIJJJJJJJJ"
        "JJJJJJJIIIHHGHHIIIJJJJJJJJJJJJJIIIHHGFGHHIIIJJJJJJIIIIIIIHHGGFCFGGHHIIIIIIIJJJJJJIIIHHGFGHHIIIJJJJJJJJJJJJJIIIHHGHHIIIJJJJJJJ"
        "JJJJJJJJIIIHHHIIIIJJJJJJJJJJJJJJJJIIIHIIIIJJJJJJJJJJJJJJJJJJIIIIIIJJJJJJJJJJJJJJJJJJJJIIIJJJJJJJJJJJKKJJJJJJJJJJIJJJJJJJJJJJJ"
        "KKKKJJJJJJJJIJJJJJJJJJJKJKKKKJJJJJJJJIJJJJJJJJJJJKKKKKKJJJJJJJJJJJJJJJJJJJKKKKKKKJJJJJJJJJJJJJJJJJJJJKKKKKJJJJJJJJJJJJJJKKKJJ",
        "OOOOOOOONNNMLNNNNOOOPPPPQOOOOONNNNNMMLMNNNNOOOOPPPOONNNNNNNMMMKLMMNNNNOOOOPOONNNNMMMMMLKLMMMNNNNOOOPOONNMMMMMLLLJLLMMMNNNNOOO"
        "ONNNMLLLLLLKJKLLLMMMNNOOOONNMMLLLKKKJIJKKLLMMMNNOONNNMMLLKKJJJIJJKKLLMMNNNONNMMMLKKKJJJIJJKKKLLMMNNONNMMLLKKJJIIHIJJKKKLLMMNN"
        "NMMLLKKJJIHHGHIJJJKKLLMMNNMMLLKJJJIHFEFHIJJJKKLMMMLKKJJJIHIHGEADGHIHIJJJKKLMMMLKKJJJIHFEFHIJJJKLLMMNNMMLLKKJJJIHGHHIJJKKLLMMN"
        "NNMMLLKKKJJIHIIJJKKLLMMNNONNMMLLKKKJJIJJJKKKLMMMNNONNNMMLLKKJJIJJJKKLLMMNNNOONNMMMLLKKJIJKKKLLLMMNNOOOONNMMMLLLKJKLLLLLLMNNNO"
        "OOONNNNMMMLLJLLLMMMMMNNOOPOOONNNNMMMLKLMMMMMNNNNOOPOOOONNNNMMLKMMMNNNNNNNOOPPPOOOONNNNMLMMNNNNNOOOOOQPPPOOOONNNNLMNNNOOOOOOOO",
        "JJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJ"
        "JJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJIIIIJJJJJJJJJJJJJJJJJJJJIIIHIIIJJJJJJJJJJJJJJJJJIIIIHIIIJJJJJJJJJ"
        "JJJJJJJJIIHHGHHIIJJJJJJJJJJJJJJIIIIHGFGHIIIIJJJJJJIIIIIIHHHHGFCFGHHHHIIIIIIJJJJJJIIIIHGFGHIIIIJJJJJJJJJJJJJJIIHHGHHIIJJJJJJJJ"
        "JJJJJJJJJIIIHIIIIJJJJJJJJJJJJJJJJJIIIHIIIJJJJJJJJJJJJJJJJJJJJIIIIJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJ"
        "JJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJ",
        "JIHIJIFDFIGCACGIFDEIJIGIJ",
        "LMMMMMMMLLLKJKLLLMMMMMMMLMLMMMMMMMLLLJLLLMMMMMMMMMMMLLMMMLLLLKJKLLLMMMMMMMMMMMLLMLMLLLKJKLLLMMMMMMMMMMMMLLLLLLLKJKLLLMMMMMMMM"
        "MMMLLLLLLLKKJKLLLLMMMMMMMMMLMLLLLLKKKJKKLLLLMMMMMLMLMMLLLKKKKKIJKKLLLLLMLMMLLLLLLLKJKJJIJJKKLLLLLLMLLLLLLLLKJJJIHIJJKKLLLLLLL"
        "LLLLLKKKJIHHGHIJJKKKLLLLLKLLLKKKJJIHFEGHIJJKKKKKKLJJJJJIIIHHGEAEFHHIIIJJJJJLKKKKKKJJIHGEFHIJJKKKLLLKLLLLLKKKJJIHGHHIJKKKLLLLL"
        "LLLLLLLKKJJIHIJJJKLLLLLLLLMLLLLLLKKJJIJJKJKLLLLLLLMMLMLLLLLKKJIKKKKKLLLMMLMLMMMMMLLLLKKJKKKLLLLLMLMMMMMMMMMLLLLKJKKLLLLLLLMMM"
        "MMMMMMMMLLLKJKLLLLLLLMMMMMMMMMMMMLLLKJKLLLMLMLLMMMMMMMMMMMLLLKJKLLLLMMMLLMMMMMMMMMMMLLLJLLLMMMMMMMLMLMMMMMMMLLLKJKLLLMMMMMMML",
        "JJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJ"
        "JJJJJJJJJJJIIIJJJJJJJJJJJJJJJJJJJJJIIIIIJJJJJJJJJJJJJJJJJJJIIIIIIJJJJJJJJJJJJJJJJJJIIIIHIIIJJJJJJJJJJJJJJJJIIIIHHHIIIJJJJJJJJ"
        "JJJJJJIIIIHHGHHIIIJJJJJJJJJJJJIIIIHHGFGHHIIIIJJJJJIIIIHHHHHGGFDFGGHHHHHIIIIJJJJJIIIIHHGFGHHIIIIJJJJJJJJJJJJIIIHHGHHIIIIJJJJJJ"
        "JJJJJJJJIIIHHHIIIIJJJJJJJJJJJJJJJJIIIHIIIIJJJJJJJJJJJJJJJJJJIIIIIIJJJJJJJJJJJJJJJJJJJIIIIIJJJJJJJJJJJJJJJJJJJJJIIIJJJJJJJJJJJ"
        "JJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJJJJJJJJJJJJJIJJJJJJJJJJJJ",
        "QQQPPPPPOOOOOOOOOPPPPPQQQQQPPPPPOOOOONNOOOOPPPPPQQQPPPPPOOOONNNNNOOOOPPPPPQPPPPPOOOONNNNNNNOOOOPPPPPPPPPOOOONNNNNNNNNOOOOPPPP"
        "PPPOOOONNNNNMNNNNNOOOOPPPPPOOOONNNNNMMMNNNNNOOOOPPPOOOONNNNNMMMMMNNNNNOOOOPOOOONNNNNMMMIMMMNNNNNOOOOOOONNNNNMGHMEMMMMNNNNNOOO"
        "OONNNNNMMMMIGMMMMMNNNNNOOONNNNNMMIGMGDHIMMMMNNNNNONNNNNMMMMEFEAEFDMMMMNNNNNONNNNNMMMMIHDGMGIMMNNNNNOOONNNNNMMMMMGHMMMMNNNNNOO"
        "OOONNNNNMMMMEMHGMNNNNNOOOOOOONNNNNMMMIMMMNNNNNOOOOPOOOONNNNNMMMMMNNNNNOOOOPPPOOOONNNNNMMMNNNNNOOOOPPPPPOOOONNNNNMNNNNNOOOOPPP"
        "PPPPOOOONNNNNNNNNOOOOPPPPPPPPPOOOONNNNNNNOOOOPPPPPQPPPPPOOOONNNNNOOOOPPPPPQQQPPPPPOOOONNNOOOOPPPPPQQQQQPPPPPOOOONOOOOPPPPPQQQ",
        "JJJQQJQPJJPHOIJPIPQQQJRJRJRQQQQPPPPPIOOIIPPPQQQQQJQQJJQJJPJPHOHOIPPPJPQJJQQQQQQPPJPPIOIOIIOPPPPJJQQQQJJJPPIPIOOIIHOOOPIPPPQQJ"
        "QQPPPIPOIOHIIHIOOOIIPPPQQQPPJPPOOOONIHIHOOOOIIPPPJPPJPPOOIIIHHNNNIIIOIPPJPPJPPPOOOOIHHIHHIINOIOOPPPJPPPOOOONHHIIGIHHNIOIOOPPJ"
        "PIOOOOIIIGHFGGNIINNOOOOIPPIOOOHNIIGGGGGGIHHIHHOOOIOHHHGGGHGGFECEFGGGGGGHHHOIOOOHHIHHIGGGGGGIINHOOOIOPIOOOONNIINGGFHGIIIOOOOIP"
        "JPOOOIOINHHIGIIHHNOOOOOPPJPPOOOIONIIHHIHHIOOOOOPPJPPJPOIOIIINNNHHIIIOOOPJPPJPPPIIOOOOHIHINOOOOOPJPPQQQPPPIIOOOIHIIHOIOOIPPPQQ"
        "JQQPPPIOOOOHIIOOIOIPPJJJQQQQJJPPPOOIIOIOIOPJPPQQQQQQJJQPJPPOIOHOHOIPJJQJJQQJQQQQQPPPIIOOIOPPPPQQQQQJRJQJQQQPIPIIOHPJJPQJQQJJJ",
        "JRRQJJJJPPPPIIPPJHJQJQIJRIIIQHJQPIIPIIIPIIJJJIIJJIIJJQQQPIIHOIOIOIPIPJQJIIJHQJIQPIPPIOOOOOOIIPJPIJIJJJQJJIIIOIOOIIIIOIPPPPQQJ"
        "JQJPIHPOIHOOIIIOIHIIIJPQQIHPPIIOOOIONINOOHHIIIPPJJJPPIIIHOOHINNIIHOIOIHIHJPHPHIIOOHOIHNNINIIOOHIIPHPHPIIOIOOHNHHIHNIIOHHHOIPI"
        "HHIHOIIIIIIHHIINNIIHHHIIIIOIIIHHHIHHMMIGHGGIOIIHGIIHHHIHIHIGGHHHGGIHIHIHHHIIGHIIOIGGHGIMMHHIHHHIIIOIIIIHHHIINNIIHHIIIIIIOHIHH"
        "IPIOHHHNIINHIHHNHNOIOIIPHPHPIIHOOIININNHINHOOIIHPHPJHIHIOIOHIINNIHOOHIIIPPJJJPPIIIHHONNINNIOOOIIPPHIQQPJIIIHIOIIINOHHOPHIPJQJ"
        "JQQPPPPIOIIIIOOIOIIIJJQJJJIJIPJPIIOOOOOOIPPIPPIJQHJIIJQJPIPIOIOIOHIIPPQQJJIIJJIIJJJIIPIIIPIIPPJHQIIIRJIQJQJHJPPIIPPPPJJJJQRRJ",
    },
    {   // stereo as magnitude and difference
        "DGEGHJHIFHEHIKHJGJHKJMJLHJHJLMKKFHFHHKHJFHEHIKHHHJHJJKJLHJGIKLIIGJHJJLJKHJHJKMJKJKJMMNLMKLJLMMMLHKHKJMJLHJHJKLJJJLJLLMKMJKJKL"
        "LKJEHEGHJHIFHEGIKHHHJHJJLJKHJHILMJIFHEGHJHIEHEFHJGFGHGIHJIJGIFGIKHGHKIKKLKLHJHIJLIFKLKLLMMMKLJJMNKHHJHIJKJKGIGGJKHFIKHIKLJJIJ"
        "GHJLIG",
        "CCBA",
        "HHHHBCCDDDDDEDFD",
        "MMMMMMMMMMMMMMMMDCDDEDEDEDEEFEGEGEHFHFHFHFIFIGIGJGJGKGKGLHKHLHLHMHMHMHLHMIMHMILIMHLIMHMIMIMJMIMJMKMKMLMLMLMMMLMLMMMMMMMMMMMMM"
        "MMM",
        "CGKLLLLMMMMFEGIKLKLMMMIFEFIKKLMMMJGFEFHJKLMMKIGEDEHKLMMKJIFEDEJLLMLJIGFDCFKKMLKKJIGEDIKLMLLLLIGGHKMMMMMLLHHIKMMMMMMLKLLMM",
        "CFEKGIGLEGEJJKIKGIHMJKJMHJHKLMLLEHFJHJHLFHFIJKIIKLJLMMKLJLIIKMJJGIHLIKIMHJHKKLKLJKJMLMLMJLJMMMLMHJHLJLJMHJGJKLJKMMKMMMMMLMKJM"
        "MLKDGEIGIGKEGEHIJHJGJHLJLJMHIGJKLJKFHFIHJGJFGEGIJGHIJHIKLJKHIGHIKHHGJHKKLJLHJHJKLJIILKMMMLMKLKLLMKKGJGJKLJKGJFIJKHGKLJJLMKKIK"
        "HHHJGH",
        "CCBA",
        "HHHHBCCDCDDDEEFE",
        "MMMMMMMMMMMMMMMMDDEDEDEDEEEEEEFEFEGEGEGFFFGFIFIFJFJFJGJGKGLGLHLHLHKHKHKHKHKILILJLJLJLJLJLJLJLKLKLKLKLKLKLKMLMLLLLLMLMMMMMMMMM"
        "MML",
        "BFHHIIHJKLMFCEHIIIJKLMHEDFIJIJLLMHGEDFIIJKLMHIHEEFHJKLMIIIHFEGIKLMHHIHHFEGIKMIIJIIIGGIJMKJKKKJIHHJMMLLLLKJJJJMMMMMMMMMMMM",
        "JMIJFIIMIMKMKJKMKMIMJIFJIMIJKIGDFJKIFJFDADFJFIKJFDGIKJIMIJFIJMIMKMKJKMKMIMIIEJIMJ",
        "NNQNNOLPLNOMJMOOLPLOONQOOOMPMNOKOKNOLILOOKOKONQPQOQPOPQPONOPONMNOPONOPQPOPQOMPMNOKOKOOLILOOKOKONQPQNNNQNONLPLOOMJMOOLPLOOOQNO"
        "ONPOOLKOKLLJHIKMKOKMOOPOOLKOKLKHNHKJHFHJKINHKLKOKLPONOPONMNONMLMNONMNOPONOPLKOKLKHNHKJHFHJKHNHKLKOKLOOPOOLKOKLKIHJLMKOKMONPOO"
        "OOOOOLJNJKIHGGHLJNJKOOOOOMLNLMJHMHIHFEEGJHMHIMLNLMJIMIJHFLFHGEBDGHFLFHJIMIJMLNLMIHMHJGEEFHIHMHJMLNLMOOOOOKJNJLHGGHIKJNJLOOOOO"
        "OOPNOMKOKMLJHIKLKOKLOOPOOLKOKLKHNHKJHFHJKHNHKLKOKLPONOPONMNONMLMNONMNOPONOPLKOKLKHNHKJHFHJKHNHKLKOKLOOPOOMKOKMKIHJLLKOKLOOPNO"
        "ONQOOOLPLOOMJMOOLPLNONQNNNQPQNOKOKOOLILOOKOKONMPMOQPOPQPONOPONMNOPONOPQPOPQOQPQNOKOKOOLILONKOKONMPMOOOQNOOLPLOOMJMONLPLONNQNN",
        "KJJLLLJJKKIHLKLHIKJHGGKGGHJHGFEJEFGHGEECBCEEGHGFEJEFGHJHGGKGGHJKIHLKLHIJKIJLLLJJK",
        "MMLKOONNNOOKLLMMLKJKNNNNNKJKLMMLKJINNMNNIJKLMLKKJIIMMMIIJKLLLKJIHHMLMHHIJKLKJIIHGHLHGHIIJKIIHHGFELEFGHHIIHGGFFEDBDEFFFGHIIHHG"
        "FELEFGHHIIKJIIHGHLHGHIIJKLKJIHHMLMHHIJKLLLKJIIMMMIIJJKLMLKJINNMNNIJKLMMLKJKNNNNNKJKLMMLLKOONNNOOKLMM",
        "OONNMMNPPPPOOOPPPPNMMNNOOOONMMLLPPPOOOOOPPPLLMMNOOOONMLLLLPOOOOOOOPLLLLMNNOONNMLLKKOOOOOOOOOKKLLMNNONNMMLKKJKOOONOOOKJKKLMNNN"
        "NNMMLLKJJOONNNOOJJKKLMMNNNMMLLKKJJKNNNNNKJJKKLLMMNMMLLKKJJIINNNNNIIJJKKLLMMMLLKKKJJIIJNMNJIIJJKKKLLMLLKKJJJIIHHMMMHHIIJJJKKLL"
        "KKJJJJIIHHHHMHHHHIIJJJKKKKJIIHIHHGGGFMFGGGHHHHIIJKJIIHGGGFFFEDCDEFFFGGGHIIJKJIIHHHHGGGFMFGGGHHIHIIJKKKKJJJIIHHHHMHHHHIIJJJJKK"
        "LLKKJJJIIHHMMMHHIIJJJKKLLMLLKKKJJIIJNMNJIIJJKKKLLMMMLLKKJJIINNNNNIIJJKKLLMMNMMLLKKJJKNNNNNKJJKKLLMMNNNMMLKKJJOONNNOOJJKLLMMNN"
        "NNMMLKKJKOOONOOOKJKKLMMNNONNMLLKKOOOOOOOOOKKLLMNNOONNMLLLLPOOOOOOOPLLLLMNOOOONMMLLPPPOOOOOPPPLLMMNOOOONNMMNPPPPOOOPPPPNMMNNOO",
        "LKLJHJLKLKJKJFJKJKLKLJHJLKLJJJFDFJJJHGHDACHFHJJJFDFJJJLKLJHJLKLKJKJFJKJKLKLJHJLKL",
        "KKKKKKLMMMMMMMMMMMLKKKKKKKKKKKKKMMMMMMMMMMMKKKKKKKKKKKKKKLMMMMMMMMMLKKKKKKKKKKKKKKKMMMMMMMMMKKKKKKKKKKKKKKKKKMMMMMMMKKKKKKKKK"
        "KKKKKKKJJMMMMMMMJJKKKKKKKKKKKKJJJJKLMLLMKJJJKKKKKKJJJJJJJJJJLLLLLJJJJJJJJJJJJJJJJJJIIJLLLJIIJJJJJJJJJJJJJJIIIIILLLIIIIIJJJJJJ"
        "IIIIIIIIIHHIKIHHIIIIIIIIIIIIIHHHHHGGFKFGGHHHHHIIIIHHHHHHGGGFFECEEFGGGHHHHHHIIIIHHHHHGGFKFGGHHHHHIIIIIIIIIIIIIHHIKIHHIIIIIIIII"
        "JJJJJJIIIIILLLIIIIIJJJJJJJJJJJJJJIIJLLLJIIJJJJJJJJJJJJJJJJJJLLLLLJJJJJJJJJJKKKKKKJJJKMLLMLKJJJJKKKKKKKKKKKKJJMMMMMMMJJKKKKKKK"
        "KKKKKKKKKMMMMMMMKKKKKKKKKKKKKKKKKMMMMMMMMMKKKKKKKKKKKKKKKLMMMMMMMMMLKKKKKKKKKKKKKKMMMMMMMMMMMKKKKKKKKKKKKKLMMMMMMMMMMMLKKKKKK",
        "MLLLLLLLMLKJKKKJKLJJIIJIIJJIHGFHFGHIHGECACEGHIHGFHFGHIJJIIJIIJJLKJKKKJKLMLLLLLLLM",
        "KKKKKKKKLKKKKLKKKKKKKKKKKKKKKKKKKKKKKKKKLKLKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK"
        "KKKKKKKKKKKKKKKKKKKKKKKKKKJKKKKJKJKKKKKKKJJJKJKJKJJJJJJJJJJJKKKKKJJJJJJJJJJJJJJJJJJJJJKKKJJJJJJJJJJJJJJJJJIIIIIJJJIIIIIJJJJJJ"
        "IIIIIIIIIIHIJIHIIIIIIIIIIIIIIIHHHHHGFJFGGHHHHIIIIIHHHHHHHGGFFECEFFGGHHHHHHHIIIIIHHHHGGFJFGGHHHHIIIIIIIIIIIIIIIHIJIHIIIIIIIIII"
        "JJJJJJIIIIIJJJIIIIIJJJJJJJJJJJJJJJJJKKKJJJJJJJJJJJJJJJJJJJJJKKKKKJJJJJJJJJJJKJKJKJJJKKKKKKKJKJKKKKJKKKKKKKKKKKKKKKKKKKKKKKKKK"
        "KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKLKLKKKKKKKKKKKKKKKKKKKKKKKKKKLKKKKLKKKKKKKK",
        "QQQPQPPPPPPOOOPPPPPQQQQQQQPPPPPPPPPOOOOOPPPPPPQPPQQQPPPPPOPOOOOOOOPOOPPPPQQPPPOPOOOOOOONOOOOOOPPPQPPOPPPOOOOOOONNNOOOOOOOPOPO"
        "OOOOOONNNONNNNNONNNOOOOOONNNNNNNNMNNNNNNMMMNNONNNNNMMMLMMMLLNNNNNLMMMMMMMMNMMMLLLLLLLMNMNLLLLLLLLMMMMMLLLKKJKKKMMMKKKJKKKLLMM"
        "MLLLKJJIJJJJMJJJJIJJKKLLMLLKKJJIHIHGGIGGHIHIJJJKLLKKJJIIHGGGFDACFGGGHHIJJKKLLKJJJIHIHGGIGGHIHIJJKKLLMLLKKJJIJJJJMJJJJIJJKLLLM"
        "MMLLKKKJKKKMMMKKKJKKLLLMMMMMLLLLLLLLNMNMLLLLLLLMMMNMMMMMMMMLNNNNNLLMMMLMMMNNNNNONNMMMNNNNNNMNNNNNNNNOOOOOONNNONNNNNONNNOOOOOO"
        "OPOPOOOOOOONNNOOOOOOOPPPOPPQPPPOOOOOONOOOOOOOPOPPPQQPPPPOOPOOOOOOOPOPPPPPQQQPPQPPPPPPOOOOOPPPPPPPPPQQQQQQQPPPPOOOOOPPPPPQPQQQ",
        "KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKJKJJJJKKKKKKKKKJKJJKKKK"
        "JJJJKJJJJKKKKKKKJJJJJJJJJJJJJJJJJJJKKKKKJJJJJJJJJJJJJJJJJJJJKKKJKJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJIIIIIIJJJIIIIIJJJJJJ"
        "IIIIIIIIIIHIJIHIIIIIIIIIIIIIIIIHHHHGFJFGHHHHIIIIIIHHHHHHHHGGFECEFGGHHHHHHHHIIIIIIHHHHGFJFGHHHHIIIIIIIIIIIIIIIIHIJIHIIIIIIIIII"
        "JJJJJJIIIIIJJJIIIIIIJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJJKJKKKJJJJJJJJJJJJJJJJJJJJKKKKKJJJJJJJJJJJJJJJJJJJKKKKKKKJJJJKJJJJ"
        "KKKKJJKJKKKKKKKKKJJJJKJKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK",
        "IIHIIHGGGHFCABFHGFGHIIHII",
        "PPOPNNNNNNONOONNNNONNOOOPPOOOONNOMNNNNNNNNNNNONNOPNONNNNNNNNNNMNNNNNNNNNNNNNNNNNONNNNNNNNNNNNNNNNNMNNNNNMMMMNMNNNMNNMNNNNNNMM"
        "LMMMMMMNNNMNMMMNNMMMMMMMLLLMMMMMNMMNMMMMMMMMMMMLLLLLLLLLMMMMMMMMMMMMMMLLLLLLLLLLLLLMMLMMMLLLLLLKKLLLKKKLKKKKLLLLLLLLLKKKKKKLK"
        "KKKKKKJJJJKKLKJJJJJKKKKKKJJJJJJJIIHHGHGGHIIJJJJJJJIIIIIHHHGFEDADEFGHHHIIIIIJJJJJJIIIHGGHGHHIIJJJJJJJKKKKKKJJJJJKLKKJJJJKKKKKK"
        "KLKKKKKKLLLLLLLLLKKKKLKKKLLLKKLLLLLLMMMLMMLLLLLLLLLLLLLMMMMMMMMMMMMMMLLLLLLLLLMMMMMMMMMMMNMMNMMMMMLLLMMMMMMMNNMMMNMNNNMMMMMML"
        "MMNNNNNNMNNMNNNMNMMMMNNNNNMNNNNNNNNNNNNNNNNNONNNNNNNNNNNNNNNNNMNNNNNNNNNNONPONNONNNNNNNNNNNMONNOOOOPPOOONNONNNNOONONNNNNNPOPP",
        "KKKJJKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKJJJKKJKKKKKJKKKKKKKKKKKKKKJJKKKJJKKKKJKKKKKKKKKKKJKKJKKKKKJJKJJKKKKKKKKJJKJJKJKK"
        "JJJJJJJJJKKKKKKKJJJJJKKJJJJJJJJJJJJKKKKKJJJJJJJJJJJJJJJJJJJJKKJKKJJJJJJJJJJJJJJJJJJIIJJJJJIIJJJJJJJJJJJJJIIIIIIJJJIIIIIJJJJJJ"
        "JIJIIIIIIHHIJIHIIIIIIIJIIIIIIIIIHHHGFJFGHHHIIIIIIIIIIHHHHHGGFECEFGGHHHHHIIIIIIIIIIHHHGFJFGHHHIIIIIIIIIJIIIIIIIHIJIHHIIIIIIJIJ"
        "JJJJJJIIIIIJJJIIIIIIJJJJJJJJJJJJJIIJJJJJIIJJJJJJJJJJJJJJJJJJKKJKKJJJJJJJJJJJJJJJJJJJJKKKKKJJJJJJJJJJJJKKJJJJJKKKKKKKJJJJJJJJJ"
        "KKJKJJKJJKKKKKKKKJJKJJKKKKKJKKJKKKKKKKKKKKJKKKKJJKKKJJKKKKKKKKKKKKKKJKKKKKJKKJJJKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKJJKKK",
        "QPPPPPOOOOONNNNOOOOPPPPPQPPPPPOOOONNNNNNNOOOOPPPPPPPPPOOOONNNNNNNNNOOOOPPPPPPPOOOONNNNNMNNNNNOOOOPPPPPOOOONNNNNMMMNNNNNOOOOPP"
        "POOOONNNNNMMMMMNNNNNOOOOPOOOONNNNNMMMMMMMNNNNNOOOOOOONNNNNMMMMMMMMMNNNNNOOOOONNNNNMMMMMLMMMMMNNNNNOOONNNNNMMMMMLLLMMMMMNNNNNO"
        "NNNNNMMMMHLLLLLMHMIMNNNNNNNNNMMMMHFHLLLHHMMMMMNNNNNNNMMMMMFDFDACFDFMMMMMNNNNNNNMMMMMHHLLLHFHMMMMNNNNNNNNNMHMHMLLLLLHMMMMNNNNN"
        "ONNNNNMMMMMLLLMMMMMNNNNNOOONNNNNMMMMMLMMMMMNNNNNOOOOONNNNNMMMMMMMMMNNNNNOOOOOOONNNNNMMMMMMMNNNNNOOOOPOOOONNNNNMMMMMNNNNNOOOOP"
        "PPOOOONNNNNMMMNNNNNOOOOPPPPPOOOONNNNNMNNNNNOOOOPPPPPPPOOOONNNNNNNNNOOOOPPPPPPPPPOOOONNNNNNNOOOOPPPPPQPPPPPOOOONNNNNOOOOPPPPPQ",
        "RQQQQJPPJPPOJOPJPPPJQQQQRQQQQQPPPPPOOJOOPPJPPQQQQJQQJQJJPPPOOOOOOOPPPPPQQJQQJJJPPPJOJOONOOOOJPPPPQQQQQJJPJPOOOONNJOOJOPPPPJQI"
        "QPPPPPOJOONNNNNOOOJPPPPPQJPPPPOJJONNNNNNNOOJOPPPJPPJPPOOOONNNJNNJNNOOOOPPPJPJPOJOONNNNNMNNNNNOOJOPJJPPOJOOJNINNMMMNNNNNJOOOPP"
        "POOOJJIJJNMMMMMNNJNNOJOOJJOJHIJJHIGGEGFHHHHJNIHJOOHHHGHGGGEEECCCEEEGGGHGHHHOOJHINJHHHHFGEGGIHJJIHJOJJOOJONNJNNMMMMMNJJIJJOOOP"
        "PPOOOJNNNNNMMMNNINJOOJOPPJJPOJOONNNNNMNNNNNOOJOPJPJPPPOOOONNJNNINNNOOOOPPJPPJPPPOJOONNNNNNNOJJOPPPPJQPPPPPJOOONNNNNOOJOPPPPPQ"
        "IQJPPPPOJOOJNNOOOOOJPJJQQQQQPPPPJOOOONOOJOJPPPJJJQQJQQPPPPOOOOOOOOOPPJJQJQQJQQQQPPJPOOOJOOOPPPPQQQQQRQQQQJPPPJOOJOOPJPPJQQQQR",
        "IIHIHJHJIPOIJOOPJIJQIJQJRIIHQIHPPJJJHOIOJJIHIIJIHJHIQIJJPJJOIOHOOJOPJJHHQIIIJQJPPIIOOOOIJJJJOPJJIHIHHJHPPIOOOJOHNIOOIJIIHPIJQ"
        "JJHIJJOOJJNIINIJJIJOJHIHJHPIJOOOJIININIIIOIJJJPIPPPIJOOHJINIIIIINIIIJJOHIJJPIOIJIJNIIIIIIINIIJHIHIJIIJOOJJHINIMMIIIIIIIOIJOJJ"
        "JHOOJIINIHIMMIHIGIHIOIHHIIHOJHIIIHHHIIMIIIIHIHOIIIGHJHIGIHHHHGHGHHHHIGIHJHGIIIOHIHIIIIMIIHHHIIIHJOHIIHHIOIHIGIHIMMIHINIIJOOHJ"
        "JJOJIOIIIIHIIMMINIHJJOOJIIJIHIHJIINIHIIIIINJIJIOIPJJIHOJJIIINIIIIINIJHOOJIPPPIPJJJINIIININIIJOOOJIPHJHIHJOJIJJINIINJJOOJJIHJJ"
        "QJIPHIIJIONINHNJOOOIPPHJHHIHIJJPOJJJJINOOOIIPPJQJIIIQHHJJPOJOOHOIOJJPJJIQIHJHIJIIHIJJOIOHJJJPPHIQHIIRJQJIPJIJPOOJIOPIJHJHIHII",
    },
};
//@@TABLES-END

// Codeword lengths in which every entry costs about the same: used where no table has been made yet.
std::vector<unsigned char> evenLengths(int entries) {
    const int k = (int)std::bit_width((unsigned)entries) - 1;
    const int longer = 2 * (entries - (1 << k));
    std::vector<unsigned char> l((size_t)entries, (unsigned char)k);
    for (int i = entries - longer; i < entries; ++i) l[(size_t)i] = (unsigned char)(k + 1);
    return l;
}

struct EncBook {
    BookShape shape;
    std::vector<unsigned char> lens;
    std::vector<uint32> rev;            // the codewords, first bit lowest, as the bit writer wants them
};

// What the encoder knows about one block size.
struct EncSize {
    int n = 0, n2 = 0;
    Floor floor;
    int partSize = 0;
    std::vector<int> bandLo, bandHi, tonLo, tonHi;      // the bins averaged around each bin: for level, for tonality
    std::vector<float> up, down, quiet, tilt;           // masking falling away upwards and downwards; the threshold in quiet; see init()
    int postLo[65] = {}, postHi[65] = {};               // the bins each floor post answers for
    Mdct mdct;
    std::vector<float> slope;
};

struct Encoder {
    int channels = 1;
    uint32 rate = 44100;
    bool coupled = false;               // stereo as the larger of the two channels and their difference
    int maxQuant = kMaxResidue;
    float nmrTone = 0, nmrNoise = 0;
    float level = 1.0f;                 // the square of the file's peak: the threshold in quiet is set against it
    EncSize size[2];
    EncBook books[kBookCount];
    bool classUsed[kClassCount] = {};
    // scratch
    std::vector<float> block, coef[2], curve[2], power, target;
    std::vector<double> sumP, sumL;
    std::vector<int> quant[2], inter, classOf;
    int floorY[2][65] = {};

    void put(BitWriter& w, int book, int entry) {
        const EncBook& b = books[book];
        w.put(b.rev[(size_t)entry], b.lens[(size_t)entry]);
#ifdef PULP_OGG_TRAIN
        ++oggTrainCounts[(size_t)(coupled ? kBookCount : 0) + (size_t)book][(size_t)entry];
#endif
    }

    // The books depend on how stereo is coded, which is decided after init() has set up the rest.
    void makeBooks() {
        maxQuant = coupled ? kMaxResidue / 2 : kMaxResidue;        // a difference can be twice as large as its parts
        for (int b = 0; b < kBookCount; ++b) {
            EncBook& bk = books[b];
            bk.shape = bookShape(b);
            const char* table = kBookLengths[coupled ? 1 : 0][b];
#ifdef PULP_OGG_TRAIN
            if (oggTrainLengths) table = oggTrainLengths[(coupled ? kBookCount : 0) + b];
            if (oggTrainCounts.size() != (size_t)kBookCount * 2) oggTrainCounts.assign((size_t)kBookCount * 2, {});
            oggTrainCounts[(size_t)(coupled ? kBookCount : 0) + (size_t)b].resize((size_t)bk.shape.entries);
#endif
            if (table && std::strlen(table) == (size_t)bk.shape.entries) {
                bk.lens.resize((size_t)bk.shape.entries);
                for (int i = 0; i < bk.shape.entries; ++i) bk.lens[(size_t)i] = (unsigned char)(table[i] - '@');
            } else bk.lens = evenLengths(bk.shape.entries);
            Codebook cb;
            cb.entries = bk.shape.entries;
            cb.lens = bk.lens;
            cb.assignCodes();
            bk.rev.resize((size_t)bk.shape.entries);
            for (int i = 0; i < bk.shape.entries; ++i) bk.rev[(size_t)i] = reverse32(cb.codes[(size_t)i]);
        }
    }

    void init(int ch, uint32 sampleRate, float quality) {
        channels = ch;
        rate = sampleRate;
        const float q = clampT(quality, 0.0f, 1.0f), top = std::max(0.0f, q - 0.8f) / 0.2f;
        // How far below the signal the quantising noise is put, in dB, for pure tones and for noise
        // (before the tilt by frequency), and how far below the threshold of hearing it must be for
        // a file played with its peak at a loud listening level. The top fifth of the scale goes
        // well past what should be audible.
        nmrTone = 10.0f + 32.0f * q + 10.0f * top;
        nmrNoise = 1.0f + 21.0f * q + 8.0f * top;
        const float quietMargin = 10.0f + 30.0f * q + 40.0f * top;

        const int longSize = rate > 64000 ? 4096 : rate > 26000 ? 2048 : rate > 13000 ? 1024 : 512;
        for (int w = 0; w < 2; ++w) {
            EncSize& z = size[w];
            z.n = w ? longSize : longSize / 8;
            z.n2 = z.n / 2;
            z.partSize = w ? 32 : 16;
            z.mdct.init(z.n);
            makeSlope(z.slope, z.n2);
            const int n2 = z.n2;
            const double binHz = (double)rate / (double)z.n;
            auto barkOf = [&](double bin) { return bark(bin * binHz); };

            // The floor's posts: evenly spread along the Bark scale, and listed coarse to fine, so
            // that each is predicted from posts on both sides of it.
            Floor& f = z.floor;
            f.type = 1;
            f.partitions = clampT(n2 / 24, 2, 11);
            const int inner = f.partitions * 4;
            std::vector<int> at((size_t)inner);
            const double top2 = barkOf(n2);
            int prev = 0;
            for (int i = 0; i < inner; ++i) {
                const double want = top2 * (i + 1) / (inner + 1);
                int bin = prev + 1;
                while (bin < n2 - (inner - i) && barkOf(bin) < want) ++bin;
                at[(size_t)i] = prev = bin;
            }
            std::vector<int> order;
            std::vector<std::pair<int, int>> spans{{0, inner}};
            for (size_t k = 0; k < spans.size(); ++k) {         // halve, then halve the halves
                const auto [lo, hi] = spans[k];
                if (lo >= hi) continue;
                const int mid = (lo + hi) / 2;
                order.push_back(at[(size_t)mid]);
                spans.push_back({lo, mid});
                spans.push_back({mid + 1, hi});
            }
            f.classDim[0] = 4;
            f.classSub[0] = 2;
            f.classMaster[0] = (unsigned char)(w * kBooksPerSize + kBookMaster);
            f.subBook[0][0] = -1;
            for (int j = 1; j < 4; ++j) f.subBook[0][j] = w * kBooksPerSize + kBookSub + j - 1;
            f.mult = 2;
            f.rangeBits = std::countr_zero((unsigned)n2);
            f.x[0] = 0;
            f.x[1] = n2;
            f.values = 2 + inner;
            for (int i = 0; i < inner; ++i) f.x[2 + i] = order[(size_t)i];
            f.finishPosts();
            for (int s = 0; s < f.values; ++s) {
                const int i = f.sorted[s];
                const int left = s ? f.x[f.sorted[s - 1]] : 0, right = s + 1 < f.values ? f.x[f.sorted[s + 1]] : n2;
                z.postLo[i] = std::min(n2 - 1, (left + f.x[i] + 1) / 2);
                z.postHi[i] = clampT((f.x[i] + right + 1) / 2, z.postLo[i] + 1, n2);
            }

            // The hearing model's tables.
            z.bandLo.resize((size_t)n2); z.bandHi.resize((size_t)n2); z.tonLo.resize((size_t)n2); z.tonHi.resize((size_t)n2);
            z.up.resize((size_t)n2); z.down.resize((size_t)n2); z.quiet.resize((size_t)n2); z.tilt.resize((size_t)n2);
            for (int k = 0; k < n2; ++k) {
                const double hz = (k + 0.5) * binHz;
                const double barkPerBin = std::max(1.0e-4, barkOf(k + 1.0) - barkOf((double)k));
                const int half = clampT((int)std::lround(0.5 / barkPerBin), 1, n2 / 4);         // about a Bark in all
                z.bandLo[(size_t)k] = std::max(0, k - half);
                z.bandHi[(size_t)k] = std::min(n2, k + half + 1);
                const int wide = std::max(half, w ? 8 : 3);
                z.tonLo[(size_t)k] = std::max(0, k - wide);
                z.tonHi[(size_t)k] = std::min(n2, k + wide + 1);
                z.up[(size_t)k] = (float)std::pow(10.0, -1.5 * barkPerBin);           // 15 dB a Bark towards higher frequencies
                z.down[(size_t)k] = (float)std::pow(10.0, -3.0 * barkPerBin);         // 30 dB a Bark towards lower ones
                const double khz = std::max(hz, 20.0) / 1000.0;
                const double spl = 3.64 * std::pow(khz, -0.8) - 6.5 * std::exp(-0.6 * (khz - 3.3) * (khz - 3.3)) + 0.001 * khz * khz * khz * khz;
                const double db = std::min(spl, 60.0) - 96.0 - quietMargin;
                z.quiet[(size_t)k] = (float)(std::pow(10.0, db / 10.0) * std::min(1.0, barkPerBin));
                // Low frequencies are coded more exactly than the model alone asks and high ones less
                // (12 dB more up to 400 Hz, falling to 8 dB less five octaves above): most of what is
                // heard as accuracy is in the few hundred bins at the bottom, most of the cost is in
                // the thousands at the top.
                const double octaves = clampT(std::log2(std::max(hz, 1.0) / 400.0), 0.0, 5.0);
                z.tilt[(size_t)k] = (float)(12.0 - 4.0 * octaves);
                // Short blocks are used around attacks, where the noise of a block lands in the quiet
                // just before the attack with nothing to hide it: they are coded more exactly throughout.
                if (!w) z.tilt[(size_t)k] = std::max(z.tilt[(size_t)k], 0.0f) + 10.0f;
            }
        }
        const size_t big = (size_t)size[1].n;
        block.resize(big);
        power.resize(big / 2); target.resize(big / 2);
        sumP.resize(big / 2 + 1); sumL.resize(big / 2 + 1);
        for (int c = 0; c < 2; ++c) { coef[c].resize(big / 2); curve[c].resize(big / 2); quant[c].resize(big / 2); }
        inter.resize(big);
        classOf.resize(big / 16 + 2);
    }

    // Chooses the floor for one channel of a block from its spectrum, writes it, and leaves the curve
    // the decoder will draw in curve[c] and the residue in quant[c]. False for a silent channel.
    bool analyse(int w, int c) {
        const EncSize& z = size[w];
        const Floor& f = z.floor;
        const int n2 = z.n2;
        const float* x = coef[c].data();
        float peak = 0.0f;
        for (int k = 0; k < n2; ++k) peak = std::max(peak, std::fabs(x[k]));
        if (!(peak > 1.0e-9f)) return false;

        // How much noise each bin can take: a set distance below the level around it (further below
        // where the spectrum is peaky, which is where a pure tone is), spread to the bins nearby as
        // masking spreads, and never less than what cannot be heard anyway.
        sumP[0] = sumL[0] = 0.0;
        for (int k = 0; k < n2; ++k) {
            const double p = (double)x[k] * x[k] + 1.0e-30;
            sumP[(size_t)k + 1] = sumP[(size_t)k] + p;
            sumL[(size_t)k + 1] = sumL[(size_t)k] + std::log(p);
        }
        float* nb = power.data();
        for (int k = 0; k < n2; ++k) {
            const int lo = z.bandLo[(size_t)k], hi = z.bandHi[(size_t)k], tl = z.tonLo[(size_t)k], th = z.tonHi[(size_t)k];
            const double mean = (sumP[(size_t)hi] - sumP[(size_t)lo]) / (hi - lo);
            const double flatDb = 4.342944819 * ((sumL[(size_t)th] - sumL[(size_t)tl]) / (th - tl) - std::log((sumP[(size_t)th] - sumP[(size_t)tl]) / (th - tl)));
            const double tonal = clampT((-flatDb - 6.0) / 20.0, 0.0, 1.0);      // noise measures about -5.5 dB flat
            const double nmr = nmrNoise + tonal * (nmrTone - nmrNoise) + z.tilt[(size_t)k];
            nb[k] = (float)(mean * std::exp(-0.2302585093 * nmr));
        }
        for (int k = 1; k < n2; ++k) nb[k] = std::max(nb[k], nb[k - 1] * z.up[(size_t)k]);
        for (int k = n2 - 2; k >= 0; --k) nb[k] = std::max(nb[k], nb[k + 1] * z.down[(size_t)k]);
        // The floor is the quantising step: noise of that step has a power of a twelfth of its square.
        // In the floor's own units, which are steps of 140/256 dB times the multiplier, below 1.0.
        float* t = target.data();
        const float unit = 1.0f / (0.02734375f * (float)f.mult);
        for (int k = 0; k < n2; ++k) t[k] = 255.0f / (float)f.mult + 0.5f * unit * std::log10(12.0f * std::max(nb[k], level * z.quiet[(size_t)k]));

        const int range = kFloorRange[f.mult - 1];
        int want[65], y[65], finalY[65];
        bool step2[65];
        for (int i = 0; i < f.values; ++i) {
            float sum = 0.0f;
            for (int k = z.postLo[i]; k < z.postHi[i]; ++k) sum += t[k];
            want[i] = clampT((int)std::lround(sum / (float)(z.postHi[i] - z.postLo[i])), 0, range - 1);
        }
        for (int lift = 0;; lift += 4) {
            // Each post is stored as its distance from the line between two posts before it; one that is
            // within a step of that line is left on it, which costs nothing.
            finalY[0] = y[0] = std::min(want[0] + lift, range - 1);
            finalY[1] = y[1] = std::min(want[1] + lift, range - 1);
            for (int i = 2; i < f.values; ++i) {
                const int lo = f.low[i], hi = f.high[i];
                const int predicted = renderPoint(f.x[lo], finalY[lo], f.x[hi], finalY[hi], f.x[i]);
                const int goal = std::min(want[i] + lift, range - 1), diff = goal - predicted;
                const int highRoom = range - predicted, lowRoom = predicted, room = std::min(highRoom, lowRoom) * 2;
                int v = 0;
                if (diff > 1) {
                    v = 2 * diff;
                    if (v >= room) v = diff + lowRoom;
                } else if (diff < -1) {
                    v = -2 * diff - 1;
                    if (v >= room) v = -diff + highRoom - 1;
                }
                y[i] = v;
                finalY[i] = v ? goal : predicted;
            }
            floor1Unwrap(f, y, finalY, step2);
            floor1Render(f, finalY, step2, n2, curve[c].data());
            const float* fl = curve[c].data();
            int* q = quant[c].data();
            float most = 0.0f;
            for (int k = 0; k < n2; ++k) most = std::max(most, std::fabs(x[k]) / fl[k]);
            if (most > (float)maxQuant && lift < 255) continue;         // the floor is too low to count from: raise it
            for (int k = 0; k < n2; ++k) q[k] = clampT((int)std::lround(x[k] / fl[k]), -maxQuant, maxQuant);
            // Where the steps are coarse, rounding changes how much energy a band has: values under
            // half a step vanish (the band goes dull or hollow) and values just over it grow. Among
            // the values that round to 0 or +-1, enough of the largest zeros are made +-1, or of the
            // smallest +-1 made zero, to leave the band the energy it had.
            const int width = w ? 16 : 8;
            for (int from = 0; from < n2; from += width) {
                float had = 0.0f;
                int ones = 0;
                for (int k = from; k < from + width; ++k)
                    if (q[k] >= -1 && q[k] <= 1) { const float r = x[k] / fl[k]; had += r * r; ones += q[k] != 0 ? 1 : 0; }
                for (int owed = (int)std::lround(had - (float)ones); owed > 0; --owed) {
                    int best = -1;
                    float bestSize = 0.25f;         // anything smaller stays zero
                    for (int k = from; k < from + width; ++k)
                        if (q[k] == 0 && std::fabs(x[k]) / fl[k] > bestSize) { best = k; bestSize = std::fabs(x[k]) / fl[k]; }
                    if (best < 0) break;
                    q[best] = x[best] < 0.0f ? -1 : 1;
                }
                for (int over = (int)std::lround((float)ones - had); over > 0; --over) {
                    int best = -1;
                    float bestSize = 1.0f;          // anything larger stays +-1
                    for (int k = from; k < from + width; ++k)
                        if ((q[k] == 1 || q[k] == -1) && std::fabs(x[k]) / fl[k] < bestSize) { best = k; bestSize = std::fabs(x[k]) / fl[k]; }
                    if (best < 0) break;
                    q[best] = 0;
                }
            }
            bool any = false;
            for (int k = 0; k < n2 && !any; ++k) any = q[k] != 0;
            if (!any) return false;
            break;
        }
        std::copy(y, y + f.values, floorY[c]);
        return true;
    }

    void writeFloor(BitWriter& bw, int w, int c) {
        const Floor& f = size[w].floor;
        const int* y = floorY[c];
        bw.put(1, 1);
        const int bits = ilog((uint32)kFloorRange[f.mult - 1] - 1);
        bw.put((uint32)y[0], bits);
        bw.put((uint32)y[1], bits);
        for (int p = 0; p < f.partitions; ++p) {
            const int* v = y + 2 + p * 4;
            int sub[4], master = 0;
            for (int j = 0; j < 4; ++j) {
                sub[j] = v[j] == 0 ? 0 : v[j] < kFloorSubEntries[0] ? 1 : v[j] < kFloorSubEntries[1] ? 2 : 3;
                master |= sub[j] << (2 * j);
            }
            put(bw, f.classMaster[0], master);
            for (int j = 0; j < 4; ++j) if (sub[j]) put(bw, f.subBook[0][sub[j]], v[j]);
        }
    }

    // Writes `count` whole numbers as a residue.
    void writeResidue(BitWriter& bw, int w, const int* v, int count) {
        const int partSize = size[w].partSize, parts = count / partSize;
        const int classBook = w * kBooksPerSize + kBookClass;
        int* cls = classOf.data();
        for (int p = 0; p < parts; ++p) {
            int most = 0;
            for (int i = 0; i < partSize; ++i) most = std::max(most, std::abs(v[p * partSize + i]));
            int c = 0;
            while (kClasses[c].limit < most) ++c;
            cls[p] = c;
            classUsed[c] = true;
        }
        cls[parts] = 0;
        for (int pass = 0; pass < 3; ++pass)
            for (int p = 0; p < parts;) {
                if (pass == 0) put(bw, classBook, cls[p] * kClassCount + cls[p + 1]);
                for (int i = 0; i < 2 && p < parts; ++i, ++p) {
                    const ClassShape& shape = kClasses[cls[p]];
                    if (pass >= shape.stages) continue;
                    const StageShape& st = shape.stage[pass];
                    const int book = valueBook(cls[p], pass), values = 2 * st.half + 1;
                    const int* part = v + p * partSize;
                    for (int k = 0; k < partSize; k += st.dim) {
                        int entry = 0, scale = 1;
                        for (int d = 0; d < st.dim; ++d) {
                            int r = part[k + d], qv = 0;
                            for (int s = 0; s <= pass; ++s) {       // what the stages before left over, then this one's share
                                const int step = shape.stage[s].step, half = shape.stage[s].half;
                                qv = clampT(r >= 0 ? (r + step / 2) / step : -((-r + step / 2) / step), -half, half);
                                r -= qv * step;
                            }
                            entry += (qv + st.half) * scale;
                            scale *= values;
                        }
                        put(bw, book, entry);
                    }
                }
            }
    }
};

// Vorbis's own way of writing a floating point number in 32 bits.
uint32 packFloat(int v) {
    return (v < 0 ? 0x80000000u : 0u) | (788u << 21) | (uint32)std::abs(v);
}

struct Packet { size_t at, bytes; int64 granule; };

// Lays packets out in Ogg pages.
struct OggWriter {
    std::vector<unsigned char>& o;
    uint32 serial, seq = 0;
    std::vector<unsigned char> lacing, body;
    int64 granule = -1;
    bool continued = false, first = true, hold = false;     // hold: do not end the page after the next packet

    OggWriter(std::vector<unsigned char>& out, uint32 serialNo) : o(out), serial(serialNo) {}
    void flush(bool last = false) {
        if (lacing.empty() && !last) return;
        const size_t start = o.size();
        o.insert(o.end(), {'O', 'g', 'g', 'S', 0, (unsigned char)((continued ? 1 : 0) | (first ? 2 : 0) | (last ? 4 : 0))});
        for (int i = 0; i < 8; ++i) o.push_back((unsigned char)((uint64)granule >> (8 * i)));
        for (int i = 0; i < 4; ++i) o.push_back((unsigned char)(serial >> (8 * i)));
        for (int i = 0; i < 4; ++i) o.push_back((unsigned char)(seq >> (8 * i)));
        o.insert(o.end(), {0, 0, 0, 0, (unsigned char)lacing.size()});
        o.insert(o.end(), lacing.begin(), lacing.end());
        o.insert(o.end(), body.begin(), body.end());
        const uint32 crc = oggCrc(0, o.data() + start, o.size() - start);
        for (int i = 0; i < 4; ++i) o[start + 22 + (size_t)i] = (unsigned char)(crc >> (8 * i));
        ++seq;
        first = false;
        continued = false;
        granule = -1;
        lacing.clear();
        body.clear();
    }
    void packet(const unsigned char* p, size_t n, int64 granulePos) {
        size_t left = n;
        for (;;) {
            if (lacing.size() == 255) { flush(); continued = left < n; }
            const size_t take = std::min<size_t>(left, 255);
            lacing.push_back((unsigned char)take);
            body.insert(body.end(), p + (n - left), p + (n - left) + take);
            left -= take;
            if (take < 255) break;
        }
        granule = granulePos;
        if (!hold && (body.size() >= 4096 || lacing.size() == 255)) flush();
    }
};

std::vector<unsigned char> encodeOggStream(const SampleData& s, float quality) {
    const int channels = s.channels > 1 ? 2 : 1;
    const int64 frames = (int64)s.ch[0].size();
    const uint32 rate = s.sampleRate >= 1.0 ? (uint32)std::lround(std::min(s.sampleRate, 4.0e9)) : 44100u;
    Encoder e;
    e.init(channels, rate, quality);
    const int n0 = e.size[0].n, n1 = e.size[1].n;

    // The samples as the encoder takes them: nothing that is not a number, nothing absurdly large.
    auto sample = [&](int c, int64 i) -> float {
        if (i < 0 || (size_t)i >= s.ch[c].size()) return 0.0f;
        const float v = s.ch[c][(size_t)i];
        return v == v ? clampT(v, -100.0f, 100.0f) : 0.0f;
    };

    float peak = 0.0f;
    for (int c = 0; c < channels; ++c)
        for (size_t i = 0; i < s.ch[c].size() && (int64)i < frames; ++i) peak = std::max(peak, std::fabs(sample(c, (int64)i)));
    e.level = peak > 0.0f ? peak * peak : 1.0f;

    // Where the sound jumps up. Long blocks spread their rounding noise over their whole length, which
    // is heard as an echo before a sudden attack; short blocks are used around each of these instead.
    const int seg = n0 / 2;
    const int64 segs = (frames + seg - 1) / seg;
    std::vector<unsigned char> attack((size_t)segs + 1, 0);
    {
        // A segment is an attack if it is much louder than the eight before it on average, or than
        // both of the two just before it (which catches a burst soon after another). The measure is
        // the energy of the difference between neighbouring samples, which favours the high
        // frequencies an attack is made of and ignores slow swells.
        double history[8] = {};
        float before[2] = {0.0f, 0.0f};
        for (int64 g = 0; g < segs; ++g) {
            double energy = 0.0;
            for (int c = 0; c < channels; ++c)
                for (int64 i = g * seg; i < (g + 1) * seg; ++i) {
                    const float v = sample(c, i), d = v - before[c];
                    before[c] = v;
                    energy += (double)d * d;
                }
            double past = 0.0;
            for (double h : history) past += h;
            const double recent = std::max(history[(g + 7) & 7], history[(g + 6) & 7]);
            if (energy > 1.0e-7 * seg * e.level && (energy > 12.0 * (past / 8.0) || energy > 10.0 * recent)) attack[(size_t)g] = 1;
            history[g & 7] = energy;
        }
    }
    auto attackIn = [&](int64 from, int64 to) {        // any attack in the samples from..to
        for (int64 g = std::max<int64>(0, from / seg); g < segs && g * seg < to; ++g)
            if (attack[(size_t)g]) return true;
        return false;
    };
    // After a block centred at `centre`, a long block would reach from there to past its own middle.
    auto nextSize = [&](int64 centre, int w) {
        const int64 middle = centre + (e.size[w].n + n1) / 4;
        return attackIn(centre, middle + (n1 + n0) / 4) ? 0 : 1;
    };

    // One block of one channel: windowed, transformed and analysed. True if the channel is not silent.
    auto prepare = [&](int c, int64 centre, int w, int wl, int wr) {
        const EncSize& z = e.size[w];
        float* x = e.block.data();
        for (int i = 0; i < z.n; ++i) x[i] = sample(c, centre - z.n2 + i);
        applyWindow(x, z.n, e.size[wl].slope.data(), e.size[wl].n2, e.size[wr].slope.data(), e.size[wr].n2);
        e.size[w].mdct.forward(x, e.coef[c].data());
        const bool used = e.analyse(w, c);
        if (!used) std::fill(e.quant[c].begin(), e.quant[c].begin() + z.n2, 0);
        return used;
    };
    // The stereo residue as the larger of each pair and their difference, in a form the decoder undoes exactly.
    auto couple = [](int a, int b, int& m, int& d) {
        if (std::abs(a) > std::abs(b)) { m = a; d = a > 0 ? a - b : b - a; }
        else { m = b; d = b > 0 ? a - b : b - a; }
    };

    // Stereo is coded as left and right, or as the larger and the difference, whichever gives the
    // smaller numbers over a sample of blocks from the whole file. (One choice for the whole stream:
    // a choice for each block would need more than two modes, which some players do not expect.)
    if (channels == 2) {
        const int64 blocks = clampT<int64>(frames / (n1 / 2), 1, 48);
        double plain = 0.0, joint = 0.0;
        for (int64 j = 0; j < blocks; ++j) {
            const int64 centre = (2 * j + 1) * frames / (2 * blocks);
            const bool usedL = prepare(0, centre, 1, 1, 1), usedR = prepare(1, centre, 1, 1, 1);
            if (!usedL || !usedR) continue;
            const int* l = e.quant[0].data();
            const int* r = e.quant[1].data();
            for (int k = 0; k < n1 / 2; ++k) {
                int m, d;
                couple(l[k], r[k], m, d);
                plain += std::log2(1.0 + std::abs(l[k])) + std::log2(1.0 + std::abs(r[k]));
                joint += std::log2(1.0 + std::abs(m)) + std::log2(1.0 + std::abs(d));
            }
        }
        e.coupled = joint < plain;
    }
    e.makeBooks();

    std::vector<unsigned char> audio;
    std::vector<Packet> packets;
    int64 centre = 0;
    int wPrev = 1, w = attackIn(0, (n1 + n0) / 4) ? 0 : 1;
    for (;;) {
        const int wNext = nextSize(centre, w);
        const EncSize& z = e.size[w];
        const int n = z.n, n2 = z.n2;
        const int wl = w ? wPrev : 0, wr = w ? wNext : 0;
        bool used[2] = {false, false};
        for (int c = 0; c < channels; ++c) used[c] = prepare(c, centre, w, wl, wr);
        const int* v = e.quant[0].data();
        int count = n2;
        if (channels == 2) {
            const int* l = e.quant[0].data();
            const int* r = e.quant[1].data();
            int* out = e.inter.data();
            if (e.coupled) {
                // A silent channel is multiplied by nothing whatever its residue is: give it the other one, so the difference is zero.
                if (!used[0]) l = r;
                if (!used[1]) r = l;
                for (int k = 0; k < n2; ++k) couple(l[k], r[k], out[2 * k], out[2 * k + 1]);
            } else for (int k = 0; k < n2; ++k) { out[2 * k] = l[k]; out[2 * k + 1] = r[k]; }
            v = out;
            count = 2 * n2;
        }

        BitWriter bw;
        bw.put(0, 1);
        bw.put((uint32)w, 1);
        if (w) { bw.put((uint32)wl, 1); bw.put((uint32)wr, 1); }
        for (int c = 0; c < channels; ++c) {
            if (used[c]) e.writeFloor(bw, w, c);
            else bw.put(0, 1);
        }
        if (used[0] || used[1]) e.writeResidue(bw, w, v, count);
        bw.finish();
        packets.push_back({audio.size(), bw.o.size(), std::min(centre, frames)});
        audio.insert(audio.end(), bw.o.begin(), bw.o.end());

        if (centre >= frames && packets.size() >= 2) break;
        centre += (n + e.size[wNext].n) / 4;
        wPrev = w;
        w = wNext;
    }

    // The three headers.
    std::vector<unsigned char> ident = {1, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, (unsigned char)channels};
    auto put32 = [](std::vector<unsigned char>& o, uint32 v) { for (int i = 0; i < 4; ++i) o.push_back((unsigned char)(v >> (8 * i))); };
    put32(ident, rate);
    put32(ident, 0);
    put32(ident, frames > 0 ? (uint32)std::min(4.0e9, (double)audio.size() * 8.0 * rate / (double)frames) : 0u);      // the average bit rate
    put32(ident, 0);
    ident.push_back((unsigned char)(std::countr_zero((unsigned)n0) | (std::countr_zero((unsigned)n1) << 4)));
    ident.push_back(1);

    static const char vendor[] = "PULP Ogg Vorbis encoder 1";
    std::vector<unsigned char> comment = {3, 'v', 'o', 'r', 'b', 'i', 's'};
    put32(comment, (uint32)(sizeof vendor - 1));
    comment.insert(comment.end(), vendor, vendor + sizeof vendor - 1);
    put32(comment, 0);
    comment.push_back(1);

    // The setup: the books of the classes that were used (the others are declared with no books),
    // and a floor, a residue, a mapping and a mode for each block size.
    BitWriter sw;
    for (unsigned char ch : {(unsigned char)5, (unsigned char)'v', (unsigned char)'o', (unsigned char)'r', (unsigned char)'b', (unsigned char)'i', (unsigned char)'s'}) sw.put(ch, 8);
    int number[kBookCount], bookCount = 0;
    for (int b = 0; b < kBookCount; ++b) number[b] = -1;
    for (int b = 0; b < kFirstValueBook; ++b) number[b] = bookCount++;
    for (int c = 1; c < kClassCount; ++c)
        if (e.classUsed[c])
            for (int p = 0; p < kClasses[c].stages; ++p) number[valueBook(c, p)] = bookCount++;
    sw.put((uint32)bookCount - 1, 8);
    for (int b = 0; b < kBookCount; ++b) {
        if (number[b] < 0) continue;
        const EncBook& bk = e.books[b];
        sw.put(0x564342, 24);
        sw.put((uint32)bk.shape.dim, 16);
        sw.put((uint32)bk.shape.entries, 24);
        sw.put(0, 1);           // lengths not in order
        sw.put(0, 1);           // every entry used
        for (unsigned char len : bk.lens) sw.put(len - 1u, 5);
        if (!bk.shape.half) { sw.put(0, 4); continue; }
        const int values = 2 * bk.shape.half + 1;
        sw.put(1, 4);           // the vectors are every combination of a short list of values
        sw.put(packFloat(-bk.shape.half * bk.shape.step), 32);
        sw.put(packFloat(bk.shape.step), 32);
        sw.put((uint32)ilog((uint32)values - 1) - 1, 4);
        sw.put(0, 1);
        for (int i = 0; i < values; ++i) sw.put((uint32)i, ilog((uint32)values - 1));
    }
    sw.put(0, 6);
    sw.put(0, 16);
    sw.put(1, 6);               // two floors
    for (int w2 = 0; w2 < 2; ++w2) {
        const Floor& f = e.size[w2].floor;
        sw.put(1, 16);
        sw.put((uint32)f.partitions, 5);
        for (int p = 0; p < f.partitions; ++p) sw.put(0, 4);
        sw.put((uint32)f.classDim[0] - 1, 3);
        sw.put((uint32)f.classSub[0], 2);
        sw.put((uint32)number[f.classMaster[0]], 8);
        for (int j = 0; j < 4; ++j) sw.put((uint32)(f.subBook[0][j] < 0 ? 0 : number[f.subBook[0][j]] + 1), 8);
        sw.put((uint32)f.mult - 1, 2);
        sw.put((uint32)f.rangeBits, 4);
        for (int i = 2; i < f.values; ++i) sw.put((uint32)f.x[i], f.rangeBits);
    }
    sw.put(1, 6);               // two residues
    for (int w2 = 0; w2 < 2; ++w2) {
        sw.put(channels == 2 ? 2 : 1, 16);
        sw.put(0, 24);
        sw.put((uint32)(e.size[w2].n2 * channels), 24);
        sw.put((uint32)e.size[w2].partSize - 1, 24);
        sw.put(kClassCount - 1, 6);
        sw.put((uint32)number[w2 * kBooksPerSize + kBookClass], 8);
        for (int c = 0; c < kClassCount; ++c) {
            sw.put(e.classUsed[c] ? (1u << kClasses[c].stages) - 1u : 0u, 3);
            sw.put(0, 1);
        }
        for (int c = 1; c < kClassCount; ++c)
            if (e.classUsed[c])
                for (int p = 0; p < kClasses[c].stages; ++p) sw.put((uint32)number[valueBook(c, p)], 8);
    }
    const int mappings = 2;
    sw.put((uint32)mappings - 1, 6);
    for (int m = 0; m < mappings; ++m) {
        sw.put(0, 16);
        sw.put(0, 1);           // one submap
        if (e.coupled) {
            sw.put(1, 1);
            sw.put(0, 8);       // one coupling step: channel 0 holds the magnitude, channel 1 the angle
            sw.put(0, 1);
            sw.put(1, 1);
        } else sw.put(0, 1);
        sw.put(0, 2);
        sw.put(0, 8);
        sw.put((uint32)(m & 1), 8);
        sw.put((uint32)(m & 1), 8);
    }
    sw.put((uint32)mappings - 1, 6);        // one mode for each mapping
    for (int m = 0; m < mappings; ++m) {
        sw.put((uint32)(m & 1), 1);
        sw.put(0, 16);
        sw.put(0, 16);
        sw.put((uint32)m, 8);
    }
    sw.put(1, 1);
    sw.finish();

    std::vector<unsigned char> out;
    out.reserve(audio.size() + audio.size() / 64 + sw.o.size() + 512);
    OggWriter ogg(out, oggCrc(oggCrc(0, sw.o.data(), sw.o.size()), audio.data(), std::min<size_t>(audio.size(), 4096)));
    ogg.packet(ident.data(), ident.size(), 0);
    ogg.flush();
    ogg.packet(comment.data(), comment.size(), 0);
    ogg.packet(sw.o.data(), sw.o.size(), 0);
    ogg.flush();
    for (size_t i = 0; i < packets.size(); ++i) {
        // The last packet goes on a page of its own: some players only shorten the final block
        // properly when the page before has told them where they are.
        if (i + 1 == packets.size()) { ogg.flush(); ogg.hold = true; }
        ogg.packet(audio.data() + packets[i].at, packets[i].bytes, packets[i].granule);
    }
    ogg.flush(true);
    return out;
}

} // namespace

std::shared_ptr<SampleData> readOggMemory(const void* data, size_t size, std::string* error) {
    try {
        return readOgg((const unsigned char*)data, data ? size : 0, error);
    } catch (const std::bad_alloc&) {       // only a file built to ask for it gets here
        if (error) *error = "not enough memory to read this Ogg file";
        return nullptr;
    }
}

std::shared_ptr<SampleData> readOggFile(const std::string& utf8Path, std::string* error) {
    std::vector<unsigned char> b;
    if (!readFileBytes(utf8Path, b)) { if (error) *error = "cannot open " + utf8Path; return nullptr; }
    auto s = readOggMemory(b.data(), b.size(), error);
    if (s) s->path = utf8Path;
    return s;
}

#ifdef PULP_OGG_TRAIN
int oggTrainBookCount() { return kBookCount; }
void oggTrainBookShape(int book, int& entries, int& dim, int& half) {
    const BookShape b = bookShape(book);
    entries = b.entries; dim = b.dim; half = b.half;
}
#endif

std::vector<unsigned char> encodeOgg(const SampleData& s, float quality) {
    return encodeOggStream(s, quality);
}

bool writeOggFile(const std::string& utf8Path, const SampleData& s, float quality, std::string* error) {
    const auto bytes = encodeOgg(s, quality);
    if (!writeFileBytes(utf8Path, bytes.data(), bytes.size())) { if (error) *error = "cannot write " + utf8Path; return false; }
    return true;
}

} // namespace pulp
