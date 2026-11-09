#!/usr/bin/env python3
"""Generate assets/sounds/call.caf (incoming-call ring, 29 s) and assets/sounds/msg.caf
(message tone, about 1.1 s), the iPhone notification sounds.

Regenerate (macOS, from the repo root):
    python3 scripts/ui/gen_ringtone.py
It writes each as a 44.1 kHz 16-bit mono WAV with the standard library only, then runs
    afconvert -f caff -d <ima4 for call, LEI16 for msg> <wav> assets/sounds/call.caf
UNNotificationSound plays at most 30 s, so the file is 29 s. LEI16 is 2.6 MB, over the 1.5 MB budget, so ima4 is used (about 0.65 MB).
"""
import math, os, struct, subprocess, sys, tempfile, wave

RATE = 44100
DURATION = 29.0
PERIOD = 2.5            # one motif every 2.5 s: ~1 s sounding, ~1.5 s silence
NOTES = [(0.0, 784.0), (0.30, 1046.5)]   # G5 then C6, a soft two-note motif
PEAK = 0.708            # about -3 dBFS

def bell(t, f):
    """Marimba/bell partials with a gentle attack and exponential decay."""
    if t < 0:
        return 0.0
    attack = min(1.0, t / 0.012)
    s = 0.0
    for mult, amp, decay in ((1.0, 1.0, 4.5), (2.0, 0.35, 7.0), (3.99, 0.12, 11.0)):
        s += amp * math.sin(2 * math.pi * f * mult * t) * math.exp(-decay * t)
    return s * attack

def render(duration, events, note_len):
    """Mix (start_seconds, hz) notes into a mono buffer, fade the tail, normalise to PEAK."""
    n = int(RATE * duration)
    buf = [0.0] * n
    for start, f in events:
        for i in range(int(RATE * note_len)):
            idx = int(start * RATE) + i
            if idx >= n:
                break
            buf[idx] += bell(i / RATE, f)
    for i in range(int(RATE * 0.05)):
        buf[n - 1 - i] *= i / (RATE * 0.05)
    m = max(abs(x) for x in buf) or 1.0
    return [x / m * PEAK for x in buf]

def write_caf(samples, out, fmt):
    with tempfile.TemporaryDirectory() as d:
        wav = os.path.join(d, "tone.wav")
        with wave.open(wav, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
            w.writeframes(b"".join(struct.pack("<h", int(x * 32767)) for x in samples))
        subprocess.check_call(["afconvert", "-f", "caff", "-d", fmt, wav, out])

def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "sounds")
    call = []
    k = 0
    while k * PERIOD < DURATION:
        call += [(k * PERIOD + off, f) for off, f in NOTES]
        k += 1
    write_caf(render(DURATION, call, 1.0), os.path.join(out_dir, "call.caf"), "ima4")  # LEI16 would be 2.6 MB
    # Message tone: a soft "ding-dong" (E6 then B5), about 1.1 s with a gentle decay.
    write_caf(render(1.1, [(0.0, 1318.5), (0.22, 987.8)], 0.9), os.path.join(out_dir, "msg.caf"), "LEI16")

if __name__ == "__main__":
    main()
