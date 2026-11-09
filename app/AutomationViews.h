#pragma once
// The windows and panels that belong to automation and controllers:
//   EventEditor     the events of one control inside the current pattern
//   LinkEditor      "Link to controller": source, range, smoothing and mapping formula for one control
//   MidiSettings    which MIDI devices play notes and which act as controllers
//   SettingsPages   the F10 window: audio and MIDI pages
//   ControllerView  the display at the top of a controller plugin's editor
#include "AudioSettings.h"
#include "Session.h"
#include "Widgets.h"
#include "PluginSettings.h"
#include "Shortcuts.h"

class EventEditor : public juce::Component, public juce::TooltipClient, private Session::Listener, private UiTicker::Client {
public:
    explicit EventEditor(Session& s);
    ~EventEditor() override;
    std::function<void(const juce::String&)> onTitle;
    void paint(juce::Graphics& g) override;
    void resized() override;
    juce::String getTooltip() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    juce::String describe() const;
private:
    void projectChanged() override;
    void uiTick() override;
    juce::Rectangle<int> grid() const;
    int length() const;                                 // ticks shown: the pattern's length
    int tickAt(int x, bool snapped) const;
    float valueAt(int y) const;
    std::vector<pulp::AutoEvent>* events(bool create);
    void targetMenu();
    Session& s_;
    IconButton target_{IconButton::Text}, clear_{IconButton::Text};
    int downTick_ = 0, lastTick_ = 0;
    float downValue_ = 0.0f;
    bool drawing_ = false, erasing_ = false, line_ = false, changed_ = false;
    juce::Point<int> lineEnd_;
    float playX_ = -1.0f;
};

class LinkEditor : public juce::Component, private Session::Listener, private UiTicker::Client {
public:
    explicit LinkEditor(Session& s);
    ~LinkEditor() override;
    void setTarget(const std::string& address);
    std::function<void(const juce::String&)> onTitle;
    void paint(juce::Graphics& g) override;
    void resized() override;
private:
    void projectChanged() override { refresh(); }
    void uiTick() override;
    void refresh();
    void apply();                                       // writes the controls back into the project's link
    Session& s_;
    std::string address_;
    juce::ComboBox source_;
    pulp::Param minP_, maxP_, smoothP_;
    ParamKnob min_, max_, smooth_;
    juce::TextEditor formula_;
    IconButton remove_{IconButton::Text};
    std::vector<Session::ControllerSource> sources_;
    juce::String status_;
    bool refreshing_ = false, wasLearning_ = false;
};

class MidiSettings : public juce::Component, private Session::Listener, private UiTicker::Client {
public:
    explicit MidiSettings(Session& s);
    ~MidiSettings() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
private:
    void projectChanged() override { repaint(); }
    void uiTick() override;
    Session& s_;
    juce::String shownText_;
};

class SettingsPages : public juce::Component {
public:
    explicit SettingsPages(Session& s);
    void resized() override;
    void paint(juce::Graphics& g) override { g.fillAll(theme::panel); }
    void showPage(int page);                            // 0 audio, 1 MIDI, 2 plugins, 3 shortcuts
    PluginSettings& plugins() { return plugins_; }
    ShortcutSettings& shortcuts() { return shortcuts_; }
private:
    IconButton audioTab_{IconButton::Text}, midiTab_{IconButton::Text}, pluginTab_{IconButton::Text}, keysTab_{IconButton::Text};
    ShortcutSettings shortcuts_;
    AudioSettings audio_;
    MidiSettings midi_;
    PluginSettings plugins_;
    int page_ = 0;
};

class ControllerView : public juce::Component, public juce::TooltipClient, private UiTicker::Client, private juce::TextEditor::Listener {
public:
    ControllerView(Session& s, std::shared_ptr<pulp::Plugin> plugin);
    static int heightFor(const pulp::Plugin& p);
    void paint(juce::Graphics& g) override;
    void resized() override;
    juce::String getTooltip() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
private:
    void uiTick() override { if (UiTicker::onSlowTick() && isShowing()) repaint(); }
    void textEditorReturnKeyPressed(juce::TextEditor&) override { applyFormula(); }
    void textEditorFocusLost(juce::TextEditor&) override { applyFormula(); }
    void applyFormula();
    juce::Rectangle<int> padArea() const;
    void setPad(juce::Point<int> p);
    Session& s_;
    std::shared_ptr<pulp::Plugin> plugin_;
    bool isPad_ = false, isFormula_ = false, dragging_ = false;
    juce::TextEditor formula_;
    juce::String formulaStatus_;
};
