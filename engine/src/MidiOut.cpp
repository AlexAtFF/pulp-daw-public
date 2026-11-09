#include "pulp/MidiOut.h"
#include "pulp/Effects.h"
#include <cmath>

namespace pulp {

MidiOut::MidiOut() {
    std::vector<std::string> ports;
    for (int i = 1; i <= kPorts; ++i) ports.push_back("Output " + std::to_string(i));
    pPort = addChoice("port", "MIDI output", ports, 0);
    pChannel = add({"channel", "MIDI channel", 1, 16, 1, 1, 16, "", {}});
    pProgram = add({"program", "Program (0 = leave alone)", 0, 128, 0, 1, 129, "", {}});
    pBank = add({"bank", "Bank (0 = leave alone)", 0, 128, 0, 1, 129, "", {}});
    pVelocity = addFloat("velocity", "Velocity scale", 0.1f, 2.0f, 1.0f, "");
}

std::vector<Preset> MidiOut::factoryPresets() const {
    return buildPresets(*this, {
        {"Channel 1", {{"channel", 1}}},
        {"Channel 2", {{"channel", 2}}},
        {"Channel 10 (drums on most instruments)", {{"channel", 10}}},
        {"Channel 1, piano (program 1)", {{"channel", 1}, {"program", 1}}},
        {"Channel 1, softer", {{"channel", 1}, {"velocity", 0.7f}}},
    });
}

void MidiOut::prepare(double) { reset(); sentProgram_ = sentBank_ = -2; sentChannel_ = -1; }

void MidiOut::reset() {
    numNotes_ = 0;
    sentBend_ = 0.0f;
}

void MidiOut::beginBlock(ProcessContext& c) {
    const unsigned ch = (unsigned)clampT(pChannel->getInt() - 1, 0, 15);
    // the instrument is told its bank and program when they are set, and again if the channel changes
    const int program = pProgram->getInt(), bank = pBank->getInt();
    if ((int)ch != sentChannel_) { sentChannel_ = (int)ch; sentProgram_ = sentBank_ = -2; }
    if (bank != sentBank_) { sentBank_ = bank; if (bank > 0) send(0xB0 | ch, 0, (unsigned)(bank - 1)); sentProgram_ = -2; }
    if (program != sentProgram_) { sentProgram_ = program; if (program > 0) send(0xC0 | ch, (unsigned)(program - 1), 0, 2); }
    // channel pitch becomes the pitch wheel (two semitones each way, the usual range)
    if (c.pitchBendSemis != sentBend_) {
        sentBend_ = c.pitchBendSemis;
        const int wheel = clampT(8192 + (int)std::lround(clampT(c.pitchBendSemis / 2.0f, -1.0f, 1.0f) * 8191.0f), 0, 16383);
        send(0xE0 | ch, (unsigned)(wheel & 0x7f), (unsigned)(wheel >> 7));
    }
}

void MidiOut::handleEvent(const NoteEvent& e, ProcessContext&) {
    const unsigned ch = (unsigned)clampT(pChannel->getInt() - 1, 0, 15);
    auto off = [&](int i) {
        send(0x80 | notes_[i].channel, notes_[i].key, 0);
        notes_[i] = notes_[--numNotes_];
    };
    switch (e.type) {
        case NoteEvent::On: {
            const unsigned key = (unsigned)clampT(e.key, 0, 127);
            const unsigned vel = (unsigned)clampT((int)std::lround(e.velocity * pVelocity->get() * 127.0f), 1, 127);
            send(0x90 | ch, key, vel);
            if (numNotes_ < 64) notes_[numNotes_++] = {e.id, (unsigned char)key, (unsigned char)ch};
            break;
        }
        case NoteEvent::Off:
        case NoteEvent::KillNote:
            for (int i = numNotes_; --i >= 0;) if (notes_[i].id == e.id) off(i);
            break;
        case NoteEvent::AllOff:
        case NoteEvent::Kill:
            while (numNotes_ > 0) off(numNotes_ - 1);
            send(0xB0 | ch, 123, 0);
            break;
        default: break;
    }
}

} // namespace pulp
