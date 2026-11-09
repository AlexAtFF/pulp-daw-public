// Hiccup: tricks on a step grid, chosen by a draw that depends on the song position and the seed only.
#include "CreativeTest.h"
using namespace pulp;
using namespace ct;

namespace {
const double kSr = 48000.0;
const size_t kBeat = 24000, kBar = 96000, kStep = 6000;        // at 120 BPM; a step is a sixteenth
const size_t kFade = 96;                                       // 2 ms

// only one trick, every step
std::shared_ptr<Hiccup> only(int trick, int grid = 2) {
    auto h = std::make_shared<Hiccup>();
    h->prepare(kSr);
    for (int k = 0; k < Hiccup::NumTricks; ++k) h->pProb[k]->set(k == trick ? 1.0f : 0.0f);
    h->pGrid->set((float)grid);
    return h;
}
void busyChances(Hiccup& h) {
    h.pProb[Hiccup::Repeat]->set(0.3f); h.pProb[Hiccup::Reverse]->set(0.25f); h.pProb[Hiccup::Stop]->set(0.15f);
    h.pProb[Hiccup::Crush]->set(0.2f); h.pProb[Hiccup::Gate]->set(0.25f);
}
}

TEST(hiccup_same_seed_and_song_position_same_output_and_another_seed_differs) {
    const auto in = busy(8.0);      // four bars
    Hiccup a, b, c;
    for (Hiccup* h : {&a, &b, &c}) { h->prepare(kSr); busyChances(*h); h->pSeed->set(17.0f); }
    c.pSeed->set(18.0f);
    const auto oa = run(a, in), ob = run(b, in), oc = run(c, in);
    CHECK(same(oa, ob));
    CHECK(!same(oa, oc));
    CHECK(!same(oa, in));
    // which trick a step fires depends on the step and the seed only, and most steps fire something with these chances
    int fired = 0, differ = 0, kinds[Hiccup::NumTricks] = {};
    for (int64 s = 0; s < 64; ++s) {
        const int k = a.trickAt(s);
        CHECK(k == b.trickAt(s) && k >= Hiccup::None && k < Hiccup::NumTricks);
        if (k != Hiccup::None) { ++fired; ++kinds[k]; }
        differ += k != c.trickAt(s);
    }
    CHECK_MSG(fired > 30 && fired < 62, "%d of 64 steps fire", fired);
    CHECK_MSG(differ > 20, "only %d of 64 steps differ with another seed", differ);
    for (int k = 0; k < Hiccup::NumTricks; ++k) CHECK_MSG(kinds[k] > 0, "%s never fires", Hiccup::trickName(k));
    // the pattern comes round with the cycle: every four bars by default, every bar when asked, never when asked
    for (int64 s = 0; s < 64; ++s) CHECK(a.trickAt(s) == a.trickAt(s + 64) && a.trickAt(s) == a.trickAt(s - 64));
    a.pCycle->set(0.0f);
    for (int64 s = 0; s < 16; ++s) CHECK(a.trickAt(s) == a.trickAt(s + 16));
    a.pCycle->set(4.0f);
    int sameLater = 0;
    for (int64 s = 0; s < 64; ++s) sameLater += a.trickAt(s) == a.trickAt(s + 64);
    CHECK(sameLater < 50);
    a.pCycle->set(2.0f);
    // the chances are honoured: over many steps a trick alone at 30 % fires on about 30 % of them
    Hiccup p;
    p.pCycle->set(4.0f);
    for (int k = 0; k < Hiccup::NumTricks; ++k) p.pProb[k]->set(k == Hiccup::Gate ? 0.3f : 0.0f);
    int n = 0;
    for (int64 s = 0; s < 20000; ++s) n += p.trickAt(s) == Hiccup::Gate;
    CHECK_MSG(std::fabs(n / 20000.0 - 0.3) < 0.015, "a 30 %% chance fired on %.1f %% of the steps", n / 200.0);

    // starting in the middle of the song: the same audio as that stretch of the run from the start, once the tape
    // holds what the tricks reach back for (two steps at most)
    Hiccup m;
    m.prepare(kSr);
    busyChances(m);
    m.pSeed->set(17.0f);
    Transport mid;
    mid.startTick = 2.0 * 4.0 * kPPQ + 100.0;       // bar three, and not on a step: it starts inside one
    const size_t from = 2 * kBar + 2500;
    const auto part = run(m, slice(in, from, 4 * kBar - 2500 + 2500), mid);
    double worst = 0.0;
    for (size_t i = 3 * kStep; i < part.l.size(); ++i)
        worst = std::max(worst, std::max(std::fabs((double)part.l[i] - oa.l[from + i]), std::fabs((double)part.r[i] - oa.r[from + i])));
    printf("    hiccup from the middle of bar 3: largest difference from the full run %.2e\n", worst);
    CHECK_MSG(worst < 1.0e-6, "difference %g", worst);
    // the same through a song that loops back: bar one after a jump sounds as bar one did (after the fill)
    Hiccup l;
    l.prepare(kSr);
    busyChances(l);
    l.pSeed->set(17.0f);
    l.pCycle->set(0.0f);
    Stereo twice = slice(in, 0, kBar);
    append(twice, slice(in, 0, kBar));
    Stereo looped = twice;
    for (size_t i = 0; i < looped.l.size(); i += kQuantum) {
        ProcessContext ctx;
        ctx.sampleRate = kSr; ctx.tempo = 120.0; ctx.playing = true;
        ctx.tickPos = (double)(i % kBar) * 0.04;
        ctx.io[0] = looped.l.data() + i; ctx.io[1] = looped.r.data() + i;
        l.process(ctx);
    }
    worst = 0.0;
    for (size_t i = 3 * kStep; i < kBar; ++i) worst = std::max(worst, std::fabs((double)looped.l[kBar + i] - looped.l[i]));
    CHECK_MSG(worst < 1.0e-6, "the second pass of a looped bar differs by %g", worst);
}

