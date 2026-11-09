#include "Playlist.h"
#include "pulp/Stretch.h"

using pulp::Clip;
using pulp::int64;
using pulp::kNumPlaylistTracks;
using pulp::kTicksPerStep;

std::vector<Clip> Playlist::clipboard_;

namespace {
struct TopButton { const char* label; const char* hint; };
const TopButton kTools[] = {
    {"P", "Draw tool (P): click to place the selected pattern, drag a clip to move it, drag an edge to resize, right-click to delete"},
    {"B", "Paint tool (B): drag to place the selected pattern over and over"},
    {"D", "Delete tool (D): click or drag over clips to delete them"},
    {"T", "Mute tool (T): click or drag over clips to mute or unmute them"},
    {"S", "Slip tool (S): drag inside a clip to slide its contents without moving the clip"},
    {"C", "Slice tool (C): drag a line across clips to cut them"},
    {"E", "Select tool (E): drag a box to select clips (Ctrl-drag does the same in any tool)"},
    {"Z", "Zoom tool (Z): drag a box to zoom into it, right-click to zoom out"},
    {"Y", "Playback tool (Y): hold the mouse down to play the song from that point"},
};
// "Line" snap in the playlist is coarser than in the piano roll: steps, beats or bars by zoom.
int playlistLine(double pxPerTick, int bar) {
    for (int t : {240, 960}) if (t * pxPerTick >= 12.0) return t;
    return bar;
}
}

Playlist::Playlist(Session& s) : s_(s) {
    setOpaque(true);
    setWantsKeyboardFocus(true);
    addAndMakeVisible(hbar_);
    addAndMakeVisible(vbar_);
    hbar_.setAutoHide(false);
    vbar_.setAutoHide(false);
    rebuildTrackTops();
    s_.addListener(this);
}

Playlist::~Playlist() { s_.removeListener(this); }

void Playlist::projectChanged() {
    rebuildTrackTops();
    updateScrollbars();
    repaint();
}

void Playlist::edited(const juce::String& what) { s_.commit(what); }

bool Playlist::locked(int track) const { return track >= 0 && track < kNumPlaylistTracks && s_.project.tracks[(size_t)track].locked; }

// ------------------------------------------------------------------ geometry

juce::Rectangle<int> Playlist::gridArea() const {
    const int x = kPickerW + kHeadW;
    return {x, kTopH + kRulerH, juce::jmax(10, getWidth() - x - kBarH), juce::jmax(10, getHeight() - kTopH - kRulerH - kBarH)};
}
juce::Rectangle<int> Playlist::rulerArea() const { return {gridArea().getX(), kTopH, gridArea().getWidth(), kRulerH}; }
juce::Rectangle<int> Playlist::headArea() const { return {kPickerW, gridArea().getY(), kHeadW, gridArea().getHeight()}; }
juce::Rectangle<int> Playlist::pickerArea() const { return {0, kTopH, kPickerW, getHeight() - kTopH}; }

void Playlist::rebuildTrackTops() {
    trackTops_.resize((size_t)kNumPlaylistTracks + 1);
    int y = 0;
    for (int i = 0; i < kNumPlaylistTracks; ++i) {
        trackTops_[(size_t)i] = y;
        if (s_.project.trackHidden(i)) continue;            // a collapsed group's tracks take no room
        y += juce::jmax(12, (int)std::lround(baseTrackH_ * s_.project.tracks[(size_t)i].height));
    }
    trackTops_[(size_t)kNumPlaylistTracks] = y;
}
int Playlist::trackTop(int track) const { return trackTops_[(size_t)juce::jlimit(0, kNumPlaylistTracks, track)]; }
int Playlist::trackHeight(int track) const { return trackTop(track + 1) - trackTop(track); }
int Playlist::trackAtY(int y) const {
    const int v = y - gridArea().getY() + scrollY_;
    auto it = std::upper_bound(trackTops_.begin(), trackTops_.end(), v);
    return juce::jlimit(0, kNumPlaylistTracks - 1, (int)(it - trackTops_.begin()) - 1);
}

int Playlist::snap(bool alt) const {
    if (alt) return 1;
    const int t = Session::snapChoices()[(size_t)s_.snap.getInt()].second;
    if (t == 0) return playlistLine(pxPerTick_, s_.project.ticksPerBar());
    if (t < 0) return s_.project.ticksPerBar();
    return t;
}
int Playlist::snapTick(double tick, bool alt, bool floor) const {
    const int g = snap(alt);
    const double v = floor ? std::floor(tick / g) : std::round(tick / g);
    return juce::jmax(0, (int)v * g);
}

juce::Rectangle<float> Playlist::clipRect(const Clip& c) const {
    return {xOfTick((double)c.start), yOfTrack(c.track) + 1.0f, juce::jmax(4.0f, (float)(c.length * pxPerTick_)), (float)trackHeight(c.track) - 2.0f};
}

int Playlist::clipAt(juce::Point<int> p, int* part) const {
    const auto& clips = s_.project.clips;
    for (int i = (int)clips.size() - 1; i >= 0; --i) {
        const auto r = clipRect(clips[(size_t)i]);
        if (!r.contains(p.toFloat())) continue;
        if (part) {
            const float edge = juce::jmin(7.0f, r.getWidth() * 0.3f);
            if (p.x <= r.getX() + 13.0f && p.y <= r.getY() + 12.0f && r.getWidth() > 30.0f) *part = 3;
            else if (p.x <= r.getX() + edge) *part = 1;
            else if (p.x >= r.getRight() - edge) *part = 2;
            else *part = 0;
        }
        return i;
    }
    return -1;
}

void Playlist::clampScroll() {
    scrollTick_ = juce::jmax(0.0, scrollTick_);
    scrollY_ = juce::jlimit(0, juce::jmax(0, trackTop(kNumPlaylistTracks) - gridArea().getHeight()), scrollY_);
}

void Playlist::zoomX(double factor, float anchorX) {
    const double anchorTick = tickOfX(anchorX);
    pxPerTick_ = juce::jlimit(0.0008, 0.5, pxPerTick_ * factor);
    scrollTick_ = anchorTick - (double)(anchorX - (float)gridArea().getX()) / pxPerTick_;
    clampScroll();
    updateScrollbars();
    repaint();
}

void Playlist::updateScrollbars() {
    const auto g = gridArea();
    const double total = (double)s_.project.songLengthTicks() + s_.project.ticksPerBar() * 16.0 + g.getWidth() / pxPerTick_;
    hbar_.setRangeLimits(0.0, total, juce::dontSendNotification);
    hbar_.setCurrentRange(scrollTick_, g.getWidth() / pxPerTick_, juce::dontSendNotification);
    vbar_.setRangeLimits(0.0, (double)trackTop(kNumPlaylistTracks), juce::dontSendNotification);
    vbar_.setCurrentRange((double)scrollY_, (double)g.getHeight(), juce::dontSendNotification);
}

void Playlist::resized() {
    const auto g = gridArea();
    hbar_.setBounds(g.getX(), getHeight() - kBarH, g.getWidth(), kBarH);
    vbar_.setBounds(getWidth() - kBarH, g.getY(), kBarH, g.getHeight());
    clampScroll();
    updateScrollbars();
}

void Playlist::uiTick() {
    if (hbar_.isMouseButtonDown() && std::fabs(hbar_.getCurrentRangeStart() - scrollTick_) > 0.5) { scrollTick_ = hbar_.getCurrentRangeStart(); repaint(); }
    if (vbar_.isMouseButtonDown() && (int)vbar_.getCurrentRangeStart() != scrollY_) { scrollY_ = (int)vbar_.getCurrentRangeStart(); repaint(); }
    float x = -1.0f;
    if (s_.project.songMode && (s_.engine.isPlaying() || s_.engine.positionTicks() > 0.0)) x = xOfTick(s_.engine.positionTicks());
    if (x != playX_) {
        const auto g = gridArea();
        auto strip = [&](float px) { if (px >= 0) repaint((int)px - 6, kTopH, 13, g.getBottom() - kTopH); };
        strip(playX_);
        playX_ = x;
        strip(playX_);
        if (s_.engine.isPlaying() && gesture_ == None && x > (float)g.getRight() - 4.0f) { scrollTick_ += g.getWidth() * 0.9 / pxPerTick_; updateScrollbars(); repaint(); }
        else if (s_.engine.isPlaying() && gesture_ == None && x >= 0.0f && x < (float)g.getX()) { scrollTick_ = juce::jmax(0.0, s_.engine.positionTicks() - 960.0); updateScrollbars(); repaint(); }
    }
}

// ------------------------------------------------------------------ painting

void Playlist::paint(juce::Graphics& g) {
    g.fillAll(theme::panelLo);
    paintGrid(g);
    paintClips(g);
    paintHeads(g);
    paintRuler(g);
    paintPicker(g);
    paintTop(g);
    g.setColour(theme::panel);
    g.fillRect(kPickerW, kTopH, kHeadW, kRulerH);
    g.fillRect(getWidth() - kBarH, kTopH, kBarH, kRulerH);
    g.fillRect(getWidth() - kBarH, getHeight() - kBarH, kBarH, kBarH);
    g.fillRect(kPickerW, getHeight() - kBarH, kHeadW, kBarH);
}

void Playlist::paintGrid(juce::Graphics& g) {
    const auto a = gridArea();
    g.saveState();
    g.reduceClipRegion(a);
    const auto clip = g.getClipBounds();
    const int first = trackAtY(clip.getY()), last = trackAtY(clip.getBottom());
    for (int t = first; t <= last; ++t) {
        if (trackHeight(t) <= 0) continue;
        const float y = yOfTrack(t);
        const auto& tr = s_.project.tracks[(size_t)t];
        g.setColour(t % 2 == 0 ? juce::Colour(0xff2b3035) : juce::Colour(0xff282c31));
        g.fillRect((float)a.getX(), y, (float)a.getWidth(), (float)trackHeight(t));
        if (!s_.project.trackAudible(t)) { g.setColour(juce::Colours::black.withAlpha(0.28f)); g.fillRect((float)a.getX(), y, (float)a.getWidth(), (float)trackHeight(t)); }
        if (tr.locked) { g.setColour(theme::warn.withAlpha(0.05f)); g.fillRect((float)a.getX(), y, (float)a.getWidth(), (float)trackHeight(t)); }
        g.setColour(theme::line.withAlpha(0.7f));
        g.drawHorizontalLine((int)y + trackHeight(t) - 1, (float)a.getX(), (float)a.getRight());
    }
    const int bar = s_.project.ticksPerBar();
    const int beat = pulp::kPPQ * 4 / juce::jmax(1, s_.project.timeSigDen);
    const int fine = playlistLine(pxPerTick_, bar);
    const int t0 = (int)(std::floor(tickOfX((float)clip.getX()) / fine) * fine), t1 = (int)tickOfX((float)clip.getRight()) + fine;
    for (int t = juce::jmax(0, t0); t <= t1; t += fine) {
        const bool isBar = t % bar == 0, isBeat = t % beat == 0;
        if (!isBar && bar * pxPerTick_ < 30.0) continue;
        g.setColour(isBar ? juce::Colour(0xff101214) : (isBeat ? juce::Colour(0xff1c1f22) : juce::Colour(0xff23272b)));
        g.drawVerticalLine((int)xOfTick(t), (float)a.getY(), (float)a.getBottom());
    }
    if (s_.project.loopEnd > s_.project.loopStart) {
        g.setColour(theme::accent.withAlpha(0.07f));
        g.fillRect(xOfTick((double)s_.project.loopStart), (float)a.getY(), (float)((s_.project.loopEnd - s_.project.loopStart) * pxPerTick_), (float)a.getHeight());
    }
    g.restoreState();
}

