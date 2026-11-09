#include "PluginSettings.h"

PluginSettings::PluginSettings(Session& s) : s_(s) {
    findBtn_.text = "FIND PLUGINS";
    findBtn_.setTooltip("Look for VST3 plugins that are not in the list yet. Each one is examined by a separate process, so a plugin that crashes cannot take PULP down.");
    rescanBtn_.text = "RESCAN ALL";
    rescanBtn_.setTooltip("Examine every VST3 plugin again, including the ones that failed before");
    folderBtn_.text = "ADD FOLDER...";
    folderBtn_.setTooltip("Also look for VST3 plugins in a folder of your choice (the standard folder, Common Files\\VST3, is always searched)");
    clearBtn_.text = "FORGET FAILED";
    clearBtn_.setTooltip("Clear the list of plugins that failed, so FIND PLUGINS tries them again");
    findBtn_.onClick = [this] { scan(false); };
    rescanBtn_.onClick = [this] { scan(true); };
    clearBtn_.onClick = [this] { PluginHost::get().clearBlacklist(); listChanged(); };
    folderBtn_.onClick = [this] {
        auto chooser = std::make_shared<juce::FileChooser>("A folder that holds VST3 plugins", juce::File(), juce::String());
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                             [safe = juce::Component::SafePointer<PluginSettings>(this), chooser](const juce::FileChooser& fc) {
            const auto dir = fc.getResult();
            if (safe == nullptr || !dir.isDirectory()) return;
            PluginHost::get().folders.addIfNotAlreadyThere(dir.getFullPathName());
            PluginHost::get().save();
            safe->scan(false);
        });
    };
    for (auto* b : {&findBtn_, &rescanBtn_, &folderBtn_, &clearBtn_}) addAndMakeVisible(*b);
    list_.setModel(this);
    list_.setRowHeight(22);
    list_.setColour(juce::ListBox::backgroundColourId, theme::inset);
    addAndMakeVisible(list_);
    listChanged();
}

void PluginSettings::listChanged() {
    entries_ = PluginHost::get().plugins();
    failed_ = PluginHost::get().blacklist();
    if (!PluginHost::get().scanning()) {
        int inst = 0;
        for (auto& e : entries_) inst += e.instrument ? 1 : 0;
        status_ = entries_.empty() && !PluginHost::get().everScanned()
                    ? juce::String("No scan has been run yet. FIND PLUGINS looks in the standard VST3 folder.")
                    : juce::String((int)entries_.size() - inst) + " effects and " + juce::String(inst) + " instruments"
                      + (failed_.isEmpty() ? juce::String() : ", " + juce::String(failed_.size()) + " failed (shown in red; they are left alone)");
    }
    list_.updateContent();
    list_.repaint();
    repaint();
}

void PluginSettings::scan(bool everything, const juce::StringArray& files, std::function<void(int, int)> done) {
    auto& host = PluginHost::get();
    if (host.scanning()) return;
    status_ = "Looking for plugins...";
    repaint();
    const auto safe = juce::Component::SafePointer<PluginSettings>(this);
    host.scan(files.isEmpty() ? host.defaultFiles() : files, everything,
              [safe](const juce::String& text) { if (safe != nullptr) { safe->status_ = text; safe->repaint(); } },
              [safe, done](int found, int failed) {
                  if (safe != nullptr) safe->listChanged();
                  if (done) done(found, failed);
              });
}

void PluginSettings::paint(juce::Graphics& g) {
    g.fillAll(theme::panel);
    auto r = getLocalBounds().reduced(10);
    g.setColour(theme::accent2);
    g.setFont(theme::font(11.0f, true));
    g.drawText("VST3 PLUGINS", r.removeFromTop(18), juce::Justification::centredLeft, false);
    auto foot = getLocalBounds().reduced(10).removeFromBottom(58);
    foot.removeFromBottom(30);
    g.setColour(theme::textDim);
    g.setFont(theme::font(12.0f));
    g.drawFittedText(status_, foot, juce::Justification::centredLeft, 2);
}

void PluginSettings::resized() {
    auto r = getLocalBounds().reduced(10);
    r.removeFromTop(20);
    auto buttons = r.removeFromBottom(26);
    r.removeFromBottom(32);
    list_.setBounds(r);
    findBtn_.setBounds(buttons.removeFromLeft(108)); buttons.removeFromLeft(4);
    rescanBtn_.setBounds(buttons.removeFromLeft(96)); buttons.removeFromLeft(4);
    folderBtn_.setBounds(buttons.removeFromLeft(108)); buttons.removeFromLeft(4);
    clearBtn_.setBounds(buttons.removeFromLeft(118));
}

void PluginSettings::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) {
    if (selected) g.fillAll(theme::panelHi);
    g.setFont(theme::font(12.5f));
    const juce::Rectangle<int> line(6, 0, width - 12, height);
    if (row < (int)entries_.size()) {
        const auto& e = entries_[(size_t)row];
        g.setColour(theme::text);
        g.drawText(e.name, line.withWidth(line.getWidth() * 5 / 10), juce::Justification::centredLeft, true);
        g.setColour(theme::textDim);
        g.drawText(e.maker, line.withTrimmedLeft(line.getWidth() * 5 / 10).withTrimmedRight(84), juce::Justification::centredLeft, true);
        g.setColour(e.instrument ? theme::good : theme::accent2);
        g.drawText(e.instrument ? "instrument" : "effect", line.withLeft(line.getRight() - 80), juce::Justification::centredRight, false);
    } else if (row - (int)entries_.size() < failed_.size()) {
        g.setColour(theme::bad);
        g.drawText(juce::File::createFileWithoutCheckingPath(failed_[row - (int)entries_.size()]).getFileName() + "   (crashed, hung or held no plugin when it was examined)",
                   line, juce::Justification::centredLeft, true);
    }
}

void PluginSettings::listBoxItemClicked(int row, const juce::MouseEvent& e) {
    if (!e.mods.isPopupMenu() || row < 0 || row >= (int)entries_.size()) return;
    juce::PopupMenu m;
    m.addSectionHeader(entries_[(size_t)row].name);
    m.addItem(1, "Take off the list");
    m.addItem(2, "Show the file in Explorer");
    const auto entry = entries_[(size_t)row];
    m.showMenuAsync(juce::PopupMenu::Options(), [entry](int r) {
        if (r == 1) PluginHost::get().forget(entry.typeId);
        else if (r == 2) juce::File::createFileWithoutCheckingPath(entry.file).revealToUser();
    });
}
