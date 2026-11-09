// What every tone effect must do: presets, state, silence, latency, transparency at rest, automation, and
// identical renders at any block size and thread count with no allocation on the audio thread.
#include "ToneTest.h"
#include <set>
using namespace pulp;
using namespace tu;
using namespace tt;

namespace {
const double kSr = 48000.0;

int latencyAt(const char* id, double sr) {
    const std::string k = id;
    if (k == "geq" || k == "crush" || k == "midside") return 0;
    if (k == "tape") return 32 + (int)std::lround(QuarterInch::kDelaySeconds * sr);
    return 32;
}
void hissOff(Plugin& p) { if (Param* h = p.param("hiss")) h->set(h->info.min); }
}

TEST(tone_oversampling_tables_are_exactly_32_samples_late) {
    // Every table: a click goes up, through as many half-sample-late stages as the table was made for, and down
    // again. It comes back centred on exactly 32 samples later, whatever the factor and the number of stages.
    double worstSym = 0.0, worstDc = 0.0, droop15 = 0.0, droop20 = 0.0;
    for (int factor : {2, 4, 8}) {
        for (int stages = 0; stages <= 4; ++stages) {
            const tonefx::OsTables* t = tonefx::osTables(factor, stages);
            CHECK(t->factor == factor && t->halfDelays == stages && t->nDown == 32 * factor + 1 - stages);
            const auto pass = [&](const std::vector<float>& in) {
                tonefx::Oversampler os;
                os.setTables(t);
                os.reset();
                std::vector<double> prev((size_t)stages, 0.0);
                std::vector<float> out(in.size());
                for (size_t i = 0; i < in.size(); ++i) {
                    float f[tonefx::Oversampler::kMaxFactor];
                    os.up(in[i], f);
                    for (int p = 0; p < factor; ++p) {
                        double v = f[p];
                        for (int s = 0; s < stages; ++s) { const double avg = 0.5 * (v + prev[(size_t)s]); prev[(size_t)s] = v; v = avg; }     // what a first-order anti-aliased stage does to a quiet signal
                        f[p] = (float)v;
                    }
                    out[i] = os.down(f);
                }
                return out;
            };
            std::vector<float> click(160, 0.0f);
            click[40] = 1.0f;
            const auto y = pass(click);
            size_t peak = 0;
            double sum = 0.0;
            for (size_t i = 0; i < y.size(); ++i) { if (std::fabs(y[i]) > std::fabs(y[peak])) peak = i; sum += y[i]; }
            CHECK_MSG(peak == 72, "factor %d, %d stages: peak %d samples late", factor, stages, (int)peak - 40);
            for (int k = 1; k < 32; ++k) worstSym = std::max(worstSym, (double)std::fabs(y[72 - k] - y[72 + k]));
            CHECK_NEAR(sum, 1.0, 2.0e-5);
            // a steady level comes back as itself
            const auto dc = pass(std::vector<float>(200, 0.37f));
            worstDc = std::max(worstDc, (double)std::fabs(dc.back() - 0.37f));
            // flat across the audible band (less what the stages' own averaging takes off, which is theirs, not the tables')
            for (double hz : {1000.0, 15000.0, 20000.0}) {
                std::vector<float> tn(4096);
                for (size_t i = 0; i < tn.size(); ++i) tn[i] = 0.5f * (float)std::sin(kTwoPi * hz * (double)i / kSr);
                const auto o = pass(tn);
                const double gain = levelDbAt(o, 2048, 2048, hz) - db(0.5) - stages * db(std::cos(kPi * hz / (factor * kSr)));
                if (hz <= 15000.0) { CHECK_MSG(std::fabs(gain) < 0.01, "factor %d, %d stages: %.3f dB at %.0f Hz", factor, stages, gain, hz); droop15 = std::max(droop15, std::fabs(gain)); }
                else droop20 = std::max(droop20, std::fabs(gain));
            }
        }
    }
    CHECK_MSG(worstSym < 2.0e-6, "lopsided by %.2g", worstSym);
    CHECK_MSG(worstDc < 1.0e-6, "DC off by %.2g", worstDc);
    CHECK(droop20 < 0.5);
    FIG("oversampling tables (factors 2, 4, 8; 0 to 4 stages): click centred on 32 samples to within %.1e, DC to within %.1e, flat to %.4f dB up to 15 kHz and %.3f dB at 20 kHz (48 kHz)", worstSym, worstDc, droop15, droop20);
}

