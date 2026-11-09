#include "PianoRoll.h"
#include "ChannelEditor.h"
#include "pulp/Sampler.h"

using pulp::kTicksPerStep;
using pulp::Note;
namespace tools = pulp::tools;

std::vector<Note> PianoRoll::clipboard_;

namespace {
struct TopButton { const char* label; int width; const char* hint; };
const TopButton kTop[] = {
    {"P", 22, "Draw tool (P): click to place a note, drag a note to move it, drag its right edge to resize, right-click to delete"},
    {"B", 22, "Paint tool (B): drag to paint a run of notes"},
    {"D", 22, "Delete tool (D): click or drag over notes to delete them"},
    {"T", 22, "Mute tool (T): click or drag over notes to mute or unmute them"},
    {"C", 22, "Slice tool (C): drag a line across notes to cut them"},
    {"E", 22, "Select tool (E): drag a box to select notes (Ctrl-drag does the same in any tool)"},
    {"Z", 22, "Zoom tool (Z): drag a box to zoom into it, right-click to zoom out"},
    {"Y", 22, "Playback tool (Y): hold the mouse down to play from that point"},
    {"", 84, "Snap for this window. Main follows the toolbar's snap. Hold Alt while dragging to bypass it."},
    {"", 118, "Channel being edited: click to choose another"},
    {"GHOST", 48, "Ghost notes: show the other channels' notes in this pattern. Double-click one to switch to its channel."},
    {"", 118, "Scale highlighting: choose a root and scale to shade the notes outside it"},
    {"", 92, "Stamp: choose a chord, then each click places the whole chord"},
    {"", 40, "Note colour group 1 to 16 for new notes (also the MIDI channel on export). Slide notes only bend notes of their own colour."},
    {"SLIDE", 44, "Slide: new notes are slide notes, which bend the sounding notes of their colour to their pitch"},
    {"PORTA", 46, "Portamento: new notes glide in from the previous note's pitch"},
    {"WAVE", 44, "Waveform backdrop: draw the channel's sample behind the grid to line notes up with it"},
    {"STEP", 40, "Step recording: each note you play lands at the step cursor, which then moves on by one snap unit"},
    {"TOOLS", 52, "Tools menu: quantize, chop, glue, arpeggiate, strum, flam, articulate, randomize, levels, flip, LFO, riff generator, MIDI import and export"},
};
const int kNumTop = (int)(sizeof kTop / sizeof kTop[0]);
enum TopId { TSnap = 8, TChannel, TGhost, TScale, TStamp, TColour, TSlide, TPorta, TWave, TStep, TTools };
}


std::vector<Note>* PianoRoll::notes() {
    auto* ch = s_.current();
    return ch ? &s_.project.pattern().notesFor(ch->id) : nullptr;
}

void PianoRoll::edited(const juce::String& what) {
    if (auto* n = notes()) pulp::sortNotes(*n);
    s_.commit(what);
}


// ------------------------------------------------------------------ geometry

juce::Rectangle<int> PianoRoll::gridArea() const {
    return {kKeyW, kTopH + kRulerH, juce::jmax(10, getWidth() - kKeyW - kBarH), juce::jmax(10, getHeight() - kTopH - kRulerH - kLaneH - kBarH)};
}
juce::Rectangle<int> PianoRoll::rulerArea() const { return {kKeyW, kTopH, gridArea().getWidth(), kRulerH}; }
juce::Rectangle<int> PianoRoll::keysArea() const { return {0, gridArea().getY(), kKeyW, gridArea().getHeight()}; }
juce::Rectangle<int> PianoRoll::laneArea() const { return {0, gridArea().getBottom(), getWidth() - kBarH, kLaneH}; }

int PianoRoll::snap(bool alt) const {
    if (alt) return 1;
    if (snapChoice_ == 0) return s_.snapTicks(pxPerTick_);
    const int t = Session::snapChoices()[(size_t)(snapChoice_ - 1)].second;
    if (t == 0) return Session::lineSnap(pxPerTick_, s_.project.ticksPerBar());
    if (t < 0) return s_.project.ticksPerBar();
    return t;
}

int PianoRoll::snapTick(double tick, bool alt, bool floor) const {
    const int g = snap(alt);
    const double v = floor ? std::floor(tick / g) : std::round(tick / g);
    return juce::jmax(0, (int)v * g);
}

juce::Rectangle<float> PianoRoll::noteRect(const Note& n) const {
    return {xOfTick(n.tick), yOfKey(n.key), juce::jmax(3.0f, (float)(n.length * pxPerTick_)), (float)keyH_};
}

int PianoRoll::noteAt(juce::Point<int> p, bool* onEdge) const {
    auto* ch = const_cast<Session&>(s_).current();
    if (!ch) return -1;
    const auto* ns = s_.project.patterns[(size_t)s_.project.currentPattern].find(ch->id);
    if (!ns) return -1;
    for (int i = (int)ns->size() - 1; i >= 0; --i) {
        const auto r = noteRect((*ns)[(size_t)i]);
        if (r.contains(p.toFloat())) {
            if (onEdge) *onEdge = p.x >= r.getRight() - juce::jmin(7.0f, r.getWidth() * 0.4f);
            return i;
        }
    }
    return -1;
}

void PianoRoll::clampScroll() {
    scrollTick_ = juce::jmax(0.0, scrollTick_);
    scrollY_ = juce::jlimit(0, juce::jmax(0, 128 * keyH_ - gridArea().getHeight()), scrollY_);
}

void PianoRoll::zoomX(double factor, float anchorX) {
    const double anchorTick = tickOfX(anchorX);
    pxPerTick_ = juce::jlimit(0.004, 2.0, pxPerTick_ * factor);
    scrollTick_ = anchorTick - (double)(anchorX - (float)gridArea().getX()) / pxPerTick_;
    clampScroll();
    updateScrollbars();
    repaint();
}

void PianoRoll::zoomToFit() {
    auto* ns = notes();
    int lo = 127, hi = 0, end = s_.project.patternLengthTicks(s_.project.pattern());
    if (ns) for (auto& n : *ns) { lo = std::min(lo, n.key); hi = std::max(hi, n.key); end = std::max(end, n.end()); }
    pxPerTick_ = juce::jlimit(0.004, 2.0, (double)(gridArea().getWidth() - 20) / (double)juce::jmax(1, end));
    scrollTick_ = 0.0;
    if (lo <= hi) scrollY_ = (127 - hi) * keyH_ - juce::jmax(0, (gridArea().getHeight() - (hi - lo + 1) * keyH_) / 2);
    clampScroll();
    updateScrollbars();
    repaint();
}

void PianoRoll::updateScrollbars() {
    const auto g = gridArea();
    int end = s_.project.patternLengthTicks(s_.project.pattern());
    if (auto* ns = notes()) for (auto& n : *ns) end = std::max(end, n.end());
    const double total = (double)end + s_.project.ticksPerBar() * 4.0 + g.getWidth() / pxPerTick_;
    hbar_.setRangeLimits(0.0, total, juce::dontSendNotification);
    hbar_.setCurrentRange(scrollTick_, g.getWidth() / pxPerTick_, juce::dontSendNotification);
    vbar_.setRangeLimits(0.0, 128.0 * keyH_, juce::dontSendNotification);
    vbar_.setCurrentRange((double)scrollY_, (double)g.getHeight(), juce::dontSendNotification);
}

void PianoRoll::resized() {
    const auto g = gridArea();
    hbar_.setBounds(kKeyW, getHeight() - kBarH, g.getWidth(), kBarH);
    vbar_.setBounds(getWidth() - kBarH, g.getY(), kBarH, g.getHeight());
    clampScroll();
    updateScrollbars();
}

