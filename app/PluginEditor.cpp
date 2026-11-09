#include "PluginEditor.h"
#include "pulp/Effects.h"
#include "pulp/Space.h"
#include "CurveView.h"
#include "CreativeViews.h"
#include "XyPad.h"

PluginEditor::PluginEditor(Session& s, std::shared_ptr<pulp::Plugin> plugin) : s_(s), plugin_(std::move(plugin)) {
    const std::string type = plugin_->typeId();
    if (type == "eq7") display_ = EqCurve;
    else if (type == "comp" || type == "limiter" || type == "gate" || type == "deess") display_ = GainReduction;
    else if (plugin_->numControlOutputs() > 0) {
        display_ = Controller;
        controller_ = std::make_unique<ControllerView>(s_, plugin_);
        addAndMakeVisible(*controller_);
    }
    else if (type == "timewarp") {
        display_ = Custom;
        customHeight_ = RewindCurveView::kHeight;
        custom_ = std::make_unique<RewindCurveView>(s_, plugin_);
        addAndMakeVisible(*custom_);
    }
    else if (type == "scratch") {
        display_ = Custom;
        customHeight_ = PlatterView::kHeight;
        custom_ = std::make_unique<PlatterView>(s_, plugin_);
        addAndMakeVisible(*custom_);
    }
    else if (type == "xyfx") {
        auto* tp = static_cast<pulp::ThumbPad*>(plugin_.get());
        auto pad = std::make_unique<XyParamPad>(s_, plugin_, tp->pX, tp->pY, tp->pActive);
        pad->hint = "The pad: press to bring the effect in, drag to play it, let go and it fades out (switch Active on to keep it in). "
                    "What the two directions do depends on the mode. With record on and the song playing, the movement is recorded.";
        pad->axisLabel = [tp](int axis) { return juce::String(pulp::ThumbPad::axisName(tp->pMode->getInt(), axis)); };
        display_ = Custom;
        customHeight_ = XyParamPad::kHeight;
        custom_ = std::move(pad);
        addAndMakeVisible(*custom_);
    }
    else if (type == "shaper") {
        display_ = Custom;
        customHeight_ = BendCurveView::kHeight;
        custom_ = std::make_unique<BendCurveView>(s_, plugin_);
        addAndMakeVisible(*custom_);
    }
    else if (type == "gauge") {
        display_ = Analyser;
        analyser_ = std::make_unique<AnalyserView>(s_, plugin_);
        addAndMakeVisible(*analyser_);
    }
    else if (type == "editor") {
        display_ = Editor;
        splice_ = std::make_unique<SpliceView>(s_, plugin_);
        addAndMakeVisible(*splice_);
    }

    presets_ = plugin_->factoryPresets();
    presetBtn_.text = "Presets (" + juce::String((int)presets_.size()) + ")";
    presetBtn_.setTooltip("Presets for this effect: click for the list, right-click for the next one");
    presetBtn_.onClick = [this] { presetMenu(); };
    presetBtn_.onRightClick = [this] { if (!presets_.empty()) choosePreset((presetIndex_ + 1) % (int)presets_.size()); };
    if (!presets_.empty()) addAndMakeVisible(presetBtn_);
    if (dynamic_cast<pulp::Spaces*>(plugin_.get())) {
        fileBtn_.text = "LOAD IMPULSE...";
        fileBtn_.setTooltip("Use your own impulse response: a recording of a room, a plate or a speaker (WAV, FLAC or AIFF, up to ten seconds). It is saved with the project.");
        fileBtn_.onClick = [this] {
            auto chooser = std::make_shared<juce::FileChooser>("An impulse response", juce::File(), Session::audioFilePattern());
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                 [safe = juce::Component::SafePointer<PluginEditor>(this), chooser](const juce::FileChooser& fc) {
                if (safe == nullptr || !fc.getResult().existsAsFile()) return;
                std::string err;
                if (static_cast<pulp::Spaces*>(safe->plugin_.get())->loadImpulse(fc.getResult().getFullPathName().toStdString(), &err)) safe->s_.commit("Load impulse response");
                else if (safe->s_.onMessage) safe->s_.onMessage("Could not load that impulse response: " + juce::String(err));
            });
        };
        addAndMakeVisible(fileBtn_);
    }

    std::vector<ParamPanel::Section> sections;
    const auto groups = plugin_->paramGroups();
    if (groups.empty()) {
        ParamPanel::Section sec;
        for (auto& p : plugin_->params()) sec.params.push_back(p.get());
        sections.push_back(std::move(sec));
    } else
        for (auto& gr : groups) {
            ParamPanel::Section sec;
            sec.title = gr.title;
            for (auto& id : gr.ids) if (auto* p = plugin_->param(id)) sec.params.push_back(p);
            sections.push_back(std::move(sec));
        }
    panel_.setSections(std::move(sections));
    panel_.onChange = [this](pulp::Param*) { repaint(); };
    panel_.onGestureEnd = [this](pulp::Param*) { s_.commit("Change effect setting"); };      // latency may have changed too
    view_.setViewedComponent(&panel_, false);
    view_.setScrollBarsShown(true, false);
    view_.setScrollBarThickness(10);
    addAndMakeVisible(view_);
}

