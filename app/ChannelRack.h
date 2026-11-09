#pragma once
// The Channel Rack: one row per channel with mute, pan, volume, the channel button and
// the step sequencer, plus the graph editor for per-step properties.
#include "ParamPanel.h"
#include "Session.h"

class ChannelRack : public juce::Component,
                    public juce::FileDragAndDropTarget,
                    public juce::DragAndDropTarget,
                    private Session::Listener,
                    private UiTicker::Client {
public:
    explicit ChannelRack(Session& s);
    ~ChannelRack() override;

    std::function<void(int)> onOpenChannel;       // show the channel settings window
    std::function<void(int)> onOpenPianoRoll;
    bool handleKey(const juce::KeyPress& k);
    void dropFiles(const juce::StringArray& files, juce::Point<int> where);
    void channelMenu(int index, juce::Component* target);
    void addChannelMenu(juce::Component* target);
    void soundFontMenu(const juce::File& sf2);

    // step operations on a channel in the current pattern
    void fillSteps(int channel, int every);
    void rotateSteps(int channel, int delta);
    void clearSteps(int channel);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    bool keyPressed(const juce::KeyPress& k) override { return handleKey(k); }
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    bool isInterestedInDragSource(const SourceDetails& d) override;
    void itemDropped(const SourceDetails& d) override;

    static constexpr int kRowH = 30, kHeaderH = 34, kLeftW = 246, kStepW = 18, kBeatGap = 5, kGraphH = 96;
    void routeToFreeTracks();       // Ctrl+L: give the selected channels their own mixer tracks
    static int stepX(int step) { return step * kStepW + (step / 4) * kBeatGap; }
    int stepAtX(int x) const;       // x relative to the start of the step area, with scrolling applied

private:
    class Row;
    class Graph;
    class Holder;
    void projectChanged() override;
    void uiTick() override;
    void rebuild();
    void repaintGraph();
    bool rowVisible(const pulp::Channel& c) const;
    void groupMenu();

    Session& s_;
    pulp::Param swing_;
    ParamKnob swingKnob_;
    NumberBox stepsBox_;
    IconButton graphBtn_{IconButton::Text}, groupBtn_{IconButton::Text}, addBtn_{IconButton::Text};
    juce::Viewport view_;
    std::unique_ptr<Holder> holder_;
    juce::OwnedArray<Row> rows_;
    juce::Component* rowFor(int index) const;
    std::unique_ptr<Graph> graph_;
    juce::ScrollBar hscroll_{false};
    bool showGraph_ = false;
    int stepScroll_ = 0;            // pixels
    int playStep_ = -1;
    juce::String filter_;           // "" = all channels
    friend class Row;
    friend class Graph;
};
