#include "ChannelRack.h"
#include "pulp/Sampler.h"
#include "PluginHost.h"

using pulp::kTicksPerStep;
using pulp::Note;
using pulp::Project;

static pulp::ParamInfo swingInfo() {
    pulp::ParamInfo i;
    i.id = "swing"; i.name = "Swing (this pattern)"; i.min = 0; i.max = 1; i.def = 0; i.unit = "%";
    return i;
}

// ------------------------------------------------------------------ Row

class ChannelRack::Row : public juce::Component, public juce::TooltipClient {
public:
    Row(ChannelRack& rack, int index) : rack_(rack), index_(index) {
        auto& ch = rack.s_.project.channels[(size_t)index];
        mute_.setParam(&ch.live->mute);
        mute_.inverted = true;
        mute_.setTooltip("Mute / solo: click to mute, right-click to solo");
        mute_.onRightClick = [this] { solo(); };
        pan_.setParam(&ch.live->pan);
        pan_.bipolar = true;
        pan_.colour = theme::accent2;
        vol_.setParam(&ch.live->volume);
        addAndMakeVisible(mute_);
        addAndMakeVisible(pan_);
        addAndMakeVisible(vol_);
        pan_.onGestureEnd = [this] { rack_.s_.commit("Channel pan"); };
        vol_.onGestureEnd = [this] { rack_.s_.commit("Channel volume"); };
        track_.minValue = 0; track_.maxValue = pulp::kNumInserts; track_.step = 1; track_.fineStep = 1;
        track_.getValue = [this] { return (double)channel().mixerTrack; };
        track_.setValue = [this](double v) { channel().mixerTrack = (int)v; rack_.s_.publish(); };
        track_.onGestureEnd = [this] { rack_.s_.commit("Channel mixer track"); };
        track_.format = [](double v) { return v < 0.5 ? juce::String("--") : juce::String((int)v); };
        track_.setTooltip("Mixer track this channel plays through (-- is the master). Drag or use the wheel; Ctrl+L gives the selected channels free tracks.");
        addAndMakeVisible(track_);
    }

    int index() const { return index_; }
    // The channel's live values may be new objects (after undo, or a project being opened).
    void rebind() {
        auto& ch = rack_.s_.project.channels[(size_t)index_];
        mute_.setParam(&ch.live->mute);
        if (pan_.param() != &ch.live->pan) pan_.setParam(&ch.live->pan);
        if (vol_.param() != &ch.live->volume) vol_.setParam(&ch.live->volume);
    }

    juce::Rectangle<int> nameRect() const { return {114, 3, 112, kRowH - 6}; }
    juce::Rectangle<int> selRect() const { return {229, 8, 10, kRowH - 16}; }
    int stepsLeft() const { return kLeftW; }

    void resized() override {
        mute_.setBounds(4, 6, 18, 18);
        pan_.setBounds(24, 2, 26, 26);
        vol_.setBounds(52, 2, 26, 26);
        track_.setBounds(80, 5, 31, 20);
    }

    juce::String getTooltip() override {
        const auto p = getMouseXYRelative();
        auto& ch = channel();
        if (nameRect().contains(p)) return juce::String(ch.name) + " (" + ch.live->plugin->displayName() + "): click to open, right-click for the channel menu";
        if (selRect().contains(p)) return "Channel selector: click to select, Ctrl-click to add to the selection";
        if (p.x >= stepsLeft()) {
            const int st = rack_.stepAtX(p.x - stepsLeft());
            if (st >= 0) return "Step " + juce::String(st + 1) + ": left-click sets, right-click clears";
        }
        return {};
    }