int PluginEditor::preferredHeight(int width) {
    return 34 + (display_ == NoDisplay ? 0 : displayHeight() + 6) + panel_.heightFor(width - 12) + 8;
}

juce::Rectangle<int> PluginEditor::displayArea() const {
    if (display_ == NoDisplay) return {};
    return {6, 32, getWidth() - 12, displayHeight()};
}

void PluginEditor::resized() {
    presetBtn_.setBounds(6, 4, juce::jmin(240, getWidth() - 12 - (fileBtn_.isVisible() ? 134 : 0)), 24);
    if (fileBtn_.isVisible()) fileBtn_.setBounds(presetBtn_.getRight() + 4, 4, 130, 24);
    auto r = getLocalBounds().withTrimmedTop(34);
    if (display_ != NoDisplay) r.removeFromTop(displayArea().getHeight() + 6);
    if (splice_) splice_->setBounds(displayArea());
    if (controller_) controller_->setBounds(displayArea());
    if (analyser_) analyser_->setBounds(displayArea());
    if (custom_) custom_->setBounds(displayArea());
    view_.setBounds(r);
    const int w = juce::jmax(200, view_.getMaximumVisibleWidth());
    panel_.setSize(w, panel_.heightFor(w));
}

void PluginEditor::presetMenu() {
    juce::PopupMenu m;
    for (int i = 0; i < (int)presets_.size(); ++i) m.addItem(i + 1, presets_[(size_t)i].name, true, i == presetIndex_);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&presetBtn_),
                    [safe = juce::Component::SafePointer<PluginEditor>(this)](int r) { if (safe && r > 0) safe->choosePreset(r - 1); });
}

void PluginEditor::choosePreset(int index) {
    if (index < 0 || index >= (int)presets_.size()) return;
    plugin_->loadState(presets_[(size_t)index].state, s_.project.projectDir);
    presetIndex_ = index;
    presetBtn_.text = "Preset: " + juce::String(presets_[(size_t)index].name);
    presetBtn_.repaint();
    repaint();
    s_.commit("Choose preset");
}

float PluginEditor::xOfHz(double hz) const {
    const auto a = displayArea();
    return (float)a.getX() + (float)a.getWidth() * (float)(std::log(hz / 20.0) / std::log(1000.0));
}
double PluginEditor::hzOfX(float x) const {
    const auto a = displayArea();
    return 20.0 * std::pow(1000.0, (double)(x - (float)a.getX()) / (double)a.getWidth());
}
float PluginEditor::yOfDb(double db) const {
    const auto a = displayArea();
    return (float)a.getCentreY() - (float)(db / 24.0) * (float)a.getHeight() * 0.5f;
}

