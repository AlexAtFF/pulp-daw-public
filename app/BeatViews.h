#pragma once
// The panels of the two beat-making instruments, shown in the channel settings window:
// the sixteen pads of Sixteen, and Chop's loop with its slices.
#include "Session.h"
#include "Widgets.h"
#include "pulp/Pads.h"

class PadGrid : public juce::Component, public juce::TooltipClient, public juce::FileDragAndDropTarget,
                public juce::DragAndDropTarget, private UiTicker::Client {
public:
    static constexpr int kHeight = 4 * 46 + 34;
    explicit PadGrid(Session& s);
    int selected = 0;
    std::function<void(int pad)> onSelect;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    juce::String getTooltip() override;
    bool isInterestedInFileDrag(const juce::StringArray&) override { return true; }
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("file:"); }
    void itemDropped(const SourceDetails& d) override { filesDropped({d.description.toString().substring(5)}, d.localPosition.x, d.localPosition.y); }
    juce::Rectangle<int> cell(int pad) const;      // pad 1 is bottom left, as on a drum machine
    int padAt(juce::Point<int> p) const;

private:
    void uiTick() override;
    pulp::PadKit* kit() const;
    void padMenu(int pad);
    Session& s_;
    IconButton kitBtn_{IconButton::Text}, clearBtn_{IconButton::Text};
    int down_ = -1;
    float lit_[pulp::PadKit::kPads] = {};
};

class SliceView : public juce::Component, public juce::TooltipClient, public juce::FileDragAndDropTarget,
                  public juce::DragAndDropTarget {
public:
    static constexpr int kHeight = 96 + 34;
    explicit SliceView(Session& s);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    juce::String getTooltip() override;
    bool isInterestedInFileDrag(const juce::StringArray&) override { return true; }
    void filesDropped(const juce::StringArray& files, int, int) override;
    bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("file:"); }
    void itemDropped(const SourceDetails& d) override { filesDropped({d.description.toString().substring(5)}, 0, 0); }
    void refresh();                                 // the loop or its slices changed
    juce::String describe() const;

private:
    pulp::Chop* chop() const;
    juce::Rectangle<int> waveArea() const { return getLocalBounds().withTrimmedBottom(34).reduced(6, 2); }
    int sliceAt(int x) const;
    Session& s_;
    IconButton loadBtn_{IconButton::Text}, sliceBtn_{IconButton::Text}, evenBtn_{IconButton::Text}, writeBtn_{IconButton::Text};
    std::vector<float> lo_, hi_;
    pulp::SamplePtr drawn_;
    int drawnWidth_ = 0;
    int down_ = -1;
};
