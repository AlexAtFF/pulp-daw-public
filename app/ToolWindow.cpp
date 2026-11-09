#include "ToolWindow.h"

PulpLookAndFeel::PulpLookAndFeel() {
    using namespace juce;
    setColour(PopupMenu::backgroundColourId, theme::panel);
    setColour(PopupMenu::textColourId, theme::text);
    setColour(PopupMenu::headerTextColourId, theme::accent);
    setColour(PopupMenu::highlightedBackgroundColourId, theme::accent.withAlpha(0.85f));
    setColour(PopupMenu::highlightedTextColourId, Colours::black);
    setColour(AlertWindow::backgroundColourId, theme::panel);
    setColour(AlertWindow::textColourId, theme::text);
    setColour(AlertWindow::outlineColourId, theme::line);
    setColour(TextEditor::backgroundColourId, theme::inset);
    setColour(TextEditor::textColourId, theme::text);
    setColour(TextEditor::outlineColourId, theme::line);
    setColour(TextEditor::focusedOutlineColourId, theme::accent);
    setColour(TextEditor::highlightColourId, theme::accent.withAlpha(0.4f));
    setColour(CaretComponent::caretColourId, theme::accent);
    setColour(TextButton::buttonColourId, theme::panelHi);
    setColour(TextButton::buttonOnColourId, theme::accent);
    setColour(TextButton::textColourOffId, theme::text);
    setColour(TextButton::textColourOnId, Colours::black);
    setColour(ComboBox::backgroundColourId, theme::inset);
    setColour(ComboBox::textColourId, theme::text);
    setColour(ComboBox::outlineColourId, theme::line);
    setColour(ComboBox::arrowColourId, theme::textDim);
    setColour(Label::textColourId, theme::text);
    setColour(ScrollBar::thumbColourId, theme::panelHi.brighter(0.2f));
    setColour(ResizableWindow::backgroundColourId, theme::panel);
    setColour(TreeView::backgroundColourId, theme::panelLo);
    setColour(TreeView::linesColourId, theme::line);
    setColour(ListBox::backgroundColourId, theme::panelLo);
    setColour(TooltipWindow::backgroundColourId, theme::panel);
    setColour(TooltipWindow::textColourId, theme::text);
}

void PulpLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                                    int thumbStart, int thumbSize, bool over, bool down) {
    g.setColour(theme::panelLo);
    g.fillRect(x, y, w, h);
    if (thumbSize <= 0) return;
    auto r = vertical ? juce::Rectangle<int>(x + 2, thumbStart, w - 4, thumbSize) : juce::Rectangle<int>(thumbStart, y + 2, thumbSize, h - 4);
    g.setColour(theme::panelHi.brighter(down ? 0.35f : (over ? 0.25f : 0.12f)));
    g.fillRoundedRectangle(r.toFloat(), 3.0f);
}

ToolWindow::ToolWindow(const juce::String& title, juce::Component* content, bool ownsContent)
    : title_(title), content_(content), owns_(ownsContent) {
    setOpaque(true);
    addAndMakeVisible(content_);
    limits_.setMinimumSize(220, 120);
    limits_.setMinimumOnscreenAmounts(kTitle, 60, 0, 60);
    border_ = std::make_unique<juce::ResizableBorderComponent>(this, &limits_);
    border_->setBorderThickness(juce::BorderSize<int>(4));
    addAndMakeVisible(*border_);
    setWantsKeyboardFocus(true);
    setMouseClickGrabsKeyboardFocus(true);
}

ToolWindow::~ToolWindow() {
    removeChildComponent(content_);
    if (owns_) delete content_;
}

void ToolWindow::resized() {
    border_->setBounds(getLocalBounds());
    content_->setBounds(getLocalBounds().reduced(4).withTrimmedTop(kTitle - 4));
}

