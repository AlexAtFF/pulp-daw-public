#include "pulp/Zip.h"
#include <cstring>

namespace pulp {

unsigned crc32(const void* data, size_t size) {
    static unsigned table[256];
    static bool ready = false;
    if (!ready) {
        for (unsigned i = 0; i < 256; ++i) {
            unsigned c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    unsigned crc = 0xFFFFFFFFu;
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

namespace {
void w16(std::vector<unsigned char>& o, unsigned v) { o.push_back((unsigned char)v); o.push_back((unsigned char)(v >> 8)); }
void w32(std::vector<unsigned char>& o, unsigned v) { w16(o, v & 0xFFFF); w16(o, v >> 16); }
unsigned r16(const unsigned char* p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
unsigned r32(const unsigned char* p) { return r16(p) | (r16(p + 2) << 16); }
}

std::vector<unsigned char> zipStore(const std::vector<ZipEntry>& entries) {
    std::vector<unsigned char> o, central;
    for (auto& e : entries) {
        const unsigned offset = (unsigned)o.size();
        const unsigned crc = crc32(e.data.data(), e.data.size());
        const unsigned size = (unsigned)e.data.size();
        // local file header
        w32(o, 0x04034b50); w16(o, 20); w16(o, 0x0800); w16(o, 0); w16(o, 0); w16(o, 0x21);
        w32(o, crc); w32(o, size); w32(o, size); w16(o, (unsigned)e.name.size()); w16(o, 0);
        o.insert(o.end(), e.name.begin(), e.name.end());
        o.insert(o.end(), e.data.begin(), e.data.end());
        // central directory record
        w32(central, 0x02014b50); w16(central, 20); w16(central, 20); w16(central, 0x0800); w16(central, 0); w16(central, 0); w16(central, 0x21);
        w32(central, crc); w32(central, size); w32(central, size); w16(central, (unsigned)e.name.size()); w16(central, 0); w16(central, 0);
        w16(central, 0); w16(central, 0); w32(central, 0); w32(central, offset);
        central.insert(central.end(), e.name.begin(), e.name.end());
    }
    const unsigned cdOffset = (unsigned)o.size();
    o.insert(o.end(), central.begin(), central.end());
    w32(o, 0x06054b50); w16(o, 0); w16(o, 0); w16(o, (unsigned)entries.size()); w16(o, (unsigned)entries.size());
    w32(o, (unsigned)central.size()); w32(o, cdOffset); w16(o, 0);
    return o;
}

bool zipRead(const std::vector<unsigned char>& b, std::vector<ZipEntry>& out, std::string* error) {
    auto fail = [&](const char* m) { if (error) *error = m; return false; };
    out.clear();
    if (b.size() < 22) return fail("not a ZIP archive");
    // find the end-of-central-directory record
    size_t eocd = std::string::npos;
    for (size_t i = b.size() - 22;; --i) {
        if (r32(&b[i]) == 0x06054b50) { eocd = i; break; }
        if (i == 0 || b.size() - i > 70000) break;
    }
    if (eocd == std::string::npos) return fail("not a ZIP archive");
    const unsigned count = r16(&b[eocd + 10]);
    size_t pos = r32(&b[eocd + 16]);
    for (unsigned n = 0; n < count; ++n) {
        if (pos + 46 > b.size() || r32(&b[pos]) != 0x02014b50) return fail("damaged ZIP directory");
        const unsigned method = r16(&b[pos + 10]), crc = r32(&b[pos + 16]), csize = r32(&b[pos + 20]), usize = r32(&b[pos + 24]);
        const unsigned nameLen = r16(&b[pos + 28]), extraLen = r16(&b[pos + 30]), commentLen = r16(&b[pos + 32]);
        const size_t local = r32(&b[pos + 42]);
        if (pos + 46 + nameLen > b.size()) return fail("damaged ZIP directory");
        ZipEntry e;
        e.name.assign((const char*)&b[pos + 46], nameLen);
        pos += 46 + nameLen + extraLen + commentLen;
        if (!e.name.empty() && e.name.back() == '/') continue;      // a folder entry
        if (method != 0 || csize != usize) return fail("this archive is compressed; PULP packs are stored uncompressed");
        if (local + 30 > b.size() || r32(&b[local]) != 0x04034b50) return fail("damaged ZIP entry");
        const size_t dataAt = local + 30 + r16(&b[local + 26]) + r16(&b[local + 28]);
        if (dataAt + usize > b.size()) return fail("damaged ZIP entry");
        e.data.assign(b.begin() + (long long)dataAt, b.begin() + (long long)(dataAt + usize));
        if (crc32(e.data.data(), e.data.size()) != crc) return fail("ZIP entry failed its checksum");
        // never accept a path that climbs out of the folder it is unpacked into
        if (e.name.find("..") != std::string::npos || e.name.find(':') != std::string::npos || e.name[0] == '/' || e.name[0] == '\\') return fail("unsafe path in archive");
        out.push_back(std::move(e));
    }
    return true;
}

} // namespace pulp
