#pragma once
// A pad that moves two parameters at once: left to right is one, bottom to top the other.
// Movements are reported like any knob's, so they can be recorded and automated.
#include "Session.h"
#include "Widgets.h"

class XyParamPad : public juce::Component, public juce::TooltipClient, private UiTicker::Client {
public:
    static constexpr int kHeight = 200;
    XyParamPad(Session& s, std::shared_ptr<pulp::Plugin> plugin, pulp::Param* x, pulp::Param* y, pulp::Param* held = nullptr)
        : s_(s), plugin_(std::move(plugin)), x_(x), y_(y), held_(held) {}
    juce::String hint = "Drag to move both controls at once";
    std::function<juce::String(int axis)> axisLabel;        // what X (0) and Y (1) do just now, when it depends on a mode
    juce::String getTooltip() override { return hint; }

    void paint(juce::Graphics& g) override {
        theme::insetBox(g, getLocalBounds().toFloat());
        const auto a = area();
        g.setColour(theme::textDim.withAlpha(0.2f));
        for (int i = 1; i < 4; ++i) {
            g.drawVerticalLine((int)(a.getX() + a.getWidth() * (float)i / 4.0f), a.getY(), a.getBottom());
            g.drawHorizontalLine((int)(a.getY() + a.getHeight() * (float)i / 4.0f), a.getX(), a.getRight());
        }
        const float px = a.getX() + x_->getNorm() * a.getWidth(), py = a.getBottom() - y_->getNorm() * a.getHeight();
        g.setColour(theme::accent.withAlpha(0.35f));
        g.drawVerticalLine((int)px, a.getY(), a.getBottom());
        g.drawHorizontalLine((int)py, a.getX(), a.getRight());
        g.setColour(down_ ? theme::accent : theme::accent2);
        g.fillEllipse(px - 8.0f, py - 8.0f, 16.0f, 16.0f);
        g.setColour(theme::textDim);
        g.setFont(theme::font(10.5f));
        const auto nameX = axisLabel ? axisLabel(0) : juce::String(x_->info.name), nameY = axisLabel ? axisLabel(1) : juce::String(y_->info.name);
        g.drawText(nameX + ": " + juce::String(x_->display()) + "  >", a.toNearestInt().reduced(4, 2), juce::Justification::bottomRight, false);
        g.drawText("^  " + nameY + ": " + juce::String(y_->display()), a.toNearestInt().reduced(4, 2), juce::Justification::topLeft, false);
    }
    void mouseDown(const juce::MouseEvent& e) override {
        down_ = true;
        ParamUi::touch(x_, 0);
        ParamUi::touch(y_, 0);
        // a switch that the pad holds on while it is pressed; if it was already on, the pad leaves it alone
        wasOn_ = held_ != nullptr && held_->get() > 0.5f * (held_->info.min + held_->info.max);
        if (held_ && !wasOn_) held_->set(held_->info.max);
        mouseDrag(e);
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        const auto a = area();
        x_->setNorm(juce::jlimit(0.0f, 1.0f, (e.position.x - a.getX()) / a.getWidth()));
        y_->setNorm(juce::jlimit(0.0f, 1.0f, (a.getBottom() - e.position.y) / a.getHeight()));
        ParamUi::touch(x_, 1);
        ParamUi::touch(y_, 1);
        repaint();
    }
    void mouseUp(const juce::MouseEvent&) override {
        down_ = false;
        ParamUi::touch(x_, 2);
        ParamUi::touch(y_, 2);
        if (held_ && !wasOn_) held_->set(held_->info.min);
        s_.commit("Move pad");
        repaint();
    }

private:
    void uiTick() override {
        if (!isShowing() || down_) return;
        const float nx = x_->getNorm(), ny = y_->getNorm();
        if (nx != seenX_ || ny != seenY_) { seenX_ = nx; seenY_ = ny; repaint(); }      // automation moves it
    }
    juce::Rectangle<float> area() const { return getLocalBounds().toFloat().reduced(10.0f); }
    Session& s_;
    std::shared_ptr<pulp::Plugin> plugin_;
    pulp::Param *x_, *y_, *held_;
    bool down_ = false, wasOn_ = false;
    float seenX_ = -1.0f, seenY_ = -1.0f;
};