void Playlist::paintClips(juce::Graphics& g) {
    const auto a = gridArea();
    g.saveState();
    g.reduceClipRegion(a);
    const auto vis = g.getClipBounds().toFloat();
    for (auto& c : s_.project.clips) {
        const auto r = clipRect(c);
        if (!r.intersects(vis)) continue;
        if (c.type == Clip::AutomationClip) { paintAutoClip(g, c, r, vis); continue; }
        if (c.type == Clip::AudioClip) {
            // an audio clip: the part of the file it plays, drawn as a waveform
            const auto* a = s_.project.audioById(c.audioId);
            const juce::Colour col = c.muted ? juce::Colour(0xff555b62) : juce::Colour(0xff3f8f86);
            g.setColour(col.withMultipliedBrightness(c.selected ? 1.2f : 0.8f));
            g.fillRoundedRectangle(r, 3.0f);
            if (a && a->valid() && r.getHeight() > 16.0f) {
                // file frames per tick; a stretched clip covers its file faster or slower than the tempo alone says
                const double spt = 60.0 * a->rate() / ((double)s_.project.global->tempo.get() * pulp::kPPQ) / s_.project.clipLengthRatio(c);
                const double head = (double)c.sourceOffsetFrames + (double)c.offset * spt;
                const double framesPerPx = spt / pxPerTick_;
                const float mid = r.getCentreY() + 5.0f, h = (r.getHeight() - 14.0f) * 0.5f * juce::jmin(1.0f, c.gain);
                const auto frames = a->frames();
                g.setColour(col.brighter(1.0f).withAlpha(c.muted ? 0.4f : 0.95f));
                const int x0 = juce::jmax((int)r.getX(), (int)vis.getX()), x1 = juce::jmin((int)r.getRight(), (int)vis.getRight());
                for (int x = x0; x < x1; ++x) {
                    double f0 = head + (double)(x - r.getX()) * framesPerPx;
                    if (c.reverse) f0 = (double)frames - f0 - framesPerPx;
                    const auto i0 = (int64)f0, i1 = (int64)(f0 + framesPerPx);
                    if (i1 < 0 || i0 >= frames) continue;
                    float mn = 0.0f, mx = 0.0f;
                    a->minMax(i0, i1 + 1, mn, mx);          // long files are drawn from their overview, not read from disk
                    g.drawVerticalLine(x, mid - mx * h, mid - mn * h + 1.0f);
                }
            }
            g.setColour(juce::Colours::black.withAlpha(0.35f));
            g.fillRect(r.withHeight(juce::jmin(12.0f, r.getHeight())));
            g.setColour(c.selected ? juce::Colours::white : juce::Colours::black.withAlpha(0.8f));
            g.drawRoundedRectangle(r, 3.0f, c.selected ? 1.5f : 1.0f);
            if (r.getWidth() > 30.0f) {
                g.setColour(theme::text);
                g.setFont(theme::font(10.5f, true));
                juce::String label = juce::String(c.group != 0 ? "[g] " : "") + (c.take > 0 ? "Take " + juce::String(c.take) + "  " : juce::String())
                                   + (a ? juce::String(a->name) : juce::String("missing audio"));
                if (c.sync) label << "  (tempo)";
                else if (s_.project.clipNeedsProcessing(c)) label << "  (stretched)";
                g.drawText(label, r.withHeight(12.0f).withTrimmedLeft(14.0f).withTrimmedRight(2.0f), juce::Justification::centredLeft, true);
                juce::Path tri;
                tri.addTriangle(r.getX() + 3.0f, r.getY() + 3.5f, r.getX() + 10.0f, r.getY() + 3.5f, r.getX() + 6.5f, r.getY() + 8.5f);
                g.fillPath(tri);
            }
            continue;
        }
        const auto* pat = s_.project.patternById(c.patternId);
        juce::Colour col = pat ? juce::Colour(pat->color) : theme::textDim;
        if (c.muted) col = juce::Colour(0xff555b62);
        g.setColour(col.withMultipliedBrightness(c.selected ? 1.15f : 0.75f));
        g.fillRoundedRectangle(r, 3.0f);
        // mini preview of the pattern's notes inside the clip
        if (pat && r.getHeight() > 16.0f) {
            int lo = 127, hi = 0;
            for (auto& kv : pat->notes) for (auto& n : kv.second) { lo = std::min(lo, n.key); hi = std::max(hi, n.key); }
            const float span = (float)std::max(6, hi - lo + 1);
            const auto body = r.withTrimmedTop(12.0f).reduced(1.0f, 2.0f);
            g.setColour(col.brighter(0.9f).withAlpha(c.muted ? 0.4f : 0.9f));
            for (auto& kv : pat->notes)
                for (auto& n : kv.second) {
                    if (n.end() <= c.offset || n.tick >= c.offset + c.length) continue;
                    const float x = r.getX() + (float)((n.tick - c.offset) * pxPerTick_);
                    const float w = juce::jmax(1.5f, (float)(n.length * pxPerTick_));
                    const float y = body.getBottom() - 2.0f - (body.getHeight() - 3.0f) * (float)(n.key - lo) / span;
                    g.fillRect(juce::jmax(x, r.getX()), y, juce::jmin(w, r.getRight() - x), 2.0f);
                }
        }
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillRect(r.withHeight(juce::jmin(12.0f, r.getHeight())));
        g.setColour(c.selected ? juce::Colours::white : juce::Colours::black.withAlpha(0.8f));
        g.drawRoundedRectangle(r, 3.0f, c.selected ? 1.5f : 1.0f);
        if (r.getWidth() > 30.0f) {
            g.setColour(theme::text);
            g.setFont(theme::font(10.5f, true));
            g.drawText(juce::String(c.group != 0 ? "[g] " : "") + (pat ? juce::String(pat->name) : juce::String("?")), r.withHeight(12.0f).withTrimmedLeft(14.0f).withTrimmedRight(2.0f), juce::Justification::centredLeft, true);
            juce::Path tri;                         // the corner that opens the clip menu
            tri.addTriangle(r.getX() + 3.0f, r.getY() + 3.5f, r.getX() + 10.0f, r.getY() + 3.5f, r.getX() + 6.5f, r.getY() + 8.5f);
            g.fillPath(tri);
        }
    }
    if (gesture_ == Marquee || gesture_ == Zooming) {
        g.setColour(theme::accent.withAlpha(0.15f));
        g.fillRect(marquee_);
        g.setColour(theme::accent);
        g.drawRect(marquee_, 1);
    }
    if (gesture_ == Slicing) { g.setColour(theme::bad); g.drawLine((float)downPos_.x, (float)downPos_.y, (float)lastPos_.x, (float)lastPos_.y, 1.5f); }
    if (playX_ >= (float)a.getX()) { g.setColour(theme::good); g.drawVerticalLine((int)playX_, (float)a.getY(), (float)a.getBottom()); }
    g.restoreState();
}

// An automation clip: its curve, the points you drag and the small rings that bend the stretch between two points.
void Playlist::paintAutoClip(juce::Graphics& g, const Clip& c, juce::Rectangle<float> r, juce::Rectangle<float> vis) {
    const auto* a = s_.project.autoById(c.autoId);
    const juce::Colour col = c.muted ? juce::Colour(0xff555b62) : juce::Colour(a ? a->color : 0xff9a6fb0);
    g.setColour(col.withMultipliedBrightness(c.selected ? 0.7f : 0.45f));
    g.fillRoundedRectangle(r, 3.0f);
    if (a && r.getHeight() > 18.0f) {
        const auto body = autoBody(r);
        auto yOf = [&](float v) { return body.getBottom() - v * body.getHeight(); };
        auto xOf = [&](double tick) { return r.getX() + (float)((tick - (double)c.offset) * pxPerTick_); };
        const float x0 = juce::jmax(r.getX(), vis.getX()), x1 = juce::jmin(r.getRight(), vis.getRight());
        juce::Path line;
        bool first = true;
        for (float x = x0; x <= x1; x += 2.0f) {
            const double tick = (double)c.offset + (double)(x - r.getX()) / pxPerTick_;
            const float y = yOf(a->min + (a->max - a->min) * pulp::evalAuto(a->points, a->lfo, tick));
            if (first) { line.startNewSubPath(x, y); first = false; } else line.lineTo(x, y);
        }
        if (!first) {
            juce::Path fill = line;
            fill.lineTo(x1, body.getBottom());
            fill.lineTo(x0, body.getBottom());
            fill.closeSubPath();
            g.setColour(col.brighter(0.6f).withAlpha(c.muted ? 0.1f : 0.22f));
            g.fillPath(fill);
            g.setColour(col.brighter(1.2f).withAlpha(c.muted ? 0.5f : 1.0f));
            g.strokePath(line, juce::PathStrokeType(1.5f));
        }
        if (r.getHeight() > 24.0f && !a->lfo.on) {
            for (size_t i = 0; i < a->points.size(); ++i) {
                const auto& pt = a->points[i];
                const float x = xOf((double)pt.tick);
                if (x < r.getX() - 1.0f || x > r.getRight() + 1.0f) continue;
                const float y = yOf(a->min + (a->max - a->min) * pt.value);
                g.setColour(juce::Colours::white);
                g.fillEllipse(x - 3.5f, y - 3.5f, 7.0f, 7.0f);
                g.setColour(juce::Colours::black);
                g.drawEllipse(x - 3.5f, y - 3.5f, 7.0f, 7.0f, 1.0f);
                if (i + 1 < a->points.size() && (pt.shape == pulp::ShapeCurve || pt.shape == pulp::ShapeDouble || pt.shape >= pulp::ShapeStairs)) {
                    const double mid = 0.5 * (double)(pt.tick + a->points[i + 1].tick);
                    const float mx = xOf(mid);
                    if (mx > r.getX() && mx < r.getRight() && (a->points[i + 1].tick - pt.tick) * pxPerTick_ > 22.0) {
                        const float my = yOf(a->min + (a->max - a->min) * pulp::evalAuto(a->points, a->lfo, mid));
                        g.setColour(col.brighter(1.4f));
                        g.drawEllipse(mx - 3.0f, my - 3.0f, 6.0f, 6.0f, 1.2f);
                    }
                }
            }
        }
    }
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillRect(r.withHeight(juce::jmin(12.0f, r.getHeight())));
    g.setColour(c.selected ? juce::Colours::white : juce::Colours::black.withAlpha(0.8f));
    g.drawRoundedRectangle(r, 3.0f, c.selected ? 1.5f : 1.0f);
    if (r.getWidth() > 30.0f) {
        g.setColour(theme::text);
        g.setFont(theme::font(10.5f, true));
        g.drawText(juce::String(c.group != 0 ? "[g] " : "") + (a ? juce::String(a->name) + (a->lfo.on ? "  (LFO)" : "") : juce::String("missing curve")),
                   r.withHeight(12.0f).withTrimmedLeft(14.0f).withTrimmedRight(2.0f), juce::Justification::centredLeft, true);
        juce::Path tri;
        tri.addTriangle(r.getX() + 3.0f, r.getY() + 3.5f, r.getX() + 10.0f, r.getY() + 3.5f, r.getX() + 6.5f, r.getY() + 8.5f);
        g.fillPath(tri);
    }
}

int Playlist::autoHit(const Clip& c, juce::Point<int> p, int& what) const {
    what = 0;
    const auto* a = s_.project.autoById(c.autoId);
    if (!a || a->lfo.on) return -1;
    const auto r = clipRect(c);
    const auto body = autoBody(r);
    auto yOf = [&](float v) { return body.getBottom() - (a->min + (a->max - a->min) * v) * body.getHeight(); };
    auto xOf = [&](double tick) { return r.getX() + (float)((tick - (double)c.offset) * pxPerTick_); };
    for (int i = (int)a->points.size() - 1; i >= 0; --i) {
        const auto& pt = a->points[(size_t)i];
        if (std::hypot(xOf((double)pt.tick) - (float)p.x, yOf(pt.value) - (float)p.y) <= 6.5f) { what = 1; return i; }
    }
    for (int i = 0; i + 1 < (int)a->points.size(); ++i) {
        const auto& pt = a->points[(size_t)i];
        if ((a->points[(size_t)i + 1].tick - pt.tick) * pxPerTick_ <= 22.0) continue;
        const double mid = 0.5 * (double)(pt.tick + a->points[(size_t)i + 1].tick);
        if (std::hypot(xOf(mid) - (float)p.x, yOf(pulp::evalAuto(a->points, a->lfo, mid)) - (float)p.y) <= 6.0f) { what = 2; return i; }
    }
    return -1;
}

