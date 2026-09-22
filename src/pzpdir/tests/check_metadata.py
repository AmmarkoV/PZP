#!/usr/bin/env python3
"""
Phase 1 gate: the metadata pzpdir stored for every blob equals what full decoders report.

Reads the record list used to pack the archive (key, stream, source path, stored name) and
`pzpdir ls --long` of the archive, then fully decodes every source file:
JPEG / PNG with PIL (im.load()), PZP / PZP containers with this repo's pzp bindings.
Compares width, height, channels and bits per channel.

Usage:
    pzpdir ls --long archive.pzpd > ls.tsv
    python3 tests/check_metadata.py list.tsv ls.tsv

Repository : https://github.com/AmmarkoV/PZP
Author     : Ammar Qammaz (AmmarkoV)
"""

import os
import sys

# The repo-root PZP.py + libpzp.so (the pip package under src/pzp may bundle an older libpzp.so)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
from PIL import Image          # noqa: E402
import PZP as pzp              # noqa: E402

PIL_MODES = {  # mode -> (channels, bits per channel)
    "1": (1, 1), "L": (1, 8), "P": (1, 8), "LA": (2, 8), "RGB": (3, 8), "RGBA": (4, 8),
    "CMYK": (4, 8), "YCbCr": (3, 8), "I;16": (1, 16), "I;16B": (1, 16), "I": (1, 16), "F": (1, 32),
}


def unescape(s):
    """Undo the record-list escaping (\\t, \\n, \\\\, leading \\# / \\@)."""
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            out.append({"t": "\t", "n": "\n", "\\": "\\", "#": "#", "@": "@"}[s[i + 1]])
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def decode(path, fourcc):
    """
    Fully decode a file and report its geometry.

    Parameters
    ----------
    path : str
        Source file.
    fourcc : str
        Format stored by pzpdir, selects the decoder.

    Returns
    -------
    tuple
        (width, height, channels, bits)
    """
    if fourcc in ("PZP ", "PZPC"):
        arr = pzp.read(path)
        h, w = arr.shape[0], arr.shape[1]
        c = arr.shape[2] if arr.ndim == 3 else 1
        return w, h, c, arr.dtype.itemsize * 8
    im = Image.open(path)
    im.load()
    c, b = PIL_MODES[im.mode]
    if im.format == "PNG" and im.mode in ("I", "I;16", "I;16B"):
        b = 16
    return im.size[0], im.size[1], c, b


def main():
    lst, ls = sys.argv[1], sys.argv[2]
    src = {}
    for line in open(lst, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line or line[0] in "#@":
            continue
        f = line.split("\t")
        src[(unescape(f[0]), f[1])] = unescape(f[2])
    checked = bad = 0
    for line in open(ls, encoding="utf-8"):
        key, stream, fourcc, meta, size, name = line.rstrip("\n").split("\t")
        key = unescape(key)
        geo = meta.split(" ")[0].rstrip("f")
        w, h, rest = geo.split("x")
        c, b = rest.split("@")
        stored = (int(w), int(h), int(c), int(b))
        path = src[(key, stream)]
        got = decode(path, fourcc)
        checked += 1
        if got != stored:
            bad += 1
            if bad <= 20:
                print("MISMATCH %s %s: stored %s, decoded %s (%s)" % (key, stream, stored, got, path))
        if checked % 2500 == 0:
            print("  %d checked" % checked, flush=True)
    print("%d blobs checked, %d mismatches" % (checked, bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
