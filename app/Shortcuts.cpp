#include "Shortcuts.h"

Shortcuts& Shortcuts::get() { static Shortcuts s; return s; }

juce::File Shortcuts::file() { return Session::dataDir().getChildFile("shortcuts.xml"); }

void Shortcuts::add(const juce::String& id, const juce::String& group, const juce::String& name, juce::KeyPress standard, std::function<void()> run) {
    commands_.push_back({id, name, group, standard, standard, std::move(run)});
}

juce::String Shortcuts::describe(const juce::KeyPress& k) {
    if (!k.isValid()) return "(none)";
    auto text = k.getTextDescription();
    // JUCE writes "ctrl + S"; the manual writes "Ctrl+S"
    text = text.replace("ctrl", "Ctrl").replace("shift", "Shift").replace("alt", "Alt").replace(" + ", "+").replace("spacebar", "Space").replace("numpad ", "Numpad ");
    return text;
}

void Shortcuts::load() {
    if (!Session::persistSettings && Session::dataDirOverride == juce::File()) return;
    if (auto xml = juce::parseXML(file()))
        for (auto* e : xml->getChildWithTagNameIterator("KEY"))
            for (auto& c : commands_)
                if (c.id == e->getStringAttribute("command")) {
                    const auto text = e->getStringAttribute("key");
                    c.key = text.isEmpty() ? juce::KeyPress() : juce::KeyPress::createFromDescription(text);
                }
}

void Shortcuts::save() const {
    if (!Session::persistSettings && Session::dataDirOverride == juce::File()) return;
    juce::XmlElement root("SHORTCUTS");
    // only what differs from the standard keys is written, so new versions can change their defaults
    for (auto& c : commands_)
        if (c.key != c.standard) {
            auto* e = root.createNewChildElement("KEY");
            e->setAttribute("command", c.id);
            e->setAttribute("key", c.key.isValid() ? c.key.getTextDescription() : juce::String());
        }
    file().getParentDirectory().createDirectory();
    root.writeTo(file());
}

bool Shortcuts::handle(const juce::KeyPress& k) {
    for (auto& c : commands_)
        if (c.key.isValid() && c.key.getKeyCode() == k.getKeyCode()
            && c.key.getModifiers().withOnlyMouseButtons().getRawFlags() == 0
            && (c.key.getModifiers().getRawFlags() & juce::ModifierKeys::allKeyboardModifiers) == (k.getModifiers().getRawFlags() & juce::ModifierKeys::allKeyboardModifiers)) {
            if (c.run) c.run();
            return true;
        }
    return false;
}

juce::String Shortcuts::assign(int index, juce::KeyPress key) {
    if (index < 0 || index >= (int)commands_.size()) return {};
    // letters are stored in upper case, as the keyboard reports them
    if (key.isValid() && key.getKeyCode() < 256) key = juce::KeyPress(juce::CharacterFunctions::toUpperCase((juce::juce_wchar)key.getKeyCode()), key.getModifiers(), 0);
    juce::String loser;
    if (key.isValid())
        for (int i = 0; i < (int)commands_.size(); ++i)
            if (i != index && commands_[(size_t)i].key == key) { loser = commands_[(size_t)i].name; commands_[(size_t)i].key = juce::KeyPress(); }
    commands_[(size_t)index].key = key;
    save();
    return loser;
}

void Shortcuts::resetAll() {
    for (auto& c : commands_) c.key = c.standard;
    save();
}

// ------------------------------------------------------------------ the settings page

ShortcutSettings::ShortcutSettings() {
    setWantsKeyboardFocus(true);
    list_.setModel(this);
    list_.setRowHeight(22);
    list_.setColour(juce::ListBox::backgroundColourId, theme::inset);
    addAndMakeVisible(list_);
    resetBtn_.text = "STANDARD KEYS";
    resetBtn_.setTooltip("Give every command its standard key back");
    resetBtn_.onClick = [this] { Shortcuts::get().resetAll(); learning_ = -1; status_ = "Every command has its standard key again."; list_.updateContent(); repaint(); list_.repaint(); };
    addAndMakeVisible(resetBtn_);
}

