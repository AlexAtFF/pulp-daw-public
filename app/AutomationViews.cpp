#include "AutomationViews.h"
#include "pulp/Controllers.h"

// ------------------------------------------------------------------ EventEditor

EventEditor::EventEditor(Session& s) : s_(s) {
    setOpaque(true);
    target_.setTooltip("The control whose events are shown. Click to choose among the controls that have events in this pattern. "
                       "To edit another control, right-click it anywhere and choose \"Edit events\".");
    target_.onClick = [this] { targetMenu(); };
    clear_.text = "CLEAR";
    clear_.setTooltip("Remove this control's events from the pattern");
    clear_.onClick = [this] {
        if (auto* ev = events(false)) { s_.project.pattern().events.erase(s_.eventTarget); (void)ev; s_.commit("Clear events"); }
    };
    addAndMakeVisible(target_);
    addAndMakeVisible(clear_);
    s_.addListener(this);
    projectChanged();
}
EventEditor::~EventEditor() { s_.removeListener(this); }

void EventEditor::projectChanged() {
    const bool known = !s_.eventTarget.empty() && s_.project.findParam(s_.eventTarget) != nullptr;
    target_.text = known ? juce::String(s_.project.paramLabel(s_.eventTarget)) : juce::String("(no control chosen)");
    target_.repaint();
    if (onTitle) onTitle("Events  -  " + juce::String(s_.project.pattern().name));
    repaint();
}

void EventEditor::resized() {
    target_.setBounds(4, 3, juce::jmax(120, getWidth() - 80), 22);
    clear_.setBounds(getWidth() - 70, 3, 64, 22);
}

juce::Rectangle<int> EventEditor::grid() const { return {38, 30, juce::jmax(20, getWidth() - 44), juce::jmax(20, getHeight() - 50)}; }
int EventEditor::length() const { return juce::jmax(pulp::kTicksPerStep, s_.project.patternLengthTicks(s_.project.pattern())); }

int EventEditor::tickAt(int x, bool snapped) const {
    const auto g = grid();
    int t = (int)((double)(x - g.getX()) / (double)g.getWidth() * (double)length());
    if (snapped) { const int sn = juce::jmax(1, s_.snapTicks((double)g.getWidth() / (double)length())); t = (t / sn) * sn; }
    return juce::jlimit(0, length(), t);
}
float EventEditor::valueAt(int y) const {
    const auto g = grid();
    return juce::jlimit(0.0f, 1.0f, 1.0f - (float)(y - g.getY()) / (float)g.getHeight());
}

std::vector<pulp::AutoEvent>* EventEditor::events(bool create) {
    if (s_.eventTarget.empty() || s_.project.findParam(s_.eventTarget) == nullptr) return nullptr;
    auto& map = s_.project.pattern().events;
    auto it = map.find(s_.eventTarget);
    if (it == map.end()) { if (!create) return nullptr; return &map[s_.eventTarget]; }
    return &it->second;
}

juce::String EventEditor::getTooltip() {
    const auto p = getMouseXYRelative();
    if (!grid().contains(p)) return {};
    const auto* prm = s_.project.findParam(s_.eventTarget);
    if (!prm) return "Right-click any knob, slider or switch and choose \"Edit events\" to draw its automation inside this pattern";
    return juce::String(prm->info.name) + ": " + juce::String(prm->displayFor(prm->fromNorm(valueAt(p.y)))) + " at step " + juce::String(tickAt(p.x, false) / pulp::kTicksPerStep + 1)
         + ".  Drag to draw steps, Shift-drag for a straight glide, right-drag to erase.";
}

void EventEditor::targetMenu() {
    juce::PopupMenu m;
    std::vector<std::string> addresses;
    for (auto& kv : s_.project.pattern().events) {
        if (kv.second.empty() || s_.project.findParam(kv.first) == nullptr) continue;
        m.addItem((int)addresses.size() + 1, s_.project.paramLabel(kv.first), true, kv.first == s_.eventTarget);
        addresses.push_back(kv.first);
    }
    if (addresses.empty()) m.addItem(999, "No control has events in this pattern yet", false);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target_), [this, addresses](int r) {
        if (r > 0 && r <= (int)addresses.size()) { s_.eventTarget = addresses[(size_t)(r - 1)]; s_.notify(); }
    });
}