TEST(hiccup_with_every_chance_at_zero_is_the_input) {
    const auto in = busy(4.0);
    Hiccup h;
    h.prepare(kSr);
    for (int k = 0; k < Hiccup::NumTricks; ++k) h.pProb[k]->set(0.0f);
    h.pMix->set(0.6f);
    for (int grid = 0; grid < 4; ++grid) {
        h.pGrid->set((float)grid);
        h.reset();
        Transport t;
        t.tempo = 97.3;
        CHECK_MSG(same(run(h, in, t), in), "grid %d", grid);
    }
    CHECK(h.meter(0) == 0.0f);
    // every chance at 100 %: stopped it is still the input, and so it is with the mix at 0
    for (int k = 0; k < Hiccup::NumTricks; ++k) h.pProb[k]->set(1.0f);
    h.reset();
    Transport stopped;
    stopped.playing = false;
    CHECK(same(run(h, in, stopped), in));
    h.reset();
    h.pMix->set(0.0f);
    CHECK(same(run(h, in), in));
    h.pMix->set(1.0f);
    h.reset();
    CHECK(!same(run(h, in), in));
    CHECK(h.meter(0) >= 1.0f);
    CHECK(h.latencySamples() == 0);
}

TEST(hiccup_repeat_plays_the_last_slice_again_and_can_fall_in_pitch) {
    const auto in = noise(2.0, 0.4f);
    {   // half-step repeats: each step plays the 3000 samples before it, twice
        auto h = only(Hiccup::Repeat);
        h->pRepLen->set(1.0f);
        const auto out = run(*h, in);
        const size_t len = kStep / 2;
        size_t wrong = 0;
        for (size_t s = 1; s < 16; ++s)
            for (size_t j = 0; j < kStep; ++j) {
                if (j % len < kFade) continue;          // the crossfades: at the step's start and where the slice comes round
                wrong += out.l[s * kStep + j] != in.l[s * kStep - len + j % len] || out.r[s * kStep + j] != in.r[s * kStep - len + j % len];
            }
        CHECK_MSG(wrong == 0, "%d samples are not the repeated slice", (int)wrong);
        // the very first step has nothing recorded before it to repeat: it is left alone
        bool first = true;
        for (size_t j = 0; j < kStep; ++j) first = first && out.l[j] == in.l[j];
        CHECK(first);
    }
    for (int choice = 0; choice < 5; ++choice) {    // every repeat length: the slice comes round that many times
        auto h = only(Hiccup::Repeat);
        h->pRepLen->set((float)choice);
        const int div = choice == 0 ? 1 : (choice == 1 ? 2 : (choice == 2 ? 3 : (choice == 3 ? 4 : 8)));
        const auto out = run(*h, in);
        const size_t len = kStep / (size_t)div, t0 = 5 * kStep;
        const size_t fi = std::min<size_t>(kFade, len / 4);
        size_t wrong = 0;
        for (size_t j = kFade; j < kStep; ++j) {
            if (j % len < fi) continue;
            wrong += out.l[t0 + j] != in.l[t0 - len + j % len];
        }
        CHECK_MSG(wrong == 0, "repeat length 1/%d: %d samples wrong", div, (int)wrong);
    }
    {   // an octave down per repeat: the second half of each step is the slice at half speed
        auto h = only(Hiccup::Repeat);
        h->pRepLen->set(1.0f);
        h->pPitch->set(12.0f);
        const auto out = run(*h, tone(880.0, 2.0, 0.5f));
        const double first = tu::measureHz(out.l, 4 * kStep + 200, 4 * kStep + 2900, kSr), second = tu::measureHz(out.l, 4 * kStep + 3200, 4 * kStep + 5900, kSr);
        CHECK_NEAR(first, 880.0, 1.0);
        CHECK_NEAR(second, 440.0, 1.0);
        h->reset();
        h->pPitch->set(7.0f);
        h->pRepLen->set(3.0f);      // four repeats: 0, -7, -14, -21 semitones
        const auto fall = run(*h, tone(1760.0, 2.0, 0.5f));
        for (int rep = 0; rep < 4; ++rep) {
            const double hz = tu::measureHz(fall.l, 4 * kStep + (size_t)rep * 1500 + 150, 4 * kStep + (size_t)rep * 1500 + 1450, kSr);
            CHECK_MSG(std::fabs(1200.0 * std::log2(hz / 1760.0) + 700.0 * rep) < 15.0, "repeat %d at %.1f Hz", rep, hz);
        }
    }
}

