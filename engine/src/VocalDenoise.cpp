// Lint: noise reduction. While "learn" is on it averages the spectrum of what passes through (and lets the
// sound through untouched); afterwards it takes that profile out by spectral subtraction.
#include "pulp/Vocal.h"
#include <thread>

namespace pulp {
using namespace vocal;

namespace {
constexpr int kNoData = -3000;          // tenths of a dB: "nothing known about this bin"
struct Hold {                           // the short hand-over lock; the audio thread only ever tries it
    std::atomic<bool>& f;
    explicit Hold(std::atomic<bool>& flag) : f(flag) { while (f.exchange(true, std::memory_order_acquire)) std::this_thread::yield(); }
    ~Hold() { f.store(false, std::memory_order_release); }
};
}

Lint::Lint() {
    pLearn = addBool("learn", "Learn noise", false);
    pAmount = addFloat("amount", "Reduction strength", 0, 4, 2, "x");
    pFloor = addFloat("floor", "Maximum reduction", 0, 60, 24, "dB");
    pTime = addFloat("tsmooth", "Time smoothing", 0, 500, 80, "ms", 2.0f);
    pFreq = addFloat("fsmooth", "Frequency smoothing", 0, 400, 60, "Hz", 2.0f);
    pAttack = addFloat("attack", "Attack", 0, 200, 10, "ms", 2.0f);
    pRelease = addFloat("release", "Release", 10, 2000, 200, "ms", 3.0f);
    for (int ch = 0; ch < 2; ++ch) {
        mine_.q[ch].assign((size_t)kMaxBins, (short)kNoData);
        shared_.q[ch].assign((size_t)kMaxBins, (short)kNoData);
    }
}

std::vector<ParamGroup> Lint::paramGroups() const {
    return {{"Profile", {"learn"}},
            {"Reduction", {"amount", "floor"}},
            {"Smoothing", {"tsmooth", "fsmooth", "attack", "release"}}};
}

static void copyProfileFields(bool& dv, bool& ds, double& dr, int& db, std::vector<short>* dq,
                              bool sv, bool ss, double sr, int sb, const std::vector<short>* sq) {
    dv = sv; ds = ss; dr = sr; db = sb;
    for (int ch = 0; ch < 2; ++ch) std::memcpy(dq[ch].data(), sq[ch].data(), sizeof(short) * (size_t)Lint::kMaxBins);
}

void Lint::prepare(double sr) {
    sr_ = sr;
    n_ = sr <= 50000.0 ? 2048 : (sr <= 100000.0 ? 4096 : 8192);
    hop_ = n_ / 4;
    bins_ = n_ / 2 + 1;
    latency_ = n_ - kQuantum;
    fft_.prepare(n_);
    win_.assign((size_t)n_, 0.0f);
    winPower_ = 0.0;
    for (int i = 0; i < n_; ++i) {
        win_[(size_t)i] = (float)std::sqrt(0.5 - 0.5 * std::cos(kTwoPi * i / n_));
        winPower_ += (double)win_[(size_t)i] * win_[(size_t)i];
    }
    re_.assign((size_t)n_, 0.0f);
    im_.assign((size_t)n_, 0.0f);
    pw_.assign((size_t)bins_, 0.0f);
    prefix_.assign((size_t)bins_ + 1, 0.0);
    for (int ch = 0; ch < 2; ++ch) {
        in_[ch].assign((size_t)n_, 0.0f);
        acc_[ch].assign((size_t)n_, 0.0f);
        fr_[ch].assign((size_t)n_, 0.0f);
        noise_[ch].assign((size_t)bins_, 0.0f);
        pt_[ch].assign((size_t)bins_, 0.0f);
        gain_[ch].assign((size_t)bins_, 1.0f);
        learn_[ch].assign((size_t)bins_, 0.0);
    }
    {
        Hold h(busy_);
        copyProfileFields(mine_.valid, mine_.same, mine_.rate, mine_.bins, mine_.q, shared_.valid, shared_.same, shared_.rate, shared_.bins, shared_.q);
        seenSerial_ = loadSerial_.load();
    }
    publishPending_ = false;
    learning_ = false;
    learnFrames_ = 0;
    applyProfile();
    reset();
}

void Lint::reset() {
    for (int ch = 0; ch < 2; ++ch) {
        std::fill(in_[ch].begin(), in_[ch].end(), 0.0f);
        std::fill(acc_[ch].begin(), acc_[ch].end(), 0.0f);
    }
    count_ = 0;
    framesSince_ = 0;
    gainsPrimed_ = false;
    primed_ = false;
}

// Working noise power per bin for the current rate and frame size, from the stored profile.
void Lint::applyProfile() {
    mHasProfile_.store(mine_.valid ? 1.0f : 0.0f, std::memory_order_relaxed);
    if (!mine_.valid || mine_.bins < 2) { mine_.valid = false; mHasProfile_.store(0.0f, std::memory_order_relaxed); return; }
    const double scale = sr_ * winPower_;
    const bool exact = mine_.rate == sr_ && mine_.bins == bins_;
    const double storedBinHz = mine_.rate / (2.0 * (mine_.bins - 1)), binHz = sr_ / n_;
    for (int ch = 0; ch < 2; ++ch) {
        const short* q = mine_.q[mine_.same ? 0 : ch].data();
        float* dst = noise_[ch].data();
        for (int k = 0; k < bins_; ++k) {
            double tenths;
            if (exact) tenths = q[k];
            else {
                const double pos = k * binHz / storedBinHz;
                const int i = (int)pos;
                if (i >= mine_.bins - 1) tenths = pos <= (double)(mine_.bins - 1) ? q[mine_.bins - 1] : kNoData;
                else tenths = (q[i] <= kNoData || q[i + 1] <= kNoData) ? kNoData : q[i] + (pos - i) * (q[i + 1] - q[i]);
            }
            dst[k] = tenths <= (double)kNoData ? 0.0f : (float)(std::pow(10.0, tenths / 100.0) * scale);
        }
    }
}

// Turns what was averaged while learning into the stored profile (rounded to a tenth of a decibel, so that
// a saved and reloaded profile works exactly like the one just learned).
void Lint::finishLearning() {
    if (learnFrames_ < 1) return;       // nothing was heard: keep whatever profile there was
    const double scale = 1.0 / ((double)learnFrames_ * sr_ * winPower_);
    for (int ch = 0; ch < 2; ++ch) {
        short* q = mine_.q[ch].data();
        const double* sum = learn_[ch].data();
        for (int k = 0; k < bins_; ++k) {
            const double d = sum[k] * scale;
            q[k] = d > 1.0e-30 ? (short)clampT((long)std::lround(100.0 * std::log10(d)), (long)kNoData, 300L) : (short)kNoData;
        }
        for (int k = bins_; k < kMaxBins; ++k) q[k] = (short)kNoData;
    }
    mine_.valid = true;
    mine_.rate = sr_;
    mine_.bins = bins_;
    mine_.same = std::memcmp(mine_.q[0].data(), mine_.q[1].data(), sizeof(short) * (size_t)bins_) == 0;
    applyProfile();
    publishPending_ = true;
    tryPublish();
}

bool Lint::tryPublish() {
    if (busy_.exchange(true, std::memory_order_acquire)) return false;
    copyProfileFields(shared_.valid, shared_.same, shared_.rate, shared_.bins, shared_.q, mine_.valid, mine_.same, mine_.rate, mine_.bins, mine_.q);
    busy_.store(false, std::memory_order_release);
    publishPending_ = false;
    return true;
}

Json Lint::saveExtra() const {
    Json j = Json::object();
    Hold h(busy_);
    if (!shared_.valid) { j.set("learned", false); return j; }
    j.set("learned", true);
    j.set("rate", shared_.rate);
    j.set("bins", shared_.bins);
    Json l = Json::array();
    for (int k = 0; k < shared_.bins; ++k) l.push((int)shared_.q[0][(size_t)k]);
    j.set("noise", l);          // noise power density per bin, in tenths of a dB
    if (!shared_.same) {
        Json r = Json::array();
        for (int k = 0; k < shared_.bins; ++k) r.push((int)shared_.q[1][(size_t)k]);
        j.set("noiseR", r);
    }
    return j;
}

void Lint::loadExtra(const Json& j, const std::string&) {
    if (!j.isObject()) return;          // a preset carries no profile: keep the one we have
    const int bins = j["bins"].i(0);
    const Json& l = j["noise"];
    const Json& r = j["noiseR"];
    const bool valid = j["learned"].b(false) && bins >= 2 && bins <= kMaxBins && l.isArray() && (int)l.size() == bins;
    const bool same = !(r.isArray() && (int)r.size() == bins);
    Hold h(busy_);
    shared_.valid = valid;
    shared_.same = same;
    shared_.rate = j["rate"].num(48000.0);
    shared_.bins = valid ? bins : 0;
    for (int ch = 0; ch < 2; ++ch) std::fill(shared_.q[ch].begin(), shared_.q[ch].end(), (short)kNoData);
    if (valid) {
        for (int k = 0; k < bins; ++k) {
            shared_.q[0][(size_t)k] = (short)clampT(l.at((size_t)k).i(kNoData), kNoData, 300);
            shared_.q[1][(size_t)k] = same ? shared_.q[0][(size_t)k] : (short)clampT(r.at((size_t)k).i(kNoData), kNoData, 300);
        }
    }
    loadSerial_.fetch_add(1);
    mHasProfile_.store(valid ? 1.0f : 0.0f, std::memory_order_relaxed);
}

// One analysis frame: the newest n_ samples of each channel.
void Lint::frame() {
    const int N = n_, mask = N - 1, start = (int)(count_ & mask);
    ++framesSince_;
    const bool needSpectrum = active_ || learning_;
    const bool sameIn = std::memcmp(in_[0].data(), in_[1].data(), sizeof(float) * (size_t)N) == 0;
    const bool mono = sameIn && (!mine_.valid || mine_.same);
    const float* win = win_.data();
    float* re = re_.data();
    float* im = im_.data();
    float* pw = pw_.data();

    const float alpha = pAmount->get();
    const float floorGain = dbToGain(-pFloor->get());
    const double hopSec = (double)hop_ / sr_;
    const float tau = 0.001f * pTime->get();
    const float lam = tau > 0.0f ? (float)std::exp(-hopSec / tau) : 0.0f;
    const float ta = 0.001f * pAttack->get(), tr = 0.001f * pRelease->get();
    const float cA = ta > 0.0f ? (float)(1.0 - std::exp(-hopSec / ta)) : 1.0f;
    const float cR = tr > 0.0f ? (float)(1.0 - std::exp(-hopSec / tr)) : 1.0f;
    const double hw = (double)pFreq->get() / (sr_ / N);
    const int wi = (int)hw;
    const double wf = hw - wi;
    double gainSum = 0.0;

    for (int ch = 0; ch < (mono ? 1 : 2); ++ch) {
        const float* x = in_[ch].data();
        float* out = fr_[ch].data();
        if (!needSpectrum) {
            // nothing to do to the sound: the frame goes back as it came (same windows, so the overlap-add stays whole)
            for (int i = 0; i < N; ++i) out[i] = x[(start + i) & mask] * win[i] * win[i] * 0.5f;
            continue;
        }
        for (int i = 0; i < N; ++i) { re[i] = x[(start + i) & mask] * win[i]; im[i] = 0.0f; }
        fft_.run(re, im, false);
        for (int k = 0; k < bins_; ++k) pw[k] = re[k] * re[k] + im[k] * im[k];
        if (learning_ && framesSince_ >= 4) {
            double* sum = learn_[ch].data();
            for (int k = 0; k < bins_; ++k) sum[k] += pw[k];
            if (mono) { double* s1 = learn_[1].data(); for (int k = 0; k < bins_; ++k) s1[k] += pw[k]; }
        }
        if (!active_) {
            for (int i = 0; i < N; ++i) out[i] = x[(start + i) & mask] * win[i] * win[i] * 0.5f;
            continue;
        }
        float* pt = pt_[ch].data();
        float* g = gain_[ch].data();
        const float* nz = noise_[ch].data();
        // smooth the power over time, then across neighbouring bins: a steadier estimate of what is there
        if (!gainsPrimed_) for (int k = 0; k < bins_; ++k) pt[k] = pw[k];
        else for (int k = 0; k < bins_; ++k) { float v = pw[k] + lam * (pt[k] - pw[k]); if (v < 1.0e-30f) v = 0.0f; pt[k] = v; }
        double* pre = prefix_.data();
        const bool smoothF = wi > 0 || wf > 0.0;
        if (smoothF) { pre[0] = 0.0; for (int k = 0; k < bins_; ++k) pre[k + 1] = pre[k] + pt[k]; }
        for (int k = 0; k < bins_; ++k) {
            double pe = pt[k];
            if (smoothF) {
                const int a = std::max(0, k - wi), b = std::min(bins_ - 1, k + wi);
                double sum = pre[b + 1] - pre[a], cnt = b - a + 1;
                if (wf > 0.0) {
                    if (k - wi - 1 >= 0) { sum += wf * pt[k - wi - 1]; cnt += wf; }
                    if (k + wi + 1 < bins_) { sum += wf * pt[k + wi + 1]; cnt += wf; }
                }
                pe = sum / cnt;
            }
            const double sub = (double)alpha * nz[k];
            const float raw = pe > sub && pe > 0.0 ? (float)std::sqrt(1.0 - sub / pe) : 0.0f;
            const float target = std::max(raw, floorGain);
            float gk = g[k];
            if (!gainsPrimed_) gk = target;
            else gk += (target - gk) * (target > gk ? cA : cR);
            g[k] = gk;
            gainSum += gk;
            re[k] *= gk; im[k] *= gk;
            if (k > 0 && k < N / 2) { re[N - k] *= gk; im[N - k] *= gk; }
        }
        fft_.run(re, im, true);
        for (int i = 0; i < N; ++i) out[i] = re[i] * win[i] * 0.5f;
    }
    if (active_) {
        gainsPrimed_ = true;
        if (mono) {
            std::memcpy(pt_[1].data(), pt_[0].data(), sizeof(float) * (size_t)bins_);
            std::memcpy(gain_[1].data(), gain_[0].data(), sizeof(float) * (size_t)bins_);
        }
        const double mean = gainSum / ((mono ? 1.0 : 2.0) * bins_);
        mReduction_.store(mean > 1.0e-6 ? (float)(-20.0 * std::log10(mean)) : 120.0f, std::memory_order_relaxed);
    } else {
        mReduction_.store(0.0f, std::memory_order_relaxed);
    }
    if (learning_ && framesSince_ >= 4) ++learnFrames_;
    for (int ch = 0; ch < 2; ++ch) {
        const float* src = fr_[mono ? 0 : ch].data();
        float* acc = acc_[ch].data();
        for (int i = 0; i < N; ++i) acc[(start + i) & mask] += src[i];
    }
}

void Lint::process(ProcessContext& c) {
    const int n = c.numSamples;
    // a profile loaded from a project, or one of ours still waiting to be handed over
    if (loadSerial_.load(std::memory_order_relaxed) != seenSerial_) {
        if (!busy_.exchange(true, std::memory_order_acquire)) {
            copyProfileFields(mine_.valid, mine_.same, mine_.rate, mine_.bins, mine_.q, shared_.valid, shared_.same, shared_.rate, shared_.bins, shared_.q);
            seenSerial_ = loadSerial_.load();
            busy_.store(false, std::memory_order_release);
            publishPending_ = false;
            applyProfile();
            gainsPrimed_ = false;
        }
    } else if (publishPending_) {
        tryPublish();
    }
    const bool learn = pLearn->getBool();
    if (learn && !learning_) {
        learning_ = true;
        learnFrames_ = 0;
        for (auto& l : learn_) std::fill(l.begin(), l.end(), 0.0);
    } else if (!learn && learning_) {
        learning_ = false;
        finishLearning();
        gainsPrimed_ = false;
    }
    const bool was = active_;
    active_ = mine_.valid && !learning_;
    if (active_ && !was) gainsPrimed_ = false;
    if (!primed_) { wet_ = active_ ? 1.0f : 0.0f; primed_ = true; }

    float* io[2] = {c.io[0], c.io[1]};
    const int mask = n_ - 1;
    const int w = (int)(count_ & mask);
    for (int ch = 0; ch < 2; ++ch) {                // n_ is a multiple of the quantum, so a quantum never wraps
        float* dst = in_[ch].data() + w;
        for (int i = 0; i < n; ++i) dst[i] = std::isfinite(io[ch][i]) ? io[ch][i] : 0.0f;
    }
    count_ += n;
    if ((count_ & (hop_ - 1)) == 0) frame();

    // the oldest n samples in the rings are now final: out they go, n_ - 64 samples after they came in
    const int r = (int)(count_ & mask);
    const float target = active_ ? 1.0f : 0.0f, step = 1.0f / (float)hop_;
    float wet = wet_;
    for (int ch = 0; ch < 2; ++ch) {
        const float* dry = in_[ch].data() + r;
        float* acc = acc_[ch].data() + r;
        float* out = io[ch];
        wet = wet_;
        for (int i = 0; i < n; ++i) {
            if (wet < target) wet = std::min(target, wet + step);
            else if (wet > target) wet = std::max(target, wet - step);
            const float d = dry[i];
            out[i] = wet <= 0.0f ? d : d + wet * (acc[i] - d);
            acc[i] = 0.0f;
        }
    }
    wet_ = wet;
    mLearning_.store(learning_ ? 1.0f : 0.0f, std::memory_order_relaxed);
    mFrames_.store((float)learnFrames_, std::memory_order_relaxed);
    scope_.push(io[0], io[1], n);
}

std::vector<Preset> Lint::factoryPresets() const {
    return buildPresets(*this, {
        {"Everyday clean-up", {}},
        {"Light touch, 9 dB", {{"amount", 1.5f}, {"floor", 9}, {"tsmooth", 60}, {"fsmooth", 40}}},
        {"Heavy hiss removal", {{"amount", 3.0f}, {"floor", 36}, {"tsmooth", 120}, {"fsmooth", 120}, {"release", 300}}},
        {"Hum and buzz lines", {{"amount", 2.5f}, {"floor", 30}, {"tsmooth", 150}, {"fsmooth", 0}, {"attack", 20}, {"release", 400}}},
        {"Speech, quick to open", {{"amount", 2.0f}, {"floor", 18}, {"tsmooth", 30}, {"fsmooth", 60}, {"attack", 0}, {"release", 120}}},
        {"Slow and smooth for pads", {{"amount", 2.0f}, {"floor", 20}, {"tsmooth", 250}, {"fsmooth", 100}, {"attack", 40}, {"release", 800}}},
        {"Deep silence between phrases", {{"amount", 3.5f}, {"floor", 60}, {"tsmooth", 100}, {"fsmooth", 80}, {"attack", 5}, {"release", 250}}},
        {"Plain subtraction (no smoothing)", {{"amount", 1.0f}, {"floor", 24}, {"tsmooth", 0}, {"fsmooth", 0}, {"attack", 0}, {"release", 10}}},
    });
}

} // namespace pulp