void PluginEditor::uiTick() {
    if (!UiTicker::onSlowTick()) return;
    if (display_ == NoDisplay || display_ == Editor || display_ == Controller || display_ == Analyser || display_ == Custom || !isShowing()) return;
    if (display_ == GainReduction) {
        const int w = juce::jmax(8, displayArea().getWidth());
        if ((int)history_.size() != w) history_.assign((size_t)w, 0.0f);
        history_.erase(history_.begin());
        history_.push_back(plugin_->meter(0));
    } else {
        // live spectrum behind the curve
        const int n = 2048;
        std::vector<float> re((size_t)n), im((size_t)n, 0.0f);
        if (plugin_->readScope(re.data(), n) == n) {
            for (int i = 0; i < n; ++i) re[(size_t)i] *= (float)(0.5 - 0.5 * std::cos(pulp::kTwoPi * i / (n - 1)));
            pulp::dsp::fft(re.data(), im.data(), n);
            const auto a = displayArea();
            if ((int)spectrum_.size() != a.getWidth()) spectrum_.assign((size_t)a.getWidth(), -100.0f);
            const double sr = s_.engine.sampleRate();
            for (int x = 0; x < a.getWidth(); ++x) {
                const double f0 = hzOfX((float)(a.getX() + x)), f1 = hzOfX((float)(a.getX() + x + 1));
                const int b0 = juce::jlimit(1, n / 2 - 1, (int)(f0 * n / sr)), b1 = juce::jlimit(b0, n / 2 - 1, (int)(f1 * n / sr));
                float mag = 0.0f;
                for (int b = b0; b <= b1; ++b) mag = std::max(mag, std::hypot(re[(size_t)b], im[(size_t)b]));
                const float db = 20.0f * std::log10(mag / (float)(n / 4) + 1.0e-9f);
                spectrum_[(size_t)x] = std::max(db, spectrum_[(size_t)x] - 1.5f);
            }
        }
    }
    repaint(displayArea());
}

void PluginEditor::paint(juce::Graphics& g) {
    g.fillAll(theme::panel);
    if (display_ == NoDisplay || display_ == Editor || display_ == Controller || display_ == Analyser || display_ == Custom) return;
    const auto a = displayArea();
    theme::insetBox(g, a.toFloat());
    g.saveState();
    g.reduceClipRegion(a.reduced(1));
    if (display_ == GainReduction) {
        // scrolling gain reduction: newest on the right, 0 dB at the top, 24 dB at the bottom
        g.setColour(theme::textDim.withAlpha(0.35f));
        g.setFont(theme::font(9.5f));
        for (int db = 6; db <= 18; db += 6) {
            const float y = (float)a.getY() + (float)a.getHeight() * (float)db / 24.0f;
            g.drawHorizontalLine((int)y, (float)a.getX(), (float)a.getRight());
            g.drawText("-" + juce::String(db) + " dB", a.getX() + 4, (int)y - 11, 50, 10, juce::Justification::centredLeft, false);
        }
        juce::Path p;
        p.startNewSubPath((float)a.getX(), (float)a.getY());
        for (size_t i = 0; i < history_.size(); ++i)
            p.lineTo((float)a.getX() + (float)i, (float)a.getY() + juce::jmin(1.0f, history_[i] / 24.0f) * (float)a.getHeight());
        p.lineTo((float)a.getRight(), (float)a.getY());
        p.closeSubPath();
        g.setColour(theme::accent.withAlpha(0.55f));
        g.fillPath(p);
        g.setColour(theme::text);
        g.setFont(theme::font(11.0f, true));
        g.drawText("Gain reduction  " + juce::String(history_.empty() ? 0.0f : history_.back(), 1) + " dB", a.reduced(6, 3), juce::Justification::topRight, false);
    } else if (auto* eq = dynamic_cast<pulp::ContourEq*>(plugin_.get())) {
        g.setFont(theme::font(9.5f));
        for (double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
            g.setColour(theme::textDim.withAlpha(0.25f));
            g.drawVerticalLine((int)xOfHz(hz), (float)a.getY(), (float)a.getBottom());
            g.setColour(theme::textDim.withAlpha(0.7f));
            g.drawText(hz >= 1000.0 ? juce::String(hz / 1000.0, 0) + "k" : juce::String(hz, 0), (int)xOfHz(hz) + 2, a.getBottom() - 12, 30, 10, juce::Justification::centredLeft, false);
        }
        for (int db = -18; db <= 18; db += 6) {
            g.setColour(theme::textDim.withAlpha(db == 0 ? 0.5f : 0.2f));
            g.drawHorizontalLine((int)yOfDb(db), (float)a.getX(), (float)a.getRight());
        }
        // spectrum
        if (!spectrum_.empty()) {
            juce::Path sp;
            sp.startNewSubPath((float)a.getX(), (float)a.getBottom());
            for (size_t x = 0; x < spectrum_.size(); ++x)
                sp.lineTo((float)a.getX() + (float)x, (float)a.getBottom() - juce::jlimit(0.0f, 1.0f, (spectrum_[x] + 90.0f) / 90.0f) * (float)a.getHeight());
            sp.lineTo((float)a.getRight(), (float)a.getBottom());
            sp.closeSubPath();
            g.setColour(theme::accent2.withAlpha(0.25f));
            g.fillPath(sp);
        }
        // the curve
        juce::Path curve;
        for (int x = 0; x <= a.getWidth(); x += 2) {
            const float y = yOfDb(eq->magnitudeDb(hzOfX((float)(a.getX() + x))));
            if (x == 0) curve.startNewSubPath((float)a.getX(), y); else curve.lineTo((float)(a.getX() + x), y);
        }
        g.setColour(theme::accent);
        g.strokePath(curve, juce::PathStrokeType(2.0f));
        // band handles
        g.setFont(theme::font(10.0f, true));
        for (int b = 0; b < pulp::ContourEq::kBands; ++b) {
            const float x = xOfHz(eq->pFreq[b]->get());
            const float y = eq->pType[b]->getInt() >= pulp::ContourEq::LowCut ? yOfDb(0.0) : yOfDb(eq->pGain[b]->get());
            g.setColour(theme::palette(b + 8).brighter(0.3f));
            g.fillEllipse(x - 7.0f, y - 7.0f, 14.0f, 14.0f);
            g.setColour(juce::Colours::black);
            g.drawText(juce::String(b + 1), (int)x - 7, (int)y - 7, 14, 14, juce::Justification::centred, false);
        }
    }
    g.restoreState();
}