void PianoRoll::uiTick() {
    s_.inputSnap = isShowing() ? snap() : 0;        // recorded notes snap to this window's grid while it is open
    // scrollbars are polled so no listener plumbing is needed
    if (!hbar_.isMouseButtonDown() && !vbar_.isMouseButtonDown()) { /* keep in step below */ }
    if (std::fabs(hbar_.getCurrentRangeStart() - scrollTick_) > 0.5 && hbar_.isMouseButtonDown()) { scrollTick_ = hbar_.getCurrentRangeStart(); repaint(); }
    if (std::abs((int)vbar_.getCurrentRangeStart() - scrollY_) > 0 && vbar_.isMouseButtonDown()) { scrollY_ = (int)vbar_.getCurrentRangeStart(); repaint(); }

    float x = -1.0f;
    if (!s_.project.songMode && (s_.engine.isPlaying() || s_.engine.positionTicks() > 0.0)) x = xOfTick(s_.engine.positionTicks());
    if (s_.stepRecording) x = xOfTick(s_.stepCursor);
    if (x != playX_) {
        const auto g = gridArea();
        auto strip = [&](float px) { if (px >= 0) repaint((int)px - 2, kTopH, 5, g.getBottom() - kTopH); };
        strip(playX_);
        playX_ = x;
        strip(playX_);
        // follow the playhead when it runs off the right edge
        if (s_.engine.isPlaying() && gesture_ == None && x > (float)g.getRight() - 4.0f) {
            scrollTick_ += g.getWidth() * 0.9 / pxPerTick_;
            updateScrollbars();
            repaint();
        } else if (s_.engine.isPlaying() && gesture_ == None && x < (float)g.getX() && x >= 0.0f) {
            scrollTick_ = juce::jmax(0.0, s_.engine.positionTicks() - 240.0);
            updateScrollbars();
            repaint();
        }
    }
}

// ------------------------------------------------------------------ painting

void PianoRoll::paint(juce::Graphics& g) {
    g.fillAll(theme::panelLo);
    paintGrid(g);
    paintNotes(g);
    paintLane(g);
    paintRuler(g);
    paintKeys(g);
    paintTop(g);
    // corner pieces
    g.setColour(theme::panel);
    g.fillRect(0, kTopH, kKeyW, kRulerH);
    g.fillRect(getWidth() - kBarH, getHeight() - kBarH, kBarH, kBarH);
    g.fillRect(0, getHeight() - kBarH, kKeyW, kBarH);
    g.fillRect(getWidth() - kBarH, kTopH, kBarH, kRulerH);
    g.fillRect(getWidth() - kBarH, laneArea().getY(), kBarH, kLaneH);
}

void PianoRoll::paintGrid(juce::Graphics& g) {
    const auto a = gridArea();
    g.saveState();
    g.reduceClipRegion(a);
    const auto clip = g.getClipBounds();
    // key rows
    const bool useScale = scaleRoot >= 0;
    const auto& sc = tools::scales()[(size_t)juce::jlimit(0, (int)tools::scales().size() - 1, scaleIndex)];
    const int topKey = juce::jlimit(0, 127, keyOfY((float)clip.getY())), botKey = juce::jlimit(0, 127, keyOfY((float)clip.getBottom()));
    for (int k = botKey; k <= topKey; ++k) {
        const float y = yOfKey(k);
        const bool black = KeyboardStrip::isBlack(k);
        juce::Colour c = black ? juce::Colour(0xff25292d) : juce::Colour(0xff2d3237);
        if (useScale) {
            if (!tools::inScale(k, scaleRoot, sc)) c = juce::Colour(0xff1d2023);
            else if (((k - scaleRoot) % 12 + 12) % 12 == 0) c = juce::Colour(0xff35404a);
            else c = juce::Colour(0xff2f353b);
        }
        g.setColour(c);
        g.fillRect((float)a.getX(), y, (float)a.getWidth(), (float)keyH_);
        g.setColour(k % 12 == 0 ? theme::line.brighter(0.25f) : theme::line.withAlpha(0.6f));
        g.drawHorizontalLine((int)(y + (float)keyH_) - 1, (float)a.getX(), (float)a.getRight());
    }
    // waveform backdrop
    if (backdrop_)
        if (auto* ch = s_.current())
            if (auto* smp = dynamic_cast<pulp::Sampler*>(ch->live->plugin.get()))
                if (auto sample = smp->sample()) {
                    const double ticksPerSec = (double)s_.project.global->tempo.get() * pulp::kPPQ / 60.0;
                    const double secPerPx = 1.0 / (pxPerTick_ * ticksPerSec);
                    const float mid = (float)a.getCentreY(), h = (float)a.getHeight() * 0.35f;
                    g.setColour(theme::accent2.withAlpha(0.28f));
                    const float* d = sample->left();
                    const auto frames = sample->frames();
                    for (int x = juce::jmax(a.getX(), clip.getX()); x < clip.getRight(); ++x) {
                        const double t0 = tickOfX((float)x) / ticksPerSec;
                        if (t0 < 0.0) continue;
                        const auto i0 = (pulp::int64)(t0 * sample->sampleRate), i1 = (pulp::int64)((t0 + secPerPx) * sample->sampleRate);
                        if (i0 >= frames) break;
                        float mn = 0, mx = 0;
                        const auto stride = std::max<pulp::int64>(1, (i1 - i0) / 32);
                        for (auto i = i0; i <= i1 && i < frames; i += stride) { mn = std::min(mn, d[i]); mx = std::max(mx, d[i]); }
                        g.drawVerticalLine(x, mid - mx * h, mid - mn * h + 1.0f);
                    }
                }
    // time lines
    const int bar = s_.project.ticksPerBar();
    const int beat = pulp::kPPQ * 4 / juce::jmax(1, s_.project.timeSigDen);
    const int fine = Session::lineSnap(pxPerTick_, bar);
    const int t0 = (int)(std::floor(tickOfX((float)clip.getX()) / fine) * fine), t1 = (int)tickOfX((float)clip.getRight()) + fine;
    for (int t = juce::jmax(0, t0); t <= t1; t += fine) {
        const float x = xOfTick(t);
        const bool isBar = t % bar == 0, isBeat = t % beat == 0;
        g.setColour(isBar ? juce::Colour(0xff0e1012) : (isBeat ? juce::Colour(0xff191c1f) : juce::Colour(0xff22262a)));
        g.drawVerticalLine((int)x, (float)a.getY(), (float)a.getBottom());
    }
    // pattern end and loop region
    const float endX = xOfTick(s_.project.patternLengthTicks(s_.project.pattern()));
    g.setColour(juce::Colours::black.withAlpha(0.25f));
    if (endX < (float)a.getRight()) g.fillRect(endX, (float)a.getY(), (float)a.getRight() - endX, (float)a.getHeight());
    if (s_.project.patLoopEnd > s_.project.patLoopStart) {
        g.setColour(theme::accent.withAlpha(0.08f));
        g.fillRect(xOfTick(s_.project.patLoopStart), (float)a.getY(), (float)((s_.project.patLoopEnd - s_.project.patLoopStart) * pxPerTick_), (float)a.getHeight());
    }
    g.restoreState();
}

