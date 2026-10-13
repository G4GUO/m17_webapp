#!/usr/bin/env python3
"""Compare two raw 8 kHz s16le files (input speech vs decoded output).

Chunked, drift tolerant comparison: each chunk of the input is matched against
a window of the output, so a variable streaming delay does not hide a working
voice path. A steadily sliding best offset means the pipeline is dropping or
inserting samples.

    python3 tools/audio_compare.py input.raw output.raw [--start N] [--len N]
"""
import argparse
import array
import math
import os
import sys


def load(path, start=0, count=None):
    with open(path, "rb") as fh:
        data = fh.read()
    a = array.array("h")
    a.frombytes(data[: len(data) & ~1])
    if start:
        a = a[start:]
    if count:
        a = a[:count]
    return a


def best_corr(x, y, lo, hi):
    """Best normalised correlation of x against y[lo:hi], with its offset."""
    best = (-1.0, 0, 0)
    n = len(x)
    if hi - lo < n:
        hi = lo + n
    if hi > len(y):
        hi = len(y)
        lo = max(0, hi - n)
    if hi - lo < n // 2:
        return best
    step = 1 if n < 4000 else 2
    for off in range(lo, hi - n + 1):
        sxy = sxx = syy = 0.0
        for i in range(0, n, step * 2):
            a, b = x[i], y[off + i]
            sxy += a * b
            sxx += a * a
            syy += b * b
        if sxx <= 0 or syy <= 0:
            continue
        c = sxy / math.sqrt(sxx * syy)
        if c > best[0]:
            best = (c, off, n)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--start", type=int, default=0, help="skip N samples of input")
    ap.add_argument("--len", type=int, default=24000, help="samples of input to use")
    ap.add_argument("--chunks", type=int, default=8)
    ap.add_argument("--search", type=int, default=8000, help="+/- search window in output")
    args = ap.parse_args()

    for p in (args.input, args.output):
        if not os.path.exists(p):
            print(f"missing: {p}")
            return 1

    inp = load(args.input, args.start, args.len)
    out = load(args.output)
    print(f"input : {len(inp)} samples ({len(inp)/8000:.2f} s) from {args.input}")
    print(f"output: {len(out)} samples ({len(out)/8000:.2f} s) from {args.output}")
    if len(out) < 4000:
        print("!! almost no decoded audio came back - the voice path is not delivering audio")
        return 2

    clen = max(800, len(inp) // args.chunks)
    scores, offsets = [], []
    print(f"\n{'chunk':>5} {'input@':>8} {'best corr':>9} {'output off':>10}")
    for k in range(args.chunks):
        x = inp[k * clen:(k + 1) * clen]
        if len(x) < 800:
            continue
        c, off, _n = best_corr(x, out, max(0, k * clen - args.search), k * clen + args.search)
        scores.append(c)
        offsets.append(off)
        print(f"{k:>5} {k*clen:>8} {c:>9.3f} {off:>10}")

    good = [c for c in scores if c > 0.4]
    mean = sum(scores) / max(1, len(scores)) if scores else -1
    print(f"\nmean correlation {mean:.3f}, chunks above 0.4: {len(good)}/{len(scores)}")
    if len(offsets) > 2:
        drift = offsets[-1] - offsets[0]
        print(f"offset drift across the file: {drift} samples ({drift/8000*1000:.0f} ms)")
    verdict = "PASS" if (mean > 0.4 or len(good) >= max(1, len(scores) // 2)) else (
        "WEAK" if mean > 0.15 else "FAIL")
    print(f"verdict: {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
