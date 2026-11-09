#pragma once
// The program-wide keyboard shortcuts: a table of commands, each with a key that can be changed
// (Settings > SHORTCUTS). The keys are kept in shortcuts.xml beside the other settings.
#include "Session.h"
#include "Widgets.h"

class Shortcuts {
public:
    struct Command {
        juce::String id, name, group;
        juce::KeyPress standard;            // the key it has until someone changes it
        juce::KeyPress key;                 // the key it has now (invalid: none)
        std::function<void()> run;
    };
    static Shortcuts& get();
    void clear() { commands_.clear(); }
    void add(const juce::String& id, const juce::String& group, const juce::String& name, juce::KeyPress standard, std::function<void()> run);
    void load();                            // after every command has been added
    void save() const;
    bool handle(const juce::KeyPress& k);   // runs the command that has this key
    std::vector<Command>& commands() { return commands_; }
    // Gives a command a key. A command that had that key loses it; its name is returned ("" if none did).
    juce::String assign(int index, juce::KeyPress key);
    void resetAll();
    static juce::String describe(const juce::KeyPress& k);

private:
    static juce::File file();
    std::vector<Command> commands_;
};

// Settings > SHORTCUTS.
class ShortcutSettings : public juce::Component, private juce::ListBoxModel {
public:
    ShortcutSettings();
    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& k) override;
    void visibilityChanged() override { learning_ = -1; list_.updateContent(); }
    void learn(int row);                    // the next key pressed becomes this command's
    juce::String describe() const;

private:
    int getNumRows() override { return (int)Shortcuts::get().commands().size(); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent& e) override;
    juce::ListBox list_;
    IconButton resetBtn_{IconButton::Text};
    int learning_ = -1;
    juce::String status_ = "Click a command, then press the key it should have. Right-click a command to take its key away.";
};
