#include "pulp/Engine.h"
#include "pulp/Dsp.h"

namespace pulp {

static ParamInfo pinfo(const char* id, const char* name, float mn, float mx, float def, const char* unit = "", int steps = 0) {
    ParamInfo i; i.id = id; i.name = name; i.min = mn; i.max = mx; i.def = def; i.unit = unit; i.steps = steps;
    return i;
}
static ParamInfo pchoice(const char* id, const char* name, std::vector<std::string> choices, int def) {
    ParamInfo i; i.id = id; i.name = name; i.min = 0; i.max = (float)(choices.size() - 1); i.def = (float)def;
    i.steps = (int)choices.size(); i.choices = std::move(choices);
    return i;
}

GlobalLive::GlobalLive()
    : tempo(pinfo("tempo", "Tempo", 10, 522, 140, "BPM")),
      masterVolume(pinfo("mastervol", "Master volume", 0, 1, 0.8f, "%")),
      masterPitch(pinfo("masterpitch", "Master pitch", -1200, 1200, 0, "ct")) {}

// ------------------------------------------------------------------ ChannelLive

ChannelLive::ChannelLive(std::shared_ptr<Plugin> instrument)
    : plugin(std::move(instrument)),
      volume(pinfo("volume", "Channel volume", 0, 1, 0.78f, "%")),
      pan(pinfo("pan", "Channel panning", -1, 1, 0, "")),
      mute(pchoice("mute", "Mute", {"On", "Muted"}, 0)),
      pitch(pinfo("pitch", "Channel pitch", -1, 1, 0, "")),
      pitchRange(pinfo("pitchrange", "Pitch range", 1, 48, 2, "st", 48)),
      poly(pinfo("poly", "Max polyphony (0 = unlimited)", 0, 64, 0, "", 65)),
      mono(pchoice("mono", "Mono", {"Off", "On"}, 0)),
      porta(pchoice("porta", "Portamento", {"Off", "On"}, 0)),
      portaTime(pinfo("portatime", "Slide time", 1, 2000, 120, "ms")),
      cut(pinfo("cut", "Cut group", 0, 99, 0, "", 100)),
      cutBy(pinfo("cutby", "Cut by group", 0, 99, 0, "", 100)),
      gate(pinfo("gate", "Gate (0 = off)", 0, 16, 0, "steps")),
      shift(pinfo("shift", "Time shift", 0, 1, 0, "steps")),
      swingMix(pinfo("swingmix", "Swing mix", 0, 1, 1, "%")),
      arpDir(pchoice("arpdir", "Arpeggiator direction", {"Off", "Up", "Down", "Up-down (bounce)", "Up-down (sticky)", "Random"}, 0)),
      arpRange(pinfo("arprange", "Arpeggiator range", 1, 8, 1, "oct", 8)),
      arpTime(pinfo("arptime", "Arpeggiator time", 0.25f, 4, 1, "steps")),
      arpGate(pinfo("arpgate", "Arpeggiator gate", 0.05f, 1, 0.9f, "%")),
      arpChord(pchoice("arpchord", "Arpeggiator chord", {"Auto (held notes)", "Octave", "Fifth", "Major", "Minor", "Sus 4", "Major 7", "Minor 7", "Dominant 7"}, 0)),
      arpRepeat(pinfo("arprepeat", "Arpeggiator repeat", 1, 8, 1, "", 8)),
      echoCount(pinfo("echocount", "Number of echoes", 0, 10, 0, "", 11)),
      echoTime(pinfo("echotime", "Echo time", 0.25f, 8, 3, "steps")),
      echoFeed(pinfo("echofeed", "Echo feedback", 0, 1, 0.5f, "%")),
      echoPan(pinfo("echopan", "Echo pan step", -1, 1, 0, "")),
      echoPitch(pinfo("echopitch", "Echo pitch step", -12, 12, 0, "st", 25)),
      echoModX(pinfo("echomodx", "Echo mod X step", -1, 1, 0, "")),
      echoModY(pinfo("echomody", "Echo mod Y step", -1, 1, 0, "")),
      velTrack(pinfo("veltrack", "Velocity to volume", 0, 1, 1, "%")),
      keyPan(pinfo("keypan", "Key to pan (per octave)", -1, 1, 0, "")),
      keyModX(pinfo("keymodx", "Key to mod X (per octave)", -1, 1, 0, "")) {
    std::memset(buf, 0, sizeof buf);
}

std::vector<Param*> ChannelLive::stripParams() {
    return {&volume, &pan, &mute, &pitch, &pitchRange, &poly, &mono, &porta, &portaTime, &cut, &cutBy, &gate, &shift, &swingMix,
            &arpDir, &arpRange, &arpTime, &arpGate, &arpChord, &arpRepeat,
            &echoCount, &echoTime, &echoFeed, &echoPan, &echoPitch, &echoModX, &echoModY, &velTrack, &keyPan, &keyModX};
}
Param* ChannelLive::stripParam(const std::string& id) {
    for (Param* p : stripParams()) if (p->info.id == id) return p;
    return nullptr;
}

float ChannelLive::volumeToGain(float v) { const float n = v / 0.78f; return n * n; }

void ChannelLive::reset() {
    numSched_ = numIn_ = numHeld_ = 0;
    arpNext_ = -1;
    arpStep_ = 0;
    arpRand_ = 0x2545F491u;
    nextId_ = 1;
    std::memset(recent_, 0, sizeof recent_);
    recentW_ = 0;
    lastKey_ = -1.0f;
    std::memset(liveOut_, 0, sizeof liveOut_);
    std::memset(liveEchoes_, 0, sizeof liveEchoes_);
    primed_ = false;
    std::memset(buf, 0, sizeof buf);
    peak.store(0.0f);
}

void ChannelLive::rtSchedule(int64 clock, const NoteEvent& ev) {
    if (numSched_ >= kMaxSched) return;
    sched_[numSched_].clock = clock;
    sched_[numSched_].ev = ev;
    ++numSched_;
}

void ChannelLive::rtNote(int64 onClock, int64 offClock, const NoteRt& n) {
    if (offClock <= onClock) offClock = onClock + 1;
    if (n.flags & kNoteSlide) {             // slide notes bend what is sounding; they never start a voice
        NoteEvent e;
        e.type = NoteEvent::SlideTo;
        e.key = n.key;
        e.group = n.color;
        e.id = 0;
        e.slideSamples = (float)(offClock - onClock);
        rtSchedule(onClock, e);
        return;
    }
    if (numIn_ >= 256) return;
    const int64 sh = (int64)std::llround((double)shift.get() * kTicksPerStep * spt_);
    in_[numIn_++] = {onClock + sh, offClock + sh, n, -1};
}

void ChannelLive::rtLiveOn(int64 clock, int key, float velocity) {
    if (key < 0 || key > 127 || numIn_ >= 256) return;
    rtLiveOff(clock, key);
    NoteRt n;
    n.key = (short)key;
    n.velocity = velocity;
    const int64 sh = (int64)std::llround((double)shift.get() * kTicksPerStep * spt_);
    in_[numIn_++] = {clock + sh, kOpen, n, key};
}

void ChannelLive::rtLiveOff(int64 clock, int key) {
    if (key < 0 || key > 127) return;
    const int64 sh = (int64)std::llround((double)shift.get() * kTicksPerStep * spt_);
    for (int i = 0; i < numIn_; ++i)                    // not started yet (time shift): give it its end
        if (in_[i].liveKey == key && in_[i].off == kOpen) in_[i].off = std::max(in_[i].on + 1, clock + sh);
    for (int i = 0; i < numHeld_; ++i)                  // held by the arpeggiator
        if (held_[i].liveKey == key && held_[i].off == kOpen) held_[i].off = clock + sh;
    if (liveOut_[key]) {
        scheduleOff(liveOut_[key], clock + sh, liveEchoes_[key]);
        liveOut_[key] = 0;
    }
}

void ChannelLive::scheduleOff(uint32 id, int64 clock, int echoes) {
    NoteEvent e;
    e.type = NoteEvent::Off;
    e.id = id;
    rtSchedule(clock, e);
    const int64 et = (int64)std::llround((double)echoTime.get() * kTicksPerStep * spt_);
    for (int k = 1; k <= echoes; ++k) { e.id = id + (uint32)k; rtSchedule(clock + k * et, e); }
}

void ChannelLive::rtReleaseAll(int64 clock, bool dropPending) {
    int w = 0;
    for (int i = 0; i < numSched_; ++i) {
        Sched s = sched_[i];
        if (s.ev.type == NoteEvent::Off) s.clock = std::min(s.clock, clock);      // let go now
        else if (dropPending && s.clock >= clock && (s.ev.type == NoteEvent::On || s.ev.type == NoteEvent::SlideTo)) continue;   // not started yet
        sched_[w++] = s;
    }
    numSched_ = w;
    for (int k = 0; k < 128; ++k)
        if (liveOut_[k]) { scheduleOff(liveOut_[k], clock, 0); liveOut_[k] = 0; }
    if (dropPending) numIn_ = 0;
    numHeld_ = 0;
    arpNext_ = -1;
    arpStep_ = 0;
}

void ChannelLive::rtRescale(int64 now, double factor) {
    auto rs = [&](int64& c) { if (c != kOpen && c > now) c = now + (int64)std::llround((double)(c - now) * factor); };
    for (int i = 0; i < numSched_; ++i) rs(sched_[i].clock);
    for (int i = 0; i < numIn_; ++i) { rs(in_[i].on); rs(in_[i].off); }
    for (int i = 0; i < numHeld_; ++i) { rs(held_[i].on); rs(held_[i].off); }
    if (arpNext_ >= 0) rs(arpNext_);
}

// Starts one sounding note: tracking, mono / polyphony rules, portamento and echoes.
uint32 ChannelLive::emit(int64 on, int64 off, const NoteRt& n) {
    NoteEvent e;
    e.type = NoteEvent::On;
    e.key = n.key;
    e.group = n.color;
    const float octaves = (float)(n.key - kRootKey) / 12.0f;
    e.velocity = kDefaultVelocity + (n.velocity - kDefaultVelocity) * velTrack.get();
    e.pan = clampT(n.pan + keyPan.get() * octaves, -1.0f, 1.0f);
    e.modX = clampT(n.modX + keyModX.get() * octaves, 0.0f, 1.0f);
    e.modY = n.modY;
    e.fineCents = n.fine;
    e.release = n.release;

    const int limit = mono.getBool() ? 1 : poly.getInt();
    if (limit > 0) {
        const uint32 old = recent_[(recentW_ - limit + 64) & 63];
        if (old) { NoteEvent k; k.type = NoteEvent::KillNote; k.id = old; rtSchedule(on, k); }
    }
    if ((porta.getBool() || (n.flags & kNotePorta)) && lastKey_ >= 0.0f && lastKey_ != (float)n.key) {
        e.glideFromKey = lastKey_;
        e.slideSamples = (float)(portaTime.get() * 0.001 * sr_);
    }
    lastKey_ = (float)n.key;

    const int echoes = echoCount.getInt();
    const uint32 id = nextId_;
    nextId_ += (uint32)echoes + 1;
    e.id = id;
    rtSchedule(on, e);
    recent_[recentW_ & 63] = id;
    recentW_ = (recentW_ + 1) & 63;
    NoteEvent offEv;
    offEv.type = NoteEvent::Off;
    offEv.id = id;
    if (off != kOpen) rtSchedule(std::max(off, on + 1), offEv);

    if (echoes > 0) {
        const int64 et = (int64)std::llround((double)echoTime.get() * kTicksPerStep * spt_);
        const float feed = echoFeed.get();
        NoteEvent ec = e;
        ec.glideFromKey = -1.0f;
        for (int k = 1; k <= echoes; ++k) {
            ec.id = id + (uint32)k;
            ec.velocity *= feed;
            ec.pan = clampT(ec.pan + echoPan.get(), -1.0f, 1.0f);
            ec.key = clampT(ec.key + echoPitch.getInt(), 0, 127);
            ec.modX = clampT(ec.modX + echoModX.get() * 0.25f, 0.0f, 1.0f);
            ec.modY = clampT(ec.modY + echoModY.get() * 0.25f, 0.0f, 1.0f);
            rtSchedule(on + k * et, ec);
            if (off != kOpen) { offEv.id = ec.id; rtSchedule(std::max(off, on + 1) + k * et, offEv); }
        }
    }
    return id;
}

void ChannelLive::runArp(int64 qEnd) {
    static const int chords[9][5] = {{0, -1}, {0, 12, -1}, {0, 7, -1}, {0, 4, 7, -1}, {0, 3, 7, -1}, {0, 5, 7, -1},
                                     {0, 4, 7, 11, -1}, {0, 3, 7, 10, -1}, {0, 4, 7, 10, -1}};
    const int dir = arpDir.getInt();
    const int64 stepLen = std::max<int64>(1, (int64)std::llround((double)arpTime.get() * kTicksPerStep * spt_));
    for (int guard = 0; guard < 512; ++guard) {
        // drop notes that ended before the next arpeggio step
        const int64 at = arpNext_ >= 0 ? arpNext_ : INT64_MIN;
        int w = 0;
        for (int i = 0; i < numHeld_; ++i) if (held_[i].off == kOpen || held_[i].off > at) held_[w++] = held_[i];
        numHeld_ = w;
        if (numHeld_ == 0) { arpNext_ = -1; arpStep_ = 0; return; }
        if (arpNext_ < 0) {
            int64 first = held_[0].on;
            for (int i = 1; i < numHeld_; ++i) first = std::min(first, held_[i].on);
            arpNext_ = first;
            continue;                       // re-check note ends against the new start
        }
        if (arpNext_ >= qEnd) return;

        // the pool of keys to walk through
        int keys[160];
        const Held* src[160];
        int n = 0;
        const int chord = arpChord.getInt();
        const int range = arpRange.getInt();
        if (chord == 0) {
            int order[32];                  // held notes in ascending key order
            for (int i = 0; i < numHeld_; ++i) order[i] = i;
            for (int i = 1; i < numHeld_; ++i) {
                const int v = order[i];
                int j = i - 1;
                while (j >= 0 && held_[order[j]].n.key > held_[v].n.key) { order[j + 1] = order[j]; --j; }
                order[j + 1] = v;
            }
            for (int o = 0; o < range; ++o)
                for (int i = 0; i < numHeld_ && n < 160; ++i) { keys[n] = held_[order[i]].n.key + 12 * o; src[n] = &held_[order[i]]; ++n; }
        } else {
            const Held* low = &held_[0];
            for (int i = 1; i < numHeld_; ++i) if (held_[i].n.key < low->n.key) low = &held_[i];
            for (int o = 0; o < range; ++o)
                for (int i = 0; chords[chord][i] >= 0 && n < 160; ++i) { keys[n] = low->n.key + chords[chord][i] + 12 * o; src[n] = low; ++n; }
        }
        const int rep = std::max(1, arpRepeat.getInt());
        const int s = arpStep_ / rep;
        int idx = 0;
        switch (dir) {
            case 1: idx = s % n; break;
            case 2: idx = n - 1 - (s % n); break;
            case 3: { const int period = std::max(1, 2 * n - 2); const int q = s % period; idx = q < n ? q : period - q; break; }
            case 4: { const int period = 2 * n; const int q = s % period; idx = q < n ? q : period - 1 - q; break; }
            default:
                arpRand_ ^= arpRand_ << 13; arpRand_ ^= arpRand_ >> 17; arpRand_ ^= arpRand_ << 5;
                idx = (int)(arpRand_ % (uint32)n);
                break;
        }
        NoteRt note = src[idx]->n;
        note.key = (short)clampT(keys[idx], 0, 127);
        emit(arpNext_, arpNext_ + std::max<int64>(1, (int64)((double)stepLen * arpGate.get())), note);
        ++arpStep_;
        arpNext_ += stepLen;
    }
}

void ChannelLive::rtPrepare(int64 qStart) {
    const int64 qEnd = qStart + kQuantum;
    const bool arp = arpDir.getInt() != 0;
    const float gateSteps = gate.get();
    int w = 0;
    for (int i = 0; i < numIn_; ++i) {
        In in = in_[i];
        if (in.on >= qEnd) { in_[w++] = in; continue; }
        if (gateSteps > 0.0f && in.off != kOpen)
            in.off = std::min(in.off, in.on + std::max<int64>(1, (int64)((double)gateSteps * kTicksPerStep * spt_)));
        if (arp) {
            if (numHeld_ < 32) held_[numHeld_++] = {in.n, in.on, in.off, in.liveKey};
        } else {
            const uint32 id = emit(in.on, in.off, in.n);
            if (in.liveKey >= 0 && in.off == kOpen) { liveOut_[in.liveKey] = id; liveEchoes_[in.liveKey] = (unsigned char)echoCount.getInt(); }
        }
    }
    numIn_ = w;
    if (arp) runArp(qEnd);
    else if (numHeld_ > 0) { numHeld_ = 0; arpNext_ = -1; arpStep_ = 0; }
}

int ChannelLive::rtOnsets(int64 qStart, int64* clocks, int max) const {
    int n = 0;
    const int64 qEnd = qStart + kQuantum;
    for (int i = 0; i < numSched_ && n < max; ++i)
        if (sched_[i].ev.type == NoteEvent::On && sched_[i].clock < qEnd) clocks[n++] = sched_[i].clock;
    return n;
}

void ChannelLive::rtProcess(int64 qStart, double tempo, double tick, bool playing, float masterPitchSemis) {
    // Collect what is due in this quantum.
    int n = 0;
    const int64 qEnd = qStart + kQuantum;
    int w = 0;
    for (int i = 0; i < numSched_; ++i) {
        if (sched_[i].clock < qEnd && n < kMaxEventsPerQuantum) {
            events_[n] = sched_[i].ev;
            events_[n].offset = (int)std::max<int64>(0, sched_[i].clock - qStart);
            ++n;
        } else sched_[w++] = sched_[i];
    }
    numSched_ = w;
    // Order by time. At the same sample: cuts first, then an earlier note's Off before a later note's On.
    auto before = [](const NoteEvent& a, const NoteEvent& b) {
        if (a.offset != b.offset) return a.offset < b.offset;
        const bool ac = a.type < NoteEvent::On, bc = b.type < NoteEvent::On;
        if (ac != bc) return ac;
        if (a.id != b.id) return a.id < b.id;
        return (int)a.type < (int)b.type;
    };
    for (int i = 1; i < n; ++i) {
        NoteEvent e = events_[i];
        int j = i - 1;
        while (j >= 0 && before(e, events_[j])) { events_[j + 1] = events_[j]; --j; }
        events_[j + 1] = e;
    }
    for (int i = 0; i < n; ++i) if (events_[i].type == NoteEvent::On) { activity.fetch_add(1, std::memory_order_relaxed); break; }

    std::memset(buf, 0, sizeof buf);
    ProcessContext c;
    c.numSamples = kQuantum;
    c.sampleRate = sr_;
    c.tempo = tempo;
    c.tickPos = tick;
    c.playing = playing;
    c.io[0] = buf[0];
    c.io[1] = buf[1];
    c.events = events_;
    c.numEvents = n;
    c.pitchBendSemis = pitch.get() * pitchRange.get() + masterPitchSemis;
    plugin->process(c);

    const float g = mute.getBool() ? 0.0f : volumeToGain(volume.get());
    const float p = pan.get();
    const float tl = g * (p > 0.0f ? 1.0f - p : 1.0f), tr = g * (p < 0.0f ? 1.0f + p : 1.0f);
    if (!primed_) { gainL_.snap(tl); gainR_.snap(tr); primed_ = true; }
    gainL_.target(tl, kQuantum);
    gainR_.target(tr, kQuantum);
    float pk = 0.0f;
    if (gainL_.left == 0 && gainR_.left == 0 && tl == 1.0f && tr == 1.0f) {
        for (int i = 0; i < kQuantum; ++i) pk = std::max(pk, std::max(std::fabs(buf[0][i]), std::fabs(buf[1][i])));
    } else {
        for (int i = 0; i < kQuantum; ++i) {
            buf[0][i] *= gainL_.next();
            buf[1][i] *= gainR_.next();
            pk = std::max(pk, std::max(std::fabs(buf[0][i]), std::fabs(buf[1][i])));
        }
    }
    if (pk > peak.load(std::memory_order_relaxed)) peak.store(pk, std::memory_order_relaxed);
}

// ------------------------------------------------------------------ PreviewPlayer

void PreviewPlayer::play(SamplePtr s, float gain) {
    sample_.set(std::move(s));
    gain_.store(gain);
    playing_.store(true);
    trigger_.fetch_add(2);
}
void PreviewPlayer::stop() { trigger_.fetch_or(1); }

void PreviewPlayer::rtRender(float* l, float* r, int n, double sampleRate) {
    const SampleData* latest = sample_.rtAcquire();
    const int t = trigger_.load(std::memory_order_relaxed);
    if (t != seen_) {
        if ((t >> 1) != (seen_ >> 1)) {         // a new play request
            cur_ = latest; pos_ = 0.0; fade_ = 1.0f; stopping_ = false;
            trigger_.fetch_and(~1, std::memory_order_relaxed);
            seen_ = t & ~1;
        } else { stopping_ = true; seen_ = t; }
    }
    if (cur_ != latest) cur_ = nullptr;        // replaced before it was triggered: drop the stale pointer
    if (!cur_) { playing_.store(false, std::memory_order_relaxed); return; }
    const double inc = cur_->sampleRate / sampleRate;
    const int64 frames = cur_->frames();
    const float g = gain_.load(std::memory_order_relaxed);
    const float* sl = cur_->left();
    const float* sr = cur_->right();
    for (int i = 0; i < n; ++i) {
        const int64 idx = (int64)pos_;
        if (idx + 1 >= frames || fade_ <= 0.0f) { cur_ = nullptr; playing_.store(false, std::memory_order_relaxed); return; }
        const float f = (float)(pos_ - (double)idx);
        if (stopping_) fade_ -= 1.0f / 256.0f;
        const float a = g * std::max(0.0f, fade_);
        l[i] += a * (sl[idx] + (sl[idx + 1] - sl[idx]) * f);
        r[i] += a * (sr[idx] + (sr[idx + 1] - sr[idx]) * f);
        pos_ += inc;
    }
}

// ------------------------------------------------------------------ Engine

Engine::Engine() {
    masterPeak_[0].store(0); masterPeak_[1].store(0);
    for (auto& row : cc_) for (auto& v : row) v.store(-1.0f);
    std::memset(qOut_, 0, sizeof qOut_);
    std::memset(qOutX_, 0, sizeof qOutX_);
}
Engine::~Engine() = default;

void Engine::prepare(double sampleRate) {
    sr_ = sampleRate;
    tempo_ = 0.0;
    clock_ = 0;
    anchorClock_ = 0;
    anchorTick_ = 0.0;
    playing_ = false;
    outAvail_ = 0;
    outRead_ = 0;
    masterGain_.snap(1.0f);
    masterPrimed_ = false;
    modeKnown_ = false;
    // the input ring starts with kInputDelay samples of silence in it, which makes the delay constant
    std::memset(inRing_, 0, sizeof inRing_);
    std::memset(inQ_, 0, sizeof inQ_);
    inR_ = 0;
    inW_ = kInputDelay;
    if (!recRing_) recRing_ = std::make_unique<float[]>((size_t)kRecRing * 2);
    recW_.store(0);
    recR_.store(0);
    countInUntil_ = -1.0e18;
    inputPeak_[0].store(0);
    inputPeak_[1].store(0);
    recBase_ = -1;
    recActive_ = false;
    waiting_ = false;
    waitingPub_.store(false);
    calibSig_ = calibrationSignal(sampleRate);
    calibPos_ = calibLeft_ = 0;
    calibReq_.store(false);
    calibratingPub_.store(false);
    clickEnv_ = 0.0f;
    endedAt_.store(-1);
    chase_ = false;
}

void Engine::setCc(int channel, int cc, float value) {
    if (cc < 0 || cc > 127) return;
    value = clampT(value, 0.0f, 1.0f);
    cc_[clampT(channel, 0, 15)][cc].store(value, std::memory_order_relaxed);
    cc_[16][cc].store(value, std::memory_order_relaxed);
}
float Engine::ccValue(int channel, int cc) const {
    if (cc < 0 || cc > 127) return -1.0f;
    return cc_[channel < 0 || channel > 15 ? 16 : channel][cc].load(std::memory_order_relaxed);
}

// Automation clips and pattern events: each parameter's value is a function of the song position.
void Engine::applyAutomation(const RenderState& st, double tick) {
    const Param* touched = touched_.load(std::memory_order_relaxed);
    for (const AutoTargetRt& t : st.autoTargets)
        if (t.param != touched) t.param->setNorm(t.valueAt(tick));
}

// Controller links, from what the controllers put out in the previous quantum.
void Engine::applyLinks(const RenderState& st) {
    const Param* touched = touched_.load(std::memory_order_relaxed);
    for (const LinkRt& l : st.links) {
        float in;
        if (l.kind == ParamLink::Midi) {
            in = cc_[l.midiChannel < 0 || l.midiChannel > 15 ? 16 : l.midiChannel][l.cc & 127].load(std::memory_order_relaxed);
            if (in < 0.0f) continue;                // that control has not been moved yet
        } else {
            if (!l.controller) continue;
            in = l.controller->controlOutput(l.output);
        }
        double x = clampT((double)in, 0.0, 1.0);
        if (l.formula) x = clampT(l.formula->eval(&x), 0.0, 1.0);
        const float v = l.min + (l.max - l.min) * (float)x;
        LinkStateRt& s = *l.state;
        if (!s.primed || l.smoothMs <= 0.0f) { s.value = v; s.primed = true; }
        else s.value += (float)(1.0 - std::exp(-(double)kQuantum / ((double)l.smoothMs * 0.001 * sr_))) * (v - s.value);
        if (l.param != touched) l.param->setNorm(clampT(s.value, 0.0f, 1.0f));
    }
}

// The tempo is a parameter like any other: when it has changed, everything scheduled moves with it.
void Engine::syncTempo(const RenderState* st) {
    const double tempo = st ? (double)st->global->tempo.get() : 140.0;
    if (tempo == tempo_) return;
    const double newSpt = 60.0 * sr_ / (tempo * (double)kPPQ);
    if (tempo_ > 0.0) {
        anchorTick_ = tickAtClock(clock_);
        anchorClock_ = clock_;
        if (st) for (auto& ch : st->channels) ch->rtRescale(clock_, newSpt / spt_);
    }
    spt_ = newSpt;
    tempo_ = tempo;
}

void Engine::startRecording() {
    // the take starts at the ring's write position; anything left over from before is dropped
    const int64 w = recW_.load();
    recR_.store(w);
    recStartW_.store(w);
    RecSegment s;
    while (recSegs_.pop(s)) {}
    recDropped_.store(0);
    recStartTick_.store(-1.0e18);
    recording_.store(true);
}

std::vector<TakePiece> planTakes(const std::vector<RecSegment>& segments, int64 totalFrames, int latencyFrames,
                                 double samplesPerTick, double punchIn, double punchOut) {
    std::vector<TakePiece> out;
    const bool punch = punchOut > punchIn;
    for (size_t k = 0; k < segments.size(); ++k) {
        const RecSegment& s = segments[k];
        const bool last = k + 1 == segments.size();
        const int64 nextFrame = last ? totalFrames : segments[k + 1].frame;
        // A frame recorded at s.frame + i was performed at tick s.tick + (i - latency) / samplesPerTick.
        // The end of a pass is still in the file, just after the next pass begins; only the very last run is short by the latency.
        double endTick = s.tick + (double)(nextFrame - s.frame - (last ? latencyFrames : 0)) / samplesPerTick;
        double startTick = s.tick;
        if (punch) { startTick = std::max(startTick, punchIn); endTick = std::min(endTick, punchOut); }
        const double whole = std::ceil(startTick - 1.0e-9);
        TakePiece p;
        p.startTick = (int64)whole;
        p.sourceOffsetFrames = s.frame + latencyFrames + (int64)std::llround((whole - s.tick) * samplesPerTick);
        p.lengthTicks = (int64)std::floor(endTick - whole + 1.0e-9);
        if (p.sourceOffsetFrames + (int64)std::ceil((double)p.lengthTicks * samplesPerTick) > totalFrames)
            p.lengthTicks = (int64)std::floor((double)(totalFrames - p.sourceOffsetFrames) / samplesPerTick);
        if (p.lengthTicks >= 4 && p.sourceOffsetFrames >= 0) out.push_back(p);
    }
    return out;
}

void Engine::startCalibration() {
    if (recording_.load() || calibratingPub_.load()) return;
    const int64 w = recW_.load();
    recR_.store(w);
    recStartW_.store(w);
    calibratingPub_.store(true);
    calibReq_.store(true);
}

std::vector<float> Engine::calibrationSignal(double sampleRate) {
    // three short bursts of noise at uneven spacing, then quiet while the last one comes back
    std::vector<float> s((size_t)(sampleRate * 1.2), 0.0f);
    const int len = (int)(sampleRate * 0.012);
    dsp::Noise noise(0xCA11B8u);
    for (double at : {0.05, 0.25, 0.52}) {
        const size_t start = (size_t)(at * sampleRate);
        for (int i = 0; i < len && start + (size_t)i < s.size(); ++i) {
            const float w = (float)(0.5 - 0.5 * std::cos(kTwoPi * i / len));
            s[start + (size_t)i] = 0.15f * w * noise.next();
        }
    }
    return s;
}

int Engine::measureRoundTrip(const float* rec, int n, double sampleRate, float* quality) {
    if (quality) *quality = 0.0f;
    const auto sig = calibrationSignal(sampleRate);
    std::vector<int> idx;
    double es = 0.0;
    for (size_t i = 0; i < sig.size(); ++i) if (sig[i] != 0.0f) { idx.push_back((int)i); es += (double)sig[i] * sig[i]; }
    if (idx.empty()) return -1;
    const int maxLag = std::min(n - idx.back() - 1, (int)(0.6 * sampleRate));
    int best = -1;
    double bestV = 0.0;
    for (int lag = 0; lag <= maxLag; ++lag) {
        double c = 0.0;
        for (int i : idx) c += (double)sig[(size_t)i] * (double)rec[i + lag];
        if (std::fabs(c) > bestV) { bestV = std::fabs(c); best = lag; }
    }
    if (best < 0) return -1;
    double er = 0.0;
    for (int i : idx) er += (double)rec[i + best] * (double)rec[i + best];
    const double q = er > 0.0 ? bestV / std::sqrt(es * er) : 0.0;
    if (quality) *quality = (float)q;
    return q >= 0.5 ? best : -1;
}

int Engine::recordRead(float* dst, int maxFrames) {
    const int64 r = recR_.load(std::memory_order_relaxed), w = recW_.load(std::memory_order_acquire);
    const int n = (int)std::min<int64>(maxFrames, w - r);
    for (int i = 0; i < n; ++i) {
        const size_t at = (size_t)((r + i) & (kRecRing - 1)) * 2;
        dst[i * 2] = recRing_[at];
        dst[i * 2 + 1] = recRing_[at + 1];
    }
    recR_.store(r + n, std::memory_order_release);
    return n;
}

// Audio clips that overlap [segStart, segEnd) go into their mixer track's pre buffer.
void Engine::renderAudioClips(const RenderState& st, int64 segStart, int64 segEnd, int64 qStart) {
    const bool offline = offline_.load(std::memory_order_relaxed);
    for (const AudioClipRt& c : st.audioClips) {
        if (c.trackPos < 0 || (!c.sample && !c.stream)) continue;
        const int64 cs = clockOfTick((double)c.start), ce = clockOfTick((double)(c.start + c.length));
        const int64 a = std::max(cs, segStart), b = std::min(ce, segEnd);
        if (a >= b) continue;
        const int64 frames = c.frames;
        if (frames < 1) continue;
        TrackLive& live = *st.tracks[(size_t)c.trackPos].live;
        const double ratio = c.rate / sr_;
        const double head = (double)c.offsetFrames + (double)c.offset * spt_ * ratio;      // file frames before the clip's first sample
        const int64 span = ce - cs;
        const int64 fadeIn = (int64)std::llround((double)c.fadeIn * spt_), fadeOut = (int64)std::llround((double)c.fadeOut * spt_);
        const int64 edge = std::max<int64>(8, (int64)(0.002 * sr_));
        // a clip that starts or stops in the middle of its file gets a 2 ms fade so the cut does not click
        const bool softStart = head > 0.5, softEnd = head + (double)span * ratio < (double)frames - 1.0;
        auto posOf = [&](int64 k) {
            const double fp = head + (double)(k - cs) * ratio;
            return c.reverse ? (double)(frames - 1) - fp : fp;
        };
        // Output samples [k0, k1) from source frames that start at index `base` of sl / sr.
        auto run = [&](int64 k0, int64 k1, const float* sl, const float* sr, int64 base) {
            for (int64 k = k0; k < k1; ++k) {
                const int64 rel = k - cs, toEnd = span - 1 - rel;
                const double fp = posOf(k);
                if (fp < 0.0 || fp > (double)(frames - 1)) continue;
                float g = c.gain;
                if (fadeIn > 0 && rel < fadeIn) g *= (float)rel / (float)fadeIn;
                if (fadeOut > 0 && toEnd < fadeOut) g *= (float)toEnd / (float)fadeOut;
                if (softStart && rel < edge) g *= (float)rel / (float)edge;
                if (softEnd && toEnd < edge) g *= (float)toEnd / (float)edge;
                const int64 i0 = (int64)fp;
                const float f = (float)(fp - (double)i0);
                float l, r;
                if (f == 0.0f) { l = sl[i0 - base]; r = sr[i0 - base]; }
                else {
                    const int64 i1 = std::min(i0 + 1, frames - 1);
                    l = sl[i0 - base] + (sl[i1 - base] - sl[i0 - base]) * f;
                    r = sr[i0 - base] + (sr[i1 - base] - sr[i0 - base]) * f;
                }
                live.pre[0][k - qStart] += l * g;
                live.pre[1][k - qStart] += r * g;
            }
        };
        if (c.sample) { run(a, b, c.sample->left(), c.sample->right(), 0); continue; }
        // Streamed from disk: copy the frames this stretch needs out of the stream's loaded pages, then play from the copy.
        constexpr int kBuf = 1024;
        float tl[kBuf], tr[kBuf];
        const int64 per = std::max<int64>(1, (int64)((double)(kBuf - 4) / (ratio + 1.0)));
        for (int64 k0 = a; k0 < b; k0 += per) {
            const int64 k1 = std::min(b, k0 + per);
            const double p0 = posOf(k0), p1 = posOf(k1 - 1);
            const int64 lo = clampT<int64>((int64)std::floor(std::min(p0, p1)), 0, frames - 1);
            const int64 hi = clampT<int64>((int64)std::floor(std::max(p0, p1)) + 1, 0, frames - 1);
            if (!c.stream->read(lo, (int)(hi - lo + 1), tl, tr, offline)) streamMisses_.fetch_add(1, std::memory_order_relaxed);
            run(k0, k1, tl, tr, lo);
        }
    }
}

void Engine::startPlaying(double countInTicks) {
    playing_ = true;
    waiting_ = false;
    chase_ = true;
    endedAt_.store(-1, std::memory_order_relaxed);
    anchorClock_ = clock_;
    // a count-in runs the clock up to the start position from that far before it
    if (countInTicks > 0.0) { countInUntil_ = anchorTick_; anchorTick_ -= countInTicks; }
    else countInUntil_ = -1.0e18;
}

// The transport jumped back to the loop start while recording: what follows is a new take.
void Engine::markLoopPass(int64 clockAtWrap, int64 qStart, double tick) {
    if (recBase_ < 0) return;
    recSegs_.push({recBase_ + (clockAtWrap - qStart) - recStartW_.load(std::memory_order_relaxed), tick});
}

void Engine::setState(std::shared_ptr<const RenderState> s) { state_.set(std::move(s)); }

void Engine::post(const Command& c) {
    std::lock_guard<std::mutex> g(cmdMutex_);
    cmds_.push(c);
}
void Engine::play(double countInTicks, bool waitForInput) { Command c{Command::Play}; c.d = countInTicks; c.a = waitForInput ? 1 : 0; post(c); }
void Engine::stop() { post({Command::Stop}); }
void Engine::pause() { post({Command::Pause}); }
void Engine::seek(double tick) { Command c{Command::Seek}; c.d = tick; post(c); }
void Engine::noteOn(int channel, int key, float velocity) { Command c{Command::NoteOn}; c.a = channel; c.b = key; c.f = velocity; post(c); }
void Engine::noteOff(int channel, int key) { Command c{Command::NoteOff}; c.a = channel; c.b = key; post(c); }
void Engine::panic() { post({Command::Panic}); }
void Engine::reportCallbackLoad(float f) {
    const float prev = cpuPub_.load(std::memory_order_relaxed);
    cpuPub_.store(f > prev ? f : prev + (f - prev) * 0.05f, std::memory_order_relaxed);
}

int Engine::scopeRead(float* dst, int n) const {
    n = std::min(n, (int)kScope);
    const int w = scopeW_.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) dst[i] = scope_[(w - n + i + kScope * 2) % kScope];
    return n;
}

