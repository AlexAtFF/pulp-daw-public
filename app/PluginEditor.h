#pragma once
// The editor panel shown for a built-in effect: preset selector, an optional display
// (EQ curve over a live spectrum, gain-reduction history), then its parameters.
#include "ParamPanel.h"
#include "Session.h"
#include "AutomationViews.h"
#include "SpliceView.h"
#include "AnalyserView.h"

class PluginEditor : public juce::Component, private UiTicker::Client {
public:
    PluginEditor(Session& s, std::shared_ptr<pulp::Plugin> plugin);
    pulp::Plugin* plugin() const { return plugin_.get(); }
    int track = -1, slot = 0;       // where the effect sits in the mixer, so the editor can be found again after undo
    std::string typeId;
    SpliceView* spliceView() const { return splice_.get(); }
    AnalyserView* analyserView() const { return analyser_.get(); }
    juce::Component* customView() const { return custom_.get(); }       // the audio editor's panel, when this is one
    int preferredHeight(int width);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;

private:
    void uiTick() override;
    void presetMenu();
    void choosePreset(int index);
    juce::Rectangle<int> displayArea() const;
    float xOfHz(double hz) const;
    double hzOfX(float x) const;
    float yOfDb(double db) const;

    Session& s_;
    std::shared_ptr<pulp::Plugin> plugin_;
    IconButton presetBtn_{IconButton::Text};
    IconButton fileBtn_{IconButton::Text};       // effects that take a file (an impulse response)
    std::vector<pulp::Preset> presets_;
    int presetIndex_ = -1;
    juce::Viewport view_;
    ParamPanel panel_;
    enum Display { NoDisplay, EqCurve, GainReduction, Editor, Controller, Analyser, Custom };
    int displayHeight() const {
        return display_ == Custom ? customHeight_ : display_ == Analyser ? AnalyserView::kHeight : display_ == EqCurve ? 184 : (display_ == GainReduction ? 90 : (display_ == Editor ? SpliceView::kHeight : (display_ == Controller ? ControllerView::heightFor(*plugin_) : 0)));
    }
    std::unique_ptr<SpliceView> splice_;
    std::unique_ptr<ControllerView> controller_;
    std::unique_ptr<AnalyserView> analyser_;
    std::unique_ptr<juce::Component> custom_;       // a plugin's own control surface (a curve to draw, a pad to drag)
    int customHeight_ = 0;
    int display_ = NoDisplay;
    int dragBand_ = -1;
    std::vector<float> history_;        // gain reduction over time
    std::vector<float> spectrum_;       // smoothed dB per pixel column
};