    void paint(juce::Graphics& g) override {
        auto& s = rack_.s_;
        auto& ch = channel();
        const bool current = s.currentChannel == index_;
        const bool selected = s.selectedChannels.count(ch.id) > 0;
        const juce::Colour col(ch.color);

        auto nr = nameRect().toFloat();
        theme::bevel(g, nr, current ? col : col.darker(0.35f).withMultipliedSaturation(0.8f));
        g.setColour(current ? juce::Colours::black : theme::text);
        g.setFont(theme::font(12.5f, current));
        g.drawFittedText(ch.name, nameRect().reduced(6, 0), juce::Justification::centredLeft, 1, 0.8f);

        g.setColour(theme::inset);
        g.fillRoundedRectangle(selRect().toFloat(), 2.0f);
        if (selected) { g.setColour(theme::good); g.fillRoundedRectangle(selRect().toFloat().reduced(2.0f), 1.5f); }

        // steps
        const auto* notes = s.project.pattern().find(ch.id);
        const int steps = s.project.pattern().steps;
        const bool stepMode = !notes || Project::isStepSequence(*notes);
        g.saveState();
        g.reduceClipRegion(stepsLeft(), 0, getWidth() - stepsLeft(), getHeight());
        const int x0 = stepsLeft() - rack_.stepScroll_;
        if (stepMode) {
            for (int i = 0; i < steps; ++i) {
                const juce::Rectangle<float> r((float)(x0 + stepX(i)), 4.0f, (float)kStepW - 2.0f, (float)kRowH - 8.0f);
                if (r.getRight() < stepsLeft() || r.getX() > getWidth()) continue;
                const bool on = notes && Project::hasStep(*notes, i);
                const bool alt = (i / 4) % 2 == 1;
                juce::Colour c = on ? (alt ? theme::stepOn.withMultipliedBrightness(0.93f) : theme::stepOn) : (alt ? theme::stepOffAlt : theme::stepOff);
                if (i == rack_.playStep_) c = on ? theme::accent : c.brighter(0.25f);
                g.setColour(c);
                g.fillRoundedRectangle(r, 2.5f);
                g.setColour(theme::line);
                g.drawRoundedRectangle(r, 2.5f, 1.0f);
            }
        } else {
            // a mini piano-roll preview for channels that hold more than plain steps
            const juce::Rectangle<float> area((float)x0, 4.0f, (float)stepX(steps), (float)kRowH - 8.0f);
            theme::insetBox(g, area);
            int lo = 127, hi = 0;
            for (auto& n : *notes) { lo = std::min(lo, n.key); hi = std::max(hi, n.key); }
            const float span = (float)std::max(12, hi - lo + 1);
            const float len = (float)std::max(steps * kTicksPerStep, 1);
            g.setColour(col.brighter(0.4f));
            for (auto& n : *notes) {
                const float x = area.getX() + area.getWidth() * (float)n.tick / len;
                const float w = std::max(2.0f, area.getWidth() * (float)n.length / len);
                const float y = area.getBottom() - 3.0f - (area.getHeight() - 5.0f) * (float)(n.key - lo) / span;
                g.fillRect(x, y, w, 2.0f);
            }
        }
        g.restoreState();
    }

    void mouseDown(const juce::MouseEvent& e) override {
        auto& s = rack_.s_;
        if (nameRect().contains(e.getPosition())) {
            // selecting a channel can rebuild the rack's rows, this one included: nothing of this row is
            // touched afterwards
            auto& rack = rack_;
            const int index = index_;
            if (e.mods.isPopupMenu()) { s.selectChannel(index); rack.channelMenu(index, rack.rowFor(index)); }
            else { s.selectChannel(index, e.mods.isCtrlDown() || e.mods.isShiftDown()); if (rack.onOpenChannel) rack.onOpenChannel(index); }
            return;
        }
        if (selRect().expanded(3).contains(e.getPosition())) { s.selectChannel(index_, e.mods.isCtrlDown() || e.mods.isShiftDown()); return; }
        if (e.x >= stepsLeft()) {
            auto& notes = s.project.pattern().notesFor(channel().id);
            if (!Project::isStepSequence(notes)) { s.selectChannel(index_); if (rack_.onOpenPianoRoll) rack_.onOpenPianoRoll(index_); return; }
            painting_ = e.mods.isPopupMenu() ? -1 : 1;
            edited_ = false;
            lastStep_ = -1;
            paintStep(e);
        }
    }
    void mouseDrag(const juce::MouseEvent& e) override { if (painting_ != 0) paintStep(e); }
    void mouseUp(const juce::MouseEvent&) override {
        if (painting_ != 0 && edited_) rack_.s_.commit("Edit steps");
        painting_ = 0;
    }

private:
    pulp::Channel& channel() { return rack_.s_.project.channels[(size_t)index_]; }
    void paintStep(const juce::MouseEvent& e) {
        const int st = rack_.stepAtX(e.x - stepsLeft());
        if (st < 0) return;
        auto& notes = rack_.s_.project.pattern().notesFor(channel().id);
        // a fast drag skips over steps between two mouse positions: fill the gap
        const int from = lastStep_ < 0 ? st : lastStep_;
        lastStep_ = st;
        bool changed = false;
        for (int i = std::min(from, st); i <= std::max(from, st); ++i) {
            if ((painting_ > 0) == Project::hasStep(notes, i)) continue;
            Project::setStep(notes, i, painting_ > 0);
            changed = true;
        }
        if (!changed) return;
        edited_ = true;
        rack_.s_.publish();
        repaint();
        rack_.repaintGraph();
    }
    void solo() {
        auto& chans = rack_.s_.project.channels;
        bool othersMuted = true;
        for (size_t i = 0; i < chans.size(); ++i)
            if ((int)i != index_ && !chans[i].live->mute.getBool()) othersMuted = false;
        const bool unsolo = othersMuted && !chans[(size_t)index_].live->mute.getBool();
        for (size_t i = 0; i < chans.size(); ++i) chans[i].live->mute.set(unsolo ? 0.0f : ((int)i == index_ ? 0.0f : 1.0f));
    }
    ChannelRack& rack_;
    int index_;
    ParamToggle mute_;
    ParamKnob pan_, vol_;
    NumberBox track_;
    int painting_ = 0;
    int lastStep_ = -1;
    bool edited_ = false;
};

// ------------------------------------------------------------------ Graph editor

