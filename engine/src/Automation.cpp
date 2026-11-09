#include "pulp/Automation.h"

namespace pulp {

const char* autoShapeName(int shape) {
    static const char* names[NumAutoShapes] = {"Single curve", "Double curve", "Hold", "Stairs", "Pulse", "Wave"};
    return names[clampT(shape, 0, (int)NumAutoShapes - 1)];
}

// Bends 0..1 towards a slow start (tension > 0) or a fast start (tension < 0).
static double bend(double x, float tension) {
    if (tension == 0.0f) return x;
    const double p = std::pow(2.0, 4.0 * std::fabs((double)tension));
    return tension > 0.0f ? std::pow(x, p) : 1.0 - std::pow(1.0 - x, p);
}
static int countFor(float tension) { return 2 + (int)std::lround(((double)tension + 1.0) * 7.0); }     // 2..16

float autoSegment(float a, float b, double x, int shape, float tension) {
    x = clampT(x, 0.0, 1.0);
    switch (shape) {
        case ShapeDouble: {         // an S: two curves mirrored about the middle
            const double y = x < 0.5 ? 0.5 * bend(2.0 * x, tension) : 1.0 - 0.5 * bend(2.0 * (1.0 - x), tension);
            return a + (b - a) * (float)y;
        }
        case ShapeHold: return a;
        case ShapeStairs: {
            const int n = countFor(tension);
            const double step = std::min((double)n - 1.0, std::floor(x * n));
            return a + (b - a) * (float)(step / (double)(n - 1));
        }
        case ShapePulse: {          // alternates between the two values, starting on the first
            const int n = countFor(tension);
            return ((int)std::floor(x * n) & 1) ? b : a;
        }
        case ShapeWave: {           // swings between the two values and arrives on the second
            const double cycles = (double)countFor(tension) - 1.5;
            return a + (b - a) * (float)(0.5 - 0.5 * std::cos(kTwoPi * cycles * x));
        }
        default: return a + (b - a) * (float)bend(x, tension);
    }
}

static float evalPoints(const std::vector<AutoPoint>& pts, double tick) {
    if (pts.empty()) return 0.5f;
    if (tick <= (double)pts.front().tick) return pts.front().value;
    if (tick >= (double)pts.back().tick) return pts.back().value;
    // the last point at or before the tick
    size_t lo = 0, hi = pts.size() - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if ((double)pts[mid].tick <= tick) lo = mid; else hi = mid;
    }
    const AutoPoint& p = pts[lo];
    const AutoPoint& q = pts[lo + 1];
    const double span = (double)(q.tick - p.tick);
    return autoSegment(p.value, q.value, span > 0.0 ? (tick - (double)p.tick) / span : 1.0, p.shape, p.tension);
}

float evalAuto(const std::vector<AutoPoint>& points, const AutoLfo& lfo, double tick) {
    float v = evalPoints(points, tick);
    if (lfo.on && lfo.periodSteps > 0.0f) {
        double ph = tick / ((double)lfo.periodSteps * kTicksPerStep) + (double)lfo.phase;
        ph -= std::floor(ph);
        double w;
        switch (lfo.shape) {
            case 1: w = ph < 0.25 ? 4.0 * ph : (ph < 0.75 ? 2.0 - 4.0 * ph : 4.0 * ph - 4.0); break;
            case 2: w = ph < 0.5 ? 1.0 : -1.0; break;
            case 3: w = 2.0 * ph - 1.0; break;
            default: w = std::sin(kTwoPi * ph); break;
        }
        v += lfo.depth * (float)w;
    }
    return clampT(v, 0.0f, 1.0f);
}

void sortEvents(std::vector<AutoEvent>& events) {
    std::stable_sort(events.begin(), events.end(), [](const AutoEvent& a, const AutoEvent& b) { return a.tick < b.tick; });
}

float evalEvents(const std::vector<AutoEvent>& ev, double tick) {
    if (ev.empty()) return 0.0f;
    if (tick < (double)ev.front().tick) return ev.front().value;
    size_t lo = 0, hi = ev.size();
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if ((double)ev[mid].tick <= tick) lo = mid; else hi = mid;
    }
    const AutoEvent& e = ev[lo];
    if (!e.ramp || lo + 1 >= ev.size()) return e.value;
    const AutoEvent& n = ev[lo + 1];
    const double span = (double)(n.tick - e.tick);
    return span > 0.0 ? e.value + (n.value - e.value) * (float)((tick - (double)e.tick) / span) : n.value;
}

void simplifyEvents(std::vector<AutoEvent>& ev, float tolerance) {
    if (ev.size() < 3) return;
    // Ramer-Douglas-Peucker on (tick, value)
    std::vector<char> keep(ev.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<size_t, size_t>> stack = {{0, ev.size() - 1}};
    while (!stack.empty()) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        if (b <= a + 1) continue;
        float worst = 0.0f;
        size_t at = a;
        for (size_t i = a + 1; i < b; ++i) {
            const double t = (double)(ev[i].tick - ev[a].tick) / std::max(1.0, (double)(ev[b].tick - ev[a].tick));
            const float line = ev[a].value + (ev[b].value - ev[a].value) * (float)t;
            const float d = std::fabs(ev[i].value - line);
            if (d > worst) { worst = d; at = i; }
        }
        if (worst > tolerance) { keep[at] = 1; stack.push_back({a, at}); stack.push_back({at, b}); }
    }
    size_t w = 0;
    for (size_t i = 0; i < ev.size(); ++i) if (keep[i]) ev[w++] = ev[i];
    ev.resize(w);
}

float AutoTargetRt::valueAt(double tick) const {
    if (segments.empty()) return param ? param->getNorm() : 0.0f;
    // the stretch that started last, at or before this position
    size_t lo = 0, hi = segments.size();
    const AutoSegmentRt* s = nullptr;
    double local = 0.0;
    if ((double)segments.front().start > tick) { s = &segments.front(); local = (double)s->offset; }
    else {
        while (hi - lo > 1) {
            const size_t mid = (lo + hi) / 2;
            if ((double)segments[mid].start <= tick) lo = mid; else hi = mid;
        }
        s = &segments[lo];
        local = std::min(tick - (double)s->start, (double)s->length) + (double)s->offset;
    }
    const AutoSourceRt& src = *s->src;
    const float v = src.isEvents ? evalEvents(src.events, local) : evalAuto(src.points, src.lfo, local);
    return clampT(src.min + (src.max - src.min) * v, 0.0f, 1.0f);
}

} // namespace pulp