TEST(tone_identity_parameters_groups_and_latency) {
    for (auto& k : kinds()) {
        auto p = k.make();
        CHECK(std::string(p->typeId()) == k.id);
        CHECK(std::string(p->displayName()) == k.name);
        CHECK(!p->isInstrument() && !p->wantsSidechain());
        // every parameter has a unique id and a name, clamps to its range, starts inside it and shows a value
        std::set<std::string> ids;
        for (auto& prm : p->params()) {
            CHECK_MSG(ids.insert(prm->info.id).second, "%s: duplicate id %s", k.name, prm->info.id.c_str());
            CHECK(!prm->info.name.empty());
            CHECK(prm->info.def >= prm->info.min && prm->info.def <= prm->info.max);
            CHECK(prm->get() == prm->info.def);
            prm->set(prm->info.max + 1000.0f);
            CHECK(prm->get() == prm->info.max);
            prm->set(prm->info.min - 1000.0f);
            CHECK(prm->get() == prm->info.min);
            CHECK(!prm->display().empty());
            prm->set(prm->info.def);
        }
        // groups: where there are any, every parameter sits in exactly one
        const auto groups = p->paramGroups();
        if (p->params().size() > 8) CHECK_MSG(!groups.empty(), "%s has %d parameters and no groups", k.name, (int)p->params().size());
        if (!groups.empty()) {
            std::set<std::string> grouped;
            for (auto& g : groups) {
                CHECK(!g.title.empty() && !g.ids.empty());
                for (auto& id : g.ids) {
                    CHECK_MSG(p->param(id) != nullptr, "%s: group %s names unknown parameter %s", k.name, g.title.c_str(), id.c_str());
                    CHECK_MSG(grouped.insert(id).second, "%s: %s is in two groups", k.name, id.c_str());
                }
            }
            CHECK(grouped == ids);
        }
        // the latency is what the notes say at each rate, and does not depend on any setting
        for (double sr : {44100.0, 48000.0, 96000.0}) {
            p->prepare(sr);
            const int lat = p->latencySamples();
            CHECK_MSG(lat == latencyAt(k.id, sr), "%s at %.0f Hz: latency %d", k.name, sr, lat);
            for (auto& prm : p->params()) {
                prm->set(prm->info.max); CHECK(p->latencySamples() == lat);
                prm->set(prm->info.min); CHECK(p->latencySamples() == lat);
                prm->set(prm->info.def);
            }
        }
        CHECK(k.make()->latencySamples() == k.latency48);
    }
}

TEST(tone_reported_latency_is_the_measured_latency) {
    // With the mix at dry the input comes back bit for bit, late by exactly the reported latency. The effects with
    // no mix control report none, and what goes in comes out in the same sample. (The wet paths are timed in each
    // effect's own tests.)
    for (auto& k : kinds()) {
        for (double sr : {44100.0, 48000.0, 96000.0}) {
            auto p = k.make();
            p->prepare(sr);
            const int lat = p->latencySamples();
            const auto in = noise(0.25, 0.7f, sr);
            if (Param* mix = p->param("mix")) {
                // everything else away from its default, so nothing but the dry path can be what comes out
                dsp::Noise rnd(17u);
                for (auto& prm : p->params()) prm->setNorm(rnd.uni());
                mix->set(0.0f);
                const auto out = runAt(*p, in, sr);
                CHECK_MSG(maxDiffDelayed(out.l, in.l, lat, 0) == 0.0 && maxDiffDelayed(out.r, in.r, lat, 0) == 0.0, "%s at %.0f Hz: dry path is not the input %d samples late", k.name, sr, lat);
                CHECK(peakOf(out.l, 0, (size_t)lat) == 0.0f);
                if (lat > 0) CHECK(maxDiffDelayed(out.l, in.l, lat - 1, 4800) > 0.01 && maxDiffDelayed(out.l, in.l, lat + 1, 4800) > 0.01);
            } else {
                CHECK(lat == 0);
                const auto out = runAt(*p, impulse(0.05, sr, 100, 0.5f), sr);
                size_t first = 0;
                for (size_t i = 0; i < out.l.size(); ++i) if (out.l[i] != 0.0f) { first = i; break; }
                CHECK_MSG(first == 100, "%s at %.0f Hz: a click at sample 100 first shows at %d", k.name, sr, (int)first);
            }
        }
    }
}

