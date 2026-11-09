#include "BeatViews.h"
#include "ChannelEditor.h"

// ------------------------------------------------------------------ PadGrid

PadGrid::PadGrid(Session& s) : s_(s) {
    kitBtn_.text = "LOAD A FOLDER OF DRUMS...";
    kitBtn_.setTooltip("Fill the pads from a folder of drum sounds. Kicks, snares, hats and the rest go to their usual pads by the words in their file names.");
    kitBtn_.onClick = [this] {
        auto chooser = std::make_shared<juce::FileChooser>("A folder of drum sounds", Session::factoryDir().getChildFile("Drums"), juce::String());
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                             [safe = juce::Component::SafePointer<PadGrid>(this), chooser](const juce::FileChooser& fc) {
            if (safe != nullptr && fc.getResult().isDirectory()) safe->s_.loadPadKit(safe->s_.currentChannel, fc.getResult());
        });
    };
    clearBtn_.text = "CLEAR ALL";
    clearBtn_.setTooltip("Empty every pad");
    clearBtn_.onClick = [this] { if (auto* k = kit()) { k->clearAll(); s_.commit("Clear pads"); } };
    addAndMakeVisible(kitBtn_);
    addAndMakeVisible(clearBtn_);
}

pulp::PadKit* PadGrid::kit() const {
    auto* ch = s_.current();
    return ch ? dynamic_cast<pulp::PadKit*>(ch->live->plugin.get()) : nullptr;
}

juce::Rectangle<int> PadGrid::cell(int pad) const {
    const int w = (getWidth() - 12) / 4, h = 46;
    return juce::Rectangle<int>(6 + (pad % 4) * w, 2 + (3 - pad / 4) * h, w, h).reduced(2);
}

int PadGrid::padAt(juce::Point<int> p) const {
    for (int i = 0; i < pulp::PadKit::kPads; ++i) if (cell(i).contains(p)) return i;
    return -1;
}

void PadGrid::resized() {
    auto row = getLocalBounds().removeFromBottom(30).reduced(6, 3);
    kitBtn_.setBounds(row.removeFromLeft(200));
    row.removeFromLeft(4);
    clearBtn_.setBounds(row.removeFromLeft(90));
}

void PadGrid::uiTick() {
    if (!UiTicker::onSlowTick()) return;
    auto* k = kit();
    if (!k || !isShowing()) return;
    bool changed = false;
    for (int i = 0; i < pulp::PadKit::kPads; ++i) {
        const float m = k->meter(i);
        if (std::abs(m - lit_[i]) > 0.01f) { lit_[i] = m; changed = true; }
    }
    if (changed) repaint();
}

void PadGrid::paint(juce::Graphics& g) {
    auto* k = kit();
    if (!k) return;
    for (int i = 0; i < pulp::PadKit::kPads; ++i) {
        const auto r = cell(i).toFloat();
        const int layers = k->layerCount(i);
        const auto base = layers > 0 ? theme::panelHi : theme::panelLo;
        g.setColour(base.interpolatedWith(theme::accent, juce::jlimit(0.0f, 1.0f, lit_[i] * 1.2f)));
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(i == selected ? theme::accent : theme::line);
        g.drawRoundedRectangle(r.reduced(0.5f), 4.0f, i == selected ? 1.6f : 1.0f);
        g.setColour(theme::textDim);
        g.setFont(theme::font(9.5f));
        g.drawText(juce::String(i + 1) + "  " + KeyboardStrip::keyName(pulp::PadKit::kBaseKey + i), r.reduced(4.0f, 2.0f), juce::Justification::topLeft, false);
        if (layers > 1) g.drawText(juce::String(layers) + " layers", r.reduced(4.0f, 2.0f), juce::Justification::topRight, false);
        g.setColour(layers > 0 ? theme::text : theme::textDim);
        g.setFont(theme::font(11.5f, layers > 0));
        g.drawFittedText(layers > 0 ? juce::String(k->padName(i)) : juce::String("empty"), cell(i).reduced(4).withTrimmedTop(12), juce::Justification::centred, 2);
    }
}

