// Session: automation from the interface (create a clip, record knob movements, links to
// controllers) and MIDI hardware (devices, hot-plugging, notes, controllers, learn).
#include "Session.h"

// One open MIDI input. Its role switches are read on the MIDI thread.
class Session::MidiPort : public juce::MidiInputCallback {
public:
    MidiPort(Session& s, const juce::MidiDeviceInfo& info) : s_(s), id(info.identifier), name(info.name) {}
    ~MidiPort() override { if (input) input->stop(); }
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& m) override { s_.handleMidi(m, notes.load(), controllers.load()); }
    Session& s_;
    juce::String id, name;
    std::atomic<bool> notes{true}, controllers{true};
    std::unique_ptr<juce::MidiInput> input;
};

// ------------------------------------------------------------------ automation clips and links

int Session::createAutomationClip(pulp::Param* p) {
    const std::string address = project.addressOf(p);
    if (address.empty()) return -1;
    // the first empty playlist track; the clip runs the length of the song (at least four bars)
    int track = 0;
    for (; track < pulp::kNumPlaylistTracks - 1; ++track) {
        bool used = false;
        for (auto& c : project.clips) if (c.track == track) { used = true; break; }
        if (!used) break;
    }
    const pulp::int64 length = juce::jmax<pulp::int64>(project.songLengthTicks(), (pulp::int64)project.ticksPerBar() * 4);
    for (auto& c : project.clips) c.selected = false;
    project.addAutomationClip(address, track, 0, length).selected = true;
    if (project.tracks[(size_t)track].name.empty()) project.tracks[(size_t)track].name = "Automation";
    if (project.tracks[(size_t)track].height < 2.0f) project.tracks[(size_t)track].height = 2.2f;      // room to draw the curve
    commit("Create automation clip");
    return (int)project.clips.size() - 1;
}

std::vector<Session::ControllerSource> Session::controllerSources() const {
    std::vector<ControllerSource> out;
    for (int t = 0; t < pulp::kNumMixerTracks; ++t)
        for (int s = 0; s < pulp::kNumSlots; ++s) {
            const auto& plug = project.mixer[(size_t)t].slots[s];
            if (!plug) continue;
            for (int o = 0; o < plug->numControlOutputs(); ++o)
                out.push_back({t, s, o, juce::String(pulp::Project::mixerTrackName(t)) + ", slot " + juce::String(s + 1) + ": " + plug->displayName() + " (" + plug->controlOutputName(o) + ")"});
        }
    return out;
}

void Session::setLink(const pulp::ParamLink& link) {
    project.setLink(link);
    commit("Link to controller");
}

void Session::removeLink(const std::string& address) {
    if (project.removeLink(address)) commit("Remove link");
}

void Session::beginMidiLearn(pulp::Param* p) {
    learnAddress_ = project.addressOf(p);
    learnArmed_.store(!learnAddress_.empty());
    notify();
}

void Session::cancelMidiLearn() {
    learnArmed_.store(false);
    learnAddress_.clear();
    notify();
}

// ------------------------------------------------------------------ recording knob movements

void Session::paramTouched(pulp::Param* p, int phase) {
    if (!p) return;
    if (phase == 0) engine.setTouched(p);
    else if (phase == 2) engine.setTouched(nullptr);
    if (phase == 2 || !recording || !engine.isPlaying()) return;
    recordParamValue(p);
}

void Session::recordParamValue(pulp::Param* p) {
    auto& r = autoRec_[p];
    if (r.address.empty()) {
        r.address = project.addressOf(p);
        if (r.address.empty()) { autoRec_.erase(p); return; }
    }
    const int tick = (int)juce::jmax(0.0, engine.positionTicks());
    if (!r.events.empty() && r.events.back().tick == tick) r.events.back().value = p->getNorm();
    else r.events.push_back({tick, p->getNorm(), true});
}

void Session::finishAutomationRecording() {
    if (autoRec_.empty()) return;
    int written = 0;
    for (auto& kv : autoRec_) {
        auto ev = kv.second.events;
        if (ev.empty() || project.findParam(kv.second.address) == nullptr) continue;
        pulp::sortEvents(ev);
        pulp::simplifyEvents(ev, 0.004f);
        ev.back().ramp = false;                             // the last value holds
        const int first = ev.front().tick, last = ev.back().tick;
        if (!project.songMode) {
            // pattern mode: the movement becomes events in the pattern, replacing what was there over the same stretch
            auto& dst = project.pattern().events[kv.second.address];
            dst.erase(std::remove_if(dst.begin(), dst.end(), [&](const pulp::AutoEvent& e) { return e.tick >= first && e.tick <= last; }), dst.end());
            dst.insert(dst.end(), ev.begin(), ev.end());
            pulp::sortEvents(dst);
        } else {
            // song mode: the movement becomes an automation clip where it was made
            int track = 0;
            for (; track < pulp::kNumPlaylistTracks - 1; ++track) {
                bool used = false;
                for (auto& c : project.clips) if (c.track == track) { used = true; break; }
                if (!used) break;
            }
            auto& clip = project.addAutomationClip(kv.second.address, track, first, juce::jmax(pulp::kTicksPerStep, last - first));
            auto* a = project.autoById(clip.autoId);
            a->points.clear();
            for (auto& e : ev) a->points.push_back({(pulp::int64)(e.tick - first), e.value, 0.0f, pulp::ShapeCurve});
            if (a->points.size() == 1) a->points.push_back({(pulp::int64)pulp::kTicksPerStep, a->points[0].value, 0.0f, pulp::ShapeCurve});
            a->name += " (recorded)";
            if (project.tracks[(size_t)track].name.empty()) project.tracks[(size_t)track].name = "Automation";
            if (project.tracks[(size_t)track].height < 2.0f) project.tracks[(size_t)track].height = 2.2f;
        }
        ++written;
    }
    autoRec_.clear();
    midiLinkSeen_.clear();
    if (written > 0) commit("Record automation");
}

