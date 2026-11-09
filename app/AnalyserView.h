#pragma once
// What Gauge shows: spectrum, spectrogram, oscilloscope, vectorscope, loudness, tuner.
#include "Session.h"
#include "Widgets.h"
#include "pulp/Analysis.h"

class AnalyserView : public juce::Component, public juce::TooltipClient, private UiTicker::Client {
public:
    static constexpr int kHeight = 236;
    AnalyserView(Session& s, std::shared_ptr<pulp::Plugin> plugin);
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    juce::String getTooltip() override;
    juce::String describe() const;          // the numbers on show, for scripts

private:
    void uiTick() override;
    pulp::Gauge* gauge() const { return static_cast<pulp::Gauge*>(plugin_.get()); }
    void analyse();
    Session& s_;
    std::shared_ptr<pulp::Plugin> plugin_;
    std::vector<float> spectrum_, held_;    // dB per pixel column
    juce::Image gram_;                      // the spectrogram, newest column on the right
    std::vector<float> left_, right_;
    double pitchKey_ = 0.0, pitchClarity_ = 0.0, pitchShown_ = 0.0;
    int tick_ = 0;
};
