#pragma once
// Long audio files played straight from disk. An AudioStream keeps only the pages near where
// it is being played in memory; a loader thread reads ahead and lets go of what is behind.
// The audio thread never touches the file: it copies from pages that are already loaded, and
// a page that is not there yet plays as silence (and is counted). An offline render asks for
// "blocking" reads instead, so an export always gets every sample.
#include "pulp/Audio.h"

namespace pulp {

class AudioStream {
public:
    static std::shared_ptr<AudioStream> open(const std::string& path, std::string* error = nullptr);
    ~AudioStream();
    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;

    AudioUid uid;
    std::string path;                   // as given to open()
    double sampleRate = 44100.0;
    int channels = 1;
    int64 frames = 0;
    float peak = 0.0f;                  // found while building the overview
    // One min / max pair per kOverviewBin frames, for drawing the waveform without the file.
    static constexpr int kOverviewBin = 256;
    std::vector<float> ovMin, ovMax;

    // Copies frames [start, start + n) into l and r; frames outside the file are zero. Audio or render thread.
    // Returns false when part of the range was not in memory (that part is zero). With blocking set, missing
    // pages are read on the spot (offline renders only).
    bool read(int64 start, int n, float* l, float* r, bool blocking);
    void prefetch(int64 frame);                         // control thread: have the pages around this frame loaded soon
    void setHeads(std::vector<int64> frames);           // control thread: positions whose pages always stay loaded
    void service();                                     // loader: load wanted pages, drop stale ones
    int pagesLoaded() const;
    int64 misses() const { return misses_.load(std::memory_order_relaxed); }
    std::shared_ptr<SampleData> loadAll(std::string* error = nullptr) const;   // the whole file in memory (for stretching and editing)

    // Files with more frames than this are streamed by Project::addAudio; shorter ones are loaded whole.
    static int64 streamAbove();
    static void setStreamAbove(int64 frames);
    static int pageFrames();
    static void setPageFrames(int frames);              // applies to streams opened afterwards

private:
    AudioStream() = default;
    bool loadPage(int page);
    WavInfo info_;
    void* file_ = nullptr;
    mutable std::mutex fileMutex_;
    int pageFrames_ = 65536, numPages_ = 0;
    std::unique_ptr<std::atomic<float*>[]> pages_;      // planar: left then right
    std::unique_ptr<std::atomic<uint32>[]> stamp_;      // service epoch at which the page was last wanted
    std::atomic<int> pins_{0};
    std::atomic<uint32> epoch_{8};
    std::atomic<int64> misses_{0};
    std::mutex serviceMutex_;
    std::vector<int> heads_;                            // pages that are never dropped
};

} // namespace pulp