void PianoRoll::paintNotes(juce::Graphics& g) {
    const auto a = gridArea();
    g.saveState();
    g.reduceClipRegion(a);
    const auto clip = g.getClipBounds().toFloat();
    auto* ch = s_.current();
    // ghost notes from the other channels
    if (ghosts_ && ch)
        for (auto& other : s_.project.channels) {
            if (other.id == ch->id) continue;
            const auto* ns = s_.project.pattern().find(other.id);
            if (!ns) continue;
            for (auto& n : *ns) {
                const auto r = noteRect(n);
                if (!r.intersects(clip)) continue;
                g.setColour(juce::Colour(other.color).withAlpha(0.3f));
                g.fillRect(r.reduced(0.0f, 1.0f));
                g.setColour(juce::Colour(other.color).brighter(0.3f).withAlpha(0.55f));
                g.drawRect(r.reduced(0.0f, 1.0f), 1.0f);
            }
        }
    if (ch)
        if (const auto* ns = s_.project.pattern().find(ch->id)) {
            g.setFont(theme::font(juce::jmin(11.0f, (float)keyH_ - 2.0f)));
            for (auto& n : *ns) {
                const auto r = noteRect(n);
                if (!r.intersects(clip)) continue;
                juce::Colour c = theme::palette(n.color + 5).brighter(0.15f + 0.45f * n.velocity);
                if (n.muted) c = juce::Colour(0xff596068);
                auto body = r.reduced(0.0f, 0.5f);
                g.setColour(n.selected ? c.brighter(0.5f) : c);
                g.fillRoundedRectangle(body, 2.0f);
                g.setColour(n.selected ? juce::Colours::white : juce::Colours::black.withAlpha(0.7f));
                g.drawRoundedRectangle(body, 2.0f, n.selected ? 1.4f : 1.0f);
                if (n.slide || n.porta) {            // a wedge marks slide and portamento notes
                    juce::Path p;
                    if (n.slide) p.addTriangle(body.getX() + 2, body.getBottom() - 2, body.getX() + 9, body.getBottom() - 2, body.getX() + 9, body.getY() + 2);
                    else p.addTriangle(body.getX() + 2, body.getY() + 2, body.getX() + 2, body.getBottom() - 2, body.getX() + 8, body.getCentreY());
                    g.setColour(juce::Colours::black.withAlpha(0.65f));
                    g.fillPath(p);
                }
                if (keyH_ >= 11 && r.getWidth() > 30.0f) {
                    g.setColour(juce::Colours::black.withAlpha(0.75f));
                    g.drawText(tools::keyName(n.key), body.withTrimmedLeft(n.slide || n.porta ? 11.0f : 3.0f), juce::Justification::centredLeft, false);
                }
            }
        }
    if (gesture_ == Marquee || gesture_ == Zooming) {
        g.setColour(theme::accent.withAlpha(0.15f));
        g.fillRect(marquee_);
        g.setColour(theme::accent);
        g.drawRect(marquee_, 1);
    }
    if (gesture_ == Slicing) {
        g.setColour(theme::bad);
        g.drawLine((float)downPos_.x, (float)downPos_.y, (float)lastPos_.x, (float)lastPos_.y, 1.5f);
    }
    if (playX_ >= (float)a.getX()) {
        g.setColour(s_.stepRecording ? theme::warn : theme::good);
        g.drawVerticalLine((int)playX_, (float)a.getY(), (float)a.getBottom());
    }
    g.restoreState();
}

void PianoRoll::paintKeys(juce::Graphics& g) {
    const auto a = keysArea();
    g.saveState();
    g.reduceClipRegion(a);
    g.fillAll(theme::panel);
    for (int k = 0; k < 128; ++k) {
        const float y = yOfKey(k);
        if (y > (float)a.getBottom() || y + (float)keyH_ < (float)a.getY()) continue;
        const bool black = KeyboardStrip::isBlack(k);
        juce::Rectangle<float> r((float)a.getX(), y, (float)a.getWidth() - 1.0f, (float)keyH_ - 1.0f);
        g.setColour(k == playingKey_ ? theme::accent : (black ? juce::Colour(0xff1c1f22) : juce::Colour(0xffd4d8dc)));
        g.fillRect(black ? r.withTrimmedRight(18.0f) : r);
        if (black) { g.setColour(juce::Colour(0xffd4d8dc)); g.fillRect(r.withLeft(r.getRight() - 18.0f)); }
        if (k % 12 == 0 || keyH_ >= 13) {
            g.setColour(black ? juce::Colour(0xff9aa0a6) : juce::Colour(0xff40464c));
            g.setFont(theme::font(juce::jmin(10.0f, (float)keyH_ - 2.0f), k % 12 == 0));
            if (k % 12 == 0 || !black) g.drawText(tools::keyName(k), r.withTrimmedRight(3.0f), juce::Justification::centredRight, false);
        }
    }
    g.restoreState();
}

void PianoRoll::paintRuler(juce::Graphics& g) {
    const auto a = rulerArea();
    g.saveState();
    g.reduceClipRegion(a);
    g.fillAll(theme::panel);
    const int bar = s_.project.ticksPerBar();
    const int beat = pulp::kPPQ * 4 / juce::jmax(1, s_.project.timeSigDen);
    const int every = bar * pxPerTick_ < 26.0 ? (int)std::ceil(26.0 / (bar * pxPerTick_)) : 1;
    if (s_.project.patLoopEnd > s_.project.patLoopStart) {
        g.setColour(theme::accent.withAlpha(0.45f));
        g.fillRect(xOfTick(s_.project.patLoopStart), (float)a.getY(), (float)((s_.project.patLoopEnd - s_.project.patLoopStart) * pxPerTick_), (float)a.getHeight());
    }
    g.setFont(theme::font(10.5f));
    const int first = juce::jmax(0, (int)(tickOfX((float)a.getX()) / bar) - 1);
    for (int b = first; xOfTick(b * bar) < (float)a.getRight(); ++b) {
        const float x = xOfTick(b * bar);
        g.setColour(theme::textDim);
        g.drawVerticalLine((int)x, (float)a.getY() + 2.0f, (float)a.getBottom());
        if (b % every == 0) { g.setColour(theme::text); g.drawText(juce::String(b + 1), (int)x + 3, a.getY(), 40, a.getHeight(), juce::Justification::centredLeft, false); }
        if (beat * pxPerTick_ > 14.0)
            for (int t = beat; t < bar; t += beat) { g.setColour(theme::textDim.withAlpha(0.5f)); g.drawVerticalLine((int)xOfTick(b * bar + t), (float)a.getBottom() - 5.0f, (float)a.getBottom()); }
    }
    for (auto& m : s_.project.pattern().markers) {
        const float x = xOfTick(m.tick);
        g.setColour(theme::warn);
        g.fillRect(x, (float)a.getY(), 2.0f, (float)a.getHeight());
        g.setFont(theme::font(10.0f, true));
        g.drawText(m.name, (int)x + 4, a.getY(), 120, a.getHeight(), juce::Justification::centredLeft, false);
    }
    if (playX_ >= (float)a.getX()) {
        juce::Path p;
        p.addTriangle(playX_ - 5.0f, (float)a.getBottom() - 8.0f, playX_ + 5.0f, (float)a.getBottom() - 8.0f, playX_, (float)a.getBottom());
        g.setColour(s_.stepRecording ? theme::warn : theme::good);
        g.fillPath(p);
    }
    g.restoreState();
}

void PianoRoll::paintLane(juce::Graphics& g) {
    const auto a = laneArea();
    g.saveState();
    g.reduceClipRegion(a);
    g.fillAll(theme::inset);
    g.setColour(theme::panel);
    g.fillRect(a.withWidth(kKeyW));
    g.setColour(theme::accent);
    g.setFont(theme::font(11.0f, true));
    g.drawFittedText(tools::propName(laneProp_), a.withWidth(kKeyW).reduced(4, 2).removeFromTop(28), juce::Justification::topLeft, 2);
    g.setColour(theme::textDim);
    g.setFont(theme::font(9.5f));
    g.drawText("click to change", a.withWidth(kKeyW).reduced(4, 2).removeFromBottom(12), juce::Justification::bottomLeft, false);
    const auto bars = a.withTrimmedLeft(kKeyW).reduced(0, 4);
    g.reduceClipRegion(a.withTrimmedLeft(kKeyW));
    const bool centred = laneProp_ == tools::Pan || laneProp_ == tools::Fine;
    if (centred) { g.setColour(theme::line.brighter(0.2f)); g.drawHorizontalLine(bars.getCentreY(), (float)bars.getX(), (float)bars.getRight()); }
    if (auto* ch = s_.current())
        if (const auto* ns = s_.project.pattern().find(ch->id))
            for (auto& n : *ns) {
                const float x = xOfTick(n.tick);
                if (x < (float)bars.getX() - 4.0f || x > (float)bars.getRight()) continue;
                const float v = tools::getProp(n, laneProp_);
                const float y = (float)bars.getBottom() - v * (float)bars.getHeight();
                g.setColour(n.selected ? juce::Colours::white : theme::palette(n.color + 5).brighter(0.4f));
                if (centred) g.fillRect(x, std::min(y, (float)bars.getCentreY()), 2.0f, std::max(1.0f, std::fabs(y - (float)bars.getCentreY())));
                else g.fillRect(x, y, 2.0f, (float)bars.getBottom() - y);
                g.fillEllipse(x - 2.5f, y - 3.0f, 7.0f, 7.0f);
            }
    g.restoreState();
    g.setColour(theme::line);
    g.drawHorizontalLine(a.getY(), 0.0f, (float)a.getRight());
}