// Right-click deletes clips, but inside an automation clip's curve area it adds a point instead:
// there only the title strip deletes (the delete tool deletes anywhere).
bool Playlist::canErase(int clipIndex, juce::Point<int> p) const {
    const auto& c = s_.project.clips[(size_t)clipIndex];
    if (locked(c.track)) return false;
    if (c.type != Clip::AutomationClip || tool_ == Delete) return true;
    return (float)p.y <= clipRect(c).getY() + 12.0f;
}

void Playlist::paintHeads(juce::Graphics& g) {
    const auto a = headArea();
    g.saveState();
    g.reduceClipRegion(a);
    g.fillAll(theme::panel);
    const int first = trackAtY(a.getY()), last = trackAtY(a.getBottom());
    for (int t = first; t <= last; ++t) {
        if (trackHeight(t) <= 0) continue;
        const auto& tr = s_.project.tracks[(size_t)t];
        const juce::Rectangle<float> r((float)a.getX(), yOfTrack(t), (float)a.getWidth(), (float)trackHeight(t));
        g.setColour(juce::Colour(tr.color));
        g.fillRect(r.reduced(1.0f, 0.5f));
        if (tr.grouped) { g.setColour(theme::accent2); g.fillRect(r.getX() + 1.0f, r.getY(), 3.0f, r.getHeight()); }
        // the top track of a group has a triangle that folds the group away
        const bool head = s_.project.trackIsGroupHead(t);
        int under = 0;
        if (head) {
            for (int u = t + 1; u < kNumPlaylistTracks && s_.project.tracks[(size_t)u].grouped; ++u) ++under;
            juce::Path tri;
            const float cx = r.getX() + 8.0f, cy = r.getCentreY();
            if (tr.collapsed) tri.addTriangle(cx - 2.5f, cy - 4.5f, cx - 2.5f, cy + 4.5f, cx + 3.5f, cy);
            else tri.addTriangle(cx - 4.5f, cy - 2.5f, cx + 4.5f, cy - 2.5f, cx, cy + 3.5f);
            g.setColour(theme::accent2.brighter(0.3f));
            g.fillPath(tri);
        }
        const auto lamp = juce::Rectangle<float>(r.getRight() - 15.0f, r.getCentreY() - 5.0f, 10.0f, 10.0f);
        g.setColour(theme::inset);
        g.fillRoundedRectangle(lamp, 2.0f);
        g.setColour(tr.mute ? theme::panelHi : (tr.solo ? theme::warn : theme::good));
        g.fillRoundedRectangle(lamp.reduced(2.0f), 1.5f);
        g.setColour(theme::text.withAlpha(s_.project.trackAudible(t) ? 0.95f : 0.45f));
        g.setFont(theme::font(11.0f));
        g.drawText((tr.locked ? juce::String("[lock] ") : juce::String()) + (tr.name.empty() ? "Track " + juce::String(t + 1) : juce::String(tr.name))
                       + (head && tr.collapsed ? "  (+" + juce::String(under) + ")" : juce::String()),
                   r.withTrimmedLeft(head ? 16.0f : (tr.grouped ? 9.0f : 6.0f)).withTrimmedRight(18.0f), juce::Justification::centredLeft, true);
        g.setColour(theme::line);
        g.drawHorizontalLine((int)r.getBottom() - 1, r.getX(), r.getRight());
    }
    g.restoreState();
}

void Playlist::paintRuler(juce::Graphics& g) {
    const auto a = rulerArea();
    g.saveState();
    g.reduceClipRegion(a);
    g.fillAll(theme::panel);
    const int bar = s_.project.ticksPerBar();
    const int every = bar * pxPerTick_ < 26.0 ? (int)std::ceil(26.0 / (bar * pxPerTick_)) : 1;
    if (s_.project.loopEnd > s_.project.loopStart) {
        g.setColour(theme::accent.withAlpha(0.45f));
        g.fillRect(xOfTick((double)s_.project.loopStart), (float)a.getY(), (float)((s_.project.loopEnd - s_.project.loopStart) * pxPerTick_), (float)a.getHeight());
    }
    g.setFont(theme::font(10.5f));
    const int first = juce::jmax(0, (int)(tickOfX((float)a.getX()) / bar) - 1);
    for (int b = first; xOfTick((double)b * bar) < (float)a.getRight(); ++b) {
        if (b % every != 0) continue;
        const float x = xOfTick((double)b * bar);
        g.setColour(theme::textDim);
        g.drawVerticalLine((int)x, (float)a.getY() + 2.0f, (float)a.getBottom());
        g.setColour(theme::text);
        g.drawText(juce::String(b + 1), (int)x + 3, a.getY(), 40, a.getHeight(), juce::Justification::centredLeft, false);
    }
    for (auto& m : s_.project.markers) {
        const float x = xOfTick((double)m.tick);
        g.setColour(theme::warn);
        g.fillRect(x, (float)a.getY(), 2.0f, (float)a.getHeight());
        g.setFont(theme::font(10.0f, true));
        g.drawText(m.name, (int)x + 4, a.getY(), 140, a.getHeight(), juce::Justification::centredLeft, false);
    }
    const float endX = xOfTick((double)s_.project.songLengthTicks());
    g.setColour(theme::bad.withAlpha(0.8f));
    g.fillRect(endX - 1.0f, (float)a.getBottom() - 6.0f, 3.0f, 6.0f);
    if (playX_ >= (float)a.getX()) {
        juce::Path p;
        p.addTriangle(playX_ - 5.0f, (float)a.getBottom() - 8.0f, playX_ + 5.0f, (float)a.getBottom() - 8.0f, playX_, (float)a.getBottom());
        g.setColour(theme::good);
        g.fillPath(p);
    }
    g.restoreState();
}

void Playlist::paintPicker(juce::Graphics& g) {
    const auto a = pickerArea();
    g.saveState();
    g.reduceClipRegion(a);
    g.fillAll(theme::panel);
    g.setColour(theme::accent2);
    g.setFont(theme::font(10.5f, true));
    g.drawText("PATTERNS", a.getX() + 6, a.getY() + 2, a.getWidth() - 8, 16, juce::Justification::centredLeft, false);
    int y = a.getY() + 20;
    g.setFont(theme::font(11.5f));
    pickerRows_.clear();
    for (int i = 0; i < (int)s_.project.patterns.size() && y < a.getBottom(); ++i) {
        const auto& p = s_.project.patterns[(size_t)i];
        const juce::Rectangle<int> r(a.getX() + 3, y, a.getWidth() - 7, 19);
        pickerRows_.push_back({r, 0, i});
        const bool cur = i == s_.project.currentPattern && drawAudioId_ == 0 && drawAutoId_ == 0;
        g.setColour(juce::Colour(p.color).withMultipliedBrightness(cur ? 1.1f : 0.55f));
        g.fillRoundedRectangle(r.toFloat(), 2.5f);
        g.setColour(cur ? juce::Colours::white : theme::text);
        g.drawText(juce::String(p.name), r.reduced(5, 0), juce::Justification::centredLeft, true);
        y += 21;
    }
    if (!s_.project.audios.empty() && y + 20 < a.getBottom()) {
        y += 4;
        g.setColour(theme::accent2);
        g.setFont(theme::font(10.5f, true));
        g.drawText("AUDIO", a.getX() + 6, y, a.getWidth() - 8, 16, juce::Justification::centredLeft, false);
        y += 18;
        g.setFont(theme::font(11.5f));
        for (auto& au : s_.project.audios) {
            if (y >= a.getBottom()) break;
            const juce::Rectangle<int> r(a.getX() + 3, y, a.getWidth() - 7, 19);
            pickerRows_.push_back({r, 1, au.id});
            g.setColour(juce::Colour(0xff3f8f86).withMultipliedBrightness(au.id == drawAudioId_ ? 1.2f : 0.6f));
            g.fillRoundedRectangle(r.toFloat(), 2.5f);
            g.setColour(au.id == drawAudioId_ ? juce::Colours::white : theme::text);
            g.drawText(juce::String(au.name), r.reduced(5, 0), juce::Justification::centredLeft, true);
            y += 21;
        }
    }
    if (!s_.project.autos.empty() && y + 20 < a.getBottom()) {
        y += 4;
        g.setColour(theme::accent2);
        g.setFont(theme::font(10.5f, true));
        g.drawText("AUTOMATION", a.getX() + 6, y, a.getWidth() - 8, 16, juce::Justification::centredLeft, false);
        y += 18;
        g.setFont(theme::font(11.5f));
        for (auto& au : s_.project.autos) {
            if (y >= a.getBottom()) break;
            const juce::Rectangle<int> r(a.getX() + 3, y, a.getWidth() - 7, 19);
            pickerRows_.push_back({r, 2, au.id});
            g.setColour(juce::Colour(au.color).withMultipliedBrightness(au.id == drawAutoId_ ? 1.2f : 0.6f));
            g.fillRoundedRectangle(r.toFloat(), 2.5f);
            g.setColour(au.id == drawAutoId_ ? juce::Colours::white : theme::text);
            g.drawText(juce::String(au.name), r.reduced(5, 0), juce::Justification::centredLeft, true);
            y += 21;
        }
    }
    g.setColour(theme::line);
    g.drawVerticalLine(a.getRight() - 1, (float)a.getY(), (float)a.getBottom());
    g.restoreState();
}

void Playlist::paintTop(juce::Graphics& g) {
    g.setColour(theme::panel);
    g.fillRect(0, 0, getWidth(), kTopH);
    int x = 4;
    for (int i = 0; i < NumTools; ++i) {
        const juce::Rectangle<int> r(x, 4, 22, kTopH - 8);
        topButtons_[i] = r;
        theme::bevel(g, r.toFloat(), hoverTop_ == i ? theme::panelHi.brighter(0.08f) : theme::panelHi);
        g.setColour(tool_ == i ? theme::accent : theme::text.withAlpha(0.85f));
        g.setFont(theme::font(13.0f, true));
        g.drawText(kTools[i].label, r, juce::Justification::centred, false);
        x += 25;
    }
    g.setColour(theme::textDim);
    g.setFont(theme::font(11.5f));
    const auto* drawAudio = s_.project.audioById(drawAudioId_);
    const auto* drawAuto = s_.project.autoById(drawAutoId_);
    g.drawText("Draws: " + juce::String(drawAuto ? drawAuto->name : (drawAudio ? drawAudio->name : s_.project.pattern().name)) + "      Snap: " + juce::String(Session::snapChoices()[(size_t)s_.snap.getInt()].first)
               + "      " + (s_.project.songMode ? "Song mode: the playlist plays" : "Pattern mode: press L to hear the playlist"),
               x + 10, 0, getWidth() - x - 20, kTopH, juce::Justification::centredLeft, true);
}

