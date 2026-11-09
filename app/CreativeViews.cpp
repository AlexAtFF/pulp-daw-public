#include "CreativeViews.h"

// ------------------------------------------------------------------ RewindCurveView

RewindCurveView::RewindCurveView(Session& s, std::shared_ptr<pulp::Plugin> plugin) : s_(s), plugin_(std::move(plugin)) {}

juce::Rectangle<float> RewindCurveView::plot(int which) const {
    auto a = getLocalBounds().toFloat().reduced(8.0f, 6.0f);
    const float h = (a.getHeight() - 10.0f) * 0.5f;
    return which == 0 ? a.removeFromTop(h) : a.removeFromBottom(h);
}

// The time plot reaches as far back as the pattern is long; the volume plot goes from silence to full.
float RewindCurveView::top(int which) const {
    return which == 0 ? (float)juce::jlimit(1.0, (double)pulp::Rewind::kMaxBeats, pulp::Rewind::patternBeats(fx()->pLength->getInt())) : 1.0f;
}

juce::Point<float> RewindCurveView::toScreen(int which, float pos, float value) const {
    const auto p = plot(which);
    return {p.getX() + pos * p.getWidth(), p.getBottom() - juce::jlimit(0.0f, 1.0f, value / top(which)) * p.getHeight()};
}

bool RewindCurveView::find(juce::Point<float> at, int& which, int& index) const {
    for (int w = 0; w < 2; ++w) {
        const auto pts = points(w);
        for (int i = 0; i < (int)pts.size(); ++i)
            if (toScreen(w, pts[(size_t)i].pos, pts[(size_t)i].value).getDistanceFrom(at) < 9.0f) { which = w; index = i; return true; }
    }
    return false;
}

juce::String RewindCurveView::getTooltip() {
    const bool time = plot(0).contains(getMouseXYRelative().toFloat());
    return juce::String(time ? "Time: how far behind the sound is, in beats, across one pattern. Flat on the floor is live; a line rising at 45 degrees freezes a moment; steps repeat. "
                             : "Volume across one pattern: up is full, down is silent. ")
         + "Drag a point, double-click to add one, right-click a point to choose how the line leaves it (hold, straight, smooth) or to remove it.";
}

juce::String RewindCurveView::describe() const {
    juce::String out = "rewind time:";
    for (auto& p : fx()->timeCurve()) out << " (" << juce::String(p.pos, 2) << "," << juce::String(p.value, 2) << "," << p.shape << ")";
    out << " volume:";
    for (auto& p : fx()->volumeCurve()) out << " (" << juce::String(p.pos, 2) << "," << juce::String(p.value, 2) << "," << p.shape << ")";
    return out;
}

void RewindCurveView::uiTick() {
    if (!isShowing()) return;
    const float phase = fx()->meter(0);
    const auto now = describe() + juce::String(fx()->pLength->getInt());
    if (phase != seenPhase_ || now != seen_) { seenPhase_ = phase; seen_ = now; repaint(); }
}

void RewindCurveView::paint(juce::Graphics& g) {
    theme::insetBox(g, getLocalBounds().toFloat());
    for (int w = 0; w < 2; ++w) {
        const auto p = plot(w);
        g.setColour(theme::panelLo.withAlpha(0.6f));
        g.fillRect(p);
        // the pattern in quarters and sixteenths
        for (int i = 0; i <= 16; ++i) {
            g.setColour(theme::textDim.withAlpha(i % 4 == 0 ? 0.35f : 0.12f));
            g.drawVerticalLine((int)(p.getX() + p.getWidth() * (float)i / 16.0f), p.getY(), p.getBottom());
        }
        const auto pts = points(w);
        const float def = w == 0 ? 0.0f : 1.0f;
        juce::Path curve;
        for (int i = 0; i <= 300; ++i) {
            const double x = (double)i / 300.0 * 0.999999;
            const auto at = toScreen(w, (float)x, (float)pulp::Rewind::valueAt(pts, x, def));
            if (i == 0) curve.startNewSubPath(at); else curve.lineTo(at);
        }
        g.setColour(w == 0 ? theme::accent : theme::accent2);
        g.strokePath(curve, juce::PathStrokeType(2.0f));
        for (int i = 0; i < (int)pts.size(); ++i) {
            const auto at = toScreen(w, pts[(size_t)i].pos, pts[(size_t)i].value);
            g.setColour(w == dragWhich_ && i == dragIndex_ ? juce::Colours::white : (w == 0 ? theme::accent : theme::accent2).brighter(0.3f));
            // the point's shape says how the line leaves it: square holds, triangle goes straight, round is smooth
            if (pts[(size_t)i].shape == pulp::Rewind::Hold) g.fillRect(at.x - 4.5f, at.y - 4.5f, 9.0f, 9.0f);
            else if (pts[(size_t)i].shape == pulp::Rewind::Line) { juce::Path t; t.addTriangle(at.x - 5.5f, at.y + 4.5f, at.x + 5.5f, at.y + 4.5f, at.x, at.y - 5.5f); g.fillPath(t); }
            else g.fillEllipse(at.x - 5.0f, at.y - 5.0f, 10.0f, 10.0f);
        }
        const float px = p.getX() + juce::jlimit(0.0f, 1.0f, seenPhase_) * p.getWidth();
        g.setColour(juce::Colours::white.withAlpha(0.5f));
        g.drawVerticalLine((int)px, p.getY(), p.getBottom());
        g.setColour(theme::textDim);
        g.setFont(theme::font(10.0f));
        g.drawText(w == 0 ? "TIME: beats behind (top = " + juce::String(top(0), 0) + ")" : juce::String("VOLUME"), p.toNearestInt().reduced(4, 2), juce::Justification::topLeft, false);
        if (pts.empty()) g.drawText(w == 0 ? "live: double-click to add a point" : "full: double-click to add a point", p.toNearestInt(), juce::Justification::centred, false);
    }
}