void Engine::releaseAll(const RenderState* st, bool dropPending) {
    if (!st) return;
    for (auto& ch : st->channels) ch->rtReleaseAll(clock_, dropPending);
}

void Engine::killAll(const RenderState* st) {
    if (!st) return;
    NoteEvent k;
    k.type = NoteEvent::Kill;
    for (auto& ch : st->channels) { ch->rtReleaseAll(clock_, true); ch->rtSchedule(clock_, k); }
}

void Engine::handleCommands(const RenderState* st) {
    Command c;
    while (cmds_.pop(c)) {
        switch (c.type) {
            case Command::Play:
                if (!playing_) {
                    if (c.a != 0) waiting_ = true;          // armed: starts when the input gets loud or a note is played
                    else startPlaying(c.d);
                }
                break;
            case Command::Pause:
                waiting_ = false;
                if (playing_) { playing_ = false; anchorTick_ = tickAtClock(clock_); anchorClock_ = clock_; releaseAll(st, true); }
                break;
            case Command::Stop:
                waiting_ = false;
                if (playing_) { playing_ = false; releaseAll(st, true); }
                else killAll(st);
                anchorTick_ = 0.0;
                anchorClock_ = clock_;
                break;
            case Command::Seek:
                anchorTick_ = std::max(0.0, c.d);
                anchorClock_ = clock_;
                chase_ = true;
                if (playing_) releaseAll(st, true);
                break;
            case Command::NoteOn:
                if (waiting_) startPlaying(0.0);
                if (st && c.a >= 0 && c.a < (int)st->channels.size()) {
                    st->channels[(size_t)c.a]->rtLiveOn(clock_, c.b, c.f);
                    for (int t : st->layerTargets[(size_t)c.a]) st->channels[(size_t)t]->rtLiveOn(clock_, c.b, c.f);
                }
                break;
            case Command::NoteOff:
                if (st && c.a >= 0 && c.a < (int)st->channels.size()) {
                    st->channels[(size_t)c.a]->rtLiveOff(clock_, c.b);
                    for (int t : st->layerTargets[(size_t)c.a]) st->channels[(size_t)t]->rtLiveOff(clock_, c.b);
                }
                break;
            case Command::Panic: killAll(st); break;
        }
    }
}

