#include "Toolbar.h"

class Toolbar::Scope : public juce::Component, public juce::SettableTooltipClient, private UiTicker::Client {
public:
    explicit Scope(Session& s) : s_(s) { setTooltip("Output oscilloscope"); }
    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat();
        theme::insetBox(g, r);
        float buf[512];
        const int n = s_.engine.scopeRead(buf, 512);
        juce::Path p;
        const float mid = r.getCentreY(), h = r.getHeight() * 0.48f;
        for (int i = 0; i < n; ++i) {
            const float x = r.getX() + 1.0f + (r.getWidth() - 2.0f) * (float)i / (float)(n - 1);
            const float y = mid - juce::jlimit(-1.0f, 1.0f, buf[i]) * h;
            if (i == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
        }
        g.setColour(theme::accent2);
        g.strokePath(p, juce::PathStrokeType(1.0f));
    }
private:
    void uiTick() override { if (UiTicker::onSlowTick()) repaint(); }
    Session& s_;
};

class Toolbar::Meter : public juce::Component, public juce::SettableTooltipClient, private UiTicker::Client {
public:
    explicit Meter(Session& s) : s_(s) { setTooltip("Output peak meter"); }
    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat();
        theme::insetBox(g, r);
        for (int c = 0; c < 2; ++c) {
            auto bar = r.reduced(2.0f).withWidth((r.getWidth() - 5.0f) * 0.5f).translated(c * ((r.getWidth() - 5.0f) * 0.5f + 1.0f), 0.0f);
            const float db = pulp::gainToDb(level_[c]);
            const float frac = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 63.0f);
            auto fill = bar.withTop(bar.getBottom() - bar.getHeight() * frac);
            g.setColour(level_[c] > 1.0f ? theme::bad : (db > -6.0f ? theme::warn : theme::good));
            g.fillRect(fill);
        }
    }
private:
    void uiTick() override {
        if (!UiTicker::onSlowTick()) return;
        bool changed = false;
        for (int c = 0; c < 2; ++c) {
            const float pk = s_.engine.masterPeak(c);
            const float next = juce::jmax(pk, level_[c] * 0.8f);
            if (std::fabs(next - level_[c]) > 1.0e-5f) changed = true;
            level_[c] = next < 1.0e-5f ? 0.0f : next;
        }
        s_.engine.resetMasterPeak();
        if (changed) repaint();
    }
    Session& s_;
    float level_[2] = {0, 0};
};

