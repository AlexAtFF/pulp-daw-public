// Session: MIDI Out channels. Their messages are taken from the engine every millisecond and sent
// to the MIDI outputs of this PC.
#include "Session.h"
#include "pulp/MidiOut.h"

class Session::MidiOutPump : private juce::HighResolutionTimer {
public:
    explicit MidiOutPump(bool silent) : silent_(silent) {
        if (!silent_) devices_ = juce::MidiOutput::getAvailableDevices();
        startTimer(1);
    }
    ~MidiOutPump() override { stopTimer(); }

    // Message thread: the MIDI Out channels of the project as it now stands.
    void setChannels(std::vector<std::shared_ptr<pulp::Plugin>> list) {
        // the outputs they name are opened here, not on the timer's thread
        for (auto& p : list) {
            const int port = static_cast<pulp::MidiOut*>(p.get())->port();
            if (silent_ || port >= devices_.size() || port >= pulp::MidiOut::kPorts || outs_[port]) continue;
            auto out = juce::MidiOutput::openDevice(devices_[port].identifier);
            const juce::ScopedLock sl(lock_);
            outs_[port] = std::move(out);
        }
        const juce::ScopedLock sl(lock_);
        channels_ = std::move(list);
    }
    juce::StringArray names() const {
        juce::StringArray out;
        for (auto& d : devices_) out.add(d.name);
        return out;
    }
    juce::String log() const {
        const juce::ScopedLock sl(lock_);
        juce::String out = "midi out: sent " + juce::String(sent_) + (silent_ ? " (silent mode: counted, not sent)" : "") + ", last:";
        for (auto& m : recent_) out << " " << m;
        return out;
    }

private:
    void hiResTimerCallback() override {
        const juce::ScopedLock sl(lock_);
        for (auto& p : channels_) {
            auto* mo = static_cast<pulp::MidiOut*>(p.get());
            pulp::MidiOutMessage m;
            while (mo->pop(m)) {
                const int port = mo->port();
                if (port >= 0 && port < pulp::MidiOut::kPorts && outs_[port])
                    outs_[port]->sendMessageNow(m.size == 2 ? juce::MidiMessage(m.status, m.data1) : juce::MidiMessage(m.status, m.data1, m.data2));
                ++sent_;
                recent_.add(juce::String::toHexString((int)m.status) + ":" + juce::String((int)m.data1) + (m.size == 3 ? ":" + juce::String((int)m.data2) : juce::String()) + "@" + juce::String(port + 1));
                if (recent_.size() > 8) recent_.remove(0);
            }
        }
    }
    const bool silent_;
    juce::Array<juce::MidiDeviceInfo> devices_;
    std::unique_ptr<juce::MidiOutput> outs_[pulp::MidiOut::kPorts];
    std::vector<std::shared_ptr<pulp::Plugin>> channels_;
    juce::CriticalSection lock_;
    int sent_ = 0;
    juce::StringArray recent_;
};

void Session::refreshMidiOuts() {
    std::vector<std::shared_ptr<pulp::Plugin>> list;
    for (auto& c : project.channels) if (dynamic_cast<pulp::MidiOut*>(c.live->plugin.get())) list.push_back(c.live->plugin);
    if (list.empty() && !midiOut_) return;         // nothing is opened until a MIDI Out channel exists
    if (!midiOut_) midiOut_ = std::make_shared<MidiOutPump>(usingNullDevice());
    midiOut_->setChannels(std::move(list));
}

juce::StringArray Session::midiOutputNames() const {
    if (midiOut_) return midiOut_->names();
    juce::StringArray out;
    if (!usingNullDevice()) for (auto& d : juce::MidiOutput::getAvailableDevices()) out.add(d.name);
    return out;
}

juce::String Session::midiOutLog() const { return midiOut_ ? midiOut_->log() : juce::String("midi out: no MIDI Out channel"); }
