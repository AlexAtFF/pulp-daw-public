#include "Widgets.h"

UiTicker& UiTicker::get() { static UiTicker t; return t; }
void UiTicker::timerCallback() {
    ++count_;
    for (int i = clients_.size(); --i >= 0;)
        if (i < clients_.size()) clients_.getUnchecked(i)->uiTick();
}

void askForText(const juce::String& title, const juce::String& message, const juce::String& initial,
                juce::Component* parent, std::function<void(juce::String)> onOk) {
    auto* w = new juce::AlertWindow(title, message, juce::MessageBoxIconType::NoIcon, parent);
    w->addTextEditor("v", initial);
    w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([w, onOk](int r) {
        if (r == 1) onOk(w->getTextEditorContents("v"));
    }), true);
}

// ------------------------------------------------------------------ ParamKnob

static float gKnobClipboard = 0.0f;
static bool gKnobClipboardSet = false;

std::function<void(pulp::Param*, juce::PopupMenu&, int)> ParamUi::addMenuItems;
std::function<void(pulp::Param*, int)> ParamUi::menuChosen;
std::function<void(pulp::Param*, int)> ParamUi::touched;
std::function<bool(pulp::Param*)> ParamUi::valid;

void ParamUi::nudge(pulp::Param* p, float value) {
    if (!p) return;
    touch(p, 0);
    p->set(value);
    touch(p, 1);
    touch(p, 2);
}

void ParamUi::showMenu(pulp::Param* p, juce::Component* target, std::function<void()> afterChange) {
    if (!p) return;
    juce::PopupMenu m;
    m.addSectionHeader(juce::String(p->info.name) + "   " + juce::String(p->display()));
    m.addItem(1, "Reset");
    m.addItem(2, "Type in value...");
    m.addSeparator();
    m.addItem(3, "Copy value");
    m.addItem(4, "Paste value", gKnobClipboardSet);
    if (addMenuItems) { m.addSeparator(); addMenuItems(p, m, 100); }
    juce::Component::SafePointer<juce::Component> safe(target);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target), [safe, p, afterChange](int r) {
        // the project may have been rebuilt while the menu was open: then the control it was opened on is gone
        if (r <= 0 || !safe || (valid && !valid(p))) return;
        auto changed = [&] { safe->repaint(); if (afterChange) afterChange(); };
        if (r == 1) { nudge(p, p->info.def); changed(); }
        else if (r == 2) {
            const bool pct = p->info.unit == "%";
            askForText(p->info.name, "Enter a value" + juce::String(p->info.unit.empty() ? "" : " (" + p->info.unit + ")"),
                       juce::String(pct ? p->get() * 100.0f : p->get()), safe.getComponent(), [safe, p, pct, afterChange](juce::String s) {
                if (!safe || (valid && !valid(p))) return;
                const float v = s.retainCharacters("0123456789.-").getFloatValue();
                nudge(p, pct ? v / 100.0f : v);
                safe->repaint();
                if (afterChange) afterChange();
            });
        } else if (r == 3) { gKnobClipboard = p->getNorm(); gKnobClipboardSet = true; }
        else if (r == 4) { nudge(p, p->fromNorm(gKnobClipboard)); changed(); }
        else if (r >= 100 && menuChosen) menuChosen(p, r - 100);
    });
}

ParamKnob::ParamKnob(pulp::Param* p) : param_(p) {}

juce::String ParamKnob::getTooltip() {
    if (!param_) return {};
    return juce::String(param_->info.name) + ": " + juce::String(param_->display());
}

void ParamKnob::uiTick() {
    if (!param_) return;
    const float n = param_->getNorm();
    if (n != shown_) repaint();
}

