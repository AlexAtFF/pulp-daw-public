#include "pulp/NoteTools.h"
#include "pulp/Dsp.h"

namespace pulp::tools {

const std::vector<NamedIntervals>& scales() {
    static const std::vector<NamedIntervals> s = {
        {"Major", {0, 2, 4, 5, 7, 9, 11}},
        {"Minor (natural)", {0, 2, 3, 5, 7, 8, 10}},
        {"Minor (harmonic)", {0, 2, 3, 5, 7, 8, 11}},
        {"Minor (melodic)", {0, 2, 3, 5, 7, 9, 11}},
        {"Dorian", {0, 2, 3, 5, 7, 9, 10}},
        {"Phrygian", {0, 1, 3, 5, 7, 8, 10}},
        {"Lydian", {0, 2, 4, 6, 7, 9, 11}},
        {"Mixolydian", {0, 2, 4, 5, 7, 9, 10}},
        {"Locrian", {0, 1, 3, 5, 6, 8, 10}},
        {"Major pentatonic", {0, 2, 4, 7, 9}},
        {"Minor pentatonic", {0, 3, 5, 7, 10}},
        {"Blues", {0, 3, 5, 6, 7, 10}},
        {"Whole tone", {0, 2, 4, 6, 8, 10}},
        {"Chromatic", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
    };
    return s;
}

const std::vector<NamedIntervals>& chords() {
    static const std::vector<NamedIntervals> c = {
        {"Major", {0, 4, 7}}, {"Minor", {0, 3, 7}}, {"Diminished", {0, 3, 6}}, {"Augmented", {0, 4, 8}},
        {"Sus 2", {0, 2, 7}}, {"Sus 4", {0, 5, 7}}, {"Power (fifth)", {0, 7}}, {"Octave", {0, 12}},
        {"Major 6", {0, 4, 7, 9}}, {"Minor 6", {0, 3, 7, 9}}, {"Major 7", {0, 4, 7, 11}}, {"Minor 7", {0, 3, 7, 10}},
        {"Dominant 7", {0, 4, 7, 10}}, {"Diminished 7", {0, 3, 6, 9}}, {"Half-diminished", {0, 3, 6, 10}},
        {"Add 9", {0, 4, 7, 14}}, {"Major 9", {0, 4, 7, 11, 14}}, {"Minor 9", {0, 3, 7, 10, 14}}, {"Dominant 9", {0, 4, 7, 10, 14}},
        {"Major (1st inversion)", {-8, -5, 0}}, {"Minor (1st inversion)", {-9, -5, 0}},
    };
    return c;
}

bool inScale(int key, int root, const NamedIntervals& scale) {
    const int d = (((key - root) % 12) + 12) % 12;
    return std::find(scale.intervals.begin(), scale.intervals.end(), d) != scale.intervals.end();
}

int snapToScale(int key, int root, const NamedIntervals& scale) {
    for (int off = 0; off < 12; ++off) {
        if (inScale(key - off, root, scale)) return key - off;
        if (inScale(key + off, root, scale)) return key + off;
    }
    return key;
}

std::string keyName(int key) {
    static const char* n[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(n[((key % 12) + 12) % 12]) + std::to_string(key / 12);
}

const char* propName(int prop) {
    static const char* n[] = {"Velocity", "Pan", "Release", "Mod X", "Mod Y", "Fine pitch"};
    return n[clampT(prop, 0, (int)NumProps - 1)];
}
float getProp(const Note& n, int prop) {
    switch (prop) {
        case Velocity: return n.velocity;
        case Pan: return (n.pan + 1.0f) * 0.5f;
        case Release: return n.release;
        case ModX: return n.modX;
        case ModY: return n.modY;
        default: return (n.fine + 1200.0f) / 2400.0f;
    }
}
void setProp(Note& n, int prop, float v) {
    v = clampT(v, 0.0f, 1.0f);
    switch (prop) {
        case Velocity: n.velocity = v; break;
        case Pan: n.pan = v * 2.0f - 1.0f; break;
        case Release: n.release = v; break;
        case ModX: n.modX = v; break;
        case ModY: n.modY = v; break;
        default: n.fine = v * 2400.0f - 1200.0f; break;
    }
}
float defaultProp(int prop) { return prop == Velocity ? kDefaultVelocity : 0.5f; }

bool anySelected(const std::vector<Note>& notes) {
    for (auto& n : notes) if (n.selected) return true;
    return false;
}

namespace {
// Runs fn on every note the tool applies to.
template <class F> void each(std::vector<Note>& notes, F fn) {
    const bool sel = anySelected(notes);
    for (auto& n : notes) if (!sel || n.selected) fn(n);
}
inline bool applies(const Note& n, bool sel) { return !sel || n.selected; }
inline int roundTo(int v, int grid) { return grid <= 1 ? v : (int)std::lround((double)v / grid) * grid; }
}

void quantize(std::vector<Note>& notes, const QuantizeOpts& o) {
    each(notes, [&](Note& n) {
        const int end = n.end();
        if (o.starts) {
            const int target = roundTo(n.tick, o.grid);
            n.tick = std::max(0, n.tick + (int)std::lround((target - n.tick) * o.strength));
            if (o.keepEnds) n.length = std::max(1, end - n.tick);
        }
        if (o.ends) {
            const int e = n.end();
            int target = roundTo(e, o.grid);
            if (target <= n.tick) target = n.tick + o.grid;
            n.length = std::max(1, e + (int)std::lround((target - e) * o.strength) - n.tick);
        }
    });
    sortNotes(notes);
}

void legato(std::vector<Note>& notes) {
    const bool sel = anySelected(notes);
    sortNotes(notes);
    for (size_t i = 0; i < notes.size(); ++i) {
        if (!applies(notes[i], sel)) continue;
        for (size_t j = i + 1; j < notes.size(); ++j)
            if (applies(notes[j], sel) && notes[j].tick > notes[i].tick) { notes[i].length = notes[j].tick - notes[i].tick; break; }
    }
}

void chop(std::vector<Note>& notes, int grid) {
    if (grid < 1) return;
    const bool sel = anySelected(notes);
    std::vector<Note> out;
    for (auto& n : notes) {
        if (!applies(n, sel) || n.length <= grid) { out.push_back(n); continue; }
        for (int t = n.tick; t < n.end(); t += grid) {
            Note p = n;
            p.tick = t;
            p.length = std::min(grid, n.end() - t);
            out.push_back(p);
        }
    }
    notes = std::move(out);
    sortNotes(notes);
}

void glue(std::vector<Note>& notes) {
    const bool sel = anySelected(notes);
    sortNotes(notes);
    std::vector<Note> out;
    for (auto& n : notes) {
        bool merged = false;
        if (applies(n, sel))
            for (auto& o : out)
                if (applies(o, sel) && o.key == n.key && o.slide == n.slide && n.tick <= o.end() && n.tick >= o.tick) {
                    o.length = std::max(o.end(), n.end()) - o.tick;
                    merged = true;
                    break;
                }
        if (!merged) out.push_back(n);
    }
    notes = std::move(out);
}

void arpeggiate(std::vector<Note>& notes, int grid, int pattern, int octaves, float gate, uint32 seed) {
    if (grid < 1) return;
    const bool sel = anySelected(notes);
    sortNotes(notes);
    std::vector<Note> out, chordNotes;
    dsp::Noise rng(seed);
    auto flush = [&]() {
        if (chordNotes.empty()) return;
        std::sort(chordNotes.begin(), chordNotes.end(), [](const Note& a, const Note& b) { return a.key < b.key; });
        std::vector<Note> pool;
        for (int o = 0; o < std::max(1, octaves); ++o)
            for (auto c : chordNotes) { c.key = clampT(c.key + 12 * o, 0, 127); pool.push_back(c); }
        const int start = chordNotes[0].tick;
        int end = start;
        for (auto& c : chordNotes) end = std::max(end, c.end());
        const int n = (int)pool.size();
        int step = 0;
        for (int t = start; t < end; t += grid, ++step) {
            int idx = 0;
            switch (pattern) {
                case ArpUp: idx = step % n; break;
                case ArpDown: idx = n - 1 - step % n; break;
                case ArpUpDown: { const int period = std::max(1, 2 * n - 2); const int q = step % period; idx = q < n ? q : period - q; break; }
                default: idx = (int)(rng.uni() * (float)n) % n; break;
            }
            Note a = pool[(size_t)idx];
            a.tick = t;
            a.length = std::max(1, std::min((int)(grid * gate), end - t));
            out.push_back(a);
        }
        chordNotes.clear();
    };
    for (auto& n : notes) {
        if (!applies(n, sel)) { out.push_back(n); continue; }
        if (!chordNotes.empty() && n.tick != chordNotes[0].tick) flush();
        chordNotes.push_back(n);
    }
    flush();
    notes = std::move(out);
    sortNotes(notes);
}

void strum(std::vector<Note>& notes, int ticksPerNote, bool downward, float velocityTaper) {
    const bool sel = anySelected(notes);
    sortNotes(notes);
    size_t i = 0;
    while (i < notes.size()) {
        if (!applies(notes[i], sel)) { ++i; continue; }
        // gather the notes that start together
        std::vector<size_t> group;
        const int t0 = notes[i].tick;
        for (size_t j = i; j < notes.size() && notes[j].tick == t0; ++j) if (applies(notes[j], sel)) group.push_back(j);
        size_t next = i;
        while (next < notes.size() && notes[next].tick == t0) ++next;
        std::sort(group.begin(), group.end(), [&](size_t a, size_t b) { return downward ? notes[a].key > notes[b].key : notes[a].key < notes[b].key; });
        for (size_t k = 0; k < group.size(); ++k) {
            Note& n = notes[group[k]];
            const int shift = (int)k * ticksPerNote;
            n.tick += shift;
            n.length = std::max(1, n.length - shift);
            n.velocity = clampT(n.velocity * (1.0f - velocityTaper * (float)k / (float)std::max<size_t>(1, group.size())), 0.02f, 1.0f);
        }
        i = next;
    }
    sortNotes(notes);
}

void flam(std::vector<Note>& notes, int ticksBefore, float velocityScale) {
    const bool sel = anySelected(notes);
    std::vector<Note> grace;
    for (auto& n : notes) {
        if (!applies(n, sel) || n.slide) continue;
        Note g = n;
        g.tick = std::max(0, n.tick - ticksBefore);
        g.length = std::max(1, std::min(ticksBefore, n.tick - g.tick));
        g.velocity = clampT(n.velocity * velocityScale, 0.02f, 1.0f);
        g.selected = false;
        if (g.tick < n.tick) grace.push_back(g);
    }
    notes.insert(notes.end(), grace.begin(), grace.end());
    sortNotes(notes);
}

void articulate(std::vector<Note>& notes, float lengthMultiply, int gapTicks) {
    const bool sel = anySelected(notes);
    sortNotes(notes);
    for (size_t i = 0; i < notes.size(); ++i) {
        Note& n = notes[i];
        if (!applies(n, sel)) continue;
        if (lengthMultiply > 0.0f) n.length = std::max(1, (int)std::lround(n.length * lengthMultiply));
        else
            for (size_t j = i + 1; j < notes.size(); ++j)
                if (applies(notes[j], sel) && notes[j].tick > n.tick) { n.length = std::max(1, notes[j].tick - n.tick - gapTicks); break; }
    }
}

void randomize(std::vector<Note>& notes, const RandomizeOpts& o) {
    dsp::Noise rng(o.seed * 2654435761u + 17u);
    each(notes, [&](Note& n) {
        if (o.velocity > 0.0f) n.velocity = clampT(n.velocity + rng.next() * o.velocity * 0.5f, 0.02f, 1.0f);
        if (o.pan > 0.0f) n.pan = clampT(n.pan + rng.next() * o.pan, -1.0f, 1.0f);
        if (o.length > 0.0f) n.length = std::max(1, (int)std::lround(n.length * (1.0f + rng.next() * o.length * 0.9f)));
        if (o.pitchRange > 0) {
            int k = clampT(n.key + (int)std::lround(rng.next() * (float)o.pitchRange), 0, 127);
            if (o.scaleRoot >= 0) k = snapToScale(k, o.scaleRoot, scales()[(size_t)clampT(o.scaleIndex, 0, (int)scales().size() - 1)]);
            n.key = clampT(k, 0, 127);
        }
    });
}

void scaleLevels(std::vector<Note>& notes, int prop, float multiply, float offset, float centre) {
    each(notes, [&](Note& n) { setProp(n, prop, centre + (getProp(n, prop) - centre) * multiply + offset); });
}

void flipVertical(std::vector<Note>& notes) {
    const bool sel = anySelected(notes);
    int lo = 127, hi = 0;
    for (auto& n : notes) if (applies(n, sel)) { lo = std::min(lo, n.key); hi = std::max(hi, n.key); }
    each(notes, [&](Note& n) { n.key = hi - (n.key - lo); });
}

void reverseTime(std::vector<Note>& notes) {
    const bool sel = anySelected(notes);
    int lo = INT32_MAX, hi = 0;
    for (auto& n : notes) if (applies(n, sel)) { lo = std::min(lo, n.tick); hi = std::max(hi, n.end()); }
    each(notes, [&](Note& n) { n.tick = lo + (hi - n.end()); });
    sortNotes(notes);
}

void transpose(std::vector<Note>& notes, int semitones) {
    const bool sel = anySelected(notes);
    for (auto& n : notes) if (applies(n, sel) && (n.key + semitones < 0 || n.key + semitones > 127)) return;   // keep the shape intact
    each(notes, [&](Note& n) { n.key += semitones; });
}

void shiftTime(std::vector<Note>& notes, int ticks) {
    const bool sel = anySelected(notes);
    for (auto& n : notes) if (applies(n, sel) && n.tick + ticks < 0) return;
    each(notes, [&](Note& n) { n.tick += ticks; });
    sortNotes(notes);
}

void trimPattern(std::vector<Note>& notes, int keepTicks, int dropTicks) {
    if (keepTicks < 1 || dropTicks < 1) return;
    const int cycle = keepTicks + dropTicks;
    std::vector<Note> out;
    for (auto n : notes) {
        const int c = n.tick / cycle, in = n.tick % cycle;
        if (in >= keepTicks) continue;                      // falls in a removed stretch
        n.tick = c * keepTicks + in;
        n.length = std::max(1, std::min(n.length, keepTicks - in));
        out.push_back(n);
    }
    notes = std::move(out);
    sortNotes(notes);
}

// The LFO's value (0..1) at a place in its cycle (0..1).
static double lfoWave(int shape, double p) {
    switch (shape) {
        case LfoSine: return 0.5 - 0.5 * std::cos(kTwoPi * p);
        case LfoTriangle: return p < 0.5 ? p * 2.0 : 2.0 - p * 2.0;
        case LfoSquare: return p < 0.5 ? 1.0 : 0.0;
        case LfoSawUp: return p;
        default: return 1.0 - p;
    }
}

void lfo(std::vector<Note>& notes, int prop, int shape, int periodTicks, float lo, float hi, float phase) {
    if (periodTicks < 1) return;
    each(notes, [&](Note& n) {
        double p = (double)n.tick / periodTicks + phase;
        p -= std::floor(p);
        setProp(n, prop, lo + (hi - lo) * (float)lfoWave(shape, p));
    });
}

std::vector<AutoEvent> lfoEvents(int lengthTicks, int shape, int periodTicks, float lo, float hi, float phase) {
    std::vector<AutoEvent> out;
    if (periodTicks < 1 || lengthTicks < 1) return out;
    phase -= std::floor(phase);
    auto value = [&](int tick) {
        double p = (double)tick / periodTicks + phase;
        p -= std::floor(p);
        return clampT(lo + (hi - lo) * (float)lfoWave(shape, p), 0.0f, 1.0f);
    };
    // where in a cycle the wave turns, and whether it glides on from there
    const bool saw = shape == LfoSawUp || shape == LfoSawDown;
    std::vector<double> turns;
    if (shape == LfoSine) for (int k = 0; k < 16; ++k) turns.push_back(k / 16.0);
    else turns = {0.0, 0.5};
    if (saw) turns = {0.0};
    const bool glide = shape != LfoSquare;
    std::vector<int> ticks = {0};
    const int firstCycle = (int)std::floor(-phase) - 1, lastCycle = lengthTicks / periodTicks + 2;
    for (int c = firstCycle; c <= lastCycle; ++c)
        for (double t : turns) {
            const int tick = (int)std::llround(((double)c + t - phase) * periodTicks);
            if (tick > 0 && tick < lengthTicks) ticks.push_back(tick);
            // a saw falls back in one tick: the tick before the turn holds the end of the ramp
            if (saw && tick - 1 > 0 && tick - 1 < lengthTicks) ticks.push_back(tick - 1);
        }
    std::sort(ticks.begin(), ticks.end());
    ticks.erase(std::unique(ticks.begin(), ticks.end()), ticks.end());
    for (int tick : ticks) {
        AutoEvent e;
        e.tick = tick;
        e.value = value(tick);
        e.ramp = glide;
        out.push_back(e);
    }
    // the tick before a saw's turn does not glide into the jump
    if (saw)
        for (size_t i = 0; i + 1 < out.size(); ++i)
            if (out[i + 1].tick == out[i].tick + 1 && std::fabs(out[i + 1].value - out[i].value) > 0.5f * std::fabs(hi - lo)) out[i].ramp = false;
    if (!out.empty()) out.back().ramp = false;
    return out;
}

void duplicateAfter(std::vector<Note>& notes) {
    const bool sel = anySelected(notes);
    int lo = INT32_MAX, hi = 0;
    for (auto& n : notes) if (applies(n, sel)) { lo = std::min(lo, n.tick); hi = std::max(hi, n.end()); }
    if (lo == INT32_MAX) return;
    // span rounded up to a whole step so repeats stay on the grid
    const int span = ((hi - lo + kTicksPerStep - 1) / kTicksPerStep) * kTicksPerStep;
    std::vector<Note> copies;
    for (auto& n : notes) {
        if (!applies(n, sel)) continue;
        Note c = n;
        c.tick += span;
        c.selected = true;
        copies.push_back(c);
        n.selected = false;
    }
    notes.insert(notes.end(), copies.begin(), copies.end());
    sortNotes(notes);
}

// ------------------------------------------------------------------ riff generator

std::vector<Note> riff(const RiffOpts& o) {
    std::vector<Note> out;
    const auto& sc = scales()[(size_t)clampT(o.scaleIndex, 0, (int)scales().size() - 1)];
    const int degrees = (int)sc.intervals.size();
    dsp::Noise rng(o.seed * 747796405u + 2891336453u);
    auto pick = [&](int n) { return (int)(rng.uni() * (float)n) % std::max(1, n); };
    // scale degree (can exceed one octave) to a key
    auto keyOf = [&](int degree) {
        const int oct = (int)std::floor((double)degree / degrees);
        const int d = ((degree % degrees) + degrees) % degrees;
        return clampT(o.root + sc.intervals[(size_t)d] + 12 * oct, 0, 127);
    };
    // Common progressions as scale degrees (0 = tonic). Only used with seven-note scales.
    static const int progs[8][4] = {{0, 4, 5, 3}, {0, 5, 3, 4}, {5, 3, 0, 4}, {0, 3, 4, 3}, {1, 4, 0, 0}, {0, 5, 2, 6}, {0, 3, 5, 4}, {0, 2, 3, 4}};
    const int* prog = progs[pick(8)];
    auto chordDegree = [&](int bar) { return degrees >= 7 ? prog[bar % 4] : (bar % 2) * 2; };
    auto chordKeys = [&](int bar) {
        std::vector<int> keys;
        const int d = chordDegree(bar);
        for (int i = 0; i < (o.sevenths ? 4 : 3); ++i) keys.push_back(keyOf(d + i * 2));
        return keys;
    };

    for (int bar = 0; bar < o.bars; ++bar) {
        const int t0 = bar * o.ticksPerBar;
        const auto keys = chordKeys(bar);
        if (o.kind == RiffOpts::Progression) {
            const int hits = o.density > 0.66f ? 2 : 1;      // one or two chords per bar
            for (int h = 0; h < hits; ++h)
                for (int k : keys) {
                    Note n;
                    n.key = k;
                    n.tick = t0 + h * o.ticksPerBar / hits;
                    n.length = o.ticksPerBar / hits - (o.density < 0.33f ? 0 : kTicksPerStep / 2);
                    n.velocity = 0.7f + 0.05f * rng.next();
                    out.push_back(n);
                }
        } else if (o.kind == RiffOpts::Arpeggio) {
            const int grid = o.density > 0.5f ? kTicksPerStep : 2 * kTicksPerStep;
            std::vector<int> pool = keys;
            for (int k : keys) pool.push_back(clampT(k + 12, 0, 127));
            const int shape = pick(3);
            int step = 0;
            for (int t = 0; t < o.ticksPerBar; t += grid, ++step) {
                const int n2 = (int)pool.size();
                int idx = shape == 0 ? step % n2 : (shape == 1 ? n2 - 1 - step % n2 : ((step % (2 * n2 - 2)) < n2 ? step % (2 * n2 - 2) : (2 * n2 - 2) - step % (2 * n2 - 2)));
                Note n;
                n.key = pool[(size_t)idx];
                n.tick = t0 + t;
                n.length = grid * 3 / 4;
                n.velocity = step % 4 == 0 ? 0.85f : 0.65f + 0.05f * rng.next();
                out.push_back(n);
            }
        } else {
            // a rhythmic one-note groove on the chord root with octave jumps and accents
            const int rootKey = clampT(keys[0] - 12, 0, 127);
            for (int s = 0; s < o.ticksPerBar / kTicksPerStep; ++s) {
                const bool strong = s % 4 == 0;
                const float chance = strong ? 0.95f : (s % 2 == 0 ? 0.25f + 0.6f * o.density : 0.1f + 0.55f * o.density);
                if (rng.uni() > chance) continue;
                Note n;
                n.key = (!strong && rng.uni() < 0.25f) ? clampT(rootKey + 12, 0, 127) : rootKey;
                n.tick = t0 + s * kTicksPerStep;
                n.length = strong ? kTicksPerStep * 2 : kTicksPerStep;
                n.velocity = strong ? 0.9f : 0.55f + 0.2f * rng.uni();
                out.push_back(n);
            }
        }
    }
    sortNotes(out);
    return out;
}

} // namespace pulp::tools
