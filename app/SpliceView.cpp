#include "SpliceView.h"

namespace {
struct Def { const char* op; const char* label; const char* hint; };
const Def kButtons[] = {
    {"play", "PLAY", "Play the selection (or everything) through the output"},
    {"trim", "TRIM", "Trim: keep only the selection"},
    {"cut", "CUT", "Cut: remove the selection and close the gap"},
    {"silence", "MUTE", "Silence the selection"},
    {"fadein", "FADE IN", "Fade the selection in"},
    {"fadeout", "FADE OUT", "Fade the selection out"},
    {"normalize", "NORM", "Normalize: make the selection as loud as it can be without clipping"},
    {"reverse", "REV", "Reverse the selection"},
    {"learn", "LEARN", "Noise removal, step 1: select a stretch that holds only the noise, then click this"},
    {"denoise", "DENOISE", "Noise removal, step 2: take the learned noise out of the selection (or everything)"},
    {"undo", "UNDO", "Undo the last edit or recording in this editor"},
    {"new", "NEW", "Clear the editor"},
    {"load", "LOAD", "Load a WAV file into the editor"},
    {"save", "SAVE", "Save the selection (or everything) as a WAV file"},
    {"toplaylist", "TO PLAYLIST", "Put the selection (or everything) on the playlist as an audio clip at the song position"},
};
}

SpliceView::SpliceView(Session& s, std::shared_ptr<pulp::Plugin> plugin) : s_(s), plugin_(std::move(plugin)) {
    for (auto& d : kButtons) {
        auto* b = buttons_.add(new IconButton(IconButton::Text));
        b->text = d.label;
        b->setTooltip(d.hint);
        const juce::String name(d.op);
        b->onClick = [this, name] {
            if (name == "load") {
                auto chooser = std::make_shared<juce::FileChooser>("Load audio into the editor", juce::File(), Session::audioFilePattern());
                chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                     [safe = juce::Component::SafePointer<SpliceView>(this), chooser](const juce::FileChooser& fc) { if (safe && fc.getResult() != juce::File()) safe->loadFile(fc.getResult()); });
            } else if (name == "save") {
                auto chooser = std::make_shared<juce::FileChooser>("Save the audio", s_.editsDir().getChildFile("edit.wav"), "*.wav");
                chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                                     [safe = juce::Component::SafePointer<SpliceView>(this), chooser](const juce::FileChooser& fc) { if (safe && fc.getResult() != juce::File()) safe->saveAs(fc.getResult().withFileExtension("wav")); });
            } else if (name == "toplaylist") toPlaylist();
            else op(name);
        };
        addAndMakeVisible(b);
    }
}

juce::Rectangle<int> SpliceView::waveArea() const { return {0, 52, getWidth(), getHeight() - 52 - 18}; }

void SpliceView::resized() {
    // two rows of buttons above the waveform
    int x = 0, y = 0;
    for (int i = 0; i < buttons_.size(); ++i) {
        const int w = 14 + buttons_[i]->text.length() * 7;
        if (x + w > getWidth()) { x = 0; y += 25; }
        buttons_[i]->setBounds(x, y, w, 23);
        x += w + 2;
    }
}

pulp::int64 SpliceView::frameAtX(int x) const {
    const auto a = ed().audio();
    const auto w = waveArea();
    if (!a || a->frames() <= 0) return 0;
    return juce::jlimit<pulp::int64>(0, a->frames(), (pulp::int64)((double)(x - w.getX()) / (double)juce::jmax(1, w.getWidth()) * (double)a->frames()));
}

juce::String SpliceView::getTooltip() {
    if (waveArea().contains(getMouseXYRelative()))
        return "The editor's audio. Drag to select a range, double-click to select everything, right-click to clear the selection. Drag from the grey bar under the waveform onto the Playlist to place the selection as a clip.";
    if (getMouseXYRelative().y >= getHeight() - 18) return "Drag from here onto the Playlist (or the Channel Rack) to use the selection there";
    return {};
}

