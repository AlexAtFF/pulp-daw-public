// The piano roll's tools menu: immediate tools, tools with a settings panel (live preview,
// then Accept or Cancel), the riff generator, and MIDI import / export.
#include "PianoRoll.h"
#include "pulp/MidiFile.h"

using pulp::kTicksPerStep;
using pulp::Note;
namespace tools = pulp::tools;

namespace {
enum ToolId { QuickQuantize = 1, Quantize, Legato, Chop, Glue, Arpeggiate, Strum, Flam, Articulate, Randomize, Levels, Flip, Reverse,
              Lfo, Trimmer, Riff, ImportMidi, ExportMidi, Fit, DialogCancel = -1, DialogAccept = -2 };

pulp::ParamInfo pf(const char* id, const char* name, float mn, float mx, float def, const char* unit = "") {
    pulp::ParamInfo i; i.id = id; i.name = name; i.min = mn; i.max = mx; i.def = def; i.unit = unit; return i;
}
pulp::ParamInfo pi(const char* id, const char* name, int mn, int mx, int def, const char* unit = "") {
    pulp::ParamInfo i; i.id = id; i.name = name; i.min = (float)mn; i.max = (float)mx; i.def = (float)def; i.unit = unit; i.steps = mx - mn + 1; return i;
}
pulp::ParamInfo pc(const char* id, const char* name, std::vector<std::string> choices, int def) {
    pulp::ParamInfo i; i.id = id; i.name = name; i.min = 0; i.max = (float)(choices.size() - 1); i.def = (float)def; i.steps = (int)choices.size(); i.choices = std::move(choices); return i;
}
pulp::ParamInfo pb(const char* id, const char* name, bool def) { return pc(id, name, {"Off", "On"}, def ? 1 : 0); }

const std::vector<std::string> kGridNames = {"Snap setting", "1/4 step", "1/3 step", "1/2 step", "Step", "1/3 beat", "1/2 beat", "Beat"};
const int kGridTicks[] = {0, 60, 80, 120, 240, 320, 480, 960};
std::vector<std::string> names(const std::vector<tools::NamedIntervals>& v) { std::vector<std::string> o; for (auto& n : v) o.push_back(n.name); return o; }
}

class PianoRoll::Dialog : public juce::Component {
public:
    using Apply = std::function<void(std::vector<Note>&, Dialog&)>;
    Dialog(PianoRoll& pr, juce::String title, std::vector<pulp::ParamInfo> infos, Apply apply)
        : pr_(pr), title_(std::move(title)), apply_(std::move(apply)) {
        std::vector<pulp::Param*> raw;
        for (auto& i : infos) { params_.push_back(std::make_unique<pulp::Param>(i)); raw.push_back(params_.back().get()); }
        panel_.setSections({{juce::String(), raw}});
        panel_.onChange = [this](pulp::Param*) { preview(); };
        addAndMakeVisible(panel_);
        ok_.text = "ACCEPT";
        ok_.setTooltip("Keep the result (Enter)");
        ok_.litColour = theme::good;
        ok_.lit = true;
        ok_.onClick = [this] { pr_.runTool(DialogAccept); };
        cancel_.text = "CANCEL";
        cancel_.setTooltip("Put the notes back as they were (Esc)");
        cancel_.onClick = [this] { pr_.runTool(DialogCancel); };
        addAndMakeVisible(ok_);
        addAndMakeVisible(cancel_);
        if (auto* ns = pr_.notes()) original_ = *ns;
        originalEvents_ = pr_.s_.project.pattern().events;
        const int w = 330;
        setSize(w, 26 + panel_.heightFor(w - 8) + 34);
        preview();
    }
    float f(const char* id) const { for (auto& p : params_) if (p->info.id == id) return p->get(); return 0.0f; }
    int i(const char* id) const { return (int)std::lround(f(id)); }
    bool b(const char* id) const { return f(id) >= 0.5f; }
    int grid(const char* id) const { const int g = kGridTicks[juce::jlimit(0, 7, i(id))]; return g == 0 ? pr_.snap() : g; }
    PianoRoll& roll() { return pr_; }
    const juce::String& title() const { return title_; }
    bool set(const std::string& id, float value) {
        for (auto& p : params_) if (p->info.id == id) { p->set(value); panel_.repaint(); preview(); return true; }
        return false;
    }

