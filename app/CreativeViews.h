#pragma once
// Controls for the creative effects: Rewind's two curves to draw, and Platter's record to drag.
#include "Session.h"
#include "Widgets.h"
#include "pulp/Creative.h"

// Rewind: the time curve above (how far behind "now" the sound is, in beats) and the volume curve below,
// both across one pattern. Drag a point; double-click to add one; right-click a point for its shape or to remove it.
class RewindCurveView : public juce::Component, public juce::TooltipClient, private UiTicker::Client {
public:
    static constexpr int kHeight = 236;
    RewindCurveView(Session& s, std::shared_ptr<pulp::Plugin> plugin);
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    juce::String getTooltip() override;
    juce::String describe() const;

    juce::Rectangle<float> plot(int which) const;          // 0 time, 1 volume
private:
    using Points = std::vector<pulp::Rewind::Point>;
    void uiTick() override;
    pulp::Rewind* fx() const { return static_cast<pulp::Rewind*>(plugin_.get()); }
    float top(int which) const;                             // the value at the top of a plot
    Points points(int which) const { return which == 0 ? fx()->timeCurve() : fx()->volumeCurve(); }
    void setPoints(int which, const Points& p) { if (which == 0) fx()->setTimeCurve(p); else fx()->setVolumeCurve(p); }
    juce::Point<float> toScreen(int which, float pos, float value) const;
    bool find(juce::Point<float> at, int& which, int& index) const;
    Session& s_;
    std::shared_ptr<pulp::Plugin> plugin_;
    int dragWhich_ = -1, dragIndex_ = -1;
    bool changed_ = false;
    float seenPhase_ = -1.0f;
    juce::String seen_;
};

// Platter: press and drag sideways to hold the record and move it under the needle; let go and it plays on.
class PlatterView : public juce::Component, public juce::TooltipClient, private UiTicker::Client {
public:
    static constexpr int kHeight = 132;
    PlatterView(Session& s, std::shared_ptr<pulp::Plugin> plugin) : s_(s), plugin_(std::move(plugin)) {}
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    juce::String getTooltip() override { return "The record: press and drag left and right to scratch it (the further, the faster). Let go and it carries on. With record on and the song playing, the scratch is recorded."; }

private:
    void uiTick() override { if (isShowing()) repaint(); }
    pulp::Platter* fx() const { return static_cast<pulp::Platter*>(plugin_.get()); }
    Session& s_;
    std::shared_ptr<pulp::Plugin> plugin_;
    bool down_ = false;
    float startX_ = 0.0f;
};
