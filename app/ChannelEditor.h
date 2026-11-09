#pragma once
// The channel settings window: the instrument's own panel on top, then the settings every
// channel shares (levels, polyphony, cut groups, time, arpeggiator, echo, tracking), and a
// keyboard for trying the sound.
#include "ParamPanel.h"
#include "Session.h"
#include <map>

// Draws an audio file as peaks. Reused by the sampler panel and the browser preview.
class WaveformView : public juce::Component, public juce::SettableTooltipClient {
public:
    void setSample(pulp::SamplePtr s);
    pulp::SamplePtr sample() const { return sample_; }
    float start = 0.0f, end = 1.0f;         // shaded trim region, 0..1
    bool showTrim = false;
    juce::String emptyText = "Drop an audio file here";
    juce::Colour colour = theme::accent2;
    std::function<void(float start, float end, bool done)> onTrim;
    std::function<void()> onClick;
    void paint(juce::Graphics& g) override;
    void resized() override { peaksWidth_ = 0; }
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
private:
    void buildPeaks(int width);
    pulp::SamplePtr sample_;
    std::vector<float> lo_, hi_;
    int peaksWidth_ = 0;
    int dragging_ = 0;      // 1 start marker, 2 end marker
};

// A playable keyboard strip. Keys are MIDI numbers; C5 is 60.
class KeyboardStrip : public juce::Component, public juce::SettableTooltipClient {
public:
    std::function<void(int key, float velocity)> onNoteOn;
    std::function<void(int key)> onNoteOff;
    int lowKey = 36, numKeys = 61;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    static bool isBlack(int key) { const int n = key % 12; return n == 1 || n == 3 || n == 6 || n == 8 || n == 10; }
    static juce::String keyName(int key);
private:
    int keyAt(juce::Point<int> p) const;
    void press(const juce::MouseEvent& e);
    int down_ = -1;
};

class PadGrid;
class SliceView;

class ChannelEditor : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      public juce::DragAndDropTarget,
                      private Session::Listener {
public:
    explicit ChannelEditor(Session& s);
    ~ChannelEditor() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("file:"); }
    void itemDropped(const SourceDetails& d) override;
    std::function<void(const juce::String&)> onTitle;

private:
    class Body;
    void projectChanged() override;
    void rebuild();
    Session& s_;
    int shownId_ = -1;
    pulp::Plugin* shownPlugin_ = nullptr;
    juce::Viewport view_;
    std::unique_ptr<Body> body_;
    WaveformView wave_;
    ParamPanel pluginPanel_, stripPanel_;
    KeyboardStrip keys_;
    juce::String sampleInfo_;
    bool hasWave_ = false;
    IconButton presetBtn_{IconButton::Text};
    // multisample instruments: the zone map and the two ways of making one
    class ZoneMap;
    std::unique_ptr<ZoneMap> zoneMap_;
    IconButton buildBtn_{IconButton::Text}, loadInstBtn_{IconButton::Text}, libraryBtn_{IconButton::Text};
    void libraryMenu();
    bool hasZones_ = false;
    IconButton pluginWinBtn_{IconButton::Text};     // hosted instruments: open the plugin's own window
    IconButton wavetableBtn_{IconButton::Text};     // Ripple: load a wavetable of your own
    IconButton matrixBtn_{IconButton::Text};        // Sextet: turn the chosen algorithm into matrix settings that can be edited
    IconButton analyseBtn_{IconButton::Text}, clearAnalysisBtn_{IconButton::Text};     // Sheaf: make a sound from a recording of one note; forget it
    std::unique_ptr<PadGrid> padGrid_;              // Sixteen's pads
    std::unique_ptr<SliceView> sliceView_;          // Chop's loop and slices
    void showPluginSections(pulp::Plugin* plug);
    const void* shownAnalysis_ = nullptr;           // Sheaf: the analysed sound the text describes
    juce::String shownPad_;                         // which pad's knobs are showing
    std::vector<pulp::Preset> presets_;
    std::map<pulp::Plugin*, int> presetIndex_;      // last preset chosen for each instrument this session
    void presetMenu();
    void choosePreset(int index);
};