    void preview() {
        auto* ns = pr_.notes();
        if (!ns) return;
        *ns = original_;
        pr_.s_.project.pattern().events = originalEvents_;      // a tool may write events (the LFO does)
        apply_(*ns, *this);
        pulp::sortNotes(*ns);
        pr_.s_.publish();
        pr_.repaint();
    }
    void cancel() {
        pr_.s_.project.pattern().events = originalEvents_;
        if (auto* ns = pr_.notes()) { *ns = original_; pr_.s_.publish(); }
    }
    void resized() override {
        auto r = getLocalBounds().reduced(4);
        r.removeFromTop(22);
        auto buttons = r.removeFromBottom(28).reduced(0, 2);
        ok_.setBounds(buttons.removeFromRight(90));
        buttons.removeFromRight(6);
        cancel_.setBounds(buttons.removeFromRight(90));
        panel_.setBounds(r);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(theme::panel);
        g.setColour(theme::accent);
        g.drawRect(getLocalBounds(), 1);
        g.setColour(theme::panelHi);
        g.fillRect(1, 1, getWidth() - 2, 22);
        g.setColour(theme::text);
        g.setFont(theme::font(13.0f, true));
        g.drawText(title_, 8, 1, getWidth() - 16, 22, juce::Justification::centredLeft, false);
    }

private:
    PianoRoll& pr_;
    juce::String title_;
    Apply apply_;
    std::vector<std::unique_ptr<pulp::Param>> params_;
    ParamPanel panel_;
    IconButton ok_{IconButton::Text}, cancel_{IconButton::Text};
    std::vector<Note> original_;
    std::map<std::string, std::vector<pulp::AutoEvent>> originalEvents_;
};

bool PianoRoll::setToolParam(const juce::String& id, float value) { return dialog_ != nullptr && dialog_->set(id.toStdString(), value); }

PianoRoll::PianoRoll(Session& s) : s_(s) {
    setOpaque(true);
    setWantsKeyboardFocus(true);
    addAndMakeVisible(hbar_);
    addAndMakeVisible(vbar_);
    hbar_.setAutoHide(false);
    vbar_.setAutoHide(false);
    scrollY_ = (127 - 78) * keyH_;      // open around C5 to C6
    s_.addListener(this);
}

PianoRoll::~PianoRoll() { s_.removeListener(this); }

void PianoRoll::projectChanged() {
    auto* ch = s_.current();
    const int id = ch ? ch->id : -1;
    const int pat = s_.project.pattern().id;
    if (id != shownChannelId_ || pat != shownPatternId_) {
        // a tool panel belongs to the notes it was opened on
        if (dialog_ && shownChannelId_ >= 0) dialog_.reset();
        shownChannelId_ = id;
        shownPatternId_ = pat;
    }
    updateScrollbars();
    repaint();
}

void PianoRoll::toolsMenu(juce::Component* target) {
    juce::PopupMenu m;
    m.addSectionHeader("Piano roll tools");
    m.addItem(QuickQuantize, "Quick quantize   (Ctrl+Q)");
    m.addItem(Quantize, "Quantize...   (Alt+Q)");
    m.addItem(Legato, "Quick legato   (Ctrl+L)");
    m.addItem(Articulate, "Articulate...   (Alt+L)");
    m.addItem(Chop, "Chop...   (Alt+U)");
    m.addItem(Glue, "Glue   (Ctrl+G)");
    m.addSeparator();
    m.addItem(Arpeggiate, "Arpeggiate...   (Alt+A)");
    m.addItem(Strum, "Strum...   (Alt+S)");
    m.addItem(Flam, "Flam...   (Alt+F)");
    m.addItem(Randomize, "Randomize...   (Alt+R)");
    m.addItem(Levels, "Scale levels...   (Alt+X)");
    m.addItem(Lfo, "LFO...   (Alt+O)");
    m.addSeparator();
    m.addItem(Flip, "Flip vertically   (Alt+Y)");
    m.addItem(Reverse, "Reverse in time");
    m.addItem(Trimmer, "Trimmer...   (Alt+W)");
    m.addItem(Riff, "Riff generator...   (Alt+E)");
    m.addSeparator();
    m.addItem(ImportMidi, "Import MIDI file...   (Ctrl+M)");
    m.addItem(ExportMidi, "Export MIDI file...   (Shift+Ctrl+M)");
    m.addItem(Fit, "Zoom to fit   (Ctrl+0)");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target).withTargetScreenArea(localAreaToGlobal(topButtons_[numTopButtons_ - 1])),
                    [safe = juce::Component::SafePointer<PianoRoll>(this)](int r) { if (safe && r > 0) safe->runTool(r); });
}

