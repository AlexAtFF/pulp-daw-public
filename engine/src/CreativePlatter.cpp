// Platter: the last ten seconds of the input are a record. While it is held the sound comes from the record:
// the record turns at its own speed (a motor with start and brake times) and a hand pushes the needle back
// and forth around the place where it was grabbed. What you hear is the record passing under the needle, at
// whatever speed and in whichever direction that is; a record that stands still gives no sound.
//
// To make a scratch with automation: switch Hold on with Platter speed at 0 (the record stops under the hand),
// then move Position. Moving it down plays backwards, up plays forwards; how fast it moves is the pitch. Range
// says how far the full travel of Position reaches. Close and open Cut between strokes for the chopped scratches.
// Switch Hold off to go back to the input. (Forwards past the grab point there is only what has come in since
// the grab: with the speed at 1 the needle is already at the newest sound and cannot be pushed further.)
#include "pulp/Creative.h"

namespace pulp {

Platter::Platter() {
    pHold = addBool("hold", "Hold", false);
    pPosition = addFloat("position", "Position", -1, 1, 0);
    pRange = addFloat("range", "Range", 20, 2000, 300, "ms", 2.0f);
    pInertia = addFloat("inertia", "Hand inertia", 1, 200, 12, "ms", 2.0f);
    pSpeed = addFloat("speed", "Platter speed", -2, 2, 1);
    pStart = addFloat("start", "Start time", 5, 3000, 150, "ms", 3.0f);
    pBrake = addFloat("brake", "Brake time", 5, 3000, 400, "ms", 3.0f);
    pCut = addFloat("cut", "Cut", 0, 1, 1, "%");
}

std::vector<ParamGroup> Platter::paramGroups() const {
    return {{"Hand", {"hold", "position", "range", "inertia", "cut"}}, {"Motor", {"speed", "start", "brake"}}};
}

float Platter::meter(int i) const {
    return i == 0 ? mBehind_.load(std::memory_order_relaxed) : (i == 1 ? mSpeed_.load(std::memory_order_relaxed) : mHeld_.load(std::memory_order_relaxed));
}

void Platter::prepare(double sr) {
    sr_ = sr;
    tape_.prepare(kSeconds, sr);
    reset();
}

void Platter::reset() {
    tape_.reset();
    engaged_ = false;
    primed_ = false;
    fade_ = 0.0f;
    base_ = 0.0;
    motor_ = 1.0;
    off1_ = off2_ = 0.0;
    lastDelay_ = 0.0;
    cut_ = 1.0f;
    moveGain_ = 1.0f;
    mBehind_.store(0.0f, std::memory_order_relaxed);
    mSpeed_.store(1.0f, std::memory_order_relaxed);
    mHeld_.store(0.0f, std::memory_order_relaxed);
}

void Platter::process(ProcessContext& c) {
    const int n = c.numSamples;
    float* L = c.io[0];
    float* R = c.io[1];
    const bool hold = pHold->getBool();
    const float cutT = clampT(pCut->get() * 4.0f, 0.0f, 1.0f);         // fully open from a quarter of the way up, like a scratch fader
    if (!primed_) { pos_.snap(pPosition->get()); cut_ = cutT; primed_ = true; }

    if (hold && !engaged_) {
        engaged_ = true;
        if (fade_ <= 0.0f) {
            // A fresh grab: the needle is on the newest sound and the record is turning, so the record is the input
            // at this instant and can take over at once. (A closed cut closes from here, in its own 1.5 ms.)
            base_ = 0.0; motor_ = 1.0; off1_ = off2_ = 0.0; lastDelay_ = 0.0; moveGain_ = 1.0f;
            cut_ = 1.0f;
            fade_ = 1.0f;
        }                           // (grabbed again before a release fade ended: carry on where the needle is)
    }
    if (!hold) engaged_ = false;

    if (!engaged_ && fade_ <= 0.0f) {       // not held: the input, untouched, and still being recorded
        for (int i = 0; i < n; ++i) tape_.push(L[i], R[i]);
        pos_.snap(pPosition->get());
        cut_ = cutT;
        mBehind_.store(0.0f, std::memory_order_relaxed);
        mSpeed_.store(1.0f, std::memory_order_relaxed);
        mHeld_.store(0.0f, std::memory_order_relaxed);
        return;
    }

    pos_.target(pPosition->get(), n);
    const double range = (double)pRange->get() * 0.001 * sr_;
    const double handA = 1.0 - std::exp(-1.0 / (std::max(0.0005, (double)pInertia->get() * 0.0005) * sr_));    // two stages, half the time each
    const double target = pSpeed->get();
    const double up = 1.0 / ((double)pStart->get() * 0.001 * sr_), down = 1.0 / ((double)pBrake->get() * 0.001 * sr_);
    const float cutStep = (float)(1.0 / (0.0015 * sr_)), moveStep = (float)(1.0 / (0.002 * sr_));
    const float fadeStep = (float)(1.0 / (0.010 * sr_));
    const double maxDelay = tape_.maxDelay() - 64.0;
    double rate = 1.0, delay = 0.0;

    for (int i = 0; i < n; ++i) {
        const float dryL = L[i], dryR = R[i];
        tape_.push(dryL, dryR);
        // the motor: speeding up takes the start time, slowing down the brake time; a change of direction goes through a stop
        const bool turning = motor_ * target < 0.0;
        const double goal = turning ? 0.0 : target;
        const double step = (turning || std::fabs(target) < std::fabs(motor_)) ? down : up;
        motor_ += clampT(goal - motor_, -step, step);
        // the hand
        const double want = (double)pos_.next() * range;
        off1_ += handA * (want - off1_);
        off2_ += handA * (off1_ - off2_);
        base_ = clampT(base_ + 1.0 - motor_, 0.0, maxDelay);
        delay = clampT(base_ - off2_, 0.0, maxDelay);
        if (delay < 1.0e-9) delay = 0.0;
        rate = 1.0 - (delay - lastDelay_);
        lastDelay_ = delay;
        float pl, pr;
        tape_.read(delay, rate, pl, pr);
        const double ar = std::fabs(rate);
        float gv = ar < 1.0e-6 ? 0.0f : clampT((float)(ar / 0.08), 0.0f, 1.0f);
        gv = gv * gv * (3.0f - 2.0f * gv);
        moveGain_ += clampT(gv - moveGain_, -moveStep, moveStep);       // (it can only jump where the needle is stopped by an end of the record)
        cut_ += clampT(cutT - cut_, -cutStep, cutStep);
        const float g = moveGain_ * cut_;
        pl *= g; pr *= g;
        const float goalFade = engaged_ ? 1.0f : 0.0f;
        if (fade_ < goalFade) fade_ = std::min(goalFade, fade_ + fadeStep);
        else if (fade_ > goalFade) fade_ = std::max(goalFade, fade_ - fadeStep);
        L[i] = crt::mixDryWet(dryL, pl, fade_);
        R[i] = crt::mixDryWet(dryR, pr, fade_);
    }
    mBehind_.store((float)(delay / sr_), std::memory_order_relaxed);
    mSpeed_.store((float)rate, std::memory_order_relaxed);
    mHeld_.store(engaged_ ? 1.0f : 0.0f, std::memory_order_relaxed);
}

std::vector<Preset> Platter::factoryPresets() const {
    return buildPresets(*this, {
        {"Ready to scratch", {{"speed", 0}, {"brake", 30}, {"start", 60}, {"range", 300}, {"inertia", 12}}},
        {"Baby scratch", {{"speed", 0}, {"brake", 20}, {"start", 40}, {"range", 150}, {"inertia", 6}}},
        {"Long drags", {{"speed", 0}, {"brake", 60}, {"start", 120}, {"range", 900}, {"inertia", 45}}},
        {"Heavy platter", {{"speed", 0}, {"brake", 250}, {"start", 500}, {"range", 400}, {"inertia", 90}}},
        {"Brake to a stop", {{"speed", 0}, {"brake", 900}, {"start", 300}, {"range", 300}}},
        {"Long power down", {{"speed", 0}, {"brake", 2400}, {"start", 1200}, {"range", 300}}},
        {"Spin back", {{"speed", -2}, {"brake", 120}, {"start", 250}, {"range", 300}}},
        {"Half speed deck", {{"speed", 0.5f}, {"brake", 200}, {"start", 200}, {"range", 300}}},
        {"Nudge", {{"speed", 1}, {"range", 60}, {"inertia", 25}}},
    });
}

} // namespace pulp
