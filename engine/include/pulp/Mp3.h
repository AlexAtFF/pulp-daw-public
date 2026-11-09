#pragma once
// MP3 (MPEG audio Layer III) reading and writing, with no outside code.
// Reads MPEG-1, MPEG-2 and MPEG-2.5 Layer III; writes MPEG-1 Layer III at a constant bitrate.
#include "pulp/Audio.h"

namespace pulp {

// Returns nullptr and fills error when the data is not MP3 at all (or there is no memory to decode it).
// A file that is cut short or damaged gives the audio that could be decoded: the whole frames before a
// cut, and after a damaged stretch the decoder picks the stream up again where two frame headers like
// the first ones follow one another. Damage inside a frame cannot be seen in MP3 and decodes to something.
// The encoder delay and padding are removed when an Info / Xing frame with a LAME extension gives them
// (the result then has exactly the length of what was encoded); otherwise only the decoder's own 529
// samples are removed from the start. ID3v2 tags in front and an ID3v1 tag behind are skipped.
// Samples are not clipped: a loud MP3 can decode to values beyond +-1.
std::shared_ptr<SampleData> readMp3Memory(const void* data, size_t size, std::string* error = nullptr);
std::shared_ptr<SampleData> readMp3File(const std::string& utf8Path, std::string* error = nullptr);       // sets path

// Constant bitrate, 96 to 320 kbps (other values go to the nearest MPEG-1 bitrate in that range).
// Rates other than 32, 44.1 and 48 kHz are resampled to 44.1 or 48 kHz first. The file starts with an
// Info frame that carries the encoder delay and padding, so decoders return exactly the original length.
std::vector<unsigned char> encodeMp3(const SampleData& s, int kbps = 192);
bool writeMp3File(const std::string& utf8Path, const SampleData& s, int kbps = 192, std::string* error = nullptr);

// True for data that starts with an ID3v2 tag (unless a FLAC stream follows the tag) or holds, in its
// first 16 KB, Layer III frame headers that follow one another (three, or two if the data ends there).
bool looksLikeMp3(const void* data, size_t size);

// What the encoder can be told beyond the bitrate; encodeMp3(s, kbps) uses the defaults.
struct Mp3EncodeOptions {
    int kbps = 192;
    enum Stereo { Auto, LeftRight, MidSide, Intensity, Dual };
    Stereo stereo = Auto;           // Auto: joint stereo, mid/side in each frame where that is cheaper. Intensity is for tests of decoders.
    bool shortBlocks = true;        // switch to short blocks at attacks
    bool mixedBlocks = false;       // with short blocks: keep long windows in the two lowest subbands. For tests of decoders.
    bool infoFrame = true;          // false: no Info frame (decoders then cannot remove the padding)
    bool crc = false;               // protect each frame's header and side information with a CRC-16
};
std::vector<unsigned char> encodeMp3(const SampleData& s, const Mp3EncodeOptions& options);

} // namespace pulp
