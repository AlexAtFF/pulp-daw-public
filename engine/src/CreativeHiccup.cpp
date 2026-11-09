// Hiccup: on a step grid locked to the song, each step may fire one trick. Which one is decided by a hash of
// the step's number and the seed, so a bar glitches the same way every time it is played or exported.
//
// Every trick is a plain function of "how far into the step are we" reading the recorded input, so a step
// can be started anywhere (a seek, an export from the middle) and can be carried on a little past its end:
// the first two milliseconds of each step crossfade from the step before, carried on, to the new one.
#include "pulp/Creative.h"

namespace pulp {

namespace {
const double kTickEps = 1.0e-6;
const int kRepDiv[] = {1, 2, 3, 4, 8};
}

Hiccup::Hiccup() {
    pGrid = addChoice("grid", "Grid", {"1/4", "1/8", "1/16", "1/32"}, 2);
    pCycle = addChoice("cycle", "Pattern repeats every", {"1 bar", "2 bars", "4 bars", "8 bars", "Never"}, 2);
    pProb[Repeat] = addFloat("repeat", "Repeat chance", 0, 1, 0.2f, "%");
    pProb[Reverse] = addFloat("reverse", "Reverse chance", 0, 1, 0.08f, "%");
    pProb[Stop] = addFloat("stop", "Tape stop chance", 0, 1, 0.04f, "%");
    pProb[Crush] = addFloat("crush", "Bit crush chance", 0, 1, 0.08f, "%");
    pProb[Gate] = addFloat("gate", "Gate chance", 0, 1, 0.1f, "%");
    pRepLen = addChoice("replen", "Repeat length", {"Whole step", "1/2 step", "1/3 step", "1/4 step", "1/8 step"}, 1);
    pPitch = addFloat("pitch", "Pitch down per repeat", 0, 12, 0, "st");
    pIntensity = addFloat("intensity", "Intensity", 0, 1, 1, "%");
    pSeed = add({"seed", "Seed", 0, 999, 1, 1, 1000, "", {}});
    pMix = addFloat("mix", "Dry / wet", 0, 1, 1, "%");
}

std::vector<ParamGroup> Hiccup::paramGroups() const {
    return {{"Grid", {"grid", "cycle", "seed"}},
            {"Chances", {"repeat", "reverse", "stop", "crush", "gate"}},
            {"Tricks", {"replen", "pitch", "intensity", "mix"}}};
}

double Hiccup::stepBeats(int grid) { return grid <= 0 ? 1.0 : (grid == 1 ? 0.5 : (grid == 2 ? 0.25 : 0.125)); }

const char* Hiccup::trickName(int t) {
    switch (t) {
        case Repeat: return "Repeat";
        case Reverse: return "Reverse";
        case Stop: return "Tape stop";
        case Crush: return "Bit crush";
        case Gate: return "Gate";
        default: return "Nothing";
    }
}

int Hiccup::crushHold(float intensity, double sr) {
    return 1 + (int)std::lround((double)intensity * intensity * (sr / 1500.0 - 1.0));      // down to 1.5 kHz
}
float Hiccup::crushLevels(float intensity) { return std::pow(2.0f, 15.0f - 13.0f * intensity); }   // 16 bits down to 3

// Each trick draws its own number for the step; those under their chance are in the running, and the one
// that got in by the widest margin wins.
int Hiccup::trickAt(int64 step) const {
    const int cyc = pCycle->getInt();
    if (cyc < 4) {
        const int64 perBar = (int64)std::llround(4.0 / stepBeats(pGrid->getInt()));
        const int64 m = perBar << cyc;
        step = ((step % m) + m) % m;
    }
    const uint32 seed = (uint32)pSeed->getInt();
    int best = None;
    float bestScore = 2.0f;
    for (int k = 0; k < NumTricks; ++k) {
        const float p = pProb[k]->get();
        if (p <= 0.0f) continue;
        const float r = crt::hashUni(seed, (uint32)(uint64)step, (uint32)((uint64)step >> 32) * 16u + (uint32)k + 1u);
        if (r >= p) continue;
        const float score = r / p;
        if (score < bestScore) { bestScore = score; best = k; }
    }
    return best;
}

void Hiccup::prepare(double sr) {
    sr_ = sr;
    tape_.prepare(8.0, sr);
    fadeLen_ = std::max(16, (int)std::lround(0.002 * sr));
    reset();
}

void Hiccup::reset() {
    tape_.reset();
    cur_ = prev_ = Step();
    stepIndex_ = 0;
    lastGrid_ = -1;
    fadeLeft_ = 0;
    nextTick_ = 0.0;
    primed_ = false;
    wasPlaying_ = false;
    mTrick_.store(-1, std::memory_order_relaxed);
}

// One sample of a step's trick. `now` is the number of the newest sample on the tape.
void Hiccup::sample(const Step& s, int64 now, float amt, float& l, float& r) const {
    const float liveL = tape_.at(0, now), liveR = tape_.at(1, now);
    if (s.kind == None) { l = liveL; r = liveR; return; }
    const double j = (double)(now - s.t0);
    float tl = 0.0f, tr = 0.0f;
    switch (s.kind) {
        case Repeat: {      // the slice that ended where the step began, over and over
            const double len = std::max(8.0, s.repLen);
            const double rep = std::floor(j / len), o = j - rep * len;
            const double v = std::exp2(-std::min(rep * (double)s.pitch, 48.0) / 12.0);
            tape_.read(j + len - o * v, v, tl, tr);
            const double fi = std::min((double)fadeLen_, len * 0.25);
            if (rep >= 1.0 && o < fi) {     // each repeat fades in over the end of the one before, carried on
                const double vp = std::exp2(-std::min((rep - 1.0) * (double)s.pitch, 48.0) / 12.0);
                float pl, pr;
                tape_.read(j + len - (o + len) * vp, vp, pl, pr);
                const float a = (float)(o / fi);
                tl = pl + (tl - pl) * a; tr = pr + (tr - pr) * a;
            }
            break;
        }
        case Reverse:       // backwards from where the step began
            tape_.read(2.0 * j + 1.0, 1.0, tl, tr);
            break;
        case Stop: {        // the speed falls in a straight line from 1 to 0 over the step
            const double u = j / s.len;
            if (u < 1.0) {
                const double speed = 1.0 - u;
                tape_.read(0.5 * s.len * u * u, speed, tl, tr);
                float g = clampT((float)(speed / 0.15), 0.0f, 1.0f);
                g = g * g * (3.0f - 2.0f * g);
                tl *= g; tr *= g;
            }
            break;
        }
        case Crush: {       // sample and hold from the step's start, then fewer bits
            const int64 since = (now - s.t0) % (int64)s.hold;
            tl = std::round(tape_.at(0, now - since) * s.levels) / s.levels;
            tr = std::round(tape_.at(1, now - since) * s.levels) / s.levels;
            l = tl; r = tr;
            return;
        }
        case Gate: {        // open for the first half of the step, and open again just in time for the next one
            const double close = 0.5 * s.len, f = (double)fadeLen_;
            float g = 1.0f;
            if (j >= close) g = std::max(clampT(1.0f - (float)((j - close) / f), 0.0f, 1.0f), clampT(1.0f - (float)((s.len - j) / f), 0.0f, 1.0f));
            tl = liveL * g; tr = liveR * g;
            break;
        }
        default: break;
    }
    l = tl * amt + liveL * (1.0f - amt);
    r = tr * amt + liveR * (1.0f - amt);
}

void Hiccup::process(ProcessContext& c) {
    const int n = c.numSamples;
    float* L = c.io[0];
    float* R = c.io[1];
    const bool playing = c.playing;

    if (!playing) {
        // Stopped: the input passes untouched. A trick that was sounding when the song stopped fades out first.
        if (wasPlaying_ && primed_) {
            if (cur_.kind != None) { prev_ = cur_; fadeLeft_ = fadeLen_; }
            cur_.kind = None;
        }
        wasPlaying_ = false;
        if (!primed_ || fadeLeft_ <= 0) {
            primed_ = false;
            fadeLeft_ = 0;
            mTrick_.store(-1, std::memory_order_relaxed);
            return;
        }
    }
    const double tps = c.tempo * kPPQ / (60.0 * c.sampleRate);
    bool jumped = false;
    if (playing) {
        if (!wasPlaying_) { tape_.forget(); primed_ = false; fadeLeft_ = 0; }     // an empty tape, as in an export from here
        else if (std::fabs(c.tickPos - nextTick_) > 0.25) jumped = true;
        nextTick_ = c.tickPos + n * tps;
        wasPlaying_ = true;
    }
    const int grid = pGrid->getInt();
    const double stepTicks = stepBeats(grid) * kPPQ;
    const float amtT = pIntensity->get();
    const bool fresh = !primed_;
    if (fresh) { mix_.snap(pMix->get()); amt_.snap(amtT); }
    mix_.target(pMix->get(), n);
    amt_.target(amtT, n);

    auto make = [&](int64 s, int64 t0) {
        Step st;
        st.kind = trickAt(s);
        st.t0 = t0;
        st.len = stepTicks / tps;
        st.repLen = st.len / kRepDiv[clampT(pRepLen->getInt(), 0, 4)];
        st.pitch = pPitch->get();
        st.hold = crushHold(amtT, sr_);
        st.levels = crushLevels(amtT);
        return st;
    };

    for (int i = 0; i < n; ++i) {
        const float dryL = L[i], dryR = R[i];
        tape_.push(dryL, dryR);
        const int64 now = tape_.n - 1;
        if (playing) {
            const double tick = c.tickPos + i * tps;
            const int64 s = (int64)std::floor((tick + kTickEps) / stepTicks);
            const bool restart = !primed_ || (jumped && i == 0) || grid != lastGrid_;
            if (restart || s != stepIndex_) {
                // the sample this step began on: this one when the song simply ran into it, worked out from the
                // song position otherwise
                const int64 into = (int64)std::floor((tick + kTickEps - (double)s * stepTicks) / tps);
                const int64 t0 = (!restart && s == stepIndex_ + 1) ? now : now - into;
                Step next = make(s, t0);
                // a trick that would have to play what was never recorded (the song has only just begun) is left out
                if ((next.kind == Repeat && t0 - (int64)std::ceil(next.repLen) < tape_.validFrom) ||
                    (next.kind == Reverse && t0 - (int64)std::ceil(next.len) < tape_.validFrom)) next.kind = None;
                if (!primed_) {             // playback begins here, on an empty tape: nothing to fade from
                    prev_ = Step();
                    fadeLeft_ = 0;
                } else {
                    prev_ = cur_;
                    fadeLeft_ = (prev_.kind != None || next.kind != None) ? fadeLen_ : 0;
                }
                cur_ = next;
                stepIndex_ = s;
                lastGrid_ = grid;
                primed_ = true;
            }
        }
        const float amt = amt_.next();
        float wl, wr;
        sample(cur_, now, amt, wl, wr);
        if (fadeLeft_ > 0) {
            float pl, pr;
            sample(prev_, now, amt, pl, pr);
            const float a = 1.0f - (float)fadeLeft_ / (float)fadeLen_;
            wl = pl + (wl - pl) * a; wr = pr + (wr - pr) * a;
            --fadeLeft_;
        }
        const float m = mix_.next();
        L[i] = crt::mixDryWet(dryL, wl, m);
        R[i] = crt::mixDryWet(dryR, wr, m);
    }
    mTrick_.store(cur_.kind, std::memory_order_relaxed);
}

std::vector<Preset> Hiccup::factoryPresets() const {
    return buildPresets(*this, {
        {"Now and then", {}},
        {"Stutter machine", {{"grid", 1}, {"repeat", 0.6f}, {"reverse", 0}, {"stop", 0}, {"crush", 0}, {"gate", 0}, {"replen", 1}, {"seed", 7}}},
        {"Falling repeats", {{"grid", 1}, {"repeat", 0.5f}, {"reverse", 0}, {"stop", 0}, {"crush", 0}, {"gate", 0}, {"replen", 3}, {"pitch", 2}, {"seed", 21}}},
        {"Backward glances", {{"grid", 0}, {"repeat", 0}, {"reverse", 0.35f}, {"stop", 0}, {"crush", 0}, {"gate", 0}, {"seed", 3}}},
        {"Flat batteries", {{"grid", 0}, {"cycle", 1}, {"repeat", 0}, {"reverse", 0}, {"stop", 0.3f}, {"crush", 0}, {"gate", 0}, {"seed", 12}}},
        {"Crushed corners", {{"grid", 2}, {"repeat", 0}, {"reverse", 0}, {"stop", 0}, {"crush", 0.4f}, {"gate", 0}, {"intensity", 0.8f}, {"seed", 40}}},
        {"Chopper", {{"grid", 2}, {"repeat", 0}, {"reverse", 0}, {"stop", 0}, {"crush", 0}, {"gate", 0.65f}, {"seed", 5}}},
        {"Thirty-second chatter", {{"grid", 3}, {"cycle", 0}, {"repeat", 0.3f}, {"reverse", 0}, {"stop", 0}, {"crush", 0.05f}, {"gate", 0.3f}, {"replen", 0}, {"seed", 88}}},
        {"Everything at once", {{"grid", 2}, {"cycle", 4}, {"repeat", 0.2f}, {"reverse", 0.2f}, {"stop", 0.12f}, {"crush", 0.2f}, {"gate", 0.2f}, {"replen", 2}, {"pitch", 1}, {"seed", 303}}},
        {"Half strength", {{"grid", 1}, {"repeat", 0.3f}, {"reverse", 0.15f}, {"stop", 0.05f}, {"crush", 0.1f}, {"gate", 0.1f}, {"intensity", 0.5f}, {"seed", 64}}},
    });
}

} // namespace pulp
