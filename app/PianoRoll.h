#pragma once
// The Piano Roll: keyboard on the left, note grid, ruler, per-note property lane, tools.
#include "ParamPanel.h"
#include "Session.h"
#include "pulp/NoteTools.h"

class PianoRoll : public juce::Component, public juce::TooltipClient, private Session::Listener, private UiTicker::Client {
public:
    enum Tool { Draw, Paint, Delete, Mute, Slice, Select, Zoom, Scrub, NumTools };
    explicit PianoRoll(Session& s);
    ~PianoRoll() override;

    bool handleKey(const juce::KeyPress& k);
    void setTool(int t);
    int tool() const { return tool_; }
    std::vector<pulp::Note>* notes();           // the current channel's notes in the current pattern
    void edited(const juce::String& what);      // commit an edit
    void toolsMenu(juce::Component* target);
    void runTool(int id);                       // ids are in PianoRollTools.cpp
    bool setToolParam(const juce::String& id, float value);    // a setting of the tool panel that is open (for scripted checks)
    void importMidi(const juce::File& f);
    void exportMidi(const juce::File& f);
    int snap(bool alt = false) const;
    void zoomToFit();
    void setBackdrop(bool on) { backdrop_ = on; repaint(); }
    juce::String describe() const;              // one line about the current state, for scripts

    void paint(juce::Graphics& g) override;
    void resized() override;
    juce::String getTooltip() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    bool keyPressed(const juce::KeyPress& k) override { return handleKey(k); }

    // scale highlighting and stamping, shared with the tools
    int scaleRoot = -1;         // 0..11, or -1 for none
    int scaleIndex = 0;
    int stampChord = -1;        // index into tools::chords(), or -1

    static constexpr int kTopH = 30, kRulerH = 20, kKeyW = 62, kLaneH = 92, kBarH = 12;

private:
    class Dialog;
    friend class Dialog;
    void projectChanged() override;
    void uiTick() override;

    // geometry
    juce::Rectangle<int> gridArea() const;
    juce::Rectangle<int> rulerArea() const;
    juce::Rectangle<int> keysArea() const;
    juce::Rectangle<int> laneArea() const;
    float xOfTick(double tick) const { return (float)gridArea().getX() + (float)((tick - scrollTick_) * pxPerTick_); }
    double tickOfX(float x) const { return scrollTick_ + (double)(x - (float)gridArea().getX()) / pxPerTick_; }
    float yOfKey(int key) const { return (float)gridArea().getY() + (float)((127 - key) * keyH_) - (float)scrollY_; }
    int keyOfY(float y) const { return 127 - (int)std::floor((y - (float)gridArea().getY() + (float)scrollY_) / (float)keyH_); }
    int snapTick(double tick, bool alt, bool floor = true) const;
    juce::Rectangle<float> noteRect(const pulp::Note& n) const;
    int noteAt(juce::Point<int> p, bool* onEdge = nullptr) const;
    void showNoteProperties(int index);     // double-click a note
    void clampScroll();
    void zoomX(double factor, float anchorX);
    void updateScrollbars();

    // painting
    void paintGrid(juce::Graphics& g);
    void paintNotes(juce::Graphics& g);
    void paintKeys(juce::Graphics& g);
    void paintRuler(juce::Graphics& g);
    void paintLane(juce::Graphics& g);
    void paintTop(juce::Graphics& g);

    // editing helpers
    void selectAll(bool on);
    void deleteSelected();
    void copySelection(bool cut);
    void paste();
    pulp::Note makeNote(int tick, int key) const;
    void addNoteAt(int tick, int key);
    void laneEdit(const juce::MouseEvent& e);
    void rulerMenu(int tick);
    void topClick(const juce::MouseEvent& e);
    void channelMenu();

    Session& s_;
    int tool_ = Draw;
    int snapChoice_ = 0;            // 0 = follow the main snap; else index into Session::snapChoices() + 1
    bool ghosts_ = true, backdrop_ = false, slideNext_ = false, portaNext_ = false;
    int colorNext_ = 0;
    int laneProp_ = pulp::tools::Velocity;
    pulp::Note template_;           // length and properties of the last note touched

    double pxPerTick_ = 0.11;
    int keyH_ = 14;
    double scrollTick_ = 0.0;
    int scrollY_ = 0;
    juce::ScrollBar hbar_{false}, vbar_{true};

    // gesture state
    enum Gesture { None, Move, Resize, Marquee, Erase, Muting, Painting, Slicing, Zooming, Scrubbing, Panning, RulerSeek, RulerLoop, Lane, KeyPlay, RightZoom };
    int gesture_ = None;
    juce::Point<int> downPos_, lastPos_;
    int downTick_ = 0, downKey_ = 0;
    bool moved_ = false, changed_ = false, wasPlaying_ = false;
    std::vector<pulp::Note> before_;        // notes at the start of the gesture
    std::vector<int> touched_;              // note indices already handled in this gesture
    juce::Rectangle<int> marquee_;
    int playingKey_ = -1;
    double zoomStartPx_ = 0.0;
    int zoomStartKeyH_ = 0;
    juce::Rectangle<int> topButtons_[32];
    int numTopButtons_ = 0;
    int hoverTop_ = -1;
    std::unique_ptr<Dialog> dialog_;
    static std::vector<pulp::Note> clipboard_;
    int shownChannelId_ = -1, shownPatternId_ = -1;
    int placedTick_ = 0;            // where the last click placed a note (undone if it turns into a ghost-note double-click)
    bool placedValid_ = false;
    float playX_ = -1.0f;
};