class ChannelRack::Graph : public juce::Component, public juce::TooltipClient {
public:
    enum Prop { Velocity, Pan, Pitch, Release, ModX, ModY, Shift, NumProps };
    explicit Graph(ChannelRack& rack) : rack_(rack) {}
    static const char* propName(int p) {
        static const char* n[] = {"Velocity", "Pan", "Note pitch", "Release", "Mod X", "Mod Y", "Shift"};
        return n[p];
    }
    juce::Rectangle<int> propRect() const { return {4, 4, kLeftW - 12, 22}; }
    juce::String getTooltip() override {
        if (propRect().contains(getMouseXYRelative())) return "Graph editor property: click to choose which per-step value the bars show";
        return juce::String("Graph editor: drag to set ") + propName(prop_) + " for each step, right-click a bar to reset it";
    }

    static float get(const Note& n, int prop) {
        switch (prop) {
            case Velocity: return n.velocity;
            case Pan: return (n.pan + 1.0f) * 0.5f;
            case Pitch: return juce::jlimit(0.0f, 1.0f, (float)(n.key - 36) / 48.0f);
            case Release: return n.release;
            case ModX: return n.modX;
            case ModY: return n.modY;
            default: return (float)(n.tick % kTicksPerStep) / (float)(kTicksPerStep - 1);
        }
    }
    static void set(Note& n, int prop, float v) {
        v = juce::jlimit(0.0f, 1.0f, v);
        switch (prop) {
            case Velocity: n.velocity = v; break;
            case Pan: n.pan = v * 2.0f - 1.0f; if (std::fabs(n.pan) < 0.04f) n.pan = 0.0f; break;
            case Pitch: n.key = 36 + juce::roundToInt(v * 48.0f); break;
            case Release: n.release = v; break;
            case ModX: n.modX = v; break;
            case ModY: n.modY = v; break;
            default: n.tick = (n.tick / kTicksPerStep) * kTicksPerStep + juce::roundToInt(v * (float)(kTicksPerStep - 1)); break;
        }
    }
    static float defaultValue(int prop) {
        switch (prop) {
            case Velocity: return pulp::kDefaultVelocity;
            case Pan: case Pitch: case Release: case ModX: case ModY: return 0.5f;
            default: return 0.0f;
        }
    }
    static bool centred(int prop) { return prop == Pan || prop == Pitch; }

    void paint(juce::Graphics& g) override {
        g.fillAll(theme::panelLo);
        theme::bevel(g, propRect().toFloat(), theme::panelHi);
        g.setColour(theme::accent);
        g.setFont(theme::font(12.0f, true));
        g.drawText(propName(prop_), propRect().reduced(8, 0), juce::Justification::centredLeft, false);
        g.setColour(theme::textDim);
        g.setFont(theme::font(10.5f));
        auto* ch = rack_.s_.current();
        g.drawFittedText(ch ? juce::String(ch->name) : juce::String(), 8, 30, kLeftW - 16, 16, juce::Justification::centredLeft, 1);

        auto area = getLocalBounds().withTrimmedLeft(kLeftW).reduced(0, 4);
        g.saveState();
        g.reduceClipRegion(area);
        if (ch) {
            const auto* notes = rack_.s_.project.pattern().find(ch->id);
            const int steps = rack_.s_.project.pattern().steps;
            const int x0 = kLeftW - rack_.stepScroll_;
            for (int i = 0; i < steps; ++i) {
                juce::Rectangle<float> col((float)(x0 + stepX(i)), (float)area.getY(), (float)kStepW - 2.0f, (float)area.getHeight());
                g.setColour(theme::inset);
                g.fillRect(col);
                const Note* n = nullptr;
                if (notes) for (auto& nn : *notes) if (nn.tick / kTicksPerStep == i) { n = &nn; break; }
                if (!n) continue;
                const float v = get(*n, prop_);
                g.setColour(juce::Colour(ch->color).brighter(0.3f));
                if (centred(prop_)) {
                    const float mid = col.getCentreY(), y = col.getBottom() - v * col.getHeight();
                    g.fillRect(col.getX() + 2.0f, std::min(mid, y), col.getWidth() - 4.0f, std::max(2.0f, std::fabs(mid - y)));
                } else {
                    const float h = std::max(2.0f, v * col.getHeight());
                    g.fillRect(col.getX() + 2.0f, col.getBottom() - h, col.getWidth() - 4.0f, h);
                }
            }
        }
        g.restoreState();
    }

    void mouseDown(const juce::MouseEvent& e) override {
        if (propRect().contains(e.getPosition())) {
            juce::PopupMenu m;
            for (int i = 0; i < NumProps; ++i) m.addItem(i + 1, propName(i), true, i == prop_);
            m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(propRect())),
                            [safe = juce::Component::SafePointer<Graph>(this)](int r) { if (safe && r > 0) { safe->prop_ = r - 1; safe->repaint(); } });
            return;
        }
        edited_ = false;
        apply(e);
    }
    void mouseDrag(const juce::MouseEvent& e) override { apply(e); }
    void mouseUp(const juce::MouseEvent&) override { if (edited_) rack_.s_.commit("Edit step graph"); edited_ = false; }