juce::String Playlist::getTooltip() {
    const auto p = getMouseXYRelative();
    if (p.y < kTopH) {
        for (int i = 0; i < NumTools; ++i) if (topButtons_[i].contains(p)) return kTools[i].hint;
        return {};
    }
    if (pickerArea().contains(p)) return "Picker: click a pattern, an audio take or an automation curve to choose what the draw tool places; drag a pattern onto a track to place it";
    if (rulerArea().contains(p)) return "Time ruler: click to move the song position, Ctrl-drag to set the loop region, double-click to clear it, right-click for markers";
    if (headArea().contains(p)) {
        const int t = trackAtY(p.y);
        if (s_.project.trackIsGroupHead(t) && p.x < headArea().getX() + 16)
            return s_.project.tracks[(size_t)t].collapsed ? "Show the tracks grouped under this one" : "Hide the tracks grouped under this one (they still play)";
        return "Track " + juce::String(t + 1) + ": click the lamp to mute, right-click the lamp to solo, right-click the name for the track menu";
    }
    if (gridArea().contains(p)) {
        const int bar = s_.project.ticksPerBar();
        const auto tick = (int64)tickOfX((float)p.x);
        int part = 0;
        const int hit = clipAt(p, &part);
        juce::String where = "Track " + juce::String(trackAtY(p.y) + 1) + ", bar " + juce::String(tick / bar + 1) + "   ";
        if (hit >= 0 && s_.project.clips[(size_t)hit].type == Clip::AutomationClip) {
            if (part == 3) return where + "Automation clip menu: rename, make unique, LFO, range, mute, delete";
            int what = 0;
            const int pt = autoHit(s_.project.clips[(size_t)hit], p, what);
            if (pt >= 0 && what == 1) return where + "Automation point: drag to move it, right-click for its shape (curve, hold, stairs, pulse, wave) or to delete it";
            if (pt >= 0 && what == 2) return where + "Tension: drag up or down to bend this stretch (for stairs, pulse and wave it sets how many)";
            if ((float)p.y > clipRect(s_.project.clips[(size_t)hit]).getY() + 12.0f) return where + "Automation curve: right-click to add a point, drag a point to move it, drag the title strip to move the clip";
        }
        if (hit >= 0 && part == 3 && s_.project.clips[(size_t)hit].type == Clip::AudioClip)
            return where + "Audio clip menu: gain, fades, reverse, stretch to the tempo, pitch, takes, mixer track";
        if (hit >= 0 && part == 3) return where + "Clip menu: make unique, mute, rename, colour, group";
        return where + kTools[tool_].hint;
    }
    return {};
}

// ------------------------------------------------------------------ editing helpers

void Playlist::selectAll(bool on) { for (auto& c : s_.project.clips) c.selected = on && !s_.project.trackHidden(c.track); repaint(); }     // not what a collapsed group hides

void Playlist::selectGroupOf(int clipIndex) {
    const int g = s_.project.clips[(size_t)clipIndex].group;
    s_.project.clips[(size_t)clipIndex].selected = true;
    if (g != 0) for (auto& c : s_.project.clips) if (c.group == g) c.selected = true;
}

void Playlist::deleteSelected() {
    auto& clips = s_.project.clips;
    const auto n = clips.size();
    clips.erase(std::remove_if(clips.begin(), clips.end(), [&](const Clip& c) { return c.selected && !locked(c.track); }), clips.end());
    if (clips.size() != n) edited("Delete clips");
}

void Playlist::duplicateSelected() {
    auto& clips = s_.project.clips;
    int64 lo = INT64_MAX, hi = 0;
    bool any = false;
    for (auto& c : clips) any = any || c.selected;
    for (auto& c : clips) if (!any || c.selected) { lo = std::min(lo, c.start); hi = std::max(hi, c.end()); }
    if (lo == INT64_MAX) return;
    const int bar = s_.project.ticksPerBar();
    const int64 span = ((hi - lo + bar - 1) / bar) * bar;       // whole bars, so repeats stay aligned
    std::vector<Clip> copies;
    int maxGroup = 0;
    for (auto& c : clips) maxGroup = std::max(maxGroup, c.group);
    for (auto& c : clips) {
        if (any && !c.selected) continue;
        Clip d = c;
        d.start += span;
        d.selected = true;
        if (d.group != 0) d.group += maxGroup;
        copies.push_back(d);
        c.selected = false;
    }
    clips.insert(clips.end(), copies.begin(), copies.end());
    edited("Duplicate clips");
}

void Playlist::groupSelected(bool group) {
    int maxGroup = 0;
    for (auto& c : s_.project.clips) maxGroup = std::max(maxGroup, c.group);
    for (auto& c : s_.project.clips) if (c.selected) c.group = group ? maxGroup + 1 : 0;
    edited(group ? "Group clips" : "Ungroup clips");
}

// ------------------------------------------------------------------ mouse

