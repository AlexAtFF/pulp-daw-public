// Second Voice: up to four pitch-shifted copies of a voice, as harmonies or as doubles.
#include "pulp/Vocal.h"

namespace pulp {
using namespace vocal;

SecondVoice::SecondVoice() {
    pMode = addChoice("mode", "Mode", {"Harmony", "Doubler (detune and delay only)"}, 0);
    pKey = addChoice("key", "Key", keyNames(), 0);
    pScale = addChoice("scale", "Scale (chromatic: semitones)", scaleNames(), Chromatic);
    pFormant = addBool("formant", "Keep formants", true);
    pRange = addChoice("range", "Voice range", {"Low (60 to 600 Hz)", "High (110 to 1200 Hz)"}, 0);
    pDry = addFloat("dry", "Dry level (lowest = off)", -60, 6, 0, "dB");
    pWet = addFloat("wet", "Voices level", -36, 6, 0, "dB");
    const int defInt[kVoices] = {7, -5, 12, -12};
    const float defPan[kVoices] = {-0.5f, 0.5f, -0.25f, 0.25f};
    const float defDly[kVoices] = {10, 16, 22, 28};
    const float defDet[kVoices] = {4, -4, 7, -7};
    for (int i = 0; i < kVoices; ++i) {
        const std::string id = "v" + std::to_string(i + 1), nm = "Voice " + std::to_string(i + 1) + " ";
        pOn[i] = addBool((id + "on").c_str(), (nm + "on").c_str(), i == 0);
        ParamInfo iv;
        iv.id = id + "int"; iv.name = nm + "interval (semitones or scale steps)"; iv.min = -24; iv.max = 24; iv.def = (float)defInt[i]; iv.steps = 49;
        pInterval[i] = add(iv);
        pLevel[i] = addFloat((id + "lvl").c_str(), (nm + "level").c_str(), -36, 6, -6, "dB");
        pPan[i] = addFloat((id + "pan").c_str(), (nm + "pan").c_str(), -1, 1, defPan[i]);
        pDelay[i] = addFloat((id + "dly").c_str(), (nm + "delay").c_str(), 0, 80, defDly[i], "ms");
        pDetune[i] = addFloat((id + "det").c_str(), (nm + "fine detune").c_str(), -50, 50, defDet[i], "ct");
    }
}

std::vector<ParamGroup> SecondVoice::paramGroups() const {
    std::vector<ParamGroup> g;
    g.push_back({"Harmony", {"mode", "key", "scale", "formant", "range"}});
    for (int i = 1; i <= kVoices; ++i) {
        const std::string id = "v" + std::to_string(i);
        g.push_back({"Voice " + std::to_string(i), {id + "on", id + "int", id + "lvl", id + "pan", id + "dly", id + "det"}});
    }
    g.push_back({"Levels", {"dry", "wet"}});
    return g;
}

void SecondVoice::prepare(double sr) {
    sr_ = sr;
    latency_ = latencyFor(sr);
    det_.prepare(sr, latency_ + 7 * (int)(sr / 55.0) + 64);
    for (auto& d : dry_) d.prepare(latency_ + 64);
    float* mono[1] = {det_.mono().d};
    for (auto& v : v_) {
        v.sh.prepare(sr, latency_ - 6, mono, det_.mono().mask, 1);
        v.dl.prepare((int)(0.08 * sr) + 64);
    }
    reset();
}

void SecondVoice::reset() {
    det_.reset();
    for (auto& d : dry_) d.clear();
    for (auto& v : v_) {
        v.sh.reset(-(double)latency_);
        v.dl.clear();
        v.running = false;
        v.fading = false;
        v.xf = 0.0f;
    }
    n_ = 0;
    noteValid_ = false;
    primed_ = false;
}

void SecondVoice::spawnGrain(Voice& v) {
    const double s = v.sh.nextMark();
    GrainCtl c;
    double T = 0.0;
    c.voiced = det_.periodAt(s, T);
    c.period = T;
    if (c.voiced) {
        T = v.sh.localPeriod(T, det_.now() - 1);
        c.period = T;
        double semis = 0.0;
        if (!doubler_) {
            // the note being sung (nearest note of the scale, held against small wobbles) and the note the
            // interval lands on from there; the voice is moved by the distance between the two
            const double keyIn = 69.0 + 12.0 * std::log2(sr_ / (T * 440.0));
            const double cand = nearestNote(keyIn, mask_);
            const bool held = noteValid_ && ((mask_ >> ((((int)note_ % 12) + 12) % 12)) & 1u);
            if (!held || (cand != note_ && std::fabs(keyIn - note_) - std::fabs(keyIn - cand) > 0.3)) note_ = cand;
            noteValid_ = true;
            const int q = (int)std::lround(note_);
            semis = clampT((double)(stepNote(q, v.interval, mask_) - q), -24.0, 24.0);
        }
        semis += v.detune;
        c.pitch = std::pow(2.0, semis / 12.0);
        c.formant = preserve_ ? 1.0 : c.pitch;
    } else {
        noteValid_ = false;
    }
    v.sh.spawn(c, det_.now() - 1);
}

void SecondVoice::process(ProcessContext& c) {
    const int n = c.numSamples;
    mask_ = scaleMask(pScale->getInt(), pKey->getInt());
    doubler_ = pMode->getInt() == 1;
    preserve_ = pFormant->getBool();
    det_.setRange(pRange->getInt());
    const float dryDb = pDry->get(), dryGain = dryDb <= -59.9f ? 0.0f : dbToGain(dryDb);
    const float wetDb = pWet->get();
    if (!primed_) dryG_.snap(dryGain);
    dryG_.target(dryGain, n);
    int sounding = 0;
    for (int i = 0; i < kVoices; ++i) {
        Voice& v = v_[i];
        const bool on = pOn[i]->getBool();
        const float lvl = on ? dbToGain(pLevel[i]->get() + wetDb) : 0.0f;
        const double th = ((double)pPan[i]->get() + 1.0) * kPi * 0.25;
        const float gl = lvl * (float)(1.41421356237 * std::cos(th)), gr = lvl * (float)(1.41421356237 * std::sin(th));
        const int target = clampT((int)std::lround(0.001 * pDelay[i]->get() * sr_), 0, (int)(0.08 * sr_));
        v.interval = (int)std::lround(pInterval[i]->get());
        v.detune = (double)pDetune[i]->get() / 100.0;
        if (!primed_) { v.gl.snap(gl); v.gr.snap(gr); }
        const bool active = on || v.gl.cur != 0.0f || v.gr.cur != 0.0f;       // an "off" voice runs until its fade is over
        if (active && !v.running) {
            v.sh.reset((double)(n_ - latency_));
            v.dl.clear();
            v.running = true;
            v.fading = false;
            v.dA = target;
        }
        if (!active) v.running = false;
        v.gl.target(gl, n);
        v.gr.target(gr, n);
        if (v.running && !v.fading && target != v.dA) { v.dB = target; v.fading = true; v.xf = 0.0f; }
        if (v.running) ++sounding;
    }
    primed_ = true;

    float* L = c.io[0];
    float* R = c.io[1];
    const int64 dmask = dry_[0].mask;
    const float xfStep = (float)(1.0 / (0.02 * sr_));          // a changed delay is reached with a 20 ms crossfade
    for (int i = 0; i < n; ++i) {
        float l = L[i], r = R[i];
        if (!std::isfinite(l)) l = 0.0f;
        if (!std::isfinite(r)) r = 0.0f;
        dry_[0].d[n_ & dmask] = l;
        dry_[1].d[n_ & dmask] = r;
        det_.push(0.5f * (l + r));
        const int64 t = n_ - latency_;
        float sl = 0.0f, sR = 0.0f;
        for (auto& v : v_) {
            if (!v.running) continue;
            int guard = 0;
            while (v.sh.due(t) && guard++ < 32) spawnGrain(v);
            float y;
            v.sh.render(t, &y);
            v.dl.d[n_ & v.dl.mask] = y;
            float a = v.dl.d[(n_ - v.dA) & v.dl.mask];
            if (v.fading) {
                a += v.xf * (v.dl.d[(n_ - v.dB) & v.dl.mask] - a);
                v.xf += xfStep;
                if (v.xf >= 1.0f) { v.dA = v.dB; v.fading = false; }
            }
            sl += a * v.gl.next();
            sR += a * v.gr.next();
        }
        const float dg = dryG_.next();
        L[i] = dry_[0].d[t & dmask] * dg + sl;
        R[i] = dry_[1].d[t & dmask] * dg + sR;
        ++n_;
    }

    const PitchDetector::Estimate* e = det_.latest();
    const bool voiced = e && e->voiced;
    mPitch_.store(voiced ? (float)(69.0 + 12.0 * std::log2(sr_ / (e->period * 440.0))) : 0.0f, std::memory_order_relaxed);
    mVoiced_.store(voiced ? 1.0f : 0.0f, std::memory_order_relaxed);
    mActive_.store((float)sounding, std::memory_order_relaxed);
    scope_.push(L, R, n);
}

std::vector<Preset> SecondVoice::factoryPresets() const {
    return buildPresets(*this, {
        {"Fifth above", {}},
        {"Third above in C major", {{"scale", Major}, {"key", 0}, {"v1int", 2}, {"v1pan", -0.35f}, {"v1lvl", -5}}},
        {"Third and fifth in C major", {{"scale", Major}, {"key", 0}, {"v1int", 2}, {"v2on", 1}, {"v2int", 4}, {"v1lvl", -7}, {"v2lvl", -8}}},
        {"Minor key trio in A", {{"scale", Minor}, {"key", 9}, {"v1int", 2}, {"v2on", 1}, {"v2int", -3}, {"v3on", 1}, {"v3int", 4}, {"v1lvl", -7}, {"v2lvl", -7}, {"v3lvl", -10}}},
        {"Octave below", {{"v1int", -12}, {"v1pan", 0}, {"v1lvl", -4}, {"v1det", 0}, {"v1dly", 0}}},
        {"Octaves both ways", {{"v1int", 12}, {"v2on", 1}, {"v2int", -12}, {"v1lvl", -10}, {"v2lvl", -6}, {"v1pan", -0.3f}, {"v2pan", 0.3f}}},
        {"Four part block chord in C major", {{"scale", Major}, {"key", 0}, {"v1int", 2}, {"v2on", 1}, {"v2int", 4}, {"v3on", 1}, {"v3int", -3}, {"v4on", 1}, {"v4int", -7},
                                              {"v1lvl", -8}, {"v2lvl", -9}, {"v3lvl", -8}, {"v4lvl", -9}}},
        {"Tight double", {{"mode", 1}, {"v1det", 6}, {"v1dly", 14}, {"v1pan", 0}, {"v1lvl", -4}}},
        {"Wide double", {{"mode", 1}, {"v2on", 1}, {"v1det", 8}, {"v2det", -8}, {"v1dly", 18}, {"v2dly", 27}, {"v1pan", -1}, {"v2pan", 1}, {"v1lvl", -5}, {"v2lvl", -5}}},
        {"Thick crowd of four", {{"mode", 1}, {"v2on", 1}, {"v3on", 1}, {"v4on", 1}, {"v1det", 9}, {"v2det", -11}, {"v3det", 17}, {"v4det", -15},
                                 {"v1dly", 12}, {"v2dly", 23}, {"v3dly", 35}, {"v4dly", 48}, {"v1pan", -0.9f}, {"v2pan", 0.9f}, {"v3pan", -0.4f}, {"v4pan", 0.4f},
                                 {"v1lvl", -7}, {"v2lvl", -7}, {"v3lvl", -9}, {"v4lvl", -9}}},
        {"Harmony only, fifth and octave", {{"dry", -60}, {"v1int", 7}, {"v2on", 1}, {"v2int", 12}, {"v1lvl", -3}, {"v2lvl", -6}}},
    });
}

} // namespace pulp
