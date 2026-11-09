#include "MixerView.h"
#include "PluginHost.h"

using pulp::kCurrentTrack;
using pulp::kMasterTrack;
using pulp::kNumMixerTracks;
using pulp::kNumSlots;
using pulp::Project;
using pulp::TrackLive;

// ------------------------------------------------------------------ one strip

class MixerView::Strip : public juce::Component, public juce::TooltipClient {
public:
    Strip(MixerView& m, int index) : m_(m), index_(index) {
        auto& live = *m.s_.project.mixer[(size_t)index].live;
        pan_.setParam(&live.pan);
        pan_.bipolar = true;
        pan_.colour = theme::accent2;
        sep_.setParam(&live.stereoSep);
        sep_.bipolar = true;
        sep_.colour = theme::good;
        mute_.setParam(&live.mute);
        mute_.inverted = true;
        mute_.setTooltip("Mute / solo: click to mute, right-click to solo");
        mute_.onRightClick = [this] { solo(); };
        send_.colour = theme::warn;
        addAndMakeVisible(pan_);
        addAndMakeVisible(sep_);
        addAndMakeVisible(mute_);
        addChildComponent(send_);
        for (auto* k : {&pan_, &sep_, &send_}) k->onGestureEnd = [this] { m_.s_.commit("Mixer control"); };
    }

    int index() const { return index_; }
    juce::Rectangle<int> nameRect() const { return {1, 1, getWidth() - 2, 34}; }
    juce::Rectangle<int> faderRect() const { return {2, faderTop_, getWidth() - 4, juce::jmax(20, faderBottom_ - faderTop_)}; }
    juce::Rectangle<int> arrowRect() const { return {2, getHeight() - 18, getWidth() - 4, 16}; }

    void resized() override {
        const int w = getWidth();
        int y = 38;
        pan_.setBounds((w - 26) / 2, y, 26, 26);
        y += 28;
        const bool showSep = w >= 50;
        sep_.setVisible(showSep);
        if (showSep) { sep_.setBounds((w - 26) / 2, y, 26, 26); y += 28; }
        faderTop_ = y + 2;
        faderBottom_ = getHeight() - 66;
        mute_.setBounds((w - 18) / 2, getHeight() - 62, 18, 16);
        send_.setBounds((w - 24) / 2, getHeight() - 44, 24, 24);
    }

    void refresh() {
        // the project's mixer may have been rebuilt (open, undo): point the controls at the live track again
        auto& l = live();
        pan_.setParam(&l.pan);
        sep_.setParam(&l.stereoSep);
        mute_.setParam(&l.mute);
        // the send knob appears on tracks the selected track feeds
        const int sel = m_.s_.project.selectedMixerTrack;
        const pulp::Route* r = sel != index_ ? m_.s_.project.mixer[(size_t)sel].routeTo(index_) : nullptr;
        send_.setParam(r && !r->sidechain ? r->level.get() : nullptr);
        send_.setVisible(r && !r->sidechain);
        repaint();
    }

    juce::String getTooltip() override {
        const auto p = getMouseXYRelative();
        const auto name = trackName();
        if (nameRect().contains(p)) return name + ": click to select, double-click to rename, right-click for the track menu";
        if (faderRect().contains(p)) return name + " volume: " + juce::String(pulp::gainToDb(TrackLive::faderToGain(live().volume.get())), 1) + " dB. Drag, Ctrl for fine, Alt-click to reset, right-click for automation and links.";

        if (arrowRect().contains(p)) {
            const int sel = m_.s_.project.selectedMixerTrack;
            if (sel == index_) return "Select another track, then click its arrow to send this track there";
            return "Send from " + juce::String(Project::mixerTrackName(sel)) + " to " + name + ": click to switch it on or off, right-click for sidechain and route-only";
        }
        return {};
    }

