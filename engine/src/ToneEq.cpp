// Ten Band (graphic equaliser) and Motion Filter (filter with its own envelope follower and LFO).
#include "pulp/Tone.h"

namespace pulp {

using tonefx::dbGain;

// ------------------------------------------------------------------ Ten Band

TenBand::TenBand() {
    static const char* ids[kBands] = {"g31", "g62", "g125", "g250", "g500", "g1k", "g2k", "g4k", "g8k", "g16k"};
    static const char* names[kBands] = {"31 Hz", "62 Hz", "125 Hz", "250 Hz", "500 Hz", "1 kHz", "2 kHz", "4 kHz", "8 kHz", "16 kHz"};
    for (int b = 0; b < kBands; ++b) pGain[b] = addFloat(ids[b], names[b], -15.0f, 15.0f, 0.0f, "dB");
    pOut = addFloat("out", "Output", -24.0f, 12.0f, 0.0f, "dB");
    prepare(48000.0);
}

// A peaking filter whose width in octaves is the same at every centre frequency, also close to the top of the
// band, and the same for every gain: boost and cut are mirror images.
void TenBand::design(dsp::Biquad& b, int band, double db) const {
    const double A = std::exp(db * 0.057564627324851146);      // 10^(db/40)
    const double al = alpha_[band], c = cosw_[band];
    b.set(1.0 + al * A, -2.0 * c, 1.0 - al * A, 1.0 + al / A, -2.0 * c, 1.0 - al / A);
}

// Response in dB of one band's filter, set to `db`, at the centre of band `at`.
double TenBand::respDb(int band, double db, int at) const {
    const double A = std::exp(db * 0.057564627324851146);
    const double al = alpha_[band], b1 = -2.0 * cosw_[band];
    const double b0 = 1.0 + al * A, b2 = 1.0 - al * A, a0 = 1.0 + al / A, a2 = 1.0 - al / A;
    const double cw = cosw_[at], c2w = cos2w_[at];
    const double num = b0 * b0 + b1 * b1 + b2 * b2 + 2.0 * b1 * (b0 + b2) * cw + 2.0 * b0 * b2 * c2w;
    const double den = a0 * a0 + b1 * b1 + a2 * a2 + 2.0 * b1 * (a0 + a2) * cw + 2.0 * a0 * a2 * c2w;
    return 10.0 * std::log10(std::max(num, 1.0e-300) / std::max(den, 1.0e-300));
}

void TenBand::prepare(double sr) {
    sr_ = sr;
    // one octave and a bit: wide enough that neighbours set alike join up with under half a dB of ripple
    const double bw = (2.0 / std::log(2.0)) * std::asinh(1.0 / (2.0 * kQ));
    for (int b = 0; b < kBands; ++b) {
        const double w = kTwoPi * std::min(centreHz(b), sr * 0.45) / sr;
        cosw_[b] = std::cos(w);
        cos2w_[b] = std::cos(2.0 * w);
        alpha_[b] = std::sin(w) * std::sinh(std::log(2.0) * 0.5 * bw * w / std::sin(w));
    }
    // How much of each filter's gain reaches each band centre, per dB, and the inverse of that table.
    double a[kBands][2 * kBands];
    for (int i = 0; i < kBands; ++i)
        for (int j = 0; j < kBands; ++j) { a[i][j] = respDb(j, 6.0, i) / 6.0; a[i][kBands + j] = i == j ? 1.0 : 0.0; }
    for (int i = 0; i < kBands; ++i) {
        int piv = i;
        for (int r = i + 1; r < kBands; ++r) if (std::fabs(a[r][i]) > std::fabs(a[piv][i])) piv = r;
        if (piv != i) for (int k = 0; k < 2 * kBands; ++k) std::swap(a[i][k], a[piv][k]);
        const double d = a[i][i];
        for (int k = 0; k < 2 * kBands; ++k) a[i][k] /= d;
        for (int r = 0; r < kBands; ++r) {
            if (r == i) continue;
            const double f = a[r][i];
            if (f != 0.0) for (int k = 0; k < 2 * kBands; ++k) a[r][k] -= f * a[i][k];
        }
    }
    for (int i = 0; i < kBands; ++i) for (int j = 0; j < kBands; ++j) inv_[i][j] = a[i][kBands + j];
    slew_ = 1.0 - std::exp(-(double)kQuantum / (0.012 * sr));
    reset();
}

void TenBand::reset() {
    for (int b = 0; b < kBands; ++b) { st_[b][0].reset(); st_[b][1].reset(); active_[b] = false; cur_[b] = solved_[b] = fdb_[b] = 0.0; }
    primed_ = false;
}

// The filter gains that make the summed curve pass through the slider settings at the band centres.
// The table gives a first answer; a few rounds of correcting what is left make it exact.
void TenBand::solve(const double* sliders, double* g) const {
    for (int i = 0; i < kBands; ++i) {
        double s = 0.0;
        for (int j = 0; j < kBands; ++j) s += inv_[i][j] * sliders[j];
        g[i] = s;
    }
    for (int round = 0; round < 6; ++round) {
        double worst = 0.0, rest[kBands];
        for (int i = 0; i < kBands; ++i) g[i] = clampT(g[i], -kMaxFilterDb, kMaxFilterDb);
        for (int i = 0; i < kBands; ++i) {
            double at = 0.0;
            for (int j = 0; j < kBands; ++j) if (g[j] != 0.0) at += respDb(j, g[j], i);
            rest[i] = sliders[i] - at;
            worst = std::max(worst, std::fabs(rest[i]));
        }
        if (worst < 0.005) break;
        for (int i = 0; i < kBands; ++i) {
            double s = 0.0;
            for (int j = 0; j < kBands; ++j) s += inv_[i][j] * rest[j];
            g[i] += s;
        }
    }
    for (int i = 0; i < kBands; ++i) g[i] = clampT(g[i], -kMaxFilterDb, kMaxFilterDb);
}

void TenBand::filterGains(double* db) const {
    double sliders[kBands];
    for (int b = 0; b < kBands; ++b) sliders[b] = pGain[b]->get();
    solve(sliders, db);
}

double TenBand::magnitudeDb(double hz) const {
    double g[kBands];
    filterGains(g);
    double db = pOut->get();
    for (int b = 0; b < kBands; ++b) {
        if (g[b] == 0.0) continue;
        dsp::Biquad q;
        design(q, b, g[b]);
        db += 20.0 * std::log10(q.magnitude(hz, sr_));
    }
    return db;
}

void TenBand::process(ProcessContext& c) {
    const int n = c.numSamples;
    bool changed = !primed_;
    for (int b = 0; b < kBands; ++b) {
        const double t = pGain[b]->get();
        if (!primed_) cur_[b] = t;
        else if (cur_[b] != t) {
            // a slider that jumps is followed over a few milliseconds, so the filters never step
            cur_[b] += (t - cur_[b]) * slew_;
            if (std::fabs(t - cur_[b]) < 0.02) cur_[b] = t;
        }
        changed = changed || cur_[b] != solved_[b];
    }
    if (changed) {
        solve(cur_, fdb_);
        for (int b = 0; b < kBands; ++b) {
            solved_[b] = cur_[b];
            dsp::Biquad q;
            design(q, b, fdb_[b]);
            if (!primed_) { co_[b].snap(q); st_[b][0].reset(); st_[b][1].reset(); active_[b] = fdb_[b] != 0.0; continue; }
            if (!active_[b] && fdb_[b] != 0.0) {
                dsp::Biquad flat;
                design(flat, b, 0.0);
                co_[b].snap(flat);
                st_[b][0].reset(); st_[b][1].reset();
                active_[b] = true;
            }
            if (active_[b]) co_[b].glide(q, n);
        }
    }
    const float outT = dbGain(pOut->get());
    if (!primed_) { out_.snap(outT); primed_ = true; }
    out_.target(outT, n);

    for (int i = 0; i < n; ++i) {
        double l = c.io[0][i], r = c.io[1][i];
        for (int b = 0; b < kBands; ++b) {
            if (!active_[b]) continue;
            co_[b].step();
            l = st_[b][0].run(co_[b], l);
            r = st_[b][1].run(co_[b], r);
        }
        const float g = out_.next();
        c.io[0][i] = (float)l * g;
        c.io[1][i] = (float)r * g;
    }
    // A band whose filter has come back to flat is left out again once it has stopped ringing: all sliders at
    // zero then passes the input through untouched, bit for bit.
    for (int b = 0; b < kBands; ++b) {
        if (!active_[b]) continue;
        st_[b][0].flush(); st_[b][1].flush();
        if (fdb_[b] == 0.0 && !co_[b].moving() && st_[b][0].quiet() && st_[b][1].quiet()) {
            active_[b] = false;
            st_[b][0].reset(); st_[b][1].reset();
        }
    }
    scope_.push(c.io[0], c.io[1], n);
}

std::vector<Preset> TenBand::factoryPresets() const {
    return buildPresets(*this, {
        {"Flat", {}},
        {"Smile curve", {{"g31", 5}, {"g62", 4}, {"g125", 2}, {"g500", -2}, {"g1k", -3}, {"g2k", -2}, {"g8k", 3}, {"g16k", 5}}},
        {"Low lift", {{"g31", 3}, {"g62", 5}, {"g125", 4}, {"g250", 2}}},
        {"Voice forward", {{"g62", -4}, {"g125", -2}, {"g1k", 2}, {"g2k", 4}, {"g4k", 3}, {"g8k", 1}}},
        {"Telephone band", {{"g31", -15}, {"g62", -15}, {"g125", -12}, {"g250", -4}, {"g1k", 3}, {"g2k", 4}, {"g4k", -6}, {"g8k", -15}, {"g16k", -15}, {"out", -2}}},
        {"Air and sub", {{"g31", 4}, {"g16k", 6}, {"g8k", 2}}},
        {"Mid scoop", {{"g250", -2}, {"g500", -5}, {"g1k", -6}, {"g2k", -3}}},
        {"Warm tilt", {{"g31", 4}, {"g62", 3}, {"g125", 2}, {"g250", 1}, {"g2k", -1}, {"g4k", -2}, {"g8k", -3}, {"g16k", -4}}},
        {"Bright tilt", {{"g31", -4}, {"g62", -3}, {"g125", -2}, {"g250", -1}, {"g2k", 1}, {"g4k", 2}, {"g8k", 3}, {"g16k", 4}}},
        {"Mud cut", {{"g125", -1}, {"g250", -4}, {"g500", -2}}},
    });
}

std::vector<ParamGroup> TenBand::paramGroups() const {
    ParamGroup bands{"Bands", {}};
    for (int b = 0; b < kBands; ++b) bands.ids.push_back(pGain[b]->info.id);
    return {bands, {"Output", {"out"}}};
}

// ------------------------------------------------------------------ Motion Filter

namespace {
const double kDivTicks[MotionFilter::kDivisions] = {120, 160, 240, 320, 480, 640, 720, 960, 1440, 1920, 3840, 7680, 15360, 30720};

inline void svfStep(dsp::Svf& s, float g, float k, float x, float& lp, float& bp, float& hp) {
    const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    const float v3 = x - s.ic2;
    const float v1 = a1 * s.ic1 + a2 * v3;
    const float v2 = s.ic2 + a2 * s.ic1 + a3 * v3;
    s.ic1 = 2.0f * v1 - s.ic1;
    s.ic2 = 2.0f * v2 - s.ic2;
    lp = v2; bp = v1; hp = x - k * v1 - v2;
}
}

double MotionFilter::divisionTicks(int index) { return kDivTicks[clampT(index, 0, kDivisions - 1)]; }

float MotionFilter::lfoWave(int shape, double phase) {
    const double ph = phase - std::floor(phase);
    switch (shape) {
        case 1: return (float)(ph < 0.25 ? 4.0 * ph : (ph < 0.75 ? 2.0 - 4.0 * ph : 4.0 * ph - 4.0));
        case 2: return ph < 0.5 ? 1.0f : -1.0f;
        case 3: return (float)(2.0 * ph - 1.0);
        case 4: return (float)(1.0 - 2.0 * ph);
        case 5: {       // a new level every cycle, the same every time the song passes this point
            uint32 h = (uint32)(long long)std::floor(phase) * 2654435761u + 0x7F4A7C15u;
            h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
            return (float)((double)(h & 0xFFFFFF) / 8388607.5 - 1.0);
        }
        default: return (float)std::sin(kTwoPi * ph);
    }
}

MotionFilter::MotionFilter() {
    pMode = addChoice("mode", "Mode", {"Low-pass", "Band-pass", "High-pass", "Notch", "Peak"}, 0);
    pSlope = addChoice("slope", "Slope", {"12 dB / octave", "24 dB / octave"}, 0);
    pCutoff = addFloat("cutoff", "Cutoff", 20.0f, 20000.0f, 1200.0f, "Hz", 3.0f);
    pReso = addFloat("reso", "Resonance", 0.0f, 1.0f, 0.3f, "%");
    pDrive = addFloat("drive", "Drive", 0.0f, 24.0f, 0.0f, "dB");
    pEnvAmt = addFloat("envamt", "Envelope amount", -5.0f, 5.0f, 0.0f, "oct");
    pEnvSens = addFloat("envsens", "Envelope sensitivity", -12.0f, 36.0f, 6.0f, "dB");
    pAttack = addFloat("attack", "Envelope attack", 0.1f, 200.0f, 5.0f, "ms", 3.0f);
    pRelease = addFloat("release", "Envelope release", 5.0f, 2000.0f, 150.0f, "ms", 3.0f);
    pLfoShape = addChoice("lfoshape", "LFO shape", {"Sine", "Triangle", "Square", "Saw up", "Saw down", "Random steps"}, 0);
    pLfoSync = addBool("lfosync", "LFO tempo sync", true);
    pLfoRate = addFloat("lforate", "LFO speed (free)", 0.01f, 20.0f, 1.0f, "Hz", 3.0f);
    pLfoDiv = addChoice("lfodiv", "LFO length (synced)", {"1/32", "1/16 triplet", "1/16", "1/8 triplet", "1/8", "1/4 triplet", "1/8 dotted", "1/4",
                                                          "1/4 dotted", "1/2", "1 bar", "2 bars", "4 bars", "8 bars"}, 7);
    pLfoDepth = addFloat("lfodepth", "LFO depth", 0.0f, 5.0f, 0.0f, "oct");
    pLfoPhase = addFloat("lfophase", "LFO right channel offset", 0.0f, 180.0f, 0.0f, "deg");
    pMix = addFloat("mix", "Dry / wet", 0.0f, 1.0f, 1.0f, "%");
    pOut = addFloat("out", "Output", -24.0f, 12.0f, 0.0f, "dB");
    for (auto& o : os_) o.setTables(tonefx::osTables(4, 1));
    for (auto& p : pub_) p.store(0.0f);
}

void MotionFilter::prepare(double sr) { sr_ = sr; reset(); }

void MotionFilter::reset() {
    for (auto& ch : svf_) for (auto& s : ch) s.reset();
    for (int ch = 0; ch < 2; ++ch) { os_[ch].reset(); sat_[ch].reset(0.0, 1.0); lastHz_[ch] = -1.0f; g_[ch] = 0.0f; sm_[ch] = 0.0; }
    gate_.reset();
    dly_.reset();
    dip_.reset();
    env_ = 0.0f;
    free_ = 0.0;
    primed_ = false;
}

void MotionFilter::process(ProcessContext& c) {
    const int n = c.numSamples;
    const float sr = (float)sr_;
    const float qT = resonanceQ(pReso->get()), driveT = pDrive->get();
    const float outT = dbGain(pOut->get()), mixT = pMix->get();
    const bool first = !primed_;
    if (first) { q_.snap(qT); drive_.snap(driveT); mix_.snap(mixT); out_.snap(outT); }
    q_.target(qT, n);
    drive_.target(driveT, n);
    mix_.target(mixT, n);
    out_.target(outT, n);
    dip_.begin(pMode->getInt() * 2 + pSlope->getInt(), n);
    const int mode = dip_.active / 2;
    const bool two = (dip_.active & 1) != 0;

    // The drive stage runs four times oversampled, and only while there is any drive.
    const bool wantDrive = driveT > 0.0f || drive_.cur > 0.0f;
    bool runDrive = wantDrive, started = false;
    if (first) { if (wantDrive) gate_.startOn(); else gate_.reset(); }
    else runDrive = gate_.begin(wantDrive, n, started);
    if (started) for (int ch = 0; ch < 2; ++ch) { os_[ch].reset(); sat_[ch].reset(0.0, 1.0); }

    // LFO: while the song plays its phase is the song position, less the time the sound spends in this effect.
    const bool sync = pLfoSync->getBool();
    const double divTicks = divisionTicks(pLfoDiv->getInt());
    const double hz = sync ? c.tempo * kPPQ / (60.0 * divTicks) : (double)pLfoRate->get();
    const double inc = hz / sr_;
    double cycles;
    if (c.playing) {
        const double tick = c.tickPos - (double)tonefx::kOsLatency * c.tempo * kPPQ / (60.0 * sr_);
        cycles = sync ? tick / divTicks : tick * 60.0 / (c.tempo * kPPQ) * hz;
        free_ = cycles + inc * n;
    } else {
        cycles = free_;
        free_ += inc * n;
        if (free_ > 1.0e9) free_ -= 1.0e9;
    }
    const int shape = pLfoShape->getInt();
    const float depth = pLfoDepth->get(), envAmt = pEnvAmt->get();
    const double offset = (double)pLfoPhase->get() / 360.0;
    const float sens = dbGain(pEnvSens->get());
    const float att = (float)(1.0 - std::exp(-1.0 / (0.001 * (double)pAttack->get() * sr_)));
    const float rel = (float)(1.0 - std::exp(-1.0 / (0.001 * (double)pRelease->get() * sr_)));
    const double base = std::log2((double)pCutoff->get());
    const double smooth = 1.0 - std::exp(-1.0 / (0.001 * kSmoothMs * sr_));
    const float maxHz = std::min(20000.0f, 0.45f * sr);

    for (int i = 0; i < n; ++i) {
        const float x[2] = {c.io[0][i], c.io[1][i]};
        float dry[2];
        dly_.push(x[0], x[1], dry[0], dry[1]);
        // the envelope follows the delayed signal, so it lines up with what the filter is given
        const float det = std::min(1.0f, std::max(std::fabs(dry[0]), std::fabs(dry[1])) * sens);
        env_ += (det > env_ ? att : rel) * (det - env_);
        const float q = q_.next(), driveDb = drive_.next(), mix = mix_.next(), outG = out_.next(), dipG = dip_.gain.next();
        float in[2] = {dry[0], dry[1]};
        if (runDrive) {
            const float osMix = gate_.mix.next();
            const double g = dbGain(driveDb);
            const double blend = std::min(1.0, (double)driveDb / 6.0);
            const double lin = (1.0 - blend) / g, sat = blend / std::sqrt(g);
            for (int ch = 0; ch < 2; ++ch) {
                float f[4];
                os_[ch].up(x[ch], f);
                for (int p = 0; p < 4; ++p) {
                    const double u = g * f[p], before = sat_[ch].u0;
                    const double y = sat_[ch].run(u, 1.0);
                    f[p] = (float)(lin * 0.5 * (before + u) + sat * y);
                }
                const float d = os_[ch].down(f);
                in[ch] += (d - in[ch]) * osMix;
            }
        }
        const float k1 = 1.0f / q;
        const float peakA = q / 0.7071f;
        const double ph = cycles + inc * (double)i;
        for (int ch = 0; ch < 2; ++ch) {
            const float lfo = depth != 0.0f ? lfoWave(shape, ch == 0 ? ph : ph + offset) : 0.0f;
            const double want = base + (double)(envAmt * env_) + (double)(depth * lfo);
            if (first && i == 0) sm_[ch] = want;
            else {
                sm_[ch] += smooth * (want - sm_[ch]);
                if (std::fabs(want - sm_[ch]) < 1.0e-7) sm_[ch] = want;
            }
            const float hzNow = clampT((float)std::exp2(sm_[ch]), 20.0f, maxHz);
            if (hzNow != lastHz_[ch]) { lastHz_[ch] = hzNow; g_[ch] = (float)std::tan(kPi * (double)hzNow / sr_); }
            const float g = g_[ch];
            float y = in[ch], lp, bp, hp;
            svfStep(svf_[ch][0], g, k1, y, lp, bp, hp);
            switch (mode) {
                case Band: y = bp * k1; break;
                case High: y = hp; break;
                case Notch: y = y - bp * k1; break;
                case Peak: y = y + ((two ? std::sqrt(peakA) : peakA) - 1.0f) * k1 * bp; break;
                default: y = lp; break;
            }
            if (two) {
                // the second stage of a low-pass or high-pass stays flat at its corner; the others repeat the first
                const float k2 = (mode == Low || mode == High) ? 1.41421356f : k1;
                svfStep(svf_[ch][1], g, k2, y, lp, bp, hp);
                switch (mode) {
                    case Band: y = bp * k2; break;
                    case High: y = hp; break;
                    case Notch: y = y - bp * k2; break;
                    case Peak: y = y + (std::sqrt(peakA) - 1.0f) * k2 * bp; break;
                    default: y = lp; break;
                }
            }
            c.io[ch][i] = dry[ch] + (y * dipG * outG - dry[ch]) * mix;
        }
    }
    for (auto& ch : svf_) for (auto& s : ch) { flushDenormal(s.ic1); flushDenormal(s.ic2); }
    flushDenormal(env_);
    primed_ = true;
    pub_[0].store(env_, std::memory_order_relaxed);
    pub_[1].store(lastHz_[0], std::memory_order_relaxed);
    pub_[2].store(lastHz_[1], std::memory_order_relaxed);
}

std::vector<Preset> MotionFilter::factoryPresets() const {
    return buildPresets(*this, {
        {"Still low-pass", {}},
        {"One-bar sweep", {{"cutoff", 900}, {"reso", 0.45f}, {"slope", 1}, {"lfodepth", 2.5f}, {"lfodiv", 10}}},
        {"Quarter-note wah", {{"mode", 1}, {"cutoff", 1100}, {"reso", 0.5f}, {"lfodepth", 1.6f}, {"lfodiv", 7}, {"lfoshape", 1}}},
        {"Touch wah", {{"mode", 1}, {"cutoff", 450}, {"reso", 0.55f}, {"envamt", 3.2f}, {"envsens", 12}, {"attack", 8}, {"release", 180}}},
        {"Ducking brightness", {{"cutoff", 9000}, {"reso", 0.2f}, {"envamt", -3.0f}, {"envsens", 9}, {"attack", 2}, {"release", 260}}},
        {"Stepped bleeps", {{"mode", 1}, {"slope", 1}, {"cutoff", 1500}, {"reso", 0.7f}, {"lfoshape", 5}, {"lfodepth", 2.2f}, {"lfodiv", 2}, {"out", -3}}},
        {"Stereo drift", {{"cutoff", 2400}, {"reso", 0.35f}, {"lfosync", 0}, {"lforate", 0.18f}, {"lfodepth", 1.2f}, {"lfophase", 90}}},
        {"Dirty low-pass", {{"cutoff", 1800}, {"reso", 0.4f}, {"slope", 1}, {"drive", 15}, {"out", -3}}},
        {"Moving notch", {{"mode", 3}, {"cutoff", 1200}, {"reso", 0.3f}, {"lfosync", 0}, {"lforate", 0.4f}, {"lfodepth", 2.0f}, {"lfophase", 60}}},
        {"Eighth-note chop", {{"mode", 2}, {"cutoff", 300}, {"reso", 0.25f}, {"lfoshape", 2}, {"lfodepth", 3.0f}, {"lfodiv", 4}}},
        {"Rising peak", {{"mode", 4}, {"cutoff", 700}, {"reso", 0.3f}, {"lfoshape", 3}, {"lfodepth", 2.0f}, {"lfodiv", 10}, {"out", -6}}},
    });
}

std::vector<ParamGroup> MotionFilter::paramGroups() const {
    return {
        {"Filter", {"mode", "slope", "cutoff", "reso", "drive"}},
        {"Envelope follower", {"envamt", "envsens", "attack", "release"}},
        {"LFO", {"lfoshape", "lfosync", "lforate", "lfodiv", "lfodepth", "lfophase"}},
        {"Output", {"mix", "out"}},
    };
}

} // namespace pulp