void Playlist::mouseMove(const juce::MouseEvent& e) {
    int h = -1;
    if (e.y < kTopH) for (int i = 0; i < NumTools; ++i) if (topButtons_[i].contains(e.getPosition())) h = i;
    if (h != hoverTop_) { hoverTop_ = h; repaint(0, 0, getWidth(), kTopH); }
    int part = 0;
    const bool edge = gridArea().contains(e.getPosition()) && (tool_ == Draw || tool_ == Paint) && clipAt(e.getPosition(), &part) >= 0 && (part == 1 || part == 2);
    setMouseCursor(edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void Playlist::mouseDown(const juce::MouseEvent& e) {
    grabKeyboardFocus();
    const auto p = e.getPosition();
    downPos_ = lastPos_ = p;
    gesture_ = None;
    changed_ = false;
    touched_.clear();
    anchor_ = -1;
    auto& clips = s_.project.clips;

    if (p.y < kTopH) { for (int i = 0; i < NumTools; ++i) if (topButtons_[i].contains(p)) setTool(i); return; }
    if (pickerArea().contains(p)) {
        for (auto& row : pickerRows_) {
            if (!row.area.contains(p)) continue;
            drawAudioId_ = drawAutoId_ = 0;
            if (row.kind == 0) { s_.selectPattern(row.id); pickerDragPattern_ = row.id; gesture_ = PickerDrag; }
            else if (row.kind == 1) drawAudioId_ = row.id;
            else drawAutoId_ = row.id;
            repaint();
            break;
        }
        return;
    }
    if (headArea().contains(p)) {
        const int t = trackAtY(p.y);
        auto& tr = s_.project.tracks[(size_t)t];
        if (p.x >= headArea().getRight() - 20) {
            if (e.mods.isPopupMenu()) {
                // solo: only this track sounds; again to release
                const bool was = tr.solo;
                for (auto& o : s_.project.tracks) o.solo = false;
                tr.solo = !was;
            } else tr.mute = !tr.mute;
            edited("Track mute");
        } else if (e.mods.isPopupMenu()) trackMenu(t);
        else if (s_.project.trackIsGroupHead(t) && p.x < headArea().getX() + 16) setCollapsed(t, !tr.collapsed);
        return;
    }
    if (rulerArea().contains(p)) {
        const int64 tick = snapTick(tickOfX((float)p.x), e.mods.isAltDown(), false);
        if (e.mods.isPopupMenu()) { rulerMenu(tick); return; }
        if (e.mods.isCtrlDown()) { gesture_ = RulerLoop; downTick_ = tick; return; }
        gesture_ = RulerSeek;
        if (s_.project.songMode) s_.seek((double)tick);
        return;
    }
    if (!gridArea().contains(p)) return;

    const bool alt = e.mods.isAltDown();
    const int track = trackAtY(p.y);
    downTrack_ = track;
    downTick_ = (int64)tickOfX((float)p.x);
    int part = 0;
    const int hit = clipAt(p, &part);

    if (e.mods.isMiddleButtonDown()) { gesture_ = Panning; return; }
    // inside an automation clip's curve: points and tension handles are dragged, a right-click adds a point
    if (hit >= 0 && clips[(size_t)hit].type == Clip::AutomationClip && (tool_ == Draw || tool_ == Paint) && !e.mods.isCtrlDown()
        && !locked(clips[(size_t)hit].track) && (float)p.y > clipRect(clips[(size_t)hit]).getY() + 12.0f) {
        auto& clip = clips[(size_t)hit];
        if (auto* a = s_.project.autoById(clip.autoId)) {
            int what = 0;
            const int idx = autoHit(clip, p, what);
            if (e.mods.isPopupMenu()) {
                if (idx >= 0 && what == 1) { autoPointMenu(hit, idx); return; }
                if (!a->lfo.on) {
                    const auto body = autoBody(clipRect(clip));
                    pulp::AutoPoint pt;
                    pt.tick = juce::jmax<int64>(0, clip.offset + (int64)snapTick(tickOfX((float)p.x), alt, false) - clip.start);
                    pt.value = juce::jlimit(0.0f, 1.0f, ((body.getBottom() - (float)p.y) / body.getHeight() - a->min) / juce::jmax(0.001f, a->max - a->min));
                    bool exists = false;
                    for (auto& q : a->points) exists = exists || q.tick == pt.tick;
                    if (!exists) {
                        a->points.push_back(pt);
                        std::stable_sort(a->points.begin(), a->points.end(), [](const pulp::AutoPoint& x, const pulp::AutoPoint& y) { return x.tick < y.tick; });
                        edited("Add automation point");
                    }
                }
                return;
            }
            if (idx >= 0) {
                for (auto& c : clips) c.selected = false;
                clip.selected = true;
                gesture_ = what == 1 ? AutoPointDrag : AutoTensionDrag;
                anchor_ = hit;
                autoIndex_ = idx;
                autoBefore_ = a->points;
                repaint();
                return;
            }
        }
    }
    if (e.mods.isPopupMenu()) {
        if (e.mods.isCtrlDown()) { gesture_ = RightZoom; zoomStartPx_ = pxPerTick_; return; }
        if (tool_ == Zoom) { zoomX(1.0 / 1.6, (float)p.x); return; }
        gesture_ = Erase;
        if (hit >= 0 && canErase(hit, p)) { clips.erase(clips.begin() + hit); changed_ = true; s_.publish(); repaint(); }
        return;
    }
    switch (tool_) {
        case Draw:
        case Paint: {
            if (e.mods.isCtrlDown()) { gesture_ = Marquee; marquee_ = {p.x, p.y, 0, 0}; return; }
            if (hit >= 0) {
                if (part == 3) {
                    if (clips[(size_t)hit].type == Clip::AudioClip) audioClipMenu(hit);
                    else if (clips[(size_t)hit].type == Clip::AutomationClip) autoClipMenu(hit);
                    else clipMenu(hit);
                    return;
                }
                if (locked(clips[(size_t)hit].track)) return;
                if (!clips[(size_t)hit].selected) { for (auto& c : clips) c.selected = false; selectGroupOf(hit); }
                anchor_ = hit;
                if (e.mods.isShiftDown() && part == 0) {
                    std::vector<Clip> copies;
                    for (auto& c : clips) if (c.selected) { copies.push_back(c); c.selected = false; }
                    const Clip ref = clips[(size_t)hit];
                    const size_t base = clips.size();
                    clips.insert(clips.end(), copies.begin(), copies.end());
                    for (size_t i = base; i < clips.size(); ++i) if (clips[i].start == ref.start && clips[i].track == ref.track) { anchor_ = (int)i; break; }
                    changed_ = true;
                }
                gesture_ = part == 1 ? ResizeL : (part == 2 ? ResizeR : Move);
                before_ = clips;
            } else {
                if (locked(track)) return;
                for (auto& c : clips) c.selected = false;
                if (drawAudioId_ != 0 && !s_.project.audioById(drawAudioId_)) drawAudioId_ = 0;
                if (drawAutoId_ != 0 && !s_.project.autoById(drawAutoId_)) drawAutoId_ = 0;
                if (drawAutoId_ != 0) {
                    // another clip of a curve that already exists
                    Clip n;
                    n.type = Clip::AutomationClip;
                    n.track = track;
                    n.start = snapTick(tickOfX((float)p.x), alt);
                    n.autoId = drawAutoId_;
                    n.length = juce::jmax<int64>(kTicksPerStep, s_.project.autoById(drawAutoId_)->length());
                    clips.push_back(n);
                }
                auto& c = drawAutoId_ != 0 ? clips.back()
                        : (drawAudioId_ != 0 ? s_.project.addAudioClip(drawAudioId_, track, snapTick(tickOfX((float)p.x), alt))
                                             : s_.project.addPatternClip(s_.project.currentPattern, track, snapTick(tickOfX((float)p.x), alt)));
                c.selected = true;
                anchor_ = (int)clips.size() - 1;
                changed_ = true;
                before_ = clips;
                gesture_ = tool_ == Paint ? Painting : Move;
                s_.publish();
            }
            repaint();
            break;
        }
        case Delete:
            gesture_ = Erase;
            if (hit >= 0 && canErase(hit, p)) { clips.erase(clips.begin() + hit); changed_ = true; s_.publish(); repaint(); }
            break;
        case Mute:
            gesture_ = Muting;
            if (hit >= 0) { clips[(size_t)hit].muted = !clips[(size_t)hit].muted; touched_.push_back(hit); changed_ = true; s_.publish(); repaint(); }
            break;
        case Slip:
            if (hit >= 0 && !locked(clips[(size_t)hit].track)) { gesture_ = Slipping; anchor_ = hit; before_ = clips; }
            break;
        case Slice: gesture_ = Slicing; break;
        case Select:
            if (hit >= 0) {
                if (!e.mods.isShiftDown() && !e.mods.isCtrlDown()) for (auto& c : clips) c.selected = false;
                selectGroupOf(hit);
                repaint();
            } else { gesture_ = Marquee; marquee_ = {p.x, p.y, 0, 0}; }
            break;
        case Zoom: gesture_ = Zooming; marquee_ = {p.x, p.y, 0, 0}; break;
        case Scrub:
            gesture_ = Scrubbing;
            wasPlaying_ = s_.engine.isPlaying();
            if (!s_.project.songMode) s_.setSongMode(true);
            s_.seek((double)snapTick(tickOfX((float)p.x), alt));
            s_.engine.play();
            break;
        default: break;
    }
}

void Playlist::mouseDrag(const juce::MouseEvent& e) {
    auto& clips = s_.project.clips;
    const auto p = e.getPosition();
    const bool alt = e.mods.isAltDown();
    const auto grid = gridArea();
    switch (gesture_) {
        case PickerDrag:
            if (std::abs(p.x - downPos_.x) + std::abs(p.y - downPos_.y) > 6)
                if (auto* dc = juce::DragAndDropContainer::findParentDragContainerFor(this)) {
                    gesture_ = None;
                    dc->startDragging("pattern:" + juce::String(pickerDragPattern_), this);
                }
            break;
        case RulerSeek: if (s_.project.songMode) s_.seek((double)snapTick(tickOfX((float)p.x), alt, false)); break;
        case RulerLoop: {
            const int64 tick = snapTick(tickOfX((float)p.x), alt, false);
            s_.project.loopStart = std::min(downTick_, tick);
            s_.project.loopEnd = std::max(downTick_, tick);
            s_.publish();
            repaint();
            break;
        }
        case Panning:
            scrollTick_ -= (p.x - lastPos_.x) / pxPerTick_;
            scrollY_ -= p.y - lastPos_.y;
            clampScroll(); updateScrollbars(); repaint();
            break;
        case RightZoom: {
            const double anchor = tickOfX((float)downPos_.x);
            pxPerTick_ = juce::jlimit(0.0008, 0.5, zoomStartPx_ * std::pow(2.0, (p.x - downPos_.x) / 120.0));
            scrollTick_ = anchor - (double)(downPos_.x - grid.getX()) / pxPerTick_;
            clampScroll(); updateScrollbars(); repaint();
            break;
        }
        case Move: {
            if (anchor_ < 0 || before_.size() != clips.size()) break;
            const Clip& a = before_[(size_t)anchor_];
            int64 dTick = (int64)snapTick((double)a.start + (tickOfX((float)p.x) - tickOfX((float)downPos_.x)), alt, false) - a.start;
            int dTrack = trackAtY(p.y) - downTrack_;
            for (size_t i = 0; i < clips.size(); ++i)
                if (before_[i].selected) {
                    dTick = std::max(dTick, -before_[i].start);
                    dTrack = juce::jlimit(-before_[i].track, kNumPlaylistTracks - 1 - before_[i].track, dTrack);
                }
            // no clip that can be seen is moved onto a track of a collapsed group
            auto landsHidden = [&](int d) {
                for (size_t i = 0; i < clips.size(); ++i)
                    if (before_[i].selected && !s_.project.trackHidden(before_[i].track) && s_.project.trackHidden(before_[i].track + d)) return true;
                return false;
            };
            while (dTrack != 0 && landsHidden(dTrack)) dTrack += dTrack > 0 ? -1 : 1;
            for (size_t i = 0; i < clips.size(); ++i)
                if (before_[i].selected && !locked(before_[i].track + dTrack)) { clips[i].start = before_[i].start + dTick; clips[i].track = before_[i].track + dTrack; }
            if (dTick != 0 || dTrack != 0) changed_ = true;
            s_.publish();
            repaint();
            break;
        }
        case ResizeR: {
            if (anchor_ < 0 || before_.size() != clips.size()) break;
            const Clip& a = before_[(size_t)anchor_];
            const int64 minLen = snap(alt);
            const int64 newEnd = snapTick((double)a.end() + (tickOfX((float)p.x) - tickOfX((float)downPos_.x)), alt, false);
            const int64 dLen = std::max(minLen, newEnd - a.start) - a.length;
            for (size_t i = 0; i < clips.size(); ++i) if (before_[i].selected) clips[i].length = std::max(minLen, before_[i].length + dLen);
            if (dLen != 0) changed_ = true;
            s_.publish();
            repaint();
            break;
        }
        case ResizeL: {
            if (anchor_ < 0 || before_.size() != clips.size()) break;
            const Clip& a = before_[(size_t)anchor_];
            const int64 minLen = snap(alt);
            int64 d = (int64)snapTick((double)a.start + (tickOfX((float)p.x) - tickOfX((float)downPos_.x)), alt, false) - a.start;
            for (size_t i = 0; i < clips.size(); ++i)
                if (before_[i].selected) d = juce::jlimit(-std::min(before_[i].offset, before_[i].start), before_[i].length - minLen, d);
            // the left edge moves; what plays at each moment stays the same
            for (size_t i = 0; i < clips.size(); ++i)
                if (before_[i].selected) { clips[i].start = before_[i].start + d; clips[i].offset = before_[i].offset + d; clips[i].length = before_[i].length - d; }
            if (d != 0) changed_ = true;
            s_.publish();
            repaint();
            break;
        }
        case Slipping: {
            if (anchor_ < 0 || before_.size() != clips.size()) break;
            const Clip& a = before_[(size_t)anchor_];
            const int64 d = (int64)snapTick(std::fabs(tickOfX((float)downPos_.x) - tickOfX((float)p.x)), alt, false) * (p.x < downPos_.x ? 1 : -1);
            clips[(size_t)anchor_].offset = std::max<int64>(0, a.offset + d);
            changed_ = clips[(size_t)anchor_].offset != a.offset || changed_;
            s_.publish();
            repaint();
            break;
        }
        case Painting: {
            if (!grid.contains(p)) break;
            const int track = trackAtY(p.y);
            if (locked(track)) break;
            const int64 len = s_.project.patternLengthTicks(s_.project.pattern());
            const int64 cell = ((int64)snapTick(tickOfX((float)p.x), alt) / len) * len;
            bool exists = false;
            for (auto& c : clips) if (c.track == track && c.start < cell + len && c.end() > cell) { exists = true; break; }
            if (!exists) { s_.project.addPatternClip(s_.project.currentPattern, track, cell).selected = true; changed_ = true; s_.publish(); repaint(); }
            break;
        }
        case Erase: {
            const int hit = clipAt(p);
            if (hit >= 0 && canErase(hit, p)) { clips.erase(clips.begin() + hit); changed_ = true; s_.publish(); repaint(); }
            break;
        }
        case AutoPointDrag: {
            if (anchor_ < 0 || anchor_ >= (int)clips.size()) break;
            auto& clip = clips[(size_t)anchor_];
            auto* a = s_.project.autoById(clip.autoId);
            if (!a || autoIndex_ < 0 || autoIndex_ >= (int)a->points.size()) break;
            const auto body = autoBody(clipRect(clip));
            auto& pts = a->points;
            const size_t i = (size_t)autoIndex_;
            if (i > 0) {        // the first point keeps its time; the others move between their neighbours
                int64 tick = clip.offset + (int64)snapTick(tickOfX((float)p.x), alt, false) - clip.start;
                const int64 lo = pts[i - 1].tick + 1, hi = i + 1 < pts.size() ? pts[i + 1].tick - 1 : INT64_MAX / 4;
                pts[i].tick = juce::jlimit(lo, juce::jmax(lo, hi), tick);
            }
            pts[i].value = juce::jlimit(0.0f, 1.0f, ((body.getBottom() - (float)p.y) / body.getHeight() - a->min) / juce::jmax(0.001f, a->max - a->min));
            // dragging the last point past the end of the clip makes the clip longer
            if (i + 1 == pts.size() && pts[i].tick - clip.offset > clip.length) clip.length = pts[i].tick - clip.offset;
            changed_ = true;
            s_.publish();
            repaint();
            break;
        }
        case AutoTensionDrag: {
            if (anchor_ < 0 || anchor_ >= (int)clips.size()) break;
            auto* a = s_.project.autoById(clips[(size_t)anchor_].autoId);
            if (!a || autoIndex_ < 0 || autoIndex_ >= (int)a->points.size() || autoIndex_ >= (int)autoBefore_.size()) break;
            const float rising = autoIndex_ + 1 < (int)a->points.size() && a->points[(size_t)autoIndex_ + 1].value < a->points[(size_t)autoIndex_].value ? -1.0f : 1.0f;
            // dragging up always pushes the middle of the curve up
            a->points[(size_t)autoIndex_].tension = juce::jlimit(-1.0f, 1.0f, autoBefore_[(size_t)autoIndex_].tension - rising * (float)(downPos_.y - p.y) / 70.0f);
            changed_ = true;
            s_.publish();
            repaint();
            break;
        }
        case Muting: {
            const int hit = clipAt(p);
            if (hit >= 0 && std::find(touched_.begin(), touched_.end(), hit) == touched_.end()) {
                clips[(size_t)hit].muted = !clips[(size_t)hit].muted;
                touched_.push_back(hit);
                changed_ = true; s_.publish(); repaint();
            }
            break;
        }
        case Marquee: case Zooming: marquee_ = juce::Rectangle<int>(downPos_, p).getIntersection(grid); repaint(); break;
        case Slicing: repaint(); break;
        case Scrubbing: s_.seek((double)snapTick(tickOfX((float)p.x), alt)); break;
        default: break;
    }
    lastPos_ = p;
}

void Playlist::mouseUp(const juce::MouseEvent& e) {
    auto& clips = s_.project.clips;
    const int g = gesture_;
    gesture_ = None;
    switch (g) {
        case Move: case Painting: if (changed_) edited(g == Painting ? "Paint clips" : "Move clips"); break;
        case ResizeL: case ResizeR: if (changed_) edited("Resize clips"); break;
        case Slipping: if (changed_) edited("Slip clip"); break;
        case AutoPointDrag: if (changed_) edited("Move automation point"); break;
        case AutoTensionDrag: if (changed_) edited("Bend automation curve"); break;
        case Erase: if (changed_) edited("Delete clips"); break;
        case Muting: if (changed_) edited("Mute clips"); break;
        case RulerLoop:
            if (s_.project.loopEnd - s_.project.loopStart < 60) s_.project.loopStart = s_.project.loopEnd = 0;
            edited("Loop region");
            break;
        case Marquee: {
            const bool add = e.mods.isShiftDown();
            std::vector<int> hits;
            for (int i = 0; i < (int)clips.size(); ++i) {
                const bool in = clipRect(clips[(size_t)i]).intersects(marquee_.toFloat());
                clips[(size_t)i].selected = add ? (clips[(size_t)i].selected || in) : in;
                if (in) hits.push_back(i);
            }
            for (int i : hits) selectGroupOf(i);
            repaint();
            break;
        }
        case Zooming:
            if (marquee_.getWidth() > 8) {
                const double t0 = tickOfX((float)marquee_.getX()), t1 = tickOfX((float)marquee_.getRight());
                pxPerTick_ = juce::jlimit(0.0008, 0.5, (double)gridArea().getWidth() / juce::jmax(1.0, t1 - t0));
                scrollTick_ = t0;
                clampScroll(); updateScrollbars();
            } else zoomX(1.6, (float)downPos_.x);
            repaint();
            break;
        case Slicing: {
            std::vector<Clip> add;
            const float x0 = (float)downPos_.x, y0 = (float)downPos_.y, x1 = (float)e.x, y1 = (float)e.y;
            for (auto& c : clips) {
                if (locked(c.track) || s_.project.trackHidden(c.track)) continue;
                const auto r = clipRect(c);
                const float cy = r.getCentreY();
                if ((cy - y0) * (cy - y1) > 0.0f) continue;
                const float x = std::fabs(y1 - y0) < 0.5f ? x0 : x0 + (x1 - x0) * (cy - y0) / (y1 - y0);
                const int64 cut = snapTick(tickOfX(x), e.mods.isAltDown(), false);
                if (cut <= c.start || cut >= c.end()) continue;
                Clip tail = c;
                tail.start = cut;
                tail.offset = c.offset + (cut - c.start);
                tail.length = c.end() - cut;
                c.length = cut - c.start;
                add.push_back(tail);
            }
            if (!add.empty()) { clips.insert(clips.end(), add.begin(), add.end()); edited("Slice clips"); }
            repaint();
            break;
        }
        case Scrubbing: if (!wasPlaying_) s_.engine.pause(); break;
        default: break;
    }
}

void Playlist::mouseDoubleClick(const juce::MouseEvent& e) {
    const auto p = e.getPosition();
    if (rulerArea().contains(p)) { s_.project.loopStart = s_.project.loopEnd = 0; edited("Loop region"); return; }
    if (headArea().contains(p) && p.x < headArea().getRight() - 20) {
        const int t = trackAtY(p.y);
        askForText("Rename track", "Track name", s_.project.tracks[(size_t)t].name, this, [this, t](juce::String n) { s_.project.tracks[(size_t)t].name = n.trim().toStdString(); edited("Rename track"); });
        return;
    }
    const int hit = clipAt(p);
    if (hit >= 0 && gridArea().contains(p) && s_.project.clips[(size_t)hit].type == Clip::AutomationClip) return;
    if (hit >= 0 && gridArea().contains(p) && s_.project.clips[(size_t)hit].type == Clip::PatternClip) {
        const int pi = s_.project.patternIndex(s_.project.clips[(size_t)hit].patternId);
        if (pi >= 0) { s_.selectPattern(pi); if (onOpenPattern) onOpenPattern(pi); }
    }
}

void Playlist::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    const float d = std::fabs(w.deltaY) > std::fabs(w.deltaX) ? w.deltaY : w.deltaX;
    if (e.mods.isCtrlDown()) zoomX(d > 0 ? 1.25 : 0.8, (float)e.x);
    else if (e.mods.isAltDown()) { baseTrackH_ = juce::jlimit(16, 90, baseTrackH_ + (d > 0 ? 4 : -4)); rebuildTrackTops(); clampScroll(); updateScrollbars(); repaint(); }
    else if (e.mods.isShiftDown() || std::fabs(w.deltaX) > std::fabs(w.deltaY)) { scrollTick_ -= d * 480.0 / pxPerTick_ * 0.25; clampScroll(); updateScrollbars(); repaint(); }
    else { scrollY_ -= (int)(d * 120.0f); clampScroll(); updateScrollbars(); repaint(); }
}

void Playlist::placeAudioFile(const juce::File& f, juce::Point<int> where) {
    if (!Session::isAudioFile(f) || !gridArea().contains(where)) return;
    const int track = trackAtY(where.y);
    if (locked(track)) return;
    s_.addAudioFile(f, track, snapTick(tickOfX((float)where.x), false));
}

bool Playlist::isInterestedInFileDrag(const juce::StringArray& files) {
    for (auto& f : files) if (Session::isAudioFile(juce::File(f))) return true;
    return false;
}

void Playlist::filesDropped(const juce::StringArray& files, int x, int y) {
    int row = 0;
    for (auto& f : files) placeAudioFile(juce::File(f), {x, y + (row++) * trackHeight(trackAtY(y))});
}

void Playlist::audioClipMenu(int clipIndex) {
    auto& clips = s_.project.clips;
    if (clipIndex < 0 || clipIndex >= (int)clips.size()) return;
    for (auto& c : clips) c.selected = false;
    selectGroupOf(clipIndex);
    repaint();
    const auto& c = clips[(size_t)clipIndex];
    const auto* a = s_.project.audioById(c.audioId);
    juce::PopupMenu m;
    m.addSectionHeader(a ? juce::String(a->name) : juce::String("Audio clip"));
    m.addItem(1, "Gain...   (now " + juce::String(pulp::gainToDb(c.gain), 1) + " dB)");
    m.addItem(2, "Normalize");
    m.addItem(3, "Fade in...   (now " + juce::String(c.fadeIn / (double)kTicksPerStep, 2) + " steps)");
    m.addItem(4, "Fade out...   (now " + juce::String(c.fadeOut / (double)kTicksPerStep, 2) + " steps)");
    m.addItem(5, "Reverse", true, c.reverse);
    m.addItem(6, "Mixer track...   (now " + juce::String(pulp::Project::mixerTrackName(c.mixerTrack)) + ")");
    m.addItem(7, c.muted ? "Unmute" : "Mute");
    if (c.takeGroup != 0) m.addItem(30, "Use this take here  (take " + juce::String(c.take) + ": it plays, the other takes are cut and silenced over this stretch)");
    {
        // stretching: the three algorithms, following the tempo, length and pitch
        juce::PopupMenu st;
        for (int i = 0; i < (int)pulp::stretch::NumModes; ++i) st.addItem(20 + i, pulp::stretch::modeName(i), true, c.stretchMode == i);
        st.addSeparator();
        const bool known = a && a->bpm > 0.0;
        st.addItem(23, known ? "Follow the project tempo" : "Follow the project tempo  (set the audio's tempo first)", known, c.sync);
        st.addItem(24, "Tempo of this audio...   (" + (known ? juce::String(a->bpm, 2) + " BPM" : juce::String("not known")) + ")");
        st.addItem(25, "Detect the tempo by listening");
        st.addSeparator();
        st.addItem(26, "Length...   (now " + juce::String(s_.project.clipLengthRatio(c) * 100.0, 1) + " % of the original)", !c.sync);
        st.addItem(27, "Pitch...   (now " + juce::String(c.pitchCents / 100.0, 2) + " semitones)");
        st.addItem(28, "Back to the original speed and pitch");
        m.addSubMenu("Stretch and pitch", st);
    }
    m.addSeparator();
    m.addItem(8, "Group the selected clips  (Shift+G)");
    m.addItem(9, "Ungroup  (Alt+G)", c.group != 0);
    m.addItem(10, "Delete");
    m.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<Playlist>(this), clipIndex](int r) {
        if (!safe || r <= 0) return;
        auto& cl = safe->s_.project.clips;
        if (clipIndex >= (int)cl.size()) return;
        auto ask = [&](const juce::String& title, const juce::String& what, double now, std::function<void(pulp::Clip&, double)> apply) {
            askForText(title, what, juce::String(now, 2), safe.getComponent(), [safe, clipIndex, apply](juce::String t) {
                if (!safe || clipIndex >= (int)safe->s_.project.clips.size()) return;
                apply(safe->s_.project.clips[(size_t)clipIndex], t.getDoubleValue());
                safe->edited("Audio clip setting");
            });
        };
        auto& c = cl[(size_t)clipIndex];
        if (r == 1) ask("Clip gain", "Gain in dB (0 leaves it as recorded)", pulp::gainToDb(c.gain), [](pulp::Clip& k, double v) { k.gain = pulp::dbToGain((float)juce::jlimit(-60.0, 24.0, v)); });
        else if (r == 2) { if (auto* a = safe->s_.project.audioById(c.audioId); a && a->valid() && a->peak() > 1.0e-6f) { c.gain = 0.98f / a->peak(); safe->edited("Normalize clip"); } }
        else if (r == 3) ask("Fade in", "Length in steps", c.fadeIn / (double)kTicksPerStep, [](pulp::Clip& k, double v) { k.fadeIn = (int64)(juce::jlimit(0.0, 256.0, v) * kTicksPerStep); });
        else if (r == 4) ask("Fade out", "Length in steps", c.fadeOut / (double)kTicksPerStep, [](pulp::Clip& k, double v) { k.fadeOut = (int64)(juce::jlimit(0.0, 256.0, v) * kTicksPerStep); });
        else if (r == 5) { c.reverse = !c.reverse; safe->edited("Reverse clip"); }
        else if (r == 6) ask("Mixer track", "0 is the master, 1 to 125 are the inserts", c.mixerTrack, [](pulp::Clip& k, double v) { k.mixerTrack = juce::jlimit(0, pulp::kNumInserts, (int)v); });
        else if (r == 7) { c.muted = !c.muted; safe->edited("Mute clip"); }
        else if (r == 30) { safe->s_.project.useTake(clipIndex); safe->edited("Use take"); }
        else if (r >= 20 && r <= 22) { safe->s_.project.setClipStretch(clipIndex, r - 20, c.sync, c.timeMul, c.pitchCents); safe->edited("Stretch mode"); }
        else if (r == 23) {
            // a clip that starts following the tempo is usually a loop: the rhythmic stretch suits it unless another was chosen
            const int mode = !c.sync && c.stretchMode == pulp::stretch::Resample && c.timeMul == 1.0f && c.pitchCents == 0.0f ? (int)pulp::stretch::Rhythmic : c.stretchMode;
            safe->s_.project.setClipStretch(clipIndex, mode, !c.sync, c.timeMul, c.pitchCents);
            safe->edited("Follow the tempo");
        }
        else if (r == 24) {
            const int audioId = c.audioId;
            const auto* item = safe->s_.project.audioById(audioId);
            askForText("Tempo of this audio", "Beats per minute (0 = not known)", juce::String(item ? item->bpm : 0.0, 2), safe.getComponent(), [safe, audioId](juce::String t) {
                if (!safe) return;
                for (auto& au : safe->s_.project.audios) if (au.id == audioId) au.bpm = juce::jlimit(0.0, 999.0, t.getDoubleValue());
                safe->edited("Audio tempo");
            });
        }
        else if (r == 25) {
            const double bpm = safe->s_.project.detectAudioTempo(c.audioId);
            safe->edited("Detect tempo");
            if (safe->s_.onMessage) safe->s_.onMessage(bpm > 0.0 ? "This audio sounds like " + juce::String(bpm, 2) + " BPM." : juce::String("No steady beat was found in this audio."));
        }
        else if (r == 26) ask("Clip length", "Percent of the original length (100 leaves it alone, 200 is twice as long)", c.timeMul * 100.0, [safe, clipIndex](pulp::Clip& k, double v) {
            safe->s_.project.setClipStretch(clipIndex, k.stretchMode, false, (float)(juce::jlimit(6.25, 1600.0, v) / 100.0), k.pitchCents);
        });
        else if (r == 27) ask("Clip pitch", "Semitones up or down (in resample mode this changes the length too)", c.pitchCents / 100.0, [safe, clipIndex](pulp::Clip& k, double v) {
            safe->s_.project.setClipStretch(clipIndex, k.stretchMode, k.sync, k.timeMul, (float)(juce::jlimit(-24.0, 24.0, v) * 100.0));
        });
        else if (r == 28) { safe->s_.project.setClipStretch(clipIndex, c.stretchMode, false, 1.0f, 0.0f); safe->edited("Reset stretch"); }
        else if (r == 8) safe->groupSelected(true);
        else if (r == 9) safe->groupSelected(false);
        else if (r == 10) safe->deleteSelected();
    });
}