void PianoRoll::runTool(int id) {
    auto* ns = notes();
    if (!ns) return;
    if (id == DialogAccept || id == DialogCancel) {
        if (!dialog_) return;
        const auto title = dialog_->title();
        if (id == DialogCancel) dialog_->cancel();
        dialog_.reset();
        if (id == DialogAccept) edited(title);
        repaint();
        grabKeyboardFocus();
        return;
    }
    if (dialog_) { dialog_->cancel(); dialog_.reset(); }

    auto open = [&](const char* title, std::vector<pulp::ParamInfo> infos, Dialog::Apply apply) {
        dialog_ = std::make_unique<Dialog>(*this, title, std::move(infos), std::move(apply));
        addAndMakeVisible(*dialog_);
        dialog_->setTopLeftPosition(getWidth() - dialog_->getWidth() - kBarH - 6, kTopH + kRulerH + 6);
    };

    switch (id) {
        case QuickQuantize: { tools::QuantizeOpts q; q.grid = snap(); tools::quantize(*ns, q); edited("Quick quantize"); break; }
        case Legato: tools::legato(*ns); edited("Quick legato"); break;
        case Glue: tools::glue(*ns); edited("Glue"); break;
        case Flip: tools::flipVertical(*ns); edited("Flip"); break;
        case Reverse: tools::reverseTime(*ns); edited("Reverse"); break;
        case Fit: zoomToFit(); break;
        case Quantize:
            open("Quantize", {pc("grid", "Grid", kGridNames, 0), pf("strength", "Strength", 0, 1, 1, "%"), pb("starts", "Start times", true),
                              pb("ends", "End times", false), pb("keep", "Keep ends", false)},
                 [](std::vector<Note>& n, Dialog& d) {
                     tools::QuantizeOpts q;
                     q.grid = d.grid("grid"); q.strength = d.f("strength"); q.starts = d.b("starts"); q.ends = d.b("ends"); q.keepEnds = d.b("keep");
                     tools::quantize(n, q);
                 });
            break;
        case Chop:
            open("Chop", {pc("grid", "Piece length", kGridNames, 0)}, [](std::vector<Note>& n, Dialog& d) { tools::chop(n, d.grid("grid")); });
            break;
        case Arpeggiate:
            open("Arpeggiate", {pc("grid", "Step", kGridNames, 0), pc("pattern", "Pattern", {"Up", "Down", "Up-down", "Random"}, 0),
                                pi("oct", "Octaves", 1, 4, 1), pf("gate", "Gate", 0.1f, 1, 1, "%"), pi("seed", "Seed", 1, 99, 1)},
                 [](std::vector<Note>& n, Dialog& d) { tools::arpeggiate(n, d.grid("grid"), d.i("pattern"), d.i("oct"), d.f("gate"), (pulp::uint32)d.i("seed")); });
            break;
        case Strum:
            open("Strum", {pi("time", "Time per note", 0, 240, 30, "ticks"), pc("dir", "Direction", {"Low to high", "High to low"}, 0), pf("taper", "Velocity taper", 0, 1, 0.2f, "%")},
                 [](std::vector<Note>& n, Dialog& d) { tools::strum(n, d.i("time"), d.i("dir") == 1, d.f("taper")); });
            break;
        case Flam:
            open("Flam", {pi("time", "Time before", 10, 240, 60, "ticks"), pf("vel", "Velocity", 0.1f, 1, 0.6f, "%")},
                 [](std::vector<Note>& n, Dialog& d) { tools::flam(n, d.i("time"), d.f("vel")); });
            break;
        case Articulate:
            open("Articulate", {pc("mode", "Mode", {"Legato with a gap", "Multiply lengths"}, 0), pi("gap", "Gap", 0, 240, 0, "ticks"), pf("mult", "Multiply", 0.1f, 2, 0.5f, "")},
                 [](std::vector<Note>& n, Dialog& d) { tools::articulate(n, d.i("mode") == 0 ? 0.0f : d.f("mult"), d.i("gap")); });
            break;
        case Randomize:
            open("Randomize", {pf("vel", "Velocity", 0, 1, 0.3f, "%"), pf("pan", "Pan", 0, 1, 0, "%"), pf("len", "Length", 0, 1, 0, "%"),
                               pi("pitch", "Pitch range", 0, 24, 0, "st"), pb("scale", "Stay in scale", true), pi("seed", "Seed", 1, 99, 1)},
                 [](std::vector<Note>& n, Dialog& d) {
                     tools::RandomizeOpts o;
                     o.velocity = d.f("vel"); o.pan = d.f("pan"); o.length = d.f("len"); o.pitchRange = d.i("pitch"); o.seed = (pulp::uint32)d.i("seed");
                     if (d.b("scale") && d.roll().scaleRoot >= 0) { o.scaleRoot = d.roll().scaleRoot; o.scaleIndex = d.roll().scaleIndex; }
                     tools::randomize(n, o);
                 });
            break;
        case Levels:
            open("Scale levels", {pc("prop", "Property", {"Velocity", "Pan", "Release", "Mod X", "Mod Y", "Fine pitch"}, laneProp_),
                                  pf("mult", "Multiply", 0, 2, 1, ""), pf("offset", "Offset", -1, 1, 0, "")},
                 [](std::vector<Note>& n, Dialog& d) { tools::scaleLevels(n, d.i("prop"), d.f("mult"), d.f("offset")); });
            break;
        case Lfo:
            // the first six write a property of each note; the last three write events for the channel's own
            // control over the whole pattern (they replace that control's events in this pattern)
            open("LFO", {pc("prop", "Property", {"Velocity", "Pan", "Release", "Mod X", "Mod Y", "Fine pitch",
                                                 "Channel volume (events)", "Channel panning (events)", "Channel pitch (events)"}, laneProp_),
                         pc("shape", "Shape", {"Sine", "Triangle", "Square", "Saw up", "Saw down"}, 0), pi("period", "Period", 1, 64, 16, "steps"),
                         pf("lo", "Low", 0, 1, 0.2f, "%"), pf("hi", "High", 0, 1, 0.9f, "%"), pf("phase", "Phase", 0, 1, 0, "%")},
                 [this](std::vector<Note>& n, Dialog& d) {
                     const int prop = d.i("prop");
                     if (prop < 6) { tools::lfo(n, prop, d.i("shape"), d.i("period") * kTicksPerStep, d.f("lo"), d.f("hi"), d.f("phase")); return; }
                     auto* ch = s_.current();
                     if (!ch) return;
                     const char* ids[3] = {"volume", "pan", "pitch"};
                     auto& pat = s_.project.pattern();
                     pat.events["ch:" + std::to_string(ch->id) + ":" + ids[prop - 6]] =
                         tools::lfoEvents(s_.project.patternLengthTicks(pat), d.i("shape"), d.i("period") * kTicksPerStep, d.f("lo"), d.f("hi"), d.f("phase"));
                 });
            break;
        case Trimmer:
            open("Trimmer", {pi("keep", "Keep", 1, 32, 12, "steps"), pi("drop", "Remove", 1, 16, 4, "steps")},
                 [](std::vector<Note>& n, Dialog& d) { tools::trimPattern(n, d.i("keep") * kTicksPerStep, d.i("drop") * kTicksPerStep); });
            break;
        case Riff:
            open("Riff generator", {pc("kind", "Kind", {"Chord progression", "Arpeggio", "Bass groove"}, 0),
                                    pi("root", "Root key", 36, 84, scaleRoot >= 0 ? 60 + scaleRoot : 60), pc("scale", "Scale", names(tools::scales()), scaleRoot >= 0 ? scaleIndex : 1),
                                    pi("bars", "Bars", 1, 16, 4), pf("density", "Density", 0, 1, 0.5f, "%"), pb("sevenths", "Sevenths", false),
                                    pi("seed", "Seed", 1, 999, 1), pb("replace", "Replace notes", true)},
                 [](std::vector<Note>& n, Dialog& d) {
                     tools::RiffOpts o;
                     o.kind = d.i("kind"); o.root = d.i("root"); o.scaleIndex = d.i("scale"); o.bars = d.i("bars"); o.density = d.f("density");
                     o.sevenths = d.b("sevenths"); o.seed = (pulp::uint32)d.i("seed");
                     o.ticksPerBar = d.roll().s_.project.ticksPerBar();
                     auto made = tools::riff(o);
                     if (d.b("replace")) n.clear();
                     n.insert(n.end(), made.begin(), made.end());
                 });
            break;
        case ImportMidi: {
            auto chooser = std::make_shared<juce::FileChooser>("Import a MIDI file into this channel", juce::File(), "*.mid;*.midi");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                 [safe = juce::Component::SafePointer<PianoRoll>(this), chooser](const juce::FileChooser& fc) {
                                     if (safe && fc.getResult().existsAsFile()) safe->importMidi(fc.getResult());
                                 });
            break;
        }
        case ExportMidi: {
            auto chooser = std::make_shared<juce::FileChooser>("Export this channel's notes as a MIDI file", juce::File(), "*.mid");
            chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                                 [safe = juce::Component::SafePointer<PianoRoll>(this), chooser](const juce::FileChooser& fc) {
                                     if (safe && fc.getResult() != juce::File()) safe->exportMidi(fc.getResult().withFileExtension("mid"));
                                 });
            break;
        }
        default: break;
    }
    repaint();
}

