#include "ParamPanel.h"

juce::String ParamChoice::getTooltip() {
    return param_ ? juce::String(param_->info.name) + ": " + juce::String(param_->display()) : juce::String();
}

void ParamChoice::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat().reduced(1.0f);
    theme::insetBox(g, r);
    if (!param_) return;
    shown_ = param_->get();
    g.setColour(theme::accent);
    g.setFont(theme::font(12.0f));
    g.drawFittedText(param_->display(), getLocalBounds().reduced(5, 1), juce::Justification::centred, 1);
}

void ParamChoice::mouseDown(const juce::MouseEvent& e) {
    if (!param_) return;
    if (e.mods.isPopupMenu()) {     // right-click: the menu every control has
        juce::Component::SafePointer<ParamChoice> self(this);
        ParamUi::showMenu(param_, this, [self] { if (self && self->onChange) self->onChange(); });
        return;
    }
    juce::PopupMenu m;
    const int cur = param_->getInt() - (int)param_->info.min;
    for (int i = 0; i < (int)param_->info.choices.size(); ++i) m.addItem(i + 1, param_->info.choices[(size_t)i], true, i == cur);
    juce::Component::SafePointer<ParamChoice> safe(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [safe](int r) {
        if (!safe || !safe->param_ || r <= 0) return;
        ParamUi::nudge(safe->param_, safe->param_->info.min + (float)(r - 1));
        safe->repaint();
        if (safe->onChange) safe->onChange();
    });
}

void ParamChoice::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) {
    if (!param_) return;
    ParamUi::nudge(param_, param_->get() + (w.deltaY > 0 ? -1.0f : 1.0f));
    repaint();
    if (onChange) onChange();
}

juce::String ParamToggle::getTooltip() {
    return param_ ? juce::String(param_->info.name) + ": " + juce::String(param_->display()) : juce::String();
}

void ParamToggle::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat();
    const bool on = param_ && (param_->getBool() != inverted);
    if (param_) shown_ = param_->get();
    auto lamp = caption.isEmpty() ? r.withSizeKeepingCentre(juce::jmin(12.0f, r.getWidth() - 2), juce::jmin(12.0f, r.getHeight() - 2))
                                  : r.removeFromTop(r.getHeight() - 13.0f).withSizeKeepingCentre(16.0f, 12.0f);
    g.setColour(theme::inset);
    g.fillRoundedRectangle(lamp, 3.0f);
    g.setColour(on ? colour : theme::panelHi);
    g.fillRoundedRectangle(lamp.reduced(2.0f), 2.0f);
    if (caption.isNotEmpty()) {
        g.setColour(theme::textDim);
        g.setFont(theme::font(10.0f));
        g.drawFittedText(caption, r.toNearestInt(), juce::Justification::centred, 1, 0.8f);
    }
}

void ParamToggle::mouseDown(const juce::MouseEvent& e) {
    if (!param_) return;
    if (e.mods.isPopupMenu()) {
        // a switch with its own right-click action (solo on a mute lamp) keeps it; every other switch gets the control menu
        if (onRightClick) { onRightClick(); return; }
        juce::Component::SafePointer<ParamToggle> self(this);
        ParamUi::showMenu(param_, this, [self] { if (self && self->onChange) self->onChange(); });
        return;
    }
    ParamUi::nudge(param_, param_->getBool() ? 0.0f : 1.0f);
    repaint();
    if (onChange) onChange();
}

void ParamPanel::setSections(std::vector<Section> sections) {
    sections_ = std::move(sections);
    cells_.clear();
    sectionStart_.clear();
    removeAllChildren();
    for (auto& s : sections_) {
        sectionStart_.push_back(cells_.size());
        for (pulp::Param* p : s.params) {
            Cell cell;
            cell.p = p;
            cell.label = p->info.name;
            const bool isBool = p->info.steps == 2 && p->info.choices.size() == 2;
            if (isBool) {
                auto t = std::make_unique<ParamToggle>(p);
                t->caption = p->info.name;
                t->onChange = [this, p] { if (onChange) onChange(p); if (onGestureEnd) onGestureEnd(p); };
                cell.c = std::move(t);
                cell.span = 1;
                cell.label = {};
            } else if (!p->info.choices.empty()) {
                auto c = std::make_unique<ParamChoice>(p);
                c->onChange = [this, p] { if (onChange) onChange(p); if (onGestureEnd) onGestureEnd(p); };
                cell.c = std::move(c);
                cell.span = 2;
            } else {
                auto k = std::make_unique<ParamKnob>(p);
                k->bipolar = p->info.min < 0.0f && p->info.max > 0.0f;
                k->onChange = [this, p] { if (onChange) onChange(p); };
                k->onGestureEnd = [this, p] { if (onGestureEnd) onGestureEnd(p); };
                cell.c = std::move(k);
                cell.span = 1;
            }
            addAndMakeVisible(*cell.c);
            cells_.push_back(std::move(cell));
        }
    }
    resized();
}

int ParamPanel::layout(int width, bool apply) {
    int y = 2;
    if (apply) rows_.clear();
    const int cols = juce::jmax(2, (width - 8) / kCellW);
    for (size_t s = 0; s < sections_.size(); ++s) {
        if (sections_[s].title.isNotEmpty()) { if (apply) rows_.push_back({sections_[s].title, y}); y += kHead; }
        const size_t end = s + 1 < sectionStart_.size() ? sectionStart_[s + 1] : cells_.size();
        int col = 0;
        for (size_t i = sectionStart_[s]; i < end; ++i) {
            auto& c = cells_[i];
            if (col + c.span > cols) { col = 0; y += kCellH; }
            if (apply) {
                c.bounds = {4 + col * kCellW, y, c.span * kCellW, kCellH};
                auto b = c.bounds.reduced(3, 2);
                if (c.label.isEmpty()) c.c->setBounds(b);
                else if (c.span == 2) c.c->setBounds(b.withTrimmedBottom(14).withSizeKeepingCentre(b.getWidth(), 22));
                else c.c->setBounds(b.withTrimmedBottom(13));
            }
            col += c.span;
        }
        if (end > sectionStart_[s]) y += kCellH;
        y += 4;
    }
    return y;
}

int ParamPanel::heightFor(int width) { return layout(width, false); }
void ParamPanel::resized() { layout(getWidth(), true); }

void ParamPanel::paint(juce::Graphics& g) {
    g.setFont(theme::font(11.0f, true));
    for (auto& r : rows_) {
        g.setColour(theme::panelLo);
        g.fillRect(2, r.y + 1, getWidth() - 4, kHead - 3);
        g.setColour(theme::accent2);
        g.drawText(r.title.toUpperCase(), 8, r.y, getWidth() - 12, kHead - 1, juce::Justification::centredLeft, false);
    }
    g.setFont(theme::font(10.0f));
    g.setColour(theme::textDim);
    for (auto& c : cells_)
        if (c.label.isNotEmpty())
            g.drawFittedText(c.label, c.bounds.reduced(1, 0).removeFromBottom(14), juce::Justification::centred, 1, 0.9f);
}
