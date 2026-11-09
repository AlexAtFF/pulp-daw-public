// Rewind: the incoming sound is recorded all the time and played back through two curves that repeat with
// the song: how far behind "now" the playback point is (in beats), and how loud it is.
//
// The time curve is followed exactly where it is continuous (the playback point speeds up, slows down, stops or
// runs backwards, like tape). Where it jumps, the old playback point carries on as it was and fades out while a
// new one fades in at the new place (the smoothing time). The place in the pattern comes from the song position
// alone, so a bar always sounds the same; what is read back is the real input of the last few seconds.
#include "pulp/Creative.h"

namespace pulp {

namespace {
const double kTickEps = 1.0e-6;     // a point on the grid counts as reached a millionth of a tick early, so
                                    // rounding in the song position cannot move a step by a sample
}

Rewind::Rewind() {
    pLength = addChoice("length", "Pattern length", {"1/2 bar", "1 bar", "2 bars", "4 bars"}, 1);
    pTime = addFloat("time", "Time amount", 0, 1, 1, "%");
    pVolume = addFloat("volume", "Volume amount", 0, 1, 1, "%");
    pSmooth = addFloat("smooth", "Smoothing", 0.5f, 50, 4, "ms", 2.0f);
    pMix = addFloat("mix", "Dry / wet", 0, 1, 1, "%");
    curves_.set(std::make_shared<Curves>());
}

double Rewind::patternBeats(int c) { return c <= 0 ? 2.0 : (c == 1 ? 4.0 : (c == 2 ? 8.0 : 16.0)); }

std::vector<ParamGroup> Rewind::paramGroups() const {
    return {{"Pattern", {"length", "time", "volume"}}, {"Output", {"smooth", "mix"}}};
}

double Rewind::tailSeconds() const {
    auto c = curves_.get();
    float reach = 0.0f;
    if (c) for (int i = 0; i < c->time.count; ++i) reach = std::max(reach, c->time.pts[i].value);
    return (double)reach * (double)pTime->get() + 0.1;
}

float Rewind::meter(int i) const {
    return i == 0 ? mPhase_.load(std::memory_order_relaxed) : (i == 1 ? mBehind_.load(std::memory_order_relaxed) : mVol_.load(std::memory_order_relaxed));
}

// ---- curves

Rewind::Curve Rewind::sanitize(const std::vector<Point>& pts, float lo, float hi) {
    std::vector<Point> p = pts;
    for (auto& q : p) {
        q.pos = std::isfinite(q.pos) ? clampT(q.pos, 0.0f, 1.0f) : 0.0f;
        q.value = std::isfinite(q.value) ? clampT(q.value, lo, hi) : lo;
        q.shape = clampT(q.shape, (int)Hold, (int)Smooth);
    }
    std::stable_sort(p.begin(), p.end(), [](const Point& a, const Point& b) { return a.pos < b.pos; });
    Curve c;
    c.count = (int)std::min<size_t>(p.size(), kMaxPoints);
    for (int i = 0; i < c.count; ++i) c.pts[i] = p[(size_t)i];
    return c;
}

void Rewind::setTimeCurve(const std::vector<Point>& pts) {
    auto next = std::make_shared<Curves>();
    if (auto now = curves_.get()) *next = *now;
    next->time = sanitize(pts, 0.0f, kMaxBeats);
    curves_.set(next);
}
void Rewind::setVolumeCurve(const std::vector<Point>& pts) {
    auto next = std::make_shared<Curves>();
    if (auto now = curves_.get()) *next = *now;
    next->volume = sanitize(pts, 0.0f, 1.0f);
    curves_.set(next);
}
std::vector<Rewind::Point> Rewind::timeCurve() const {
    auto c = curves_.get();
    return c ? std::vector<Point>(c->time.pts, c->time.pts + c->time.count) : std::vector<Point>();
}
std::vector<Rewind::Point> Rewind::volumeCurve() const {
    auto c = curves_.get();
    return c ? std::vector<Point>(c->volume.pts, c->volume.pts + c->volume.count) : std::vector<Point>();
}

// seg: the last point at or before x, -1 before the first. It is kept between calls as a place to start looking.
double Rewind::eval(const Curve& c, double x, double eps, int& seg, float def) {
    if (c.count == 0) { seg = -1; return def; }
    seg = clampT(seg, -1, c.count - 1);
    while (seg + 1 < c.count && (double)c.pts[seg + 1].pos <= x) ++seg;
    while (seg >= 0 && (double)c.pts[seg].pos > x) --seg;
    if (seg < 0) return c.pts[0].value;
    const Point& a = c.pts[seg];
    if (seg == c.count - 1 || a.shape == Hold) return a.value;      // the last point holds to the end of the pattern
    const Point& b = c.pts[seg + 1];
    double t = clampT((x - eps - (double)a.pos) / ((double)b.pos - (double)a.pos), 0.0, 1.0);
    if (a.shape == Smooth) t = t * t * (3.0 - 2.0 * t);
    return (double)a.value + ((double)b.value - (double)a.value) * t;
}
// The value a stretch of the curve arrives at, and the value the next one starts from: where they differ the curve jumps.
float Rewind::leftLimit(const Curve& c, int seg, float def) {
    if (c.count == 0) return def;
    if (seg < 0) return c.pts[0].value;
    seg = std::min(seg, c.count - 1);
    if (seg == c.count - 1 || c.pts[seg].shape == Hold) return c.pts[seg].value;
    return c.pts[seg + 1].value;
}
float Rewind::startValue(const Curve& c, int seg, float def) {
    if (c.count == 0) return def;
    return c.pts[clampT(seg, 0, c.count - 1)].value;
}

double Rewind::valueAt(const std::vector<Point>& pts, double phase, float def) {
    const Curve c = sanitize(pts, -1.0e9f, 1.0e9f);
    int seg = -1;
    return eval(c, phase, 0.0, seg, def);
}

Json Rewind::curveJson(const Curve& c) {
    Json a = Json::array();
    for (int i = 0; i < c.count; ++i) {
        Json p = Json::array();
        p.push(c.pts[i].pos); p.push(c.pts[i].value); p.push(c.pts[i].shape);
        a.push(p);
    }
    return a;
}
std::vector<Rewind::Point> Rewind::curveFromJson(const Json& j) {
    std::vector<Point> out;
    for (auto& e : j.elements()) {
        if (e.size() < 2) continue;
        Point p;
        p.pos = e.at(0).f(); p.value = e.at(1).f(); p.shape = e.at(2).i(Hold);
        out.push_back(p);
    }
    return out;
}

Json Rewind::saveExtra() const {
    Json j = Json::object();
    auto c = curves_.get();
    j.set("time", curveJson(c ? c->time : Curve()));
    j.set("volume", curveJson(c ? c->volume : Curve()));
    return j;
}
// A state with no curves in it (an old file, a bare parameter list) means flat curves: nothing happens.
void Rewind::loadExtra(const Json& j, const std::string&) {
    auto next = std::make_shared<Curves>();
    next->time = sanitize(curveFromJson(j["time"]), 0.0f, kMaxBeats);
    next->volume = sanitize(curveFromJson(j["volume"]), 0.0f, 1.0f);
    curves_.set(next);
}

// ---- audio

void Rewind::prepare(double sr) {
    sr_ = sr;
    tape_.prepare(17.0, sr);        // sixteen beats at 60 BPM, and a little
    reset();
}

void Rewind::reset() {
    tape_.reset();
    for (auto& h : heads_) h = Head();
    cur_ = 0;
    segT_ = segV_ = -1;
    lastLen_ = -1;
    seen_ = nullptr;
    vol_ = 1.0f;
    primed_ = false;
    wasPlaying_ = false;
    stopLeft_ = 0;
    nextTick_ = 0.0;
    lastX_ = 0.0;
}

void Rewind::process(ProcessContext& c) {
    const Curves* cv = curves_.rtAcquire();
    if (!cv) cv = &empty_;
    const int n = c.numSamples;
    float* L = c.io[0];
    float* R = c.io[1];
    const bool playing = c.playing;

    // Stopped: the input passes untouched. If the song has only just stopped, the effect fades out first.
    if (!playing && wasPlaying_ && primed_) { stopLen_ = std::max(8, (int)(0.005 * sr_)); stopLeft_ = stopLen_; }
    if (!playing && stopLeft_ <= 0) {
        wasPlaying_ = false;
        primed_ = false;
        mBehind_.store(0.0f, std::memory_order_relaxed);
        mVol_.store(1.0f, std::memory_order_relaxed);
        return;
    }
    const double tps = c.tempo * kPPQ / (60.0 * c.sampleRate);
    bool force = false;
    double tick0;
    if (playing) {
        // Playback starts with an empty tape, exactly as an export from this place does.
        if (!wasPlaying_) { tape_.forget(); primed_ = false; stopLeft_ = 0; }
        tick0 = c.tickPos;
        if (primed_ && std::fabs(tick0 - nextTick_) > 0.25) force = true;      // the song jumped (a seek, a loop)
    } else {
        tick0 = nextTick_;          // fading out after a stop: the pattern runs on for those few milliseconds
    }
    nextTick_ = tick0 + n * tps;
    wasPlaying_ = playing;

    const int lenChoice = pLength->getInt();
    const double patTicks = patternBeats(lenChoice) * kPPQ;
    const double eps = kTickEps / patTicks;
    const double spbT = 60.0 * sr_ / std::max(1.0, c.tempo);
    const float fadeStep = (float)(1.0 / std::max(8.0, (double)pSmooth->get() * 0.001 * sr_));
    const double maxDelay = tape_.maxDelay();
    bool first = false;
    if (!primed_) {
        spb_ = spbT;
        timeAmt_.snap(pTime->get()); volAmt_.snap(pVolume->get()); mix_.snap(pMix->get());
        for (auto& h : heads_) h = Head();
        cur_ = 0;
        heads_[0].on = true;
        segT_ = segV_ = -1;
        primed_ = true;
        first = true;
    }
    if (cv != seen_ || lenChoice != lastLen_) force = true;
    seen_ = cv;
    lastLen_ = lenChoice;
    timeAmt_.target(pTime->get(), n);
    volAmt_.target(pVolume->get(), n);
    mix_.target(pMix->get(), n);
    const double spbStep = (spbT - spb_) / n;
    double x = 0.0, behind = 0.0;

    for (int i = 0; i < n; ++i) {
        const float dryL = L[i], dryR = R[i];
        tape_.push(dryL, dryR);
        const double tick = tick0 + i * tps;
        x = std::fmod(tick + kTickEps, patTicks);
        if (x < 0.0) x += patTicks;
        x /= patTicks;

        // where the time curve wants the playback point
        const int prevSeg = segT_;
        const double tv = eval(cv->time, x, eps, segT_, 0.0f);
        bool jumpHere = force || first;
        if (!first && (segT_ != prevSeg || x < lastX_))        // passed a point, or the pattern came round
            jumpHere = jumpHere || std::fabs(startValue(cv->time, segT_, 0.0f) - leftLimit(cv->time, prevSeg, 0.0f)) > 1.0e-7f;
        lastX_ = x;
        spb_ += spbStep;
        const double D = std::min(tv * (double)timeAmt_.next() * spb_, maxDelay);
        behind = D;

        float sumFading = 0.0f;
        bool anyFading = false;
        {
            Head& h = heads_[cur_];
            bool jump = false;
            if (first) { h.delay = D; h.slope = 0.0; }
            else {
                const double dev = std::fabs(D - (h.delay + h.slope));
                jump = (jumpHere && dev > 1.0e-6) || std::fabs(D - h.delay) > 8.0;
            }
            if (jump) {
                float gainNow = 1.0f;
                for (int k = 0; k < kHeads; ++k) if (k != cur_ && heads_[k].on) gainNow -= heads_[k].gain;
                h.gain = std::max(0.0f, gainNow);
                h.slope = clampT(h.slope, -8.0, 8.0);
                int pick = -1;
                for (int k = 0; k < kHeads; ++k) if (k != cur_ && !heads_[k].on) { pick = k; break; }
                if (pick < 0) {                 // every head is busy: the quietest one gives way
                    float quiet = 2.0f;
                    for (int k = 0; k < kHeads; ++k) if (k != cur_ && heads_[k].gain < quiet) { quiet = heads_[k].gain; pick = k; }
                }
                cur_ = pick;
                heads_[cur_].on = true;
                heads_[cur_].delay = D;
                heads_[cur_].slope = 0.0;
                heads_[cur_].gain = 0.0f;
            } else if (!first) {
                h.slope = D - h.delay;
                h.delay = D;
            }
            for (int k = 0; k < kHeads; ++k) {
                if (k == cur_ || !heads_[k].on) continue;
                Head& f = heads_[k];
                f.gain -= fadeStep;
                if (f.gain <= 0.0f) { f.on = false; f.gain = 0.0f; continue; }
                f.delay = clampT(f.delay + f.slope, 0.0, maxDelay);
                sumFading += f.gain;
                anyFading = true;
            }
        }

        float wl, wr;
        const Head& h = heads_[cur_];
        tape_.read(h.delay, 1.0 - h.slope, wl, wr);
        if (anyFading) {
            const float g = std::max(0.0f, 1.0f - sumFading);
            wl *= g; wr *= g;
            for (int k = 0; k < kHeads; ++k) {
                if (k == cur_ || !heads_[k].on) continue;
                float fl, fr;
                tape_.read(heads_[k].delay, 1.0 - heads_[k].slope, fl, fr);
                wl += fl * heads_[k].gain; wr += fr * heads_[k].gain;
            }
        }

        // the volume curve, with its jumps turned into short ramps
        const float vv = clampT((float)eval(cv->volume, x, eps, segV_, 1.0f), 0.0f, 1.0f);
        if (first) vol_ = vv;
        else vol_ += clampT(vv - vol_, -fadeStep, fadeStep);
        const float g = 1.0f - volAmt_.next() * (1.0f - vol_);
        wl *= g; wr *= g;

        float m = mix_.next();
        if (!playing) {
            m = stopLeft_ > 0 ? m * (float)stopLeft_ / (float)stopLen_ : 0.0f;
            if (stopLeft_ > 0) --stopLeft_;
        }
        L[i] = crt::mixDryWet(dryL, wl, m);
        R[i] = crt::mixDryWet(dryR, wr, m);
        first = false;
        force = false;
    }
    mPhase_.store((float)x, std::memory_order_relaxed);
    spb_ = spbT;
    mBehind_.store((float)(behind / std::max(1.0, spb_)), std::memory_order_relaxed);
    mVol_.store(vol_, std::memory_order_relaxed);
}

// ---- presets

namespace {
using Pt = Rewind::Point;
const int H = Rewind::Hold, Ln = Rewind::Line, S = Rewind::Smooth;

// Steps of `stepBeats` further back every `stepPhase`, from `from`: each step plays the same slice again.
std::vector<Pt> stairs(float from, float stepPhase, int count, float stepBeats) {
    std::vector<Pt> p = {{0.0f, 0.0f, H}};
    for (int k = 0; k < count; ++k) p.push_back({from + stepPhase * (float)k, stepBeats * (float)k, H});
    return p;
}
// A tape slowing to a stop over `beats`, ending at `to`: the delay grows with the square of the time.
std::vector<Pt> tapeStop(float from, float to, float beats) {
    std::vector<Pt> p = {{0.0f, 0.0f, H}};
    const int steps = 10;
    for (int k = 0; k <= steps; ++k) {
        const float u = (float)k / (float)steps;
        p.push_back({from + (to - from) * u, 0.5f * u * u * beats, k == steps ? H : Ln});
    }
    return p;
}
// A gate from a string: 'x' open, '.' closed, one character per step; an open step closes after `duty` of its length.
std::vector<Pt> gate(const char* pattern, float duty, float from = 0.0f, float to = 1.0f) {
    std::vector<Pt> p;
    const int steps = (int)std::strlen(pattern);
    if (from > 0.0f) p.push_back({0.0f, 1.0f, H});
    for (int k = 0; k < steps && (int)p.size() + 2 <= Rewind::kMaxPoints; ++k) {
        const float a = from + (to - from) * (float)k / (float)steps, len = (to - from) / (float)steps;
        const bool open = pattern[k] == 'x';
        const bool wasOpen = !p.empty() && p.back().value > 0.5f;
        if (open) {
            if (!wasOpen || p.empty()) p.push_back({a, 1.0f, H});
            if (duty < 1.0f) p.push_back({a + len * duty, 0.0f, H});
        } else if (wasOpen || p.empty()) p.push_back({a, 0.0f, H});
    }
    return p;
}
// A dip at the start of every `beatPhase`, recovering over `recover` of it: the pumping of a ducked pad.
std::vector<Pt> pump(float beatPhase, float floor, float recover) {
    std::vector<Pt> p;
    for (float a = 0.0f; a < 0.999f; a += beatPhase) {
        p.push_back({a, floor, S});
        p.push_back({a + beatPhase * recover, 1.0f, H});
    }
    return p;
}

struct RewindPreset {
    const char* name;
    std::vector<PresetValue> values;
    std::vector<Pt> time, volume;
};
}

std::vector<Preset> Rewind::factoryPresets() const {
    const std::vector<RewindPreset> defs = {
        {"Nothing", {}, {}, {}},
        {"Half speed", {}, {{0, 0, Ln}, {1, 2, H}}, {}},
        {"Half speed, two beats at a time", {{"length", 0}}, {{0, 0, Ln}, {1, 1, H}}, {}},
        {"Slow down and catch up", {{"smooth", 8}}, {{0, 0, Ln}, {0.5f, 1, Ln}, {1, 0, H}}, {}},
        {"Tape stop at the end of the bar", {}, tapeStop(0.75f, 1.0f, 1.0f), {{0, 1, H}, {0.9375f, 1, Ln}, {1, 0, H}}},
        {"Tape stop over two beats", {}, tapeStop(0.5f, 1.0f, 2.0f), {{0, 1, H}, {0.875f, 1, Ln}, {1, 0, H}}},
        {"Stutter eighths", {{"smooth", 2}}, stairs(0.5f, 0.125f, 4, 0.5f), {}},
        {"Stutter sixteenths", {{"smooth", 2}}, stairs(0.75f, 0.0625f, 4, 0.25f), {}},
        {"Stutter triplets", {{"smooth", 2}}, stairs(0.5f, 1.0f / 12.0f, 6, 1.0f / 3.0f), {}},
        {"Stutter build", {{"smooth", 1.5f}},
            {{0, 0, H}, {0.5f, 0, H}, {0.625f, 0.5f, H}, {0.75f, 1.0f, H}, {0.8125f, 1.25f, H},
             {0.875f, 1.5f, H}, {0.90625f, 1.625f, H}, {0.9375f, 1.75f, H}, {0.96875f, 1.875f, H}},
            {}},
        {"Stuck on the first eighth", {{"smooth", 2}}, stairs(0.0f, 0.125f, 8, 0.5f), {}},
        {"Reverse swell into the bar", {}, {{0, 0, H}, {0.75f, 0, Ln}, {1, 2, H}}, {{0, 1, H}, {0.75f, 0.15f, S}, {1, 1, H}}},
        {"Reverse every second beat", {}, {{0, 0, H}, {0.25f, 0, Ln}, {0.5f, 2, H}, {0.5f, 0, H}, {0.75f, 0, Ln}, {1, 2, H}}, {}},
        {"Trance gate, straight", {{"smooth", 3}}, {}, gate("x.xxx.xxx.x.xx.x", 1.0f)},
        {"Trance gate, offbeat", {{"smooth", 3}}, {}, gate("..x...x...x.x.xx", 1.0f)},
        {"Trance gate, triplets", {{"smooth", 3}}, {}, gate("xx.xx.x.xxx.", 1.0f)},
        {"Trance gate, short sixteenths", {{"smooth", 2}, {"length", 0}}, {}, gate("xxxxxxxx", 0.5f)},
        {"Pump on every beat", {{"smooth", 6}}, {}, pump(0.25f, 0.08f, 0.7f)},
        {"Light pump on eighths", {{"smooth", 6}, {"volume", 0.6f}}, {}, pump(0.125f, 0.2f, 0.8f)},
        {"Gated last beat", {{"smooth", 2}}, {}, gate("xxxxxxxx", 0.5f, 0.75f, 1.0f)},
        {"Scratch wiggle on the last beat", {{"smooth", 3}},
            {{0, 0, H}, {0.75f, 0, S}, {0.8125f, 0.45f, S}, {0.875f, 0.1f, S}, {0.9375f, 0.5f, S}, {1, 0, H}}, {}},
        {"Scratch the second beat", {{"smooth", 3}},
            {{0, 0, H}, {0.25f, 0, S}, {0.3125f, 0.4f, S}, {0.375f, 0.05f, S}, {0.4375f, 0.4f, S}, {0.5f, 0, H}}, {}},
        {"Half speed with a gate", {{"smooth", 3}}, {{0, 0, Ln}, {1, 2, H}}, gate("xxx.xxx.xx.xxx.x", 1.0f)},
        {"Fade in over four bars", {{"length", 3}}, {}, {{0, 0, Ln}, {1, 1, H}}},
    };
    std::vector<PresetDef> plain;
    for (auto& d : defs) plain.push_back({d.name, d.values});
    auto out = buildPresets(*this, plain);
    for (size_t i = 0; i < out.size(); ++i) {
        Json ex = Json::object();
        ex.set("time", curveJson(sanitize(defs[i].time, 0.0f, kMaxBeats)));
        ex.set("volume", curveJson(sanitize(defs[i].volume, 0.0f, 1.0f)));
        out[i].state.set("extra", ex);
    }
    return out;
}

} // namespace pulp
