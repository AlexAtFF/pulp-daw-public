#pragma once
// A floating, movable, resizable window inside the workspace. It can be detached onto the
// desktop (for a second screen) and docked back.
#include "Theme.h"
#include <functional>

// Styles the stock JUCE pieces PULP still uses (popup menus, text boxes, dialogs, scrollbars).
class PulpLookAndFeel : public juce::LookAndFeel_V4 {
public:
    PulpLookAndFeel();
    juce::Font getPopupMenuFont() override { return theme::font(14.0f); }
    juce::Font getAlertWindowMessageFont() override { return theme::font(14.0f); }
    juce::Font getAlertWindowTitleFont() override { return theme::font(16.0f, true); }
    juce::Font getTextButtonFont(juce::TextButton&, int) override { return theme::font(13.0f); }
    juce::Font getComboBoxFont(juce::ComboBox&) override { return theme::font(13.0f); }
    juce::Font getLabelFont(juce::Label&) override { return theme::font(13.0f); }
    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int w, int h, bool vertical, int thumbStart, int thumbSize, bool over, bool down) override;
};

class ToolWindow : public juce::Component {
public:
    ToolWindow(const juce::String& title, juce::Component* content, bool ownsContent = true);
    ~ToolWindow() override;

    std::function<bool(const juce::KeyPress&)> onKey;      // global shortcuts
    std::function<bool(bool)> onKeyState;                   // key up / down changes (typing keyboard)
    bool keyStateChanged(bool isDown) override { return onKeyState ? onKeyState(isDown) : false; }
    std::function<void()> onVisibilityChanged;
    juce::Component* content() const { return content_; }
    const juce::String& title() const { return title_; }
    void setTitle(const juce::String& t) { title_ = t; repaint(); }
    void setMinimumSize(int w, int h) { limits_.setMinimumSize(w, h); }
    bool isDetached() const { return isOnDesktop(); }
    void setDetached(bool detached);
    void showAndRaise();
    void toggle();          // F-key behaviour: bring to front, or hide when already in front

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent&) override { hover_ = 0; repaint(0, 0, getWidth(), kTitle); }
    bool keyPressed(const juce::KeyPress& k) override { return onKey ? onKey(k) : false; }
    void visibilityChanged() override { if (onVisibilityChanged) onVisibilityChanged(); }
    void broughtToFront() override { repaint(); }
    void focusOfChildComponentChanged(FocusChangeType) override { repaint(0, 0, getWidth(), kTitle); }
    static constexpr int kTitle = 22;

private:
    juce::Rectangle<int> closeBox() const { return {getWidth() - 21, 3, 16, 16}; }
    juce::Rectangle<int> detachBox() const { return {getWidth() - 40, 3, 16, 16}; }
    juce::String title_;
    juce::Component* content_;
    bool owns_;
    juce::ComponentDragger dragger_;
    juce::ComponentBoundsConstrainer limits_;
    std::unique_ptr<juce::ResizableBorderComponent> border_;
    juce::Component::SafePointer<juce::Component> home_;     // parent to return to when docked back
    juce::Rectangle<int> homeBounds_;
    int hover_ = 0;
    bool draggingTitle_ = false;
};
