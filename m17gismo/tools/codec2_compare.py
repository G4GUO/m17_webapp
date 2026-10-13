#!/usr/bin/env python3
"""Compare two Codec2 runs produced by tests/codec2_frame_codec.

    tools/codec2_compare.py stock.c2 mod.c2 [--stock-raw stock.raw --mod-raw mod.raw]

Reports whether the encoded bitstreams are identical (the property that matters
for on-air compatibility with other M17 stations) and, when the decoded raw
files are given, how far the audio differs (the modified codec only promises
bitstream compatibility, not identical decoded audio).

Codec2 is a vocoder, so decoded audio is compared on energy envelopes, not
sample-by-sample waveform correlation.
"""
import argparse
import array
import math
import sys


def read_bytes(path):
    with open(path, "rb") as fh:
        return fh.read()


def read_pcm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    a = array.array("h")
    a.frombytes(data[: len(data) & ~1])
    return a


def envelopes(pcm, frames, offset=0, flen=160):
    out = []
    for k in range(frames):
        base = offset + k * flen
        if base + flen > len(pcm):
            break
        s = 0
        for i in range(flen):
            v = pcm[base + i]
            s += v * v
        out.append(math.sqrt(s / flen))
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
    ap.add_argument("stock_c2")
    ap.add_argument("mod_c2")
    ap.add_argument("--stock-raw")
    ap.add_argument("--mod-raw")
    ap.add_argument("--frame-bytes", type=int, default=8)
    args = ap.parse_args()

    a = read_bytes(args.stock_c2)
    b = read_bytes(args.mod_c2)
    fb = args.frame_bytes
    na, nb = len(a) // fb, len(b) // fb
    print(f"stock bitstream : {len(a)} bytes ({na} frames)")
    print(f"mod   bitstream : {len(b)} bytes ({nb} frames)")

    n = min(na, nb)
    differing = 0
    first = -1
    bit_errors = 0
    for i in range(n):
        x, y = a[i * fb:(i + 1) * fb], b[i * fb:(i + 1) * fb]
        if x != y:
            if first < 0:
                first = i
            differing += 1
            for p, q in zip(x, y):
                bit_errors += bin(p ^ q).count("1")

    print(f"identical frames: {n - differing}/{n}")
    if differing:
        print(f"  first difference at frame {first}, {bit_errors} bit errors total")
        i = first
        print(f"  stock {a[i*fb:(i+1)*fb].hex()}  mod {b[i*fb:(i+1)*fb].hex()}")
    else:
        print("  bitstreams are byte-for-byte identical")

    if args.stock_raw and args.mod_raw:
        ra, rb = read_pcm(args.stock_raw), read_pcm(args.mod_raw)
        frames = min(len(ra), len(rb)) // 160
        ea, eb = envelopes(ra, frames), envelopes(rb, frames)
        print(f"\ndecoded audio   : stock {len(ra)} samples, mod {len(rb)} samples")
        print(f"  envelope correlation over {frames} frames: {corr(ea, eb):.4f}")
        maxd = max(abs(ra[i] - rb[i]) for i in range(min(len(ra), len(rb))))
        print(f"  max sample difference: {maxd}")
        if maxd == 0:
            print("  decoded audio is sample-exact as well")

    verdict = "PASS" if differing == 0 else "FAIL"
    print(f"\nBITEXACT: {verdict} (encoder output {'identical' if differing == 0 else 'differs'})")
    return 0 if differing == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