    void paint(juce::Graphics& g) override {
        auto& proj = m_.s_.project;
        auto& tr = proj.mixer[(size_t)index_];
        const bool selected = proj.selectedMixerTrack == index_;
        g.fillAll(selected ? theme::panelHi : theme::panel);
        g.setColour(theme::line);
        g.drawVerticalLine(getWidth() - 1, 0.0f, (float)getHeight());

        // name
        const juce::Colour col(tr.color);
        g.setColour(selected ? col.brighter(0.5f) : col);
        g.fillRoundedRectangle(nameRect().toFloat().reduced(1.0f), 2.0f);
        g.setColour(selected ? juce::Colours::black : theme::text);
        g.setFont(theme::font(getWidth() < 50 ? 9.5f : 10.5f, selected));
        g.drawFittedText(trackName(), nameRect().reduced(2, 1), juce::Justification::centred, 3, 0.75f);

        // meter and fader
        const auto fr = faderRect();
        const auto meter = fr.withWidth(fr.getWidth() / 2 - 1).reduced(1, 0);
        const auto slot = fr.withLeft(fr.getX() + fr.getWidth() / 2 + 1).reduced(1, 0);
        theme::insetBox(g, meter.toFloat());
        for (int c = 0; c < 2; ++c) {
            auto bar = meter.reduced(2).toFloat();
            bar = bar.withWidth(bar.getWidth() * 0.5f - 0.5f).translated(c * (bar.getWidth() * 0.5f + 0.5f), 0.0f);
            const float db = pulp::gainToDb(level_[c]);
            const float frac = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 66.0f);
            g.setColour(level_[c] > 1.0f ? theme::bad : (db > -6.0f ? theme::warn : theme::good));
            g.fillRect(bar.withTop(bar.getBottom() - bar.getHeight() * frac));
        }
        theme::insetBox(g, slot.toFloat().reduced((float)slot.getWidth() * 0.3f, 0.0f));
        const float v = live().volume.get();
        const float ky = (float)fr.getBottom() - 6.0f - v * ((float)fr.getHeight() - 12.0f);
        const juce::Rectangle<float> knob((float)slot.getX(), ky - 5.0f, (float)slot.getWidth(), 10.0f);
        theme::bevel(g, knob, dragging_ ? theme::accent : theme::text.withAlpha(0.85f), 2.0f);
        // 0 dB mark
        const float zy = (float)fr.getBottom() - 6.0f - 0.8f * ((float)fr.getHeight() - 12.0f);
        g.setColour(theme::textDim);
        g.drawHorizontalLine((int)zy, (float)slot.getX() - 1.0f, (float)slot.getX() + 3.0f);

        // routing arrow
        const int sel = proj.selectedMixerTrack;
        const auto ar = arrowRect().toFloat();
        if (sel != index_ && index_ != kCurrentTrack) {
            const pulp::Route* r = proj.mixer[(size_t)sel].routeTo(index_);
            const bool can = r != nullptr || proj.canRoute(sel, index_);
            juce::Path p;
            p.addTriangle(ar.getCentreX() - 6.0f, ar.getBottom() - 3.0f, ar.getCentreX() + 6.0f, ar.getBottom() - 3.0f, ar.getCentreX(), ar.getY() + 3.0f);
            g.setColour(r ? (r->sidechain ? theme::accent2 : theme::warn) : (can ? theme::panelHi.brighter(0.25f) : theme::panelLo));
            g.fillPath(p);
            if (r && r->sidechain) { g.setFont(theme::font(8.5f, true)); g.setColour(theme::accent2); g.drawText("SC", ar.withTrimmedLeft(ar.getWidth() * 0.5f + 5.0f), juce::Justification::centredLeft, false); }
        }
        // effect dots: one per used slot
        int used = 0;
        for (auto& sl : tr.slots) used += sl ? 1 : 0;
        g.setColour(theme::accent);
        for (int i = 0; i < used; ++i) g.fillRect(3 + i * 4, 35, 3, 2);
    }

    void mouseDown(const juce::MouseEvent& e) override {
        auto& s = m_.s_;
        if (nameRect().contains(e.getPosition())) {
            m_.selectTrack(index_);
            if (e.mods.isPopupMenu()) m_.trackMenu(index_, this);
            return;
        }
        if (arrowRect().contains(e.getPosition())) {
            const int sel = s.project.selectedMixerTrack;
            if (sel == index_ || index_ == kCurrentTrack) return;
            if (!e.mods.isPopupMenu()) { m_.toggleRoute(sel, index_); return; }
            juce::PopupMenu m;
            m.addItem(1, "Route to this track only");
            m.addItem(2, "Sidechain to this track");
            m.addItem(3, "Remove this route", s.project.mixer[(size_t)sel].routeTo(index_) != nullptr);
            const int to = index_;
            m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [safe = juce::Component::SafePointer<Strip>(this), sel, to](int r) {
                if (!safe || r <= 0) return;
                auto& proj = safe->m_.s_.project;
                if (r == 1 && proj.canRoute(sel, to)) { proj.mixer[(size_t)sel].routes.clear(); proj.setRoute(sel, to, true); }
                else if (r == 1 && proj.mixer[(size_t)sel].routeTo(to)) { auto keep = *proj.mixer[(size_t)sel].routeTo(to); keep.sidechain = false; proj.mixer[(size_t)sel].routes = {keep}; }
                else if (r == 2) proj.setRoute(sel, to, true, true);
                else if (r == 3) proj.setRoute(sel, to, false);
                safe->m_.s_.commit("Mixer routing");
            });
            return;
        }
        if (faderRect().contains(e.getPosition())) {
            m_.selectTrack(index_);
            if (e.mods.isPopupMenu()) {     // the menu every control has: reset, type in, automation clip, link, events
                ParamUi::showMenu(&live().volume, this, [mv = &m_] { mv->s_.commit("Fader"); });
                return;
            }
            if (e.mods.isAltDown()) { ParamUi::nudge(&live().volume, 0.8f); s.commit("Reset fader"); return; }
            dragging_ = true;
            dragStart_ = live().volume.get();
            ParamUi::touch(&live().volume, 0);
            repaint();
        }
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (!dragging_) return;
        const float range = (float)juce::jmax(40, faderRect().getHeight() - 12) * (e.mods.isCtrlDown() ? 8.0f : 1.0f);
        live().volume.set(dragStart_ - (float)e.getDistanceFromDragStartY() / range);
        ParamUi::touch(&live().volume, 1);
        repaint();
    }
    void mouseUp(const juce::MouseEvent&) override {
        if (dragging_) { dragging_ = false; ParamUi::touch(&live().volume, 2); m_.s_.commit("Fader"); }
    }
    void mouseDoubleClick(const juce::MouseEvent& e) override {
        if (!nameRect().contains(e.getPosition())) return;
        const int i = index_;
        auto* mv = &m_;
        askForText("Rename mixer track", "Track name", m_.s_.project.mixer[(size_t)i].name, this, [mv, i](juce::String t) {
            mv->s_.project.mixer[(size_t)i].name = t.trim().toStdString();
            mv->s_.commit("Rename mixer track");
        });
    }
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override {
        if (faderRect().contains(e.getPosition())) { ParamUi::nudge(&live().volume, live().volume.get() + (w.deltaY > 0 ? 0.01f : -0.01f)); repaint(); }
        else juce::Component::mouseWheelMove(e, w);
    }

    void tick() {
        auto& l = live();
        bool changed = false;
        const float pk[2] = {l.peakL.load(std::memory_order_relaxed), l.peakR.load(std::memory_order_relaxed)};
        l.resetPeaks();
        for (int c = 0; c < 2; ++c) {
            const float next = juce::jmax(pk[c], level_[c] * 0.82f);
            const float v = next < 1.0e-5f ? 0.0f : next;
            if (v != level_[c]) changed = true;
            level_[c] = v;
        }
        const float vol = l.volume.get();
        if (vol != shownVol_) { shownVol_ = vol; changed = true; }
        if (changed) repaint(faderRect().expanded(0, 6));
    }

