#pragma once
// The audio editor's panel, shown inside the effect window of a Splice: the recorded or loaded
// audio as a waveform with a selection, and the editing buttons.
#include "Session.h"
#include "Widgets.h"
#include "pulp/Editor.h"

class SpliceView : public juce::Component, public juce::TooltipClient, private UiTicker::Client {
public:
    SpliceView(Session& s, std::shared_ptr<pulp::Plugin> plugin);
    static constexpr int kHeight = 252;

    // Everything a button does is also a named operation, so scripts can run them.
    // trim, cut, silence, fadein, fadeout, normalize, reverse, learn, denoise, undo, new, all, play, stop
    bool op(const juce::String& name);
    void setSelection(double fromSeconds, double toSeconds);
    bool loadFile(const juce::File& f);
    bool saveAs(const juce::File& f);
    int toPlaylist();                       // puts the selection (or everything) on the playlist; returns the clip index or -1
    juce::String describe() const;

    void paint(juce::Graphics& g) override;
    void resized() override;
    juce::String getTooltip() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

private:
    void uiTick() override;
    pulp::Splice& ed() const { return *static_cast<pulp::Splice*>(plugin_.get()); }
    juce::Rectangle<int> waveArea() const;
    pulp::int64 frameAtX(int x) const;
    void apply(std::shared_ptr<pulp::SampleData> next, const juce::String& what);
    juce::File writeSelection(const juce::String& prefix);

    Session& s_;
    std::shared_ptr<pulp::Plugin> plugin_;
    struct Button { const char* op; const char* label; const char* hint; };
    juce::OwnedArray<IconButton> buttons_;
    std::vector<juce::String> buttonHints_;
    pulp::int64 dragAnchor_ = 0;
    bool selecting_ = false, draggingOut_ = false;
    pulp::uint64 shownUid_ = 0;
    pulp::int64 shownFrames_ = -1;
    juce::String status_;
};
