#!/usr/bin/env python3
"""Checks time-stretch results. Needs python3-numpy (container only).

    analyze-tempo.py clicks <render.wav> <expected bpm> [first click seconds]
        Finds click onsets; reports their spacing (-> BPM) and how far each lands from the
        expected beat grid.
    analyze-tempo.py pitch <render.wav> <expected Hz>
        Reports the dominant frequency (pitch must survive stretching).
    analyze-tempo.py make-clicks <out.wav> <bpm> <seconds> <first click seconds>
    analyze-tempo.py make-tone <out.wav> <Hz> <seconds>
"""
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
read_wav = __import__("compare-audio").read_wav
RATE = 44100


def write_wav(path, x):
    d = (np.clip(x, -1, 1) * 32767).astype("<i2").tobytes()
    Path(path).write_bytes(b"RIFF" + struct.pack("<I", 36 + len(d)) + b"WAVEfmt "
                           + struct.pack("<IHHIIHH", 16, 1, 2, RATE, RATE * 4, 4, 16)
                           + b"data" + struct.pack("<I", len(d)) + d)


def make_clicks(out, bpm, seconds, first):
    x = np.zeros(int(RATE * seconds))
    burst = np.sin(2 * np.pi * 1000 * np.arange(441) / RATE) * np.exp(-np.arange(441) / 80)
    t = first
    while t < seconds - 0.02:
        i = int(round(t * RATE))
        x[i:i + len(burst)] += 0.8 * burst
        t += 60.0 / bpm
    write_wav(out, np.stack([x, x], 1))


def make_tone(out, hz, seconds):
    t = np.arange(int(RATE * seconds)) / RATE
    x = 0.5 * np.sin(2 * np.pi * hz * t)
    write_wav(out, np.stack([x, x], 1))


def onsets(x, rate):
    env = np.abs(x.mean(axis=1))
    threshold = 0.25 * env.max()
    found, i, gap = [], 0, int(0.1 * rate)
    above = np.flatnonzero(env > threshold)
    for idx in above:
        if not found or idx - found[-1] > gap:
            found.append(idx)
    return np.array(found) / rate


def clicks(path, bpm, first):
    x, rate, _ = read_wav(path)
    t = onsets(x, rate)
    beat = 60.0 / bpm
    if len(t) < 3:
        sys.exit(f"only {len(t)} clicks found")
    spacing = np.diff(t)
    grid_error = (t - first) / beat
    grid_error = (grid_error - np.round(grid_error)) * beat * 1000
    print(f"clicks found      {len(t)} (first at {t[0]:.4f} s)")
    print(f"measured tempo    {60.0 / spacing.mean():.3f} BPM (want {bpm})")
    print(f"spacing jitter    max {np.abs(spacing - beat).max() * 1000:.2f} ms")
    print(f"off-grid          mean {grid_error.mean():+.2f} ms, worst {np.abs(grid_error).max():.2f} ms")


def pitch(path, hz):
    x, rate, _ = read_wav(path)
    mid = x[len(x) // 4: 3 * len(x) // 4].mean(axis=1)
    spectrum = np.abs(np.fft.rfft(mid * np.hanning(len(mid))))
    k = int(np.argmax(spectrum))
    # Parabolic interpolation for sub-bin accuracy.
    a, b, c = spectrum[k - 1], spectrum[k], spectrum[k + 1]
    k += 0.5 * (a - c) / (a - 2 * b + c)
    found = k * rate / len(mid)
    print(f"dominant pitch    {found:.2f} Hz (want {hz}, error {1200 * np.log2(found / hz):+.1f} cents)")
    print(f"length            {len(x) / rate:.3f} s")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "clicks":
        clicks(sys.argv[2], float(sys.argv[3]), float(sys.argv[4]) if len(sys.argv) > 4 else 0.0)
    elif cmd == "pitch":
        pitch(sys.argv[2], float(sys.argv[3]))
    elif cmd == "make-clicks":
        make_clicks(sys.argv[2], float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5]))
    elif cmd == "make-tone":
        make_tone(sys.argv[2], float(sys.argv[3]), float(sys.argv[4]))
