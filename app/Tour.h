#pragma once
// The first-run tour: a few steps laid over the program, each pointing at one part of it and saying
// what it is for. Shown once, the first time the program starts; File > Take the tour shows it again.
#include "Theme.h"
#include "Widgets.h"

class TourOverlay : public juce::Component {
public:
    struct Step {
        juce::String title, text;
        std::function<juce::Rectangle<int>()> target;   // the part of the window it is about (empty: none)
        std::function<void()> before;                   // makes that part visible
    };
    explicit TourOverlay(std::vector<Step> steps);
    std::function<void(bool finished)> onClose;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override {}        // clicks do not reach the program underneath
    bool keyPressed(const juce::KeyPress& k) override;
    void go(int step);
    int step() const { return step_; }
    int numSteps() const { return (int)steps_.size(); }
    juce::String describe() const;

private:
    juce::Rectangle<int> bubble() const;
    std::vector<Step> steps_;
    int step_ = 0;
    IconButton next_{IconButton::Text}, back_{IconButton::Text}, skip_{IconButton::Text};
};