private:
    TrackLive& live() { return *m_.s_.project.mixer[(size_t)index_].live; }
    juce::String trackName() const {
        const auto& t = m_.s_.project.mixer[(size_t)index_];
        if (!t.name.empty()) return t.name;
        return index_ == kMasterTrack ? juce::String("Master") : (index_ == kCurrentTrack ? juce::String("Current") : juce::String(index_));
    }
    void solo() {
        auto& mixer = m_.s_.project.mixer;
        bool othersMuted = true;
        for (int i = 1; i < kNumMixerTracks - 1; ++i) if (i != index_ && !mixer[(size_t)i].live->mute.getBool()) othersMuted = false;
        const bool unsolo = othersMuted && !mixer[(size_t)index_].live->mute.getBool();
        for (int i = 1; i < kNumMixerTracks - 1; ++i) mixer[(size_t)i].live->mute.set(unsolo ? 0.0f : (i == index_ ? 0.0f : 1.0f));
    }
    MixerView& m_;
    int index_;
    ParamKnob pan_, sep_, send_;
    ParamToggle mute_;
    int faderTop_ = 100, faderBottom_ = 200;
    float level_[2] = {0, 0}, shownVol_ = -1.0f, dragStart_ = 0.0f;
    bool dragging_ = false;
};

class MixerView::Holder : public juce::Component {
public:
    void paint(juce::Graphics& g) override { g.fillAll(theme::panelLo); }
};

// ------------------------------------------------------------------ the selected track's panel