TEST(tone_neutral_settings_are_transparent) {
    // Bit for bit where an effect has a setting that does nothing; a stated bound where it has not.
    const auto in = groove(0.5, 0.8f);
    const auto exact = [&](Plugin& p, const char* what) {
        p.prepare(kSr);
        const int lat = p.latencySamples();
        const auto out = run(p, in);
        CHECK_MSG(maxDiffDelayed(out.l, in.l, lat, 0) == 0.0 && maxDiffDelayed(out.r, in.r, lat, 0) == 0.0, "%s is not bit-transparent (off by %g)", what, maxDiffDelayed(out.l, in.l, lat, 0));
    };
    TenBand geq;
    exact(geq, "Ten Band, all sliders at zero");
    MotionFilter mf;
    mf.pMode->set(MotionFilter::Peak); mf.pReso->set(0.0f); mf.pSlope->set(1);
    exact(mf, "Motion Filter, peak mode with no resonance, no drive");
    Coarse crush;
    crush.pBits->set(24.0f);
    exact(crush, "Coarse, 24 bits and the highest rate");
    crush.pMode->set(Coarse::Filtered); crush.pDither->set(1);
    exact(crush, "Coarse, 24 bits and the highest rate, filtered mode with dither");
    Bend bend;
    bend.pOver->set(0);
    exact(bend, "Bend, straight line, oversampling off");
    bend.pSymmetry->set(Bend::Mirror);
    exact(bend, "Bend, straight line mirrored, oversampling off");
    MiddleSides ms;
    exact(ms, "Middle and Sides at rest");
    for (const char* id : {"modfilter", "tape", "tube", "amp", "fuzz", "crush", "shaper"}) {
        auto p = make(id);
        set(*p, "mix", 0.0f);
        exact(*p, id);
    }
    // Bend's straight line through the oversampling filters: the filters are flat to well past the audible band
    {
        double worst = 0.0;
        for (int over = 1; over < 4; ++over) {
            Bend b;
            b.prepare(kSr);
            b.pOver->set((float)over);
            for (double hz : {100.0, 1000.0, 10000.0, 15000.0}) worst = std::max(worst, std::fabs(gainDbAt(b, hz, 0.5f)));
        }
        CHECK_MSG(worst < 0.01, "%.4f dB", worst);
        FIG("bend, straight line, oversampled: within %.4f dB of unity from 100 Hz to 15 kHz", worst);
    }
    // The saturators have no setting that does nothing (other than the mix): with everything at its mildest a
    // quiet signal comes through at unity to within the bounds below.
    {
        Glass g;
        g.prepare(kSr);
        g.pDrive->set(0.0f); g.pBias->set(0.0f); g.pSag->set(0.0f); g.pTone->set(0.0f); g.pOut->set(0.0f); g.pStages->set(0);
        double worst = 0.0;
        for (double hz : {100.0, 1000.0, 10000.0}) worst = std::max(worst, std::fabs(gainDbAt(g, hz, 0.001f)));
        CHECK_MSG(worst < 0.15, "Glass at its mildest: %.3f dB", worst);     // 0.12 dB of it is the stage's own averaging at 10 kHz
        QuarterInch t;
        t.prepare(96000.0);
        t.pDrive->set(0.0f); t.pHyst->set(0.0f); t.pBump->set(0.0f); t.pSpeed->set(3);
        double worstTape = 0.0;
        for (double hz : {200.0, 1000.0, 8000.0}) worstTape = std::max(worstTape, std::fabs(gainDbAt(t, hz, 0.001f, 96000.0)));
        CHECK_MSG(worstTape < 0.6, "Quarter Inch at its mildest: %.3f dB", worstTape);
        FIG("mildest settings, quiet signal: glass within %.3f dB of unity (100 Hz to 10 kHz), quarter inch at 30 ips and 96 kHz within %.3f dB (200 Hz to 8 kHz)", worst, worstTape);
    }
}

