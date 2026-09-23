"""
pzp.histograms - the 256-bin histograms of PZPD archives (spec §3.8), shared by the tools that build them
(src/pzpdir/scripts/pzpdir_histograms.py for an existing archive, RGBToPoseDetect2D's convertToPZPD.py while
converting), so both compute exactly the same rows.

Tables:
    hist_<kind>          record, bulk   h:u16[256]                        one row per file
    hist_<kind>_global   global         h:u16[256] pixels:u64 files:u64   pixel-weighted over the archive
Kinds:
    rgb    luminance, PIL's "L" rule: (19595 R + 38470 G + 7471 B + 32768) >> 16; 16-bit input by its high byte
    seg    segmentation label = one channel of an 8-bit image
    depth  16-bit depth in 256 fixed bins (depth >> 8); a 16-bit single-channel image, or two 8-bit
           channels (high byte, low byte) of a combined label + depth image
Values are fractions of the pixels, 65535 meaning 1.0, each bin rounded to nearest (a row sums to about 65535).

Repository : https://github.com/AmmarkoV/PZP
Author     : Ammar Qammaz (AmmarkoV)
"""

import io

import numpy as np

SCHEMA = "h:u16[256]"                                  #: schema of hist_<kind>
GLOBAL_SCHEMA = "h:u16[256] pixels:u64 files:u64"      #: schema of hist_<kind>_global
KINDS = ("rgb", "seg", "depth")                        #: kinds, in table order


def table_names(kind):
    """(record table, global table) names of a kind."""
    return "hist_" + kind, "hist_%s_global" % kind


def decode_pil(data):
    """
    An image (bytes or a path) through PIL, as the histogram tools read non-PZP images.

    Returns
    -------
    numpy.ndarray
        uint16 for 16-bit images ("I;16" or, with older Pillow, 32-bit "I"); uint8 L / RGB / RGBA otherwise
        (palette / CMYK images become RGB).
    """
    from PIL import Image
    img = Image.open(io.BytesIO(data) if isinstance(data, (bytes, bytearray, memoryview)) else data)
    if img.mode.startswith("I"):
        return np.clip(np.array(img), 0, 65535).astype(np.uint16)
    if img.mode not in ("L", "RGB", "RGBA"):
        img = img.convert("RGB")
    return np.array(img)


def decode_file(path):
    """An image file as a numpy array: PZP (single frames and containers) natively, anything else through PIL."""
    with open(path, "rb") as f:
        head = f.read(4)
    if path.lower().endswith(".pzp") or head == b"0PZP":
        import pzp
        return pzp.read(path)
    return decode_pil(path)


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
    """
    Pixel counts of the 256 bins (int64).

    Parameters
    ----------
    arr : numpy.ndarray
        The decoded image.
    kind : str
        "rgb", "seg" or "depth".
    spec :
        seg: the label channel (ignored for single-channel images); depth: (high, low) channels (ignored for
        16-bit images); rgb: unused.
    """
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


def csv_row(values):
    """Integers as one CSV row (no newline)."""
    return ",".join(str(int(v)) for v in values)


def global_csv(total, files):
    """The CSV row of hist_<kind>_global from the summed pixel counts and the number of files."""
    return csv_row(list(normalize(total)) + [int(total.sum()), files]) + "\n"
