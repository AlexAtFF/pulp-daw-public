#pragma once
// FLAC reading and writing: a complete decoder and a fixed-predictor encoder, with no outside code.
#include "pulp/Audio.h"

namespace pulp {

// Returns nullptr and fills error on failure. Channels beyond the second are dropped (like the WAV reader).
// Every frame's checksums are verified; a file that simply ends early gives the frames that were whole.
std::shared_ptr<SampleData> readFlacMemory(const void* data, size_t size, std::string* error = nullptr);
std::shared_ptr<SampleData> readFlacFile(const std::string& utf8Path, std::string* error = nullptr);

// bits is 16 or 24. Lossless: decoding gives back exactly the integers the floats were rounded to
// (the same rounding and clamping as encodeWav without dither).
std::vector<unsigned char> encodeFlac(const SampleData& s, int bits = 24);
bool writeFlacFile(const std::string& utf8Path, const SampleData& s, int bits = 24, std::string* error = nullptr);

} // namespace pulp