void EventEditor::mouseDown(const juce::MouseEvent& e) {
    drawing_ = erasing_ = line_ = changed_ = false;
    if (!grid().contains(e.getPosition()) || events(true) == nullptr) return;
    downTick_ = lastTick_ = tickAt(e.x, !e.mods.isAltDown());
    downValue_ = valueAt(e.y);
    if (e.mods.isPopupMenu()) erasing_ = true;
    else if (e.mods.isShiftDown()) { line_ = true; lineEnd_ = e.getPosition(); }
    else drawing_ = true;
    mouseDrag(e);
}

void EventEditor::mouseDrag(const juce::MouseEvent& e) {
    auto* ev = events(true);
    if (!ev) return;
    const int tick = tickAt(e.x, !e.mods.isAltDown());
    const int a = juce::jmin(lastTick_, tick), b = juce::jmax(lastTick_, tick);
    if (line_) { lineEnd_ = e.getPosition(); repaint(); return; }
    if (erasing_ || drawing_) {
        const size_t n = ev->size();
        // drawing replaces what lies between the last drawn step and this one, but keeps the step it came from
        ev->erase(std::remove_if(ev->begin(), ev->end(), [&](const pulp::AutoEvent& x) {
            return erasing_ ? (x.tick >= a && x.tick <= b) : (x.tick == tick || (x.tick > a && x.tick < b));
        }), ev->end());
        if (drawing_) ev->push_back({tick, valueAt(e.y), false});
        pulp::sortEvents(*ev);
        changed_ = changed_ || drawing_ || ev->size() != n;
        lastTick_ = tick;
        s_.publish();
        repaint();
    }
}

void EventEditor::mouseUp(const juce::MouseEvent& e) {
    if (line_) {
        if (auto* ev = events(true)) {
            int t1 = tickAt(e.x, !e.mods.isAltDown());
            float v0 = downValue_, v1 = valueAt(e.y);
            int t0 = downTick_;
            if (t1 < t0) { std::swap(t0, t1); std::swap(v0, v1); }
            if (t1 > t0) {
                ev->erase(std::remove_if(ev->begin(), ev->end(), [&](const pulp::AutoEvent& x) { return x.tick >= t0 && x.tick <= t1; }), ev->end());
                ev->push_back({t0, v0, true});
                ev->push_back({t1, v1, false});
                pulp::sortEvents(*ev);
                changed_ = true;
            }
        }
    }
    if (changed_) {
        auto& map = s_.project.pattern().events;
        auto it = map.find(s_.eventTarget);
        if (it != map.end() && it->second.empty()) map.erase(it);
        s_.commit(erasing_ ? "Erase events" : "Draw events");
    }
    drawing_ = erasing_ = line_ = changed_ = false;
    repaint();
}

void EventEditor::uiTick() {
    if (!UiTicker::onSlowTick()) return;
    if (!isShowing()) return;
    float x = -1.0f;
    if (!s_.project.songMode && s_.engine.isPlaying()) x = (float)grid().getX() + (float)(s_.engine.positionTicks() / (double)length()) * (float)grid().getWidth();
    if (x != playX_) { playX_ = x; repaint(); }
}

