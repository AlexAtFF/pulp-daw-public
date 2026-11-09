#pragma once
// The audio settings window (F10): driver, devices, sample rate, buffer size and which
// inputs and outputs are active.
#include "Session.h"
#include "Theme.h"

class AudioSettings : public juce::Component, private juce::ChangeListener {
public:
    explicit AudioSettings(Session& s);
    ~AudioSettings() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void visibilityChanged() override { if (isShowing()) refresh(); }
    void refresh();

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }
    void apply();
    Session& s_;
    juce::ComboBox type_, output_, input_, rate_, buffer_;
    juce::TextButton panel_{"Driver control panel"};
    juce::TextButton measure_{"Measure latency"}, forget_{"Forget"};
    juce::String latencyText() const;
    juce::Rectangle<int> latencyArea_;
    juce::StringArray outNames_, inNames_;
    juce::BigInteger outActive_, inActive_;
    juce::Rectangle<int> outArea_, inArea_, infoArea_;
    juce::String info_, error_;
    bool refreshing_ = false;
};
