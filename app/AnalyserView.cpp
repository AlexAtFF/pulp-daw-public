#include "AnalyserView.h"
#include "pulp/Dsp.h"
#include "pulp/Multisample.h"

namespace {
const double kLowHz = 20.0, kHighHz = 20000.0;
float xOfHz(double hz, juce::Rectangle<int> a) { return (float)a.getX() + (float)(std::log(hz / kLowHz) / std::log(kHighHz / kLowHz)) * (float)a.getWidth(); }
double hzOfX(float x, juce::Rectangle<int> a) { return kLowHz * std::pow(kHighHz / kLowHz, (double)(x - (float)a.getX()) / (double)juce::jmax(1, a.getWidth())); }
juce::String dbText(float db) { return db <= -119.0f ? juce::String("-inf") : juce::String(db, 1); }
// black, through violet and orange, to white
juce::Colour heat(float t) {
    t = juce::jlimit(0.0f, 1.0f, t);
    return juce::Colour::fromHSV(0.72f - 0.62f * t, juce::jlimit(0.0f, 1.0f, 1.3f - t), juce::jlimit(0.0f, 1.0f, t * 1.6f), 1.0f);
}
}

AnalyserView::AnalyserView(Session& s, std::shared_ptr<pulp::Plugin> plugin) : s_(s), plugin_(std::move(plugin)) {}

juce::String AnalyserView::getTooltip() {
    switch (gauge()->pView->getInt()) {
        case pulp::Gauge::Spectrum: return "Spectrum: how loud each frequency is right now (the thin line is the highest it has been). Click to clear the held line.";
        case pulp::Gauge::Spectrogram: return "Spectrogram: frequency upward, time to the right, brightness is level";
        case pulp::Gauge::Oscilloscope: return "Oscilloscope: the waveform itself, left above right";
        case pulp::Gauge::Vectorscope: return "Vectorscope: a vertical line is mono, a wide cloud is wide stereo, a horizontal line is out of phase. The bar below is the correlation: right of centre is safe in mono.";
        case pulp::Gauge::Loudness: return "Loudness: momentary (400 ms), short-term (3 s) and integrated (since playback started) in LUFS, with peak and RMS levels. Click to start the integrated reading again.";
        default: return "Tuner: the note that is sounding and how far it is from being in tune";
    }
}

void AnalyserView::mouseDown(const juce::MouseEvent&) {
    held_.clear();
    gauge()->resetIntegrated();
    repaint();
}

void AnalyserView::analyse() {
    const auto a = getLocalBounds().reduced(1);
    const int view = gauge()->pView->getInt();
    const double sr = s_.engine.sampleRate();
    if (view == pulp::Gauge::Spectrum || view == pulp::Gauge::Spectrogram) {
        const int n = 4096;
        std::vector<float> re((size_t)n), im((size_t)n, 0.0f);
        if (gauge()->readScope(re.data(), n) != n) return;
        for (int i = 0; i < n; ++i) re[(size_t)i] *= (float)(0.5 - 0.5 * std::cos(pulp::kTwoPi * i / (n - 1)));
        pulp::dsp::fft(re.data(), im.data(), n);
        auto level = [&](double f0, double f1) {
            const int b0 = juce::jlimit(1, n / 2 - 1, (int)(f0 * n / sr)), b1 = juce::jlimit(b0, n / 2 - 1, (int)(f1 * n / sr));
            float mag = 0.0f;
            for (int b = b0; b <= b1; ++b) mag = std::max(mag, std::hypot(re[(size_t)b], im[(size_t)b]));
            return 20.0f * std::log10(mag / (float)(n / 4) + 1.0e-9f);
        };
        if (view == pulp::Gauge::Spectrum) {
            if ((int)spectrum_.size() != a.getWidth()) { spectrum_.assign((size_t)a.getWidth(), -140.0f); held_.clear(); }
            if (held_.size() != spectrum_.size()) held_.assign(spectrum_.size(), -140.0f);
            for (int x = 0; x < a.getWidth(); ++x) {
                const float db = level(hzOfX((float)(a.getX() + x), a), hzOfX((float)(a.getX() + x + 1), a));
                spectrum_[(size_t)x] = std::max(db, spectrum_[(size_t)x] - 1.2f);
                held_[(size_t)x] = std::max(held_[(size_t)x] - (gauge()->pHold->getBool() ? 0.0f : 0.15f), db);
            }
        } else {
            if (gram_.getWidth() != a.getWidth() || gram_.getHeight() != a.getHeight()) { gram_ = juce::Image(juce::Image::RGB, juce::jmax(1, a.getWidth()), juce::jmax(1, a.getHeight()), true); }
            gram_.moveImageSection(0, 0, 2, 0, gram_.getWidth() - 2, gram_.getHeight());
            const float range = gauge()->pRange->get();
            for (int y = 0; y < gram_.getHeight(); ++y) {
                // frequency rises from the bottom, on a log scale
                const double f0 = kLowHz * std::pow(kHighHz / kLowHz, (double)(gram_.getHeight() - 1 - y) / gram_.getHeight());
                const double f1 = kLowHz * std::pow(kHighHz / kLowHz, (double)(gram_.getHeight() - y) / gram_.getHeight());
                const auto c = heat((level(f0, f1) + range) / range);
                gram_.setPixelAt(gram_.getWidth() - 1, y, c);
                gram_.setPixelAt(gram_.getWidth() - 2, y, c);
            }
        }
    } else if (view == pulp::Gauge::Tuner) {
        if (++tick_ % 3 != 0) return;              // ten times a second is plenty for a needle
        auto s = std::make_shared<pulp::SampleData>();
        s->sampleRate = sr;
        s->channels = 1;
        s->ch[0].resize(8192);
        if (gauge()->readScope(s->ch[0].data(), 8192) != 8192) return;
        float peak = 0.0f;
        for (float v : s->ch[0]) peak = std::max(peak, std::abs(v));
        if (peak < 0.003f) { pitchClarity_ = 0.0; return; }
        const auto p = pulp::ms::detectPitch(*s);
        pitchClarity_ = p.clarity;
        if (p.clarity >= 0.5 && p.hz > 20.0) { pitchKey_ = p.midi; pitchShown_ += 0.5 * ((p.midi - std::round(p.midi)) * 100.0 - pitchShown_); }
    } else {
        const int n = view == pulp::Gauge::Oscilloscope ? 4096 : 2048;
        left_.resize((size_t)n);
        right_.resize((size_t)n);
        gauge()->readStereo(left_.data(), right_.data(), n);
    }
}