void PianoRoll::paintTop(juce::Graphics& g) {
    g.setColour(theme::panel);
    g.fillRect(0, 0, getWidth(), kTopH);
    int x = 4;
    numTopButtons_ = 0;
    auto* ch = s_.current();
    for (int i = 0; i < kNumTop; ++i) {
        juce::String label = kTop[i].label;
        bool lit = false;
        juce::Colour litCol = theme::accent;
        if (i < NumTools) lit = tool_ == i;
        else if (i == TSnap) label = "Snap: " + juce::String(snapChoice_ == 0 ? "Main" : Session::snapChoices()[(size_t)(snapChoice_ - 1)].first);
        else if (i == TChannel) label = ch ? juce::String(ch->name) : juce::String("(no channel)");
        else if (i == TGhost) lit = ghosts_;
        else if (i == TScale) label = scaleRoot < 0 ? juce::String("Scale: none") : juce::String(tools::keyName(scaleRoot)).dropLastCharacters(1) + " " + tools::scales()[(size_t)scaleIndex].name;
        else if (i == TStamp) label = stampChord < 0 ? juce::String("Stamp: none") : juce::String(tools::chords()[(size_t)stampChord].name);
        else if (i == TColour) { label = juce::String(colorNext_ + 1); lit = true; litCol = theme::palette(colorNext_ + 5).brighter(0.4f); }
        else if (i == TSlide) lit = slideNext_;
        else if (i == TPorta) lit = portaNext_;
        else if (i == TWave) lit = backdrop_;
        else if (i == TStep) { lit = s_.stepRecording; litCol = theme::warn; }
        juce::Rectangle<int> r(x, 4, kTop[i].width, kTopH - 8);
        topButtons_[numTopButtons_++] = r;
        theme::bevel(g, r.toFloat(), hoverTop_ == i ? theme::panelHi.brighter(0.08f) : theme::panelHi);
        g.setColour(lit ? litCol : theme::text.withAlpha(0.85f));
        g.setFont(theme::font(i < NumTools ? 13.0f : 11.5f, i < NumTools || lit));
        g.drawFittedText(label, r.reduced(4, 0), juce::Justification::centred, 1, 0.75f);
        x += kTop[i].width + (i == NumTools - 1 ? 10 : 3);
    }
}

juce::String PianoRoll::getTooltip() {
    const auto p = getMouseXYRelative();
    if (p.y < kTopH) {
        for (int i = 0; i < numTopButtons_; ++i) if (topButtons_[i].contains(p)) return kTop[i].hint;
        return {};
    }
    if (rulerArea().contains(p)) return "Time ruler: click to move the play position, Ctrl-drag to loop a range, double-click to clear the loop, right-click for markers";
    if (keysArea().contains(p)) return juce::String(tools::keyName(keyOfY((float)p.y))) + ": click to hear this key";
    if (laneArea().contains(p)) return p.x < kKeyW ? juce::String("Property shown in this lane: click to choose velocity, pan, release, mod X, mod Y or fine pitch")
                                                   : juce::String(tools::propName(laneProp_)) + ": drag across the bars to set it for each note, right-click to reset";
    if (gridArea().contains(p)) {
        const int tick = (int)tickOfX((float)p.x);
        const int bar = s_.project.ticksPerBar();
        return juce::String(tools::keyName(keyOfY((float)p.y))) + "   bar " + juce::String(tick / bar + 1) + " step " + juce::String((tick % bar) / kTicksPerStep + 1)
             + "   " + kTop[tool_].hint;
    }
    return {};
}

// ------------------------------------------------------------------ editing helpers

void PianoRoll::selectAll(bool on) {
    if (auto* ns = notes()) for (auto& n : *ns) n.selected = on;
    repaint();
}

void PianoRoll::deleteSelected() {
    auto* ns = notes();
    if (!ns) return;
    const auto before = ns->size();
    ns->erase(std::remove_if(ns->begin(), ns->end(), [](const Note& n) { return n.selected; }), ns->end());
    if (ns->size() != before) edited("Delete notes");
}

void PianoRoll::copySelection(bool cut) {
    auto* ns = notes();
    if (!ns) return;
    clipboard_.clear();
    int first = INT32_MAX;
    const bool sel = tools::anySelected(*ns);
    for (auto& n : *ns) if (!sel || n.selected) { clipboard_.push_back(n); first = std::min(first, n.tick); }
    // keep the position inside the bar so a paste lands on the same beat
    const int base = first == INT32_MAX ? 0 : (first / s_.project.ticksPerBar()) * s_.project.ticksPerBar();
    for (auto& n : clipboard_) n.tick -= base;
    if (cut && sel) deleteSelected();
}

void PianoRoll::paste() {
    auto* ns = notes();
    if (!ns || clipboard_.empty()) return;
    const int bar = s_.project.ticksPerBar();
    const int at = (int)std::ceil(scrollTick_ / bar) * bar;      // the first whole bar in view
    for (auto& n : *ns) n.selected = false;
    for (auto n : clipboard_) { n.tick += at; n.selected = true; ns->push_back(n); }
    edited("Paste notes");
}

Note PianoRoll::makeNote(int tick, int key) const {
    Note n = template_;
    n.tick = tick;
    n.key = juce::jlimit(0, 127, key);
    n.color = colorNext_;
    n.slide = slideNext_;
    n.porta = portaNext_;
    n.muted = false;
    n.selected = true;
    return n;
}

void PianoRoll::addNoteAt(int tick, int key) {
    auto* ns = notes();
    if (!ns) return;
    if (stampChord >= 0 && !slideNext_) {
        for (int iv : tools::chords()[(size_t)stampChord].intervals)
            if (key + iv >= 0 && key + iv <= 127) ns->push_back(makeNote(tick, key + iv));
    } else ns->push_back(makeNote(tick, key));
}

void PianoRoll::laneEdit(const juce::MouseEvent& e) {
    auto* ns = notes();
    if (!ns) return;
    const auto bars = laneArea().withTrimmedLeft(kKeyW).reduced(0, 4);
    const float v = e.mods.isPopupMenu() ? tools::defaultProp(laneProp_) : 1.0f - (float)(e.y - bars.getY()) / (float)bars.getHeight();
    const bool sel = tools::anySelected(*ns);
    // every note whose bar lies between the last mouse position and this one
    const float x0 = (float)std::min(lastPos_.x, e.x) - 4.0f, x1 = (float)std::max(lastPos_.x, e.x) + 4.0f;
    for (auto& n : *ns) {
        if (sel && !n.selected) continue;
        const float x = xOfTick(n.tick);
        if (x >= x0 && x <= x1) { tools::setProp(n, laneProp_, v); changed_ = true; }
    }
    lastPos_ = e.getPosition();
    s_.publish();
    repaint();
}

// ------------------------------------------------------------------ mouse