void ParamKnob::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat();
    if (label.isNotEmpty()) {
        g.setColour(theme::textDim);
        g.setFont(theme::font(10.0f));
        g.drawText(label, r.removeFromBottom(11.0f), juce::Justification::centred, false);
    }
    const float d = juce::jmin(r.getWidth(), r.getHeight()) - 2.0f;
    auto k = juce::Rectangle<float>(d, d).withCentre(r.getCentre());
    const float n = param_ ? param_->getNorm() : 0.0f;
    shown_ = n;
    const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = juce::MathConstants<float>::pi * 2.75f;
    const float a = a0 + n * (a1 - a0);
    const float cx = k.getCentreX(), cy = k.getCentreY(), rad = d * 0.5f;

    juce::Path track;
    track.addCentredArc(cx, cy, rad - 1.5f, rad - 1.5f, 0.0f, a0, a1, true);
    g.setColour(theme::inset);
    g.strokePath(track, juce::PathStrokeType(2.4f));
    juce::Path val;
    const float from = bipolar ? (a0 + a1) * 0.5f : a0;
    val.addCentredArc(cx, cy, rad - 1.5f, rad - 1.5f, 0.0f, juce::jmin(from, a), juce::jmax(from, a), true);
    g.setColour(isEnabled() ? colour : theme::textDim);
    g.strokePath(val, juce::PathStrokeType(2.4f));

    const float body = rad - 4.0f;
    g.setColour(theme::panelHi.brighter(dragging_ ? 0.15f : 0.0f));
    g.fillEllipse(cx - body, cy - body, body * 2.0f, body * 2.0f);
    g.setColour(theme::line);
    g.drawEllipse(cx - body, cy - body, body * 2.0f, body * 2.0f, 1.0f);
    g.setColour(theme::text);
    const float px = cx + std::sin(a) * body * 0.85f, py = cy - std::cos(a) * body * 0.85f;
    const float qx = cx + std::sin(a) * body * 0.25f, qy = cy - std::cos(a) * body * 0.25f;
    g.drawLine(qx, qy, px, py, 1.6f);
}

void ParamKnob::setValueNorm(float n, bool endGesture) {
    if (!param_) return;
    if (!dragging_) ParamUi::touch(param_, 0);
    param_->setNorm(n);
    ParamUi::touch(param_, 1);
    if (!dragging_) ParamUi::touch(param_, 2);
    repaint();
    if (onChange) onChange();
    if (endGesture && onGestureEnd) onGestureEnd();
}

void ParamKnob::reset() {
    if (!param_) return;
    ParamUi::nudge(param_, param_->info.def);
    repaint();
    if (onChange) onChange();
    if (onGestureEnd) onGestureEnd();
}

void ParamKnob::mouseDown(const juce::MouseEvent& e) {
    if (!param_) return;
    if (e.mods.isPopupMenu()) {
        const auto now = juce::Time::getMillisecondCounter();
        if (now - lastRightClick_ < 400) { lastRightClick_ = 0; juce::PopupMenu::dismissAllActiveMenus(); reset(); return; }
        lastRightClick_ = now;
        showMenu();
        return;
    }
    if (e.mods.isAltDown()) { reset(); return; }
    dragStartNorm_ = param_->getNorm();
    dragging_ = true;
    ParamUi::touch(param_, 0);
    repaint();
}

void ParamKnob::mouseDrag(const juce::MouseEvent& e) {
    if (!param_ || !dragging_) return;
    const float range = e.mods.isCtrlDown() ? 1200.0f : 160.0f;
    setValueNorm(dragStartNorm_ - (float)e.getDistanceFromDragStartY() / range, false);
}

void ParamKnob::mouseUp(const juce::MouseEvent&) {
    if (dragging_) {
        dragging_ = false;
        ParamUi::touch(param_, 2);
        repaint();
        if (onGestureEnd) onGestureEnd();
    }
}

void ParamKnob::mouseDoubleClick(const juce::MouseEvent& e) {
    if (!e.mods.isPopupMenu()) reset();
}

void ParamKnob::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    if (!param_) return;
    const float stepN = param_->info.steps > 1 ? 1.0f / (float)(param_->info.steps - 1) : (e.mods.isCtrlDown() ? 0.005f : 0.03f);
    setValueNorm(param_->getNorm() + (w.deltaY > 0 ? stepN : -stepN), true);
}

void ParamKnob::showMenu() {
    if (!param_) return;
    juce::Component::SafePointer<ParamKnob> safe(this);
    ParamUi::showMenu(param_, this, [safe] {
        if (!safe) return;
        if (safe->onChange) safe->onChange();
        if (safe->onGestureEnd) safe->onGestureEnd();
    });
}