void SpliceView::uiTick() {
    if (!UiTicker::onSlowTick()) return;
    if (!isShowing()) return;
    const auto a = ed().audio();
    const pulp::uint64 uid = a ? a->uid.v : 0;
    const pulp::int64 frames = a ? a->frames() : 0;
    if (uid != shownUid_ || frames != shownFrames_ || ed().capturing()) repaint();
}

void SpliceView::paint(juce::Graphics& g) {
    const auto a = ed().audio();
    shownUid_ = a ? a->uid.v : 0;
    shownFrames_ = a ? a->frames() : 0;
    const auto w = waveArea();
    theme::insetBox(g, w.toFloat());
    const bool rec = ed().pRec->getBool();
    if (!a || a->frames() == 0) {
        g.setColour(theme::textDim);
        g.setFont(theme::font(12.5f));
        g.drawFittedText(rec ? (ed().capturing() ? "Recording..." : "Armed: recording starts " + juce::String(ed().pMode->getInt() == pulp::Splice::OnPlay ? "when the song plays" : "when sound arrives"))
                             : juce::String("Nothing here yet. Switch Record on below to capture what passes through this mixer track, or LOAD a file."),
                         w.reduced(12), juce::Justification::centred, 3);
    } else {
        const auto frames = a->frames();
        const double perPx = (double)frames / (double)juce::jmax(1, w.getWidth());
        // selection
        const auto selA = ed().selA, selB = ed().selB;
        if (selB > selA) {
            g.setColour(theme::accent.withAlpha(0.22f));
            g.fillRect((float)w.getX() + (float)(selA / perPx), (float)w.getY() + 1.0f, (float)((selB - selA) / perPx), (float)w.getHeight() - 2.0f);
        }
        const int chans = a->channels;
        for (int c = 0; c < chans; ++c) {
            const float h = (float)w.getHeight() / (float)chans, mid = (float)w.getY() + h * ((float)c + 0.5f);
            const float* d = a->ch[c].data();
            g.setColour(theme::accent2);
            for (int x = 0; x < w.getWidth(); ++x) {
                const auto i0 = (pulp::int64)(x * perPx), i1 = juce::jmin(frames, (pulp::int64)((x + 1) * perPx) + 1);
                float mn = 0.0f, mx = 0.0f;
                const auto stride = juce::jmax<pulp::int64>(1, (i1 - i0) / 48);
                for (auto i = i0; i < i1; i += stride) { mn = juce::jmin(mn, d[i]); mx = juce::jmax(mx, d[i]); }
                g.drawVerticalLine(w.getX() + x, mid - mx * h * 0.48f, mid - mn * h * 0.48f + 1.0f);
            }
            g.setColour(theme::line);
            g.drawHorizontalLine((int)mid, (float)w.getX(), (float)w.getRight());
        }
        if (selB > selA) {
            g.setColour(theme::accent);
            g.drawVerticalLine(w.getX() + (int)(selA / perPx), (float)w.getY(), (float)w.getBottom());
            g.drawVerticalLine(w.getX() + (int)(selB / perPx), (float)w.getY(), (float)w.getBottom());
        }
        if (ed().capturing()) {
            g.setColour(theme::bad);
            g.setFont(theme::font(11.0f, true));
            g.drawText("RECORDING", w.reduced(6, 3), juce::Justification::topRight, false);
        }
    }
    // the bar under the waveform: length, selection, and the handle to drag the audio out by
    const juce::Rectangle<int> bar(0, getHeight() - 17, getWidth(), 17);
    g.setColour(theme::panelHi);
    g.fillRect(bar);
    g.setColour(theme::text);
    g.setFont(theme::font(11.0f));
    juce::String info;
    if (a && a->frames() > 0) {
        const double sr = a->sampleRate;
        info << juce::String((double)a->frames() / sr, 2) << " s, " << (a->channels > 1 ? "stereo" : "mono") << ", " << juce::String(sr, 0) << " Hz";
        if (ed().selB > ed().selA) info << "    selection " << juce::String((double)ed().selA / sr, 3) << " to " << juce::String((double)ed().selB / sr, 3) << " s";
        if (ed().unsaved()) info << "    (not in a file yet: saved with the project)";
        if (ed().noise.valid()) info << "    noise learned";
    }
    if (status_.isNotEmpty()) info << "    " << status_;
    g.drawText(info, bar.reduced(6, 0), juce::Justification::centredLeft, true);
    g.setColour(theme::textDim);
    g.drawText("drag to playlist", bar.reduced(6, 0), juce::Justification::centredRight, false);
}