void EventEditor::paint(juce::Graphics& g) {
    g.fillAll(theme::panel);
    const auto a = grid();
    theme::insetBox(g, a.toFloat());
    const int len = length();
    // grid: bars, beats and quarter levels
    const int bar = s_.project.ticksPerBar();
    for (int t = 0; t <= len; t += pulp::kTicksPerStep * 4) {
        const float x = (float)a.getX() + (float)t / (float)len * (float)a.getWidth();
        g.setColour(t % bar == 0 ? theme::textDim.withAlpha(0.5f) : theme::textDim.withAlpha(0.18f));
        g.drawVerticalLine((int)x, (float)a.getY(), (float)a.getBottom());
        if (t % bar == 0 && t < len) { g.setFont(theme::font(10.0f)); g.setColour(theme::textDim); g.drawText(juce::String(t / bar + 1), (int)x + 3, a.getBottom() + 2, 30, 12, juce::Justification::centredLeft, false); }
    }
    const auto* prm = s_.project.findParam(s_.eventTarget);
    g.setFont(theme::font(9.5f));
    for (int i = 0; i <= 4; ++i) {
        const float y = (float)a.getY() + (float)a.getHeight() * (float)i / 4.0f;
        g.setColour(theme::textDim.withAlpha(0.2f));
        g.drawHorizontalLine((int)y, (float)a.getX(), (float)a.getRight());
        g.setColour(theme::textDim);
        const float v = 1.0f - (float)i / 4.0f;
        g.drawText(prm ? juce::String(prm->displayFor(prm->fromNorm(v))) : juce::String(juce::roundToInt(v * 100.0f)) + "%", 0, (int)y - 6, 36, 12, juce::Justification::centredRight, false);
    }
    const auto* ev = const_cast<EventEditor*>(this)->events(false);
    if (!prm) {
        g.setColour(theme::textDim);
        g.setFont(theme::font(12.5f));
        g.drawFittedText("Right-click any knob, slider or switch and choose \"Edit events\".\nIts movements inside this pattern are drawn here.", a.reduced(16), juce::Justification::centred, 3);
        return;
    }
    if (ev && !ev->empty()) {
        g.saveState();
        g.reduceClipRegion(a.reduced(1));
        auto xOf = [&](double t) { return (float)a.getX() + (float)(t / (double)len) * (float)a.getWidth(); };
        auto yOf = [&](float v) { return (float)a.getBottom() - v * (float)a.getHeight(); };
        juce::Path line, fill;
        line.startNewSubPath((float)a.getX(), yOf(ev->front().value));
        for (size_t i = 0; i < ev->size(); ++i) {
            const auto& e = (*ev)[i];
            line.lineTo(xOf(e.tick), yOf(i == 0 ? e.value : pulp::evalEvents(*ev, e.tick - 0.001)));
            line.lineTo(xOf(e.tick), yOf(e.value));
        }
        line.lineTo((float)a.getRight(), yOf(ev->back().value));
        fill = line;
        fill.lineTo((float)a.getRight(), (float)a.getBottom());
        fill.lineTo((float)a.getX(), (float)a.getBottom());
        fill.closeSubPath();
        g.setColour(theme::accent.withAlpha(0.18f));
        g.fillPath(fill);
        g.setColour(theme::accent);
        g.strokePath(line, juce::PathStrokeType(1.6f));
        for (auto& e : *ev) g.fillEllipse(xOf(e.tick) - 2.5f, yOf(e.value) - 2.5f, 5.0f, 5.0f);
        g.restoreState();
    }
    if (line_) {
        g.setColour(theme::warn);
        const float x0 = (float)a.getX() + (float)downTick_ / (float)len * (float)a.getWidth();
        g.drawLine(x0, (float)a.getBottom() - downValue_ * (float)a.getHeight(), (float)lineEnd_.x, (float)lineEnd_.y, 1.5f);
    }
    if (playX_ >= 0.0f) { g.setColour(theme::good); g.drawVerticalLine((int)playX_, (float)a.getY(), (float)a.getBottom()); }
}

juce::String EventEditor::describe() const {
    juce::String out = "events target=" + juce::String(s_.eventTarget) + " :";
    auto it = s_.project.pattern().events.find(s_.eventTarget);
    if (it != s_.project.pattern().events.end())
        for (auto& e : it->second) out << " " << e.tick << "=" << juce::String(e.value, 3) << (e.ramp ? "r" : "");
    return out;
}

