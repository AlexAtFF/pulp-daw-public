#pragma once
// Controls for stepped and on/off parameters, and a panel that lays out any set of
// parameters as labelled knobs, choices and toggles under headings.
#include "Widgets.h"

// A stepped parameter shown as text; click for a menu of choices, wheel to step through.
class ParamChoice : public juce::Component, public juce::SettableTooltipClient, private UiTicker::Client {
public:
    explicit ParamChoice(pulp::Param* p = nullptr) : param_(p) {}
    void setParam(pulp::Param* p) { param_ = p; repaint(); }
    std::function<void()> onChange;
    juce::String getTooltip() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
private:
    void uiTick() override { if (param_ && param_->get() != shown_) repaint(); }
    pulp::Param* param_;
    float shown_ = -1e9f;
};

// An on/off parameter drawn as a lamp with an optional caption.
class ParamToggle : public juce::Component, public juce::SettableTooltipClient, private UiTicker::Client {
public:
    explicit ParamToggle(pulp::Param* p = nullptr) : param_(p) {}
    void setParam(pulp::Param* p) { param_ = p; repaint(); }
    std::function<void()> onChange;
    std::function<void()> onRightClick;
    juce::String caption;
    juce::Colour colour = theme::good;
    bool inverted = false;          // lamp lit when the value is off (used for mute)
    juce::String getTooltip() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
private:
    void uiTick() override { if (param_ && param_->get() != shown_) repaint(); }
    pulp::Param* param_;
    float shown_ = -1e9f;
};

class ParamPanel : public juce::Component {
public:
    struct Section { juce::String title; std::vector<pulp::Param*> params; };
    void setSections(std::vector<Section> sections);
    int heightFor(int width);
    std::function<void(pulp::Param*)> onChange;         // any edit
    std::function<void(pulp::Param*)> onGestureEnd;     // edit finished
    void resized() override;
    void paint(juce::Graphics& g) override;
private:
    struct Cell { pulp::Param* p; std::unique_ptr<juce::Component> c; int span; juce::Rectangle<int> bounds; juce::String label; };
    struct Row { juce::String title; int y; };
    int layout(int width, bool apply);
    std::vector<Section> sections_;
    std::vector<Cell> cells_;
    std::vector<size_t> sectionStart_;
    std::vector<Row> rows_;
    static constexpr int kCellW = 62, kCellH = 58, kHead = 20;
};