class MixerView::Panel : public juce::Component, public juce::TooltipClient {
public:
    explicit Panel(MixerView& m) : m_(m) {
        for (int i = 0; i < kNumSlots; ++i) {
            bypass_[i].inverted = true;
            bypass_[i].setTooltip("Effect on / bypassed");
            mix_[i].colour = theme::accent2;
            addAndMakeVisible(bypass_[i]);
            addAndMakeVisible(mix_[i]);
            mix_[i].onGestureEnd = [this] { m_.s_.commit("Slot mix"); };
            bypass_[i].onChange = [this] { m_.s_.commit("Slot bypass"); };
        }
        eqView_.setViewedComponent(&eq_, false);
        eqView_.setScrollBarsShown(true, false);
        eqView_.setScrollBarThickness(10);
        addAndMakeVisible(eqView_);
        eq_.onGestureEnd = [this](pulp::Param*) { m_.s_.commit("Track EQ"); };
        inBtn_.setTooltip("Recording input for this track. Choosing one arms the track: with Record lit, pressing play records a take from it. Input 1 is the first input of the audio device.");
        inBtn_.onClick = [this] { inputMenu(); };
        monBtn_.setTooltip("Input monitoring: hear the input through this track's effects. Leave it off if your interface lets you hear yourself directly.");
        monBtn_.onClick = [this] {
            auto& tr = m_.s_.project.mixer[(size_t)m_.s_.project.selectedMixerTrack];
            tr.monitor = !tr.monitor;
            m_.s_.commit("Input monitoring");
            if (tr.monitor && m_.s_.onMessage)
                m_.s_.onMessage("Monitoring is on: the input now plays through PULP.\n\nWith speakers and a live microphone this can feed back loudly, so use headphones. "
                                "If your interface already lets you hear yourself (direct monitoring), switch this off again or you will hear yourself twice.");
        };
        outBtn_.setTooltip("Hardware output: play this track on another pair of the sound card's outputs as well (a headphone mix, an outboard effect). "
                           "Tick the outputs in the audio settings (F10) first. To hear it only there, switch off its send to the master.");
        outBtn_.onClick = [this] {
            const int track = m_.s_.project.selectedMixerTrack;
            int pairs = 1;
            if (auto* d = m_.s_.devices.getCurrentAudioDevice()) pairs = juce::jlimit(1, 4, d->getActiveOutputChannels().countNumberOfSetBits() / 2);
            juce::PopupMenu m;
            m.addItem(1, "None (mixer routing only)", true, m_.s_.project.mixer[(size_t)track].output == 0);
            for (int p = 1; p <= 4; ++p)
                m.addItem(p + 1, "Outputs " + juce::String(p * 2 - 1) + "+" + juce::String(p * 2) + (p > pairs ? "  (not active on this device)" : ""), p <= pairs, m_.s_.project.mixer[(size_t)track].output == p);
            m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&outBtn_), [this, track](int r) {
                if (r <= 0) return;
                m_.s_.project.mixer[(size_t)track].output = r - 1;
                m_.s_.commit("Track output");
            });
        };
        addAndMakeVisible(inBtn_);
        addAndMakeVisible(monBtn_);
        addAndMakeVisible(outBtn_);
        refresh();
    }
    void inputMenu() {
        const int track = m_.s_.project.selectedMixerTrack;
        juce::PopupMenu m;
        const char* names[4] = {"None", "Input 1 (mono)", "Input 2 (mono)", "Inputs 1+2 (stereo)"};
        for (int i = 0; i < 4; ++i) m.addItem(i + 1, names[i], true, m_.s_.project.mixer[(size_t)track].input == i);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&inBtn_), [this, track](int r) {
            if (r <= 0) return;
            m_.s_.project.mixer[(size_t)track].input = r - 1;
            m_.s_.commit("Track input");
        });
    }
    void tick() {
        if (!isShowing()) return;
        auto& e = m_.s_.engine;
        const float a = e.inputPeak(0), b = e.inputPeak(1);
        e.resetInputPeak();
        const float n0 = juce::jmax(a, inLevel_[0] * 0.85f), n1 = juce::jmax(b, inLevel_[1] * 0.85f);
        if (std::fabs(n0 - inLevel_[0]) > 1.0e-4f || std::fabs(n1 - inLevel_[1]) > 1.0e-4f) repaint(0, getHeight() - 46, getWidth(), 26);
        inLevel_[0] = n0 < 1.0e-5f ? 0.0f : n0;
        inLevel_[1] = n1 < 1.0e-5f ? 0.0f : n1;
    }
    juce::Rectangle<int> slotRect(int i) const { return {26, 26 + i * 22, getWidth() - 26 - 34, 20}; }

    void refresh() {
        auto& tr = m_.s_.project.mixer[(size_t)m_.s_.project.selectedMixerTrack];
        auto& l = *tr.live;
        for (int i = 0; i < kNumSlots; ++i) {
            bypass_[i].setParam(&l.slotBypass[i]);
            mix_[i].setParam(&l.slotMix[i]);
            bypass_[i].setVisible((bool)tr.slots[i]);
            mix_[i].setVisible((bool)tr.slots[i]);
        }
        eq_.setSections({{"Track EQ: low shelf, mid, high shelf", {&l.eqFreq[0], &l.eqGain[0], &l.eqQ[0], &l.eqFreq[1], &l.eqGain[1], &l.eqQ[1], &l.eqFreq[2], &l.eqGain[2], &l.eqQ[2]}},
                         {"Switches", {&l.phase, &l.swap}}});
        const int sel = m_.s_.project.selectedMixerTrack;
        const bool insert = sel != kMasterTrack && sel != kCurrentTrack;
        static const char* inNames[4] = {"IN: none", "IN: 1 mono", "IN: 2 mono", "IN: 1+2"};
        inBtn_.text = inNames[juce::jlimit(0, 3, tr.input)];
        inBtn_.setLit(tr.input != 0);
        inBtn_.litColour = theme::bad;
        monBtn_.text = tr.monitor ? "MONITOR on" : "MONITOR off";
        monBtn_.setLit(tr.monitor);
        inBtn_.setVisible(insert);
        monBtn_.setVisible(insert);
        outBtn_.setVisible(insert);
        outBtn_.text = tr.output == 0 ? juce::String("OUT: mixer routing only") : "OUT: also outputs " + juce::String(tr.output * 2 - 1) + "+" + juce::String(tr.output * 2);
        outBtn_.setLit(tr.output != 0);
        inBtn_.repaint();
        monBtn_.repaint();
        outBtn_.repaint();
        resized();
        repaint();
    }
    void resized() override {
        for (int i = 0; i < kNumSlots; ++i) {
            bypass_[i].setBounds(5, 27 + i * 22, 18, 18);
            mix_[i].setBounds(getWidth() - 30, 25 + i * 22, 22, 22);
        }
        const int y = 26 + kNumSlots * 22 + 6;
        eqView_.setBounds(1, y, getWidth() - 1, juce::jmax(60, getHeight() - y - 74));
        outBtn_.setBounds(6, getHeight() - 70, 186, 22);
        inBtn_.setBounds(6, getHeight() - 44, 86, 22);
        monBtn_.setBounds(96, getHeight() - 44, 96, 22);
        const int w = juce::jmax(120, eqView_.getMaximumVisibleWidth());
        eq_.setSize(w, eq_.heightFor(w));
    }
    juce::String getTooltip() override {
        const auto p = getMouseXYRelative();
        if (inBtn_.isVisible() && p.y >= getHeight() - 46 && p.x >= getWidth() - 64)
            return "Input level now: input 1 at " + juce::String(pulp::gainToDb(inLevel_[0]), 1) + " dB, input 2 at " + juce::String(pulp::gainToDb(inLevel_[1]), 1)
                 + " dB. If it sits near -95 dB while you speak, the microphone is muted, unplugged or its gain is all the way down on the interface.";
        for (int i = 0; i < kNumSlots; ++i)
            if (slotRect(i).contains(p)) {
                auto& sl = m_.s_.project.mixer[(size_t)m_.s_.project.selectedMixerTrack].slots[i];
                return sl ? juce::String(sl->displayName()) + " in slot " + juce::String(i + 1) + ": click to open it, right-click to replace, move or remove it"
                          : "Empty effect slot " + juce::String(i + 1) + ": click to choose an effect";
            }
        return {};
    }
    void paint(juce::Graphics& g) override {
        auto& proj = m_.s_.project;
        const int sel = proj.selectedMixerTrack;
        auto& tr = proj.mixer[(size_t)sel];
        g.fillAll(theme::panel);
        g.setColour(theme::line);
        g.drawVerticalLine(0, 0.0f, (float)getHeight());
        g.setColour(theme::accent);
        g.setFont(theme::font(13.0f, true));
        g.drawText(tr.name.empty() ? juce::String(Project::mixerTrackName(sel)) : juce::String(tr.name) + "  (" + juce::String(Project::mixerTrackName(sel)) + ")",
                   8, 2, getWidth() - 16, 22, juce::Justification::centredLeft, true);
        g.setFont(theme::font(12.0f));
        for (int i = 0; i < kNumSlots; ++i) {
            const auto r = slotRect(i);
            theme::insetBox(g, r.toFloat());
            g.setColour(tr.slots[i] ? theme::text : theme::textDim.withAlpha(0.6f));
            g.drawText(tr.slots[i] ? juce::String(tr.slots[i]->displayName()) : "Slot " + juce::String(i + 1), r.reduced(6, 0), juce::Justification::centredLeft, true);
        }
        // delay compensation read-out
        int own = 0;
        for (auto& sl : tr.slots) if (sl) own += sl->latencySamples();
        g.setColour(theme::textDim);
        g.setFont(theme::font(10.5f));
        g.drawText("Delay compensation: this track " + juce::String(own) + ", whole mix " + juce::String(m_.s_.engine.latencySamples()) + " samples",
                   6, getHeight() - 18, getWidth() - 10, 16, juce::Justification::centredLeft, true);
        // hardware input level, both channels
        if (inBtn_.isVisible()) {
            const juce::Rectangle<float> box((float)getWidth() - 62.0f, (float)getHeight() - 43.0f, 56.0f, 20.0f);
            theme::insetBox(g, box);
            for (int c = 0; c < 2; ++c) {
                const float db = pulp::gainToDb(inLevel_[c]);
                const float frac = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
                g.setColour(inLevel_[c] >= 1.0f ? theme::bad : (db > -6.0f ? theme::warn : theme::good));
                g.fillRect(box.getX() + 2.0f, box.getY() + 2.0f + c * 8.5f, (box.getWidth() - 4.0f) * frac, 7.5f);
            }
        }
    }
    void mouseDown(const juce::MouseEvent& e) override {
        for (int i = 0; i < kNumSlots; ++i)
            if (slotRect(i).contains(e.getPosition())) {
                auto& tr = m_.s_.project.mixer[(size_t)m_.s_.project.selectedMixerTrack];
                if (tr.slots[i] && !e.mods.isPopupMenu()) {
                    if (m_.onOpenEffect) m_.onOpenEffect(tr.slots[i], juce::String(tr.slots[i]->displayName()) + "  -  " + juce::String(Project::mixerTrackName(m_.s_.project.selectedMixerTrack)) + " slot " + juce::String(i + 1));
                } else m_.slotMenu(i, this);
                return;
            }
    }