private:
    void apply(const juce::MouseEvent& e) {
        auto* ch = rack_.s_.current();
        if (!ch || e.x < kLeftW) return;
        const int st = rack_.stepAtX(e.x - kLeftW);
        if (st < 0) return;
        auto& notes = rack_.s_.project.pattern().notesFor(ch->id);
        Note* n = Project::stepNote(notes, st);
        if (!n) return;
        const auto area = getLocalBounds().reduced(0, 4);
        const float v = e.mods.isPopupMenu() ? defaultValue(prop_) : 1.0f - (float)(e.y - area.getY()) / (float)area.getHeight();
        set(*n, prop_, v);
        edited_ = true;
        rack_.s_.publish();
        repaint();
        for (auto* r : rack_.rows_) r->repaint();
    }
    ChannelRack& rack_;
    int prop_ = Velocity;
    bool edited_ = false;
};

void ChannelRack::repaintGraph() { if (graph_) graph_->repaint(); }

class ChannelRack::Holder : public juce::Component {
public:
    void paint(juce::Graphics& g) override { g.fillAll(theme::panel); }
};

// ------------------------------------------------------------------ ChannelRack

ChannelRack::ChannelRack(Session& s) : s_(s), swing_(swingInfo()), swingKnob_(&swing_) {
    setWantsKeyboardFocus(true);
    holder_ = std::make_unique<Holder>();
    view_.setViewedComponent(holder_.get(), false);
    view_.setScrollBarsShown(true, false);
    view_.setScrollBarThickness(10);
    addAndMakeVisible(view_);
    graph_ = std::make_unique<Graph>(*this);
    addChildComponent(*graph_);
    addChildComponent(hscroll_);
    hscroll_.setAutoHide(false);

    swingKnob_.label = "SWING";
    swingKnob_.onChange = [this] { s_.project.pattern().swing = swing_.get(); s_.publish(); };
    swingKnob_.onGestureEnd = [this] { s_.commit("Swing"); };
    addAndMakeVisible(swingKnob_);

    stepsBox_.caption = "STEPS";
    stepsBox_.minValue = 4; stepsBox_.maxValue = 256; stepsBox_.step = 4; stepsBox_.fineStep = 1;
    stepsBox_.getValue = [this] { return (double)s_.project.pattern().steps; };
    stepsBox_.setValue = [this](double v) { s_.project.pattern().steps = (int)v; s_.commit("Pattern length"); };
    stepsBox_.format = [](double v) { return juce::String((int)v); };
    stepsBox_.setTooltip("Steps in this pattern. Drag or use the wheel (4 at a time, Ctrl for single steps), double-click to type.");
    addAndMakeVisible(stepsBox_);

    graphBtn_.text = "GRAPH";
    graphBtn_.setTooltip("Graph editor: per-step velocity, pan, pitch, release, mod X, mod Y and shift for the selected channel");
    graphBtn_.onClick = [this] { showGraph_ = !showGraph_; graphBtn_.setLit(showGraph_); resized(); };
    addAndMakeVisible(graphBtn_);

    groupBtn_.text = "All";
    groupBtn_.setTooltip("Channel group filter: show all channels or one group");
    groupBtn_.onClick = [this] { groupMenu(); };
    addAndMakeVisible(groupBtn_);

    addBtn_.text = "+";
    addBtn_.setTooltip("Add a channel");
    addBtn_.onClick = [this] { addChannelMenu(&addBtn_); };
    holder_->addAndMakeVisible(addBtn_);

    s_.addListener(this);
    rebuild();
}

ChannelRack::~ChannelRack() { s_.removeListener(this); }

bool ChannelRack::rowVisible(const pulp::Channel& c) const { return filter_.isEmpty() || juce::String(c.group) == filter_; }

juce::Component* ChannelRack::rowFor(int index) const {
    for (auto* r : rows_) if (r->index() == index) return r;
    return nullptr;
}

void ChannelRack::rebuild() {
    // Rows are kept when the same channels are showing: most changes arrive while a row is in the middle of
    // handling a click, and a row must not be destroyed under its own feet.
    std::vector<int> showing;
    for (int i = 0; i < (int)s_.project.channels.size(); ++i)
        if (rowVisible(s_.project.channels[(size_t)i])) showing.push_back(i);
    bool same = rows_.size() == (int)showing.size();
    for (int k = 0; same && k < rows_.size(); ++k) same = rows_[k]->index() == showing[(size_t)k];
    int y = 0;
    const int w = juce::jmax(100, view_.getMaximumVisibleWidth());
    if (!same) {
        rows_.clear();
        for (int i : showing) holder_->addAndMakeVisible(rows_.add(new Row(*this, i)));
    }
    for (auto* r : rows_) {
        r->rebind();
        r->setBounds(0, y, w, kRowH);
        r->repaint();
        y += kRowH;
    }
    addBtn_.setBounds(116, y + 4, 60, 20);
    holder_->setSize(w, y + 30);
    swing_.set(s_.project.pattern().swing);
    groupBtn_.text = filter_.isEmpty() ? "All" : filter_;
    groupBtn_.repaint();
    const int total = stepX(s_.project.pattern().steps);
    const int visible = juce::jmax(1, getWidth() - kLeftW - 14);
    hscroll_.setVisible(total > visible);
    hscroll_.setRangeLimits(0.0, (double)total);
    stepScroll_ = juce::jlimit(0, juce::jmax(0, total - visible), stepScroll_);
    hscroll_.setCurrentRange((double)stepScroll_, (double)visible, juce::dontSendNotification);
    if (graph_) graph_->repaint();
    repaint();
}

