// The File and Edit menus: new, open, save, versions, packing, backups, export, undo history.
#include "MainComponent.h"
#include "ParamPanel.h"
#include "pulp/Templates.h"

namespace {
pulp::ParamInfo choice(const char* id, const char* name, std::vector<std::string> c, int def) {
    pulp::ParamInfo i; i.id = id; i.name = name; i.min = 0; i.max = (float)(c.size() - 1); i.def = (float)def; i.steps = (int)c.size(); i.choices = std::move(c);
    return i;
}
const double kRates[] = {44100.0, 48000.0, 88200.0, 96000.0};
const int kBits[] = {16, 24, 32};
}

// The export settings window.
class ExportPanel : public juce::Component {
public:
    explicit ExportPanel(MainComponent& m)
        : main_(m),
          mode_(choice("mode", "What to export", {"The whole song", "The selected pattern"}, 0)),
          rate_(choice("rate", "Sample rate", {"44100 Hz", "48000 Hz", "88200 Hz", "96000 Hz"}, 1)),
          bits_(choice("bits", "Bit depth", {"16-bit", "24-bit", "32-bit float"}, 1)),
          tail_(choice("tail", "Tail", {"Leave remainder", "Cut at the end", "Wrap onto the start"}, 0)),
          dither_(choice("dither", "Dither", {"Off", "On"}, 0)),
          format_(choice("format", "File format", formatNames(), 0)),
          stems_(choice("stems", "Split mixer tracks", {"One file", "A file for each mixer track"}, 0)),
          mp3_(choice("mp3", "MP3 bitrate", {"128 kbps", "192 kbps", "256 kbps", "320 kbps"}, 1)) {
        std::vector<pulp::Param*> list{&mode_, &format_, &rate_, &bits_, &tail_, &dither_, &stems_};
        if (Session::exportFormats().contains("mp3")) list.push_back(&mp3_);
        panel_.setSections({{"Export", list}});
        addAndMakeVisible(panel_);
        go_.text = "EXPORT...";
        go_.lit = true;
        go_.litColour = theme::good;
        go_.setTooltip("Choose a file and render it. The render runs in the background, faster than real time.");
        go_.onClick = [this] { start(); };
        cancel_.text = "CANCEL";
        cancel_.setTooltip("Stop the export that is running");
        cancel_.onClick = [this] { main_.session().cancelExport(); };
        addAndMakeVisible(go_);
        addAndMakeVisible(cancel_);
    }
    Session::ExportOptions options() const {
        Session::ExportOptions o;
        o.song = mode_.getInt() == 0;
        o.sampleRate = kRates[rate_.getInt()];
        o.bits = kBits[bits_.getInt()];
        o.tail = tail_.getInt();
        o.dither = dither_.getBool();
        o.stems = stems_.getBool();
        static const int kKbps[] = {128, 192, 256, 320};
        o.mp3Kbps = kKbps[mp3_.getInt()];
        return o;
    }
    void run(const juce::File& file) {
        status_ = "Rendering " + file.getFileName() + "...";
        progress_ = 0.0;
        repaint();
        juce::Component::SafePointer<ExportPanel> safe(this);
        main_.session().exportWav(file, options(),
            [safe](double p) { if (safe) { safe->progress_ = p; safe->repaint(); } },
            [safe, file](juce::String err) {
                if (!safe) return;
                safe->progress_ = err.isEmpty() ? 1.0 : 0.0;
                safe->status_ = err.isEmpty() ? "Saved " + file.getFullPathName() : err;
                safe->repaint();
            });
    }
    void start() {
        auto& s = main_.session();
        const auto startDir = s.projectFile != juce::File() ? s.projectFile.getParentDirectory() : juce::File::getSpecialLocation(juce::File::userMusicDirectory);
        const auto ext = Session::exportFormats()[format_.getInt()];
        auto chooser = std::make_shared<juce::FileChooser>("Export", startDir.getChildFile((s.projectFile != juce::File() ? s.projectFile.getFileNameWithoutExtension() : juce::String("untitled")) + "." + ext), "*." + ext);
        juce::Component::SafePointer<ExportPanel> safe(this);
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe, chooser, ext](const juce::FileChooser& fc) {
            if (safe && fc.getResult() != juce::File()) safe->run(fc.getResult().withFileExtension(ext));
        });
    }
    void resized() override {
        auto r = getLocalBounds().reduced(6);
        auto buttons = r.removeFromBottom(30).reduced(0, 2);
        go_.setBounds(buttons.removeFromRight(110));
        buttons.removeFromRight(6);
        cancel_.setBounds(buttons.removeFromRight(90));
        r.removeFromBottom(44);
        panel_.setBounds(r);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(theme::panel);
        auto r = getLocalBounds().reduced(8);
        r.removeFromBottom(34);
        auto bar = r.removeFromBottom(14);
        theme::insetBox(g, bar.toFloat());
        g.setColour(theme::accent);
        g.fillRect(bar.reduced(2).withWidth((int)((bar.getWidth() - 4) * progress_)));
        g.setColour(theme::text);
        g.setFont(theme::font(12.0f));
        g.drawFittedText(status_, r.removeFromBottom(22), juce::Justification::centredLeft, 1, 0.7f);
    }