void SpliceView::mouseDown(const juce::MouseEvent& e) {
    selecting_ = draggingOut_ = false;
    if (e.y >= getHeight() - 18) { draggingOut_ = true; return; }
    if (!waveArea().contains(e.getPosition())) return;
    if (e.mods.isPopupMenu()) { ed().selA = ed().selB = 0; repaint(); return; }
    dragAnchor_ = frameAtX(e.x);
    ed().selA = ed().selB = dragAnchor_;
    selecting_ = true;
    repaint();
}

void SpliceView::mouseDrag(const juce::MouseEvent& e) {
    if (draggingOut_) {
        if (e.getDistanceFromDragStart() > 6)
            if (auto* dc = juce::DragAndDropContainer::findParentDragContainerFor(this)) {
                draggingOut_ = false;
                const auto f = writeSelection("clip");
                if (f != juce::File()) dc->startDragging("file:" + f.getFullPathName(), this);
            }
        return;
    }
    if (!selecting_) return;
    const auto f = frameAtX(e.x);
    ed().selA = juce::jmin(dragAnchor_, f);
    ed().selB = juce::jmax(dragAnchor_, f);
    repaint();
}

void SpliceView::mouseUp(const juce::MouseEvent&) { selecting_ = draggingOut_ = false; }

void SpliceView::mouseDoubleClick(const juce::MouseEvent& e) {
    if (waveArea().contains(e.getPosition())) op("all");
}

void SpliceView::setSelection(double fromSeconds, double toSeconds) {
    const auto a = ed().audio();
    if (!a) return;
    ed().selA = juce::jlimit<pulp::int64>(0, a->frames(), (pulp::int64)(fromSeconds * a->sampleRate));
    ed().selB = juce::jlimit<pulp::int64>(ed().selA, a->frames(), (pulp::int64)(toSeconds * a->sampleRate));
    repaint();
}

void SpliceView::apply(std::shared_ptr<pulp::SampleData> next, const juce::String& what) {
    ed().setAudio(next, true);
    status_ = what;
    s_.dirty = true;
    s_.notify();
    repaint();
}

bool SpliceView::op(const juce::String& name) {
    auto a = ed().audio();
    const auto selA = ed().selA, selB = ed().selB;
    if (name == "undo") { const bool ok = ed().undo(); status_ = ok ? "undone" : "nothing to undo"; repaint(); return ok; }
    if (name == "new") { ed().clear(); status_.clear(); s_.dirty = true; s_.notify(); repaint(); return true; }
    if (name == "stop") { s_.stopPreview(); return true; }
    if (!a || a->frames() == 0) { status_ = "no audio"; repaint(); return false; }
    if (name == "all") { ed().selA = 0; ed().selB = a->frames(); repaint(); return true; }
    if (name == "play") { s_.engine.preview.play(pulp::edit::slice(*a, selA, selB), 0.8f); return true; }
    if (name == "trim") { apply(pulp::edit::trim(*a, selA, selB), "trimmed"); ed().selA = ed().selB = 0; return true; }
    if (name == "cut") { apply(pulp::edit::cut(*a, selA, selB), "cut"); ed().selB = ed().selA; return true; }
    if (name == "silence") { apply(pulp::edit::silence(*a, selA, selB), "silenced"); return true; }
    if (name == "fadein") { apply(pulp::edit::fade(*a, selA, selB, true), "faded in"); return true; }
    if (name == "fadeout") { apply(pulp::edit::fade(*a, selA, selB, false), "faded out"); return true; }
    if (name == "normalize") { apply(pulp::edit::normalize(*a, selA, selB), "normalized"); return true; }
    if (name == "reverse") { apply(pulp::edit::reverse(*a, selA, selB), "reversed"); return true; }
    if (name == "learn") {
        ed().noise = pulp::edit::learnNoise(*a, selA, selB);
        status_ = ed().noise.valid() ? "noise learned: now select what to clean and press DENOISE" : "select a longer stretch of noise (at least 50 ms)";
        repaint();
        return ed().noise.valid();
    }
    if (name == "denoise") {
        if (!ed().noise.valid()) { status_ = "learn the noise first: select noise only, press LEARN"; repaint(); return false; }
        apply(pulp::edit::removeNoise(*a, selA, selB, ed().noise, 0.85f), "noise removed");
        return true;
    }
    return false;
}

