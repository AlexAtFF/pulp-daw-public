#pragma once
// The Mixer: master, 125 inserts and the "current" track as strips, with the selected
// track's effect slots, EQ and switches in a panel on the right.
#include "ParamPanel.h"
#include "Session.h"

class MixerView : public juce::Component, public juce::TooltipClient, private Session::Listener, private UiTicker::Client {
public:
    explicit MixerView(Session& s);
    ~MixerView() override;
    std::function<void(std::shared_ptr<pulp::Plugin>, const juce::String& title)> onOpenEffect;

    void selectTrack(int index);
    bool moveTrack(int track, int by);          // an insert track changes places with its neighbour (by = -1 or 1); the selection follows it
    bool setEffect(int track, int slot, const std::string& typeId);    // empty typeId clears the slot
    void toggleRoute(int from, int to);
    juce::String describe() const;

    void paint(juce::Graphics& g) override;
    void resized() override;
    juce::String getTooltip() override;
    void mouseDown(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& k) override;

    static constexpr int kTopH = 26, kPanelW = 262;

private:
    class Strip;
    class Holder;
    class Panel;
    friend class Strip;
    friend class Panel;
    void projectChanged() override;
    void uiTick() override;
    void layoutStrips();
    void layoutMenu();
    void trackMenu(int track, juce::Component* target);
    void slotMenu(int slot, juce::Component* target);
    int stripWidth() const;
    std::vector<int> order() const;     // track indices in display order: master, current, left dock, inserts, right dock

    Session& s_;
    int layout_ = 1;                    // 0 compact .. 3 extra large
    juce::Viewport view_;
    std::unique_ptr<Holder> holder_;
    juce::OwnedArray<Strip> strips_;    // one per mixer track, indexed by track number
    std::unique_ptr<Panel> panel_;
    IconButton layoutBtn_{IconButton::Text};
    juce::String signature_;
};