void ChannelRack::projectChanged() { rebuild(); }

void ChannelRack::resized() {
    auto r = getLocalBounds();
    auto head = r.removeFromTop(kHeaderH).reduced(4, 2);
    swingKnob_.setBounds(head.removeFromLeft(44));
    head.removeFromLeft(6);
    stepsBox_.setBounds(head.removeFromLeft(58));
    head.removeFromLeft(6);
    graphBtn_.setBounds(head.removeFromLeft(58).reduced(0, 3));
    head.removeFromLeft(6);
    groupBtn_.setBounds(head.removeFromLeft(110).reduced(0, 3));
    if (showGraph_) graph_->setBounds(r.removeFromBottom(kGraphH));
    graph_->setVisible(showGraph_);
    const int total = stepX(s_.project.pattern().steps);
    if (total > r.getWidth() - kLeftW - 14) hscroll_.setBounds(r.removeFromBottom(10).withTrimmedLeft(kLeftW));
    view_.setBounds(r);
    rebuild();
}

void ChannelRack::paint(juce::Graphics& g) {
    g.fillAll(theme::panel);
    g.setColour(theme::panelLo);
    g.fillRect(0, 0, getWidth(), kHeaderH);
    g.setColour(theme::textDim);
    g.setFont(theme::font(11.0f));
    g.drawText(juce::String(s_.project.pattern().name), getLocalBounds().removeFromTop(kHeaderH).reduced(8, 0), juce::Justification::centredRight, true);
}

int ChannelRack::stepAtX(int x) const {
    x += stepScroll_;
    if (x < 0) return -1;
    const int beatW = 4 * kStepW + kBeatGap;
    const int beat = x / beatW, in = x % beatW;
    const int st = beat * 4 + juce::jmin(3, in / kStepW);
    return st < s_.project.pattern().steps ? st : -1;
}

void ChannelRack::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    if (e.mods.isShiftDown() || std::fabs(w.deltaX) > std::fabs(w.deltaY)) {
        const int total = stepX(s_.project.pattern().steps);
        const int visible = juce::jmax(1, getWidth() - kLeftW - 14);
        const float d = std::fabs(w.deltaX) > std::fabs(w.deltaY) ? w.deltaX : w.deltaY;
        stepScroll_ = juce::jlimit(0, juce::jmax(0, total - visible), stepScroll_ - (int)(d * 200.0f));
        rebuild();
    } else juce::Component::mouseWheelMove(e, w);
}

void ChannelRack::uiTick() {
    int st = -1;
    if (s_.engine.isPlaying() && !s_.project.songMode) st = (int)(s_.engine.positionTicks() / kTicksPerStep);
    if (st != playStep_) {
        playStep_ = st;
        for (auto* r : rows_) r->repaint(kLeftW, 0, r->getWidth() - kLeftW, kRowH);
    }
    if ((double)stepScroll_ != hscroll_.getCurrentRangeStart() && hscroll_.isVisible()) {
        stepScroll_ = (int)hscroll_.getCurrentRangeStart();
        for (auto* r : rows_) r->repaint();
        if (graph_) graph_->repaint();
    }
}

// ------------------------------------------------------------------ step operations

void ChannelRack::fillSteps(int channel, int every) {
    auto* c = s_.channel(channel);
    if (!c) return;
    auto& notes = s_.project.pattern().notesFor(c->id);
    for (int i = 0; i < s_.project.pattern().steps; i += every) Project::setStep(notes, i, true);
    s_.commit("Fill steps");
}

void ChannelRack::rotateSteps(int channel, int delta) {
    auto* c = s_.channel(channel);
    if (!c) return;
    auto& notes = s_.project.pattern().notesFor(c->id);
    const int len = s_.project.pattern().steps * kTicksPerStep;
    for (auto& n : notes) n.tick = ((n.tick + delta * kTicksPerStep) % len + len) % len;
    pulp::sortNotes(notes);
    s_.commit("Rotate steps");
}

void ChannelRack::clearSteps(int channel) {
    auto* c = s_.channel(channel);
    if (!c) return;
    s_.project.pattern().notesFor(c->id).clear();
    s_.commit("Clear steps");
}

// ------------------------------------------------------------------ menus

