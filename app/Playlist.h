#pragma once
// The Playlist: 500 tracks that hold pattern clips (audio and automation clips come later),
// with a picker panel, track headers, a ruler with markers and a loop region.
#include "Session.h"
#include "Widgets.h"

class Playlist : public juce::Component,
                 public juce::TooltipClient,
                 public juce::DragAndDropTarget,
                 public juce::FileDragAndDropTarget,
                 private Session::Listener,
                 private UiTicker::Client {
public:
    enum Tool { Draw, Paint, Delete, Mute, Slip, Slice, Select, Zoom, Scrub, NumTools };
    explicit Playlist(Session& s);
    ~Playlist() override;

    std::function<void(int patternIndex)> onOpenPattern;    // double-click on a clip
    bool handleKey(const juce::KeyPress& k);
    bool handleMoreKeys(const juce::KeyPress& k);
    void setTool(int t);
    juce::String describe() const;
    juce::String describeTracks(int count) const;       // where the first tracks are drawn: for scripted checks
    void setCollapsed(int track, bool collapsed);       // shows or hides the tracks grouped under this one

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
    bool isInterestedInDragSource(const SourceDetails& d) override { const auto s = d.description.toString(); return s.startsWith("pattern:") || s.startsWith("file:"); }
    void itemDropped(const SourceDetails& d) override;
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    void placeAudioFile(const juce::File& f, juce::Point<int> where);

    static constexpr int kTopH = 30, kRulerH = 20, kPickerW = 120, kHeadW = 112, kBarH = 12;

private:
    void projectChanged() override;
    void uiTick() override;

    juce::Rectangle<int> gridArea() const;
    juce::Rectangle<int> rulerArea() const;
    juce::Rectangle<int> headArea() const;
    juce::Rectangle<int> pickerArea() const;
    float xOfTick(double tick) const { return (float)gridArea().getX() + (float)((tick - scrollTick_) * pxPerTick_); }
    double tickOfX(float x) const { return scrollTick_ + (double)(x - (float)gridArea().getX()) / pxPerTick_; }
    int trackTop(int track) const;              // pixels from the top of track 0
    int trackHeight(int track) const;
    int trackAtY(int y) const;
    float yOfTrack(int track) const { return (float)gridArea().getY() + (float)trackTop(track) - (float)scrollY_; }
    int snap(bool alt) const;
    int snapTick(double tick, bool alt, bool floor = true) const;
    juce::Rectangle<float> clipRect(const pulp::Clip& c) const;
    int clipAt(juce::Point<int> p, int* part = nullptr) const;     // part: 0 body, 1 left edge, 2 right edge, 3 menu corner
    void clampScroll();
    void zoomX(double factor, float anchorX);
    void updateScrollbars();
    void rebuildTrackTops();

    void paintGrid(juce::Graphics& g);
    void paintClips(juce::Graphics& g);
    void paintHeads(juce::Graphics& g);
    void paintRuler(juce::Graphics& g);
    void paintPicker(juce::Graphics& g);
    void paintTop(juce::Graphics& g);

    void selectAll(bool on);
    void selectGroupOf(int clipIndex);
    void deleteSelected();
    void duplicateSelected();
    void groupSelected(bool group);
    void clipMenu(int clipIndex);
    void trackMenu(int track);
    void rulerMenu(pulp::int64 tick);
    void edited(const juce::String& what);
    bool locked(int track) const;

    Session& s_;
    int tool_ = Draw;
    double pxPerTick_ = 0.03;
    int baseTrackH_ = 30;
    double scrollTick_ = 0.0;
    int scrollY_ = 0;
    std::vector<int> trackTops_;        // kNumPlaylistTracks + 1 entries
    juce::ScrollBar hbar_{false}, vbar_{true};

    enum Gesture { None, Move, ResizeL, ResizeR, Marquee, Erase, Muting, Painting, Slipping, Slicing, Zooming, Scrubbing, Panning, RulerSeek, RulerLoop, RightZoom, PickerDrag, AutoPointDrag, AutoTensionDrag };
    // ---- automation clips ----
    void paintAutoClip(juce::Graphics& g, const pulp::Clip& c, juce::Rectangle<float> r, juce::Rectangle<float> vis);
    static juce::Rectangle<float> autoBody(juce::Rectangle<float> clip) { return clip.withTrimmedTop(13.0f).reduced(1.0f, 3.0f); }
    int autoHit(const pulp::Clip& c, juce::Point<int> p, int& what) const;     // point index; what: 1 a point, 2 the tension handle after it
    void autoPointMenu(int clipIndex, int pointIndex);
    void autoClipMenu(int clipIndex);
    bool canErase(int clipIndex, juce::Point<int> p) const;
    int autoIndex_ = -1;
    std::vector<pulp::AutoPoint> autoBefore_;
    int drawAutoId_ = 0;                // a curve chosen in the picker: the draw tool places a clip of it
    struct PickerRow { juce::Rectangle<int> area; int kind; int id; };          // kind 0 pattern (id = index), 1 audio, 2 automation
    std::vector<PickerRow> pickerRows_;
    int gesture_ = None;
    juce::Point<int> downPos_, lastPos_;
    int anchor_ = -1;                   // clip index the drag started on
    int downTrack_ = 0;
    pulp::int64 downTick_ = 0;
    bool changed_ = false, wasPlaying_ = false;
    std::vector<pulp::Clip> before_;
    std::vector<int> touched_;
    juce::Rectangle<int> marquee_;
    double zoomStartPx_ = 0.0;
    int hoverTop_ = -1;
    juce::Rectangle<int> topButtons_[16];
    float playX_ = -1.0f;
    int pickerDragPattern_ = -1;
    int drawAudioId_ = 0;               // an audio item chosen in the picker: the draw tool places it instead of a pattern
    void audioClipMenu(int clipIndex);
    static std::vector<pulp::Clip> clipboard_;
};
