// Bend (waveshaper with a drawable curve) and Middle and Sides.
#include "pulp/Tone.h"

namespace pulp {

using tonefx::dbGain;
using tonefx::OsGate;

// ------------------------------------------------------------------ Bend

namespace {

double sign(double v) { return v > 0.0 ? 1.0 : (v < 0.0 ? -1.0 : 0.0); }

// Slope at the first or last point: the three-point estimate, held back so the curve cannot overshoot.
double endSlope(double h0, double h1, double d0, double d1) {
    double m = ((2.0 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
    if (sign(m) != sign(d0)) m = 0.0;
    else if (sign(d0) != sign(d1) && std::fabs(m) > 3.0 * std::fabs(d0)) m = 3.0 * d0;
    return m;
}

Bend::Points tidy(Bend::Points pts) {
    Bend::Points out;
    for (auto& p : pts) {
        if (!std::isfinite(p.first) || !std::isfinite(p.second)) continue;
        out.push_back({clampT(p.first, -1.0f, 1.0f), clampT(p.second, -1.0f, 1.0f)});
    }
    std::stable_sort(out.begin(), out.end(), [](const std::pair<float, float>& a, const std::pair<float, float>& b) { return a.first < b.first; });
    Bend::Points merged;
    for (auto& p : out) {
        if (!merged.empty() && p.first - merged.back().first < 1.0e-4f) merged.back().second = p.second;    // two points on one x: the later one counts
        else merged.push_back(p);
    }
    if ((int)merged.size() > Bend::kMaxPoints) merged.resize((size_t)Bend::kMaxPoints);
    if (merged.size() < 2) return Bend::straightLine();
    return merged;
}

struct BendPreset { const char* name; std::vector<PresetValue> values; Bend::Points curve; };

} // namespace

double Bend::Curve::at(double v) const {
    if (v <= x[0]) return y[0];
    if (v >= x[n - 1]) return y[n - 1];
    int i = cell[clampT((int)((v + 1.0) * 32.0), 0, 63)];
    while (i < n - 2 && v >= x[i + 1]) ++i;
    const double dx = v - x[i];
    return y[i] + dx * (m[i] + dx * (c2[i] + dx * c3[i]));
}

// Monotone cubic through the points: smooth, and never past a point's level on the way to the next.
std::shared_ptr<const Bend::Curve> Bend::build(const Points& pts) {
    auto cv = std::make_shared<Curve>();
    Curve& k = *cv;
    const int n = (int)pts.size();
    k.n = n;
    for (int i = 0; i < n; ++i) { k.x[i] = pts[(size_t)i].first; k.y[i] = pts[(size_t)i].second; }
    double h[kMaxPoints] = {}, d[kMaxPoints] = {};
    for (int i = 0; i + 1 < n; ++i) { h[i] = k.x[i + 1] - k.x[i]; d[i] = (k.y[i + 1] - k.y[i]) / h[i]; }
    if (n == 2) k.m[0] = k.m[1] = d[0];
    else {
        for (int i = 1; i + 1 < n; ++i) {
            if (d[i - 1] * d[i] <= 0.0) k.m[i] = 0.0;
            else {
                const double w1 = 2.0 * h[i] + h[i - 1], w2 = h[i] + 2.0 * h[i - 1];
                k.m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i]);
            }
        }
        k.m[0] = endSlope(h[0], h[1], d[0], d[1]);
        k.m[n - 1] = endSlope(h[n - 2], h[n - 3], d[n - 2], d[n - 3]);
    }
    for (int i = 0; i + 1 < n; ++i) {
        k.c2[i] = (3.0 * d[i] - 2.0 * k.m[i] - k.m[i + 1]) / h[i];
        k.c3[i] = (k.m[i] + k.m[i + 1] - 2.0 * d[i]) / (h[i] * h[i]);
    }
    for (int c = 0; c < 64; ++c) {
        const double start = -1.0 + (double)c / 32.0;
        int seg = 0;
        while (seg < n - 2 && k.x[seg + 1] <= start) ++seg;
        k.cell[c] = (unsigned char)seg;
    }
    return cv;
}

Bend::Bend() {
    pPre = addFloat("pre", "Gain in", -24.0f, 36.0f, 0.0f, "dB");
    pPost = addFloat("post", "Gain out", -36.0f, 12.0f, 0.0f, "dB");
    pSymmetry = addChoice("symmetry", "Curve covers", {"One polarity, mirrored", "Both polarities as drawn"}, 1);
    pOver = addChoice("over", "Oversampling", {"Off", "2x", "4x", "8x"}, 2);
    pMix = addFloat("mix", "Dry / wet", 0.0f, 1.0f, 1.0f, "%");
    tab_[0] = tonefx::osTables(2, 0);
    tab_[1] = tonefx::osTables(4, 0);
    tab_[2] = tonefx::osTables(8, 0);
    for (auto& o : os_) o.setTables(tab_[1]);
    setCurve(straightLine());
}

void Bend::setCurve(Points points) {
    points = tidy(std::move(points));
    auto cv = build(points);
    {
        std::lock_guard<std::mutex> g(m_);
        points_ = points;
    }
    curve_.set(std::move(cv));
}

Bend::Points Bend::curve() const {
    std::lock_guard<std::mutex> g(m_);
    return points_;
}

double Bend::transfer(double x) const {
    const auto cv = curve_.get();
    return shape(*cv, pSymmetry->getInt(), cv->at(0.0), x);
}

Json Bend::saveExtra() const {
    Json arr = Json::array();
    for (auto& p : curve()) {
        Json pt = Json::array();
        pt.push(p.first);
        pt.push(p.second);
        arr.push(pt);
    }
    Json j = Json::object();
    j.set("curve", arr);
    return j;
}

void Bend::loadExtra(const Json& j, const std::string&) {
    const Json& arr = j["curve"];
    if (!arr.isArray()) { setCurve(straightLine()); return; }
    Points pts;
    for (auto& e : arr.elements())
        if (e.isArray() && e.size() >= 2) pts.push_back({e.at(0).f(), e.at(1).f()});
    setCurve(std::move(pts));
}

void Bend::prepare(double sr) { sr_ = sr; reset(); }

void Bend::reset() {
    for (auto& o : os_) o.reset();
    gate_.reset();
    dly_.reset();
    dip_.reset();
    factor_ = 0;
    primed_ = false;
}

void Bend::process(ProcessContext& c) {
    const int n = c.numSamples;
    const Curve& cv = *curve_.rtAcquire();
    const float preT = dbGain(pPre->get()), postT = dbGain(pPost->get()), mixT = pMix->get();
    const bool first = !primed_;
    if (first) { pre_.snap(preT); post_.snap(postT); mix_.snap(mixT); primed_ = true; }
    pre_.target(preT, n);
    post_.target(postT, n);
    mix_.target(mixT, n);
    dip_.begin(pSymmetry->getInt(), n);
    const int symmetry = dip_.active;
    const double zero = cv.at(0.0);     // "as drawn" is shifted so that nothing in gives nothing out

    // Oversampling: off, or 2, 4 or 8 times. A change of factor goes through "off" so that it never clicks.
    static const int factors[4] = {0, 2, 4, 8};
    const int wantFactor = factors[clampT(pOver->getInt(), 0, 3)];
    bool run, started = false;
    if (first) {
        run = started = wantFactor != 0;
        if (run) gate_.startOn(); else gate_.reset();
    } else {
        run = gate_.begin(wantFactor != 0 && (gate_.state == OsGate::Off || wantFactor == factor_), n, started);
    }
    if (started) {
        factor_ = wantFactor;
        for (auto& o : os_) { o.setTables(tab_[factor_ == 2 ? 0 : (factor_ == 8 ? 2 : 1)]); o.reset(); }
    }
    const int R = factor_;
    const bool blend = !(run && gate_.state == OsGate::On && gate_.mix.cur >= 1.0f && gate_.mix.left == 0 && gate_.mix.tgt >= 1.0f);

    for (int i = 0; i < n; ++i) {
        const double pre = pre_.next();
        const float post = post_.next(), mix = mix_.next(), dipG = dip_.gain.next();
        const float osMix = run ? gate_.mix.next() : 0.0f;
        const float x[2] = {c.io[0][i], c.io[1][i]};
        float dry[2];
        dly_.push(x[0], x[1], dry[0], dry[1]);
        for (int ch = 0; ch < 2; ++ch) {
            float wet = blend ? (float)shape(cv, symmetry, zero, pre * (double)dry[ch]) : 0.0f;
            if (run) {
                float f[tonefx::Oversampler::kMaxFactor];
                os_[ch].up(x[ch], f);
                for (int p = 0; p < R; ++p) f[p] = (float)shape(cv, symmetry, zero, pre * (double)f[p]);
                const float y = os_[ch].down(f);
                wet = blend ? wet + (y - wet) * osMix : y;
            }
            c.io[ch][i] = dry[ch] + (wet * post * dipG - dry[ch]) * mix;
        }
    }
}

std::vector<Preset> Bend::factoryPresets() const {
    Points sine;
    for (int i = 0; i <= 12; ++i) { const double x = -1.0 + (double)i / 6.0; sine.push_back({(float)x, (float)(0.9 * std::sin(1.5 * kPi * x))}); }
    const std::vector<BendPreset> defs = {
        {"Straight line", {}, straightLine()},
        {"Soft knee", {{"symmetry", 0}, {"pre", 6}, {"post", 0}}, {{0.0f, 0.0f}, {0.3f, 0.3f}, {0.6f, 0.55f}, {0.8f, 0.68f}, {1.0f, 0.75f}}},
        {"Flat ceiling", {{"symmetry", 0}, {"pre", 9}, {"post", 3}}, {{0.0f, 0.0f}, {0.45f, 0.45f}, {0.55f, 0.5f}, {1.0f, 0.5f}}},
        {"Fold back", {{"symmetry", 0}, {"pre", 6}, {"post", -3}, {"over", 3}}, {{0.0f, 0.0f}, {0.4f, 0.8f}, {0.7f, 0.2f}, {1.0f, 0.9f}}},
        {"Half wave", {{"symmetry", 1}, {"pre", 3}}, {{-1.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 1.0f}}},
        {"Staircase", {{"symmetry", 0}, {"pre", 3}, {"post", -2}, {"over", 3}},
         {{0.0f, 0.0f}, {0.12f, 0.02f}, {0.2f, 0.3f}, {0.42f, 0.32f}, {0.5f, 0.6f}, {0.72f, 0.62f}, {0.8f, 0.9f}, {1.0f, 0.92f}}},
        {"Crossover gap", {{"symmetry", 0}}, {{0.0f, 0.0f}, {0.15f, 0.0f}, {0.4f, 0.3f}, {1.0f, 1.0f}}},
        {"Lopsided warmth", {{"symmetry", 1}, {"pre", 6}, {"post", -1}}, {{-1.0f, -0.6f}, {-0.5f, -0.42f}, {0.0f, 0.0f}, {0.4f, 0.45f}, {0.75f, 0.8f}, {1.0f, 0.9f}}},
        {"Sine fold", {{"symmetry", 1}, {"pre", 0}, {"post", -2}, {"over", 3}}, sine},
        {"Quiet parts down", {{"symmetry", 0}}, {{0.0f, 0.0f}, {0.5f, 0.15f}, {0.8f, 0.5f}, {1.0f, 1.0f}}},
    };
    std::vector<Preset> out;
    for (auto& d : defs) {
        Preset p = buildPresets(*this, {{d.name, d.values}})[0];
        Json arr = Json::array();
        for (auto& pt : tidy(d.curve)) {
            Json one = Json::array();
            one.push(pt.first);
            one.push(pt.second);
            arr.push(one);
        }
        Json extra = Json::object();
        extra.set("curve", arr);
        p.state.set("extra", extra);
        out.push_back(p);
    }
    return out;
}

// ------------------------------------------------------------------ Middle and Sides

MiddleSides::MiddleSides() {
    pMidLevel = addFloat("mlevel", "Middle level (lowest = off)", kLevelOff, 12.0f, 0.0f, "dB");
    pMidLow = addFloat("mlow", "Middle low shelf", -12.0f, 12.0f, 0.0f, "dB");
    pMidLowHz = addFloat("mlowhz", "Middle low shelf corner", 40.0f, 800.0f, 200.0f, "Hz", 3.0f);
    pMidHigh = addFloat("mhigh", "Middle high shelf", -12.0f, 12.0f, 0.0f, "dB");
    pMidHighHz = addFloat("mhighhz", "Middle high shelf corner", 1000.0f, 16000.0f, 5000.0f, "Hz", 3.0f);
    pSideLevel = addFloat("slevel", "Sides level (lowest = off)", kLevelOff, 12.0f, 0.0f, "dB");
    pSideLow = addFloat("slow", "Sides low shelf", -12.0f, 12.0f, 0.0f, "dB");
    pSideLowHz = addFloat("slowhz", "Sides low shelf corner", 40.0f, 800.0f, 200.0f, "Hz", 3.0f);
    pSideHigh = addFloat("shigh", "Sides high shelf", -12.0f, 12.0f, 0.0f, "dB");
    pSideHighHz = addFloat("shighhz", "Sides high shelf corner", 1000.0f, 16000.0f, 5000.0f, "Hz", 3.0f);
    pWidth = addFloat("width", "Width", 0.0f, 2.0f, 1.0f, "%");
    pBalance = addFloat("balance", "Balance", -1.0f, 1.0f, 0.0f);
    pOut = addFloat("out", "Output", -24.0f, 12.0f, 0.0f, "dB");
}

void MiddleSides::prepare(double sr) { sr_ = sr; reset(); }

void MiddleSides::reset() {
    for (auto& s : shelf_) { s.st.reset(); s.active = false; s.seenDb = 0.0f; s.seenHz = -1.0f; }
    primed_ = false;
}

void MiddleSides::updateShelf(Shelf& s, bool high, float db, float hz, int n) {
    if (primed_ && db == s.seenDb && hz == s.seenHz) return;
    s.seenDb = db;
    s.seenHz = hz;
    dsp::Biquad q;
    const double corner = std::min((double)hz, 0.45 * sr_);
    if (!primed_) {
        if (high) q.highShelf(corner, 0.7071, db, sr_); else q.lowShelf(corner, 0.7071, db, sr_);
        s.co.snap(q);
        s.st.reset();
        s.active = db != 0.0f;
        return;
    }
    if (!s.active) {
        if (db == 0.0f) return;
        if (high) q.highShelf(corner, 0.7071, 0.0, sr_); else q.lowShelf(corner, 0.7071, 0.0, sr_);
        s.co.snap(q);
        s.st.reset();
        s.active = true;
    }
    if (high) q.highShelf(corner, 0.7071, db, sr_); else q.lowShelf(corner, 0.7071, db, sr_);
    s.co.glide(q, n);
}

void MiddleSides::process(ProcessContext& c) {
    const int n = c.numSamples;
    const float midT = pMidLevel->get() <= kLevelOff + 0.01f ? 0.0f : dbGain(pMidLevel->get());
    const float sideT = (pSideLevel->get() <= kLevelOff + 0.01f ? 0.0f : dbGain(pSideLevel->get())) * pWidth->get();
    const float out = dbGain(pOut->get()), bal = pBalance->get();
    const float glT = out * (bal > 0.0f ? 1.0f - bal : 1.0f), grT = out * (bal < 0.0f ? 1.0f + bal : 1.0f);
    updateShelf(shelf_[0], false, pMidLow->get(), pMidLowHz->get(), n);
    updateShelf(shelf_[1], true, pMidHigh->get(), pMidHighHz->get(), n);
    updateShelf(shelf_[2], false, pSideLow->get(), pSideLowHz->get(), n);
    updateShelf(shelf_[3], true, pSideHigh->get(), pSideHighHz->get(), n);
    if (!primed_) { mid_.snap(midT); side_.snap(sideT); gl_.snap(glT); gr_.snap(grT); primed_ = true; }
    mid_.target(midT, n);
    side_.target(sideT, n);
    gl_.target(glT, n);
    gr_.target(grT, n);

    for (int i = 0; i < n; ++i) {
        const double l = c.io[0][i], r = c.io[1][i];
        const double M = 0.5 * (l + r), S = 0.5 * (l - r);
        double m = M, s = S;
        for (int k = 0; k < 4; ++k) {
            Shelf& sh = shelf_[k];
            if (!sh.active) continue;
            sh.co.step();
            if (k < 2) m = sh.st.run(sh.co, m); else s = sh.st.run(sh.co, s);
        }
        // Worked out as what changes, added to the input: with everything at rest nothing changes and the input
        // passes bit for bit, and a mono input (no side at all) stays exactly mono.
        const double dM = m * (double)mid_.next() - M, dS = s * (double)side_.next() - S;
        c.io[0][i] = (float)(l + (dM + dS)) * gl_.next();
        c.io[1][i] = (float)(r + (dM - dS)) * gr_.next();
    }
    for (auto& sh : shelf_) {
        if (!sh.active) continue;
        sh.st.flush();
        if (sh.seenDb == 0.0f && !sh.co.moving() && sh.st.quiet()) { sh.active = false; sh.st.reset(); }
    }
}

double MiddleSides::midDb(double hz) const {
    if (pMidLevel->get() <= kLevelOff + 0.01f) return -144.0;
    dsp::Biquad lo, hi;
    lo.lowShelf(std::min((double)pMidLowHz->get(), 0.45 * sr_), 0.7071, pMidLow->get(), sr_);
    hi.highShelf(std::min((double)pMidHighHz->get(), 0.45 * sr_), 0.7071, pMidHigh->get(), sr_);
    return (double)pMidLevel->get() + 20.0 * std::log10(lo.magnitude(hz, sr_) * hi.magnitude(hz, sr_));
}

double MiddleSides::sideDb(double hz) const {
    if (pSideLevel->get() <= kLevelOff + 0.01f || pWidth->get() <= 0.0f) return -144.0;
    dsp::Biquad lo, hi;
    lo.lowShelf(std::min((double)pSideLowHz->get(), 0.45 * sr_), 0.7071, pSideLow->get(), sr_);
    hi.highShelf(std::min((double)pSideHighHz->get(), 0.45 * sr_), 0.7071, pSideHigh->get(), sr_);
    return (double)pSideLevel->get() + 20.0 * std::log10((double)pWidth->get() * lo.magnitude(hz, sr_) * hi.magnitude(hz, sr_));
}

std::vector<Preset> MiddleSides::factoryPresets() const {
    return buildPresets(*this, {
        {"Unchanged", {}},
        {"Airy sides", {{"shigh", 4}, {"shighhz", 6000}, {"width", 1.2f}}},
        {"Tight low end", {{"slow", -12}, {"slowhz", 160}, {"mlow", 1.5f}, {"mlowhz", 90}}},
        {"Centre forward", {{"mlevel", 2}, {"mhigh", 2}, {"mhighhz", 3500}, {"slevel", -1.5f}}},
        {"Sides only", {{"mlevel", kLevelOff}}},
        {"Middle only", {{"slevel", kLevelOff}}},
        {"Room down", {{"slevel", -4}, {"slow", -3}, {"slowhz", 300}}},
        {"Warm centre, bright sides", {{"mlow", 2.5f}, {"mlowhz", 180}, {"mhigh", -1.5f}, {"shigh", 3}, {"shighhz", 7000}}},
    });
}

std::vector<ParamGroup> MiddleSides::paramGroups() const {
    return {
        {"Middle", {"mlevel", "mlow", "mlowhz", "mhigh", "mhighhz"}},
        {"Sides", {"slevel", "slow", "slowhz", "shigh", "shighhz"}},
        {"Output", {"width", "balance", "out"}},
    };
}

} // namespace pulp