void Engine::loopRange(const RenderState& st, double& ls, double& le) const {
    if (st.songMode) {
        if (st.loopEnd > st.loopStart) { ls = (double)st.loopStart; le = (double)st.loopEnd; }
        else { ls = 0.0; le = (double)std::max<int64>(st.songLengthTicks, kTicksPerStep); }
    } else if (st.patLoopEnd > st.patLoopStart) {
        ls = (double)st.patLoopStart;
        le = (double)st.patLoopEnd;
    } else {
        ls = 0.0;
        int len = 16 * kTicksPerStep;
        if (st.currentPattern >= 0 && st.currentPattern < (int)st.patterns.size()) len = st.patterns[(size_t)st.currentPattern]->lengthTicks;
        le = (double)std::max(len, kTicksPerStep);
    }
}

void Engine::emitNotes(const RenderState& st, int64 segStart, int64 segEnd, double loopEnd) {
    const size_t numCh = st.channels.size();
    auto emitPattern = [&](const PatternRt& p, int64 shift, int minTick, int maxTick) {
        // shift: song tick of the pattern's tick 0. Notes with tick in [minTick, maxTick) are eligible.
        const size_t n = std::min(numCh, p.channelNotes.size());
        for (size_t c = 0; c < n; ++c) {
            const auto& notes = p.channelNotes[c];
            if (notes.empty()) continue;
            auto it = std::partition_point(notes.begin(), notes.end(), [&](const NoteRt& nt) {
                return nt.tick < minTick || clockOfTick((double)(shift + nt.tick)) < segStart;
            });
            for (; it != notes.end(); ++it) {
                if (it->tick >= maxTick) break;
                const double t = (double)(shift + it->tick);
                if (t >= loopEnd) break;
                const int64 on = clockOfTick(t);
                if (on >= segEnd) break;
                const int endTick = std::min(it->tick + it->length, maxTick);
                const int64 off = clockOfTick((double)(shift + endTick));
                st.channels[c]->rtNote(on, off, *it);
                for (int t2 : st.layerTargets[c]) st.channels[(size_t)t2]->rtNote(on, off, *it);
            }
        }
    };
    if (!st.songMode) {
        if (st.currentPattern >= 0 && st.currentPattern < (int)st.patterns.size())
            emitPattern(*st.patterns[(size_t)st.currentPattern], 0, 0, INT32_MAX);
        return;
    }
    const double tLo = tickAtClock(segStart) - 2.0, tHi = tickAtClock(segEnd) + 2.0;
    for (const ClipRt& cl : st.clips) {
        if ((double)cl.start > tHi) break;
        if ((double)(cl.start + cl.length) < tLo) continue;
        if (cl.pattern < 0 || cl.pattern >= (int)st.patterns.size()) continue;
        emitPattern(*st.patterns[(size_t)cl.pattern], cl.start - cl.offset, (int)cl.offset, (int)std::min<int64>(cl.offset + cl.length, INT32_MAX));
    }
}

