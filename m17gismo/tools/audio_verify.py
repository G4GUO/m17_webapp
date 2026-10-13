#!/usr/bin/env python3
"""Verify the m17web voice path against a reference c2enc/c2dec decode.

Codec2 3200 is a vocoder: the decoded waveform is synthesised, so sample level
correlation with the original speech is a poor metric.  Two useful checks:

  * envelope correlation against the reference decode (same codec, same input)
  * sample level agreement with the reference decode

Usage:
    tools/audio_verify.py REFERENCE.raw device_under_test.raw [--frames 200]
"""
import argparse
import array
import math
import sys


def load(path):
    with open(path, "rb") as fh:
        data = fh.read()
    a = array.array("h")
    a.frombytes(data[: len(data) & ~1])
    return a


def envelopes(a, num_frames, offset=0):
    out = []
    for k in range(num_frames):
        base = offset + k * 160
        if base + 160 > len(a):
            break
        s = 0
        for i in range(160):
            v = a[base + i]
            s += v * v
        out.append(math.sqrt(s / 160.0))
    return out


def corr(x, y):
    n = min(len(x), len(y))
    if n < 4:
        return -1.0
    mx = sum(x[:n]) / n
    my = sum(y[:n]) / n
    num = sum((x[i] - mx) * (y[i] - my) for i in range(n))
    dx = math.sqrt(sum((v - mx) ** 2 for v in x[:n]))
    dy = math.sqrt(sum((v - my) ** 2 for v in y[:n]))
    return num / max(1e-9, dx * dy)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("device")
    ap.add_argument("--frames", type=int, default=140, help="20 ms frames to compare")
    ap.add_argument("--search", type=int, default=4000, help="alignment search in samples")
    args = ap.parse_args()

    ref = load(args.reference)
    dut = load(args.device)
    print(f"reference : {len(ref)} samples ({len(ref)/8000:.2f} s)")
    print(f"m17web out: {len(dut)} samples ({len(dut)/8000:.2f} s)")
    if len(dut) < 1600:
        print("!! m17web produced almost no audio")
        return 2

    er = envelopes(ref, args.frames)
    best = (-1.0, 0)
    for off in range(0, args.search, 160):
        em = envelopes(dut, args.frames, off)
        c = corr(er, em)
        if c > best[0]:
            best = (c, off)
    em = envelopes(dut, args.frames)
    print(f"envelope correlation (no offset)      : {corr(er, em):.3f}")
    print(f"envelope correlation (best offset {best[1]:>5}): {best[0]:.3f}")

    # sample level agreement at the best offset
    off = best[1]
    n = min(len(er), len(em)) * 160
    num = sum(abs(ref[i] - dut[off + i]) for i in range(0, n, 7))
    den = sum(abs(ref[i]) for i in range(0, n, 7))
    ratio = num / max(1.0, den)
    print(f"mean |ref - dut| / mean |ref| : {ratio:.3f} (lower is better)")
    print("first 6 ref envelope:", [round(v) for v in er[:6]])
    print("first 6 dut envelope:", [round(v) for v in em[:6]])
    verdict = "PASS" if best[0] > 0.7 else ("WEAK" if best[0] > 0.4 else "FAIL")
    print(f"verdict: {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