void RewindCurveView::mouseDown(const juce::MouseEvent& e) {
    int which = -1, index = -1;
    const bool hit = find(e.position, which, index);
    if (e.mods.isPopupMenu()) {
        if (!hit) return;
        juce::PopupMenu m;
        const auto pts = points(which);
        m.addItem(1, "Hold (a step)", true, pts[(size_t)index].shape == pulp::Rewind::Hold);
        m.addItem(2, "Straight line to the next point", true, pts[(size_t)index].shape == pulp::Rewind::Line);
        m.addItem(3, "Smooth curve to the next point", true, pts[(size_t)index].shape == pulp::Rewind::Smooth);
        m.addSeparator();
        m.addItem(4, "Remove this point");
        m.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<RewindCurveView>(this), which, index](int r) {
            if (safe == nullptr || r <= 0) return;
            auto p = safe->points(which);
            if (index >= (int)p.size()) return;
            if (r == 4) p.erase(p.begin() + index); else p[(size_t)index].shape = r - 1;
            safe->setPoints(which, p);
            safe->s_.commit(r == 4 ? "Remove curve point" : "Change curve shape");
            safe->repaint();
        });
        return;
    }
    dragWhich_ = hit ? which : -1;
    dragIndex_ = hit ? index : -1;
    changed_ = false;
    repaint();
}

void RewindCurveView::mouseDrag(const juce::MouseEvent& e) {
    if (dragWhich_ < 0) return;
    auto pts = points(dragWhich_);
    if (dragIndex_ >= (int)pts.size()) return;
    const auto p = plot(dragWhich_);
    float pos = juce::jlimit(0.0f, 1.0f, (e.position.x - p.getX()) / p.getWidth());
    // a point stays between its neighbours; with Shift it is not pulled to the sixteenths
    if (!e.mods.isShiftDown()) pos = std::round(pos * 32.0f) / 32.0f;
    if (dragIndex_ > 0) pos = juce::jmax(pos, pts[(size_t)dragIndex_ - 1].pos);
    if (dragIndex_ + 1 < (int)pts.size()) pos = juce::jmin(pos, pts[(size_t)dragIndex_ + 1].pos);
    pts[(size_t)dragIndex_].pos = pos;
    pts[(size_t)dragIndex_].value = juce::jlimit(0.0f, 1.0f, (p.getBottom() - e.position.y) / p.getHeight()) * top(dragWhich_);
    setPoints(dragWhich_, pts);
    changed_ = true;
    repaint();
}

void RewindCurveView::mouseUp(const juce::MouseEvent&) {
    if (dragWhich_ >= 0 && changed_) s_.commit("Edit curve");
    dragWhich_ = dragIndex_ = -1;
    repaint();
}