void Engine::sequence(const RenderState& st, int64 qStart) {
    int64 seg = qStart;
    const int64 qEnd = qStart + kQuantum;
    for (int guard = 0; seg < qEnd && playing_ && guard < 256; ++guard) {
        double ls, le;
        loopRange(st, ls, le);
        const int64 endClock = clockOfTick(le);
        if (endClock <= seg) {                  // at or past the end (for example the pattern got shorter)
            if (looping_.load(std::memory_order_relaxed)) { anchorClock_ = seg; anchorTick_ = ls; markLoopPass(seg, qStart, ls); continue; }
            playing_ = false;
            endedAt_.store(seg, std::memory_order_relaxed);
            releaseAll(&st, false);
            break;
        }
        const int64 segEnd = std::min(qEnd, endClock);
        emitNotes(st, seg, segEnd, le);
        if (st.songMode) renderAudioClips(st, seg, segEnd, qStart);
        const bool metro = metronome_.load(std::memory_order_relaxed);
        if (metro || tickAtClock(seg) < countInUntil_) {
            // a click on every beat that starts inside this segment (always during a count-in)
            const double bt = (double)std::max(1, st.beatTicks);
            const int64 bpb = std::max(1, st.beatsPerBar);
            for (double beat = std::ceil((tickAtClock(seg) - 1.0) / bt); numClicks_ < 8; beat += 1.0) {
                const double t = beat * bt;
                if (t >= le) break;
                if (!metro && t >= countInUntil_) break;
                const int64 on = clockOfTick(t);
                if (on >= segEnd) break;
                if (on >= seg) clicks_[numClicks_++] = {(int)(on - qStart), ((((int64)beat % bpb) + bpb) % bpb) == 0};
            }
        }
        seg = segEnd;
        if (seg == endClock) {
            if (looping_.load(std::memory_order_relaxed)) { anchorClock_ = seg; anchorTick_ = ls; markLoopPass(seg, qStart, ls); }
            else { playing_ = false; anchorTick_ = le; anchorClock_ = seg; endedAt_.store(seg, std::memory_order_relaxed); releaseAll(&st, false); }
        }
    }
}

