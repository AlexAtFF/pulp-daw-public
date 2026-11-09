#pragma once
// MIDI Out: a channel that makes no sound of its own. Its notes leave the engine as MIDI messages,
// which the program sends to a hardware or software MIDI output.
#include "pulp/Plugin.h"

namespace pulp {

struct MidiOutMessage {
    unsigned char status = 0, data1 = 0, data2 = 0;
    unsigned char size = 3;         // 2 for program change
};

class MidiOut : public Instrument {
public:
    static constexpr int kPorts = 8;
    MidiOut();
    const char* typeId() const override { return "midiout"; }
    const char* displayName() const override { return "MIDI Out"; }
    void prepare(double sampleRate) override;
    void reset() override;
    std::vector<Preset> factoryPresets() const override;
    // The program's side: take the next message that is waiting (any one thread).
    bool pop(MidiOutMessage& m) { return queue_.pop(m); }
    int port() const { return pPort->getInt(); }            // which of the program's outputs, from 0
    Param *pPort, *pChannel, *pProgram, *pBank, *pVelocity;

protected:
    void beginBlock(ProcessContext& ctx) override;
    void handleEvent(const NoteEvent& e, ProcessContext& ctx) override;
    void render(float*, float*, int, ProcessContext&) override {}

private:
    void send(unsigned status, unsigned d1, unsigned d2, unsigned size = 3) { queue_.push({(unsigned char)status, (unsigned char)d1, (unsigned char)d2, (unsigned char)size}); }
    struct Sounding { uint32 id; unsigned char key, channel; };
    Sounding notes_[64];
    int numNotes_ = 0;
    int sentProgram_ = -2, sentBank_ = -2, sentChannel_ = -1;
    float sentBend_ = 0.0f;
    SpscQueue<MidiOutMessage, 1024> queue_;
};

} // namespace pulp