TEST(hiccup_reverse_stop_crush_and_gate_do_what_they_say) {
    const auto in = noise(2.0, 0.4f);
    {   // reverse: each step is the step before it, backwards
        auto h = only(Hiccup::Reverse);
        const auto out = run(*h, in);
        size_t wrong = 0;
        for (size_t s = 1; s < 16; ++s)
            for (size_t j = kFade; j < kStep; ++j) wrong += out.l[s * kStep + j] != in.l[s * kStep - 1 - j] || out.r[s * kStep + j] != in.r[s * kStep - 1 - j];
        CHECK_MSG(wrong == 0, "%d samples are not the step before, reversed", (int)wrong);
        // half intensity: half the trick, half the input
        h->reset();
        h->pIntensity->set(0.5f);
        const auto half = run(*h, in);
        double worst = 0.0;
        for (size_t j = kFade; j < kStep; ++j) worst = std::max(worst, std::fabs((double)half.l[3 * kStep + j] - 0.5 * (in.l[3 * kStep - 1 - j] + in.l[3 * kStep + j])));
        CHECK(worst < 1e-6);
        h->reset();
        h->pIntensity->set(0.0f);
        CHECK(same(run(*h, in), in));
    }
    {   // tape stop on quarter notes: the speed falls in a straight line to nothing over the beat
        auto h = only(Hiccup::Stop, 0);
        const double f = 1000.0;
        const auto out = run(*h, tone(f, 2.0, 0.5f));
        // (while the sound is less than a sample behind, the first 220 samples of the stop, it is read with a straight
        // line between two samples: good to 1.1e-3 at 1 kHz. After that the reading is far finer.)
        double worst = 0.0, worstLater = 0.0;
        for (size_t s = 1; s < 4; ++s)
            for (size_t j = kFade; j < kBeat; ++j) {
                const double u = (double)j / kBeat, src = (double)(s * kBeat) + kBeat * (u - 0.5 * u * u);
                double g = std::min(1.0, (1.0 - u) / 0.15);
                g = g * g * (3.0 - 2.0 * g);
                const double d = std::fabs((double)out.l[s * kBeat + j] - 0.5 * g * std::sin(kTwoPi * f * src / kSr));
                worst = std::max(worst, d);
                if (j > 400) worstLater = std::max(worstLater, d);
            }
        CHECK_MSG(worst < 1.5e-3 && worstLater < 5.0e-5, "the stop is off its curve by %g (%g after the first 400 samples)", worst, worstLater);
        const double quarter = tu::measureHz(out.l, kBeat + 5500, kBeat + 6500, kSr), half = tu::measureHz(out.l, kBeat + 11500, kBeat + 12500, kSr);
        CHECK_NEAR(quarter, 750.0, 15.0);
        CHECK_NEAR(half, 500.0, 15.0);
        CHECK(peak(out, 2 * kBeat - 40, 2 * kBeat) < 1.0e-3f);
        printf("    hiccup tape stop: worst difference from its curve %.2e (%.2e after the first 400 samples); %.1f Hz a quarter of the way, %.1f Hz half way (1000 Hz in)\n", worst, worstLater, quarter, half);
    }
    {   // bit crush at full intensity: held for 32 samples from the step's start, in steps of a quarter of full scale
        auto h = only(Hiccup::Crush);
        CHECK(Hiccup::crushHold(1.0f, kSr) == 32 && Hiccup::crushLevels(1.0f) == 4.0f);
        CHECK(Hiccup::crushHold(0.0f, kSr) == 1 && Hiccup::crushLevels(0.0f) == 32768.0f);
        const auto out = run(*h, in);
        size_t wrong = 0;
        for (size_t s = 1; s < 16; ++s)
            for (size_t j = kFade; j < kStep; ++j) {
                const float want = std::round(in.l[s * kStep + j / 32 * 32] * 4.0f) / 4.0f;
                wrong += out.l[s * kStep + j] != want;
            }
        CHECK_MSG(wrong == 0, "%d samples are not held and quantised", (int)wrong);
        // gentler at half intensity: nine samples, 8.5 bits
        h->reset();
        h->pIntensity->set(0.5f);
        const int hold = Hiccup::crushHold(0.5f, kSr);
        const float levels = Hiccup::crushLevels(0.5f);
        CHECK(hold == 9);
        CHECK_NEAR(levels, std::pow(2.0, 8.5), 0.01);
        const auto mild = run(*h, in);
        wrong = 0;
        for (size_t j = kFade; j < kStep; ++j) wrong += mild.l[2 * kStep + j] != std::round(in.l[2 * kStep + j / (size_t)hold * (size_t)hold] * levels) / levels;
        CHECK(wrong == 0);
    }
    {   // gate: the first half of each step is the input, the second half is silent
        auto h = only(Hiccup::Gate);
        const auto out = run(*h, in);
        size_t wrong = 0;
        for (size_t s = 0; s < 16; ++s) {
            for (size_t j = 0; j < kStep / 2; ++j) wrong += out.l[s * kStep + j] != in.l[s * kStep + j];
            for (size_t j = kStep / 2 + kFade; j < kStep - kFade; ++j) wrong += out.l[s * kStep + j] != 0.0f;
        }
        CHECK_MSG(wrong == 0, "%d samples wrong", (int)wrong);
        // half intensity: the closed half is at half level
        h->reset();
        h->pIntensity->set(0.5f);
        const auto half = run(*h, dc(1.0, 1.0f));
        CHECK_NEAR(half.l[kStep + kStep * 3 / 4], 0.5, 1e-6);
    }
}

