#pragma once
// Small controls used everywhere: knobs, number boxes, icon buttons, meters.
#include "Theme.h"
#include "pulp/Plugin.h"
#include <functional>

// One 60 Hz timer shared by every control that mirrors a live value. Views that scroll or follow the
// play position use every tick; meters and displays whose motion is counted in ticks use every second
// one (onSlowTick), 30 times a second.
class UiTicker : private juce::Timer {
public:
    struct Client {
        virtual ~Client() { UiTicker::get().clients_.removeFirstMatchingValue(this); }
        virtual void uiTick() = 0;
    protected:
        Client() { UiTicker::get().clients_.add(this); }
    };
    static UiTicker& get();
    static bool onSlowTick() { return (get().count_ & 1) == 0; }
private:
    UiTicker() { startTimerHz(60); }
    unsigned count_ = 0;
    void timerCallback() override;
    juce::Array<Client*> clients_;
};

void askForText(const juce::String& title, const juce::String& message, const juce::String& initial,
                juce::Component* parent, std::function<void(juce::String)> onOk);

// What every control (knob, fader, switch, selector) shares: the right-click menu, and telling
// the program when it is grabbed, moved and let go, so movements can be recorded and automation
// leaves a control alone while it is held. The program fills in the hooks.
struct ParamUi {
    static std::function<void(pulp::Param*, juce::PopupMenu&, int firstId)> addMenuItems;   // automation and controller items
    static std::function<void(pulp::Param*, int item)> menuChosen;                           // item counts from 0
    static std::function<void(pulp::Param*, int phase)> touched;                             // 0 grabbed, 1 moved, 2 let go
    static std::function<bool(pulp::Param*)> valid;                                          // still part of the project?
    static void touch(pulp::Param* p, int phase) { if (touched && p) touched(p, phase); }
    static void nudge(pulp::Param* p, float value);                                          // one complete change: grab, set, let go
    // The standard menu: reset, type in value, copy, paste, create automation clip, link to controller, edit events.
    static void showMenu(pulp::Param* p, juce::Component* target, std::function<void()> afterChange);
};

// A knob bound to a live parameter. Drag vertically; Ctrl for fine control;
// Alt-click or double right-click resets; wheel nudges; right-click opens the menu.
class ParamKnob : public juce::Component, public juce::SettableTooltipClient, private UiTicker::Client {
public:
    explicit ParamKnob(pulp::Param* p = nullptr);
    void setParam(pulp::Param* p) { param_ = p; repaint(); }
    pulp::Param* param() const { return param_; }
    std::function<void()> onChange;        // on every change made here
    std::function<void()> onGestureEnd;    // when a drag, wheel move or reset finishes
    juce::Colour colour = theme::accent;
    bool bipolar = false;
    juce::String label;                    // optional tiny caption under the knob

    juce::String getTooltip() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    void showMenu();
    void setValueNorm(float n, bool endGesture);
    void reset();

private:
    void uiTick() override;
    pulp::Param* param_ = nullptr;
    float shown_ = -1.0f;
    float dragStartNorm_ = 0.0f;
    bool dragging_ = false;
    juce::uint32 lastRightClick_ = 0;
};

// A number you drag up and down or double-click to type.
class NumberBox : public juce::Component, public juce::SettableTooltipClient, private UiTicker::Client {
public:
    std::function<double()> getValue;
    std::function<void(double)> setValue;
    std::function<void()> onGestureEnd;     // a drag, wheel move or typed value finished
    std::function<void()> onRightClick;
    std::function<juce::String(double)> format;
    double minValue = 0, maxValue = 100, step = 1, fineStep = 0.1;
    juce::String caption;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
private:
    void uiTick() override;
    double dragStart_ = 0, shown_ = -1e30;
    bool dragged_ = false;
};

// A flat button that draws a vector icon.
class IconButton : public juce::Component, public juce::SettableTooltipClient {
public:
    enum Icon { Play, Stop, Record, Pattern, Song, Metronome, Text };
    explicit IconButton(Icon i) : icon(i) {}
    Icon icon;
    juce::String text;
    bool lit = false;
    juce::Colour litColour = theme::accent;
    std::function<void()> onClick;
    std::function<void()> onRightClick;
    void setLit(bool b) { if (lit != b) { lit = b; repaint(); } }
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseEnter(const juce::MouseEvent&) override { over_ = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { over_ = false; repaint(); }
private:
    bool over_ = false;
};