void PianoRoll::mouseMove(const juce::MouseEvent& e) {
    int h = -1;
    if (e.y < kTopH) for (int i = 0; i < numTopButtons_; ++i) if (topButtons_[i].contains(e.getPosition())) h = i;
    if (h != hoverTop_) { hoverTop_ = h; repaint(0, 0, getWidth(), kTopH); }
    if (gridArea().contains(e.getPosition()) && (tool_ == Draw || tool_ == Paint)) {
        bool edge = false;
        const int hit = noteAt(e.getPosition(), &edge);
        setMouseCursor(hit >= 0 && edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    } else setMouseCursor(juce::MouseCursor::NormalCursor);
}

void PianoRoll::mouseDown(const juce::MouseEvent& e) {
    grabKeyboardFocus();
    const auto p = e.getPosition();
    downPos_ = lastPos_ = p;
    gesture_ = None;
    moved_ = changed_ = false;
    touched_.clear();
    if (p.y < kTopH) { topClick(e); return; }
    auto* ns = notes();
    if (!ns) return;

    if (keysArea().contains(p)) {
        gesture_ = KeyPlay;
        playingKey_ = juce::jlimit(0, 127, keyOfY((float)p.y));
        s_.liveNoteOn(playingKey_, juce::jlimit(0.3f, 1.0f, 0.35f + 0.65f * (float)p.x / (float)kKeyW));
        repaint(keysArea());
        return;
    }
    if (rulerArea().contains(p)) {
        const int tick = snapTick(tickOfX((float)p.x), e.mods.isAltDown(), false);
        if (e.mods.isPopupMenu()) { rulerMenu(tick); return; }
        if (e.mods.isCtrlDown()) { gesture_ = RulerLoop; downTick_ = tick; return; }
        gesture_ = RulerSeek;
        if (s_.stepRecording) { s_.stepCursor = tick; s_.notify(); } else s_.engine.seek(tick);
        return;
    }
    if (laneArea().contains(p)) {
        if (p.x < kKeyW) {
            juce::PopupMenu m;
            for (int i = 0; i < tools::NumProps; ++i) m.addItem(i + 1, tools::propName(i), true, i == laneProp_);
            m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(laneArea().withWidth(kKeyW))),
                            [safe = juce::Component::SafePointer<PianoRoll>(this)](int r) { if (safe && r > 0) { safe->laneProp_ = r - 1; safe->repaint(); } });
            return;
        }
        gesture_ = Lane;
        laneEdit(e);
        return;
    }
    if (!gridArea().contains(p)) return;

    const bool alt = e.mods.isAltDown();
    const int key = juce::jlimit(0, 127, keyOfY((float)p.y));
    downKey_ = key;
    downTick_ = (int)tickOfX((float)p.x);
    bool edge = false;
    const int hit = noteAt(p, &edge);

    if (e.mods.isMiddleButtonDown()) { gesture_ = Panning; return; }
    if (e.mods.isPopupMenu()) {
        if (e.mods.isCtrlDown()) { gesture_ = RightZoom; zoomStartPx_ = pxPerTick_; zoomStartKeyH_ = keyH_; return; }
        if (tool_ == Zoom) { zoomX(1.0 / 1.6, (float)p.x); return; }
        gesture_ = Erase;
        if (hit >= 0) { ns->erase(ns->begin() + hit); changed_ = true; s_.publish(); repaint(); }
        return;
    }
    switch (tool_) {
        case Draw:
        case Paint: {
            if (e.mods.isCtrlDown()) { gesture_ = Marquee; marquee_ = {p.x, p.y, 0, 0}; return; }
            if (hit >= 0) {
                Note& n = (*ns)[(size_t)hit];
                if (!n.selected) { for (auto& o : *ns) o.selected = false; n.selected = true; }
                template_ = n;
                if (e.mods.isShiftDown() && !edge) {
                    // Shift-drag clones: the copies become the selection and follow the mouse
                    std::vector<Note> copies;
                    for (auto& o : *ns) if (o.selected) { copies.push_back(o); o.selected = false; }
                    downTick_ -= 0;
                    const size_t base = ns->size();
                    ns->insert(ns->end(), copies.begin(), copies.end());
                    touched_.push_back((int)base);          // the clone of the first selected note anchors the drag
                    for (size_t i = base; i < ns->size(); ++i) if ((*ns)[i].tick == template_.tick && (*ns)[i].key == template_.key) { touched_[0] = (int)i; break; }
                    changed_ = true;
                } else touched_.push_back(hit);
                gesture_ = edge ? Resize : Move;
                before_ = *ns;
                playingKey_ = template_.key;
                if (!template_.slide) s_.engine.noteOn(s_.currentChannel, playingKey_, template_.velocity);
            } else {
                for (auto& o : *ns) o.selected = false;
                const int tick = snapTick(tickOfX((float)p.x), alt);
                const size_t base = ns->size();
                addNoteAt(tick, key);
                placedTick_ = tick;
                placedValid_ = true;
                changed_ = true;
                touched_.push_back((int)base);
                before_ = *ns;
                gesture_ = tool_ == Paint ? Painting : Move;
                downTick_ = tick;
                playingKey_ = key;
                if (!slideNext_) s_.engine.noteOn(s_.currentChannel, key, template_.velocity);
                s_.publish();
            }
            repaint();
            break;
        }
        case Delete:
            gesture_ = Erase;
            if (hit >= 0) { ns->erase(ns->begin() + hit); changed_ = true; s_.publish(); repaint(); }
            break;
        case Mute:
            gesture_ = Muting;
            if (hit >= 0) { (*ns)[(size_t)hit].muted = !(*ns)[(size_t)hit].muted; touched_.push_back(hit); changed_ = true; s_.publish(); repaint(); }
            break;
        case Slice: gesture_ = Slicing; break;
        case Select:
            if (hit >= 0) {
                if (!e.mods.isShiftDown() && !e.mods.isCtrlDown()) for (auto& o : *ns) o.selected = false;
                (*ns)[(size_t)hit].selected = !(*ns)[(size_t)hit].selected || !(e.mods.isShiftDown() || e.mods.isCtrlDown());
                template_ = (*ns)[(size_t)hit];
                repaint();
            } else { gesture_ = Marquee; marquee_ = {p.x, p.y, 0, 0}; }
            break;
        case Zoom: gesture_ = Zooming; marquee_ = {p.x, p.y, 0, 0}; break;
        case Scrub:
            gesture_ = Scrubbing;
            wasPlaying_ = s_.engine.isPlaying();
            s_.engine.seek(snapTick(tickOfX((float)p.x), alt));
            s_.engine.play();
            break;
        default: break;
    }
}

