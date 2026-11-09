// PULP: program entry point, main window and command line.
//   PULP.exe [project.pulp]
//   --project <file>   open a project
//   --script <file>    run a script of interface actions (docs/SCRIPTING.md)
//   --log <file>       where script output goes
//   --no-audio         do not open a sound card (silent, for automated tests)
//   --size <w>x<h>     window size
//   --data-dir <dir>   keep settings, autosaves and the crash log here (tests)
//   --layout           a scripted run keeps and restores window places like a normal run (tests)
//   --scan-vst3 <plugin file> <result file>   look inside one plugin and exit (run by the plugin scanner)
#include "MainComponent.h"
#include "Script.h"
#include "PluginHost.h"
#if JUCE_WINDOWS
 #include <windows.h>
#endif

class MainWindow : public juce::DocumentWindow {
public:
    MainWindow(Session& s, juce::Rectangle<int> size)
        : juce::DocumentWindow("PULP", theme::bg, juce::DocumentWindow::allButtons) {
        setUsingNativeTitleBar(true);
        main = new MainComponent(s);
        main->setSize(size.getWidth(), size.getHeight());
        main->onTitle = [this](const juce::String& t) { setName(t); };
        setContentOwned(main, true);
        setResizable(true, false);
        setResizeLimits(1340, 600, 10000, 10000);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
        main->grabKeyboardFocus();
    }
    // closing asks about unsaved work first
    void closeButtonPressed() override { main->ifSafeToDiscard([] { juce::JUCEApplication::getInstance()->quit(); }); }
    MainComponent* main = nullptr;
};

static void writeCrashLog(void*) {
    const auto log = Session::dataDir().getChildFile("crash.log");
    log.getParentDirectory().createDirectory();
    log.appendText("PULP " + juce::String(JUCE_APPLICATION_VERSION_STRING) + " crashed at " + juce::Time::getCurrentTime().toString(true, true) + "\n"
                   + juce::SystemStats::getStackBacktrace() + "\n");
}

class PulpApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "PULP"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override {
        const auto args = getCommandLineParameterArray();
        auto value = [&](const char* name) -> juce::String {
            const int i = args.indexOf(name);
            return i >= 0 && i + 1 < args.size() ? args[i + 1].unquoted() : juce::String();
        };
        // the plugin scanner's child process: examine one file, write what it holds, and go.
        // If the plugin crashes, only this process dies, quietly.
        if (const int i = args.indexOf("--scan-vst3"); i >= 0 && i + 2 < args.size()) {
           #if JUCE_WINDOWS
            SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
           #endif
            setApplicationReturnValue(PluginHost::runScanChild(args[i + 1].unquoted(), juce::File(args[i + 2].unquoted())));
            quit();
            return;
        }
        const auto cwd = juce::File::getCurrentWorkingDirectory();
        const bool noAudio = args.contains("--no-audio");
        juce::Rectangle<int> size(1500, 860);
        if (auto sz = value("--size"); sz.isNotEmpty())
            size.setSize(juce::jmax(1340, sz.upToFirstOccurrenceOf("x", false, true).getIntValue()),
                         juce::jmax(560, sz.fromFirstOccurrenceOf("x", false, true).getIntValue()));

        const auto script = value("--script");
        const auto log = value("--log");
        // scripted runs never touch the user's own settings or backups unless given their own folder
        Session::persistSettings = script.isEmpty();
        MainComponent::keepLayout = script.isEmpty() || args.contains("--layout");
        if (auto dd = value("--data-dir"); dd.isNotEmpty()) Session::dataDirOverride = cwd.getChildFile(dd);
        if (Session::persistSettings || Session::dataDirOverride != juce::File()) juce::SystemStats::setApplicationCrashHandler(writeCrashLog);

        juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);
        PluginHost::get().start();
        session = std::make_unique<Session>();
        session->newDefaultProject();
        session->beginRunLock();
        session->start(noAudio);
        window = std::make_unique<MainWindow>(*session, size);

        // a project named on the command line (double-clicking a .pulp file arrives this way)
        juce::String toOpen = value("--project");
        if (toOpen.isEmpty() && args.size() > 0 && !args[0].startsWith("--") && (args[0].unquoted().endsWithIgnoreCase(".pulp") || args[0].unquoted().endsWithIgnoreCase(".pulpz")))
            toOpen = args[0].unquoted();
        if (toOpen.isNotEmpty()) session->openProject(cwd.getChildFile(toOpen));
        session->notify();

        if (script.isNotEmpty()) {
            runner = std::make_unique<ScriptRunner>(*window->main, log.isNotEmpty() ? cwd.getChildFile(log) : juce::File());
            if (runner->load(cwd.getChildFile(script))) runner->start();
            else quit();
        } else { window->main->checkCrashRecovery(); window->main->checkFirstRun(); }
    }

    void shutdown() override {
        if (session) { session->saveDeviceSettings(); session->shutdown(); session->endRunLock(); }
        runner.reset();
        PluginHost::get().closeWindows();
        window.reset();
        session.reset();
        PluginHost::get().shutdown();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }

    void systemRequestedQuit() override {
        if (window && !runner) window->closeButtonPressed();
        else quit();
    }

private:
    PulpLookAndFeel lookAndFeel;
    std::unique_ptr<Session> session;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<ScriptRunner> runner;
};

START_JUCE_APPLICATION(PulpApp)
