#!/usr/bin/env python3
"""
pzpdir_histograms.py - Add per-file and archive-wide 256-bin histograms to a .pzpd archive (spec §3.8).

For every record that has a blob in the stream, one row of a bulk record table (`h:u16[256]`), and one row of a
global table for the whole archive (`h:u16[256], pixels:u64, files:u64`). Values are fractions of the pixels,
65535 meaning 1.0 (each bin rounded to nearest, so a row sums to about 65535).

  --rgb STREAM          hist_rgb:   luminance (PIL's "L": (19595 R + 38470 G + 7471 B + 32768) >> 16), 256 levels
  --seg STREAM[:CH]     hist_seg:   segmentation label = channel CH (default 0) of an 8-bit image, 256 labels
  --depth STREAM[:H,L]  hist_depth: 16-bit depth in 256 fixed bins (depth >> 8); a 16-bit single-channel image, or
                                    two 8-bit channels H (high byte) and L (low byte), default 1,2 (label + depth files)

The global histogram is pixel-weighted: the pixel counts of every file are summed, then normalized. Tables that
already exist are replaced; no record data is rewritten (spec §7). Run it again after the files change.

Usage:
    pzpdir_histograms.py coco_val2017_rgb_all.pzpd --rgb rgb --seg all:0 --depth all:1,2 [--workers N]

Needs numpy, PIL (for non-PZP images) and the pzp.pzpdir module.

Repository : https://github.com/AmmarkoV/PZP
Author     : Ammar Qammaz (AmmarkoV)
"""

import argparse
import io
import os
import sys
from multiprocessing import Pool

import numpy as np

try:
    import pzp.pzpdir as pzpdir
except ImportError:
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    import pzp.pzpdir as pzpdir

SCHEMA = "h:u16[256]"
GLOBAL_SCHEMA = "h:u16[256] pixels:u64 files:u64"


def decode(a, ordinal, stream):
    """A blob as a numpy array: PZP natively, other images through PIL (palette / CMYK images as RGB)."""
    inf = a.info(ordinal, stream)
    if inf is None:
        return None
    if inf["format"] in ("PZP ", "PZPC"):
        return a.read_image(ordinal, stream)
    from PIL import Image
    img = Image.open(io.BytesIO(a.read(ordinal, stream)))
    if img.mode.startswith("I"):
        # 16-bit images open as "I;16" or, with older Pillow, as 32-bit "I": both become uint16
        return np.clip(np.array(img), 0, 65535).astype(np.uint16)
    if img.mode not in ("L", "RGB", "RGBA"):
        img = img.convert("RGB")
    return np.array(img)


def luminance(arr):
    """8-bit luminance with PIL's integer rule (Image.convert("L")); 16-bit input is reduced to its high byte."""
    if arr.dtype != np.uint8:
        arr = (arr.astype(np.uint32) >> 8).astype(np.uint8)
    if arr.ndim == 2 or arr.shape[2] == 1:
        return arr.reshape(arr.shape[0], arr.shape[1])
    r, g, b = (arr[:, :, i].astype(np.uint32) for i in range(3))
    return ((19595 * r + 38470 * g + 7471 * b + 32768) >> 16).astype(np.uint8)


def depth16(arr, hi_lo):
    """The 16-bit depth of an image: itself (16-bit, one channel) or its (high, low) 8-bit channels."""
    if arr.dtype == np.uint16:
        return arr if arr.ndim == 2 else arr[:, :, 0]
    if arr.ndim != 3:
        raise ValueError("an 8-bit depth image needs two channels (high, low)")
    hi, lo = hi_lo
    return (arr[:, :, hi].astype(np.uint16) << 8) | arr[:, :, lo]


def counts_of(arr, kind, spec):
    """Pixel counts of the 256 bins (int64)."""
    if kind == "rgb":
        v = luminance(arr)
    elif kind == "seg":
        if arr.dtype != np.uint8:
            raise ValueError("segmentation labels must be 8-bit")
        v = arr if arr.ndim == 2 else arr[:, :, spec]
    else:
        v = (depth16(arr, spec) >> 8).astype(np.uint8)
    return np.bincount(v.ravel(), minlength=256).astype(np.int64)


