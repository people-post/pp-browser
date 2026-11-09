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

def env(t, dur, attack, release):
    if t < 0 or t >= dur:
        return 0.0
    return min(1.0, t / attack) * min(1.0, (dur - t) / release)

def render_fn(duration, events, peak):
    """events: (start_s, length_s, fn(t)->sample); mixed, faded at the tail, normalised to peak."""
    n = int(RATE * duration)
    buf = [0.0] * n
    for start, length, fn in events:
        base = int(start * RATE)
        for i in range(int(length * RATE)):
            if base + i >= n:
                break
            buf[base + i] += fn(i / RATE)
    for i in range(int(RATE * 0.05)):
        buf[n - 1 - i] *= i / (RATE * 0.05)
    m = max(abs(x) for x in buf) or 1.0
    return [x / m * peak for x in buf]

def phone_bell(t):
    """Bright telephone bell: 1000 + 1250 Hz with 20 Hz tremolo and weaker 2nd/3rd harmonics."""
    trem = 0.6 + 0.4 * math.sin(2 * math.pi * 20 * t)
    s = 0.0
    for f, a in ((1000, 1.0), (1250, 1.0), (2000, 0.25), (2500, 0.25), (3000, 0.12), (3750, 0.12)):
        s += a * math.sin(2 * math.pi * f * t)
    return s * trem * env(t, 0.4, 0.005, 0.06)

def call_a():
    ev = []
    k = 0
    while k * 3.0 < DURATION:
        for b in (0.0, 0.6):
            ev.append((k * 3.0 + b, 0.4, phone_bell))
        k += 1
    return render_fn(DURATION, ev, 0.891)  # -1 dBFS

def marimba(f):
    def fn(t):
        s = 0.0
        for mult, amp, decay in ((1.0, 1.0, 14.0), (4.0, 0.5, 40.0), (9.2, 0.15, 90.0)):
            s += amp * math.sin(2 * math.pi * f * mult * t) * math.exp(-decay * t)
        return s * min(1.0, t / 0.002)
    return fn

def call_b():
    motif = (1046.5, 1318.5, 1568.0, 2093.0)   # C6 E6 G6 C7, 120 ms apart
    period = 2 * 4 * 0.12 + 0.12 + 1.6         # two motifs back to back, then a pause
    ev = []
    k = 0
    while k * period < DURATION:
        for rep in range(2):
            for j, f in enumerate(motif):
                ev.append((k * period + (rep * 4 + j) * 0.12, 0.35, marimba(f)))
        k += 1
    return render_fn(DURATION, ev, 0.891)

def main():
    args = sys.argv[1:]
    which = "default"
    if args[:1] == ["--candidate"]:
        which, args = args[1], args[2:]
    out_dir = args[0] if args else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "sounds")
    if which in ("a", "b"):
        # --candidate a|b writes call-a.caf / call-b.caf (ring candidates) to out_dir.
        write_caf(call_a() if which == "a" else call_b(), os.path.join(out_dir, "call-%s.caf" % which), "ima4")
        return
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