TEST(tone_presets_are_stable_and_reload) {
    for (auto& k : kinds()) {
        const auto presets = k.make()->factoryPresets();
        CHECK_MSG((int)presets.size() >= k.minPresets, "%s has %d presets", k.name, (int)presets.size());
        std::set<std::string> names;
        for (auto& pr : presets) {
            CHECK_MSG(names.insert(pr.name).second, "%s: two presets called %s", k.name, pr.name.c_str());
            // every value in the preset is a real parameter and inside its range
            auto probe = k.make();
            for (auto& prm : probe->params()) {
                const float v = pr.state["params"][prm->info.id].f(-1.0e9f);
                CHECK_MSG(v >= prm->info.min && v <= prm->info.max, "%s / %s: %s = %g is out of range", k.name, pr.name.c_str(), prm->info.id.c_str(), v);
            }
            CHECK(pr.state["type"].str("") == k.id);
            // noise with a loud tone in front, a sine, and silence
            for (int sig = 0; sig < 3; ++sig) {
                auto p = k.make();
                p->prepare(kSr);
                p->loadState(pr.state);
                Stereo in = sig == 0 ? noise(0.5, 0.3f) : (sig == 1 ? sineWave(220, 0.5, 0.8f) : silence(0.5));
                if (sig == 0) for (size_t i = 0; i < 2000; ++i) in.l[i] = in.r[i] = 0.9f * (float)std::sin(i * 0.05);
                const bool hiss = p->param("hiss") && p->param("hiss")->get() > p->param("hiss")->info.min;
                const auto out = run(*p, in);
                CHECK_MSG(allFinite(out), "%s / %s is not finite", k.name, pr.name.c_str());
                CHECK_MSG(peakOf(out.l) < 4.0f && peakOf(out.r) < 4.0f, "%s / %s runs away (peak %.2f)", k.name, pr.name.c_str(), peakOf(out.l));
                if (sig == 2 && !hiss) CHECK_MSG(peakOf(out.l) == 0.0f && peakOf(out.r) == 0.0f, "%s / %s makes sound out of silence", k.name, pr.name.c_str());
                if (sig == 1 && !(std::string(k.id) == "midside" && pr.name == "Sides only")) CHECK_MSG(rmsOf(out.l, 12000) > 1.0e-4, "%s / %s is silent on a sine", k.name, pr.name.c_str());
                // save, recreate, load: same state
                auto q = k.make();
                q->prepare(kSr);
                q->loadState(p->saveState());
                CHECK(q->saveState() == p->saveState());
                // through text, as a project file does it
                auto t = k.make();
                t->prepare(kSr);
                t->loadState(Json::parse(p->saveState().dump()));
                CHECK(t->saveState() == p->saveState());
                // and the same state gives the same sound
                if (sig == 0) {
                    const auto again = run(*q, in), text = run(*t, in);
                    CHECK_MSG(again.l == out.l && again.r == out.r, "%s / %s sounds different after a reload", k.name, pr.name.c_str());
                    CHECK_MSG(text.l == out.l && text.r == out.r, "%s / %s sounds different after a reload through text", k.name, pr.name.c_str());
                }
            }
        }
    }
}

TEST(tone_silence_in_gives_silence_out) {
    for (auto& k : kinds()) {
        const auto presets = k.make()->factoryPresets();
        for (size_t pi = 0; pi <= presets.size(); ++pi) {      // every preset, and the defaults
            auto p = k.make();
            p->prepare(kSr);
            if (pi < presets.size()) p->loadState(presets[pi].state);
            hissOff(*p);
            const char* what = pi < presets.size() ? presets[pi].name.c_str() : "defaults";
            // loud noise, then nothing: the output must fall to exact zero, not crawl in denormals or sit on a DC offset
            Stereo in = noise(0.3, 0.8f);
            append(in, silence(2.5));
            const auto out = run(*p, in);
            CHECK(allFinite(out));
            const size_t tail = out.l.size() - 4800;
            CHECK_MSG(peakOf(out.l, tail) == 0.0f && peakOf(out.r, tail) == 0.0f, "%s / %s: %g left 2.4 s after the sound stopped", k.name, what, std::max(peakOf(out.l, tail), peakOf(out.r, tail)));
            // one second in, whatever is left is far below anything audible
            const size_t mid = (size_t)(1.3 * kSr);
            CHECK_MSG(peakOf(out.l, mid) < 1.0e-6f, "%s / %s: %g left 1 s after the sound stopped", k.name, what, peakOf(out.l, mid));
            // no number in the output is a denormal
            bool denormal = false;
            for (float v : out.l) if (v != 0.0f && std::fabs(v) < 1.0e-37f) denormal = true;
            CHECK_MSG(!denormal, "%s / %s puts out denormals", k.name, what);
        }
    }
}

