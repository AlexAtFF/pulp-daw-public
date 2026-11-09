"""Records what actually reaches the default Windows output device (WASAPI loopback),
to prove PULP makes the right sound on real hardware.

  python -I loopback_capture.py <seconds> <out.wav>            record
  python -I loopback_capture.py compare <capture.wav> <reference.wav> [seconds]
        line the capture up with an offline render of the same project and report how
        closely they match (1.0 = identical shape) over the first [seconds] of the reference
"""
import sys, time, wave
import numpy as np


def read(path):
    w = wave.open(path)
    rate, ch, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
    raw = w.readframes(w.getnframes())
    if width == 2:
        d = np.frombuffer(raw, dtype=np.int16).astype(np.float64) / 32768.0
    elif width == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)
        d = v.astype(np.float64) / 8388608.0
    else:
        d = np.frombuffer(raw, dtype=np.float32).astype(np.float64)
    return d.reshape(-1, ch).mean(axis=1), rate


def compare(cap_path, ref_path, seconds):
    cap, rate = read(cap_path)
    ref, rrate = read(ref_path)
    if rate != rrate:
        print("RESULT: sample rates differ (%d vs %d)" % (rate, rrate)); return 1
    if seconds:
        ref = ref[: int(seconds * rate)]
    peak = float(np.abs(cap).max()) if len(cap) else 0.0
    print("capture %.2f s, peak %.5f (%.1f dBFS); reference %.2f s" % (len(cap) / rate, peak, 20 * np.log10(max(peak, 1e-9)), len(ref) / rate))
    if peak < 1e-4 or len(cap) < len(ref):
        print("RESULT: capture is silent or too short"); return 1
    n = 1
    while n < len(cap) + len(ref):
        n <<= 1
    corr = np.fft.irfft(np.fft.rfft(cap, n) * np.conj(np.fft.rfft(ref, n)), n)[: len(cap) - len(ref) + 1]
    lag = int(np.argmax(corr))
    seg = cap[lag: lag + len(ref)]
    r = float(np.dot(seg, ref) / (np.linalg.norm(seg) * np.linalg.norm(ref) + 1e-20))
    gain = float(np.dot(seg, ref) / (np.dot(ref, ref) + 1e-20))
    print("best alignment at %.4f s, correlation %.5f, level relative to the render %.1f dB" % (lag / rate, r, 20 * np.log10(max(abs(gain), 1e-9))))
    print("RESULT:", "ok" if r > 0.98 else "does not match")
    return 0 if r > 0.98 else 1


if sys.argv[1] == "compare":
    sys.exit(compare(sys.argv[2], sys.argv[3], float(sys.argv[4]) if len(sys.argv) > 4 else None))

import pyaudiowpatch as pa
seconds = float(sys.argv[1]); out = sys.argv[2]
p = pa.PyAudio()
spk = p.get_default_wasapi_loopback()
rate = int(spk["defaultSampleRate"]); ch = spk["maxInputChannels"]
print("loopback device:", spk["name"], rate, "Hz", ch, "ch", flush=True)
frames = []
st = p.open(format=pa.paInt16, channels=ch, rate=rate, input=True, input_device_index=spk["index"],
            frames_per_buffer=1024, stream_callback=lambda d, n, t, s: (frames.append(d), (None, pa.paContinue))[1])
time.sleep(seconds)
st.stop_stream(); st.close(); p.terminate()
data = np.frombuffer(b"".join(frames), dtype=np.int16).reshape(-1, ch)
with wave.open(out, "wb") as w:
    w.setnchannels(ch); w.setsampwidth(2); w.setframerate(rate); w.writeframes(data.tobytes())
pk = float(np.abs(data).max()) / 32768.0 if len(data) else 0.0
print("captured %.2f s of sound, peak %.5f (%.1f dBFS)" % (len(data) / rate, pk, 20 * np.log10(max(pk, 1e-9))))