void Engine::renderQuantum() {
    const RenderState* st = state_.rtAcquire();
    if (!playing_) anchorClock_ = clock_;

    if (st) applyLinks(*st);
    syncTempo(st);
    if (st) for (auto& ch : st->channels) ch->rtBegin(spt_, sr_);
    // A switch between pattern and song mode rewinds. It is handled before this block's commands so that a
    // play command arriving together with the switch (recording does that) keeps its count-in.
    if (st) {
        if (!modeKnown_) { modeKnown_ = true; lastSongMode_ = st->songMode; }
        if (st->songMode != lastSongMode_) {
            lastSongMode_ = st->songMode;
            anchorTick_ = 0.0;
            anchorClock_ = clock_;
            if (playing_) releaseAll(st, true);
        }
    }
    handleCommands(st);
    if (st && chase_) {
        // The transport started or jumped: put every automated control where the song says it is at this
        // position. That makes what is heard depend on the position alone, so an export matches playback.
        chase_ = false;
        if (playing_) {
            applyAutomation(*st, tickAtClock(clock_));
            syncTempo(st);
            for (auto& ch : st->channels) ch->rtBegin(spt_, sr_);
        }
    }
    const double tempo = tempo_;

    std::memset(qOut_, 0, sizeof qOut_);
    if (!st) {
        preview.rtRender(qOut_[0], qOut_[1], kQuantum, sr_);
        clock_ += kQuantum;
        clockPub_.store(clock_, std::memory_order_relaxed);
        return;
    }


    // this quantum's share of the hardware input (already delayed by kInputDelay samples)
    for (int i = 0; i < kQuantum; ++i) {
        if (inR_ == inW_) { inQ_[0][i] = inQ_[1][i] = 0.0f; continue; }
        inQ_[0][i] = inRing_[0][inR_];
        inQ_[1][i] = inRing_[1][inR_];
        inR_ = (inR_ + 1) & (kInRing - 1);
    }
    {
        float p0 = 0.0f, p1 = 0.0f;
        for (int i = 0; i < kQuantum; ++i) { p0 = std::max(p0, std::fabs(inQ_[0][i])); p1 = std::max(p1, std::fabs(inQ_[1][i])); }
        if (p0 > inputPeak_[0].load(std::memory_order_relaxed)) inputPeak_[0].store(p0, std::memory_order_relaxed);
        if (p1 > inputPeak_[1].load(std::memory_order_relaxed)) inputPeak_[1].store(p1, std::memory_order_relaxed);
        // "wait for input": the first loud quantum starts the transport, and with it the take
        if (waiting_ && std::max(p0, p1) >= inputThreshold_.load(std::memory_order_relaxed)) startPlaying(0.0);
    }
    for (auto& t : st->tracks) if (t.usesPre) std::memset(t.live->pre, 0, sizeof t.live->pre);

    const double tickNow = tickAtClock(clock_);
    // recording: every input frame from the moment the position reaches its start (after any count-in),
    // and with punch in / out only while the position is inside the punch range
    if (calibReq_.load(std::memory_order_relaxed)) { calibReq_.store(false, std::memory_order_relaxed); calibPos_ = 0; calibLeft_ = (int)calibSig_.size(); }
    recBase_ = -1;
    bool recNow = calibLeft_ > 0;
    if (!recNow && recording_.load(std::memory_order_relaxed) && playing_ && tickNow >= countInUntil_) {
        const double pin = punchIn_.load(std::memory_order_relaxed), pout = punchOut_.load(std::memory_order_relaxed);
        recNow = pout <= pin || (tickNow + (double)kQuantum / spt_ > pin && tickNow < pout);
    }
    if (recNow) {
        const int64 w = recW_.load(std::memory_order_relaxed);
        if (w - recR_.load(std::memory_order_acquire) + kQuantum <= kRecRing) {
            if (!recActive_ && calibLeft_ <= 0) {
                recActive_ = true;
                if (recStartTick_.load(std::memory_order_relaxed) < -1.0e17) recStartTick_.store(tickNow, std::memory_order_relaxed);
                recSegs_.push({w - recStartW_.load(std::memory_order_relaxed), tickNow});
            }
            recBase_ = w;
            for (int i = 0; i < kQuantum; ++i) {
                const size_t at = (size_t)((w + i) & (kRecRing - 1)) * 2;
                recRing_[at] = inQ_[0][i];
                recRing_[at + 1] = inQ_[1][i];
            }
            recW_.store(w + kQuantum, std::memory_order_release);
        } else recDropped_.fetch_add(kQuantum, std::memory_order_relaxed);
    } else recActive_ = false;
    if (playing_) {
        sequence(*st, clock_);
        // automation: the value each control should reach by the end of this quantum
        if (playing_ && !st->autoTargets.empty()) applyAutomation(*st, tickAtClock(clock_ + kQuantum));
    }

    // input monitoring through the armed tracks
    for (auto& t : st->tracks) {
        if (!t.monitor || t.input == 0) continue;
        float* pl = t.live->pre[0];
        float* pr = t.live->pre[1];
        for (int i = 0; i < kQuantum; ++i) {
            const float a = inQ_[0][i], b = inQ_[1][i];
            if (t.input == 1) { pl[i] += a; pr[i] += a; }
            else if (t.input == 2) { pl[i] += b; pr[i] += b; }
            else { pl[i] += a; pr[i] += b; }
        }
    }

    // Channel settings stage: arpeggiator, echo, voice rules. Then cut groups across channels.
    for (auto& ch : st->channels) ch->rtPrepare(clock_);
    for (auto& a : st->channels) {
        const int group = a->cut.getInt();
        if (group <= 0) continue;
        int64 ons[32];
        const int n = a->rtOnsets(clock_, ons, 32);
        if (n == 0) continue;
        NoteEvent k;
        k.type = NoteEvent::Kill;
        for (auto& b : st->channels)
            if (b->cutBy.getInt() == group)
                for (int i = 0; i < n; ++i) b->rtSchedule(ons[i], k);
    }

    const float masterPitch = st->global->masterPitch.get() * 0.01f;
    int voices = 0;
    // Instruments are independent of each other: spread them over the worker threads.
    const int64 qClock = clock_;
    const bool qPlaying = playing_;
    auto runChannel = [&](int i) { st->channels[(size_t)i]->rtProcess(qClock, tempo, tickNow, qPlaying, masterPitch); };
    if (pool_ && st->channels.size() > 1) pool_->run((int)st->channels.size(), runChannel);
    else for (int i = 0; i < (int)st->channels.size(); ++i) runChannel(i);
    for (auto& ch : st->channels) voices += ch->plugin->activeVoices();

    if (st->tracks.empty() || st->masterPos < 0) {
        for (auto& ch : st->channels)
            for (int i = 0; i < kQuantum; ++i) { qOut_[0][i] += ch->buf[0][i]; qOut_[1][i] += ch->buf[1][i]; }
    } else {
        // Mixer tracks level by level; tracks in one level do not feed each other.
        for (size_t k = 0; k + 1 < st->levelStart.size(); ++k) {
            const int a = st->levelStart[k], n = st->levelStart[k + 1] - a;
            auto runTrack = [&](int i) { mixTrack(*st, a + i, tempo, tickNow); };
            if (pool_ && n > 1) pool_->run(n, runTrack);
            else for (int i = 0; i < n; ++i) runTrack(i);
        }
        const auto& master = *st->tracks[(size_t)st->masterPos].live;
        std::memcpy(qOut_[0], master.out[0], sizeof(float) * kQuantum);
        std::memcpy(qOut_[1], master.out[1], sizeof(float) * kQuantum);
    }
    // tracks with a hardware output of their own play there as well (after their fader, without the master's effects)
    if (anyHwOut_) std::memset(qOutX_, 0, sizeof qOutX_);
    anyHwOut_ = false;
    for (auto& t : st->tracks) {
        if (t.hwOut <= 0 || t.hwOut > kMaxOutPairs || t.index == kMasterTrack) continue;
        float* dl = t.hwOut == 1 ? qOut_[0] : qOutX_[t.hwOut - 2][0];
        float* dr = t.hwOut == 1 ? qOut_[1] : qOutX_[t.hwOut - 2][1];
        if (t.hwOut > 1 && !anyHwOut_) { std::memset(qOutX_, 0, sizeof qOutX_); anyHwOut_ = true; }
        for (int i = 0; i < kQuantum; ++i) { dl[i] += t.live->out[0][i]; dr[i] += t.live->out[1][i]; }
    }
    latencyPub_.store(st->latency, std::memory_order_relaxed);

    preview.rtRender(qOut_[0], qOut_[1], kQuantum, sr_);     // browser previews follow the master volume
    renderClicks();
    const float mv = st->global->masterVolume.get() / 0.8f;
    const float mg = mv == 1.0f ? 1.0f : std::pow(mv, 2.89f);
    if (!masterPrimed_) { masterGain_.snap(mg); masterPrimed_ = true; }     // no ramp from an arbitrary start
    masterGain_.target(mg, kQuantum);
    const bool unity = masterGain_.left == 0 && mg == 1.0f;
    if (!unity)
        for (int i = 0; i < kQuantum; ++i) {
            const float g = masterGain_.next();
            qOut_[0][i] *= g; qOut_[1][i] *= g;
            if (anyHwOut_) for (int p = 0; p < kMaxOutPairs - 1; ++p) { qOutX_[p][0][i] *= g; qOutX_[p][1][i] *= g; }
        }
    if (calibLeft_ > 0) {
        // the calibration bursts go out at a fixed level, whatever the master volume is
        for (int i = 0; i < kQuantum; ++i) {
            const size_t at = (size_t)(calibPos_ + i);
            const float v = at < calibSig_.size() ? calibSig_[at] : 0.0f;
            qOut_[0][i] += v;
            qOut_[1][i] += v;
        }
        calibPos_ += kQuantum;
        calibLeft_ -= kQuantum;
        if (calibLeft_ <= 0) calibratingPub_.store(false, std::memory_order_relaxed);
    }

    float pl = 0.0f, pr = 0.0f;
    int sw = scopeW_.load(std::memory_order_relaxed);
    for (int i = 0; i < kQuantum; ++i) {
        pl = std::max(pl, std::fabs(qOut_[0][i]));
        pr = std::max(pr, std::fabs(qOut_[1][i]));
        scope_[sw] = 0.5f * (qOut_[0][i] + qOut_[1][i]);
        sw = (sw + 1) % kScope;
    }
    scopeW_.store(sw, std::memory_order_relaxed);
    if (pl > masterPeak_[0].load(std::memory_order_relaxed)) masterPeak_[0].store(pl, std::memory_order_relaxed);
    if (pr > masterPeak_[1].load(std::memory_order_relaxed)) masterPeak_[1].store(pr, std::memory_order_relaxed);

    clock_ += kQuantum;
    if (!playing_) anchorClock_ = clock_;
    playingPub_.store(playing_, std::memory_order_relaxed);
    waitingPub_.store(waiting_, std::memory_order_relaxed);
    posPub_.store(tickAtClock(clock_), std::memory_order_relaxed);
    clockPub_.store(clock_, std::memory_order_relaxed);
    voicesPub_.store(voices, std::memory_order_relaxed);
}