private:
    MainComponent& main_;
    static std::vector<std::string> formatNames() {
        std::vector<std::string> out;
        for (auto& f : Session::exportFormats()) out.push_back(f.toUpperCase().toStdString());
        return out;
    }
    pulp::Param mode_, rate_, bits_, tail_, dither_, format_, stems_, mp3_;
    ParamPanel panel_;
    IconButton go_{IconButton::Text}, cancel_{IconButton::Text};
    double progress_ = 0.0;
    juce::String status_ = "Choose the settings, then Export.";
};

juce::Component* MainComponent::makeExportPanel() { return new ExportPanel(*this); }

void MainComponent::exportTo(const juce::File& file, const Session::ExportOptions& opt, std::function<void(juce::String)> done) {
    session_.exportWav(file, opt, {}, std::move(done));
}

// Runs `then` straight away, or after asking what to do with unsaved changes.
void MainComponent::ifSafeToDiscard(std::function<void()> then) {
    if (!session_.dirty || !Session::persistSettings) { then(); return; }
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                     .withTitle("Unsaved changes")
                                     .withMessage("Save the changes to " + session_.projectTitle().upToLastOccurrenceOf(" *", false, false) + " first?")
                                     .withButton("Save").withButton("Don't save").withButton("Cancel")
                                     .withAssociatedComponent(this),
                                 [this, then](int r) {
                                     if (r == 1) fileSave(then);
                                     else if (r == 2) then();
                                 });
}

void MainComponent::fileNew() { ifSafeToDiscard([this] { session_.newProject(); }); }

void MainComponent::fileOpen() {
    ifSafeToDiscard([this] {
        const auto startDir = session_.projectFile != juce::File() ? session_.projectFile.getParentDirectory() : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        auto chooser = std::make_shared<juce::FileChooser>("Open a project", startDir, "*.pulp;*.pulpz");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, chooser](const juce::FileChooser& fc) {
            if (fc.getResult().existsAsFile()) session_.openProject(fc.getResult());
        });
    });
}

void MainComponent::fileSaveAs(std::function<void()> then) {
    const auto startDir = session_.projectFile != juce::File() ? session_.projectFile.getParentDirectory() : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    auto chooser = std::make_shared<juce::FileChooser>("Save the project", startDir.getChildFile(session_.projectFile != juce::File() ? session_.projectFile.getFileName() : juce::String("untitled.pulp")), "*.pulp");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this, chooser, then](const juce::FileChooser& fc) {
        if (fc.getResult() != juce::File() && session_.saveProject(fc.getResult().withFileExtension("pulp")) && then) then();
    });
}

void MainComponent::fileSave(std::function<void()> then) {
    if (session_.projectFile == juce::File()) { fileSaveAs(std::move(then)); return; }
    if (session_.saveProject(session_.projectFile) && then) then();
}

void MainComponent::fileSaveNewVersion() {
    if (session_.projectFile == juce::File()) { fileSaveAs({}); return; }
    session_.saveNewVersion();
}

void MainComponent::filePack() {
    const auto startDir = session_.projectFile != juce::File() ? session_.projectFile.getParentDirectory() : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    auto chooser = std::make_shared<juce::FileChooser>("Pack the project and its audio into one file", startDir.getChildFile(session_.projectTitle().upToLastOccurrenceOf(" *", false, false) + ".pulpz"), "*.pulpz");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this, chooser](const juce::FileChooser& fc) {
        if (fc.getResult() != juce::File()) session_.packProject(fc.getResult().withFileExtension("pulpz"));
    });
}

void MainComponent::fileExport() { window("export")->showAndRaise(); }