void PianoRoll::mouseDrag(const juce::MouseEvent& e) {
    auto* ns = notes();
    if (!ns) return;
    const auto p = e.getPosition();
    const bool alt = e.mods.isAltDown();
    if (std::abs(p.x - downPos_.x) + std::abs(p.y - downPos_.y) > 2) moved_ = true;
    const auto grid = gridArea();
    switch (gesture_) {
        case KeyPlay: {
            const int k = juce::jlimit(0, 127, keyOfY((float)p.y));
            if (k != playingKey_) { s_.liveNoteOff(playingKey_); playingKey_ = k; s_.liveNoteOn(k, 0.8f); repaint(keysArea()); }
            break;
        }
        case RulerSeek: {
            const int tick = snapTick(tickOfX((float)p.x), alt, false);
            if (s_.stepRecording) { s_.stepCursor = tick; } else s_.engine.seek(tick);
            break;
        }
        case RulerLoop: {
            const int tick = snapTick(tickOfX((float)p.x), alt, false);
            s_.project.patLoopStart = std::min(downTick_, tick);
            s_.project.patLoopEnd = std::max(downTick_, tick);
            s_.publish();
            repaint();
            break;
        }
        case Lane: laneEdit(e); break;
        case Panning:
            scrollTick_ -= (p.x - lastPos_.x) / pxPerTick_;
            scrollY_ -= p.y - lastPos_.y;
            clampScroll(); updateScrollbars(); repaint();
            break;
        case RightZoom: {
            const double f = std::pow(2.0, (p.x - downPos_.x) / 120.0);
            const double anchor = tickOfX((float)downPos_.x);
            pxPerTick_ = juce::jlimit(0.004, 2.0, zoomStartPx_ * f);
            scrollTick_ = anchor - (double)(downPos_.x - grid.getX()) / pxPerTick_;
            keyH_ = juce::jlimit(6, 40, zoomStartKeyH_ + (downPos_.y - p.y) / 12);
            clampScroll(); updateScrollbars(); repaint();
            break;
        }
        case Move: {
            if (touched_.empty() || before_.size() != ns->size()) break;
            const Note& anchor = before_[(size_t)touched_[0]];
            int dTick = snapTick(anchor.tick + (tickOfX((float)p.x) - tickOfX((float)downPos_.x)), alt, false) - anchor.tick;
            int dKey = juce::jlimit(0, 127, keyOfY((float)p.y)) - downKey_;
            for (size_t i = 0; i < ns->size(); ++i) if (before_[i].selected) { dTick = std::max(dTick, -before_[i].tick); dKey = juce::jlimit(-before_[i].key, 127 - before_[i].key, dKey); }
            bool keyChanged = false;
            for (size_t i = 0; i < ns->size(); ++i)
                if (before_[i].selected) {
                    if ((*ns)[i].key != before_[i].key + dKey && (int)i == touched_[0]) keyChanged = true;
                    (*ns)[i].tick = before_[i].tick + dTick;
                    (*ns)[i].key = before_[i].key + dKey;
                }
            if (dTick != 0 || dKey != 0) changed_ = true;
            if (keyChanged && playingKey_ >= 0) {
                s_.engine.noteOff(s_.currentChannel, playingKey_);
                playingKey_ = (*ns)[(size_t)touched_[0]].key;
                if (!(*ns)[(size_t)touched_[0]].slide) s_.engine.noteOn(s_.currentChannel, playingKey_, anchor.velocity);
            }
            s_.publish();
            repaint();
            break;
        }
        case Resize: {
            if (touched_.empty() || before_.size() != ns->size()) break;
            const Note& anchor = before_[(size_t)touched_[0]];
            const int minLen = alt ? 1 : std::min(snap(false), kTicksPerStep / 4);
            const int newEnd = snapTick(anchor.end() + (tickOfX((float)p.x) - tickOfX((float)downPos_.x)), alt, false);
            const int dLen = std::max(minLen, newEnd - anchor.tick) - anchor.length;
            for (size_t i = 0; i < ns->size(); ++i) if (before_[i].selected) (*ns)[i].length = std::max(minLen, before_[i].length + dLen);
            template_.length = (*ns)[(size_t)touched_[0]].length;
            if (dLen != 0) changed_ = true;
            s_.publish();
            repaint();
            break;
        }
        case Painting: {
            if (!grid.contains(p)) break;
            const int tick = snapTick(tickOfX((float)p.x), alt), key = juce::jlimit(0, 127, keyOfY((float)p.y));
            // one note per snap cell along the drag, no stacking
            const int step = std::max(snap(alt), template_.length);
            const int cell = (tick / step) * step;
            bool exists = false;
            for (auto& n : *ns) if (n.key == key && n.tick < cell + step && n.end() > cell) { exists = true; break; }
            if (!exists) { addNoteAt(cell, key); changed_ = true; s_.publish(); repaint(); }
            break;
        }
        case Erase: {
            const int hit = noteAt(p);
            if (hit >= 0) { ns->erase(ns->begin() + hit); changed_ = true; s_.publish(); repaint(); }
            break;
        }
        case Muting: {
            const int hit = noteAt(p);
            if (hit >= 0 && std::find(touched_.begin(), touched_.end(), hit) == touched_.end()) {
                (*ns)[(size_t)hit].muted = !(*ns)[(size_t)hit].muted;
                touched_.push_back(hit);
                changed_ = true; s_.publish(); repaint();
            }
            break;
        }
        case Marquee: case Zooming:
            marquee_ = juce::Rectangle<int>(downPos_, p).getIntersection(grid);
            repaint();
            break;
        case Slicing: repaint(); break;
        case Scrubbing: s_.engine.seek(snapTick(tickOfX((float)p.x), alt)); break;
        default: break;
    }
    if (gesture_ != Lane) lastPos_ = p;
}

void PianoRoll::mouseUp(const juce::MouseEvent& e) {
    auto* ns = notes();
    const int g = gesture_;
    gesture_ = None;
    if (!ns) return;
    switch (g) {
        case KeyPlay: s_.liveNoteOff(playingKey_); playingKey_ = -1; repaint(keysArea()); break;
        case Move: case Resize: case Painting:
            if (playingKey_ >= 0) { s_.engine.noteOff(s_.currentChannel, playingKey_); playingKey_ = -1; }
            if (changed_) edited(g == Resize ? "Resize notes" : (g == Painting ? "Paint notes" : "Move notes"));
            break;
        case Erase: if (changed_) edited("Delete notes"); break;
        case Muting: if (changed_) edited("Mute notes"); break;
        case Lane: if (changed_) edited("Edit note properties"); break;
        case RulerLoop:
            if (s_.project.patLoopEnd - s_.project.patLoopStart < 30) { s_.project.patLoopStart = s_.project.patLoopEnd = 0; }
            s_.publish(); repaint();
            break;
        case Marquee: {
            const bool add = e.mods.isShiftDown();
            for (auto& n : *ns) {
                const bool in = noteRect(n).intersects(marquee_.toFloat());
                n.selected = add ? (n.selected || in) : in;
            }
            repaint();
            break;
        }
        case Zooming:
            if (marquee_.getWidth() > 8 && marquee_.getHeight() > 8) {
                const double t0 = tickOfX((float)marquee_.getX()), t1 = tickOfX((float)marquee_.getRight());
                const int topKey = keyOfY((float)marquee_.getY());
                pxPerTick_ = juce::jlimit(0.004, 2.0, (double)gridArea().getWidth() / juce::jmax(1.0, t1 - t0));
                scrollTick_ = t0;
                scrollY_ = (127 - topKey) * keyH_;
                clampScroll(); updateScrollbars();
            } else zoomX(1.6, (float)downPos_.x);
            repaint();
            break;
        case Slicing: {
            // cut every note the line crosses, at the snapped time where it crosses that note's row
            std::vector<Note> add;
            const float x0 = (float)downPos_.x, y0 = (float)downPos_.y, x1 = (float)e.x, y1 = (float)e.y;
            for (auto& n : *ns) {
                const auto r = noteRect(n);
                const float cy = r.getCentreY();
                if ((cy - y0) * (cy - y1) > 0.0f) continue;                 // the line does not reach this row
                const float x = std::fabs(y1 - y0) < 0.5f ? x0 : x0 + (x1 - x0) * (cy - y0) / (y1 - y0);
                const int cut = snapTick(tickOfX(x), e.mods.isAltDown(), false);
                if (cut <= n.tick || cut >= n.end()) continue;
                Note tail = n;
                tail.tick = cut;
                tail.length = n.end() - cut;
                n.length = cut - n.tick;
                add.push_back(tail);
            }
            if (!add.empty()) { ns->insert(ns->end(), add.begin(), add.end()); edited("Slice notes"); }
            repaint();
            break;
        }
        case Scrubbing: if (!wasPlaying_) s_.engine.pause(); break;
        default: break;
    }
}

void PianoRoll::mouseDoubleClick(const juce::MouseEvent& e) {
    const auto p = e.getPosition();
    if (rulerArea().contains(p)) { s_.project.patLoopStart = s_.project.patLoopEnd = 0; s_.publish(); repaint(); return; }
    // one of this channel's own notes: its properties, to type in
    if (gridArea().contains(p)) { const int own = noteAt(p); if (own >= 0) { showNoteProperties(own); return; } }
    if (!gridArea().contains(p) || !ghosts_) return;
    // a ghost note: switch to its channel
    auto* cur = s_.current();
    for (int i = 0; i < (int)s_.project.channels.size(); ++i) {
        auto& ch = s_.project.channels[(size_t)i];
        if (cur && ch.id == cur->id) continue;
        if (const auto* ns = s_.project.pattern().find(ch.id))
            for (auto& n : *ns)
                if (noteRect(n).contains(p.toFloat())) {
                    // the first click of the double-click placed a note here: take it back
                    if (auto* mine = notes(); mine && placedValid_) {
                        const auto before = mine->size();
                        mine->erase(std::remove_if(mine->begin(), mine->end(), [&](const Note& o) { return o.selected && o.tick == placedTick_; }), mine->end());
                        if (mine->size() != before) s_.publish();
                    }
                    placedValid_ = false;
                    s_.selectChannel(i);
                    return;
                }
    }
}