void Playlist::autoPointMenu(int clipIndex, int pointIndex) {
    auto& clips = s_.project.clips;
    if (clipIndex < 0 || clipIndex >= (int)clips.size()) return;
    const int autoId = clips[(size_t)clipIndex].autoId;
    const auto* a = s_.project.autoById(autoId);
    if (!a || pointIndex < 0 || pointIndex >= (int)a->points.size()) return;
    const auto& pt = a->points[(size_t)pointIndex];
    juce::PopupMenu m;
    m.addSectionHeader("Automation point");
    m.addItem(1, "Delete this point", a->points.size() > 2);
    m.addItem(2, "Type in value...   (now " + juce::String(pt.value * 100.0f, 1) + " %)");
    m.addItem(3, "Straighten (tension to zero)", pt.tension != 0.0f);
    m.addSectionHeader("Shape of the stretch after this point");
    for (int i = 0; i < pulp::NumAutoShapes; ++i) m.addItem(10 + i, pulp::autoShapeName(i), pointIndex + 1 < (int)a->points.size(), pt.shape == i);
    m.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<Playlist>(this), autoId, pointIndex](int r) {
        if (!safe || r <= 0) return;
        auto* au = safe->s_.project.autoById(autoId);
        if (!au || pointIndex >= (int)au->points.size()) return;
        if (r == 1 && au->points.size() > 2) { au->points.erase(au->points.begin() + pointIndex); safe->edited("Delete automation point"); }
        else if (r == 2) askForText("Automation point", "Value in percent of the control's travel", juce::String(au->points[(size_t)pointIndex].value * 100.0f, 1), safe.getComponent(), [safe, autoId, pointIndex](juce::String t) {
            if (!safe) return;
            auto* a2 = safe->s_.project.autoById(autoId);
            if (!a2 || pointIndex >= (int)a2->points.size()) return;
            a2->points[(size_t)pointIndex].value = juce::jlimit(0.0f, 1.0f, t.getFloatValue() / 100.0f);
            safe->edited("Automation point value");
        });
        else if (r == 3) { au->points[(size_t)pointIndex].tension = 0.0f; safe->edited("Straighten automation"); }
        else if (r >= 10 && r < 10 + pulp::NumAutoShapes) { au->points[(size_t)pointIndex].shape = r - 10; safe->edited("Automation shape"); }
    });
}