void RewindCurveView::mouseDoubleClick(const juce::MouseEvent& e) {
    int which = -1, index = -1;
    if (find(e.position, which, index)) return;
    which = plot(0).contains(e.position) ? 0 : (plot(1).contains(e.position) ? 1 : -1);
    if (which < 0) return;
    auto pts = points(which);
    if ((int)pts.size() >= pulp::Rewind::kMaxPoints) return;
    const auto p = plot(which);
    pulp::Rewind::Point pt;
    pt.pos = std::round(juce::jlimit(0.0f, 1.0f, (e.position.x - p.getX()) / p.getWidth()) * 32.0f) / 32.0f;
    pt.value = juce::jlimit(0.0f, 1.0f, (p.getBottom() - e.position.y) / p.getHeight()) * top(which);
    pt.shape = pulp::Rewind::Line;
    pts.push_back(pt);
    setPoints(which, pts);          // the plugin sorts them
    s_.commit("Add curve point");
    repaint();
}

// ------------------------------------------------------------------ PlatterView

void PlatterView::paint(juce::Graphics& g) {
    theme::insetBox(g, getLocalBounds().toFloat());
    const auto a = getLocalBounds().toFloat().reduced(8.0f);
    const float d = juce::jmin(a.getHeight(), a.getWidth() * 0.4f);
    const juce::Rectangle<float> disc(a.getX() + 6.0f, a.getCentreY() - d * 0.5f, d, d);
    g.setColour(juce::Colour(0xff15171a));
    g.fillEllipse(disc);
    g.setColour(theme::textDim.withAlpha(0.3f));
    for (float r = 0.35f; r < 1.0f; r += 0.16f) g.drawEllipse(disc.withSizeKeepingCentre(d * r, d * r), 1.0f);
    g.setColour(theme::accent);
    g.fillEllipse(disc.withSizeKeepingCentre(d * 0.3f, d * 0.3f));
    // the record turns with the sound under the needle: one turn in 1.8 seconds, as at 33 1/3
    const float behind = fx()->meter(0);
    const float angle = -behind / 1.8f * juce::MathConstants<float>::twoPi;
    g.setColour(juce::Colours::white);
    g.drawLine(disc.getCentreX(), disc.getCentreY(), disc.getCentreX() + std::sin(angle) * d * 0.48f, disc.getCentreY() - std::cos(angle) * d * 0.48f, 2.0f);
    const auto text = a.withTrimmedLeft(d + 22.0f);
    g.setColour(down_ ? theme::accent : theme::text);
    g.setFont(theme::font(13.0f, true));
    g.drawText(down_ ? "holding the record" : "press and drag sideways to scratch", text.withHeight(22.0f), juce::Justification::centredLeft, true);
    g.setColour(theme::textDim);
    g.setFont(theme::mono(12.5f));
    g.drawText("speed under the needle " + juce::String(fx()->meter(1), 2) + "x", text.withTrimmedTop(26.0f).withHeight(18.0f), juce::Justification::centredLeft, false);
    g.drawText(juce::String(behind, 2) + " s behind the input", text.withTrimmedTop(46.0f).withHeight(18.0f), juce::Justification::centredLeft, false);
    // where the hand is
    const auto bar = text.withTrimmedTop(74.0f).withHeight(12.0f).withTrimmedRight(10.0f);
    g.setColour(theme::panelLo);
    g.fillRect(bar);
    const float pos = fx()->pPosition->get();
    g.setColour(theme::accent2);
    g.fillRect(juce::Rectangle<float>(bar.getCentreX() + pos * bar.getWidth() * 0.5f - 3.0f, bar.getY(), 6.0f, bar.getHeight()));
}

void PlatterView::mouseDown(const juce::MouseEvent& e) {
    down_ = true;
    startX_ = e.position.x;
    ParamUi::touch(fx()->pHold, 0);
    ParamUi::touch(fx()->pPosition, 0);
    fx()->pPosition->set(0.0f);
    fx()->pHold->set(1.0f);
    ParamUi::touch(fx()->pHold, 1);
    repaint();
}

void PlatterView::mouseDrag(const juce::MouseEvent& e) {
    if (!down_) return;
    fx()->pPosition->set(juce::jlimit(-1.0f, 1.0f, (e.position.x - startX_) / juce::jmax(60.0f, (float)getWidth() * 0.4f)));
    ParamUi::touch(fx()->pPosition, 1);
    repaint();
}

void PlatterView::mouseUp(const juce::MouseEvent&) {
    if (!down_) return;
    down_ = false;
    fx()->pHold->set(0.0f);
    fx()->pPosition->set(0.0f);
    ParamUi::touch(fx()->pHold, 1);
    ParamUi::touch(fx()->pPosition, 1);
    ParamUi::touch(fx()->pHold, 2);
    ParamUi::touch(fx()->pPosition, 2);
    repaint();
}