// The note's values as numbers. If the note is part of a selection, OK applies what was changed to all of it.
void PianoRoll::showNoteProperties(int index) {
    auto* ns = notes();
    if (!ns || index < 0 || index >= (int)ns->size()) return;
    const Note n = (*ns)[(size_t)index];
    auto* w = new juce::AlertWindow("Note properties", juce::String(tools::keyName(n.key)) + (n.selected ? "  (changes go to every selected note)" : ""),
                                    juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor("velocity", juce::String(juce::roundToInt(n.velocity * 127.0f)), "Velocity (1 to 127)");
    w->addTextEditor("pan", juce::String(juce::roundToInt(n.pan * 100.0f)), "Pan (-100 left to 100 right)");
    w->addTextEditor("release", juce::String(juce::roundToInt(n.release * 100.0f)), "Release (0 to 100)");
    w->addTextEditor("modx", juce::String(juce::roundToInt(n.modX * 100.0f)), "Mod X: filter cutoff (0 to 100)");
    w->addTextEditor("mody", juce::String(juce::roundToInt(n.modY * 100.0f)), "Mod Y: resonance (0 to 100)");
    w->addTextEditor("fine", juce::String(juce::roundToInt(n.fine)), "Fine pitch (cents, -1200 to 1200)");
    w->addTextEditor("length", juce::String(n.length), "Length (ticks; 240 is one step)");
    w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    const int id = s_.current() ? s_.current()->id : -1;
    w->enterModalState(true, juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<PianoRoll>(this), w, index, n, id](int result) {
        if (result != 1 || safe == nullptr) return;
        auto* now = safe->notes();
        if (!now || index >= (int)now->size() || !safe->s_.current() || safe->s_.current()->id != id) return;
        auto num = [&](const char* name) { return w->getTextEditorContents(name).getFloatValue(); };
        Note want = n;
        want.velocity = juce::jlimit(1.0f, 127.0f, num("velocity")) / 127.0f;
        want.pan = juce::jlimit(-100.0f, 100.0f, num("pan")) / 100.0f;
        want.release = juce::jlimit(0.0f, 100.0f, num("release")) / 100.0f;
        want.modX = juce::jlimit(0.0f, 100.0f, num("modx")) / 100.0f;
        want.modY = juce::jlimit(0.0f, 100.0f, num("mody")) / 100.0f;
        want.fine = juce::jlimit(-1200.0f, 1200.0f, num("fine"));
        want.length = juce::jlimit(1, 1000000, (int)num("length"));
        // only what was changed is applied, so a selection keeps its differences in everything else
        for (int i = 0; i < (int)now->size(); ++i) {
            auto& t = (*now)[(size_t)i];
            if (i != index && !(n.selected && t.selected)) continue;
            if (want.velocity != n.velocity) t.velocity = want.velocity;
            if (want.pan != n.pan) t.pan = want.pan;
            if (want.release != n.release) t.release = want.release;
            if (want.modX != n.modX) t.modX = want.modX;
            if (want.modY != n.modY) t.modY = want.modY;
            if (want.fine != n.fine) t.fine = want.fine;
            if (want.length != n.length) t.length = want.length;
        }
        safe->edited("Note properties");
    }), true);
}

void PianoRoll::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    const float d = std::fabs(w.deltaY) > std::fabs(w.deltaX) ? w.deltaY : w.deltaX;
    if (e.mods.isCtrlDown()) zoomX(d > 0 ? 1.25 : 0.8, (float)e.x);
    else if (e.mods.isAltDown()) {
        const int topKey = keyOfY((float)e.y);
        keyH_ = juce::jlimit(6, 40, keyH_ + (d > 0 ? 2 : -2));
        scrollY_ = (127 - topKey) * keyH_ - (e.y - gridArea().getY());
        clampScroll(); updateScrollbars(); repaint();
    } else if (e.mods.isShiftDown() || std::fabs(w.deltaX) > std::fabs(w.deltaY)) {
        scrollTick_ -= d * 240.0 / pxPerTick_ * 0.5;
        clampScroll(); updateScrollbars(); repaint();
    } else {
        scrollY_ -= (int)(d * 120.0f);
        clampScroll(); updateScrollbars(); repaint();
    }
}

// ------------------------------------------------------------------ top bar and menus

void PianoRoll::setTool(int t) { tool_ = juce::jlimit(0, (int)NumTools - 1, t); repaint(0, 0, getWidth(), kTopH); }

void PianoRoll::channelMenu() {
    juce::PopupMenu m;
    for (int i = 0; i < (int)s_.project.channels.size(); ++i) {
        const auto* ns = s_.project.pattern().find(s_.project.channels[(size_t)i].id);
        m.addItem(i + 1, juce::String(s_.project.channels[(size_t)i].name) + (ns && !ns->empty() ? "   (" + juce::String((int)ns->size()) + " notes)" : juce::String()), true, i == s_.currentChannel);
    }
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(topButtons_[TChannel])),
                    [safe = juce::Component::SafePointer<PianoRoll>(this)](int r) { if (safe && r > 0) safe->s_.selectChannel(r - 1); });
}

void PianoRoll::topClick(const juce::MouseEvent& e) {
    int hit = -1;
    for (int i = 0; i < numTopButtons_; ++i) if (topButtons_[i].contains(e.getPosition())) hit = i;
    if (hit < 0) return;
    auto safe = juce::Component::SafePointer<PianoRoll>(this);
    auto at = juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(topButtons_[hit]));
    if (hit < NumTools) { setTool(hit); return; }
    switch (hit) {
        case TSnap: {
            juce::PopupMenu m;
            m.addItem(1, "Main (follow the toolbar)", true, snapChoice_ == 0);
            const auto& c = Session::snapChoices();
            for (int i = 0; i < (int)c.size(); ++i) m.addItem(i + 2, c[(size_t)i].first, true, snapChoice_ == i + 1);
            m.showMenuAsync(at, [safe](int r) { if (safe && r > 0) { safe->snapChoice_ = r - 1; safe->repaint(); } });
            break;
        }
        case TChannel: channelMenu(); break;
        case TGhost: ghosts_ = !ghosts_; break;
        case TScale: {
            juce::PopupMenu m, roots;
            m.addItem(1, "None", true, scaleRoot < 0);
            for (int i = 0; i < 12; ++i) roots.addItem(100 + i, juce::String(tools::keyName(i)).dropLastCharacters(1), true, scaleRoot == i);
            m.addSubMenu("Root", roots);
            m.addSeparator();
            for (int i = 0; i < (int)tools::scales().size(); ++i) m.addItem(200 + i, tools::scales()[(size_t)i].name, true, scaleRoot >= 0 && scaleIndex == i);
            m.showMenuAsync(at, [safe](int r) {
                if (!safe || r <= 0) return;
                if (r == 1) safe->scaleRoot = -1;
                else if (r >= 200) { safe->scaleIndex = r - 200; if (safe->scaleRoot < 0) safe->scaleRoot = 0; }
                else if (r >= 100) safe->scaleRoot = r - 100;
                safe->repaint();
            });
            break;
        }
        case TStamp: {
            juce::PopupMenu m;
            m.addItem(1, "None", true, stampChord < 0);
            for (int i = 0; i < (int)tools::chords().size(); ++i) m.addItem(10 + i, tools::chords()[(size_t)i].name, true, stampChord == i);
            m.showMenuAsync(at, [safe](int r) { if (safe && r > 0) { safe->stampChord = r == 1 ? -1 : r - 10; safe->repaint(); } });
            break;
        }
        case TColour: {
            juce::PopupMenu m;
            for (int i = 0; i < 16; ++i) { juce::PopupMenu::Item it; it.itemID = i + 1; it.text = "Colour " + juce::String(i + 1); it.colour = theme::palette(i + 5).brighter(0.4f); it.isTicked = i == colorNext_; m.addItem(it); }
            m.addSeparator();
            m.addItem(100, "Apply this colour to the selected notes");
            m.showMenuAsync(at, [safe](int r) {
                if (!safe || r <= 0) return;
                if (r == 100) { if (auto* ns = safe->notes()) { for (auto& n : *ns) if (n.selected) n.color = safe->colorNext_; safe->edited("Note colour"); } }
                else safe->colorNext_ = r - 1;
                safe->repaint();
            });
            break;
        }
        case TSlide: slideNext_ = !slideNext_; if (slideNext_) portaNext_ = false; break;
        case TPorta: portaNext_ = !portaNext_; if (portaNext_) slideNext_ = false; break;
        case TWave: backdrop_ = !backdrop_; break;
        case TStep:
            s_.stepRecording = !s_.stepRecording;
            if (s_.stepRecording) s_.stepCursor = snapTick(s_.engine.positionTicks(), false);
            s_.notify();
            break;
        case TTools: toolsMenu(this); break;
        default: break;
    }
    repaint();
}