void ShortcutSettings::paint(juce::Graphics& g) {
    g.fillAll(theme::panel);
    auto r = getLocalBounds().reduced(10);
    g.setColour(theme::accent2);
    g.setFont(theme::font(11.0f, true));
    g.drawText("KEYBOARD SHORTCUTS (the keys inside the piano roll, the playlist and the channel rack are fixed)", r.removeFromTop(18), juce::Justification::centredLeft, true);
    auto foot = getLocalBounds().reduced(10).removeFromBottom(58);
    foot.removeFromBottom(30);
    g.setColour(learning_ >= 0 ? theme::accent : theme::textDim);
    g.setFont(theme::font(12.0f));
    g.drawFittedText(status_, foot, juce::Justification::centredLeft, 2);
}

void ShortcutSettings::resized() {
    auto r = getLocalBounds().reduced(10);
    r.removeFromTop(20);
    auto buttons = r.removeFromBottom(26);
    r.removeFromBottom(32);
    list_.setBounds(r);
    resetBtn_.setBounds(buttons.removeFromLeft(130));
}

void ShortcutSettings::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool) {
    auto& cmds = Shortcuts::get().commands();
    if (row < 0 || row >= (int)cmds.size()) return;
    const auto& c = cmds[(size_t)row];
    if (row == learning_) g.fillAll(theme::accent.withAlpha(0.25f));
    g.setFont(theme::font(12.5f));
    g.setColour(theme::textDim);
    g.drawText(c.group, 6, 0, 86, height, juce::Justification::centredLeft, true);
    g.setColour(theme::text);
    g.drawText(c.name, 96, 0, width - 96 - 170, height, juce::Justification::centredLeft, true);
    g.setColour(row == learning_ ? theme::accent : (c.key == c.standard ? theme::textDim : theme::good));
    g.setFont(theme::mono(12.5f));
    g.drawText(row == learning_ ? juce::String("press a key...") : Shortcuts::describe(c.key), width - 170, 0, 164, height, juce::Justification::centredRight, true);
}

void ShortcutSettings::listBoxItemClicked(int row, const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) {
        Shortcuts::get().assign(row, juce::KeyPress());
        learning_ = -1;
        status_ = Shortcuts::get().commands()[(size_t)row].name + " has no key now.";
        list_.repaint();
        repaint();
        return;
    }
    learn(row);
}

void ShortcutSettings::learn(int row) {
    learning_ = row;
    status_ = "Press the key for \"" + Shortcuts::get().commands()[(size_t)row].name + "\" (Escape to leave it as it is).";
    grabKeyboardFocus();
    list_.repaint();
    repaint();
}

bool ShortcutSettings::keyPressed(const juce::KeyPress& k) {
    if (learning_ < 0) return false;
    auto& cmds = Shortcuts::get().commands();
    if (k.getKeyCode() == juce::KeyPress::escapeKey && !k.getModifiers().isAnyModifierKeyDown()) status_ = "Left as it was.";
    else {
        const auto key = juce::KeyPress(k.getKeyCode(), juce::ModifierKeys(k.getModifiers().getRawFlags() & juce::ModifierKeys::allKeyboardModifiers), 0);
        const auto loser = Shortcuts::get().assign(learning_, key);
        status_ = cmds[(size_t)learning_].name + " is now " + Shortcuts::describe(cmds[(size_t)learning_].key) + (loser.isEmpty() ? juce::String(".") : ". " + loser + " had that key and now has none.");
    }
    learning_ = -1;
    list_.repaint();
    repaint();
    return true;
}

juce::String ShortcutSettings::describe() const {
    juce::String out = "shortcuts:";
    for (auto& c : Shortcuts::get().commands()) out << " [" << c.id << "=" << Shortcuts::describe(c.key) << "]";
    return out + " status: " + status_;
}