def normalize(counts):
    """Counts -> u16 fractions of the total (65535 = 1.0), rounded to nearest."""
    total = int(counts.sum())
    if total == 0:
        return np.zeros(256, dtype=np.int64)
    return (counts * 65535 + total // 2) // total


_A = None
_JOBS = None


def _init(path, jobs):
    global _A, _JOBS
    _A = pzpdir.open(path)
    _JOBS = jobs


def _work(rng):
    """Counts for the ordinals of one range: [(ordinal, {kind: counts or None})]."""
    out = []
    for o in range(*rng):
        got = {}
        cache = {}
        for kind, stream, spec in _JOBS:
            if stream not in cache:
                cache[stream] = decode(_A, o, stream)
            arr = cache[stream]
            got[kind] = None if arr is None else counts_of(arr, kind, spec)
        out.append((o, got))
    return out


def parse_jobs(args):
    jobs = []
    if args.rgb:
        jobs.append(("rgb", args.rgb, None))
    if args.seg:
        st, _, ch = args.seg.partition(":")
        jobs.append(("seg", st, int(ch) if ch else 0))
    if args.depth:
        st, _, hl = args.depth.partition(":")
        hi, lo = (int(x) for x in hl.split(",")) if hl else (1, 2)
        jobs.append(("depth", st, (hi, lo)))
    return jobs


def csv_row(values):
    return ",".join(str(int(v)) for v in values)


def main():
    ap = argparse.ArgumentParser(description="Add 256-bin histograms to a .pzpd archive (spec §3.8).",
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("archive", help="The archive's manifest")
    ap.add_argument("--rgb", help="Stream of the RGB images (luminance histogram)")
    ap.add_argument("--seg", help="STREAM[:CHANNEL] of the segmentation labels")
    ap.add_argument("--depth", help="STREAM[:HIGH,LOW] of the 16-bit depth")
    ap.add_argument("--workers", type=int, default=os.cpu_count(), help="Decoding processes")
    args = ap.parse_args()
    jobs = parse_jobs(args)
    if not jobs:
        sys.exit("nothing to do: give --rgb, --seg and / or --depth")

    with pzpdir.open(args.archive) as a:
        n = len(a)
        for _, stream, _ in jobs:
            if stream not in a.streams:
                sys.exit("no stream %r in %s (streams: %s)" % (stream, args.archive, ", ".join(a.streams)))
        keys = [a.key(o) for o in range(n)]
        existing = set(a.tables)

    step = max(1, min(256, n // (args.workers * 8) or 1))
    ranges = [(i, min(n, i + step)) for i in range(0, n, step)]
    rows = {kind: [] for kind, _, _ in jobs}
    total = {kind: np.zeros(256, dtype=np.int64) for kind, _, _ in jobs}
    files = {kind: 0 for kind, _, _ in jobs}
    done = 0
    with Pool(args.workers, initializer=_init, initargs=(args.archive, jobs)) as pool:
        for part in pool.imap(_work, ranges):
            for o, got in part:
                for kind, c in got.items():
                    if c is None:
                        continue
                    rows[kind].append((keys[o], csv_row(normalize(c))))
                    total[kind] += c
                    files[kind] += 1
            done += len(part)
            print("  %d/%d" % (done, n), end="\r", flush=True, file=sys.stderr)
    print(file=sys.stderr)

    for kind, _, _ in jobs:
        name, gname = "hist_" + kind, "hist_%s_global" % kind
        op = "replace" if name in existing else "add"
        um = pzpdir.edit_table(args.archive, op, name, rows[kind], schema=SCHEMA, bulk=True)
        if um:
            sys.exit("%s: %d rows matched no record" % (name, um))
        g = csv_row(list(normalize(total[kind])) + [int(total[kind].sum()), files[kind]])
        pzpdir.edit_table(args.archive, "replace" if gname in existing else "add", gname, schema=GLOBAL_SCHEMA, global_=True, global_csv=g)
        print("%-10s %6d files  %12d pixels  -> tables %s, %s" % (kind, files[kind], int(total[kind].sum()), name, gname))


if __name__ == "__main__":
    main()