private:
    MixerView& m_;
    ParamToggle bypass_[kNumSlots];
    ParamKnob mix_[kNumSlots];
    ParamPanel eq_;
    juce::Viewport eqView_;
    IconButton inBtn_{IconButton::Text}, monBtn_{IconButton::Text}, outBtn_{IconButton::Text};
    float inLevel_[2] = {0, 0};
};

// ------------------------------------------------------------------ MixerView

MixerView::MixerView(Session& s) : s_(s) {
    setOpaque(true);
    setWantsKeyboardFocus(true);
    holder_ = std::make_unique<Holder>();
    for (int i = 0; i < kNumMixerTracks; ++i) holder_->addAndMakeVisible(strips_.add(new Strip(*this, i)));
    view_.setViewedComponent(holder_.get(), false);
    view_.setScrollBarsShown(false, true);
    view_.setScrollBarThickness(11);
    addAndMakeVisible(view_);
    panel_ = std::make_unique<Panel>(*this);
    addAndMakeVisible(*panel_);
    layoutBtn_.text = "LAYOUT";
    layoutBtn_.setTooltip("Mixer layout: compact, medium, wide or extra large strips");
    layoutBtn_.onClick = [this] { layoutMenu(); };
    addAndMakeVisible(layoutBtn_);
    s_.addListener(this);
}