void ChannelRack::addChannelMenu(juce::Component* target) {
    juce::PopupMenu m;
    m.addSectionHeader("Add channel");
    int id = 100;
    std::vector<std::string> ids;
    for (auto& t : pulp::PluginRegistry::global().types())
        if (t.instrument) { m.addItem(id++, juce::String(t.name) + "   (" + juce::String(t.category) + ")"); ids.push_back(t.id); }
    {
        // instruments from other makers
        juce::PopupMenu vst;
        for (auto& e : PluginHost::get().plugins())
            if (e.instrument) { vst.addItem(id++, e.name + (e.maker.isEmpty() ? juce::String() : "   (" + e.maker + ")")); ids.push_back(e.typeId); }
        vst.addSeparator();
        vst.addItem(2, "Find VST3 plugins...");
        m.addSubMenu("VST3 instruments", vst);
    }
    m.addSeparator();
    m.addItem(1, "Audio file...");
    m.addItem(3, "SoundFont preset (.sf2)...");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target), [this, ids](int r) {
        if (r >= 100 && r - 100 < (int)ids.size()) {
            const int index = s_.addInstrumentChannel(ids[(size_t)(r - 100)]);
            if (index < 0) { if (s_.onMessage) s_.onMessage("That instrument could not be loaded."); }
            else if (PluginHost::isHosted(s_.project.channels[(size_t)index].live->plugin.get()) && onOpenChannel) onOpenChannel(index);
        }
        else if (r == 2) { if (s_.onShowSettings) s_.onShowSettings(2); }
        else if (r == 3) {
            auto chooser = std::make_shared<juce::FileChooser>("A SoundFont", juce::File(), "*.sf2");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                 [safe = juce::Component::SafePointer<ChannelRack>(this), chooser](const juce::FileChooser& fc) {
                if (safe != nullptr && fc.getResult().existsAsFile()) safe->soundFontMenu(fc.getResult());
            });
        }
        else if (r == 1) {
            auto chooser = std::make_shared<juce::FileChooser>("Add an audio file as a channel", Session::factoryDir(), Session::audioFilePattern());
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                                 [this, chooser](const juce::FileChooser& fc) { for (auto& f : fc.getResults()) s_.addSampleChannel(f); });
        }
    });
}

void ChannelRack::channelMenu(int index, juce::Component* target) {
    auto* c = s_.channel(index);
    if (!c) return;
    juce::PopupMenu m, colours, replace, group;
    m.addSectionHeader(c->name);
    m.addItem(1, "Rename...");
    for (int i = 0; i < 16; ++i) {
        juce::PopupMenu::Item it;
        it.itemID = 200 + i;
        it.text = "Colour " + juce::String(i + 1);
        it.colour = theme::palette(i).brighter(0.3f);
        colours.addItem(it);
    }
    m.addSubMenu("Colour", colours);
    std::vector<std::string> ids;
    int id = 300;
    for (auto& t : pulp::PluginRegistry::global().types())
        if (t.instrument) { replace.addItem(id++, t.name); ids.push_back(t.id); }
    for (auto& e : PluginHost::get().plugins())
        if (e.instrument) { replace.addItem(id++, e.name + "   (VST3)"); ids.push_back(e.typeId); }
    m.addSubMenu("Replace instrument", replace);
    m.addItem(2, "Clone");
    m.addItem(3, "Delete", s_.project.channels.size() > 1);
    m.addSeparator();
    m.addItem(10, "Fill each 2 steps");
    m.addItem(11, "Fill each 4 steps");
    m.addItem(12, "Fill each 8 steps");
    m.addItem(13, "Rotate left  (Shift+Ctrl+Left)");
    m.addItem(14, "Rotate right  (Shift+Ctrl+Right)");
    m.addItem(15, "Clear this channel's notes");
    if (onOpenPianoRoll) m.addItem(16, "Send to piano roll");
    m.addSeparator();
    juce::StringArray groups;
    for (auto& ch : s_.project.channels) if (!ch.group.empty()) groups.addIfNotAlreadyThere(ch.group);
    group.addItem(400, "New group from selected channels...");
    for (int i = 0; i < groups.size(); ++i) group.addItem(410 + i, "Add selected to \"" + groups[i] + "\"");
    group.addItem(401, "Remove selected from their group", !c->group.empty());
    m.addSubMenu("Group", group);
    if (std::string(c->live->plugin->typeId()) == "layer") {
        m.addSeparator();
        m.addItem(20, "Layer: set children to the selected channels");
        m.addItem(21, "Layer: clear children  (" + juce::String((int)c->layerChildren.size()) + " now)");
    }
    m.addItem(24, "Route to a free mixer track  (Ctrl+L)", c->mixerTrack == 0);
    m.addItem(22, "Move up  (Alt+Up)", index > 0);
    m.addItem(23, "Move down  (Alt+Down)", index + 1 < (int)s_.project.channels.size());

    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target), [this, index, ids, groups](int r) {
        auto* ch = s_.channel(index);
        if (!ch || r == 0) return;
        auto selectedOrThis = [&]() {
            std::vector<int> v;
            for (auto& c2 : s_.project.channels) if (s_.selectedChannels.count(c2.id)) v.push_back(c2.id);
            if (v.empty()) v.push_back(ch->id);
            return v;
        };
        if (r == 1) askForText("Rename channel", "Channel name", ch->name, this, [this, index](juce::String t) {
            if (auto* c2 = s_.channel(index); c2 && t.trim().isNotEmpty()) { c2->name = t.trim().toStdString(); s_.commit("Rename channel"); }
        });
        else if (r == 2) s_.cloneChannel(index);
        else if (r == 3) s_.deleteChannel(index);
        else if (r == 10) fillSteps(index, 2);
        else if (r == 11) fillSteps(index, 4);
        else if (r == 12) fillSteps(index, 8);
        else if (r == 13) rotateSteps(index, -1);
        else if (r == 14) rotateSteps(index, 1);
        else if (r == 15) clearSteps(index);
        else if (r == 16) { if (onOpenPianoRoll) onOpenPianoRoll(index); }
        else if (r == 20) {
            ch->layerChildren.clear();
            for (int cid : selectedOrThis()) if (cid != ch->id) ch->layerChildren.push_back(cid);
            s_.commit("Set layer children");
        }
        else if (r == 21) { ch->layerChildren.clear(); s_.commit("Clear layer children"); }
        else if (r == 24) { s_.selectChannel(index); routeToFreeTracks(); }
        else if (r == 22) s_.moveChannel(index, -1);
        else if (r == 23) s_.moveChannel(index, 1);
        else if (r >= 200 && r < 216) { ch->color = theme::palette(r - 200).getARGB(); s_.commit("Channel colour"); }
        else if (r >= 300 && r - 300 < (int)ids.size()) s_.replaceChannelInstrument(index, ids[(size_t)(r - 300)]);
        else if (r == 400) {
            const auto sel = selectedOrThis();
            askForText("New channel group", "Group name", "Group", this, [this, sel](juce::String t) {
                if (t.trim().isEmpty()) return;
                for (auto& c2 : s_.project.channels) if (std::find(sel.begin(), sel.end(), c2.id) != sel.end()) c2.group = t.trim().toStdString();
                s_.commit("Channel group");
            });
        }
        else if (r == 401) {
            const auto sel = selectedOrThis();
            for (auto& c2 : s_.project.channels) if (std::find(sel.begin(), sel.end(), c2.id) != sel.end()) c2.group.clear();
            s_.commit("Channel group");
        }
        else if (r >= 410 && r - 410 < groups.size()) {
            const auto sel = selectedOrThis();
            for (auto& c2 : s_.project.channels) if (std::find(sel.begin(), sel.end(), c2.id) != sel.end()) c2.group = groups[r - 410].toStdString();
            s_.commit("Channel group");
        }
    });
}