bool SpliceView::loadFile(const juce::File& f) {
    std::string err;
    auto sample = pulp::readWavFile(f.getFullPathName().toStdString(), &err);
    if (!sample) { if (s_.onMessage) s_.onMessage("Could not load " + f.getFileName() + ": " + juce::String(err)); return false; }
    ed().setAudio(sample, true);
    ed().selA = ed().selB = 0;
    status_ = "loaded " + f.getFileName();
    s_.dirty = true;
    s_.notify();
    repaint();
    return true;
}

bool SpliceView::saveAs(const juce::File& f) {
    const auto a = ed().audio();
    if (!a) return false;
    const auto part = pulp::edit::slice(*a, ed().selA, ed().selB);
    if (!pulp::writeWavFile(f.getFullPathName().toStdString(), *part, pulp::WavFormat::Float32)) { if (s_.onMessage) s_.onMessage("Could not write " + f.getFullPathName()); return false; }
    status_ = "saved " + f.getFileName();
    repaint();
    return true;
}

juce::File SpliceView::writeSelection(const juce::String& prefix) {
    const auto a = ed().audio();
    if (!a || a->frames() == 0) return {};
    const auto dir = s_.editsDir().getChildFile("edits");
    dir.createDirectory();
    const auto f = dir.getNonexistentChildFile(prefix + "_" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S"), ".wav", false);
    const auto part = pulp::edit::slice(*a, ed().selA, ed().selB);
    if (!pulp::writeWavFile(f.getFullPathName().toStdString(), *part, pulp::WavFormat::Float32)) return {};
    return f;
}

int SpliceView::toPlaylist() {
    const auto f = writeSelection("clip");
    if (f == juce::File()) { status_ = "no audio"; repaint(); return -1; }
    // the first empty track, at the song position (the start of the bar it is in)
    int track = 0;
    for (; track < pulp::kNumPlaylistTracks - 1; ++track) {
        bool used = false;
        for (auto& c : s_.project.clips) if (c.track == track) { used = true; break; }
        if (!used) break;
    }
    const int bar = s_.project.ticksPerBar();
    const auto tick = s_.project.songMode ? ((pulp::int64)juce::jmax(0.0, s_.engine.positionTicks()) / bar) * bar : 0;
    const int clip = s_.addAudioFile(f, track, tick);
    status_ = clip >= 0 ? "placed on playlist track " + juce::String(track + 1) : juce::String("could not place it");
    repaint();
    return clip;
}

juce::String SpliceView::describe() const {
    const auto a = ed().audio();
    juce::String out = "editor rec=" + juce::String((int)ed().pRec->getBool()) + " capturing=" + juce::String((int)ed().capturing());
    if (a) out << " frames=" << a->frames() << " ch=" << a->channels << " peak=" << juce::String(a->peak(), 4) << " sel=" << ed().selA << ".." << ed().selB;
    else out << " empty";
    out << " unsaved=" << (int)ed().unsaved() << " undo=" << (int)ed().canUndo() << " noise=" << (int)ed().noise.valid() << " status=" << status_;
    return out;
}
