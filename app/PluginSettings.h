#pragma once
// Settings > Plugins: the VST3 plugins PULP knows about, and the scanner that finds them.
#include "PluginHost.h"
#include "Widgets.h"

class PluginSettings : public juce::Component, private juce::ListBoxModel {
public:
    explicit PluginSettings(Session& s);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void scan(bool everything, const juce::StringArray& files = {}, std::function<void(int, int)> done = nullptr);
    void listChanged();
    juce::String status() const { return status_; }

private:
    int getNumRows() override { return (int)entries_.size() + failed_.size(); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent& e) override;
    Session& s_;
    IconButton findBtn_{IconButton::Text}, rescanBtn_{IconButton::Text}, folderBtn_{IconButton::Text}, clearBtn_{IconButton::Text};
    juce::ListBox list_;
    std::vector<PluginHost::Entry> entries_;
    juce::StringArray failed_;
    juce::String status_;
};