void ChannelRack::groupMenu() {
    juce::PopupMenu m;
    juce::StringArray groups;
    for (auto& ch : s_.project.channels) if (!ch.group.empty()) groups.addIfNotAlreadyThere(ch.group);
    m.addItem(1, "All", true, filter_.isEmpty());
    for (int i = 0; i < groups.size(); ++i) m.addItem(10 + i, groups[i], true, filter_ == groups[i]);
    if (groups.isEmpty()) m.addItem(2, "(right-click a channel and choose Group to make one)", false);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&groupBtn_), [this, groups](int r) {
        if (r == 1) filter_.clear();
        else if (r >= 10 && r - 10 < groups.size()) filter_ = groups[r - 10];
        else return;
        rebuild();
    });
}

// ------------------------------------------------------------------ keys and drops

bool ChannelRack::handleKey(const juce::KeyPress& k) {
    const auto mods = k.getModifiers();
    const int code = k.getKeyCode();
    const int n = (int)s_.project.channels.size();
    if (code == juce::KeyPress::upKey && mods.isAltDown()) { s_.moveChannel(s_.currentChannel, -1); return true; }
    if (code == juce::KeyPress::downKey && mods.isAltDown()) { s_.moveChannel(s_.currentChannel, 1); return true; }
    if (code == juce::KeyPress::upKey && !mods.isAnyModifierKeyDown()) { s_.selectChannel(juce::jmax(0, s_.currentChannel - 1)); return true; }
    if (code == juce::KeyPress::downKey && !mods.isAnyModifierKeyDown()) { s_.selectChannel(juce::jmin(n - 1, s_.currentChannel + 1)); return true; }
    if (code == juce::KeyPress::leftKey && mods.isShiftDown() && mods.isCtrlDown()) { rotateSteps(s_.currentChannel, -1); return true; }
    if (code == juce::KeyPress::rightKey && mods.isShiftDown() && mods.isCtrlDown()) { rotateSteps(s_.currentChannel, 1); return true; }
    if (code == juce::KeyPress::deleteKey && mods.isAltDown()) { if (n > 1) s_.deleteChannel(s_.currentChannel); return true; }
    if (code == 'C' && mods.isAltDown()) { s_.cloneChannel(s_.currentChannel); return true; }
    if (code == 'L' && mods.isCtrlDown() && !mods.isAltDown() && !mods.isShiftDown()) { routeToFreeTracks(); return true; }
    // 1 to 9 and 0 mute or unmute the first ten channels; with Ctrl they solo
    if (code >= '0' && code <= '9' && !mods.isAltDown() && !mods.isShiftDown() && !s_.typingKeyboard) {
        const int index = code == '0' ? 9 : code - '1';
        if (index >= n) return true;
        auto& chans = s_.project.channels;
        if (mods.isCtrlDown()) {
            bool othersMuted = true;
            for (int i = 0; i < n; ++i) if (i != index && !chans[(size_t)i].live->mute.getBool()) othersMuted = false;
            const bool unsolo = othersMuted && !chans[(size_t)index].live->mute.getBool();
            for (int i = 0; i < n; ++i) chans[(size_t)i].live->mute.set(unsolo ? 0.0f : (i == index ? 0.0f : 1.0f));
        } else chans[(size_t)index].live->mute.set(chans[(size_t)index].live->mute.getBool() ? 0.0f : 1.0f);
        s_.commit(mods.isCtrlDown() ? "Solo channel" : "Mute channel");
        return true;
    }
    // Shift with the arrows grows the selection
    if ((code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) && mods.isShiftDown() && !mods.isCtrlDown() && !mods.isAltDown()) {
        s_.selectChannel(juce::jlimit(0, n - 1, s_.currentChannel + (code == juce::KeyPress::upKey ? -1 : 1)), true);
        return true;
    }
    // the channel's steps or notes in this pattern: copy, cut, paste
    if ((code == 'C' || code == 'X' || code == 'V') && mods.isCtrlDown() && !mods.isAltDown() && !mods.isShiftDown()) {
        static std::vector<pulp::Note> clipboard;
        auto* ch = s_.current();
        if (!ch) return true;
        auto& notes = s_.project.pattern().notesFor(ch->id);
        if (code == 'V') { if (clipboard.empty()) return true; notes = clipboard; s_.commit("Paste steps"); return true; }
        clipboard = notes;
        if (code == 'X') { notes.clear(); s_.commit("Cut steps"); }
        return true;
    }
    return false;
}