void PianoRoll::importMidi(const juce::File& f) {
    pulp::MidiSong song;
    std::string err;
    if (!pulp::readMidiFile(f.getFullPathName().toStdString(), song, &err)) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Import MIDI", "Could not read " + f.getFileName() + ": " + juce::String(err), "OK", this);
        return;
    }
    auto place = [this](std::vector<Note> in) {
        auto* ns = notes();
        if (!ns) return;
        for (auto& n : in) n.selected = false;
        *ns = std::move(in);
        edited("Import MIDI");
        zoomToFit();
    };
    if (song.tracks.size() == 1) { place(song.tracks[0].notes); return; }
    juce::PopupMenu m;
    m.addSectionHeader(f.getFileName() + ": which track?");
    for (int i = 0; i < (int)song.tracks.size(); ++i)
        m.addItem(i + 1, juce::String(i + 1) + ".  " + (song.tracks[(size_t)i].name.empty() ? juce::String("Track") : juce::String(song.tracks[(size_t)i].name))
                             + "   (" + juce::String((int)song.tracks[(size_t)i].notes.size()) + " notes)");
    m.addSeparator();
    m.addItem(1000, "All tracks merged");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [song, place](int r) {
        if (r <= 0) return;
        if (r == 1000) {
            std::vector<Note> all;
            for (auto& t : song.tracks) all.insert(all.end(), t.notes.begin(), t.notes.end());
            place(std::move(all));
        } else place(song.tracks[(size_t)(r - 1)].notes);
    });
}

void PianoRoll::exportMidi(const juce::File& f) {
    auto* ns = notes();
    auto* ch = s_.current();
    if (!ns || !ch) return;
    pulp::MidiSong song;
    song.tempo = s_.project.global->tempo.get();
    song.timeSigNum = s_.project.timeSigNum;
    song.timeSigDen = s_.project.timeSigDen;
    pulp::MidiTrack t;
    t.name = ch->name;
    t.notes = *ns;
    song.tracks.push_back(std::move(t));
    if (!pulp::writeMidiFile(f.getFullPathName().toStdString(), song))
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export MIDI", "Could not write " + f.getFullPathName(), "OK", this);
}