// ------------------------------------------------------------------ LinkEditor

static pulp::ParamInfo linkInfo(const char* id, const char* name, float mn, float mx, float def, const char* unit, float skew = 1.0f) {
    pulp::ParamInfo i;
    i.id = id; i.name = name; i.min = mn; i.max = mx; i.def = def; i.unit = unit; i.skew = skew;
    return i;
}

LinkEditor::LinkEditor(Session& s)
    : s_(s), minP_(linkInfo("min", "Control position when the controller is at its lowest", 0, 1, 0, "%")),
      maxP_(linkInfo("max", "Control position when the controller is at its highest", 0, 1, 1, "%")),
      smoothP_(linkInfo("smooth", "Smoothing", 0, 2000, 0, "ms", 3.0f)), min_(&minP_), max_(&maxP_), smooth_(&smoothP_) {
    setOpaque(true);
    min_.label = "FROM";
    max_.label = "TO";
    smooth_.label = "SMOOTH";
    for (auto* k : {&min_, &max_, &smooth_}) { addAndMakeVisible(k); k->onGestureEnd = [this] { apply(); }; }
    source_.setTooltip("What moves this control: a hardware knob or fader, or one of the controllers in the mixer");
    source_.onChange = [this] { if (!refreshing_) apply(); };
    addAndMakeVisible(source_);
    formula_.setFont(theme::mono(13.0f));
    formula_.setTextToShowWhenEmpty("mapping formula, for example  1 - x  or  x * x   (x is the controller, 0 to 1)", theme::textDim);
    formula_.onReturnKey = [this] { apply(); };
    formula_.onFocusLost = [this] { apply(); };
    addAndMakeVisible(formula_);
    remove_.text = "REMOVE LINK";
    remove_.setTooltip("Unlink this control");
    remove_.onClick = [this] { s_.cancelMidiLearn(); s_.removeLink(address_); };
    addAndMakeVisible(remove_);
    s_.addListener(this);
}
LinkEditor::~LinkEditor() { s_.removeListener(this); }

void LinkEditor::setTarget(const std::string& address) {
    address_ = address;
    status_.clear();
    refresh();
}

void LinkEditor::refresh() {
    refreshing_ = true;
    sources_ = s_.controllerSources();
    const pulp::ParamLink* link = s_.project.linkFor(address_);
    source_.clear(juce::dontSendNotification);
    source_.addItem("Not linked", 1);
    source_.addItem("MIDI: learn (choose this, then move a knob or fader on your hardware)", 2);
    if (link && link->kind == pulp::ParamLink::Midi)
        source_.addItem("MIDI: " + (link->midiChannel < 0 ? juce::String("any channel") : "channel " + juce::String(link->midiChannel + 1)) + ", controller " + juce::String(link->cc), 3);
    for (size_t i = 0; i < sources_.size(); ++i) source_.addItem(sources_[i].name, 10 + (int)i);
    int sel = 1;
    if (s_.midiLearning() && s_.learnAddress() == address_) sel = 2;
    else if (link && link->kind == pulp::ParamLink::Midi) sel = 3;
    else if (link)
        for (size_t i = 0; i < sources_.size(); ++i)
            if (sources_[i].track == link->track && sources_[i].slot == link->slot && sources_[i].output == link->output) sel = 10 + (int)i;
    source_.setSelectedId(sel, juce::dontSendNotification);
    if (link) {
        minP_.set(link->min); maxP_.set(link->max); smoothP_.set(link->smoothMs);
        if (!formula_.hasKeyboardFocus(false)) formula_.setText(link->formula, false);
    }
    remove_.setEnabled(link != nullptr);
    if (onTitle) onTitle("Link to controller  -  " + juce::String(s_.project.paramLabel(address_)));
    refreshing_ = false;
    repaint();
}