TEST(tone_reset_and_prepare_start_clean) {
    // After reset() or a new prepare() a plugin gives exactly what a new one gives: no state, no ramp left over.
    for (auto& k : kinds()) {
        const auto presets = k.make()->factoryPresets();
        for (size_t pi : {(size_t)1, presets.size() - 1}) {
            const auto in = groove(0.4, 0.7f);
            auto fresh = k.make();
            fresh->loadState(presets[pi].state);
            fresh->prepare(kSr);
            const auto want = run(*fresh, in);
            auto used = k.make();
            used->prepare(kSr);
            run(*used, noise(0.3, 0.9f));                   // default settings, other sound
            used->loadState(presets[pi].state);
            used->reset();
            const auto afterReset = run(*used, in);
            CHECK_MSG(afterReset.l == want.l && afterReset.r == want.r, "%s / %s: reset() leaves something behind", k.name, presets[pi].name.c_str());
            used->prepare(96000.0);
            runAt(*used, noise(0.1, 0.9f, 96000.0), 96000.0);
            used->prepare(kSr);
            const auto afterPrepare = run(*used, in);
            CHECK_MSG(afterPrepare.l == want.l && afterPrepare.r == want.r, "%s / %s: prepare() leaves something behind", k.name, presets[pi].name.c_str());
        }
    }
}

TEST(tone_survive_automation_of_every_parameter) {
    // Every parameter jumps to a new random value every quantum while noise and a tone run through, with the song
    // playing. Nothing may become infinite or run away; when the input stops, so does the output.
    for (auto& k : kinds()) {
        for (double sr : {44100.0, 96000.0}) {
            auto p = k.make();
            p->prepare(sr);
            dsp::Noise rnd(1234u);
            Stereo in = noise(1.0, 0.7f, sr);
            addTo(in, sineWave(90, 1.0, 0.5f, sr));
            append(in, silence(2.5, sr));
            const size_t active = (size_t)(1.0 * sr) / kQuantum * kQuantum;
            Transport t;
            t.sr = sr; t.playing = true; t.tempo = 133.0;
            const auto out = run(*p, in, t, [&](size_t i) {
                if (i < active) for (auto& prm : p->params()) prm->setNorm(rnd.uni());
                else hissOff(*p);
            });
            CHECK_MSG(allFinite(out), "%s at %.0f Hz is not finite under automation", k.name, sr);
            const float peak = std::max(peakOf(out.l), peakOf(out.r));
            // The most gain anything here can be asked for is Motion Filter's resonance on two stages plus its output
            // gain, and Ten Band's filters while the sliders are thrown about: both well under 60 dB.
            CHECK_MSG(peak < 1.2f * 500.0f, "%s at %.0f Hz runs away under automation (peak %.1f)", k.name, sr, peak);
            const size_t tail = out.l.size() - (size_t)(0.1 * sr);
            CHECK_MSG(peakOf(out.l, tail) == 0.0f && peakOf(out.r, tail) == 0.0f, "%s at %.0f Hz: %g left 2.4 s after the input stopped", k.name, sr, std::max(peakOf(out.l, tail), peakOf(out.r, tail)));
            if (sr == 44100.0) FIG("%s: every parameter randomised every quantum, input peak 1.2: output peak %.2f", k.name, peak);
        }
    }
}

