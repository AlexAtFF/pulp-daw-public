#include "CurveView.h"

BendCurveView::BendCurveView(Session& s, std::shared_ptr<pulp::Plugin> plugin) : s_(s), plugin_(std::move(plugin)) {}

juce::Rectangle<float> BendCurveView::plot() const {
    const auto a = getLocalBounds().toFloat().reduced(8.0f);
    const float side = juce::jmin(a.getWidth(), a.getHeight());
    return juce::Rectangle<float>(side, side).withCentre(a.getCentre());
}

juce::Point<float> BendCurveView::toScreen(float x, float y) const {
    const auto p = plot();
    return {p.getX() + (x + 1.0f) * 0.5f * p.getWidth(), p.getBottom() - (y + 1.0f) * 0.5f * p.getHeight()};
}

juce::Point<float> BendCurveView::fromScreen(juce::Point<float> pt) const {
    const auto p = plot();
    return {juce::jlimit(-1.0f, 1.0f, (pt.x - p.getX()) / p.getWidth() * 2.0f - 1.0f), juce::jlimit(-1.0f, 1.0f, (p.getBottom() - pt.y) / p.getHeight() * 2.0f - 1.0f)};
}

int BendCurveView::pointAt(juce::Point<float> pt) const {
    const auto pts = bend()->curve();
    for (int i = 0; i < (int)pts.size(); ++i)
        if (toScreen(pts[(size_t)i].first, pts[(size_t)i].second).getDistanceFrom(pt) < 9.0f) return i;
    return -1;
}

juce::String BendCurveView::describe() const {
    juce::String out = "curve:";
    for (auto& p : bend()->curve()) out << " (" << juce::String(p.first, 2) << "," << juce::String(p.second, 2) << ")";
    return out + " transfer(0.5)=" + juce::String(bend()->transfer(0.5), 3);
}

// A preset or an undo may have replaced the curve underneath.
void BendCurveView::uiTick() {
    if (!UiTicker::onSlowTick()) return;
    if (!isShowing() || dragging_ >= 0) return;
    const auto now = describe() + juce::String(bend()->pSymmetry->getInt()) + juce::String(bend()->pPre->get(), 2);
    if (now != seen_) { seen_ = now; repaint(); }
}

void BendCurveView::paint(juce::Graphics& g) {
    theme::insetBox(g, getLocalBounds().toFloat());
    const auto p = plot();
    g.setColour(theme::textDim.withAlpha(0.2f));
    for (int i = 0; i <= 4; ++i) {
        g.drawVerticalLine((int)(p.getX() + p.getWidth() * (float)i / 4.0f), p.getY(), p.getBottom());
        g.drawHorizontalLine((int)(p.getY() + p.getHeight() * (float)i / 4.0f), p.getX(), p.getRight());
    }
    g.setColour(theme::textDim.withAlpha(0.45f));
    g.drawLine(juce::Line<float>(toScreen(-1.0f, -1.0f), toScreen(1.0f, 1.0f)), 1.0f);       // "nothing changes"
    g.setFont(theme::font(9.5f));
    g.drawText("in", (int)p.getRight() - 22, (int)p.getBottom() - 12, 20, 10, juce::Justification::centredRight, false);
    g.drawText("out", (int)p.getX() + 3, (int)p.getY() + 2, 24, 10, juce::Justification::centredLeft, false);
    // The line through the points is the curve as drawn. What a sample actually gets can differ: when the
    // curve covers both polarities it is shifted so that silence stays silence (the thin line); when it is
    // mirrored only the right half is used.
    const auto pts = bend()->curve();
    const bool mirror = bend()->pSymmetry->getInt() == pulp::Bend::Mirror;
    const float shift = mirror || pts.empty() ? 0.0f : pts[0].second - (float)bend()->transfer((double)pts[0].first);
    juce::Path drawn, heard;
    for (int i = 0; i <= 200; ++i) {
        const float x = -1.0f + 2.0f * (float)i / 200.0f;
        const float t = (float)bend()->transfer((double)x);
        const auto a1 = toScreen(x, juce::jlimit(-1.0f, 1.0f, t + shift)), a2 = toScreen(x, juce::jlimit(-1.0f, 1.0f, t));
        if (i == 0) { drawn.startNewSubPath(a1); heard.startNewSubPath(a2); } else { drawn.lineTo(a1); heard.lineTo(a2); }
    }
    if (std::abs(shift) > 0.005f) { g.setColour(theme::text.withAlpha(0.45f)); g.strokePath(heard, juce::PathStrokeType(1.0f)); }
    g.setColour(theme::accent);
    g.strokePath(drawn, juce::PathStrokeType(2.0f));
    for (int i = 0; i < (int)pts.size(); ++i) {
        const auto at = toScreen(pts[(size_t)i].first, pts[(size_t)i].second);
        const bool unused = mirror && pts[(size_t)i].first < 0.0f;
        g.setColour(i == dragging_ ? theme::accent : (unused ? theme::textDim.withAlpha(0.5f) : theme::accent2));
        g.fillEllipse(at.x - 5.0f, at.y - 5.0f, 10.0f, 10.0f);
        g.setColour(theme::line);
        g.drawEllipse(at.x - 5.0f, at.y - 5.0f, 10.0f, 10.0f, 1.0f);
    }
    if (bend()->pSymmetry->getInt() == pulp::Bend::Mirror) {
        g.setColour(theme::textDim);
        g.drawText("mirrored: the right half is used for both polarities (grey points are not used)", (int)p.getX(), (int)p.getBottom() + 1, (int)p.getWidth(), 8, juce::Justification::centred, false);
    }
}

void BendCurveView::mouseDown(const juce::MouseEvent& e) {
    const int i = pointAt(e.position);
    if (e.mods.isPopupMenu()) {
        auto pts = bend()->curve();
        if (i >= 0 && pts.size() > 2) { pts.erase(pts.begin() + i); bend()->setCurve(pts); s_.commit("Remove curve point"); repaint(); }
        return;
    }
    dragging_ = i;
    changed_ = false;
    repaint();
}

void BendCurveView::mouseDrag(const juce::MouseEvent& e) {
    if (dragging_ < 0) return;
    auto pts = bend()->curve();
    if (dragging_ >= (int)pts.size()) return;
    auto v = fromScreen(e.position);
    // a point stays between its neighbours
    if (dragging_ > 0) v.x = juce::jmax(v.x, pts[(size_t)dragging_ - 1].first + 0.01f);
    if (dragging_ + 1 < (int)pts.size()) v.x = juce::jmin(v.x, pts[(size_t)dragging_ + 1].first - 0.01f);
    pts[(size_t)dragging_] = {v.x, v.y};
    bend()->setCurve(pts);
    changed_ = true;
    repaint();
}

void BendCurveView::mouseUp(const juce::MouseEvent&) {
    if (dragging_ >= 0 && changed_) s_.commit("Edit curve");
    dragging_ = -1;
    repaint();
}

void BendCurveView::mouseDoubleClick(const juce::MouseEvent& e) {
    auto pts = bend()->curve();
    if (pointAt(e.position) >= 0 || (int)pts.size() >= pulp::Bend::kMaxPoints) return;
    const auto v = fromScreen(e.position);
    pts.push_back({v.x, v.y});
    bend()->setCurve(pts);          // setCurve sorts the points
    s_.commit("Add curve point");
    repaint();
}