void Engine::setThreads(int n) {
    n = clampT(n, 1, 64);
    if (n == threads()) return;
    pool_.reset();
    if (n > 1) pool_ = std::make_unique<WorkerPool>(n);
}

// One mixer track for one quantum: gather its inputs (delayed where plugin delay
// compensation says so), then run the strip.
void Engine::mixTrack(const RenderState& st, int pos, double tempo, double tick) {
    const TrackRt& t = st.tracks[(size_t)pos];
    TrackLive& live = *t.live;
    std::memset(live.in, 0, sizeof live.in);
    live.hasSide = false;
    for (int c : t.channels) {
        const auto& ch = *st.channels[(size_t)c];
        for (int i = 0; i < kQuantum; ++i) { live.in[0][i] += ch.buf[0][i]; live.in[1][i] += ch.buf[1][i]; }
    }
    if (t.usesPre)
        for (int i = 0; i < kQuantum; ++i) { live.in[0][i] += live.pre[0][i]; live.in[1][i] += live.pre[1][i]; }
    if (t.channelDelay > 0 && t.channelLine && (!t.channels.empty() || t.usesPre)) t.channelLine->process(live.in[0], live.in[1], kQuantum, t.channelDelay);
    for (const TrackInRt& in : t.inputs) {
        const TrackLive& src = *st.tracks[(size_t)in.from].live;
        float tmp[2][kQuantum];
        const float* sl = src.out[0];
        const float* sr = src.out[1];
        if (in.delay > 0 && in.rt->delay) {
            std::memcpy(tmp[0], sl, sizeof(float) * kQuantum);
            std::memcpy(tmp[1], sr, sizeof(float) * kQuantum);
            in.rt->delay->process(tmp[0], tmp[1], kQuantum, in.delay);
            sl = tmp[0]; sr = tmp[1];
        }
        if (in.sidechain) {
            if (!live.hasSide) { std::memset(live.side, 0, sizeof live.side); live.hasSide = true; }
            for (int i = 0; i < kQuantum; ++i) { live.side[0][i] += sl[i]; live.side[1][i] += sr[i]; }
            continue;
        }
        float g = 1.0f;
        if (in.level) { const float v = in.level->get(); g = v * v; }
        Ramp& ramp = in.rt->gain;
        if (!in.rt->primed) { ramp.snap(g); in.rt->primed = true; }
        ramp.target(g, kQuantum);
        if (ramp.left == 0 && g == 1.0f) {
            for (int i = 0; i < kQuantum; ++i) { live.in[0][i] += sl[i]; live.in[1][i] += sr[i]; }
        } else {
            for (int i = 0; i < kQuantum; ++i) { const float gg = ramp.next(); live.in[0][i] += sl[i] * gg; live.in[1][i] += sr[i] * gg; }
        }
    }
    TrackLive::SlotRt slots[kNumSlots];
    for (int s = 0; s < kNumSlots; ++s) { slots[s].plugin = t.slots[s].get(); slots[s].dry = t.slotDry[s].get(); slots[s].latency = t.slotLatency[s]; }
    live.rtProcess(slots, sr_, tempo, tick, playing_);
}