TEST(tone_parameter_moves_do_not_click) {
    // Every parameter of every effect is thrown from one end of its range to the other (a choice: from each value
    // to the next) in the middle of a tone. The bound: no step between two samples is larger than the steepest step
    // of the settled sound before or after the move (with a quarter to spare) plus an eighth of the larger peak.
    // A click, a plain discontinuity, would be of the order of the peak itself.
    const double seconds = 0.3;
    const size_t at = 7232;         // a quantum boundary, 150 ms in
    for (auto& k : kinds()) {
        double worstRatio = 0.0;
        std::string worstWhat;
        auto probe = k.make();
        for (size_t pi = 0; pi < probe->params().size(); ++pi) {
            const ParamInfo& info = probe->params()[pi]->info;
            std::vector<float> values;
            if (info.steps > 0) for (int v = 0; v < info.steps; ++v) values.push_back(info.min + (float)v);
            else values = {info.min, info.max};
            const Stereo in = sineWave(220, seconds, 0.25f);
            std::vector<float> steadyStep(values.size()), steadyPeak(values.size());
            const auto setUp = [&](Plugin& p, float v) {
                p.prepare(kSr);
                // something for every control to act on: the LFO and the envelope move the filter, the shelves are in
                if (std::string(k.id) == "modfilter") { set(p, "lfodepth", 1.0f); set(p, "envamt", 1.0f); set(p, "reso", 0.5f); }
                if (std::string(k.id) == "midside") { set(p, "mlow", 6.0f); set(p, "shigh", 6.0f); }
                if (std::string(k.id) == "tape") { set(p, "wow", 0.5f); set(p, "flutter", 0.5f); }
                if (std::string(k.id) == "crush") set(p, "rate", 12000.0f);
                p.params()[pi]->set(v);
            };
            for (size_t vi = 0; vi < values.size(); ++vi) {
                auto p = k.make();
                setUp(*p, values[vi]);
                const auto out = run(*p, in);
                steadyStep[vi] = maxStep(out.l, 4800);
                steadyPeak[vi] = peakOf(out.l, 4800);
            }
            for (size_t vi = 0; vi < values.size(); ++vi) {
                const size_t to = (vi + 1) % values.size();
                if (to == vi) continue;
                auto p = k.make();
                setUp(*p, values[vi]);
                const auto out = run(*p, in, {}, [&](size_t i) { if (i == at) p->params()[pi]->set(values[to]); });
                const float step = maxStep(out.l, at - 64);
                float allowed = 1.25f * std::max(steadyStep[vi], steadyStep[to]) + 0.125f * std::max(steadyPeak[vi], steadyPeak[to]);
                // Two stated exceptions. Coarse's bit depth and rate pass through every setting in between on the way,
                // and at some of those the tone is all steps: the bound there is the largest step a quantiser or a hold
                // can make of this tone at all, its peak to peak. Motion Filter's cutoff thrown across all ten octaves
                // at once opens the filter in under a millisecond: a quarter of the peak instead of an eighth.
                if (std::string(k.id) == "crush" && (info.id == "bits" || info.id == "rate")) allowed = 0.5f;
                if (std::string(k.id) == "modfilter" && info.id == "cutoff") allowed = 1.25f * std::max(steadyStep[vi], steadyStep[to]) + 0.25f * std::max(steadyPeak[vi], steadyPeak[to]);
                CHECK_MSG(step <= allowed, "%s, %s %g -> %g: step %.5f, allowed %.5f", k.name, info.id.c_str(), values[vi], values[to], step, allowed);
                const double ratio = step / std::max(1.0e-9f, allowed);
                if (ratio > worstRatio) { worstRatio = ratio; worstWhat = info.id; }
            }
        }
        FIG("%s: every parameter thrown across its range under a tone: the largest step is %.0f %% of the bound (at '%s')", k.name, 100.0 * worstRatio, worstWhat.c_str());
    }
}