void ChannelRack::routeToFreeTracks() {
    auto& p = s_.project;
    bool any = false;
    for (auto& ch : p.channels) {
        if (!s_.selectedChannels.count(ch.id) || ch.mixerTrack != 0) continue;
        const int t = p.firstFreeMixerTrack();
        if (t < 0) break;
        ch.mixerTrack = t;
        p.mixer[(size_t)t].name = ch.name;
        p.mixer[(size_t)t].color = juce::Colour(ch.color).darker(0.25f).getARGB();
        any = true;
    }
    if (any) s_.commit("Route channels to free mixer tracks");
}

// The presets of a SoundFont as a menu; the one chosen becomes a channel.
void ChannelRack::soundFontMenu(const juce::File& file) {
    const auto names = s_.soundFontPresets(file);
    if (names.isEmpty()) return;
    // sixteen to a group, so that a General MIDI bank reads as its families
    juce::PopupMenu menu;
    menu.addSectionHeader(file.getFileName() + ": which preset?");
    if (names.size() <= 24) for (int i = 0; i < names.size(); ++i) menu.addItem(i + 1, names[i]);
    else for (int a = 0; a < names.size(); a += 16) {
        juce::PopupMenu sub;
        for (int i = a; i < juce::jmin(names.size(), a + 16); ++i) sub.addItem(i + 1, names[i]);
        menu.addSubMenu(names[a].fromFirstOccurrenceOf("  ", false, false) + " ...", sub);
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [safe = juce::Component::SafePointer<ChannelRack>(this), file](int choice) {
        if (safe != nullptr && choice > 0) safe->s_.addSoundFontChannel(file, choice - 1);
    });
}

void ChannelRack::dropFiles(const juce::StringArray& files, juce::Point<int> where) {
    // Dropping on a sampler channel's button replaces its sample; anywhere else adds channels.
    const auto inHolder = holder_->getLocalPoint(this, where);
    if (files.size() == 1)
        for (int i = 0; i < rows_.size(); ++i) {
            auto* row = rows_[i];
            const auto local = row->getLocalPoint(holder_.get(), inHolder);
            if (row->getBounds().contains(inHolder) && row->nameRect().contains(local)) {
                int chanIndex = -1, seen = 0;
                for (int c = 0; c < (int)s_.project.channels.size(); ++c)
                    if (rowVisible(s_.project.channels[(size_t)c]) && seen++ == i) { chanIndex = c; break; }
                if (chanIndex >= 0 && s_.setChannelSample(chanIndex, juce::File(files[0]))) return;
            }
        }
    for (auto& f : files) {
        const juce::File file(f);
        if (file.hasFileExtension("sf2")) { soundFontMenu(file); return; }
        if (file.hasFileExtension("mid;midi")) { const auto what = s_.importMidiSong(file); if (what.isNotEmpty() && s_.onMessage) s_.onMessage(what); continue; }
        if (Session::isInstrumentFile(file)) s_.addInstrumentFileChannel(file);
        else if (Session::isAudioFile(file)) s_.addSampleChannel(file);
    }
}

bool ChannelRack::isInterestedInFileDrag(const juce::StringArray& files) {
    for (auto& f : files) if (Session::isAudioFile(juce::File(f)) || Session::isInstrumentFile(juce::File(f))) return true;
    return false;
}
void ChannelRack::filesDropped(const juce::StringArray& files, int x, int y) { dropFiles(files, {x, y}); }

bool ChannelRack::isInterestedInDragSource(const SourceDetails& d) {
    const auto s = d.description.toString();
    return s.startsWith("file:") || s.startsWith("plugin:");
}
void ChannelRack::itemDropped(const SourceDetails& d) {
    const auto s = d.description.toString();
    if (s.startsWith("file:")) dropFiles({s.substring(5)}, d.localPosition);
    else if (s.startsWith("plugin:")) {
        // "plugin:<type>" or "plugin:<type>#<preset>"
        const auto id = s.substring(7);
        s_.addInstrumentChannel(id.upToFirstOccurrenceOf("#", false, false).toStdString(), id.contains("#") ? id.fromFirstOccurrenceOf("#", false, false).getIntValue() : -1);
    }
}