void LinkEditor::apply() {
    if (address_.empty() || s_.project.findParam(address_) == nullptr) return;
    const int sel = source_.getSelectedId();
    pulp::ParamLink link;
    if (auto* old = s_.project.linkFor(address_)) link = *old;
    link.target = address_;
    link.min = minP_.get();
    link.max = maxP_.get();
    link.smoothMs = smoothP_.get();
    const auto text = formula_.getText().trim().toStdString();
    std::string err;
    if (!text.empty() && pulp::Formula::compile(text, {"x"}, &err) == nullptr) { status_ = "Formula: " + juce::String(err); repaint(); return; }
    status_.clear();
    link.formula = text;
    if (sel == 1) { s_.cancelMidiLearn(); s_.removeLink(address_); return; }
    if (sel == 2) {
        // keep what is already set for when the control arrives
        if (s_.project.linkFor(address_)) s_.setLink(link);
        s_.beginMidiLearn(s_.project.findParam(address_));
        status_ = "Waiting: move a knob or fader on your MIDI hardware...";
        repaint();
        return;
    }
    s_.cancelMidiLearn();
    if (sel >= 10 && sel - 10 < (int)sources_.size()) {
        const auto& src = sources_[(size_t)(sel - 10)];
        link.kind = pulp::ParamLink::Controller;
        link.track = src.track; link.slot = src.slot; link.output = src.output;
    } else if (sel != 3 || link.kind != pulp::ParamLink::Midi) return;
    s_.setLink(link);
}

void LinkEditor::uiTick() {
    if (!UiTicker::onSlowTick()) return;
    const bool learning = s_.midiLearning() && s_.learnAddress() == address_;
    if (wasLearning_ && !learning) { status_.clear(); refresh(); }
    wasLearning_ = learning;
}

void LinkEditor::resized() {
    auto r = getLocalBounds().reduced(10);
    r.removeFromTop(24);
    source_.setBounds(r.removeFromTop(26));
    r.removeFromTop(10);
    auto knobs = r.removeFromTop(58);
    min_.setBounds(knobs.removeFromLeft(64));
    max_.setBounds(knobs.removeFromLeft(64));
    smooth_.setBounds(knobs.removeFromLeft(64));
    remove_.setBounds(knobs.removeFromRight(110).withSizeKeepingCentre(110, 24));
    r.removeFromTop(8);
    formula_.setBounds(r.removeFromTop(26));
}

void LinkEditor::paint(juce::Graphics& g) {
    g.fillAll(theme::panel);
    const auto* p = s_.project.findParam(address_);
    g.setColour(theme::accent);
    g.setFont(theme::font(13.5f, true));
    g.drawText(p ? juce::String(s_.project.paramLabel(address_)) + "   " + juce::String(p->display()) : juce::String("Right-click a control and choose \"Link to controller\""),
               10, 8, getWidth() - 20, 20, juce::Justification::centredLeft, true);
    g.setColour(status_.startsWith("Formula") ? theme::bad : theme::textDim);
    g.setFont(theme::font(11.5f));
    g.drawFittedText(status_.isNotEmpty() ? status_
                                          : juce::String("FROM and TO set how far the control travels (FROM above TO turns it upside down). The formula reshapes the controller first."),
                     10, getHeight() - 40, getWidth() - 20, 32, juce::Justification::topLeft, 2);
}

// ------------------------------------------------------------------ MidiSettings

MidiSettings::MidiSettings(Session& s) : s_(s) { s_.addListener(this); }
MidiSettings::~MidiSettings() { s_.removeListener(this); }

void MidiSettings::uiTick() {
    if (!UiTicker::onSlowTick()) return;
    if (isShowing() && s_.lastMidiText != shownText_) repaint();
}

