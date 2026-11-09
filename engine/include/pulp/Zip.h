#pragma once
// A minimal ZIP archive (stored, no compression) for packing a project and its audio
// into one file. Files written here open in any unzip tool.
#include <string>
#include <vector>

namespace pulp {

struct ZipEntry {
    std::string name;                   // path inside the archive, with forward slashes
    std::vector<unsigned char> data;
};

unsigned crc32(const void* data, size_t size);
std::vector<unsigned char> zipStore(const std::vector<ZipEntry>& entries);
// Reads an archive made by zipStore (or any archive whose entries are stored uncompressed).
bool zipRead(const std::vector<unsigned char>& bytes, std::vector<ZipEntry>& out, std::string* error = nullptr);

} // namespace pulp