Toolbar::Toolbar(Session& s)
    : session_(s), masterVol_(&s.project.global->masterVolume), masterPitch_(&s.project.global->masterPitch) {
    scope_ = std::make_unique<Scope>(s);
    meter_ = std::make_unique<Meter>(s);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&patSong_, &play_, &stop_, &rec_, &metro_, &keys_, &countIn_, &snap_, &tempo_, &masterVol_, &masterPitch_, scope_.get(), meter_.get()})
        addAndMakeVisible(c);
    countIn_.text = "IN 0";
    countIn_.setTooltip("Count-in: click to choose none, one bar or two bars of metronome before play or record starts");
    countIn_.onClick = [this] { session_.countInBars = (session_.countInBars + 1) % 3; session_.notify(); };

    rec_.litColour = theme::bad;
    rec_.setTooltip("Record (R): arm it, then press play. Notes you play are written into the pattern; every mixer track with an input selected records a take into the playlist.");
    rec_.setTooltip("Record (R): arm it, then press play. Notes you play are written into the pattern; every mixer track with an input selected records a take into the playlist. "
                    "Right-click for loop takes, punch in / out, wait for input and overdub / replace.");
    rec_.onClick = [this] { session_.recording = !session_.recording; session_.notify(); };
    rec_.onRightClick = [this] { recordMenu(); };
    metro_.setTooltip("Metronome (Ctrl+M): a click on every beat while playing. It is never part of an export. Right-click to choose its sound.");
    metro_.onClick = [this] { session_.setMetronome(!session_.engine.metronome()); };
    metro_.onRightClick = [this] {
        juce::PopupMenu m;
        m.addSectionHeader("Metronome sound");
        const char* names[pulp::Engine::NumClickSounds] = {"Beep", "Tick", "Wood block", "Bell"};
        for (int i = 0; i < pulp::Engine::NumClickSounds; ++i) m.addItem(i + 1, names[i], true, session_.engine.metronomeSound() == i);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&metro_), [this](int r) {
            if (r > 0) { session_.engine.setMetronomeSound(r - 1); session_.saveRecordOptions(); }
        });
    };
    keys_.text = "KEYS";
    keys_.setTooltip("Typing keyboard (Ctrl+T): play the selected channel from the computer keyboard. Z S X D C ... is one octave, Q 2 W 3 E ... the next.");
    keys_.onClick = [this] { session_.typingKeyboard = !session_.typingKeyboard; session_.notify(); };
    snap_.setParam(&session_.snap);
    snap_.setTooltip("Main snap: the grid that notes and clips snap to. Line follows the zoom level.");

    patSong_.onClick = [this] { session_.setSongMode(!session_.project.songMode); };
    patSong_.setTooltip("Pattern / song mode (L). Pattern mode loops the selected pattern; song mode plays the playlist.");
    play_.onClick = [this] { session_.togglePlay(); };
    play_.setTooltip("Play / stop (Space)");
    play_.litColour = theme::good;
    stop_.onClick = [this] { session_.stop(); };
    stop_.setTooltip("Stop. Press again while stopped to cut every sound.");

    tempo_.caption = "TEMPO";
    tempo_.minValue = 10; tempo_.maxValue = 522; tempo_.step = 1; tempo_.fineStep = 0.1;
    tempo_.getValue = [this] { return (double)session_.project.global->tempo.get(); };
    tempo_.setValue = [this](double v) { session_.setTempo(v); };
    tempo_.onGestureEnd = [this] { session_.commit("Tempo"); };
    masterVol_.onGestureEnd = [this] { session_.commit("Master volume"); };
    masterPitch_.onGestureEnd = [this] { session_.commit("Master pitch"); };
    tempo_.format = [](double v) { return juce::String(v, 1); };
    tempo_.setTooltip("Tempo, 10 to 522 BPM, and the time signature. Drag up or down, Ctrl for fine steps, double-click to type, right-click to tap or to change the time signature.");
    tempo_.onRightClick = [this] {
        juce::PopupMenu m;
        m.addItem(1, "Tap tempo (choose this again in time with the beat)");
        m.addItem(2, "Type in value...");
        // the time signature lives with the tempo
        juce::PopupMenu sig;
        static const int sigs[][2] = {{2, 4}, {3, 4}, {4, 4}, {5, 4}, {6, 4}, {7, 4}, {6, 8}, {7, 8}, {9, 8}, {12, 8}};
        for (int i = 0; i < 10; ++i)
            sig.addItem(200 + i, juce::String(sigs[i][0]) + " / " + juce::String(sigs[i][1]), true, session_.project.timeSigNum == sigs[i][0] && session_.project.timeSigDen == sigs[i][1]);
        m.addSubMenu("Time signature (" + juce::String(session_.project.timeSigNum) + " / " + juce::String(session_.project.timeSigDen) + ")", sig);
        m.addSeparator();
        if (ParamUi::addMenuItems) ParamUi::addMenuItems(&session_.project.global->tempo, m, 100);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&tempo_), [this](int r) {
            if (r == 1) session_.tapTempo();
            else if (r == 2) askForText("Tempo", "Beats per minute (10 to 522)", juce::String(session_.project.global->tempo.get(), 1), this,
                                        [this](juce::String t) { session_.setTempo(t.getDoubleValue()); });
            else if (r >= 200 && r < 210) { static const int sigs[][2] = {{2, 4}, {3, 4}, {4, 4}, {5, 4}, {6, 4}, {7, 4}, {6, 8}, {7, 8}, {9, 8}, {12, 8}}; session_.setTimeSignature(sigs[r - 200][0], sigs[r - 200][1]); }
            else if (r >= 100 && ParamUi::menuChosen) ParamUi::menuChosen(&session_.project.global->tempo, r - 100);
        });
    };
    masterVol_.label = "VOL";
    masterPitch_.label = "PITCH";
    masterPitch_.bipolar = true;
    masterPitch_.colour = theme::accent2;
    projectChanged();
    session_.addListener(this);
}