juce::String PadGrid::getTooltip() {
    const int pad = padAt(getMouseXYRelative());
    auto* k = kit();
    if (pad < 0 || !k) return {};
    return "Pad " + juce::String(pad + 1) + (k->layerCount(pad) > 0 ? ": " + juce::String(k->padName(pad)) : juce::String(" (empty)"))
         + ". Click to hear it (lower in the pad is louder) and to show its knobs. Drop a sound on it to load it; hold Ctrl while dropping to add a layer "
           "(harder hits play later layers). Right-click for more. In the piano roll this pad is the key " + KeyboardStrip::keyName(pulp::PadKit::kBaseKey + pad) + ".";
}

void PadGrid::mouseDown(const juce::MouseEvent& e) {
    const int pad = padAt(e.getPosition());
    if (pad < 0) return;
    if (pad != selected) { selected = pad; if (onSelect) onSelect(pad); repaint(); }
    if (e.mods.isPopupMenu()) { padMenu(pad); return; }
    const auto c = cell(pad);
    const float vel = juce::jlimit(0.15f, 1.0f, 0.25f + 0.75f * (float)(e.y - c.getY()) / (float)juce::jmax(1, c.getHeight()));
    down_ = pad;
    s_.engine.noteOn(s_.currentChannel, pulp::PadKit::kBaseKey + pad, vel);
}

void PadGrid::mouseUp(const juce::MouseEvent&) {
    if (down_ >= 0) s_.engine.noteOff(s_.currentChannel, pulp::PadKit::kBaseKey + down_);
    down_ = -1;
}

void PadGrid::padMenu(int pad) {
    auto* k = kit();
    if (!k) return;
    juce::PopupMenu m;
    m.addSectionHeader("Pad " + juce::String(pad + 1) + (k->layerCount(pad) > 0 ? ": " + juce::String(k->padName(pad)) : juce::String()));
    m.addItem(1, "Load a sound...");
    m.addItem(2, "Add a layer...", k->layerCount(pad) > 0 && k->layerCount(pad) < pulp::PadKit::kLayers);
    m.addItem(3, "Rename...", k->layerCount(pad) > 0);
    m.addItem(4, "Clear this pad", k->layerCount(pad) > 0);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(cell(pad))),
                    [safe = juce::Component::SafePointer<PadGrid>(this), pad](int r) {
        if (safe == nullptr || r <= 0) return;
        auto* kit = safe->kit();
        if (!kit) return;
        if (r == 1 || r == 2) {
            auto chooser = std::make_shared<juce::FileChooser>("A drum sound", Session::factoryDir().getChildFile("Drums"), Session::audioFilePattern());
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                 [safe, chooser, pad, add = r == 2](const juce::FileChooser& fc) {
                if (safe != nullptr && fc.getResult().existsAsFile()) safe->s_.setPadSample(safe->s_.currentChannel, pad, fc.getResult(), add);
            });
        }
        else if (r == 3) askForText("Rename pad", "Pad name", kit->padName(pad), safe.getComponent(), [safe, pad](juce::String n) {
            if (safe == nullptr) return;
            if (auto* kk = safe->kit()) { kk->setPadName(pad, n.trim().toStdString()); safe->s_.commit("Rename pad"); }
        });
        else if (r == 4) { kit->clearPad(pad); safe->s_.commit("Clear pad"); }
    });
}

void PadGrid::filesDropped(const juce::StringArray& files, int x, int y) {
    const int pad = padAt({x, y});
    // several files at once are a kit; one file goes to the pad it was dropped on
    if (files.size() > 1) {
        juce::Array<juce::File> list;
        for (auto& f : files) list.add(juce::File(f));
        s_.loadPadKit(s_.currentChannel, list);
    } else if (juce::File(files[0]).isDirectory()) s_.loadPadKit(s_.currentChannel, juce::File(files[0]));
    else if (pad >= 0) {
        s_.setPadSample(s_.currentChannel, pad, juce::File(files[0]), juce::ModifierKeys::currentModifiers.isCtrlDown());
        selected = pad;
        if (onSelect) onSelect(pad);
    }
}

// ------------------------------------------------------------------ SliceView