void ToolWindow::paint(juce::Graphics& g) {
    g.fillAll(theme::panel);
    auto bar = getLocalBounds().removeFromTop(kTitle);
    const bool front = hasKeyboardFocus(true);
    g.setColour(front ? theme::panelHi.brighter(0.06f) : theme::panelHi);
    g.fillRect(bar);
    g.setColour(front ? theme::accent : theme::textDim);
    g.fillRect(bar.removeFromLeft(4));
    g.setColour(theme::text);
    g.setFont(theme::font(13.0f, true));
    g.drawText(title_, bar.reduced(6, 0).withTrimmedRight(44), juce::Justification::centredLeft, true);
    // detach / dock and close boxes
    auto cb = closeBox().toFloat(), db = detachBox().toFloat();
    g.setColour(hover_ == 1 ? theme::bad : theme::textDim);
    g.drawLine(cb.getX() + 4, cb.getY() + 4, cb.getRight() - 4, cb.getBottom() - 4, 1.6f);
    g.drawLine(cb.getRight() - 4, cb.getY() + 4, cb.getX() + 4, cb.getBottom() - 4, 1.6f);
    g.setColour(hover_ == 2 ? theme::accent : theme::textDim);
    if (isDetached()) g.drawRect(db.reduced(3.5f), 1.4f);
    else { g.drawRect(db.reduced(3.5f).translated(1.5f, -1.5f), 1.2f); g.drawRect(db.reduced(3.5f).translated(-1.5f, 1.5f), 1.2f); }
    g.setColour(theme::line);
    g.drawRect(getLocalBounds(), 1);
}

void ToolWindow::mouseMove(const juce::MouseEvent& e) {
    const int h = closeBox().contains(e.getPosition()) ? 1 : (detachBox().contains(e.getPosition()) ? 2 : 0);
    if (h != hover_) { hover_ = h; repaint(0, 0, getWidth(), kTitle); }
}

void ToolWindow::mouseDown(const juce::MouseEvent& e) {
    toFront(true);
    draggingTitle_ = false;
    if (closeBox().contains(e.getPosition())) { setVisible(false); return; }
    if (detachBox().contains(e.getPosition())) { setDetached(!isDetached()); return; }
    if (e.y < kTitle) { draggingTitle_ = true; dragger_.startDraggingComponent(this, e); }
}

void ToolWindow::mouseDrag(const juce::MouseEvent& e) {
    if (draggingTitle_) dragger_.dragComponent(this, e, isDetached() ? nullptr : &limits_);
}

void ToolWindow::mouseUp(const juce::MouseEvent&) { draggingTitle_ = false; }

void ToolWindow::mouseDoubleClick(const juce::MouseEvent& e) {
    if (e.y >= kTitle || isDetached() || !getParentComponent()) return;
    // double-click the title: fill the workspace, or go back to the previous size
    auto full = getParentComponent()->getLocalBounds();
    if (auto* p = getParentComponent()) {
        full = p->getLocalBounds();
        if (auto v = p->getProperties()["workTop"]; !v.isVoid()) full.removeFromTop((int)v);
        if (auto v = p->getProperties()["workLeft"]; !v.isVoid()) full.removeFromLeft((int)v);
    }
    if (getBounds() == full) setBounds(homeBounds_.isEmpty() ? full.reduced(60) : homeBounds_);
    else { homeBounds_ = getBounds(); setBounds(full); }
}

void ToolWindow::setDetached(bool detached) {
    if (detached == isDetached()) return;
    const bool vis = isVisible();
    if (detached) {
        home_ = getParentComponent();
        homeBounds_ = getBounds();
        const auto screenPos = getScreenPosition();
        addToDesktop(juce::ComponentPeer::windowAppearsOnTaskbar | juce::ComponentPeer::windowHasDropShadow);
        setTopLeftPosition(screenPos);
    } else {
        removeFromDesktop();
        if (home_) { home_->addChildComponent(this); setBounds(homeBounds_); }
    }
    setVisible(vis);
    toFront(true);
    repaint();
}

void ToolWindow::showAndRaise() {
    setVisible(true);
    toFront(true);
    grabKeyboardFocus();
}

void ToolWindow::toggle() {
    if (isVisible() && hasKeyboardFocus(true)) { setVisible(false); if (home_) home_->grabKeyboardFocus(); else if (auto* p = getParentComponent()) p->grabKeyboardFocus(); }
    else showAndRaise();
}
