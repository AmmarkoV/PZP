"""
PZP.py — Python wrapper for reading PZP image files via ctypes.

Usage:
    import PZP
    img = PZP.read("image.pzp")          # numpy array, or dict of raw bytes
    meta = PZP.info("image.pzp")         # metadata dict without decoding pixels

Returned numpy array shape:
    8-bit  → (height, width, channels)   dtype uint8
    16-bit → (height, width, channels)   dtype uint16  (big-endian pairs, as stored in PNM)

If numpy is not available, read() returns a dict:
    {
        "data":         bytes,
        "width":        int,
        "height":       int,
        "channels":     int,
        "bpp":          int,   # bits-per-pixel of the original image
    }
"""

import ctypes
import os
import sys

# ---------------------------------------------------------------------------
# Load the shared library
# ---------------------------------------------------------------------------

def _find_lib():
    """Search for libpzp.so next to this file, then on LD_LIBRARY_PATH."""
    candidates = [
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "libpzp.so"),
        "libpzp.so",
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise OSError(
        "libpzp.so not found. Build it with:\n"
        "    make libpzp.so\n"
        "from the PZP source directory."
    )


_lib = ctypes.CDLL(_find_lib())

# pzp_decompress_file signature
_lib.pzp_decompress_file.restype  = ctypes.POINTER(ctypes.c_ubyte)
_lib.pzp_decompress_file.argtypes = [
    ctypes.c_char_p,                   # filename
    ctypes.POINTER(ctypes.c_uint),     # width
    ctypes.POINTER(ctypes.c_uint),     # height
    ctypes.POINTER(ctypes.c_uint),     # bpp_ext
    ctypes.POINTER(ctypes.c_uint),     # channels_ext
    ctypes.POINTER(ctypes.c_uint),     # bpp_int
    ctypes.POINTER(ctypes.c_uint),     # channels_int
    ctypes.POINTER(ctypes.c_uint),     # configuration
]

_lib.pzp_free.restype  = None
_lib.pzp_free.argtypes = [ctypes.c_void_p]

# ---------------------------------------------------------------------------
# Optional numpy support
# ---------------------------------------------------------------------------

try:
    import numpy as np
    _NUMPY = True
except ImportError:
    _NUMPY = False

# ---------------------------------------------------------------------------
# Internal helper
# ---------------------------------------------------------------------------

def _decode(filename: str):
    """
    Call the C decompressor and return (raw_bytes, meta_dict).
    The raw buffer is freed before this function returns.
    """
    filename_b = filename.encode(sys.getfilesystemencoding())

    width       = ctypes.c_uint(0)
    height      = ctypes.c_uint(0)
    bpp_ext     = ctypes.c_uint(0)
    ch_ext      = ctypes.c_uint(0)
    bpp_int     = ctypes.c_uint(0)
    ch_int      = ctypes.c_uint(0)
    config      = ctypes.c_uint(0)

    ptr = _lib.pzp_decompress_file(
        filename_b,
        ctypes.byref(width),
        ctypes.byref(height),
        ctypes.byref(bpp_ext),
        ctypes.byref(ch_ext),
        ctypes.byref(bpp_int),
        ctypes.byref(ch_int),
        ctypes.byref(config),
    )

    if not ptr:
        raise RuntimeError(f"PZP: failed to decompress '{filename}'")

    w  = width.value
    h  = height.value
    be = bpp_ext.value
    ce = ch_ext.value
    bi = bpp_int.value
    ci = ch_int.value

    # Buffer size = width * height * channelsInternal * (bppInternal / 8)
    n_bytes = w * h * ci * (bi // 8)

    # Copy out of C-owned memory before freeing
    raw = bytes(ptr[:n_bytes])
    _lib.pzp_free(ptr)

    meta = {
        "width":          w,
        "height":         h,
        "bpp":            be,
        "channels":       ce,
        "bpp_internal":   bi,
        "ch_internal":    ci,
        "configuration":  config.value,
    }
    return raw, meta


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def read(filename: str):
    """
    Decompress a PZP file and return the pixel data.

    With numpy:    returns ndarray shaped (height, width, channels)
                   dtype uint8  for  8-bit images
                   dtype uint16 for 16-bit images (native byte-order)

    Without numpy: returns dict with keys 'data', 'width', 'height',
                   'channels', 'bpp'.
    """
    raw, meta = _decode(filename)

    w  = meta["width"]
    h  = meta["height"]
    be = meta["bpp"]          # external bits-per-pixel (per channel)
    ce = meta["channels"]     # external channel count

    if _NUMPY:
        if be == 8:
            arr = np.frombuffer(raw, dtype=np.uint8).reshape(h, w, ce)
        elif be == 16:
            # PNM stores 16-bit values big-endian; the PZP internal split
            # preserves that byte order (hi-byte channel first, lo-byte second).
            arr = np.frombuffer(raw, dtype=">u2").reshape(h, w, ce)
            arr = arr.astype(np.uint16)   # convert to native endian
        else:
            raise ValueError(f"PZP: unsupported bit depth {be}")

        # Return a writable copy so the caller can modify it freely
        return arr.copy()

    # Fallback: no numpy
    return {
        "data":     raw,
        "width":    w,
        "height":   h,
        "channels": ce,
        "bpp":      be,
    }


def info(filename: str) -> dict:
    """
    Return metadata for a PZP file without retaining the pixel buffer.
    Keys: width, height, bpp, channels, bpp_internal, ch_internal, configuration.
    """
    _raw, meta = _decode(filename)
    return meta
