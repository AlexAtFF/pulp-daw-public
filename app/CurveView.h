#pragma once
// The drawing editor for Bend, the waveshaper: its transfer curve with the points that define it.
// Drag a point; double-click to add one; right-click a point to remove it.
#include "Session.h"
#include "Widgets.h"
#include "pulp/Tone.h"

class BendCurveView : public juce::Component, public juce::TooltipClient, private UiTicker::Client {
public:
    static constexpr int kHeight = 216;
    BendCurveView(Session& s, std::shared_ptr<pulp::Plugin> plugin);
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    juce::String getTooltip() override {
        return "The curve: what goes in runs left to right, what comes out runs bottom to top; a straight diagonal changes nothing. "
               "Drag a point, double-click to add one, right-click a point to remove it.";
    }
    juce::String describe() const;

private:
    void uiTick() override;
    pulp::Bend* bend() const { return static_cast<pulp::Bend*>(plugin_.get()); }
    juce::Rectangle<float> plot() const;
    juce::Point<float> toScreen(float x, float y) const;
    juce::Point<float> fromScreen(juce::Point<float> p) const;
    int pointAt(juce::Point<float> p) const;
    Session& s_;
    std::shared_ptr<pulp::Plugin> plugin_;
    int dragging_ = -1;
    bool changed_ = false;
    juce::String seen_;
};