SliceView::SliceView(Session& s) : s_(s) {
    loadBtn_.text = "LOAD A LOOP...";
    loadBtn_.setTooltip("Choose the loop to cut up (or drop one on this window)");
    loadBtn_.onClick = [this] {
        auto chooser = std::make_shared<juce::FileChooser>("A loop to slice", juce::File(), Session::audioFilePattern());
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [safe = juce::Component::SafePointer<SliceView>(this), chooser](const juce::FileChooser& fc) {
            if (safe != nullptr && fc.getResult().existsAsFile()) safe->s_.setChannelSample(safe->s_.currentChannel, fc.getResult());
        });
    };
    sliceBtn_.text = "SLICE AGAIN";
    sliceBtn_.setTooltip("Find the hits again, using the Sensitivity knob below (higher finds more)");
    sliceBtn_.onClick = [this] { if (auto* c = chop()) { c->detect(); s_.commit("Slice the loop"); } };
    evenBtn_.text = "EVEN...";
    evenBtn_.setTooltip("Cut the loop into equal slices, for loops with no clear hits");
    evenBtn_.onClick = [this] {
        juce::PopupMenu m;
        for (int n : {2, 4, 8, 16, 32}) m.addItem(n, juce::String(n) + " equal slices");
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&evenBtn_), [safe = juce::Component::SafePointer<SliceView>(this)](int r) {
            if (safe == nullptr || r <= 0) return;
            if (auto* c = safe->chop()) { c->sliceEvenly(r); safe->s_.commit("Slice the loop evenly"); }
        });
    };
    writeBtn_.text = "WRITE NOTES";
    writeBtn_.setTooltip("Write one note per slice into this channel's piano roll, each where its slice sits in the loop, so the pattern plays the loop "
                         "at the project's tempo. Then move, delete and repeat the notes to rearrange the beat. Replaces the notes this channel has in the pattern.");
    writeBtn_.onClick = [this] { s_.writeSliceNotes(s_.currentChannel); };
    for (auto* b : {&loadBtn_, &sliceBtn_, &evenBtn_, &writeBtn_}) addAndMakeVisible(*b);
}

pulp::Chop* SliceView::chop() const {
    auto* ch = s_.current();
    return ch ? dynamic_cast<pulp::Chop*>(ch->live->plugin.get()) : nullptr;
}

void SliceView::resized() {
    auto row = getLocalBounds().removeFromBottom(30).reduced(6, 3);
    loadBtn_.setBounds(row.removeFromLeft(108)); row.removeFromLeft(4);
    sliceBtn_.setBounds(row.removeFromLeft(92)); row.removeFromLeft(4);
    evenBtn_.setBounds(row.removeFromLeft(62)); row.removeFromLeft(4);
    writeBtn_.setBounds(row.removeFromLeft(100));
    drawnWidth_ = 0;
}

void SliceView::refresh() { drawnWidth_ = 0; repaint(); }

juce::String SliceView::describe() const {
    auto* c = chop();
    auto s = c ? c->sample() : nullptr;
    if (!s) return "No loop loaded";
    const double seconds = (double)s->frames() / s->sampleRate;
    return juce::String((int)c->slices().size()) + " slices, " + juce::String(c->beats(), 0) + " beats (" + juce::String(c->beats() * 60.0 / seconds, 1) + " BPM), "
         + juce::String(seconds, 2) + " s";
}

int SliceView::sliceAt(int x) const {
    auto* c = chop();
    auto s = c ? c->sample() : nullptr;
    if (!s) return -1;
    const auto area = waveArea();
    const double frame = (double)(x - area.getX()) / (double)juce::jmax(1, area.getWidth()) * (double)s->frames();
    const auto starts = c->slices();
    int found = -1;
    for (int i = 0; i < (int)starts.size(); ++i) if ((double)starts[(size_t)i] <= frame) found = i;
    return frame < 0.0 || frame >= (double)s->frames() ? -1 : found;
}