void AnalyserView::uiTick() {
    if (!UiTicker::onSlowTick()) return;
    if (!isShowing()) return;
    analyse();
    repaint();
}

juce::String AnalyserView::describe() const {
    auto* g = gauge();
    return "gauge view=" + juce::String(g->pView->getInt()) + " peakL=" + dbText(g->meter(pulp::Gauge::PeakL)) + " peakR=" + dbText(g->meter(pulp::Gauge::PeakR))
         + " rms=" + dbText(g->meter(pulp::Gauge::Rms)) + " momentary=" + dbText(g->meter(pulp::Gauge::Momentary)) + " short=" + dbText(g->meter(pulp::Gauge::ShortTerm))
         + " integrated=" + dbText((float)g->integratedLufs()) + " correlation=" + juce::String(g->meter(pulp::Gauge::Correlation), 2)
         + " note=" + (pitchClarity_ >= 0.5 ? juce::String(pulp::ms::noteName((int)std::lround(pitchKey_))) + " " + juce::String(pitchShown_, 0) + "ct" : juce::String("-"));
}

void AnalyserView::paint(juce::Graphics& g) {
    const auto a = getLocalBounds();
    theme::insetBox(g, a.toFloat());
    auto* ga = gauge();
    const int view = ga->pView->getInt();
    const float range = ga->pRange->get();
    g.saveState();
    g.reduceClipRegion(a.reduced(1));
    g.setFont(theme::font(9.5f));
    auto freqGrid = [&] {
        for (double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
            g.setColour(theme::textDim.withAlpha(0.22f));
            g.drawVerticalLine((int)xOfHz(hz, a), (float)a.getY(), (float)a.getBottom());
            g.setColour(theme::textDim.withAlpha(0.8f));
            g.drawText(hz >= 1000.0 ? juce::String(hz / 1000.0, 0) + "k" : juce::String(hz, 0), (int)xOfHz(hz, a) + 2, a.getBottom() - 12, 30, 10, juce::Justification::centredLeft, false);
        }
    };
    if (view == pulp::Gauge::Spectrum) {
        freqGrid();
        for (float db = -12.0f; db > -range; db -= 12.0f) {
            const float y = (float)a.getY() + (-db / range) * (float)a.getHeight();
            g.setColour(theme::textDim.withAlpha(0.18f));
            g.drawHorizontalLine((int)y, (float)a.getX(), (float)a.getRight());
            g.setColour(theme::textDim.withAlpha(0.7f));
            g.drawText(juce::String((int)db), a.getX() + 3, (int)y - 10, 30, 10, juce::Justification::centredLeft, false);
        }
        auto yOf = [&](float db) { return (float)a.getY() + juce::jlimit(0.0f, 1.0f, -db / range) * (float)a.getHeight(); };
        if (!spectrum_.empty()) {
            juce::Path fill, line;
            fill.startNewSubPath((float)a.getX(), (float)a.getBottom());
            for (size_t x = 0; x < spectrum_.size(); ++x) {
                fill.lineTo((float)a.getX() + (float)x, yOf(spectrum_[x]));
                if (x == 0) line.startNewSubPath((float)a.getX(), yOf(held_[x])); else line.lineTo((float)a.getX() + (float)x, yOf(held_[x]));
            }
            fill.lineTo((float)a.getRight(), (float)a.getBottom());
            fill.closeSubPath();
            g.setColour(theme::accent2.withAlpha(0.45f));
            g.fillPath(fill);
            g.setColour(theme::accent.withAlpha(0.8f));
            g.strokePath(line, juce::PathStrokeType(1.0f));
        }
    } else if (view == pulp::Gauge::Spectrogram) {
        if (gram_.isValid()) g.drawImageAt(gram_, a.getX() + 1, a.getY() + 1);
        g.setColour(juce::Colours::white.withAlpha(0.6f));
        for (double hz : {100.0, 1000.0, 10000.0}) {
            const int y = a.getBottom() - (int)(std::log(hz / kLowHz) / std::log(kHighHz / kLowHz) * a.getHeight());
            g.drawText(hz >= 1000.0 ? juce::String(hz / 1000.0, 0) + "k" : juce::String(hz, 0), a.getX() + 3, y - 5, 30, 10, juce::Justification::centredLeft, false);
        }
    } else if (view == pulp::Gauge::Oscilloscope) {
        // start on a rising zero crossing so a steady note stands still
        int start = 0;
        const int n = (int)left_.size(), shown = juce::jmin(n / 2, 1200);
        for (int i = 1; i < n - shown; ++i) if (left_[(size_t)i - 1] + right_[(size_t)i - 1] <= 0.0f && left_[(size_t)i] + right_[(size_t)i] > 0.0f) { start = i; break; }
        for (int side = 0; side < 2 && n > 0; ++side) {
            const auto half = a.withHeight(a.getHeight() / 2).translated(0, side * a.getHeight() / 2);
            g.setColour(theme::textDim.withAlpha(0.25f));
            g.drawHorizontalLine(half.getCentreY(), (float)a.getX(), (float)a.getRight());
            juce::Path p;
            const auto& d = side == 0 ? left_ : right_;
            for (int x = 0; x < half.getWidth(); ++x) {
                const float v = d[(size_t)(start + x * shown / juce::jmax(1, half.getWidth()))];
                const float y = (float)half.getCentreY() - juce::jlimit(-1.0f, 1.0f, v) * (float)half.getHeight() * 0.47f;
                if (x == 0) p.startNewSubPath((float)half.getX(), y); else p.lineTo((float)(half.getX() + x), y);
            }
            g.setColour(side == 0 ? theme::accent2 : theme::accent);
            g.strokePath(p, juce::PathStrokeType(1.2f));
            g.setColour(theme::textDim);
            g.drawText(side == 0 ? "L" : "R", half.getX() + 4, half.getY() + 3, 12, 10, juce::Justification::centredLeft, false);
        }
    } else if (view == pulp::Gauge::Vectorscope) {
        const auto scope = a.withTrimmedBottom(26);
        const float cx = (float)scope.getCentreX(), cy = (float)scope.getCentreY(), r = (float)juce::jmin(scope.getWidth(), scope.getHeight()) * 0.46f;
        g.setColour(theme::textDim.withAlpha(0.25f));
        g.drawLine(cx, cy - r, cx, cy + r);
        g.drawLine(cx - r, cy, cx + r, cy);
        g.drawLine(cx - r * 0.7f, cy - r * 0.7f, cx + r * 0.7f, cy + r * 0.7f);
        g.drawLine(cx - r * 0.7f, cy + r * 0.7f, cx + r * 0.7f, cy - r * 0.7f);
        g.setColour(theme::textDim);
        g.drawText("L", (int)(cx - r * 0.7f) - 12, (int)(cy - r * 0.7f) - 10, 12, 10, juce::Justification::centred, false);
        g.drawText("R", (int)(cx + r * 0.7f), (int)(cy - r * 0.7f) - 10, 12, 10, juce::Justification::centred, false);
        g.setColour(theme::accent2.withAlpha(0.7f));
        for (size_t i = 0; i < left_.size(); ++i) {
            // mid goes up, side goes across
            const float mid = (left_[i] + right_[i]) * 0.7071f, side = (right_[i] - left_[i]) * 0.7071f;
            g.fillRect(cx + juce::jlimit(-1.0f, 1.0f, side) * r, cy - juce::jlimit(-1.0f, 1.0f, mid) * r, 1.3f, 1.3f);
        }
        const auto bar = a.withTop(a.getBottom() - 22).reduced(30, 5);
        const float c = ga->meter(pulp::Gauge::Correlation);
        g.setColour(theme::panelLo);
        g.fillRect(bar);
        g.setColour(c >= 0.0f ? theme::good : theme::bad);
        const float mid = (float)bar.getCentreX();
        g.fillRect(juce::Rectangle<float>(juce::jmin(mid, mid + c * (float)bar.getWidth() * 0.5f), (float)bar.getY(), std::abs(c) * (float)bar.getWidth() * 0.5f, (float)bar.getHeight()));
        g.setColour(theme::text);
        g.drawText("-1", bar.getX() - 22, bar.getY(), 20, bar.getHeight(), juce::Justification::centredRight, false);
        g.drawText("+1", bar.getRight() + 2, bar.getY(), 20, bar.getHeight(), juce::Justification::centredLeft, false);
        g.drawText("correlation " + juce::String(c, 2), bar, juce::Justification::centred, false);
    } else if (view == pulp::Gauge::Loudness) {
        struct Row { const char* name; float value; const char* unit; };
        const Row rows[] = {{"Momentary (400 ms)", ga->meter(pulp::Gauge::Momentary), "LUFS"}, {"Short-term (3 s)", ga->meter(pulp::Gauge::ShortTerm), "LUFS"},
                            {"Integrated (since play)", (float)ga->integratedLufs(), "LUFS"}, {"Peak, left", ga->meter(pulp::Gauge::PeakL), "dBFS"},
                            {"Peak, right", ga->meter(pulp::Gauge::PeakR), "dBFS"}, {"RMS", ga->meter(pulp::Gauge::Rms), "dBFS"}};
        const int rowH = (a.getHeight() - 8) / 6;
        for (int i = 0; i < 6; ++i) {
            const auto row = juce::Rectangle<int>(a.getX() + 8, a.getY() + 4 + i * rowH, a.getWidth() - 16, rowH - 4);
            g.setColour(theme::textDim);
            g.setFont(theme::font(11.5f));
            g.drawText(rows[i].name, row.withWidth(150), juce::Justification::centredLeft, false);
            const auto bar = row.withTrimmedLeft(154).withTrimmedRight(96).reduced(0, 6);
            g.setColour(theme::panelLo);
            g.fillRect(bar);
            const float t = juce::jlimit(0.0f, 1.0f, (rows[i].value + 60.0f) / 60.0f);
            const bool hot = rows[i].value > (i < 3 ? -9.0f : -0.3f);
            g.setColour(hot ? theme::bad : (i < 3 ? theme::accent : theme::accent2));
            g.fillRect(bar.withWidth((int)(t * (float)bar.getWidth())));
            g.setColour(theme::text);
            g.setFont(theme::mono(15.0f));
            g.drawText(dbText(rows[i].value) + " " + rows[i].unit, row.withLeft(row.getRight() - 92), juce::Justification::centredRight, false);
        }
    } else {
        const bool sure = pitchClarity_ >= 0.5;
        const int key = (int)std::lround(pitchKey_);
        g.setColour(sure ? theme::text : theme::textDim);
        g.setFont(theme::font(64.0f, true));
        g.drawText(sure ? juce::String(pulp::ms::noteName(key)) : juce::String("-"), a.withTrimmedBottom(a.getHeight() / 2 - 10), juce::Justification::centred, false);
        const auto dial = a.withTop(a.getCentreY() + 20).reduced(40, 0).withHeight(46);
        g.setColour(theme::panelLo);
        g.fillRect(dial.withHeight(8).translated(0, 14));
        g.setFont(theme::font(10.0f));
        for (int c = -50; c <= 50; c += 10) {
            const float x = (float)dial.getCentreX() + (float)c / 50.0f * (float)dial.getWidth() * 0.5f;
            g.setColour(c == 0 ? theme::good : theme::textDim.withAlpha(0.6f));
            g.drawVerticalLine((int)x, (float)dial.getY() + (c == 0 ? 4.0f : 10.0f), (float)dial.getY() + 26.0f);
            if (c % 50 == 0) g.drawText(juce::String(c), (int)x - 16, dial.getY() + 30, 32, 12, juce::Justification::centred, false);
        }
        if (sure) {
            const float x = (float)dial.getCentreX() + juce::jlimit(-1.0f, 1.0f, (float)pitchShown_ / 50.0f) * (float)dial.getWidth() * 0.5f;
            g.setColour(std::abs(pitchShown_) < 5.0 ? theme::good : theme::accent);
            g.fillRect(x - 2.0f, (float)dial.getY(), 4.0f, 30.0f);
            g.setColour(theme::text);
            g.setFont(theme::font(13.0f));
            g.drawText(juce::String(pulp::keyToHz(pitchKey_), 1) + " Hz,  " + (pitchShown_ >= 0.0 ? "+" : "") + juce::String(pitchShown_, 0) + " cents", a.withTop(dial.getBottom() + 2).withHeight(18), juce::Justification::centred, false);
        }
    }
    g.restoreState();
}