void MidiSettings::paint(juce::Graphics& g) {
    g.fillAll(theme::panel);
    auto r = getLocalBounds().reduced(10);
    g.setColour(theme::accent2);
    g.setFont(theme::font(11.0f, true));
    auto head = r.removeFromTop(18);
    g.drawText("MIDI INPUT DEVICES", head, juce::Justification::centredLeft, false);
    g.drawText("NOTES", head.withLeft(head.getRight() - 150).withWidth(60), juce::Justification::centred, false);
    g.drawText("CONTROLLERS", head.withLeft(head.getRight() - 90).withWidth(90), juce::Justification::centred, false);
    const auto devices = s_.midiDevices();
    auto list = r.removeFromTop(juce::jmax(60, r.getHeight() - 70));
    theme::insetBox(g, list.toFloat());
    g.setFont(theme::font(12.5f));
    if (devices.empty()) {
        g.setColour(theme::textDim);
        g.drawFittedText(s_.usingNullDevice() ? "Silent test mode: MIDI devices are not opened."
                                               : "No MIDI device is connected. Plug in a keyboard or controller and it appears here; there is nothing to switch on.",
                         list.reduced(12), juce::Justification::centred, 3);
    }
    auto rows = list.reduced(4);
    for (size_t i = 0; i < devices.size() && rows.getHeight() >= 22; ++i) {
        auto line = rows.removeFromTop(22);
        g.setColour(devices[i].open ? theme::text : theme::bad);
        g.drawText(devices[i].name + (devices[i].open ? "" : "   (could not be opened)"), line.withTrimmedRight(160).withTrimmedLeft(4), juce::Justification::centredLeft, true);
        auto lamp = [&](juce::Rectangle<int> box, bool on) {
            g.setColour(theme::panelHi);
            g.fillRoundedRectangle(box.toFloat(), 3.0f);
            if (on) { g.setColour(theme::good); g.fillRoundedRectangle(box.toFloat().reduced(2.5f), 2.0f); }
        };
        lamp(juce::Rectangle<int>(line.getRight() - 128, line.getY() + 3, 16, 16), devices[i].notes);
        lamp(juce::Rectangle<int>(line.getRight() - 53, line.getY() + 3, 16, 16), devices[i].controllers);
    }
    r.removeFromTop(8);
    g.setColour(theme::textDim);
    g.setFont(theme::font(12.0f));
    shownText_ = s_.lastMidiText;
    g.drawFittedText("Notes: the device plays the selected channel and is recorded. Controllers: its knobs and faders can be linked to any control "
                     "(right-click the control > Link to controller). Devices are found as they are plugged in.\nLast message: "
                     + (shownText_.isEmpty() ? juce::String("none yet") : shownText_),
                     r, juce::Justification::topLeft, 4);
}

void MidiSettings::mouseDown(const juce::MouseEvent& e) {
    const auto devices = s_.midiDevices();
    auto rows = getLocalBounds().reduced(10).withTrimmedTop(18).reduced(4);
    const int i = (e.y - rows.getY()) / 22;
    if (i < 0 || i >= (int)devices.size()) return;
    const int right = rows.getRight();
    if (e.x >= right - 136 && e.x < right - 104) s_.setMidiDeviceRole(i, !devices[(size_t)i].notes, devices[(size_t)i].controllers);
    else if (e.x >= right - 61 && e.x < right - 29) s_.setMidiDeviceRole(i, devices[(size_t)i].notes, !devices[(size_t)i].controllers);
}

// ------------------------------------------------------------------ SettingsPages

SettingsPages::SettingsPages(Session& s) : audio_(s), midi_(s), plugins_(s) {
    audioTab_.text = "AUDIO";
    midiTab_.text = "MIDI";
    pluginTab_.text = "PLUGINS";
    pluginTab_.setTooltip("VST3 plugins: find them, see which were found and which failed");
    pluginTab_.onClick = [this] { showPage(2); };
    addAndMakeVisible(pluginTab_);
    addChildComponent(plugins_);
    keysTab_.text = "SHORTCUTS";
    keysTab_.setTooltip("Keyboard shortcuts: see them and change them");
    keysTab_.onClick = [this] { showPage(3); };
    addAndMakeVisible(keysTab_);
    addChildComponent(shortcuts_);
    audioTab_.setTooltip("Audio device, sample rate, buffer size, inputs and outputs, latency");
    midiTab_.setTooltip("MIDI keyboards and controllers");
    audioTab_.onClick = [this] { showPage(0); };
    midiTab_.onClick = [this] { showPage(1); };
    addAndMakeVisible(audioTab_);
    addAndMakeVisible(midiTab_);
    addAndMakeVisible(audio_);
    addChildComponent(midi_);
    showPage(0);
}