void SliceView::paint(juce::Graphics& g) {
    const auto area = waveArea();
    theme::insetBox(g, area.toFloat());
    auto* c = chop();
    auto s = c ? c->sample() : nullptr;
    if (!s) {
        g.setColour(theme::textDim);
        g.setFont(theme::font(12.0f));
        g.drawFittedText("Drop a drum loop here. Chop finds its hits and puts each slice on its own key, from C5 upward.", area.reduced(10), juce::Justification::centred, 2);
        return;
    }
    const int w = area.getWidth();
    if (s != drawn_ || w != drawnWidth_) {
        drawn_ = s;
        drawnWidth_ = w;
        lo_.assign((size_t)w, 0.0f);
        hi_.assign((size_t)w, 0.0f);
        const auto frames = s->frames();
        for (int x = 0; x < w; ++x) {
            const pulp::int64 a = frames * x / w, b = juce::jmax<pulp::int64>(a + 1, frames * (x + 1) / w);
            float lo = 0.0f, hi = 0.0f;
            for (pulp::int64 i = a; i < b; ++i) { const float v = s->left()[i]; lo = juce::jmin(lo, v); hi = juce::jmax(hi, v); }
            lo_[(size_t)x] = lo; hi_[(size_t)x] = hi;
        }
    }
    const float mid = (float)area.getCentreY(), half = (float)area.getHeight() * 0.46f;
    const auto starts = c->slices();
    // alternate shading tells the slices apart
    for (size_t i = 0; i < starts.size(); ++i) {
        const float x0 = (float)area.getX() + (float)starts[i] / (float)s->frames() * (float)w;
        const float x1 = (float)area.getX() + (float)(i + 1 < starts.size() ? starts[i + 1] : s->frames()) / (float)s->frames() * (float)w;
        if ((int)i == down_) { g.setColour(theme::accent.withAlpha(0.25f)); g.fillRect(x0, (float)area.getY() + 1.0f, x1 - x0, (float)area.getHeight() - 2.0f); }
        else if (i % 2) { g.setColour(juce::Colours::white.withAlpha(0.035f)); g.fillRect(x0, (float)area.getY() + 1.0f, x1 - x0, (float)area.getHeight() - 2.0f); }
    }
    g.setColour(theme::accent2);
    for (int x = 0; x < w; ++x)
        g.drawVerticalLine(area.getX() + x, mid - hi_[(size_t)x] * half, mid - lo_[(size_t)x] * half + 1.0f);
    g.setFont(theme::font(9.0f));
    for (size_t i = 0; i < starts.size(); ++i) {
        const int x = area.getX() + (int)((double)starts[i] / (double)s->frames() * w);
        g.setColour(theme::accent);
        g.drawVerticalLine(x, (float)area.getY() + 1.0f, (float)area.getBottom() - 1.0f);
        g.setColour(theme::text);
        g.drawText(juce::String((int)i + 1), x + 2, area.getY() + 1, 20, 10, juce::Justification::topLeft, false);
    }
    g.setColour(theme::textDim);
    g.setFont(theme::font(10.5f));
    g.drawText(describe(), area.reduced(4, 2), juce::Justification::bottomRight, false);
}

juce::String SliceView::getTooltip() {
    const int i = sliceAt(getMouseXYRelative().x);
    if (i < 0 || !waveArea().contains(getMouseXYRelative())) return {};
    return "Slice " + juce::String(i + 1) + ": click to hear it. It plays on the key " + KeyboardStrip::keyName(pulp::Chop::kBaseKey + i)
         + ". Right-click to cut here or to join this slice to the one before.";
}

void SliceView::mouseDown(const juce::MouseEvent& e) {
    if (!waveArea().contains(e.getPosition())) return;
    auto* c = chop();
    const int i = sliceAt(e.x);
    if (!c || i < 0) return;
    if (e.mods.isPopupMenu()) {
        auto s = c->sample();
        const auto area = waveArea();
        const pulp::int64 frame = (pulp::int64)((double)(e.x - area.getX()) / (double)juce::jmax(1, area.getWidth()) * (double)s->frames());
        juce::PopupMenu m;
        m.addItem(1, "Cut here", (int)c->slices().size() < pulp::Chop::kMaxSlices);
        m.addItem(2, "Join slice " + juce::String(i + 1) + " to the one before", i > 0);
        m.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<SliceView>(this), frame, i](int r) {
            if (safe == nullptr || r <= 0) return;
            auto* cc = safe->chop();
            if (!cc) return;
            auto starts = cc->slices();
            if (r == 1) starts.push_back(frame);
            else if (r == 2 && i > 0 && i < (int)starts.size()) starts.erase(starts.begin() + i);
            cc->setSlices(starts);
            safe->s_.commit(r == 1 ? "Cut a slice" : "Join slices");
        });
        return;
    }
    down_ = i;
    s_.engine.noteOn(s_.currentChannel, pulp::Chop::kBaseKey + i, pulp::kDefaultVelocity);
    repaint();
}

void SliceView::mouseUp(const juce::MouseEvent&) {
    if (down_ >= 0) s_.engine.noteOff(s_.currentChannel, pulp::Chop::kBaseKey + down_);
    down_ = -1;
    repaint();
}

void SliceView::filesDropped(const juce::StringArray& files, int, int) {
    s_.setChannelSample(s_.currentChannel, juce::File(files[0]));
}