void PianoRoll::rulerMenu(int tick) {
    juce::PopupMenu m;
    auto& markers = s_.project.pattern().markers;
    int near = -1;
    for (int i = 0; i < (int)markers.size(); ++i) if (std::abs(markers[(size_t)i].tick - tick) * pxPerTick_ < 10.0) near = i;
    m.addItem(1, "Add a time marker here...");
    m.addItem(2, "Rename this marker...", near >= 0);
    m.addItem(3, "Delete this marker", near >= 0);
    m.addSeparator();
    m.addItem(4, "Clear the loop range", s_.project.patLoopEnd > s_.project.patLoopStart);
    m.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<PianoRoll>(this), tick, near](int r) {
        if (!safe) return;
        auto& s = safe->s_;
        if (r == 1) askForText("Time marker", "Marker name", "Marker", safe.getComponent(), [safe, tick](juce::String t) {
            if (!safe) return;
            auto& mk = safe->s_.project.pattern().markers;
            mk.push_back({tick, t.toStdString()});
            std::sort(mk.begin(), mk.end(), [](const pulp::PatternMarker& a, const pulp::PatternMarker& b) { return a.tick < b.tick; });
            safe->s_.commit("Add marker");
        });
        else if (r == 2) askForText("Time marker", "Marker name", s.project.pattern().markers[(size_t)near].name, safe.getComponent(), [safe, near](juce::String t) {
            if (!safe || near >= (int)safe->s_.project.pattern().markers.size()) return;
            safe->s_.project.pattern().markers[(size_t)near].name = t.toStdString();
            safe->s_.commit("Rename marker");
        });
        else if (r == 3) { auto& mk = s.project.pattern().markers; mk.erase(mk.begin() + near); s.commit("Delete marker"); }
        else if (r == 4) { s.project.patLoopStart = s.project.patLoopEnd = 0; s.publish(); safe->repaint(); }
    });
}

// ------------------------------------------------------------------ keys

bool PianoRoll::handleKey(const juce::KeyPress& k) {
    auto* ns = notes();
    if (!ns) return false;
    const auto mods = k.getModifiers();
    const int code = k.getKeyCode();
    const bool ctrl = mods.isCtrlDown(), shift = mods.isShiftDown(), alt = mods.isAltDown();
    const bool plain = !ctrl && !shift && !alt;

    if (plain && !s_.typingKeyboard) {
        static const char keys[] = {'P', 'B', 'D', 'T', 'C', 'E', 'Z', 'Y'};
        for (int i = 0; i < NumTools; ++i) if (code == keys[i]) { setTool(i); return true; }
    }
    if (code == juce::KeyPress::deleteKey && plain) { deleteSelected(); return true; }
    if (code == 'A' && ctrl && !shift && !alt) { selectAll(true); return true; }
    if (code == 'D' && ctrl && !shift && !alt) { selectAll(false); return true; }
    if (code == 'C' && ctrl && !alt) { copySelection(false); return true; }
    if (code == 'X' && ctrl && !alt) { copySelection(true); return true; }
    if (code == 'V' && ctrl && !alt) { paste(); return true; }
    if (code == 'B' && ctrl && !alt) { tools::duplicateAfter(*ns); edited("Duplicate notes"); return true; }
    if (code == juce::KeyPress::upKey && (shift || ctrl) && !alt) { tools::transpose(*ns, ctrl ? 12 : 1); edited("Transpose"); return true; }
    if (code == juce::KeyPress::downKey && (shift || ctrl) && !alt) { tools::transpose(*ns, ctrl ? -12 : -1); edited("Transpose"); return true; }
    if (code == juce::KeyPress::leftKey && shift && !ctrl) { tools::shiftTime(*ns, -snap()); edited("Nudge notes"); return true; }
    if (code == juce::KeyPress::rightKey && shift && !ctrl) { tools::shiftTime(*ns, snap()); edited("Nudge notes"); return true; }
    if (code == juce::KeyPress::pageUpKey) { zoomX(1.25, (float)gridArea().getCentreX()); return true; }
    if (code == juce::KeyPress::pageDownKey) { zoomX(0.8, (float)gridArea().getCentreX()); return true; }
    if (code == juce::KeyPress::homeKey) { scrollTick_ = 0; updateScrollbars(); repaint(); return true; }
    if (code == '0' && ctrl) { zoomToFit(); return true; }
    // more of the reference program's keys
    if (code == 'M' && alt && !ctrl) {                  // Alt+M mutes the selected notes, Alt+Shift+M unmutes them
        bool any = false;
        for (auto& n : *ns) if (n.selected) { n.muted = !shift; any = true; }
        if (any) edited(shift ? "Unmute notes" : "Mute notes");
        return true;
    }
    if (code == 'I' && shift && !ctrl && !alt) { for (auto& n : *ns) n.selected = !n.selected; repaint(); return true; }      // invert the selection
    if (plain && !s_.typingKeyboard && code == 'S') { slideNext_ = !slideNext_; repaint(); return true; }                  // new notes are slides
    if (plain && !s_.typingKeyboard && code == 'O') { portaNext_ = !portaNext_; repaint(); return true; }                  // new notes glide
    if (code == 'V' && alt && !ctrl && !shift) { ghosts_ = !ghosts_; repaint(); return true; }                              // ghost notes of other channels
    if (plain && !s_.typingKeyboard && (code == 'G' || code == 'K')) {                                                    // the channel above / below
        const int n = (int)s_.project.channels.size();
        s_.selectChannel(juce::jlimit(0, n - 1, s_.currentChannel + (code == 'G' ? -1 : 1)));
        return true;
    }
    if (code == '4' && shift && !ctrl && !alt) { zoomToFit(); return true; }                                               // show everything
    if (code == juce::KeyPress::escapeKey && dialog_) { runTool(-1); return true; }
    if (code == juce::KeyPress::returnKey && dialog_) { runTool(-2); return true; }

    // the tools menu shortcuts, as in the reference program
    struct KeyTool { int code; bool ctrl, shift, alt; int tool; };
    static const KeyTool map[] = {
        {'Q', true, false, false, 1}, {'Q', false, false, true, 2}, {'L', true, false, false, 3}, {'U', false, false, true, 4},
        {'G', true, false, false, 5}, {'A', false, false, true, 6}, {'S', false, false, true, 7}, {'F', false, false, true, 8},
        {'L', false, false, true, 9}, {'R', false, false, true, 10}, {'X', false, false, true, 11}, {'Y', false, false, true, 12},
        {'O', false, false, true, 14}, {'W', false, false, true, 15}, {'E', false, false, true, 16},
        {'M', true, false, false, 17}, {'M', true, true, false, 18}};
    for (auto& m : map)
        if (code == m.code && ctrl == m.ctrl && shift == m.shift && alt == m.alt) { runTool(m.tool); return true; }
    return false;
}

juce::String PianoRoll::describe() const {
    auto* ch = const_cast<Session&>(s_).current();
    const auto* ns = ch ? s_.project.patterns[(size_t)s_.project.currentPattern].find(ch->id) : nullptr;
    juce::String out = "piano tool=" + juce::String(tool_) + " snap=" + juce::String(snap()) + " notes=" + juce::String(ns ? (int)ns->size() : 0);
    if (ns) {
        int sel = 0;
        for (auto& n : *ns) sel += n.selected ? 1 : 0;
        out << " selected=" << sel << " :";
        int shown = 0;
        for (auto& n : *ns) {
            if (++shown > 24) { out << " ..."; break; }
            out << " " << n.tick << "+" << n.length << "@" << tools::keyName(n.key) << (n.muted ? "m" : "") << (n.slide ? "s" : "") << (n.selected ? "*" : "");
        }
    }
    return out;
}