TEST(tone_render_is_identical_at_any_block_size_and_thread_count) {
    auto build = [](Project& p) {
        p.global->tempo.set(126.0f);
        auto kick = addSampleChannel(p, drums::makeKick({}, kSr), "Kick");
        p.channels.back().mixerTrack = 1;
        for (int s : {0, 4, 8, 12, 14}) Project::setStep(p.pattern().notesFor(kick.id), s, true);
        auto snare = addSampleChannel(p, drums::makeSnare({}, kSr), "Snare");
        p.channels.back().mixerTrack = 1;
        for (int s : {4, 12}) Project::setStep(p.pattern().notesFor(snare.id), s, true);
        auto pad = addSampleChannel(p, sine(220, 1.5, kSr, 0.3f), "Pad");
        p.channels.back().mixerTrack = 2;
        for (int s : {0, 8}) Project::setStep(p.pattern().notesFor(pad.id), s, true);
        auto hat = addSampleChannel(p, drums::makeHat({}, kSr), "Hat");
        p.channels.back().mixerTrack = 3;
        for (int s = 0; s < 16; s += 2) Project::setStep(p.pattern().notesFor(hat.id), s, true);
        auto bass = addSampleChannel(p, sine(55, 0.4, kSr, 0.6f), "Bass");
        p.channels.back().mixerTrack = 4;
        for (int s : {2, 6, 10, 15}) Project::setStep(p.pattern().notesFor(bass.id), s, true);
        auto lead = addSampleChannel(p, sine(880, 0.3, 44100.0, 0.25f), "Lead");
        p.channels.back().mixerTrack = 5;
        for (int s : {1, 5, 9, 13}) Project::setStep(p.pattern().notesFor(lead.id), s, true);

        auto load = [](std::shared_ptr<Plugin> plug, int preset) { plug->loadState(plug->factoryPresets()[(size_t)preset].state); return plug; };
        p.setSlot(1, 0, load(std::make_shared<QuarterInch>(), 3));      // worn cassette: wow, flutter and hiss
        p.setSlot(1, 1, load(std::make_shared<TenBand>(), 1));
        p.setSlot(2, 0, load(std::make_shared<MotionFilter>(), 2));     // quarter-note wah, locked to the song
        p.setSlot(2, 1, load(std::make_shared<Glass>(), 3));
        p.setSlot(3, 0, load(std::make_shared<Coarse>(), 5));           // dithered
        p.setSlot(3, 1, load(std::make_shared<MotionFilter>(), 5));     // random steps
        p.setSlot(4, 0, load(std::make_shared<Stack>(), 7));
        p.setSlot(4, 1, load(std::make_shared<MiddleSides>(), 2));
        p.setSlot(5, 0, load(std::make_shared<Splat>(), 2));
        p.setSlot(5, 1, load(std::make_shared<Bend>(), 3));             // fold back, eight times oversampled
        p.setSlot(5, 2, load(std::make_shared<MotionFilter>(), 6));     // free LFO, right channel ahead
        p.setSlot(0, 0, load(std::make_shared<MiddleSides>(), 1));
        p.setSlot(0, 1, load(std::make_shared<Stack>(), 1));
        auto tape = load(std::make_shared<QuarterInch>(), 4);
        p.setSlot(0, 2, tape);
    };
    std::shared_ptr<SampleData> ref;
    struct Case { int threads, block; } cases[] = {{1, 512}, {1, 64}, {1, 33}, {1, 4096}, {4, 512}, {4, 64}, {3, 1000}};
    for (auto& cs : cases) {
        Project p;
        build(p);
        RenderOptions o;
        o.lengthSamples = 72000;
        o.threads = cs.threads;
        o.blockSize = cs.block;
        const int64 before = Rt::allocationsInRt.load();
        auto out = renderOffline(p, o);
        CHECK_MSG(Rt::allocationsInRt.load() == before, "threads %d block %d: %lld allocations on the audio thread", cs.threads, cs.block, (long long)(Rt::allocationsInRt.load() - before));
        if (!ref) {
            ref = out;
            CHECK(peakOf(out->ch[0]) > 0.05f && peakOf(out->ch[0]) < 4.0f);
            for (float v : out->ch[0]) if (!std::isfinite(v)) { CHECK(false); break; }
            CHECK(maxAbsDiff(out->ch[0], out->ch[1]) > 1.0e-4);       // it is a stereo mix
            continue;
        }
        CHECK_MSG(identical(*ref, *out), "threads %d block %d differs", cs.threads, cs.block);
    }
    // the project reports the latency the effects add
    Project p;
    build(p);
    CHECK(p.compile()->latency >= 32 + 128);
}

TEST(tone_processing_does_not_allocate) {
    // Straight calls, with the real-time marker set as the engine sets it, while every parameter is being moved.
    for (auto& k : kinds()) {
        auto p = k.make();
        p->prepare(kSr);
        dsp::Noise rnd(99u);
        Stereo io = noise(0.3, 0.6f);
        const int64 before = Rt::allocationsInRt.load();
        for (size_t i = 0; i + kQuantum <= io.l.size(); i += kQuantum) {
            for (auto& prm : p->params()) prm->setNorm(rnd.uni());
            ProcessContext c;
            c.playing = (i / kQuantum) % 7 != 0;
            c.tickPos = (double)i * 0.04;
            c.io[0] = io.l.data() + i;
            c.io[1] = io.r.data() + i;
            Rt::Scope rt;
            p->process(c);
            if (i == 6400) p->reset();
        }
        CHECK_MSG(Rt::allocationsInRt.load() == before, "%s allocates in process() or reset()", k.name);
    }
    // Bend's curve can be replaced while it runs: the audio side picks the new one up without allocating
    Bend b;
    b.prepare(kSr);
    Stereo io = noise(0.3, 0.6f);
    int64 inRt = 0;
    for (size_t i = 0; i + kQuantum <= io.l.size(); i += kQuantum) {
        if ((i / kQuantum) % 5 == 0) b.setCurve({{-1.0f, -0.5f}, {0.0f, (float)((i / kQuantum) % 3) * 0.1f}, {1.0f, 0.8f}});
        ProcessContext c;
        c.io[0] = io.l.data() + i;
        c.io[1] = io.r.data() + i;
        const int64 before = Rt::allocationsInRt.load();
        { Rt::Scope rt; b.process(c); }
        inRt += Rt::allocationsInRt.load() - before;
    }
    CHECK(inRt == 0);
}

