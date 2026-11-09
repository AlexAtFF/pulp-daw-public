#include "Tour.h"

TourOverlay::TourOverlay(std::vector<Step> steps) : steps_(std::move(steps)) {
    setWantsKeyboardFocus(true);
    setAlwaysOnTop(true);
    next_.text = "NEXT";
    next_.lit = true;
    next_.litColour = theme::good;
    next_.onClick = [this] { if (step_ + 1 < (int)steps_.size()) go(step_ + 1); else if (onClose) onClose(true); };
    back_.text = "BACK";
    back_.onClick = [this] { if (step_ > 0) go(step_ - 1); };
    skip_.text = "SKIP THE TOUR";
    skip_.onClick = [this] { if (onClose) onClose(false); };
    addAndMakeVisible(next_);
    addAndMakeVisible(back_);
    addAndMakeVisible(skip_);
}

void TourOverlay::go(int step) {
    step_ = juce::jlimit(0, (int)steps_.size() - 1, step);
    if (steps_[(size_t)step_].before) steps_[(size_t)step_].before();
    next_.text = step_ + 1 < (int)steps_.size() ? "NEXT" : "DONE";
    back_.setVisible(step_ > 0);
    toFront(true);
    resized();
    repaint();
}

juce::String TourOverlay::describe() const {
    const auto& s = steps_[(size_t)step_];
    const auto t = s.target ? s.target() : juce::Rectangle<int>();
    return "tour step " + juce::String(step_ + 1) + " of " + juce::String((int)steps_.size()) + ": " + s.title + " target=" + t.toString() + " bubble=" + bubble().toString();
}

// The text box sits beside what it is about: below it if there is room, else above, else in the middle.
juce::Rectangle<int> TourOverlay::bubble() const {
    const int w = juce::jmin(460, getWidth() - 40), h = 190;
    const auto t = steps_[(size_t)step_].target ? steps_[(size_t)step_].target() : juce::Rectangle<int>();
    if (t.isEmpty()) return juce::Rectangle<int>(w, h).withCentre(getLocalBounds().getCentre());
    int x = juce::jlimit(20, juce::jmax(20, getWidth() - w - 20), t.getCentreX() - w / 2);
    if (t.getBottom() + h + 16 <= getHeight()) return {x, t.getBottom() + 12, w, h};
    if (t.getY() - h - 12 >= 0) return {x, t.getY() - h - 12, w, h};
    if (t.getRight() + w + 16 <= getWidth()) return {t.getRight() + 12, juce::jlimit(20, juce::jmax(20, getHeight() - h - 20), t.getCentreY() - h / 2), w, h};
    return juce::Rectangle<int>(w, h).withCentre(getLocalBounds().getCentre());
}

void TourOverlay::resized() {
    if (steps_.empty()) return;
    auto b = bubble().reduced(12);
    auto row = b.removeFromBottom(26);
    next_.setBounds(row.removeFromRight(84));
    row.removeFromRight(6);
    back_.setBounds(row.removeFromRight(70));
    skip_.setBounds(row.removeFromLeft(120));
}

void TourOverlay::paint(juce::Graphics& g) {
    if (steps_.empty()) return;
    const auto& s = steps_[(size_t)step_];
    const auto t = s.target ? s.target() : juce::Rectangle<int>();
    // everything is dimmed except the part this step is about
    juce::Path dim;
    dim.addRectangle(getLocalBounds());
    if (!t.isEmpty()) { dim.setUsingNonZeroWinding(false); dim.addRoundedRectangle(t.toFloat().expanded(4.0f), 6.0f); }
    g.setColour(juce::Colours::black.withAlpha(0.62f));
    g.fillPath(dim);
    if (!t.isEmpty()) { g.setColour(theme::accent); g.drawRoundedRectangle(t.toFloat().expanded(4.0f), 6.0f, 2.0f); }
    const auto b = bubble();
    g.setColour(theme::panel);
    g.fillRoundedRectangle(b.toFloat(), 8.0f);
    g.setColour(theme::accent);
    g.drawRoundedRectangle(b.toFloat(), 8.0f, 1.5f);
    auto inner = b.reduced(14, 12);
    g.setColour(theme::textDim);
    g.setFont(theme::font(10.5f));
    g.drawText(juce::String(step_ + 1) + " of " + juce::String((int)steps_.size()), inner.removeFromTop(12), juce::Justification::centredRight, false);
    g.setColour(theme::accent);
    g.setFont(theme::font(17.0f, true));
    g.drawText(s.title, inner.removeFromTop(24), juce::Justification::centredLeft, true);
    inner.removeFromTop(4);
    inner.removeFromBottom(32);
    g.setColour(theme::text);
    g.setFont(theme::font(13.5f));
    g.drawFittedText(s.text, inner, juce::Justification::topLeft, 7, 0.9f);
}

bool TourOverlay::keyPressed(const juce::KeyPress& k) {
    if (k.getKeyCode() == juce::KeyPress::escapeKey) { if (onClose) onClose(false); return true; }
    if (k.getKeyCode() == juce::KeyPress::rightKey || k.getKeyCode() == juce::KeyPress::returnKey) { next_.onClick(); return true; }
    if (k.getKeyCode() == juce::KeyPress::leftKey) { back_.onClick(); return true; }
    return true;        // while the tour is up, keys do nothing else
}
