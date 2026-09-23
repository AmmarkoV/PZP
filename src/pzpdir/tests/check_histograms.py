#!/usr/bin/env python3
"""
Check the histogram tables of an archive (spec §3.8) against an independent recomputation:
  - hist_rgb:   PIL's own Image.convert("L") for 8-bit images, the high byte for 16-bit ones;
  - hist_seg:   np.histogram of the label channel;
  - hist_depth: np.histogram of the 16-bit depth over 256 equal bins of 0..65536;
every per-file row equal, rows summing to 65535 within rounding, and the pixel-weighted global row equal.

Usage:
    check_histograms.py ARCHIVE [--rgb STREAM] [--seg STREAM[:CH]] [--depth STREAM[:H,L]] [--sample N]

Repository : https://github.com/AmmarkoV/PZP
Author     : Ammar Qammaz (AmmarkoV)
"""

import argparse
import io
import os
import random
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
import pzp.pzpdir as pzpdir   # noqa: E402


def image_counts(a, o, stream, kind, spec):
    inf = a.info(o, stream)
    if inf is None:
        return None
    if kind == "rgb":
        from PIL import Image
        img = Image.open(io.BytesIO(a.read(o, stream)))
        if img.mode.startswith("I"):
            v = (np.array(img).astype(np.uint32) >> 8).astype(np.uint8)
        else:
            v = np.array(img.convert("L"))
        return np.histogram(v, bins=256, range=(0, 256))[0]
    arr = a.read_image(o, stream)
    if arr.ndim == 3 and arr.shape[2] == 1:
        arr = arr[:, :, 0]
    if kind == "seg":
        return np.histogram(arr if arr.ndim == 2 else arr[:, :, spec], bins=256, range=(0, 256))[0]
    d = arr.astype(np.int64) if arr.dtype != np.uint8 else arr[:, :, spec[0]].astype(np.int64) * 256 + arr[:, :, spec[1]]
    return np.histogram(d, bins=256, range=(0, 65536))[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archive")
    ap.add_argument("--rgb")
    ap.add_argument("--seg")
    ap.add_argument("--depth")
    ap.add_argument("--sample", type=int, default=0, help="check N random records only (the global row still uses all)")
    args = ap.parse_args()
    jobs = []
    if args.rgb:
        jobs.append(("rgb", args.rgb, None))
    if args.seg:
        s, _, c = args.seg.partition(":")
        jobs.append(("seg", s, int(c) if c else 0))
    if args.depth:
        s, _, hl = args.depth.partition(":")
        jobs.append(("depth", s, tuple(int(x) for x in hl.split(",")) if hl else (1, 2)))
    fails = 0
    with pzpdir.open(args.archive) as a:
        n = len(a)
        for kind, stream, spec in jobs:
            idx, rows = a.table_all("hist_" + kind)
            g = a.global_table("hist_%s_global" % kind)
            total, files, bad, sums = np.zeros(256, np.int64), 0, 0, []
            sample = set(random.Random(1).sample(range(n), args.sample)) if args.sample else None
            for o in range(n):
                c = image_counts(a, o, stream, kind, spec)
                have = idx[o + 1] - idx[o]
                if c is None:
                    bad += have != 0
                    continue
                total += c
                files += 1
                if sample is not None and o not in sample:
                    continue
                want = (c * 65535 + int(c.sum()) // 2) // int(c.sum())
                got = rows["h"][idx[o]] if have == 1 else None
                if got is None or not np.array_equal(got.astype(np.int64), want):
                    bad += 1
                else:
                    sums.append(int(got.sum()))
            gw = (total * 65535 + int(total.sum()) // 2) // int(total.sum())
            gok = len(g) == 1 and np.array_equal(g["h"][0].astype(np.int64), gw) and int(g["pixels"][0]) == int(total.sum()) and int(g["files"][0]) == files
            spread = (min(sums) - 65535, max(sums) - 65535) if sums else (0, 0)
            ok = bad == 0 and gok and -128 <= spread[0] and spread[1] <= 128
            fails += not ok
            print("%s%-6s\033[0m %d files checked, %d wrong rows, global %s, row sums 65535%+d..%+d" %
                  ("\033[32m" if ok else "\033[31m", kind, len(sums), bad, "ok" if gok else "WRONG", spread[0], spread[1]))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
