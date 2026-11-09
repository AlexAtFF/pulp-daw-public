#pragma once
// The Browser: factory library, the current project's sounds, folders the user has added,
// favourites and the plugin database. Click to preview, drag onto the rack.
#include "ChannelEditor.h"
#include "Session.h"

class Browser : public juce::Component,
                private Session::Listener,
                private juce::TextEditor::Listener {
public:
    explicit Browser(Session& s);
    ~Browser() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void refresh();
    bool openPath(const juce::StringArray& labels);        // opens the items so named, one inside the other (scripts)

    // Used by the tree items.
    Session& session() { return s_; }
    void preview(const juce::File& f);
    void openFile(const juce::File& f);
    void addFolder();
    void removeFolder(const juce::File& f);
    bool isFavourite(const juce::File& f) const { return favourites_.contains(f.getFullPathName()); }
    void toggleFavourite(const juce::File& f);
    const juce::StringArray& folders() const { return folders_; }
    const juce::StringArray& favourites() const { return favourites_; }
    juce::String searchText() const { return search_.getText().trim(); }

private:
    void projectChanged() override;
    void textEditorTextChanged(juce::TextEditor&) override;
    void loadSettings();
    void saveSettings();
    Session& s_;
    juce::TextEditor search_;
    juce::TreeView tree_;
    std::unique_ptr<juce::TreeViewItem> root_;
    WaveformView wave_;
    juce::String info_;
    IconButton sync_{IconButton::Text};
    bool syncPreview_ = false;      // previews of loops are stretched to the project tempo
    juce::StringArray folders_, favourites_;
    juce::String projectSignature_;
public:
    void jumpToHistory(int index) { s_.jumpToHistory(index); }
};