void Playlist::autoClipMenu(int clipIndex) {
    auto& clips = s_.project.clips;
    if (clipIndex < 0 || clipIndex >= (int)clips.size()) return;
    for (auto& c : clips) c.selected = false;
    selectGroupOf(clipIndex);
    repaint();
    const int autoId = clips[(size_t)clipIndex].autoId;
    const auto* a = s_.project.autoById(autoId);
    if (!a) return;
    juce::PopupMenu m, lfo;
    m.addSectionHeader(juce::String(a->name));
    m.addItem(50, "Controls: " + juce::String(s_.project.paramLabel(a->target)), false);
    m.addItem(1, "Rename...");
    m.addItem(2, "Make unique  (give this clip its own copy of the curve)");
    lfo.addItem(20, "LFO on  (the curve becomes the centre line the LFO swings around)", true, a->lfo.on);
    const char* shapes[4] = {"Sine", "Triangle", "Square", "Saw"};
    for (int i = 0; i < 4; ++i) lfo.addItem(21 + i, shapes[i], true, a->lfo.shape == i);
    lfo.addItem(25, "Cycle length...   (now " + juce::String(a->lfo.periodSteps, 2) + " steps)");
    lfo.addItem(26, "Depth...   (now " + juce::String(a->lfo.depth * 100.0f, 0) + " %)");
    m.addSubMenu("LFO", lfo);
    m.addItem(3, "Range: lowest position...   (now " + juce::String(a->min * 100.0f, 0) + " %)");
    m.addItem(4, "Range: highest position...   (now " + juce::String(a->max * 100.0f, 0) + " %)");
    m.addSeparator();
    m.addItem(5, clips[(size_t)clipIndex].muted ? "Unmute" : "Mute");
    m.addItem(6, "Delete");
    m.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<Playlist>(this), clipIndex, autoId](int r) {
        if (!safe || r <= 0) return;
        auto& s = safe->s_;
        auto* au = s.project.autoById(autoId);
        if (!au || clipIndex >= (int)s.project.clips.size()) return;
        auto ask = [&](const juce::String& title, const juce::String& what, double now, std::function<void(pulp::AutoClip&, double)> apply) {
            askForText(title, what, juce::String(now, 2), safe.getComponent(), [safe, autoId, apply](juce::String t) {
                if (!safe) return;
                if (auto* a2 = safe->s_.project.autoById(autoId)) { apply(*a2, t.getDoubleValue()); safe->edited("Automation clip setting"); }
            });
        };
        if (r == 1) askForText("Rename automation clip", "Name", au->name, safe.getComponent(), [safe, autoId](juce::String t) {
            if (!safe || t.trim().isEmpty()) return;
            if (auto* a2 = safe->s_.project.autoById(autoId)) { a2->name = t.trim().toStdString(); safe->edited("Rename automation clip"); }
        });
        else if (r == 2) { s.project.makeAutoClipUnique(clipIndex); safe->edited("Make unique"); }
        else if (r == 3) ask("Automation range", "Lowest position of the control, in percent", au->min * 100.0, [](pulp::AutoClip& a2, double v) { a2.min = (float)juce::jlimit(0.0, 100.0, v) / 100.0f; });
        else if (r == 4) ask("Automation range", "Highest position of the control, in percent", au->max * 100.0, [](pulp::AutoClip& a2, double v) { a2.max = (float)juce::jlimit(0.0, 100.0, v) / 100.0f; });
        else if (r == 5) { s.project.clips[(size_t)clipIndex].muted = !s.project.clips[(size_t)clipIndex].muted; safe->edited("Mute clip"); }
        else if (r == 6) safe->deleteSelected();
        else if (r == 20) { au->lfo.on = !au->lfo.on; safe->edited("Automation LFO"); }
        else if (r >= 21 && r <= 24) { au->lfo.shape = r - 21; au->lfo.on = true; safe->edited("Automation LFO"); }
        else if (r == 25) ask("LFO cycle length", "Steps per cycle (16 is one bar)", au->lfo.periodSteps, [](pulp::AutoClip& a2, double v) { a2.lfo.periodSteps = (float)juce::jlimit(0.05, 4096.0, v); a2.lfo.on = true; });
        else if (r == 26) ask("LFO depth", "Percent of the control's travel each way", au->lfo.depth * 100.0, [](pulp::AutoClip& a2, double v) { a2.lfo.depth = (float)juce::jlimit(0.0, 100.0, v) / 100.0f; a2.lfo.on = true; });
    });
}

void Playlist::itemDropped(const SourceDetails& d) {
    const auto s = d.description.toString();
    if (s.startsWith("file:")) { placeAudioFile(juce::File(s.substring(5)), d.localPosition); return; }
    if (!s.startsWith("pattern:") || !gridArea().contains(d.localPosition)) return;
    const int track = trackAtY(d.localPosition.y);
    if (locked(track)) return;
    for (auto& c : s_.project.clips) c.selected = false;
    s_.project.addPatternClip(s.substring(8).getIntValue(), track, snapTick(tickOfX((float)d.localPosition.x), false)).selected = true;
    edited("Place pattern");
}

// ------------------------------------------------------------------ menus

void Playlist::clipMenu(int clipIndex) {
    auto& clips = s_.project.clips;
    if (clipIndex < 0 || clipIndex >= (int)clips.size()) return;
    for (auto& c : clips) c.selected = false;
    selectGroupOf(clipIndex);
    repaint();
    const auto* pat = s_.project.patternById(clips[(size_t)clipIndex].patternId);
    juce::PopupMenu m, colours;
    m.addSectionHeader(pat ? juce::String(pat->name) : juce::String("Clip"));
    m.addItem(1, "Make unique  (give this clip its own copy of the pattern)");
    m.addItem(2, clips[(size_t)clipIndex].muted ? "Unmute" : "Mute");
    m.addItem(3, "Rename pattern...");
    for (int i = 0; i < 16; ++i) { juce::PopupMenu::Item it; it.itemID = 100 + i; it.text = "Colour " + juce::String(i + 1); it.colour = theme::palette(i).brighter(0.3f); colours.addItem(it); }
    m.addSubMenu("Pattern colour", colours);
    m.addSeparator();
    m.addItem(4, "Group the selected clips  (Shift+G)");
    m.addItem(5, "Ungroup  (Alt+G)", clips[(size_t)clipIndex].group != 0);
    m.addItem(6, "Select this pattern");
    m.addItem(7, "Delete");
    m.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<Playlist>(this), clipIndex](int r) {
        if (!safe || r <= 0) return;
        auto& s = safe->s_;
        auto& cl = s.project.clips;
        if (clipIndex >= (int)cl.size()) return;
        const int pid = cl[(size_t)clipIndex].patternId;
        if (r == 1) { s.project.makeClipUnique(clipIndex); safe->edited("Make unique"); }
        else if (r == 2) { cl[(size_t)clipIndex].muted = !cl[(size_t)clipIndex].muted; safe->edited("Mute clip"); }
        else if (r == 3) {
            if (auto* p = s.project.patternById(pid))
                askForText("Rename pattern", "Pattern name", p->name, safe.getComponent(), [safe, pid](juce::String t) {
                    if (!safe || t.trim().isEmpty()) return;
                    if (auto* p2 = safe->s_.project.patternById(pid)) { p2->name = t.trim().toStdString(); safe->edited("Rename pattern"); }
                });
        }
        else if (r == 4) safe->groupSelected(true);
        else if (r == 5) safe->groupSelected(false);
        else if (r == 6) s.selectPattern(s.project.patternIndex(pid));
        else if (r == 7) safe->deleteSelected();
        else if (r >= 100 && r < 116) { if (auto* p = s.project.patternById(pid)) { p->color = theme::palette(r - 100).getARGB(); safe->edited("Pattern colour"); } }
    });
}

void Playlist::trackMenu(int track) {
    auto& tr = s_.project.tracks[(size_t)track];
    juce::PopupMenu m, colours, sizes;
    m.addSectionHeader(tr.name.empty() ? "Track " + juce::String(track + 1) : juce::String(tr.name));
    m.addItem(1, "Rename...");
    for (int i = 0; i < 16; ++i) { juce::PopupMenu::Item it; it.itemID = 100 + i; it.text = "Colour " + juce::String(i + 1); it.colour = theme::palette(i).brighter(0.3f); colours.addItem(it); }
    m.addSubMenu("Colour", colours);
    const float hs[4] = {0.6f, 1.0f, 1.5f, 2.2f};
    const char* hn[4] = {"Small", "Normal", "Large", "Extra large"};
    for (int i = 0; i < 4; ++i) sizes.addItem(200 + i, hn[i], true, std::fabs(tr.height - hs[i]) < 0.01f);
    m.addSubMenu("Size", sizes);
    m.addItem(2, "Mute", true, tr.mute);
    m.addItem(3, "Solo", true, tr.solo);
    m.addItem(4, "Lock  (clips on it cannot be changed)", true, tr.locked);
    m.addItem(5, "Group with the track above", track > 0, tr.grouped);
    if (s_.project.trackIsGroupHead(track)) m.addItem(6, tr.collapsed ? "Show the group's tracks" : "Hide the group's tracks  (they still play)");
    m.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<Playlist>(this), track, hs](int r) {
        if (!safe || r <= 0) return;
        auto& t = safe->s_.project.tracks[(size_t)track];
        if (r == 1) askForText("Rename track", "Track name", t.name, safe.getComponent(), [safe, track](juce::String n) {
            if (safe) { safe->s_.project.tracks[(size_t)track].name = n.trim().toStdString(); safe->edited("Rename track"); }
        });
        else if (r == 2) { t.mute = !t.mute; safe->edited("Track mute"); }
        else if (r == 3) { const bool was = t.solo; for (auto& o : safe->s_.project.tracks) o.solo = false; t.solo = !was; safe->edited("Track solo"); }
        else if (r == 4) { t.locked = !t.locked; safe->edited("Track lock"); }
        else if (r == 5) {
            // a group that changes is shown whole
            t.grouped = !t.grouped;
            safe->s_.project.tracks[(size_t)safe->s_.project.trackGroupHead(track)].collapsed = false;
            t.collapsed = false;
            safe->edited("Track group");
        }
        else if (r == 6) safe->setCollapsed(track, !t.collapsed);
        else if (r >= 200 && r < 204) { t.height = hs[r - 200]; safe->edited("Track size"); }
        else if (r >= 100 && r < 116) { t.color = theme::palette(r - 100).darker(0.6f).getARGB(); safe->edited("Track colour"); }
    });
}