// Metronome: a short synthesized click, higher and louder on the first beat of the bar.
// Four sounds: a sine beep, a noise tick, a wood block and a bell.
void Engine::renderClicks() {
    int next = 0;
    for (int i = 0; i < kQuantum; ++i) {
        while (next < numClicks_ && clicks_[next].offset <= i) {
            const bool accent = clicks_[next].accent;
            clickKind_ = clickSound_.load(std::memory_order_relaxed);
            clickPhase_ = 0.0;
            clickAge_ = 0;
            switch (clickKind_) {
                case ClickTick:  clickHz_ = accent ? 3400.0 : 2500.0; clickEnv_ = accent ? 0.5f : 0.36f; break;
                case ClickBlock: clickHz_ = accent ? 1320.0 : 880.0;  clickEnv_ = accent ? 0.34f : 0.26f; break;
                case ClickBell:  clickHz_ = accent ? 845.0 : 587.0;   clickEnv_ = accent ? 0.26f : 0.2f; break;
                default:         clickHz_ = accent ? 1760.0 : 1100.0; clickEnv_ = accent ? 0.35f : 0.25f; break;
            }
            ++next;
        }
        if (clickEnv_ > 1.0e-4f) {
            float v;
            double decay;
            switch (clickKind_) {
                case ClickTick: {
                    clickNoise_ ^= clickNoise_ << 13; clickNoise_ ^= clickNoise_ >> 17; clickNoise_ ^= clickNoise_ << 5;
                    const float nz = (float)((double)clickNoise_ / 2147483648.0 - 1.0);
                    v = clickEnv_ * (0.55f * nz + 0.45f * (float)std::sin(clickPhase_));
                    decay = 0.004;
                    break;
                }
                case ClickBlock:
                    v = clickEnv_ * (float)(std::sin(clickPhase_) + 0.45 * std::sin(clickPhase_ * 2.76));
                    decay = 0.012;
                    break;
                case ClickBell:
                    v = clickEnv_ * (float)(std::tanh(2.5 * std::sin(clickPhase_)) + 0.8 * std::tanh(2.5 * std::sin(clickPhase_ * 1.4395)));
                    decay = 0.06;
                    break;
                default:
                    v = clickEnv_ * (float)std::sin(clickPhase_);
                    decay = 0.02;
                    break;
            }
            clickPhase_ += kTwoPi * clickHz_ / sr_;
            clickEnv_ *= (float)std::exp(-1.0 / (decay * sr_));
            ++clickAge_;
            qOut_[0][i] += v;
            qOut_[1][i] += v;
        }
    }
    numClicks_ = 0;
}