void SettingsPages::showPage(int page) {
    page_ = page;
    audio_.setVisible(page == 0);
    midi_.setVisible(page == 1);
    plugins_.setVisible(page == 2);
    shortcuts_.setVisible(page == 3);
    keysTab_.setLit(page == 3);
    audioTab_.setLit(page == 0);
    midiTab_.setLit(page == 1);
    pluginTab_.setLit(page == 2);
}

void SettingsPages::resized() {
    auto r = getLocalBounds();
    auto tabs = r.removeFromTop(28).reduced(8, 3);
    audioTab_.setBounds(tabs.removeFromLeft(70));
    tabs.removeFromLeft(4);
    midiTab_.setBounds(tabs.removeFromLeft(70));
    tabs.removeFromLeft(4);
    pluginTab_.setBounds(tabs.removeFromLeft(84));
    tabs.removeFromLeft(4);
    keysTab_.setBounds(tabs.removeFromLeft(96));
    shortcuts_.setBounds(r);
    audio_.setBounds(r);
    midi_.setBounds(r);
    plugins_.setBounds(r);
}

// ------------------------------------------------------------------ ControllerView

ControllerView::ControllerView(Session& s, std::shared_ptr<pulp::Plugin> plugin) : s_(s), plugin_(std::move(plugin)) {
    const std::string type = plugin_->typeId();
    isPad_ = type == "ctl_xy";
    isFormula_ = type == "ctl_formula";
    if (isFormula_) {
        formula_.setFont(theme::mono(13.5f));
        formula_.setText(static_cast<pulp::FormulaCtl*>(plugin_.get())->formula(), false);
        formula_.addListener(this);
        addAndMakeVisible(formula_);
    }
}

int ControllerView::heightFor(const pulp::Plugin& p) {
    const std::string type = p.typeId();
    return type == "ctl_xy" ? 196 : (type == "ctl_formula" ? 104 : 44);
}

juce::Rectangle<int> ControllerView::padArea() const { return {0, 0, juce::jmin(170, getWidth() / 2), 170}; }

void ControllerView::resized() {
    if (isFormula_) formula_.setBounds(0, 0, getWidth(), 26);
}

juce::String ControllerView::getTooltip() {
    if (isPad_ && padArea().contains(getMouseXYRelative())) return "Drag the dot: X and Y are two controller outputs. Link any control to them (right-click it > Link to controller).";
    return "What this controller is putting out right now. To use it, right-click any knob, slider or switch and choose \"Link to controller\".";
}

void ControllerView::applyFormula() {
    auto* f = static_cast<pulp::FormulaCtl*>(plugin_.get());
    const auto text = formula_.getText().trim().toStdString();
    if (text == f->formula()) return;
    std::string err;
    if (f->setFormula(text, &err)) { formulaStatus_.clear(); s_.commit("Change formula"); }
    else formulaStatus_ = "Not understood: " + juce::String(err);
    repaint();
}

void ControllerView::setPad(juce::Point<int> p) {
    const auto a = padArea().reduced(6);
    auto* pad = static_cast<pulp::PadXy*>(plugin_.get());
    pad->pX->set(juce::jlimit(0.0f, 1.0f, (float)(p.x - a.getX()) / (float)a.getWidth()));
    pad->pY->set(juce::jlimit(0.0f, 1.0f, 1.0f - (float)(p.y - a.getY()) / (float)a.getHeight()));
    ParamUi::touch(pad->pX, 1);
    ParamUi::touch(pad->pY, 1);
    repaint();
}