Toolbar::~Toolbar() { session_.removeListener(this); }

void Toolbar::recordMenu() {
    auto& o = session_.recOpt;
    const bool region = session_.project.loopEnd > session_.project.loopStart;
    juce::PopupMenu m;
    m.addSectionHeader("Recording audio");
    m.addItem(1, "Loop recording: with a loop region set, keep looping and stack every pass as a take", true, o.loopTakes);
    m.addItem(2, region ? "Punch in / out: record only inside the loop region" : "Punch in / out: record only inside the loop region  (Ctrl-drag the playlist ruler to set one)", true, o.punch);
    m.addItem(3, "Wait for input: start when you sing, play or hit a key", true, o.waitForInput);
    m.addSectionHeader("Recording notes");
    m.addItem(4, "Overdub: keep the notes already there", true, !o.replaceNotes);
    m.addItem(5, "Replace: wipe the notes the playhead passes", true, o.replaceNotes);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&rec_), [this](int r) {
        auto& opt = session_.recOpt;
        if (r == 1) opt.loopTakes = !opt.loopTakes;
        else if (r == 2) opt.punch = !opt.punch;
        else if (r == 3) opt.waitForInput = !opt.waitForInput;
        else if (r == 4) opt.replaceNotes = false;
        else if (r == 5) opt.replaceNotes = true;
        if (r > 0) session_.saveRecordOptions();
    });
}

void Toolbar::addWindowButton(const juce::String& label, const juce::String& hint, std::function<void()> toggle, std::function<bool()> isOpen) {
    WinButton w;
    w.button = std::make_unique<IconButton>(IconButton::Text);
    w.button->text = label;
    w.button->setTooltip(hint);
    w.button->onClick = std::move(toggle);
    w.isOpen = std::move(isOpen);
    addAndMakeVisible(*w.button);
    windows_.push_back(std::move(w));
    resized();
}

void Toolbar::addMenuButton(const juce::String& label, const juce::String& hint, std::function<void(juce::Component*)> open) {
    auto b = std::make_unique<IconButton>(IconButton::Text);
    b->text = label;
    b->setTooltip(hint);
    auto* raw = b.get();
    b->onClick = [raw, open] { open(raw); };
    addAndMakeVisible(*b);
    menus_.push_back(std::move(b));
    resized();
}

void Toolbar::projectChanged() {
    masterVol_.setParam(&session_.project.global->masterVolume);
    masterPitch_.setParam(&session_.project.global->masterPitch);
    patSong_.text = session_.project.songMode ? "SONG" : "PAT";
    patSong_.setLit(session_.project.songMode);
    patSong_.repaint();
    // the tempo box says the time signature too
    tempo_.caption = "TEMPO   " + juce::String(session_.project.timeSigNum) + "/" + juce::String(session_.project.timeSigDen);
    tempo_.repaint();
    repaint();
}

juce::String Toolbar::getTooltip() {
    const auto m = getMouseXYRelative();
    if (timeArea_.contains(m)) return "Song position as bar : step : tick";
    if (statsArea_.contains(m)) return "Audio thread load, sounding voices, and buffers that arrived late. " + session_.deviceSummary();
    if (brandArea_.contains(m)) return session_.deviceSummary();
    if (patternArea_.contains(m)) return "Pattern selector: click for the list, wheel or numpad + and - to step, F4 for the next empty pattern, right-click to rename";
    return {};
}

void Toolbar::setHint(const juce::String& h) {
    if (h != hint_) { hint_ = h; repaint(hintArea_); }
}