void Engine::process(const float* const* in, int numIn, float* const* out, int numOut, int n) {
    Rt::Scope rt;
    // take in the hardware input first: the quanta rendered below read it kInputDelay samples later
    const float* in0 = in != nullptr && numIn > 0 ? in[0] : nullptr;
    const float* in1 = in != nullptr && numIn > 1 ? in[1] : nullptr;
    for (int i = 0; i < n; ++i) {
        inRing_[0][inW_] = in0 ? in0[i] : 0.0f;
        inRing_[1][inW_] = in1 ? in1[i] : 0.0f;
        inW_ = (inW_ + 1) & (kInRing - 1);
    }
    int done = 0;
    while (done < n) {
        if (outAvail_ == 0) { renderQuantum(); outAvail_ = kQuantum; outRead_ = 0; }
        const int k = std::min(n - done, outAvail_);
        if (numOut >= 2) {
            std::memcpy(out[0] + done, qOut_[0] + outRead_, sizeof(float) * (size_t)k);
            std::memcpy(out[1] + done, qOut_[1] + outRead_, sizeof(float) * (size_t)k);
        } else if (numOut == 1) {
            for (int i = 0; i < k; ++i) out[0][done + i] = 0.5f * (qOut_[0][outRead_ + i] + qOut_[1][outRead_ + i]);
        }
        // outputs 3 and up carry the tracks that chose them, or silence
        for (int c = 2; c < numOut; ++c) {
            const int pair = c / 2;
            if (anyHwOut_ && pair < kMaxOutPairs) std::memcpy(out[c] + done, qOutX_[pair - 1][c & 1] + outRead_, sizeof(float) * (size_t)k);
            else std::memset(out[c] + done, 0, sizeof(float) * (size_t)k);
        }
        outRead_ += k;
        outAvail_ -= k;
        done += k;
    }
}

} // namespace pulp