MixerView::~MixerView() { s_.removeListener(this); }

int MixerView::stripWidth() const {
    static const int w[4] = {40, 56, 74, 98};
    return w[juce::jlimit(0, 3, layout_)];
}

std::vector<int> MixerView::order() const {
    std::vector<int> o = {kMasterTrack, kCurrentTrack};
    for (int dock : {-1, 0, 1})
        for (int i = 1; i <= pulp::kNumInserts; ++i) if (s_.project.mixer[(size_t)i].dock == dock) o.push_back(i);
    return o;
}

void MixerView::layoutStrips() {
    const int w = stripWidth(), h = juce::jmax(160, view_.getMaximumVisibleHeight());
    int x = 0;
    for (int t : order()) {
        // a gap marks the docks: master and current, left dock, the inserts, right dock
        strips_[t]->setBounds(x, 0, w, h);
        x += w;
        if (t == kCurrentTrack) x += 6;
    }
    holder_->setSize(x, h);
}

void MixerView::resized() {
    auto r = getLocalBounds();
    auto top = r.removeFromTop(kTopH);
    layoutBtn_.setBounds(top.removeFromLeft(70).reduced(3, 3));
    panel_->setBounds(r.removeFromRight(kPanelW));
    view_.setBounds(r);
    layoutStrips();
}

void MixerView::projectChanged() {
    // strips keep their identity; only docks and layout change their positions
    juce::String sig;
    for (int i = 1; i <= pulp::kNumInserts; ++i) sig << s_.project.mixer[(size_t)i].dock;
    if (sig != signature_) { signature_ = sig; layoutStrips(); }
    for (auto* st : strips_) st->refresh();
    panel_->refresh();
    repaint();
}

void MixerView::uiTick() {
    if (!UiTicker::onSlowTick()) return;
    if (!isShowing()) return;
    const auto vis = view_.getViewArea();
    for (auto* st : strips_)
        if (st->getRight() >= vis.getX() && st->getX() <= vis.getRight()) st->tick();
    panel_->tick();
}

void MixerView::paint(juce::Graphics& g) {
    g.fillAll(theme::panelLo);
    g.setColour(theme::panel);
    g.fillRect(0, 0, getWidth(), kTopH);
    g.setColour(theme::textDim);
    g.setFont(theme::font(11.5f));
    g.drawText("Select a track, then click another track's arrow to send to it. Right-click an arrow for sidechain.", 80, 0, getWidth() - kPanelW - 90, kTopH, juce::Justification::centredLeft, true);
}

juce::String MixerView::getTooltip() { return {}; }

void MixerView::mouseDown(const juce::MouseEvent&) { grabKeyboardFocus(); }

void MixerView::selectTrack(int index) {
    index = juce::jlimit(0, kNumMixerTracks - 1, index);
    if (s_.project.selectedMixerTrack == index) return;
    s_.project.selectedMixerTrack = index;
    s_.publish();       // the "current" track now listens to this one
    s_.notify();
}

bool MixerView::moveTrack(int track, int by) {
    const int to = track + by;
    if (track < 1 || track > pulp::kNumInserts || to < 1 || to > pulp::kNumInserts || (by != 1 && by != -1)) return false;
    const bool wasSelected = s_.project.selectedMixerTrack == track;
    if (!s_.project.swapMixerTracks(track, to)) return false;
    if (!wasSelected) s_.project.selectedMixerTrack = to;       // what was moved is what stays selected
    s_.commit("Move mixer track");
    return true;
}

bool MixerView::setEffect(int track, int slot, const std::string& typeId) {
    std::shared_ptr<pulp::Plugin> plug;
    if (!typeId.empty()) {
        plug = pulp::PluginRegistry::global().create(typeId);
        if (!plug) return false;
    }
    if (!s_.project.setSlot(track, slot, plug)) return false;
    s_.project.mixer[(size_t)track].live->slotMix[slot].set(1.0f);
    s_.project.mixer[(size_t)track].live->slotBypass[slot].set(0.0f);
    s_.commit(plug ? "Add effect" : "Remove effect");
    return true;
}

