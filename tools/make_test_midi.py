# Writes out/m13/tempo.mid for scripts/m14_tempomap.txt: four bars of piano with three tempos
# (bars 1 and 2 at 120 BPM, bar 3 at 90, bar 4 at 150; format 1, 96 ticks to the quarter note).
import os, struct

def var(v):
    b = [v & 0x7f]
    v >>= 7
    while v:
        b.append((v & 0x7f) | 0x80)
        v >>= 7
    return bytes(reversed(b))

def tempo(bpm):
    return b"\xff\x51\x03" + int(60e6 / bpm).to_bytes(3, "big")

Q = 96
BAR = 4 * Q
conductor = b"\x00" + tempo(120) + b"\x00\xff\x58\x04\x04\x02\x18\x08"
conductor += var(2 * BAR) + tempo(90) + var(BAR) + tempo(150) + b"\x00\xff\x2f\x00"

notes = b"\x00\xff\x03\x05Piano" + b"\x00\xc0\x00"
last = 0
events = []
for bar in range(4):
    for beat in range(4):
        t = bar * BAR + beat * Q
        events.append((t, 0x90, 60 + (bar * 2 + beat) % 12, 90))
        events.append((t + Q - 8, 0x80, 60 + (bar * 2 + beat) % 12, 0))
events.sort()
for t, status, key, vel in events:
    notes += var(t - last) + bytes([status, key, vel])
    last = t
notes += b"\x00\xff\x2f\x00"

data = b"MThd" + struct.pack(">IHHH", 6, 1, 2, Q)
data += b"MTrk" + struct.pack(">I", len(conductor)) + conductor
data += b"MTrk" + struct.pack(">I", len(notes)) + notes
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "out", "m13")
os.makedirs(out, exist_ok=True)
open(os.path.join(out, "tempo.mid"), "wb").write(data)
print("wrote", os.path.normpath(os.path.join(out, "tempo.mid")), len(data), "bytes")
