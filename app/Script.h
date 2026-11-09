#pragma once
// Runs a text script of interface actions, so the program can be checked without a
// person clicking: one command per line, see docs/SCRIPTING.md.
#include "MainComponent.h"

class ScriptRunner : private juce::Timer {
public:
    ScriptRunner(MainComponent& main, juce::File logFile);
    bool load(const juce::File& script);
    void start() { startTimer(50); }
    void log(const juce::String& line);
    static bool saveSnapshot(juce::Component& c, const juce::File& file, float scale = 1.0f);

private:
    void timerCallback() override;
    bool run(const juce::String& line);     // false = stop running (quit)
    MainComponent& main_;
    juce::File log_;
    juce::StringArray lines_;
    int next_ = 0;
    int errors_ = 0;
    juce::File baseDir_;
    bool exportBusy_ = false;
    bool scriptBusy_ = false;       // an asynchronous command (calibration) has not answered yet
};
