#include "pulp/Flac.h"
#include <bit>

namespace pulp {

namespace {

// A frame header ends in a CRC-8 (polynomial 0x07) and the whole frame in a CRC-16 (0x8005); both start at zero.
struct CrcTables {
    unsigned char c8[256] = {};
    unsigned short c16[256] = {};
    constexpr CrcTables() {
        for (unsigned i = 0; i < 256; ++i) {
            unsigned a = i, b = i << 8;
            for (int k = 0; k < 8; ++k) {
                a = (a & 0x80) ? ((a << 1) ^ 0x07) : (a << 1);
                b = (b & 0x8000) ? ((b << 1) ^ 0x8005) : (b << 1);
            }
            c8[i] = (unsigned char)(a & 0xFF);
            c16[i] = (unsigned short)(b & 0xFFFF);
        }
    }
};
constexpr CrcTables kCrc;
unsigned crc8(const unsigned char* p, size_t n) {
    unsigned c = 0;
    while (n--) c = kCrc.c8[c ^ *p++];
    return c;
}
unsigned crc16(const unsigned char* p, size_t n) {
    unsigned c = 0;
    while (n--) c = ((c << 8) & 0xFFFF) ^ kCrc.c16[(c >> 8) ^ *p++];
    return c;
}

// STREAMINFO carries the MD5 of the unencoded audio, which other programs use to verify a file.
struct Md5 {
    uint32 h[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    unsigned char buf[64] = {};
    size_t fill = 0;
    uint64 total = 0;

    void block(const unsigned char* p) {
        static const uint32 k[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const unsigned char rot[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
        uint32 w[16];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32)p[i * 4] | ((uint32)p[i * 4 + 1] << 8) | ((uint32)p[i * 4 + 2] << 16) | ((uint32)p[i * 4 + 3] << 24);
        uint32 a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            uint32 f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
            else { f = c ^ (b | ~d); g = (7 * i) & 15; }
            f += a + k[i] + w[g];
            a = d; d = c; c = b;
            b += std::rotl(f, (int)rot[(i >> 4) * 4 + (i & 3)]);
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    void update(const unsigned char* p, size_t n) {
        total += n;
        while (n) {
            if (fill == 0 && n >= 64) { block(p); p += 64; n -= 64; continue; }
            const size_t take = std::min(n, 64 - fill);
            std::memcpy(buf + fill, p, take);
            fill += take; p += take; n -= take;
            if (fill == 64) { block(buf); fill = 0; }
        }
    }
    void finish(unsigned char out[16]) {
        const uint64 bits = total * 8;
        unsigned char pad[72] = {0x80};
        const size_t padLen = (fill < 56 ? 56 : 120) - fill;
        for (int i = 0; i < 8; ++i) pad[padLen + (size_t)i] = (unsigned char)(bits >> (8 * i));
        update(pad, padLen + 8);
        for (int i = 0; i < 16; ++i) out[i] = (unsigned char)(h[i / 4] >> (8 * (i % 4)));
    }
};

// ------------------------------------------------------------------ decoder

// Reads bits most significant first. Past the end it returns zeros and sets eof, so a damaged or cut
// file can never make the decoder read outside the buffer; every loop that uses it is bounded by a block size.
struct BitReader {
    const unsigned char* start;
    const unsigned char* p;
    const unsigned char* end;
    uint64 acc = 0;        // the next bits, left-aligned; only the top n count (below them may sit bits not yet taken from p)
    int n = 0;
    bool eof = false;

    BitReader(const unsigned char* from, const unsigned char* to) : start(from), p(from), end(to) {}
    void refill() {
        if (end - p >= 8) {         // eight bytes at once; p moves only past the whole bytes that fitted
            const uint64 v = ((uint64)p[0] << 56) | ((uint64)p[1] << 48) | ((uint64)p[2] << 40) | ((uint64)p[3] << 32) |
                             ((uint64)p[4] << 24) | ((uint64)p[5] << 16) | ((uint64)p[6] << 8) | (uint64)p[7];
            acc |= v >> n;
            p += (63 - n) >> 3;
            n |= 56;
            return;
        }
        while (n <= 56 && p < end) { acc |= (uint64)*p++ << (56 - n); n += 8; }
    }
    uint32 bits(int k) {       // k is 0 to 32
        if (k == 0) return 0;
        if (n < k) {
            refill();
            if (n < k) { eof = true; acc = 0; n = 0; return 0; }
        }
        const uint32 v = (uint32)(acc >> (64 - k));
        acc <<= k;
        n -= k;
        return v;
    }
    int64 sbits(int k) {       // a two's complement number of up to 64 bits
        if (k <= 0) return 0;
        uint64 v = k > 32 ? (uint64)bits(k - 32) << 32 : 0;
        v |= bits(k > 32 ? 32 : k);
        return (int64)(v << (64 - k)) >> (64 - k);
    }
    uint32 unary() {           // how many zero bits come before the next one bit
        uint32 q = 0;
        for (;;) {
            const int z = std::countl_zero(acc);
            if (z < n) {
                acc = (acc << z) << 1;
                n -= z + 1;
                return q + (uint32)z;
            }
            q += (uint32)n;
            acc = 0;
            n = 0;
            refill();
            if (n == 0) { eof = true; return 0; }
        }
    }
    // One Rice-coded number: a unary quotient, then k plain bits. Nearly always both are already in acc.
    uint32 rice(int k) {
        if (n < 32) refill();
        const int z = std::countl_zero(acc);
        if (z + 1 + k <= n) {
            acc = (acc << z) << 1;
            const uint32 low = k ? (uint32)(acc >> (64 - k)) : 0u;
            acc <<= k;
            n -= z + 1 + k;
            return ((uint32)z << k) | low;
        }
        const uint32 q = unary();
        return (q << k) | bits(k);
    }
    void align() { const int r = n & 7; acc <<= r; n -= r; }
    size_t consumed() const { return (size_t)(p - start) - (size_t)(n / 8); }       // whole bytes, once aligned
};

struct StreamInfo {
    unsigned rate = 0, channels = 0, bps = 0;
    uint64 total = 0;          // 0 means the encoder did not know
};

struct FrameHeader {
    bool variable = false;     // variable block size: number counts samples, otherwise frames
    int blockSize = 0, channels = 0, bps = 0;
    int assignment = 0;        // 0 independent, 1 left/side, 2 side/right, 3 mid/side
    uint64 number = 0;
    size_t bytes = 0;
};
enum class Hdr { Ok, Short, Bad, Crc };

inline bool isSync(const unsigned char* p) { return p[0] == 0xFF && (p[1] & 0xFE) == 0xF8; }

Hdr parseFrameHeader(const unsigned char* p, size_t avail, const StreamInfo& si, FrameHeader& h) {
    if (avail < 2) return Hdr::Short;
    if (!isSync(p)) return Hdr::Bad;
    if (avail < 6) return Hdr::Short;
    h.variable = (p[1] & 1) != 0;
    const int bsCode = p[2] >> 4, srCode = p[2] & 15, chCode = p[3] >> 4, bpsCode = (p[3] >> 1) & 7;
    if (bsCode == 0 || srCode == 15 || chCode > 10 || bpsCode == 3 || (p[3] & 1)) return Hdr::Bad;
    size_t i = 4;
    // the frame or sample number, coded like UTF-8 but running to seven bytes (36 bits)
    const unsigned lead = p[i++];
    int more = 0;
    uint64 v = lead;
    if (lead >= 0x80) {
        more = std::countl_one((unsigned char)lead) - 1;
        if (more < 1 || more > 6) return Hdr::Bad;
        v = lead & (0x7Fu >> (more + 1));
    }
    for (int k = 0; k < more; ++k) {
        if (i >= avail) return Hdr::Short;
        if ((p[i] & 0xC0) != 0x80) return Hdr::Bad;
        v = (v << 6) | (p[i++] & 0x3Fu);
    }
    h.number = v;
    const size_t tail = (bsCode == 6 ? 1u : bsCode == 7 ? 2u : 0u) + (srCode == 12 ? 1u : srCode >= 13 ? 2u : 0u);
    if (avail - i < tail + 1) return Hdr::Short;
    if (bsCode == 1) h.blockSize = 192;
    else if (bsCode <= 5) h.blockSize = 576 << (bsCode - 2);
    else if (bsCode == 6) h.blockSize = (int)p[i++] + 1;
    else if (bsCode == 7) { h.blockSize = (((int)p[i] << 8) | (int)p[i + 1]) + 1; i += 2; }
    else h.blockSize = 256 << (bsCode - 8);
    // the sample rate is the same in every frame of a file, so STREAMINFO's is used; only its bytes are skipped here
    i += (srCode == 12 ? 1u : srCode >= 13 ? 2u : 0u);
    if (crc8(p, i) != p[i]) return Hdr::Crc;
    h.bytes = i + 1;
    h.channels = chCode < 8 ? chCode + 1 : 2;
    h.assignment = chCode < 8 ? 0 : chCode - 7;
    static const int depth[8] = {0, 8, 12, 0, 16, 20, 24, 32};
    h.bps = bpsCode ? depth[bpsCode] : (int)si.bps;
    return Hdr::Ok;
}

// True if a frame of the same stream starts somewhere in the given bytes. Used to tell damage from a
// file that was cut short: after a cut nothing follows, after damage the next frames are still there.
bool laterFrameExists(const unsigned char* p, size_t n, const StreamInfo& si, const FrameHeader* current) {
    for (size_t i = 0; i + 1 < n; ++i) {
        if (!isSync(p + i)) continue;
        FrameHeader h;
        if (parseFrameHeader(p + i, n - i, si, h) != Hdr::Ok || h.channels != (int)si.channels) continue;
        if (!current || (h.variable == current->variable && h.number > current->number)) return true;
    }
    return false;
}

// The decoding helpers return false for a stream that breaks the format's rules. Running out of data
// is reported through BitReader::eof instead, and the caller looks at that first.
bool decodeResidual(BitReader& br, int64* s, int n, int order) {
    const unsigned method = br.bits(2);
    if (method > 1) return false;
    const int paramBits = method ? 5 : 4, escape = method ? 31 : 15;
    const int partOrder = (int)br.bits(4);
    const int parts = 1 << partOrder;
    if (partOrder > 0 && ((n & (parts - 1)) != 0 || (n >> partOrder) < order)) return false;
    int i = order;
    for (int part = 0; part < parts && !br.eof; ++part) {
        const int end = (n >> partOrder) * (part + 1);
        const int k = (int)br.bits(paramBits);
        if (k == escape) {      // an unencoded partition: plain numbers of a stated width
            const int raw = (int)br.bits(5);
            for (; i < end; ++i) s[i] = br.sbits(raw);
        } else {
            for (; i < end; ++i) {
                const uint32 u = br.rice(k);
                s[i] = (int32_t)((u >> 1) ^ (0u - (u & 1u)));
            }
        }
    }
    return true;
}

// The predictors run in unsigned arithmetic, which gives the right answer for every valid stream and
// has no undefined overflow when a damaged one feeds them nonsense. 64 bits are needed: 24-bit samples
// times 15-bit coefficients summed over 32 taps do not fit in 32.
void restoreFixed(int64* s, int n, int order) {
    auto u = [&](int i) { return (uint64)s[i]; };
    if (order == 1) for (int i = 1; i < n; ++i) s[i] = (int64)(u(i) + u(i - 1));
    else if (order == 2) for (int i = 2; i < n; ++i) s[i] = (int64)(u(i) + 2 * u(i - 1) - u(i - 2));
    else if (order == 3) for (int i = 3; i < n; ++i) s[i] = (int64)(u(i) + 3 * u(i - 1) - 3 * u(i - 2) + u(i - 3));
    else if (order == 4) for (int i = 4; i < n; ++i) s[i] = (int64)(u(i) + 4 * u(i - 1) - 6 * u(i - 2) + 4 * u(i - 3) - u(i - 4));
}
void restoreLpc(int64* s, int n, int order, const int64* coef, int shift) {
    for (int i = order; i < n; ++i) {
        uint64 sum = 0;
        const int64* past = s + i - 1;
        for (int j = 0; j < order; ++j) sum += (uint64)coef[j] * (uint64)past[-j];
        s[i] = (int64)((uint64)s[i] + (uint64)((int64)sum >> shift));
    }
}

bool decodeSubframe(BitReader& br, int64* s, int n, int bps) {
    if (br.bits(1)) return false;
    const unsigned type = br.bits(6);
    int wasted = 0;
    if (br.bits(1)) {           // the low bits of every sample in this block are zero and were left out
        const uint32 count = br.unary();
        if (count >= (uint32)(bps - 1)) return false;        // some bits must be left to store
        wasted = (int)count + 1;
        bps -= wasted;
    }
    if (type == 0) {
        const int64 v = br.sbits(bps);
        std::fill(s, s + n, v);
    } else if (type == 1) {
        for (int i = 0; i < n && !br.eof; ++i) s[i] = br.sbits(bps);
    } else if (type >= 8 && type <= 12) {
        const int order = (int)type - 8;
        if (order > n) return false;
        for (int i = 0; i < order; ++i) s[i] = br.sbits(bps);
        if (!decodeResidual(br, s, n, order)) return false;
        if (!br.eof) restoreFixed(s, n, order);
    } else if (type >= 32) {
        const int order = (int)type - 31;
        if (order > n) return false;
        for (int i = 0; i < order; ++i) s[i] = br.sbits(bps);
        const int precision = (int)br.bits(4) + 1;
        const int shift = (int)br.sbits(5);
        if (precision == 16 || shift < 0) return false;
        int64 coef[32];
        for (int i = 0; i < order; ++i) coef[i] = br.sbits(precision);
        if (!decodeResidual(br, s, n, order)) return false;
        if (!br.eof) restoreLpc(s, n, order, coef, shift);
    } else return false;        // a reserved subframe type
    if (wasted) for (int i = 0; i < n; ++i) s[i] = (int64)((uint64)s[i] << wasted);
    return true;
}

// ------------------------------------------------------------------ encoder

constexpr int kBlock = 4096;
constexpr int kMaxPartOrder = 6;
const unsigned kRates[12] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};

struct BitWriter {
    std::vector<unsigned char>& o;
    uint64 acc = 0;
    int n = 0;
    explicit BitWriter(std::vector<unsigned char>& out) : o(out) {}
    void put(uint32 v, int k) {         // k is 0 to 32 and v has no bits above k
        acc = (acc << k) | v;
        n += k;
        while (n >= 8) { n -= 8; o.push_back((unsigned char)(acc >> n)); }
    }
    void putSigned(int32_t v, int k) { put(k >= 32 ? (uint32)v : ((uint32)v & ((1u << k) - 1u)), k); }
    void rice(uint32 u, int k) {
        const uint32 q = u >> k, low = u & ((1u << k) - 1u);
        if (q + 1 + (uint32)k <= 32) { put((1u << k) | low, (int)q + 1 + k); return; }
        for (uint32 z = q; z;) { const int step = (int)std::min<uint32>(z, 32); put(0, step); z -= (uint32)step; }
        put(1, 1);
        put(low, k);
    }
    void align() { if (n) put(0, 8 - n); }
};

// One channel of one block, analysed: the cheapest way to store it and exactly how many bits that takes.
struct Subframe {
    enum Kind { Constant, Verbatim, Fixed } kind = Constant;
    int bps = 0, wasted = 0, order = 0, partOrder = 0;
    bool rice2 = false;                 // 5-bit Rice parameters, needed once one of them passes 14
    unsigned char k[1 << kMaxPartOrder] = {};
    uint64 bits = 0;
    std::vector<int32_t> x;             // the samples with the wasted bits shifted out
    std::vector<uint32> u;              // residuals folded to unsigned, valid from index `order`
};

void planSubframe(const int32_t* in, int n, int bps, int kLimit, Subframe& sf) {
    sf.x.assign(in, in + n);
    sf.bps = bps; sf.wasted = 0; sf.order = 0; sf.partOrder = 0; sf.rice2 = false;
    uint32 any = 0;
    bool same = true;
    for (int i = 0; i < n; ++i) { any |= (uint32)in[i]; same = same && in[i] == in[0]; }
    if (same) { sf.kind = Subframe::Constant; sf.bits = 8u + (unsigned)bps; return; }
    int32_t* x = sf.x.data();
    sf.wasted = std::countr_zero(any);
    if (sf.wasted) {
        for (int i = 0; i < n; ++i) x[i] >>= sf.wasted;
        sf.bps = bps -= sf.wasted;
    }
    const uint64 head = 8u + (unsigned)sf.wasted;        // the wasted count is written in unary
    sf.kind = Subframe::Verbatim;
    sf.bits = head + (uint64)n * (unsigned)bps;

    // The fixed predictor whose error is smallest in total; its real cost is worked out below.
    int order = 0;
    if (n > 4) {
        uint64 err[5] = {};
        for (int i = 4; i < n; ++i) {
            const int64 a = x[i], b = x[i - 1], c = x[i - 2], d = x[i - 3], e = x[i - 4];
            const int64 r[5] = {a, a - b, a - 2 * b + c, a - 3 * b + 3 * c - d, a - 4 * b + 6 * c - 4 * d + e};
            for (int j = 0; j < 5; ++j) err[j] += (uint64)(r[j] < 0 ? -r[j] : r[j]);
        }
        for (int j = 1; j < 5; ++j) if (err[j] < err[order]) order = j;
    }
    sf.u.resize((size_t)n);
    uint32* u = sf.u.data();
    uint32 top = 0;
    for (int i = order; i < n; ++i) {       // at most 25-bit input, so a fourth difference stays inside 30 bits
        int64 r = x[i];
        if (order == 1) r -= x[i - 1];
        else if (order == 2) r += -2 * (int64)x[i - 1] + x[i - 2];
        else if (order == 3) r += -3 * (int64)x[i - 1] + 3 * (int64)x[i - 2] - x[i - 3];
        else if (order == 4) r += -4 * (int64)x[i - 1] + 6 * (int64)x[i - 2] - 4 * (int64)x[i - 3] + x[i - 4];
        u[i] = (uint32)((r << 1) ^ (r >> 63));
        top |= u[i];
    }

    // Exact Rice cost, sums[part][k] = the unary bits parameter k needs, for each of the smallest partitions.
    // Coarser partitions are sums of finer ones, which makes trying every partition order cheap.
    // Only parameters near log2 of a partition's mean can be the best one (a parameter k costs within
    // count bits of count * (k + 1) + sum / 2^k, whose minimum is that sharp), and the mean of a merged
    // partition lies between its parts', so one pass over the plain sums gives the range worth counting.
    const int kTop = std::min(kLimit, (int)std::bit_width(top));
    int finest = std::min(kMaxPartOrder, std::countr_zero((unsigned)n));
    while (finest > 0 && (n >> finest) <= order) --finest;
    uint64 sums[1 << kMaxPartOrder][31];
    const int size = n >> finest;
    int kLo = kTop, kHi = 0;
    for (int part = 0; part < (1 << finest); ++part) {
        const int from = std::max(order, part * size), to = (part + 1) * size;
        uint64 sum = 0;
        for (int i = from; i < to; ++i) sum += u[i];
        sums[part][0] = sum;
        int near = 0;
        while (near < kTop && ((uint64)(to - from) << (near + 1)) <= sum) ++near;
        kLo = std::min(kLo, near);
        kHi = std::max(kHi, near);
    }
    kLo = std::max(0, kLo - 1);
    kHi = std::min(kTop, kHi + 2);
    for (int part = 0; part < (1 << finest); ++part) {
        const int from = std::max(order, part * size), to = (part + 1) * size;
        for (int k = std::max(kLo, 1); k <= kHi; ++k) {
            uint64 sum = 0;
            for (int i = from; i < to; ++i) sum += u[i] >> k;
            sums[part][k] = sum;
        }
    }
    const unsigned paramBits = kLimit > 14 ? 5u : 4u;
    uint64 best = ~0ull;
    unsigned char ks[1 << kMaxPartOrder];
    for (int po = finest; po >= 0; --po) {
        const int parts = 1 << po;
        uint64 total = 0;
        for (int part = 0; part < parts; ++part) {
            const uint64 count = (uint64)((n >> po) - (part ? 0 : order));
            uint64 least = ~0ull;
            for (int k = kLo; k <= kHi; ++k) {
                const uint64 b = count * (unsigned)(k + 1) + sums[part][k];
                if (b < least) { least = b; ks[part] = (unsigned char)k; }
            }
            total += paramBits + least;
        }
        if (total <= best) {            // on a tie the coarser split wins
            best = total;
            sf.partOrder = po;
            std::memcpy(sf.k, ks, (size_t)parts);
        }
        for (int part = 0; part < parts / 2; ++part)
            for (int k = kLo; k <= kHi; ++k) sums[part][k] = sums[part * 2][k] + sums[part * 2 + 1][k];
    }
    int widest = 0;
    for (int part = 0; part < (1 << sf.partOrder); ++part) widest = std::max(widest, (int)sf.k[part]);
    sf.rice2 = widest > 14;
    if (paramBits == 5 && !sf.rice2) best -= (uint64)1 << sf.partOrder;      // the parameters fit in 4 bits after all
    const uint64 fixedBits = head + (uint64)order * (unsigned)bps + 6 + best;
    if (fixedBits < sf.bits) { sf.kind = Subframe::Fixed; sf.bits = fixedBits; sf.order = order; }
}

void writeSubframe(BitWriter& bw, const Subframe& sf, int n) {
    const unsigned type = sf.kind == Subframe::Constant ? 0u : sf.kind == Subframe::Verbatim ? 1u : 8u + (unsigned)sf.order;
    bw.put((type << 1) | (sf.wasted ? 1u : 0u), 8);
    if (sf.wasted) bw.put(1, sf.wasted);
    const int32_t* x = sf.x.data();
    if (sf.kind == Subframe::Constant) { bw.putSigned(x[0], sf.bps); return; }
    if (sf.kind == Subframe::Verbatim) { for (int i = 0; i < n; ++i) bw.putSigned(x[i], sf.bps); return; }
    for (int i = 0; i < sf.order; ++i) bw.putSigned(x[i], sf.bps);
    bw.put(sf.rice2 ? 1u : 0u, 2);
    bw.put((uint32)sf.partOrder, 4);
    const uint32* u = sf.u.data();
    int i = sf.order;
    for (int part = 0; part < (1 << sf.partOrder); ++part) {
        const int end = (n >> sf.partOrder) * (part + 1), k = sf.k[part];
        bw.put((uint32)k, sf.rice2 ? 5 : 4);
        for (; i < end; ++i) bw.rice(u[i], k);
    }
}

// Rounds and clamps the way encodeWav does.
inline int32_t toInt(float v, double scale) {
    double x = (double)v * scale;
    if (!(x == x)) return 0;
    x = x > scale - 1.0 ? scale - 1.0 : (x < -scale ? -scale : x);
    return (int32_t)std::llround(x);
}

} // namespace

std::shared_ptr<SampleData> readFlacMemory(const void* data, size_t size, std::string* error) {
    auto fail = [&](const char* m) { if (error) *error = m; return std::shared_ptr<SampleData>(); };
    const unsigned char* d = (const unsigned char*)data;
    if (!d) size = 0;
    size_t pos = 0;
    while (size - pos >= 10 && std::memcmp(d + pos, "ID3", 3) == 0) {      // an ID3v2 tag some programs put in front
        const size_t body = ((size_t)(d[pos + 6] & 127) << 21) | ((size_t)(d[pos + 7] & 127) << 14) | ((size_t)(d[pos + 8] & 127) << 7) | (size_t)(d[pos + 9] & 127);
        const size_t whole = 10 + body + ((d[pos + 5] & 0x10) ? 10u : 0u);
        if (whole > size - pos) return fail("not a FLAC file");
        pos += whole;
    }
    if (size - pos < 4 || std::memcmp(d + pos, "fLaC", 4) != 0) return fail("not a FLAC file");
    pos += 4;

    StreamInfo si;
    bool haveInfo = false;
    for (bool last = false; !last;) {
        if (size - pos < 4) return fail("FLAC metadata is cut short");
        last = (d[pos] & 0x80) != 0;
        const unsigned type = d[pos] & 0x7F;
        const size_t len = ((size_t)d[pos + 1] << 16) | ((size_t)d[pos + 2] << 8) | (size_t)d[pos + 3];
        pos += 4;
        if (len > size - pos) return fail("FLAC metadata is cut short");
        if (!haveInfo) {            // STREAMINFO always comes first; every other block is skipped
            if (type != 0 || len < 34) return fail("FLAC file has no stream info");
            const unsigned char* p = d + pos;
            si.rate = ((unsigned)p[10] << 12) | ((unsigned)p[11] << 4) | ((unsigned)p[12] >> 4);
            si.channels = ((p[12] >> 1) & 7u) + 1u;
            si.bps = (((p[12] & 1u) << 4) | ((unsigned)p[13] >> 4)) + 1u;
            si.total = ((uint64)(p[13] & 15u) << 32) | ((uint64)p[14] << 24) | ((uint64)p[15] << 16) | ((uint64)p[16] << 8) | (uint64)p[17];
            if (si.rate == 0 || si.bps < 4) return fail("bad FLAC stream info");
            haveInfo = true;
        }
        pos += len;
    }

    auto s = std::make_shared<SampleData>();
    s->sampleRate = si.rate;
    s->channels = si.channels > 1 ? 2 : 1;
    // the total is only a hint here: a damaged header must not be able to ask for all the memory there is
    const size_t expect = (size_t)std::min<uint64>(si.total, (uint64)size * 4);
    for (int c = 0; c < s->channels; ++c) s->ch[c].reserve(expect);

    std::vector<int64> buf[3];      // left, right, and one to decode the channels that are dropped into
    uint64 decoded = 0;
    int frames = 0;
    while (pos < size && (!si.total || decoded < si.total)) {
        const unsigned char* f = d + pos;
        const size_t avail = size - pos;
        if (avail >= 2 ? !isSync(f) : f[0] != 0xFF) {
            // Not a frame. A tag stuck on the end of the file is harmless; it is damage if audio is still
            // owed or more frames follow.
            if (si.total || laterFrameExists(f, avail, si, nullptr)) return fail("FLAC frame sync lost");
            break;
        }
        FrameHeader h;
        const Hdr r = parseFrameHeader(f, avail, si, h);
        if (r == Hdr::Bad) return fail("bad FLAC frame header");
        if (r == Hdr::Crc) return fail("FLAC frame header CRC mismatch");
        bool cut = r == Hdr::Short;
        if (!cut) {
            if (h.channels != (int)si.channels) return fail("FLAC channel count changes mid-stream");
            const int n = h.blockSize;
            for (auto& b : buf) if (b.size() < (size_t)n) b.resize((size_t)n);
            BitReader br(f + h.bytes, f + avail);
            bool ok = true;
            for (int c = 0; c < h.channels && ok && !br.eof; ++c) {
                const bool side = (h.assignment == 1 && c == 1) || (h.assignment == 2 && c == 0) || (h.assignment == 3 && c == 1);
                ok = decodeSubframe(br, buf[std::min(c, 2)].data(), n, h.bps + (side ? 1 : 0));     // a difference needs one more bit
            }
            br.align();
            const unsigned crc = br.bits(16);
            if (br.eof) cut = true;
            else if (!ok) return fail("damaged FLAC frame");
            else {
                const size_t len = h.bytes + br.consumed();
                if (crc16(f, len - 2) != crc) return fail("FLAC frame CRC mismatch");
                int64* a = buf[0].data();
                int64* b = buf[1].data();
                if (h.assignment == 1) for (int i = 0; i < n; ++i) b[i] = (int64)((uint64)a[i] - (uint64)b[i]);
                else if (h.assignment == 2) for (int i = 0; i < n; ++i) a[i] = (int64)((uint64)a[i] + (uint64)b[i]);
                else if (h.assignment == 3) {
                    for (int i = 0; i < n; ++i) {       // mid lost its lowest bit when it was halved; it is the same as side's
                        const uint64 side = (uint64)b[i], mid = ((uint64)a[i] << 1) | (side & 1);
                        a[i] = (int64)(mid + side) >> 1;
                        b[i] = (int64)(mid - side) >> 1;
                    }
                }
                const double scale = std::ldexp(1.0, 1 - h.bps);
                const size_t at = s->ch[0].size();
                for (int c = 0; c < s->channels; ++c) {
                    s->ch[c].resize(at + (size_t)n);
                    float* out = s->ch[c].data() + at;
                    const int64* in = buf[c].data();
                    // up to 24 bits a float holds the value exactly, and the 32-bit conversion is the fast one
                    if (h.bps <= 24) for (int i = 0; i < n; ++i) out[i] = (float)(int32_t)in[i] * (float)scale;
                    else for (int i = 0; i < n; ++i) out[i] = (float)((double)in[i] * scale);
                }
                decoded += (uint64)n;
                pos += len;
                ++frames;
            }
        }
        if (cut) {
            // The data ran out inside this frame. If the file was cut here that is the end of it, and what
            // came before is still good; if frames follow, this one was damaged instead.
            if (avail > 2 && laterFrameExists(f + 2, avail - 2, si, r == Hdr::Ok ? &h : nullptr)) return fail("damaged FLAC frame");
            if (!frames) return fail("FLAC file is cut short");
            break;
        }
    }
    if (!frames && si.total) return fail("FLAC file is cut short");
    if (si.total && decoded > si.total)
        for (int c = 0; c < s->channels; ++c) s->ch[c].resize((size_t)si.total);
    return s;
}

std::shared_ptr<SampleData> readFlacFile(const std::string& utf8Path, std::string* error) {
    std::vector<unsigned char> b;
    if (!readFileBytes(utf8Path, b)) { if (error) *error = "cannot open " + utf8Path; return nullptr; }
    auto s = readFlacMemory(b.data(), b.size(), error);
    if (s) s->path = utf8Path;
    return s;
}

std::vector<unsigned char> encodeFlac(const SampleData& s, int bits) {
    bits = bits == 16 ? 16 : 24;
    const int channels = s.channels > 1 ? 2 : 1;
    const size_t frames = s.ch[0].size();
    const unsigned rate = s.sampleRate >= 1.0 ? (unsigned)std::lround(std::min(s.sampleRate, 655350.0)) : 44100u;
    const double scale = bits == 16 ? 32768.0 : 8388608.0;
    const int bytes = bits / 8;
    const int kLimit = bits > 16 ? 30 : 14;      // 16-bit files keep to the 4-bit Rice parameters every decoder knows

    // How a frame header states the rate: a table entry, or the number itself in kHz, Hz or tens of Hz;
    // failing all of those, "see STREAMINFO".
    unsigned rateCode = 0, rateExtra = 0;
    int rateExtraBytes = 0;
    for (unsigned i = 1; i < 12; ++i) if (kRates[i] == rate) rateCode = i;
    if (!rateCode) {
        if (rate % 1000 == 0 && rate / 1000 <= 255) { rateCode = 12; rateExtra = rate / 1000; rateExtraBytes = 1; }
        else if (rate <= 65535) { rateCode = 13; rateExtra = rate; rateExtraBytes = 2; }
        else if (rate % 10 == 0) { rateCode = 14; rateExtra = rate / 10; rateExtraBytes = 2; }
    }

    std::vector<unsigned char> o;
    o.reserve(64 + frames * (size_t)(channels * bytes) * 3 / 4);
    o.insert(o.end(), {'f', 'L', 'a', 'C', 0x80, 0, 0, 34});       // STREAMINFO is the only, and so the last, metadata block
    o.resize(o.size() + 34);                                         // filled in at the end

    Md5 md5;
    std::vector<unsigned char> pcm((size_t)(kBlock * channels * bytes));
    std::vector<int32_t> x[4];          // left, right, mid, side
    for (auto& v : x) v.resize(kBlock);
    Subframe sf[4];
    size_t minFrame = ~(size_t)0, maxFrame = 0;
    uint64 number = 0;
    for (size_t at = 0; at < frames; at += kBlock, ++number) {
        const int n = (int)std::min<size_t>(kBlock, frames - at);
        for (int c = 0; c < channels; ++c) {
            const size_t have = s.ch[c].size() > at ? std::min<size_t>((size_t)n, s.ch[c].size() - at) : 0;
            for (size_t i = 0; i < have; ++i) x[c][i] = toInt(s.ch[c][at + i], scale);
            for (size_t i = have; i < (size_t)n; ++i) x[c][i] = 0;
        }
        unsigned char* m = pcm.data();
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < channels; ++c) {
                const uint32 v = (uint32)x[c][(size_t)i];
                *m++ = (unsigned char)v;
                *m++ = (unsigned char)(v >> 8);
                if (bytes == 3) *m++ = (unsigned char)(v >> 16);
            }
        md5.update(pcm.data(), (size_t)(m - pcm.data()));

        // Stereo is stored whichever way is smallest for this block: as it is, or with one channel
        // replaced by the difference, or as average and difference.
        int first = 0, second = 1;
        unsigned chCode = 0;
        planSubframe(x[0].data(), n, bits, kLimit, sf[0]);
        if (channels == 2) {
            for (int i = 0; i < n; ++i) {
                const int32_t l = x[0][(size_t)i], r = x[1][(size_t)i];
                x[2][(size_t)i] = (l + r) >> 1;
                x[3][(size_t)i] = l - r;
            }
            planSubframe(x[1].data(), n, bits, kLimit, sf[1]);
            planSubframe(x[2].data(), n, bits, kLimit, sf[2]);
            planSubframe(x[3].data(), n, bits + 1, kLimit, sf[3]);
            const uint64 cost[4] = {sf[0].bits + sf[1].bits, sf[0].bits + sf[3].bits, sf[3].bits + sf[1].bits, sf[2].bits + sf[3].bits};
            int pick = 0;
            for (int i = 1; i < 4; ++i) if (cost[i] < cost[pick]) pick = i;
            static const int pairs[4][2] = {{0, 1}, {0, 3}, {3, 1}, {2, 3}};
            first = pairs[pick][0];
            second = pairs[pick][1];
            chCode = pick == 0 ? 1u : 7u + (unsigned)pick;
        }

        const size_t start = o.size();
        unsigned char h[16];
        size_t k = 0;
        h[k++] = 0xFF;
        h[k++] = 0xF8;                  // fixed block size, so frames are numbered rather than samples
        const unsigned sizeCode = n == kBlock ? 12u : (n <= 256 ? 6u : 7u);
        h[k++] = (unsigned char)((sizeCode << 4) | rateCode);
        h[k++] = (unsigned char)((chCode << 4) | ((bits == 16 ? 4u : 6u) << 1));
        if (number < 0x80) h[k++] = (unsigned char)number;
        else {
            int len = 2;
            while (len < 7 && number >= (1ull << (5 * len + 1))) ++len;
            h[k++] = (unsigned char)(((0xFFu << (8 - len)) & 0xFFu) | (unsigned)(number >> (6 * (len - 1))));
            for (int i = len - 2; i >= 0; --i) h[k++] = (unsigned char)(0x80u | ((number >> (6 * i)) & 0x3Fu));
        }
        if (sizeCode == 7) h[k++] = (unsigned char)((n - 1) >> 8);
        if (sizeCode != 12) h[k++] = (unsigned char)((n - 1) & 255);
        if (rateExtraBytes == 2) h[k++] = (unsigned char)(rateExtra >> 8);
        if (rateExtraBytes) h[k++] = (unsigned char)(rateExtra & 255);
        h[k] = (unsigned char)crc8(h, k);
        o.insert(o.end(), h, h + k + 1);

        BitWriter bw(o);
        writeSubframe(bw, sf[first], n);
        if (channels == 2) writeSubframe(bw, sf[second], n);
        bw.align();
        const unsigned crc = crc16(o.data() + start, o.size() - start);
        o.push_back((unsigned char)(crc >> 8));
        o.push_back((unsigned char)(crc & 255));
        const size_t len = o.size() - start;
        minFrame = std::min(minFrame, len);
        maxFrame = std::max(maxFrame, len);
    }

    unsigned char* si = o.data() + 8;
    if (!frames) minFrame = 0;
    si[0] = si[2] = (unsigned char)(kBlock >> 8);
    si[1] = si[3] = (unsigned char)(kBlock & 255);
    for (int i = 0; i < 3; ++i) {
        si[4 + i] = (unsigned char)(minFrame >> (16 - 8 * i));
        si[7 + i] = (unsigned char)(maxFrame >> (16 - 8 * i));
    }
    const uint64 total = (uint64)frames < (1ull << 36) ? (uint64)frames : 0;       // 36 bits; 0 says "unknown"
    const uint64 packed = ((uint64)rate << 44) | ((uint64)(channels - 1) << 41) | ((uint64)(bits - 1) << 36) | total;
    for (int i = 0; i < 8; ++i) si[10 + i] = (unsigned char)(packed >> (56 - 8 * i));
    md5.finish(si + 18);
    return o;
}

bool writeFlacFile(const std::string& utf8Path, const SampleData& s, int bits, std::string* error) {
    const auto bytes = encodeFlac(s, bits);
    if (!writeFileBytes(utf8Path, bytes.data(), bytes.size())) { if (error) *error = "cannot write " + utf8Path; return false; }
    return true;
}

} // namespace pulp