void MixerView::toggleRoute(int from, int to) {
    auto& proj = s_.project;
    if (proj.mixer[(size_t)from].routeTo(to)) proj.setRoute(from, to, false);
    else if (!proj.setRoute(from, to, true)) { if (s_.onMessage) s_.onMessage("That route would make a feedback loop, so it was not made."); return; }
    s_.commit("Mixer routing");
}

void MixerView::layoutMenu() {
    juce::PopupMenu m;
    const char* names[4] = {"Compact", "Medium", "Wide", "Extra large"};
    for (int i = 0; i < 4; ++i) m.addItem(i + 1, names[i], true, i == layout_);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&layoutBtn_), [this](int r) {
        if (r > 0) { layout_ = r - 1; layoutStrips(); for (auto* st : strips_) st->resized(); repaint(); }
    });
}

void MixerView::trackMenu(int track, juce::Component* target) {
    auto& tr = s_.project.mixer[(size_t)track];
    juce::PopupMenu m, colours, dock;
    m.addSectionHeader(tr.name.empty() ? juce::String(Project::mixerTrackName(track)) : juce::String(tr.name));
    m.addItem(1, "Rename...");
    for (int i = 0; i < 16; ++i) { juce::PopupMenu::Item it; it.itemID = 100 + i; it.text = "Colour " + juce::String(i + 1); it.colour = theme::palette(i).brighter(0.3f); colours.addItem(it); }
    m.addSubMenu("Colour", colours);
    const bool insert = track != kMasterTrack && track != kCurrentTrack;
    dock.addItem(10, "Left", insert, tr.dock == -1);
    dock.addItem(11, "Middle", insert, tr.dock == 0);
    dock.addItem(12, "Right", insert, tr.dock == 1);
    m.addSubMenu("Dock to", dock, insert);
    m.addItem(2, "Route to the master only", insert);
    m.addItem(4, "Move left  (Alt+Left)", insert && track > 1);
    m.addItem(5, "Move right  (Alt+Right)", insert && track < pulp::kNumInserts);
    m.addItem(3, "Reset this track (fader, pan, EQ, effects, routing)");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target), [this, track, insert](int r) {
        if (r <= 0) return;
        auto& t = s_.project.mixer[(size_t)track];
        if (r == 1) askForText("Rename mixer track", "Track name", t.name, this, [this, track](juce::String n) { s_.project.mixer[(size_t)track].name = n.trim().toStdString(); s_.commit("Rename mixer track"); });
        else if (r >= 10 && r <= 12) { t.dock = r - 11; s_.commit("Dock mixer track"); }
        else if (r == 2) { t.routes.clear(); s_.project.setRoute(track, kMasterTrack, true); s_.commit("Mixer routing"); }
        else if (r == 4 || r == 5) moveTrack(track, r == 4 ? -1 : 1);
        else if (r == 3) {
            for (auto* p : t.live->params()) p->set(p->info.def);
            for (int sl = 0; sl < kNumSlots; ++sl) s_.project.setSlot(track, sl, nullptr);
            t.name.clear();
            t.color = pulp::MixerTrack().color;
            t.dock = 0;
            t.routes.clear();
            if (insert) s_.project.setRoute(track, kMasterTrack, true);
            s_.commit("Reset mixer track");
        }
        else if (r >= 100 && r < 116) { t.color = theme::palette(r - 100).darker(0.3f).getARGB(); s_.commit("Mixer track colour"); }
    });
}

void MixerView::slotMenu(int slot, juce::Component* target) {
    const int track = s_.project.selectedMixerTrack;
    auto& tr = s_.project.mixer[(size_t)track];
    juce::PopupMenu m;
    m.addSectionHeader(tr.slots[slot] ? juce::String(tr.slots[slot]->displayName()) : "Slot " + juce::String(slot + 1));
    std::vector<std::string> ids;
    juce::StringArray cats;
    for (auto& t : pulp::PluginRegistry::global().types()) if (!t.instrument) cats.addIfNotAlreadyThere(t.category);
    for (auto& cat : cats) {
        juce::PopupMenu sub;
        for (auto& t : pulp::PluginRegistry::global().types())
            if (!t.instrument && juce::String(t.category) == cat) { sub.addItem(100 + (int)ids.size(), t.name); ids.push_back(t.id); }
        m.addSubMenu(cat, sub);
    }
    {
        // effects from other makers
        juce::PopupMenu vst;
        for (auto& e : PluginHost::get().plugins())
            if (!e.instrument) { vst.addItem(100 + (int)ids.size(), e.name + (e.maker.isEmpty() ? juce::String() : "   (" + e.maker + ")")); ids.push_back(e.typeId); }
        vst.addSeparator();
        vst.addItem(5, "Find VST3 plugins...");
        m.addSeparator();
        m.addSubMenu("VST3 plugins", vst);
    }
    if (tr.slots[slot]) {
        m.addSeparator();
        m.addItem(1, "Open");
        m.addItem(2, "Move up", slot > 0);
        m.addItem(3, "Move down", slot + 1 < kNumSlots);
        m.addItem(4, "Remove");
    }
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target).withTargetScreenArea(target->localAreaToGlobal(panel_->slotRect(slot))), [this, track, slot, ids](int r) {
        if (r <= 0) return;
        auto& t = s_.project.mixer[(size_t)track];
        if (r >= 100 && r - 100 < (int)ids.size()) {
            if (!setEffect(track, slot, ids[(size_t)(r - 100)])) { if (s_.onMessage) s_.onMessage("That effect could not be loaded."); }
            else if (onOpenEffect)
                onOpenEffect(t.slots[slot], juce::String(t.slots[slot]->displayName()) + "  -  " + juce::String(Project::mixerTrackName(track)) + " slot " + juce::String(slot + 1));
        }
        else if (r == 5) { if (s_.onShowSettings) s_.onShowSettings(2); }
        else if (r == 1) { if (onOpenEffect && t.slots[slot]) onOpenEffect(t.slots[slot], t.slots[slot]->displayName()); }
        else if (r == 2 || r == 3) {
            // the effect's automation, events and links go with it
            s_.project.swapSlots(track, slot, slot + (r == 2 ? -1 : 1));
            s_.commit("Move effect");
        }
        else if (r == 4) setEffect(track, slot, {});
    });
}

