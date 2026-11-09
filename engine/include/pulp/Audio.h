#pragma once
// Sample data in memory, WAV reading and writing, and the shared sample cache.
#include "pulp/Core.h"
#include <map>

namespace pulp {

// Immutable once published. Mono files have one channel; right() then returns the left data.
uint64 nextAudioUid();
// A number that identifies one piece of audio data, for caches of processed audio. A copy gets its own.
struct AudioUid {
    uint64 v = nextAudioUid();
    AudioUid() = default;
    AudioUid(const AudioUid&) : v(nextAudioUid()) {}
    AudioUid& operator=(const AudioUid&) { v = nextAudioUid(); return *this; }
};
struct SampleData {
    AudioUid uid;
    std::string path;                 // where it came from ("" for generated data)
    double sampleRate = 44100.0;
    int channels = 1;
    std::vector<float> ch[2];
    int64 frames() const { return (int64)ch[0].size(); }
    const float* left() const { return ch[0].data(); }
    const float* right() const { return channels > 1 ? ch[1].data() : ch[0].data(); }
    float peak() const;
};
using SamplePtr = std::shared_ptr<const SampleData>;

enum class WavFormat { Pcm16, Pcm24, Float32 };

// What a WAV file holds, read from its header without loading the audio.
struct WavInfo {
    unsigned format = 0, channels = 0, rate = 0, bits = 0, blockAlign = 0;      // format 1 = PCM, 3 = float
    int64 dataOffset = 0, dataBytes = 0;
    int64 frames() const { return blockAlign ? dataBytes / (int64)blockAlign : 0; }
};
bool readWavInfo(const std::string& path, WavInfo& info, std::string* error = nullptr);
// Converts raw frames from a WAV data chunk to floats. Channels beyond the second are dropped; r may be null.
void decodeWavFrames(const unsigned char* src, const WavInfo& info, size_t frames, float* l, float* r);
void* openFileForRead(const std::string& utf8Path);        // a FILE*, or null

// Returns nullptr and fills error on failure.
std::shared_ptr<SampleData> readWavFile(const std::string& path, std::string* error = nullptr);
// Any audio file the engine can read: WAV, FLAC, AIFF. Decided by the file's first bytes, not its name.
std::shared_ptr<SampleData> readAudioFile(const std::string& utf8Path, std::string* error = nullptr);
std::shared_ptr<SampleData> readWavMemory(const void* data, size_t size, std::string* error = nullptr);
bool writeWavFile(const std::string& path, const SampleData& s, WavFormat fmt, bool dither = false, std::string* error = nullptr);
std::vector<unsigned char> encodeWav(const SampleData& s, WavFormat fmt, bool dither = false);

// Loads audio files once and shares them. Paths beginning with "factory:" resolve against
// the factory library folder. Thread-safe; never call from the audio thread.
class SampleCache {
public:
    static SampleCache& global();
    void setFactoryRoot(const std::string& dir);
    const std::string& factoryRoot() const { return factoryRoot_; }
    std::string resolve(const std::string& path, const std::string& projectDir = {}) const;
    SamplePtr load(const std::string& path, const std::string& projectDir = {}, std::string* error = nullptr);
    void put(const std::string& key, SamplePtr s);
    void clear();
private:
    std::mutex m_;
    std::map<std::string, SamplePtr> map_;
    std::string factoryRoot_;
};

// Writes a WAV file a block at a time (for recording). The file is readable at any moment:
// if the program dies mid-take, everything flushed so far is still there.
class WavStreamWriter {
public:
    ~WavStreamWriter() { close(); }
    bool open(const std::string& path, double sampleRate, int channels, WavFormat fmt = WavFormat::Float32);
    bool write(const float* interleaved, int frames);       // not for the audio thread
    void flush();
    void close();                                            // fixes up the sizes in the header
    int64 frames() const { return frames_; }
    bool isOpen() const { return file_ != nullptr; }
    const std::string& path() const { return path_; }
private:
    void* file_ = nullptr;
    std::string path_;
    int channels_ = 1;
    WavFormat fmt_ = WavFormat::Float32;
    int64 frames_ = 0;
};

// File helpers that accept UTF-8 paths.
bool readFileBytes(const std::string& path, std::vector<unsigned char>& out);
bool writeFileBytes(const std::string& path, const void* data, size_t size);
bool readTextFile(const std::string& path, std::string& out);
bool writeTextFile(const std::string& path, const std::string& text);

} // namespace pulp

namespace pulp {
// SHA-256 as lowercase hex, for the factory library manifest.
std::string sha256Hex(const void* data, size_t size);
}