void Toolbar::patternMenu() {
    juce::PopupMenu m;
    m.addSectionHeader("Patterns");
    auto& p = session_.project;
    for (int i = 0; i < (int)p.patterns.size(); ++i)
        m.addItem(100 + i, juce::String(i + 1) + ".  " + p.patterns[(size_t)i].name + (p.patterns[(size_t)i].empty() ? "   (empty)" : ""), true, i == p.currentPattern);
    m.addSeparator();
    m.addItem(1, "Next empty pattern  (F4)");
    m.addItem(2, "Clone this pattern");
    m.addItem(3, "Rename this pattern...");
    m.addItem(4, p.patterns.size() > 1 ? "Delete this pattern" : "Clear this pattern");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(patternArea_)), [this](int r) {
        if (r >= 100) session_.selectPattern(r - 100);
        else if (r == 1) session_.newPattern();
        else if (r == 2) session_.clonePattern();
        else if (r == 3) askForText("Rename pattern", "Pattern name", session_.project.pattern().name, this, [this](juce::String t) {
            if (t.trim().isNotEmpty()) { session_.project.pattern().name = t.trim().toStdString(); session_.commit("Rename pattern"); }
        });
        else if (r == 4) session_.deletePattern();
    });
}

void Toolbar::mouseDown(const juce::MouseEvent& e) {
    if (!patternArea_.contains(e.getPosition())) return;
    if (e.mods.isPopupMenu())
        askForText("Rename pattern", "Pattern name", session_.project.pattern().name, this, [this](juce::String t) {
            if (t.trim().isNotEmpty()) { session_.project.pattern().name = t.trim().toStdString(); session_.commit("Rename pattern"); }
        });
    else patternMenu();
}

void Toolbar::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    if (patternArea_.contains(e.getPosition())) session_.selectPattern(session_.project.currentPattern + (w.deltaY > 0 ? -1 : 1));
}

void Toolbar::uiTick() {
    if (!UiTicker::onSlowTick()) return;
    const bool playing = session_.engine.isPlaying();
    // while armed and waiting for input the play button shows amber
    const bool waiting = session_.engine.waitingForInput();
    if (playing != wasPlaying_ || waiting != wasWaiting_) {
        wasPlaying_ = playing;
        wasWaiting_ = waiting;
        play_.litColour = waiting ? theme::warn : theme::good;
        play_.lit = playing || waiting;
        play_.repaint();
    }
    // position as bar : step : tick
    const double t = juce::jmax(0.0, session_.engine.positionTicks());
    const int tpb = session_.project.ticksPerBar();
    const int bar = (int)(t / tpb) + 1;
    const int step = (int)(std::fmod(t, (double)tpb) / pulp::kTicksPerStep) + 1;
    const int tick = (int)std::fmod(t, (double)pulp::kTicksPerStep);
    const auto time = juce::String(bar) + ":" + juce::String(step).paddedLeft('0', 2) + ":" + juce::String(tick).paddedLeft('0', 3);
    if (time != time_) { time_ = time; repaint(timeArea_); }
    const auto stats = juce::String(juce::roundToInt(session_.engine.cpuLoad() * 100.0f)) + "|" + juce::String(session_.engine.activeVoices())
                     + "|" + juce::String(session_.xruns());
    if (stats != stats_) { stats_ = stats; repaint(statsArea_); }
    for (auto& w : windows_) w.button->setLit(w.isOpen && w.isOpen());
    rec_.setLit(session_.recording);
    rec_.litColour = session_.audioRecording() ? theme::bad.brighter(0.4f) : theme::bad;
    const auto ci = "IN " + juce::String(session_.countInBars);
    if (countIn_.text != ci) { countIn_.text = ci; countIn_.repaint(); }
    countIn_.setLit(session_.countInBars > 0);
    metro_.setLit(session_.engine.metronome());
    keys_.setLit(session_.typingKeyboard);
}

