#!/usr/bin/env python3
"""Compares two WAV files: loudness of each, time offset between them and the residual
after subtracting one from the other (a "null test"). Used to check that Mixtopia's
playback/export path doesn't change the audio. Needs python3-numpy (container only).

    compare-audio.py original.wav rendered.wav
"""
import struct
import sys

import numpy as np


def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        sys.exit(f"{path}: not a WAV file")
    pos, fmt, samples = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, channels, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE:  # WAVE_FORMAT_EXTENSIBLE: real tag is in the sub-format GUID
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, channels, rate, bits)
        elif cid == b"data":
            samples = body
        pos += 8 + size + (size & 1)
    tag, channels, rate, bits = fmt
    if tag == 3:
        x = np.frombuffer(samples, dtype="<f4" if bits == 32 else "<f8").astype(np.float64)
    elif bits == 16:
        x = np.frombuffer(samples, dtype="<i2") / 32768.0
    elif bits == 24:
        b = np.frombuffer(samples, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = np.where(v >= 1 << 23, v - (1 << 24), v) / float(1 << 23)
    elif bits == 32:
        x = np.frombuffer(samples, dtype="<i4") / float(1 << 31)
    else:
        sys.exit(f"{path}: unsupported {bits}-bit format {tag}")
    return x.reshape(-1, channels), rate, f"{bits}-bit {'float' if tag == 3 else 'PCM'}"


def db(v):
    return 20 * np.log10(max(v, 1e-12))


def main():
    a, rate_a, desc_a = read_wav(sys.argv[1])
    b, rate_b, desc_b = read_wav(sys.argv[2])
    for name, x, rate, desc in (("original", a, rate_a, desc_a), ("rendered", b, rate_b, desc_b)):
        print(f"{name:9s} {desc}, {rate} Hz, {x.shape[1]} ch, {len(x) / rate:.3f} s, "
              f"peak {db(np.abs(x).max()):6.2f} dBFS, RMS {db(np.sqrt(np.mean(x ** 2))):6.2f} dBFS")
    if rate_a != rate_b:
        sys.exit("sample rates differ; can't null-test")

    # Find the time offset with cross-correlation on the first channel (a few seconds is enough).
    n = min(len(a), len(b), rate_a * 5)
    corr = np.fft.irfft(np.fft.rfft(b[:n, 0], 2 * n) * np.conj(np.fft.rfft(a[:n, 0], 2 * n)))
    lag = int(np.argmax(corr))
    if lag > n:
        lag -= 2 * n
    a2, b2 = (a, b[lag:]) if lag >= 0 else (a[-lag:], b)
    m = min(len(a2), len(b2))
    a2, b2 = a2[:m, :min(a.shape[1], b.shape[1])], b2[:m, :min(a.shape[1], b.shape[1])]
    gain = db(np.sqrt(np.mean(b2 ** 2)) / np.sqrt(np.mean(a2 ** 2)))
    residual = db(np.sqrt(np.mean((b2 - a2) ** 2)) / np.sqrt(np.mean(a2 ** 2)))
    print(f"offset    {lag} samples ({1000 * lag / rate_a:.2f} ms)")
    print(f"level     rendered is {gain:+.3f} dB vs original")
    print(f"residual  {residual:.1f} dB below the signal (below -90 dB = identical in practice)")


if __name__ == "__main__":
    main()