// ------------------------------------------------------------------ NumberBox

void NumberBox::uiTick() {
    if (getValue && getValue() != shown_) repaint();
}

void NumberBox::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat();
    theme::insetBox(g, r);
    const double v = getValue ? getValue() : 0.0;
    shown_ = v;
    auto inner = r.reduced(4.0f, 1.0f);
    if (caption.isNotEmpty()) {
        g.setColour(theme::textDim);
        g.setFont(theme::font(9.5f));
        g.drawText(caption, inner.removeFromTop(11.0f), juce::Justification::centredLeft, false);
    }
    g.setColour(theme::accent);
    g.setFont(theme::mono(juce::jmin(20.0f, inner.getHeight() + (caption.isEmpty() ? 1.0f : 0.0f))));
    g.drawText(format ? format(v) : juce::String(v, 1), inner, juce::Justification::centredRight, false);
}

void NumberBox::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) { if (onRightClick) onRightClick(); return; }
    dragStart_ = getValue ? getValue() : 0.0;
}

void NumberBox::mouseDrag(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu() || !setValue) return;
    const double st = e.mods.isCtrlDown() ? fineStep : step;
    const double v = dragStart_ - std::round(e.getDistanceFromDragStartY() / 3.0) * st;
    setValue(juce::jlimit(minValue, maxValue, v));
    dragged_ = true;
    repaint();
}

void NumberBox::mouseUp(const juce::MouseEvent&) {
    if (dragged_ && onGestureEnd) onGestureEnd();
    dragged_ = false;
}

void NumberBox::mouseDoubleClick(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu() || !setValue) return;
    juce::Component::SafePointer<NumberBox> safe(this);
    askForText(caption.isEmpty() ? "Value" : caption, "Enter a value", juce::String(getValue ? getValue() : 0.0), this, [safe](juce::String s) {
        if (safe && safe->setValue) { safe->setValue(juce::jlimit(safe->minValue, safe->maxValue, s.getDoubleValue())); safe->repaint(); if (safe->onGestureEnd) safe->onGestureEnd(); }
    });
}

void NumberBox::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    if (!setValue || !getValue) return;
    const double st = e.mods.isCtrlDown() ? fineStep : step;
    setValue(juce::jlimit(minValue, maxValue, getValue() + (w.deltaY > 0 ? st : -st)));
    repaint();
    if (onGestureEnd) onGestureEnd();
}

// ------------------------------------------------------------------ IconButton

void IconButton::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat().reduced(1.0f);
    theme::bevel(g, r, over_ ? theme::panelHi.brighter(0.08f) : theme::panelHi);
    const auto c = lit ? litColour : theme::text.withAlpha(isEnabled() ? 0.85f : 0.3f);
    g.setColour(c);
    auto i = r.reduced(r.getHeight() * 0.28f);
    const float s = juce::jmin(i.getWidth(), i.getHeight());
    auto q = juce::Rectangle<float>(s, s).withCentre(i.getCentre());
    switch (icon) {
        case Play: {
            juce::Path p;
            p.addTriangle(q.getX(), q.getY(), q.getRight(), q.getCentreY(), q.getX(), q.getBottom());
            g.fillPath(p);
            break;
        }
        case Stop: g.fillRect(q); break;
        case Record: g.fillEllipse(q); break;
        case Metronome: {
            juce::Path p;
            p.addTriangle(q.getCentreX(), q.getY(), q.getRight(), q.getBottom(), q.getX(), q.getBottom());
            g.strokePath(p, juce::PathStrokeType(1.5f));
            g.drawLine(q.getCentreX(), q.getBottom() - 2.0f, q.getRight(), q.getY() + 2.0f, 1.5f);
            break;
        }
        case Pattern: case Song: case Text:
            g.setFont(theme::font(juce::jmin(13.0f, r.getHeight() * 0.55f), true));
            g.drawText(text, r, juce::Justification::centred, false);
            break;
    }
}

void IconButton::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) { if (onRightClick) onRightClick(); return; }
    if (onClick) onClick();
}
