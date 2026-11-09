// Milestone 14: the piano roll's LFO tool as events for a control.
#include "Util.h"
#include "pulp/Automation.h"
#include "pulp/NoteTools.h"
using namespace pulp;
using namespace tu;

namespace {
double wave(int shape, double p) {
    p -= std::floor(p);
    switch (shape) {
        case tools::LfoSine: return 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * p);
        case tools::LfoTriangle: return p < 0.5 ? p * 2.0 : 2.0 - p * 2.0;
        case tools::LfoSquare: return p < 0.5 ? 1.0 : 0.0;
        case tools::LfoSawUp: return p;
        default: return 1.0 - p;
    }
}
void run(Engine& e, int n, int block = 256) {
    std::vector<float> l((size_t)block), r((size_t)block);
    float* out[2] = {l.data(), r.data()};
    for (int c = 0; c < n; c += block) e.process(nullptr, 0, out, 2, std::min(block, n - c));
}
}

TEST(lfo_events_follow_the_wave) {
    const int length = 3840;
    const float lo = 0.2f, hi = 0.9f;
    for (int shape : {tools::LfoSine, tools::LfoTriangle, tools::LfoSquare, tools::LfoSawUp, tools::LfoSawDown})
        for (int period : {960, 1000, 3840, 7000})
            for (float phase : {0.0f, 0.3f, 0.75f}) {
                const auto ev = tools::lfoEvents(length, shape, period, lo, hi, phase);
                CHECK(!ev.empty() && ev.front().tick == 0);
                bool ordered = true, inside = true;
                for (size_t i = 0; i < ev.size(); ++i) {
                    if (i > 0 && ev[i].tick <= ev[i - 1].tick) ordered = false;
                    if (ev[i].tick < 0 || ev[i].tick >= length || ev[i].value < 0.0f || ev[i].value > 1.0f) inside = false;
                }
                CHECK(ordered && inside);
                CHECK(!ev.back().ramp);
                // sixteen straight lines a cycle are within 1 % of a sine; the others are exact but for the tick a turn falls between
                const double tol = shape == tools::LfoSine ? 0.011 * (hi - lo) : 0.004;
                double worst = 0.0;
                int worstTick = -1;
                for (int t = 0; t < length; t += 7) {
                    // after the last event the value holds: nothing to compare there
                    if (t > ev.back().tick) break;
                    const double p = (double)t / period + phase;
                    const double frac = p - std::floor(p);
                    // a step is taken to the nearest tick: leave out the two ticks around it
                    const double toTurn = std::min({frac, 1.0 - frac, std::fabs(frac - 0.5)}) * period;
                    if ((shape == tools::LfoSquare || shape == tools::LfoSawUp || shape == tools::LfoSawDown) && toTurn < 2.0) continue;
                    const double want = lo + (hi - lo) * wave(shape, p);
                    const double err = std::fabs((double)evalEvents(ev, (double)t) - want);
                    if (err > worst) { worst = err; worstTick = t; }
                }
                CHECK_MSG(worst <= tol, "shape %d period %d phase %.2f: off by %.4f at tick %d", shape, period, (double)phase, worst, worstTick);
            }
    // how many events it takes: two a cycle for a triangle or a square, sixteen for a sine
    CHECK(tools::lfoEvents(3840, tools::LfoSquare, 960, 0.0f, 1.0f, 0.0f).size() == 8);
    CHECK(tools::lfoEvents(3840, tools::LfoTriangle, 960, 0.0f, 1.0f, 0.0f).size() == 8);
    CHECK(tools::lfoEvents(3840, tools::LfoSine, 960, 0.0f, 1.0f, 0.0f).size() == 64);
    // nothing sensible to make: nothing made
    CHECK(tools::lfoEvents(3840, tools::LfoSine, 0, 0.0f, 1.0f, 0.0f).empty());
    CHECK(tools::lfoEvents(0, tools::LfoSine, 960, 0.0f, 1.0f, 0.0f).empty());
    // low above high turns the wave over
    const auto down = tools::lfoEvents(3840, tools::LfoSquare, 1920, 0.8f, 0.2f, 0.0f);
    CHECK_NEAR(evalEvents(down, 100.0), 0.2, 1e-6);
    CHECK_NEAR(evalEvents(down, 1000.0), 0.8, 1e-6);
}

TEST(lfo_events_move_a_channel_control) {
    const double kSr = 48000.0;
    Project p;
    p.global->tempo.set(120.0f);            // 25 samples per tick
    auto s = std::make_shared<SampleData>();
    s->sampleRate = kSr; s->channels = 1;
    s->ch[0].assign(64, 0.9f);
    auto ch = addSampleChannel(p, s, "Click");
    const std::string vol = "ch:" + std::to_string(ch.id) + ":volume";
    const std::string pan = "ch:" + std::to_string(ch.id) + ":pan";
    p.pattern().events[vol] = tools::lfoEvents(3840, tools::LfoSquare, 1920, 0.2f, 0.8f, 0.0f);
    p.pattern().events[pan] = tools::lfoEvents(3840, tools::LfoSawUp, 3840, 0.0f, 1.0f, 0.0f);
    CHECK(p.isAutomated(vol) && p.isAutomated(pan));
    p.prepare(kSr);
    Engine e;
    e.prepare(kSr);
    e.setState(p.compile());
    e.play();
    run(e, 25 * 500);                       // tick 500: the square is high
    CHECK_NEAR(ch.live->volume.get(), 0.8, 1e-6);
    CHECK_NEAR(ch.live->pan.get(), -1.0 + 2.0 * (500.0 / 3840.0), 0.04);
    run(e, 25 * 1000);                      // tick 1500: low
    CHECK_NEAR(ch.live->volume.get(), 0.2, 1e-6);
    CHECK_NEAR(ch.live->pan.get(), -1.0 + 2.0 * (1500.0 / 3840.0), 0.04);
    run(e, 25 * 2000);                      // tick 3500: low still, the saw nearly at the top
    CHECK_NEAR(ch.live->volume.get(), 0.2, 1e-6);
    CHECK(ch.live->pan.get() > 0.75f);
    run(e, 25 * 500);                       // tick 4000: the pattern has come round, both start again
    CHECK_NEAR(ch.live->volume.get(), 0.8, 1e-6);
    CHECK(ch.live->pan.get() < -0.85f);
    // saved and loaded with the project
    Project q;
    CHECK(q.fromJson(p.toJson()));
    CHECK(q.pattern().events.count(vol) == 1 && q.pattern().events[vol].size() == p.pattern().events[vol].size());
}
