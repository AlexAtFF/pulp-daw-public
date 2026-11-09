#pragma once
// Ogg Vorbis reading and writing: a complete Vorbis I decoder and an encoder, with no outside code.
#include "pulp/Audio.h"

namespace pulp {

// Returns nullptr and fills error for input that is not Ogg Vorbis (or whose headers are unusable).
// The first Vorbis stream in the file is decoded; other streams multiplexed with it, and anything
// chained after it, are skipped. Every page's checksum is verified. A file that is damaged or cut
// short part way through the audio gives the audio that came before the damage.
// Streams of more than two channels give their front left and front right channels (no downmix).
std::shared_ptr<SampleData> readOggMemory(const void* data, size_t size, std::string* error = nullptr);
std::shared_ptr<SampleData> readOggFile(const std::string& utf8Path, std::string* error = nullptr);

// quality runs from 0 (smallest) to 1 (best and biggest); stereo music at 44.1 kHz comes to roughly
// 110, 200 and 480 kbit/s at 0.3, 0.6 and 1.0. Lossy: the decoded audio has exactly the length of the
// original and is meant to sound like it, but the samples are not the same. Mono and stereo only.
std::vector<unsigned char> encodeOgg(const SampleData& s, float quality = 0.6f);
bool writeOggFile(const std::string& utf8Path, const SampleData& s, float quality = 0.6f, std::string* error = nullptr);

} // namespace pulp
