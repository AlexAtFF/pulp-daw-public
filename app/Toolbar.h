#pragma once
// The strip across the top: window buttons, hint bar, transport, tempo, pattern selector,
// master controls and meters.
#include "ParamPanel.h"
#include "Session.h"
#include "Widgets.h"

class Toolbar : public juce::Component, public juce::TooltipClient, private Session::Listener, private UiTicker::Client {
public:
    explicit Toolbar(Session& s);
    ~Toolbar() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void setHint(const juce::String& h);
    juce::String getTooltip() override;
    static constexpr int kHeight = 66;

    // Window buttons. The workspace registers each window it has.
    void addWindowButton(const juce::String& label, const juce::String& hint, std::function<void()> toggle, std::function<bool()> isOpen);
    void addMenuButton(const juce::String& label, const juce::String& hint, std::function<void(juce::Component*)> open);
    void patternMenu();
    void recordMenu();

private:
    void projectChanged() override;
    void uiTick() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;

    class Scope;
    class Meter;
    struct WinButton { std::unique_ptr<IconButton> button; std::function<bool()> isOpen; };
    Session& session_;
    IconButton patSong_{IconButton::Text}, play_{IconButton::Play}, stop_{IconButton::Stop};
    IconButton rec_{IconButton::Record}, metro_{IconButton::Metronome}, keys_{IconButton::Text}, countIn_{IconButton::Text};
    ParamChoice snap_;
    NumberBox tempo_;
    ParamKnob masterVol_, masterPitch_;
    std::unique_ptr<Scope> scope_;
    std::unique_ptr<Meter> meter_;
    std::vector<WinButton> windows_;
    std::vector<std::unique_ptr<IconButton>> menus_;
    juce::String hint_, time_, stats_;
    juce::Rectangle<int> hintArea_, timeArea_, statsArea_, brandArea_, patternArea_;
    bool wasPlaying_ = false, wasWaiting_ = false;
};