void MainComponent::fileMenu(juce::Component* target) {
    juce::PopupMenu m, recent, backups, templates;
    m.addItem(1, "New");
    {
        int id = 300;
        for (auto& t : pulp::projectTemplates()) templates.addItem(id++, juce::String(t.name) + "   -   " + juce::String(t.description));
    }
    m.addSubMenu("New from template", templates);
    m.addItem(12, "Open the demo song");
    m.addItem(2, "Open...   (Ctrl+O)");
    const auto recents = session_.recentFiles();
    for (int i = 0; i < recents.size(); ++i) recent.addItem(100 + i, juce::File(recents[i]).getFileName());
    m.addSubMenu("Open recent", recent, !recents.isEmpty());
    m.addSeparator();
    m.addItem(3, "Save   (Ctrl+S)");
    m.addItem(4, "Save as...   (Shift+Ctrl+S)");
    m.addItem(5, "Save new version   (Ctrl+N)");
    m.addItem(6, "Pack project and audio into one file...");
    m.addSeparator();
    const auto baks = session_.listBackups();
    for (int i = 0; i < juce::jmin(15, baks.size()); ++i)
        backups.addItem(200 + i, baks[i].getFileNameWithoutExtension().replace("__", "   ") + "   (" + baks[i].getLastModificationTime().toString(true, true, false) + ")");
    m.addSubMenu("Recover an autosaved backup", backups, !baks.isEmpty());
    m.addItem(7, "Export audio (" + Session::exportFormats().joinIntoString(", ").toUpperCase() + ")...   (Ctrl+R)");
    m.addItem(10, "Import a MIDI file as channels and a pattern...");
    m.addItem(11, "Export a MIDI file...");
    m.addSeparator();
    m.addItem(13, "Take the tour");
    m.addItem(8, "Open .pulp files with PULP when double-clicked");
    m.addItem(9, "Exit");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target), [this, recents, baks](int r) {
        if (r == 1) fileNew();
        else if (r == 2) fileOpen();
        else if (r == 3) fileSave({});
        else if (r == 4) fileSaveAs({});
        else if (r == 5) fileSaveNewVersion();
        else if (r == 6) filePack();
        else if (r == 7) fileExport();
        else if (r == 13) startTour();
        else if (r == 12) ifSafeToDiscard([this] { session_.newFromTemplate(-1); window("playlist")->showAndRaise(); });
        else if (r >= 300 && r < 340) ifSafeToDiscard([this, r] { session_.newFromTemplate(r - 300); });
        else if (r == 10 || r == 11) {
            const bool in = r == 10;
            auto chooser = std::make_shared<juce::FileChooser>(in ? "Import a MIDI file" : "Export a MIDI file", juce::File(), "*.mid;*.midi");
            chooser->launchAsync(in ? juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                    : juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                                 [this, chooser, in](const juce::FileChooser& fc) {
                if (fc.getResult() == juce::File()) return;
                if (in) { const auto what = session_.importMidiSong(fc.getResult()); if (what.isNotEmpty()) { window("playlist")->showAndRaise(); showMessage(what); } }
                else if (session_.exportMidiSong(fc.getResult().withFileExtension("mid"))) showMessage("Wrote " + fc.getResult().withFileExtension("mid").getFileName());
            });
        }
        else if (r == 8) {
            // registers the file type for this Windows user only
            const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
            const bool ok = juce::WindowsRegistry::registerFileAssociation(".pulp", "PULP.Project", "PULP project", exe, 0, true, juce::WindowsRegistry::WoW64_Default);
            showMessage(ok ? "Done. Double-clicking a .pulp file now opens it in PULP." : "Windows did not accept the change.");
        }
        else if (r == 9) ifSafeToDiscard([] { juce::JUCEApplication::getInstance()->quit(); });
        else if (r >= 200 && r - 200 < baks.size()) { const auto f = baks[r - 200]; ifSafeToDiscard([this, f] { session_.restoreBackup(f); }); }
        else if (r >= 100 && r - 100 < recents.size()) { const juce::File f(recents[r - 100]); ifSafeToDiscard([this, f] { session_.openProject(f); }); }
    });
}

void MainComponent::editMenu(juce::Component* target) {
    juce::PopupMenu m, hist;
    const int pos = session_.historyPos, n = (int)session_.history.size();
    m.addItem(1, "Undo / redo last step   (Ctrl+Z)", n > 1);
    m.addItem(2, "Step back   (Ctrl+Alt+Z)", pos > 0);
    m.addItem(3, "Step forward   (Shift+Ctrl+Z)", pos + 1 < n);
    for (int i = n - 1; i >= juce::jmax(0, n - 30); --i) hist.addItem(100 + i, juce::String(i + 1) + ".  " + session_.history[(size_t)i].name, true, i == pos);
    m.addSubMenu("History", hist, n > 0);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target), [this](int r) {
        if (r == 1) session_.undoToggle();
        else if (r == 2) session_.undoStep();
        else if (r == 3) session_.redoStep();
        else if (r >= 100) session_.jumpToHistory(r - 100);
    });
}

// After a crash, offer the newest autosave.
void MainComponent::checkCrashRecovery() {
    if (!session_.crashedLastTime) return;
    const auto baks = session_.listBackups();
    if (baks.isEmpty()) return;
    const auto newest = baks[0];
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::WarningIcon)
                                     .withTitle("PULP did not close properly last time")
                                     .withMessage("An autosaved backup from " + newest.getLastModificationTime().toString(true, true, false) + " is available: "
                                                  + newest.getFileNameWithoutExtension().upToFirstOccurrenceOf("__", false, false) + ".\n\nRestore it?")
                                     .withButton("Restore").withButton("Not now")
                                     .withAssociatedComponent(this),
                                 [this, newest](int r) { if (r == 1) session_.restoreBackup(newest); });
}