void PluginEditor::mouseDown(const juce::MouseEvent& e) {
    dragBand_ = -1;
    auto* eq = dynamic_cast<pulp::ContourEq*>(plugin_.get());
    if (!eq || !displayArea().contains(e.getPosition())) return;
    float best = 16.0f;
    for (int b = 0; b < pulp::ContourEq::kBands; ++b) {
        const float x = xOfHz(eq->pFreq[b]->get());
        const float y = eq->pType[b]->getInt() >= pulp::ContourEq::LowCut ? yOfDb(0.0) : yOfDb(eq->pGain[b]->get());
        const float d = std::hypot(x - (float)e.x, y - (float)e.y);
        if (d < best) { best = d; dragBand_ = b; }
    }
    if (dragBand_ >= 0 && (e.mods.isPopupMenu() || e.mods.isAltDown())) {      // reset the band
        eq->pGain[dragBand_]->set(0.0f);
        dragBand_ = -1;
        s_.commit("Reset EQ band");
        repaint();
    }
}

void PluginEditor::mouseDrag(const juce::MouseEvent& e) {
    auto* eq = dynamic_cast<pulp::ContourEq*>(plugin_.get());
    if (!eq || dragBand_ < 0) return;
    const auto a = displayArea();
    eq->pFreq[dragBand_]->set((float)juce::jlimit(20.0, 20000.0, hzOfX((float)e.x)));
    eq->pGain[dragBand_]->set(juce::jlimit(-24.0f, 24.0f, (float)(a.getCentreY() - e.y) / ((float)a.getHeight() * 0.5f) * 24.0f));
    repaint();
}

void PluginEditor::mouseUp(const juce::MouseEvent&) {
    if (dragBand_ >= 0) s_.commit("Move EQ band");
    dragBand_ = -1;
}

void PluginEditor::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    auto* eq = dynamic_cast<pulp::ContourEq*>(plugin_.get());
    if (!eq || !displayArea().contains(e.getPosition())) { juce::Component::mouseWheelMove(e, w); return; }
    // the wheel narrows or widens the nearest band
    int band = 0;
    float best = 1.0e9f;
    for (int b = 0; b < pulp::ContourEq::kBands; ++b) { const float d = std::fabs(xOfHz(eq->pFreq[b]->get()) - (float)e.x); if (d < best) { best = d; band = b; } }
    eq->pQ[band]->set(eq->pQ[band]->get() * (w.deltaY > 0 ? 1.15f : 1.0f / 1.15f));
    repaint();
}