// ------------------------------------------------------------------ MIDI devices

void Session::loadMidiSettings() {
    midiRoles_.clear();
    if (auto x = loadSettings("MIDI"))
        for (auto* e : x->getChildWithTagNameIterator("DEVICE"))
            midiRoles_[e->getStringAttribute("name")] = {e->getBoolAttribute("notes", true), e->getBoolAttribute("controllers", true)};
}

void Session::saveMidiSettings() {
    auto x = std::make_unique<juce::XmlElement>("MIDI");
    for (auto& kv : midiRoles_) {
        auto* e = x->createNewChildElement("DEVICE");
        e->setAttribute("name", kv.first);
        e->setAttribute("notes", kv.second.first);
        e->setAttribute("controllers", kv.second.second);
    }
    saveSettings(std::move(x));
}

// Opens every MIDI input that is connected and closes the ones that have gone. Called at start and whenever
// Windows reports that a device was plugged in or removed.
void Session::refreshMidiDevices() {
    if (midiDisabled_) { notify(); return; }
    const auto available = juce::MidiInput::getAvailableDevices();
    for (int i = (int)midiPorts_.size(); --i >= 0;) {
        bool still = false;
        for (auto& d : available) still = still || d.identifier == midiPorts_[(size_t)i]->id;
        if (!still) midiPorts_.erase(midiPorts_.begin() + i);
    }
    for (auto& d : available) {
        bool have = false;
        for (auto& p : midiPorts_) have = have || p->id == d.identifier;
        if (have) continue;
        auto port = std::make_shared<MidiPort>(*this, d);
        const auto role = midiRoles_.find(d.name);
        if (role != midiRoles_.end()) { port->notes = role->second.first; port->controllers = role->second.second; }
        port->input = juce::MidiInput::openDevice(d.identifier, port.get());
        if (port->input) port->input->start();
        midiPorts_.push_back(std::move(port));
    }
    notify();
}

std::vector<Session::MidiDevice> Session::midiDevices() const {
    std::vector<MidiDevice> out;
    for (auto& p : midiPorts_) out.push_back({p->name, p->notes.load(), p->controllers.load(), p->input != nullptr});
    return out;
}

void Session::setMidiDeviceRole(int index, bool notes, bool controllers) {
    if (index < 0 || index >= (int)midiPorts_.size()) return;
    auto& p = *midiPorts_[(size_t)index];
    p.notes = notes;
    p.controllers = controllers;
    midiRoles_[p.name] = {notes, controllers};
    saveMidiSettings();
    notify();
}

void Session::startMidi(bool openDevices) {
    loadMidiSettings();
    midiDisabled_ = !openDevices;
    if (!openDevices) return;
    midiWatch_ = juce::MidiDeviceListConnection::make([this] { refreshMidiDevices(); });
    refreshMidiDevices();
}

void Session::stopMidi() {
    midiWatch_.reset();
    midiPorts_.clear();
}

// A message from a MIDI device (on the MIDI thread) or from a script (on the message thread).
void Session::handleMidi(const juce::MidiMessage& m, bool notes, bool controllers) {
    if (m.isNoteOnOrOff() && notes) {
        const int key = m.getNoteNumber();
        const bool on = m.isNoteOn();
        const float velocity = m.getFloatVelocity();
        // sound at once, from this thread; what is written into the pattern is done on the interface thread
        if (on) engine.noteOn(currentChannel, key, velocity);
        else engine.noteOff(currentChannel, key);
        const double tick = engine.positionTicks();
        juce::MessageManager::callAsync([this, key, on, velocity, tick] { if (on) liveNoteOn(key, velocity, false, tick); else liveNoteOff(key, false, tick); });
    } else if (m.isController() && controllers) {
        const int ch = m.getChannel() - 1, cc = m.getControllerNumber();
        engine.setCc(ch, cc, (float)m.getControllerValue() / 127.0f);
        if (learnArmed_.exchange(false))
            juce::MessageManager::callAsync([this, ch, cc] {
                if (learnAddress_.empty()) return;
                pulp::ParamLink link;
                if (auto* old = project.linkFor(learnAddress_)) link = *old;        // keep range, smoothing and formula
                link.target = learnAddress_;
                link.kind = pulp::ParamLink::Midi;
                link.midiChannel = ch;
                link.cc = cc;
                learnAddress_.clear();
                setLink(link);
            });
    } else if (m.isPitchWheel() && notes) {
        const float bend = (float)(m.getPitchWheelValue() - 8192) / 8192.0f;
        juce::MessageManager::callAsync([this, bend] { if (auto* ch = current()) ch->live->pitch.set(bend); });
    } else return;
    const auto text = m.getDescription();
    juce::MessageManager::callAsync([this, text] { lastMidiText = text; });
}