TEST(hiccup_tricks_start_and_end_without_clicks) {
    // a sine through every trick but the crusher (whose steps are the point of it): no step from one sample to the
    // next beyond the sine's own plus what a 2 ms crossfade between two full-level signals can add
    const float amp = 0.5f;
    const double f = 1000.0;
    const float own = amp * (float)(kTwoPi * f / kSr);
    const float bound = own + 2.0f * amp / (float)kFade + 0.002f;
    float worstAll = 0.0f;
    for (int grid = 0; grid < 4; ++grid)
        for (uint32 seed : {1u, 2u, 3u}) {
            Hiccup h;
            h.prepare(kSr);
            busyChances(h);
            h.pProb[Hiccup::Crush]->set(0.0f);
            h.pGrid->set((float)grid);
            h.pSeed->set((float)seed);
            h.pRepLen->set((float)(seed % 5));
            h.pPitch->set(seed == 2 ? 5.0f : 0.0f);
            Transport t;
            t.tempo = seed == 3 ? 171.3 : 120.0;
            // one bar in before the measurement starts, so the tape is full
            const auto out = run(h, tone(f, 6.0, amp), t);
            const float worst = maxStep(out.l, kBar);
            worstAll = std::max(worstAll, worst);
            CHECK_MSG(worst <= bound, "grid %d seed %u: step %.4f, bound %.4f", grid, seed, worst, bound);
        }
    printf("    hiccup: largest step with a 1 kHz sine at 0.5 over all tricks but the crusher %.4f (the sine alone %.4f, bound %.4f)\n", worstAll, own, bound);
    {   // stopping the song in the middle of a trick: a 2 ms fade to the input, then the input exactly
        auto h = only(Hiccup::Reverse);
        const auto in = tone(f, 2.0, amp);
        Stereo out = in;
        const size_t stopAt = (kBar / 2 + 1000) / kQuantum * kQuantum;
        for (size_t i = 0; i < out.l.size(); i += kQuantum) {
            ProcessContext c;
            c.sampleRate = kSr; c.tempo = 120.0;
            c.playing = i < stopAt;
            c.tickPos = c.playing ? (double)i * 0.04 : 0.0;
            c.io[0] = out.l.data() + i; c.io[1] = out.r.data() + i;
            h->process(c);
        }
        CHECK(maxStep(out.l, kBar / 4) <= bound);
        bool exact = true;
        for (size_t i = stopAt + kFade; i < out.l.size(); ++i) exact = exact && out.l[i] == in.l[i];
        CHECK(exact);
        CHECK(out.l[stopAt + 10] != in.l[stopAt + 10]);     // it did fade: it was not cut
    }
}