void ControllerView::mouseDown(const juce::MouseEvent& e) {
    if (!isPad_ || !padArea().contains(e.getPosition())) return;
    auto* pad = static_cast<pulp::PadXy*>(plugin_.get());
    dragging_ = true;
    ParamUi::touch(pad->pX, 0);
    setPad(e.getPosition());
}
void ControllerView::mouseDrag(const juce::MouseEvent& e) { if (dragging_) setPad(e.getPosition()); }
void ControllerView::mouseUp(const juce::MouseEvent&) {
    if (!dragging_) return;
    dragging_ = false;
    auto* pad = static_cast<pulp::PadXy*>(plugin_.get());
    ParamUi::touch(pad->pX, 2);
    s_.commit("Move the pad");
}

void ControllerView::paint(juce::Graphics& g) {
    g.fillAll(theme::panel);
    int y = 0;
    if (isPad_) {
        const auto a = padArea();
        theme::insetBox(g, a.toFloat());
        const auto in = a.reduced(6);
        g.setColour(theme::textDim.withAlpha(0.25f));
        g.drawVerticalLine(in.getCentreX(), (float)in.getY(), (float)in.getBottom());
        g.drawHorizontalLine(in.getCentreY(), (float)in.getX(), (float)in.getRight());
        auto* pad = static_cast<pulp::PadXy*>(plugin_.get());
        auto dot = [&](float x, float yy, juce::Colour c, float size) {
            g.setColour(c);
            g.fillEllipse((float)in.getX() + x * (float)in.getWidth() - size * 0.5f, (float)in.getBottom() - yy * (float)in.getHeight() - size * 0.5f, size, size);
        };
        dot(plugin_->controlOutput(0), plugin_->controlOutput(1), theme::accent2.withAlpha(0.6f), 9.0f);       // where the smoothed output is
        dot(pad->pX->get(), pad->pY->get(), theme::accent, 13.0f);                                              // where the hand put it
        y = 0;
        g.setColour(theme::textDim);
        g.setFont(theme::font(11.5f));
        g.drawFittedText("Drag the dot. X and Y are two outputs you can link controls to.", a.getRight() + 10, 4, getWidth() - a.getRight() - 14, 40, juce::Justification::topLeft, 3);
        y = 50;
    } else if (isFormula_) {
        g.setColour(formulaStatus_.isNotEmpty() ? theme::bad : theme::textDim);
        g.setFont(theme::font(11.5f));
        g.drawFittedText(formulaStatus_.isNotEmpty() ? formulaStatus_
                                                     : juce::String("Press Enter to apply. Variables: a b c (the knobs), t (seconds), beat, tempo. Functions: sin cos abs min max clamp if frac sine tri saw square."),
                         0, 28, getWidth(), 30, juce::Justification::topLeft, 2);
        y = 62;
    }
    // output meters
    const int x0 = isPad_ ? padArea().getRight() + 10 : 0;
    for (int i = 0; i < plugin_->numControlOutputs(); ++i) {
        const juce::Rectangle<float> bar((float)x0 + 46.0f, (float)y + (float)i * 20.0f + 4.0f, (float)(getWidth() - x0) - 100.0f, 12.0f);
        g.setColour(theme::text);
        g.setFont(theme::font(11.5f, true));
        g.drawText(plugin_->controlOutputName(i), x0, (int)bar.getY() - 2, 44, 16, juce::Justification::centredLeft, false);
        theme::insetBox(g, bar);
        const float v = plugin_->controlOutput(i);
        g.setColour(theme::accent);
        g.fillRect(bar.reduced(2.0f).withWidth((bar.getWidth() - 4.0f) * v));
        g.setColour(theme::textDim);
        g.drawText(juce::String(juce::roundToInt(v * 100.0f)) + "%", (int)bar.getRight() + 6, (int)bar.getY() - 2, 44, 16, juce::Justification::centredLeft, false);
    }
}