void Playlist::rulerMenu(int64 tick) {
    auto& markers = s_.project.markers;
    int near = -1;
    for (int i = 0; i < (int)markers.size(); ++i) if (std::llabs(markers[(size_t)i].tick - tick) * pxPerTick_ < 10.0) near = i;
    juce::PopupMenu m;
    m.addItem(1, "Add a time marker here...  (Alt+T adds one at the song position)");
    m.addItem(2, "Rename this marker...", near >= 0);
    m.addItem(3, "Delete this marker", near >= 0);
    m.addSeparator();
    m.addItem(4, "Clear the loop region", s_.project.loopEnd > s_.project.loopStart);
    m.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<Playlist>(this), tick, near](int r) {
        if (!safe) return;
        auto& s = safe->s_;
        if (r == 1) askForText("Time marker", "Marker name", "Marker", safe.getComponent(), [safe, tick](juce::String t) {
            if (!safe) return;
            auto& mk = safe->s_.project.markers;
            mk.push_back({tick, t.toStdString()});
            std::sort(mk.begin(), mk.end(), [](const pulp::TimeMarker& a, const pulp::TimeMarker& b) { return a.tick < b.tick; });
            safe->edited("Add marker");
        });
        else if (r == 2) askForText("Time marker", "Marker name", s.project.markers[(size_t)near].name, safe.getComponent(), [safe, near](juce::String t) {
            if (safe && near < (int)safe->s_.project.markers.size()) { safe->s_.project.markers[(size_t)near].name = t.toStdString(); safe->edited("Rename marker"); }
        });
        else if (r == 3) { s.project.markers.erase(s.project.markers.begin() + near); safe->edited("Delete marker"); }
        else if (r == 4) { s.project.loopStart = s.project.loopEnd = 0; safe->edited("Loop region"); }
    });
}

// ------------------------------------------------------------------ keys

void Playlist::setTool(int t) { tool_ = juce::jlimit(0, (int)NumTools - 1, t); repaint(0, 0, getWidth(), kTopH); }

bool Playlist::handleKey(const juce::KeyPress& k) {
    auto& clips = s_.project.clips;
    const auto mods = k.getModifiers();
    const int code = k.getKeyCode();
    const bool ctrl = mods.isCtrlDown(), shift = mods.isShiftDown(), alt = mods.isAltDown();
    const bool plain = !ctrl && !shift && !alt;
    if (plain && !s_.typingKeyboard) {
        static const char keys[] = {'P', 'B', 'D', 'T', 'S', 'C', 'E', 'Z', 'Y'};
        for (int i = 0; i < NumTools; ++i) if (code == keys[i]) { setTool(i); return true; }
    }
    if (code == juce::KeyPress::deleteKey && plain) { deleteSelected(); return true; }
    if (code == 'A' && ctrl && !alt) { selectAll(true); return true; }
    if (code == 'D' && ctrl && !alt) { selectAll(false); return true; }
    if (code == 'B' && ctrl && !alt) { duplicateSelected(); return true; }
    if (code == 'G' && shift && !ctrl && !alt) { groupSelected(true); return true; }
    if (code == 'G' && alt && !ctrl) { groupSelected(false); return true; }
    if ((code == 'C' || code == 'X') && ctrl && !alt) {
        clipboard_.clear();
        int64 first = INT64_MAX;
        for (auto& c : clips) if (c.selected) { clipboard_.push_back(c); first = std::min(first, c.start); }
        const int bar = s_.project.ticksPerBar();
        const int64 base = first == INT64_MAX ? 0 : (first / bar) * bar;
        for (auto& c : clipboard_) c.start -= base;
        if (code == 'X') deleteSelected();
        return true;
    }
    if (code == 'V' && ctrl && !alt) {
        if (clipboard_.empty()) return true;
        const int bar = s_.project.ticksPerBar();
        const int64 at = (int64)std::ceil(scrollTick_ / bar) * bar;
        for (auto& c : clips) c.selected = false;
        for (auto c : clipboard_) { c.start += at; c.selected = true; c.group = 0; clips.push_back(c); }
        edited("Paste clips");
        return true;
    }
    if ((code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) && shift && !ctrl) {
        const int64 d = (code == juce::KeyPress::leftKey ? -1 : 1) * (int64)snap(false);
        for (auto& c : clips) if (c.selected && c.start + d < 0) return true;
        for (auto& c : clips) if (c.selected && !locked(c.track)) c.start += d;
        edited("Nudge clips");
        return true;
    }
    if ((code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) && shift && !ctrl) {
        const int d = code == juce::KeyPress::upKey ? -1 : 1;
        for (auto& c : clips) if (c.selected && (c.track + d < 0 || c.track + d >= kNumPlaylistTracks)) return true;
        for (auto& c : clips) if (c.selected && !locked(c.track) && !locked(c.track + d)) c.track += d;
        edited("Move clips");
        return true;
    }
    if (code == 'T' && alt && !ctrl) {
        const int64 tick = snapTick(s_.project.songMode ? s_.engine.positionTicks() : scrollTick_, false);
        s_.project.markers.push_back({tick, "Marker " + std::to_string(s_.project.markers.size() + 1)});
        std::sort(s_.project.markers.begin(), s_.project.markers.end(), [](const pulp::TimeMarker& a, const pulp::TimeMarker& b) { return a.tick < b.tick; });
        edited("Add marker");
        return true;
    }
    if (code == juce::KeyPress::pageUpKey) { zoomX(1.25, (float)gridArea().getCentreX()); return true; }
    if (code == juce::KeyPress::pageDownKey) { zoomX(0.8, (float)gridArea().getCentreX()); return true; }
    if (code == juce::KeyPress::homeKey) { scrollTick_ = 0; updateScrollbars(); repaint(); return true; }
    return handleMoreKeys(k);
}

juce::String Playlist::describe() const {
    juce::String out = "playlist tool=" + juce::String(tool_) + " snap=" + juce::String(snap(false)) + " clips=" + juce::String((int)s_.project.clips.size())
                     + " length=" + juce::String(s_.project.songLengthTicks()) + " loop=" + juce::String(s_.project.loopStart) + ".." + juce::String(s_.project.loopEnd) + " :";
    int shown = 0;
    for (auto& c : s_.project.clips) {
        if (++shown > 24) { out << " ..."; break; }
        out << " t" << c.track << ":" << c.start << "+" << c.length << (c.offset ? "o" + juce::String(c.offset) : juce::String())
            << (c.type == Clip::AutomationClip ? "u" + juce::String(c.autoId) : (c.type == Clip::AudioClip ? "a" + juce::String(c.audioId) : "p" + juce::String(s_.project.patternIndex(c.patternId))))
            << (c.take > 0 ? "k" + juce::String(c.take) : juce::String()) << (c.sync ? "S" : "")
            << (c.muted ? "m" : "") << (c.group ? "g" : "") << (c.selected ? "*" : "");
    }
    return out;
}

juce::String Playlist::describeTracks(int count) const {
    juce::String out = "tracks:";
    for (int t = 0; t < juce::jmin(count, kNumPlaylistTracks); ++t) {
        const auto& tr = s_.project.tracks[(size_t)t];
        out << " " << t << "@" << trackTop(t) << "+" << trackHeight(t) << (tr.grouped ? "g" : "") << (tr.collapsed ? "c" : "") << (s_.project.trackHidden(t) ? "h" : "");
    }
    return out;
}

void Playlist::setCollapsed(int track, bool collapsed) {
    if (track < 0 || track >= kNumPlaylistTracks || !s_.project.trackIsGroupHead(track)) return;
    s_.project.tracks[(size_t)track].collapsed = collapsed;
    // clips that can no longer be seen are not left selected
    if (collapsed) for (auto& c : s_.project.clips) if (s_.project.trackHidden(c.track)) c.selected = false;
    edited(collapsed ? "Hide group" : "Show group");
}

// More of the reference program's playlist keys.
bool Playlist::handleMoreKeys(const juce::KeyPress& k) {
    auto& clips = s_.project.clips;
    const auto mods = k.getModifiers();
    const int code = k.getKeyCode();
    const bool ctrl = mods.isCtrlDown(), shift = mods.isShiftDown(), alt = mods.isAltDown();
    if (code == 'M' && alt && !ctrl) {                  // Alt+M mutes the selected clips, Alt+Shift+M unmutes them
        bool any = false;
        for (auto& c : clips) if (c.selected) { c.muted = !shift; any = true; }
        if (any) edited(shift ? "Unmute clips" : "Mute clips");
        return true;
    }
    if (code == 'I' && shift && !ctrl && !alt) { for (auto& c : clips) c.selected = !c.selected && !s_.project.trackHidden(c.track); repaint(); return true; }     // invert the selection
    if (code == 'Q' && shift && !ctrl && !alt) {        // quick quantize: selected clips start on the grid
        const int64 grid = juce::jmax(1, snap(false));
        bool any = false;
        for (auto& c : clips) if (c.selected && !locked(c.track)) { c.start = ((c.start + grid / 2) / grid) * grid; any = true; }
        if (any) edited("Quantize clip starts");
        return true;
    }
    if (code == 'C' && shift && !ctrl && !alt) {        // select every clip that plays the same thing as a selected one
        std::vector<pulp::Clip> sources;
        for (auto& c : clips) if (c.selected) sources.push_back(c);
        for (auto& c : clips)
            for (auto& s : sources)
                if (c.type == s.type && c.patternId == s.patternId && c.audioId == s.audioId && c.autoId == s.autoId && !s_.project.trackHidden(c.track)) c.selected = true;
        repaint();
        return true;
    }
    if ((code == '4' || code == '5' || code == '0') && shift && !ctrl && !alt) {
        const auto g = gridArea();
        if (code == '0') {                              // centre on the play position
            scrollTick_ = juce::jmax(0.0, s_.engine.positionTicks() - (double)g.getWidth() * 0.5 / pxPerTick_);
        } else {
            // show the whole song (4), or what is selected (5)
            int64 from = INT64_MAX, to = 0;
            for (auto& c : clips) if (code == '4' || c.selected) { from = std::min(from, c.start); to = std::max(to, c.start + c.length); }
            if (from == INT64_MAX) { from = 0; to = (int64)s_.project.ticksPerBar() * 8; }
            if (code == '4') from = 0;
            const double span = (double)juce::jmax<int64>(to - from, s_.project.ticksPerBar());
            pxPerTick_ = juce::jlimit(0.0005, 2.0, (double)(g.getWidth() - 20) / span);
            scrollTick_ = (double)from;
        }
        updateScrollbars();
        repaint();
        return true;
    }
    return false;
}