void Toolbar::resized() {
    auto r = getLocalBounds().reduced(4);
    const int rightW = juce::jmin(838, r.getWidth() - 300);
    auto right = r.removeFromRight(rightW);
    r.removeFromRight(6);

    // left block: brand and window buttons on top, hint bar under them
    auto top = r.removeFromTop(24);
    brandArea_ = top.removeFromLeft(64);
    top.removeFromLeft(2);
    for (auto& m : menus_) { m->setBounds(top.removeFromLeft(42)); top.removeFromLeft(2); }
    top.removeFromLeft(6);
    for (auto& w : windows_) {
        const int bw = juce::jmax(44, 12 + w.button->text.length() * 7);
        w.button->setBounds(top.removeFromLeft(bw));
        top.removeFromLeft(2);
    }
    top.removeFromLeft(8);
    metro_.setBounds(top.removeFromLeft(28));
    top.removeFromLeft(2);
    keys_.setBounds(top.removeFromLeft(46));
    top.removeFromLeft(2);
    countIn_.setBounds(top.removeFromLeft(40));
    top.removeFromLeft(6);
    snap_.setBounds(top.removeFromLeft(92));
    r.removeFromTop(4);
    hintArea_ = r;

    auto row = right.reduced(0, 6);
    patSong_.setBounds(row.removeFromLeft(52));
    row.removeFromLeft(4);
    play_.setBounds(row.removeFromLeft(44));
    row.removeFromLeft(2);
    stop_.setBounds(row.removeFromLeft(44));
    row.removeFromLeft(2);
    rec_.setBounds(row.removeFromLeft(44));
    row.removeFromLeft(8);
    timeArea_ = row.removeFromLeft(96);
    row.removeFromLeft(6);
    tempo_.setBounds(row.removeFromLeft(84));
    row.removeFromLeft(6);
    patternArea_ = row.removeFromLeft(150);
    row.removeFromLeft(8);
    masterVol_.setBounds(row.removeFromLeft(40));
    masterPitch_.setBounds(row.removeFromLeft(40));
    row.removeFromLeft(6);
    statsArea_ = row.removeFromLeft(88);
    row.removeFromLeft(6);
    meter_->setBounds(row.removeFromRight(16));
    row.removeFromRight(4);
    scope_->setBounds(row);
}

void Toolbar::paint(juce::Graphics& g) {
    g.fillAll(theme::panel);
    g.setColour(theme::line);
    g.drawHorizontalLine(getHeight() - 1, 0.0f, (float)getWidth());

    g.setColour(theme::accent);
    g.setFont(theme::font(22.0f, true));
    g.drawText("PULP", brandArea_, juce::Justification::centredLeft, false);

    theme::insetBox(g, hintArea_.toFloat());
    g.setColour(theme::text);
    g.setFont(theme::font(13.0f));
    g.drawFittedText(hint_, hintArea_.reduced(8, 2), juce::Justification::centredLeft, 2);

    theme::insetBox(g, timeArea_.toFloat());
    auto ta = timeArea_.reduced(4, 1);
    g.setColour(theme::textDim);
    g.setFont(theme::font(9.5f));
    g.drawText("BAR:STEP:TICK", ta.removeFromTop(11), juce::Justification::centredLeft, false);
    g.setColour(theme::accent2);
    g.setFont(theme::mono(18.0f));
    g.drawText(time_, ta, juce::Justification::centredRight, false);

    theme::insetBox(g, patternArea_.toFloat());
    auto pa = patternArea_.reduced(5, 1);
    g.setColour(theme::textDim);
    g.setFont(theme::font(9.5f));
    g.drawText("PATTERN " + juce::String(session_.project.currentPattern + 1) + " OF " + juce::String((int)session_.project.patterns.size()),
               pa.removeFromTop(11), juce::Justification::centredLeft, false);
    g.setColour(theme::accent);
    g.setFont(theme::font(15.0f, true));
    g.drawFittedText(session_.project.pattern().name, pa, juce::Justification::centredLeft, 1, 0.8f);

    theme::insetBox(g, statsArea_.toFloat());
    auto parts = juce::StringArray::fromTokens(stats_, "|", "");
    auto sa = statsArea_.reduced(5, 3);
    g.setFont(theme::font(11.0f));
    const char* names[3] = {"CPU", "VOICES", "UNDERRUNS"};
    for (int i = 0; i < 3; ++i) {
        auto line = sa.removeFromTop(sa.getHeight() / (3 - i));
        g.setColour(theme::textDim);
        g.drawText(names[i], line, juce::Justification::centredLeft, false);
        g.setColour(i == 2 && parts[2].getIntValue() > 0 ? theme::bad : theme::text);
        g.drawText(parts[i] + (i == 0 ? "%" : ""), line, juce::Justification::centredRight, false);
    }
}