TEST(tone_cpu_cost) {
    // How long ten seconds of stereo at 48 kHz take, each effect at a demanding preset. A figure for the notes;
    // the only thing asserted is that none of them is slower than real time in an optimised build.
    struct Load { const char* id; int preset; } loads[] = {{"geq", 1}, {"modfilter", 7}, {"tape", 3}, {"tube", 3}, {"amp", 4}, {"fuzz", 2}, {"crush", 1}, {"shaper", 3}, {"midside", 7}};
    const Stereo in = groove(2.0, 0.7f);
    for (auto& ld : loads) {
        auto p = make(ld.id);
        p->prepare(kSr);
        const auto presets = p->factoryPresets();
        p->loadState(presets[(size_t)ld.preset].state);
        const auto t0 = std::chrono::steady_clock::now();
        double audio = 0.0;
#ifdef _DEBUG
        const int passes = 1;
#else
        const int passes = 5;
#endif
        for (int pass = 0; pass < passes; ++pass) { const auto out = run(*p, in); audio += (double)out.l.size() / kSr; }
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
#ifndef _DEBUG
        CHECK_MSG(took < audio, "%s takes %.2f s for %.0f s of audio", ld.id, took, audio);
#else
        CHECK(took > 0.0);
#endif
        FIG("cpu: %s (%s) uses %.2f %% of one core at 48 kHz stereo in this build", p->displayName(), presets[(size_t)ld.preset].name.c_str(), 100.0 * took / audio);
    }
    // Ten Band works its filter gains out again in every quantum in which a slider is moving: the cost of that
    {
        TenBand eq;
        eq.prepare(kSr);
        dsp::Noise rnd(3u);
        const auto t0 = std::chrono::steady_clock::now();
        const auto out = run(eq, in, {}, [&](size_t) { for (auto* g : eq.pGain) g->set(15.0f * rnd.next()); });
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(allFinite(out));
        FIG("cpu: Ten Band with all ten sliders moving in every quantum uses %.2f %% of one core", 100.0 * took / ((double)out.l.size() / kSr));
    }
}

TEST(tone_unusual_sample_rates_are_safe) {
    // Half and four times the usual rate: every effect at its defaults and at two presets, noise and a tone through,
    // then every parameter thrown about. Nothing here is measured against a curve; it must simply stay sane.
    for (double sr : {22050.0, 192000.0}) {
        for (auto& k : kinds()) {
            const auto presets = k.make()->factoryPresets();
            for (size_t pi = 0; pi < 3; ++pi) {
                auto p = k.make();
                p->prepare(sr);
                if (pi > 0) p->loadState(presets[pi == 1 ? 1 : presets.size() - 1].state);
                CHECK(p->latencySamples() == latencyAt(k.id, sr));
                Stereo in = noise(0.25, 0.5f, sr);
                addTo(in, sineWave(300, 0.25, 0.4f, sr));
                dsp::Noise rnd(7u);
                const size_t half = in.l.size() / 2 / kQuantum * kQuantum;
                Transport t;
                t.sr = sr; t.playing = true;
                const auto out = run(*p, in, t, [&](size_t i) { if (i >= half) for (auto& prm : p->params()) prm->setNorm(rnd.uni()); });
                CHECK_MSG(allFinite(out), "%s at %.0f Hz is not finite", k.name, sr);
                CHECK_MSG(std::max(peakOf(out.l), peakOf(out.r)) < 500.0f, "%s at %.0f Hz: peak %.1f", k.name, sr, std::max(peakOf(out.l), peakOf(out.r)));
                CHECK_MSG(rmsOf(out.l, 0, half) > 1.0e-4, "%s at %.0f Hz is silent", k.name, sr);
            }
        }
    }
}