bool MixerView::keyPressed(const juce::KeyPress& k) {
    const int sel = s_.project.selectedMixerTrack;
    if (k.getKeyCode() == juce::KeyPress::leftKey && !k.getModifiers().isAnyModifierKeyDown()) { selectTrack(sel == kCurrentTrack ? 0 : juce::jmax(0, sel - 1)); return true; }
    if (k.getKeyCode() == juce::KeyPress::rightKey && !k.getModifiers().isAnyModifierKeyDown()) { selectTrack(juce::jmin(pulp::kNumInserts, sel + 1)); return true; }
    const auto mods = k.getModifiers();
    const int code = k.getKeyCode();
    // Alt+Left / Alt+Right move the selected track one place
    if ((code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) && mods.isAltDown() && !mods.isCtrlDown() && !mods.isShiftDown()) {
        moveTrack(sel, code == juce::KeyPress::leftKey ? -1 : 1);
        return true;
    }
    // S solos the selected track (again to bring the others back)
    if (code == 'S' && !mods.isAnyModifierKeyDown() && !s_.typingKeyboard && sel >= 1 && sel <= pulp::kNumInserts) {
        auto& mixer = s_.project.mixer;
        bool othersMuted = true;
        for (int i = 1; i <= pulp::kNumInserts; ++i) if (i != sel && !mixer[(size_t)i].live->mute.getBool()) othersMuted = false;
        const bool unsolo = othersMuted && !mixer[(size_t)sel].live->mute.getBool();
        for (int i = 1; i <= pulp::kNumInserts; ++i) mixer[(size_t)i].live->mute.set(unsolo ? 0.0f : (i == sel ? 0.0f : 1.0f));
        s_.commit("Solo mixer track");
        return true;
    }
    // Ctrl+L sends the channels selected in the rack to the selected track; with Shift, to it and the tracks after it
    if (code == 'L' && mods.isCtrlDown() && !mods.isAltDown() && sel >= 1 && sel <= pulp::kNumInserts) {
        int track = sel;
        for (auto& c : s_.project.channels)
            if (s_.selectedChannels.count(c.id)) { c.mixerTrack = juce::jmin(track, pulp::kNumInserts); if (mods.isShiftDown()) ++track; }
        s_.commit("Channels to mixer track");
        return true;
    }
    if (code == juce::KeyPress::F2Key && !mods.isAnyModifierKeyDown() && sel != kCurrentTrack) {
        askForText("Rename mixer track", "Track name", s_.project.mixer[(size_t)sel].name, this, [this, sel](juce::String name) { s_.project.mixer[(size_t)sel].name = name.trim().toStdString(); s_.commit("Rename mixer track"); });
        return true;
    }
    return false;
}

juce::String MixerView::describe() const {
    auto& p = s_.project;
    juce::String out = "mixer selected=" + juce::String(p.selectedMixerTrack) + " latency=" + juce::String(s_.engine.latencySamples()) + " :";
    for (int i = 0; i < kNumMixerTracks; ++i) {
        const auto& t = p.mixer[(size_t)i];
        juce::String fx;
        for (int s = 0; s < kNumSlots; ++s) if (t.slots[s]) fx << (fx.isEmpty() ? "" : "+") << t.slots[s]->typeId();
        juce::String routes;
        for (auto& r : t.routes) if (!(r.to == 0 && !r.sidechain)) routes << ">" << r.to << (r.sidechain ? "sc" : "");
        const bool toMaster = t.routeTo(0) != nullptr;
        if (fx.isEmpty() && routes.isEmpty() && t.name.empty() && (toMaster || i == 0 || i == kCurrentTrack) && t.live->volume.get() == 0.8f) continue;
        out << " [" << i << (t.name.empty() ? juce::String() : " " + juce::String(t.name)) << (fx.isEmpty() ? juce::String() : " fx:" + fx) << routes
            << (toMaster || i == 0 || i == kCurrentTrack ? "" : " !master") << " vol=" << juce::String(t.live->volume.get(), 2) << "]";
    }
    return out;
}
