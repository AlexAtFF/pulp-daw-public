// Stack: amplifier voicings, tone stack and cabinet.
#include "pulp/Tone.h"

namespace pulp {

using tonefx::dbGain;

namespace {

struct CabSpec { float hpHz, hpQ, lowHz, lowDb, lowQ, dipHz, dipDb, dipQ, presHz, presDb, presQ, lpHz, lpQ1, lpQ2; };
// Each cabinet is a small network: the low roll-off and thump of the box, a low-mid dip, the presence peak of
// the cone, and a steep roll-off above it. Nothing here is measured from a real cabinet.
const CabSpec kCabs[Stack::NumCabs] = {
    {20, 0.7f, 100, 0, 1, 400, 0, 1, 3000, 0, 1, 20000, 0.7f, 0.7f},                    // off (not used)
    {95, 0.9f, 170, 1.5f, 1.0f, 480, -2.0f, 1.0f, 2600, 5.0f, 1.3f, 5400, 0.75f, 1.2f},  // one twelve-inch speaker, open back
    {85, 1.1f, 140, 2.5f, 1.0f, 450, -3.0f, 0.9f, 3000, 4.5f, 1.4f, 5000, 0.75f, 1.3f},  // two
    {75, 1.4f, 115, 3.5f, 1.1f, 400, -4.5f, 0.8f, 3400, 5.5f, 1.6f, 4500, 0.8f, 1.5f},   // four, closed back
    {42, 1.2f, 80, 3.0f, 1.0f, 300, -2.5f, 0.8f, 1600, 3.0f, 1.2f, 3200, 0.7f, 1.1f},    // one fifteen-inch bass speaker
};
const double kCoupleHz = 20.0, kPresenceHz = 3800.0, kDcHz = 8.0;

double db20(double lin) { return 20.0 * std::log10(std::max(lin, 1.0e-12)); }
double poleLpMag(double hz, double corner, double sr) {
    const double a = 1.0 - std::exp(-kTwoPi * corner / sr), w = kTwoPi * hz / sr;
    return a / std::sqrt(1.0 + (1.0 - a) * (1.0 - a) - 2.0 * (1.0 - a) * std::cos(w));
}
double poleHpMag(double hz, double corner, double sr) {
    const double a = 1.0 - std::exp(-kTwoPi * corner / sr), w = kTwoPi * hz / sr;
    return (1.0 - a) * 2.0 * std::fabs(std::sin(0.5 * w)) / std::sqrt(1.0 + (1.0 - a) * (1.0 - a) - 2.0 * (1.0 - a) * std::cos(w));
}
// What each anti-aliased stage takes off the top: for a quiet signal it is the average of two neighbouring fast
// samples. At four times 48 kHz that is 0.03 dB at 5 kHz, 0.12 dB at 10 kHz and 0.47 dB at 20 kHz, per stage.
double averagingDb(double hz, double fastSr) { return db20(std::cos(kPi * hz / fastSr)); }

} // namespace

const Stack::Voice& Stack::voice(int v) {
    static const Voice voices[NumVoicings] = {
        // stages, gain at 0 and at 10, low cut, voicing peak (Hz, dB, Q), roll-off between stages, bias,
        // bass / middle (Hz, Q) / treble corners, middle at noon, power stage drive, level trim
        {1, 0.0f, 24.0f, 60.0f, 2200.0f, 3.0f, 0.5f, 12000.0f, 0.20f, 110.0f, 600.0f, 0.7f, 3200.0f, -3.0f, -6.0f, 0.0f},
        {2, 12.0f, 42.0f, 90.0f, 800.0f, 4.0f, 0.6f, 7000.0f, 0.35f, 120.0f, 700.0f, 0.7f, 3400.0f, -1.0f, 0.0f, -3.0f},
        {3, 24.0f, 62.0f, 130.0f, 1000.0f, 6.0f, 0.7f, 5500.0f, 0.30f, 120.0f, 750.0f, 0.8f, 3600.0f, 0.0f, 3.0f, -4.0f},
        {2, 0.0f, 34.0f, 28.0f, 250.0f, 2.0f, 0.5f, 5000.0f, 0.25f, 70.0f, 420.0f, 0.7f, 2000.0f, -2.0f, 0.0f, 0.0f},
    };
    return voices[clampT(v, 0, (int)NumVoicings - 1)];
}

Stack::Stack() {
    pVoicing = addChoice("voicing", "Voicing", {"Clean", "Crunch", "Lead", "Bass"}, 1);
    pGain = addFloat("gain", "Gain", 0.0f, 10.0f, 5.0f);
    pBass = addFloat("bass", "Bass", 0.0f, 10.0f, 5.0f);
    pMid = addFloat("mid", "Middle", 0.0f, 10.0f, 5.0f);
    pTreble = addFloat("treble", "Treble", 0.0f, 10.0f, 5.0f);
    pPresence = addFloat("presence", "Presence", 0.0f, 10.0f, 3.0f);
    pMaster = addFloat("master", "Master", -36.0f, 6.0f, -9.0f, "dB");
    pCab = addChoice("cab", "Cabinet", {"Off", "1 x 12", "2 x 12", "4 x 12", "Bass 15"}, 2);
    pMic = addFloat("mic", "Microphone (centre to edge)", 0.0f, 1.0f, 0.3f, "%");
    pMix = addFloat("mix", "Dry / wet", 0.0f, 1.0f, 1.0f, "%");
    for (int m = 2; m <= kMaxStages + 1; ++m) tab_[m] = tonefx::osTables(4, m);
    for (auto& ch : ch_) ch.os.setTables(tab_[3]);
}

void Stack::designInput(int voicing, double sr, dsp::Biquad& hp, dsp::Biquad& pre) {
    const Voice& v = voice(voicing);
    hp.highpass(v.hpHz, 0.7071, sr);
    pre.peak(v.preHz, v.preQ, v.preDb, sr);
}

void Stack::designTone(int voicing, const Settings& s, double fastSr, dsp::Biquad* four) {
    const Voice& v = voice(voicing);
    four[0].lowShelf(v.bassHz, 0.7071, ((double)s.bass - 5.0) * 2.4, fastSr);
    four[1].peak(v.midHz, v.midQ, ((double)s.mid - 5.0) * 2.0 + v.midNoonDb, fastSr);
    four[2].highShelf(v.trebleHz, 0.7071, ((double)s.treble - 5.0) * 2.4, fastSr);
    four[3].highShelf(kPresenceHz, 0.7071, (double)s.presence * 0.9, fastSr);
}

void Stack::designCab(int cab, float mic, double sr, dsp::Biquad* six) {
    const CabSpec& k = kCabs[clampT(cab, 0, (int)NumCabs - 1)];
    const double m = mic;
    const double top = std::min((double)k.lpHz * (1.0 - 0.38 * m), 0.45 * sr);
    six[0].highpass(k.hpHz, k.hpQ, sr);
    six[1].peak(k.lowHz, k.lowQ, k.lowDb + 2.0 * m, sr);                 // further out the box is heard more than the cone
    six[2].peak(k.dipHz, k.dipQ, k.dipDb * (1.0 + 0.4 * m), sr);
    six[3].peak(std::min((double)k.presHz, 0.45 * sr), k.presQ, k.presDb - 6.0 * m, sr);
    six[4].lowpass(top, k.lpQ1, sr);
    six[5].lowpass(std::min(top * 1.12, 0.45 * sr), k.lpQ2, sr);
}

void Stack::prepare(double sr) { sr_ = sr; reset(); }

void Stack::reset() {
    for (auto& ch : ch_) {
        ch.os.reset();
        ch.inHp.reset(); ch.inPre.reset();
        for (auto& s : ch.st) s.reset(0.0, kKneeNeg);
        ch.power.reset(kPowerBias, kPowerKneeNeg);
        for (auto& p : ch.couple) p.reset();
        for (auto& p : ch.inter) p.reset();
        for (auto& s : ch.tone) s.reset();
        for (auto& s : ch.cab) s.reset();
        ch.dc.reset();
    }
    dly_.reset();
    dip_.reset();
    seen_ = {-1, -1, -1, -1, -1};
    primed_ = false;
}

void Stack::process(ProcessContext& c) {
    const int n = c.numSamples;
    const double fastSr = sr_ * 4.0;
    const float gainT = pGain->get(), masterT = dbGain(pMaster->get()), mixT = pMix->get();
    const bool first = !primed_;
    if (first) { gain_.snap(gainT); master_.snap(masterT); mix_.snap(mixT); primed_ = true; }
    gain_.target(gainT, n);
    master_.target(masterT, n);
    mix_.target(mixT, n);

    const bool switched = dip_.begin(pVoicing->getInt() * 8 + pCab->getInt(), n);
    const int voicing = dip_.active / 8, cab = dip_.active % 8;
    const Voice& v = voice(voicing);
    const Settings s = settings();
    const bool turned = s.bass != seen_.bass || s.mid != seen_.mid || s.treble != seen_.treble || s.presence != seen_.presence || s.mic != seen_.mic;
    if (switched || turned) {
        dsp::Biquad t4[kToneFilters], c6[kCabFilters];
        designTone(voicing, s, fastSr, t4);
        designCab(cab, s.mic, sr_, c6);
        seen_ = s;
        if (switched) {
            // a new voicing or cabinet starts from rest; the sound is faded out at this moment
            dsp::Biquad hp, pre;
            designInput(voicing, sr_, hp, pre);
            inHpC_.snap(hp);
            inPreC_.snap(pre);
            for (int k = 0; k < kToneFilters; ++k) toneC_[k].snap(t4[k]);
            for (int k = 0; k < kCabFilters; ++k) cabC_[k].snap(c6[k]);
            for (auto& ch : ch_) {
                ch.os.setTables(tab_[v.stages + 1]);
                ch.inHp.reset(); ch.inPre.reset();
                for (auto& st : ch.st) st.reset(v.bias, kKneeNeg);
                ch.power.reset(kPowerBias, kPowerKneeNeg);
                for (auto& p : ch.couple) { p.reset(); p.setHz(kCoupleHz, fastSr); }
                for (auto& p : ch.inter) { p.reset(); p.setHz(v.interLp, fastSr); }
                for (auto& st : ch.tone) st.reset();
                for (auto& st : ch.cab) st.reset();
                ch.dc.reset();
                ch.dc.setHz(kDcHz, sr_);
            }
        } else {
            for (int k = 0; k < kToneFilters; ++k) toneC_[k].glide(t4[k], n * 4);
            for (int k = 0; k < kCabFilters; ++k) cabC_[k].glide(c6[k], n);
        }
    }

    const int stages = v.stages;
    const double b = v.bias;
    const double rest = tonefx::Sig::f(b, kKneeNeg), invSlope = 1.0 / tonefx::Sig::slope(b, kKneeNeg);
    const double pg = dbGain(v.powerDb);
    const double prest = tonefx::Sig::f(kPowerBias, kPowerKneeNeg), pinv = 1.0 / tonefx::Sig::slope(kPowerBias, kPowerKneeNeg);
    const double polarity = (stages & 1) ? -1.0 : 1.0;      // every preamp stage turns the signal over; it is put right before the power stage
    const double level = dbGain(v.levelDb);
    const double perStage = 0.11512925464970229 / (double)stages;

    for (int i = 0; i < n; ++i) {
        const double knob = gain_.next();
        const double gs = std::exp(((double)v.gainLo + ((double)v.gainHi - (double)v.gainLo) * knob * 0.1) * perStage);
        const float master = master_.next(), mix = mix_.next(), dipG = dip_.gain.next();
        const float x[2] = {c.io[0][i], c.io[1][i]};
        float dry[2], f[2][4];
        dly_.push(x[0], x[1], dry[0], dry[1]);
        for (int ch = 0; ch < 2; ++ch) {
            Chan& k = ch_[ch];
            k.os.up((float)k.inPre.run(inPreC_, k.inHp.run(inHpC_, x[ch])), f[ch]);
        }
        for (int p = 0; p < 4; ++p) {
            for (auto& tc : toneC_) tc.step();
            for (int ch = 0; ch < 2; ++ch) {
                Chan& k = ch_[ch];
                double y = f[ch][p];
                for (int st = 0; st < stages; ++st) {
                    y = -(k.st[st].run(b + gs * y, kKneeNeg) - rest) * invSlope;
                    y = k.inter[st].lp(y);
                    y = k.couple[st].hp(y);
                }
                y *= polarity;
                for (int t = 0; t < kToneFilters; ++t) y = k.tone[t].run(toneC_[t], y);
                y = (k.power.run(kPowerBias + pg * y, kPowerKneeNeg) - prest) * pinv;
                f[ch][p] = (float)y;
            }
        }
        for (auto& cc : cabC_) cc.step();
        for (int ch = 0; ch < 2; ++ch) {
            Chan& k = ch_[ch];
            double y = k.os.down(f[ch]);
            if (cab != CabOff) for (int t = 0; t < kCabFilters; ++t) y = k.cab[t].run(cabC_[t], y);
            y = k.dc.hp(y);
            c.io[ch][i] = dry[ch] + ((float)(y * level) * master * dipG - dry[ch]) * mix;
        }
    }
    for (auto& k : ch_) {
        k.inHp.flush(); k.inPre.flush();
        for (auto& p : k.couple) p.flush();
        for (auto& p : k.inter) p.flush();
        for (auto& st : k.tone) st.flush();
        for (auto& st : k.cab) st.flush();
        k.dc.flush();
    }
}

double Stack::toneStackDb(double hz) const {
    dsp::Biquad t4[kToneFilters];
    designTone(pVoicing->getInt(), settings(), sr_ * 4.0, t4);
    double db = 0.0;
    for (auto& q : t4) db += db20(q.magnitude(hz, sr_ * 4.0));
    return db;
}

double Stack::cabinetDb(double hz) const {
    const int cab = pCab->getInt();
    if (cab == CabOff) return 0.0;
    dsp::Biquad c6[kCabFilters];
    designCab(cab, pMic->get(), sr_, c6);
    double db = 0.0;
    for (auto& q : c6) db += db20(q.magnitude(hz, sr_));
    return db;
}

double Stack::responseDb(double hz) const {
    const Voice& v = voice(pVoicing->getInt());
    const double fastSr = sr_ * 4.0;
    dsp::Biquad hp, pre;
    designInput(pVoicing->getInt(), sr_, hp, pre);
    double db = db20(hp.magnitude(hz, sr_)) + db20(pre.magnitude(hz, sr_));
    db += (double)v.gainLo + ((double)v.gainHi - (double)v.gainLo) * (double)pGain->get() * 0.1;
    db += v.stages * (db20(poleLpMag(hz, v.interLp, fastSr)) + db20(poleHpMag(hz, kCoupleHz, fastSr)) + averagingDb(hz, fastSr));
    db += toneStackDb(hz);
    db += (double)v.powerDb + averagingDb(hz, fastSr);
    db += cabinetDb(hz);
    db += db20(poleHpMag(hz, kDcHz, sr_));
    db += (double)v.levelDb + (double)pMaster->get();
    return db;
}

std::vector<Preset> Stack::factoryPresets() const {
    return buildPresets(*this, {
        {"Crunch through two twelves", {}},
        {"Glassy clean", {{"voicing", 0}, {"gain", 3.5f}, {"bass", 4.5f}, {"mid", 4}, {"treble", 6.5f}, {"presence", 5}, {"cab", 1}, {"mic", 0.15f}, {"master", -8}}},
        {"Edge of breakup", {{"voicing", 0}, {"gain", 8.5f}, {"bass", 5}, {"mid", 6}, {"treble", 5.5f}, {"presence", 4}, {"cab", 2}, {"mic", 0.3f}, {"master", -10}}},
        {"Rhythm wall", {{"voicing", 1}, {"gain", 7}, {"bass", 6}, {"mid", 4}, {"treble", 6}, {"presence", 5}, {"cab", 3}, {"mic", 0.25f}, {"master", -10}}},
        {"Singing lead", {{"voicing", 2}, {"gain", 6.5f}, {"bass", 4.5f}, {"mid", 7}, {"treble", 5.5f}, {"presence", 4}, {"cab", 3}, {"mic", 0.4f}, {"master", -11}}},
        {"Scooped and saturated", {{"voicing", 2}, {"gain", 9}, {"bass", 7}, {"mid", 2}, {"treble", 7}, {"presence", 6}, {"cab", 3}, {"mic", 0.2f}, {"master", -12}}},
        {"Round bass", {{"voicing", 3}, {"gain", 3}, {"bass", 6}, {"mid", 5}, {"treble", 4}, {"presence", 1}, {"cab", 4}, {"mic", 0.4f}, {"master", -8}}},
        {"Growling bass", {{"voicing", 3}, {"gain", 8}, {"bass", 5.5f}, {"mid", 6.5f}, {"treble", 5.5f}, {"presence", 3}, {"cab", 4}, {"mic", 0.2f}, {"master", -10}}},
        {"Direct, no cabinet", {{"voicing", 1}, {"gain", 4}, {"cab", 0}, {"treble", 4}, {"presence", 0}, {"master", -12}}},
        {"Small and boxy", {{"voicing", 1}, {"gain", 6}, {"bass", 3}, {"mid", 7.5f}, {"treble", 4.5f}, {"cab", 1}, {"mic", 0.9f}, {"master", -9}}},
    });
}

std::vector<ParamGroup> Stack::paramGroups() const {
    return {
        {"Amplifier", {"voicing", "gain", "master"}},
        {"Tone", {"bass", "mid", "treble", "presence"}},
        {"Cabinet", {"cab", "mic"}},
        {"Output", {"mix"}},
    };
}

} // namespace pulp
