"""
pzp — Python bindings for the PZP lossless image codec.

Usage:
    import pzp

    # Decompress
    img  = pzp.read("image.pzp")              # numpy array, or raw-bytes dict
    meta = pzp.info("image.pzp")              # metadata dict

    # Compress
    pzp.write("out.pzp", img)                                 # zstd only
    pzp.write("out.pzp", img, use_rle=True)                   # + delta pre-filter
    pzp.write("out.pzp", img, use_palette=True)               # + palette indexing
    pzp.write("out.pzp", img, use_rle=True, use_palette=True) # all filters

    # Without numpy — pass raw bytes explicitly
    pzp.write("out.pzp", raw_bytes, width=640, height=360, bpp=8, channels=3)

    # Inspect which flags were used when the file was written
    arr, flags = pzp.read("image.pzp", return_flags=True)
    if flags & pzp.USE_PALETTE:
        print("palette mode")

Returned numpy array shape:
    8-bit  → (height, width, channels)   dtype uint8
    16-bit → (height, width, channels)   dtype uint16  (native byte-order)

If numpy is not available, read() returns a dict:
    {
        "data":          bytes,
        "width":         int,
        "height":        int,
        "channels":      int,
        "bpp":           int,
        "configuration": int,
    }

Configuration flags (combinable with |):
    USE_COMPRESSION = 1   # always enabled; zstd entropy coding
    USE_RLE         = 2   # delta pre-filter (better ratio for smooth images)
    USE_PALETTE     = 4   # per-channel palette indexing (best for images with
                          # few unique values per channel, e.g. segmentation maps)
"""

import ctypes
import os
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Locate libpzp shared library
# ---------------------------------------------------------------------------

def _find_lib() -> str:
    """
    Search order:
    1. Next to this __init__.py  (pip-installed wheel, non-editable)
    2. Repo root one level up    (editable install: src/pzp/ → repo root)
    3. System dynamic linker     (make install to /usr/local/lib, etc.)
    """
    lib_name = {
        "linux":  "libpzp.so",
        "darwin": "libpzp.dylib",
        "win32":  "pzp.dll",
    }.get(sys.platform, "libpzp.so")

    pkg_dir  = Path(__file__).parent.resolve()
    candidates = [
        pkg_dir / lib_name,                  # installed wheel
        pkg_dir.parent.parent / lib_name,    # editable: src/pzp/../../
        Path(lib_name),                      # LD_LIBRARY_PATH / system
    ]
    for p in candidates:
        if p.exists():
            return str(p)

    raise OSError(
        f"{lib_name} not found.\n"
        "Build it first:\n"
        "    make libpzp.so\n"
        "then re-install:\n"
        "    pip install -e .\n"
        "or install system-wide:\n"
        "    sudo make install"
    )


_lib = ctypes.CDLL(_find_lib())

# pzp_decompress_file
_lib.pzp_decompress_file.restype  = ctypes.POINTER(ctypes.c_ubyte)
_lib.pzp_decompress_file.argtypes = [
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_uint),
    ctypes.POINTER(ctypes.c_uint),
    ctypes.POINTER(ctypes.c_uint),
    ctypes.POINTER(ctypes.c_uint),
    ctypes.POINTER(ctypes.c_uint),
    ctypes.POINTER(ctypes.c_uint),
    ctypes.POINTER(ctypes.c_uint),
]

_lib.pzp_free.restype  = None
_lib.pzp_free.argtypes = [ctypes.c_void_p]

# pzp_compress_file
_lib.pzp_compress_file.restype  = ctypes.c_int
_lib.pzp_compress_file.argtypes = [
    ctypes.POINTER(ctypes.c_ubyte),
    ctypes.c_uint,
    ctypes.c_uint,
    ctypes.c_uint,
    ctypes.c_uint,
    ctypes.c_uint,
    ctypes.c_char_p,
]

# ---------------------------------------------------------------------------
# Configuration flag constants (mirror of PZPFlags in pzp.h)
# ---------------------------------------------------------------------------

USE_COMPRESSION = 1
USE_RLE         = 2
USE_PALETTE     = 4

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
    Call the C decompressor and return (raw_buf, meta_dict).

    raw_buf is a numpy uint8 ndarray (if numpy is available) or a bytes object.
    Uses the ctypes Array buffer protocol for a single C-level memcpy — avoids
    the O(n) Python-level iteration that POINTER slicing would cause.
    """
    filename_b = filename.encode(sys.getfilesystemencoding())

    width  = ctypes.c_uint(0)
    height = ctypes.c_uint(0)
    bpp_ext = ctypes.c_uint(0)
    ch_ext  = ctypes.c_uint(0)
    bpp_int = ctypes.c_uint(0)
    ch_int  = ctypes.c_uint(0)
    config  = ctypes.c_uint(0)

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
        raise RuntimeError(f"pzp: failed to decompress '{filename}'")

    w  = width.value
    h  = height.value
    be = bpp_ext.value
    ce = ch_ext.value
    bi = bpp_int.value
    ci = ch_int.value

    n_bytes = w * h * ci * (bi // 8)

    # Cast to fixed-size Array → buffer protocol → single C-level memcpy
    addr  = ctypes.cast(ptr, ctypes.c_void_p).value
    c_arr = (ctypes.c_ubyte * n_bytes).from_address(addr)

    if _NUMPY:
        raw_buf = np.ctypeslib.as_array(c_arr).copy()
    else:
        raw_buf = bytes(c_arr)

    _lib.pzp_free(ptr)

    meta = {
        "width":         w,
        "height":        h,
        "bpp":           be,
        "channels":      ce,
        "bpp_internal":  bi,
        "ch_internal":   ci,
        "configuration": config.value,
    }
    return raw_buf, meta


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def read(filename: str, *, return_flags: bool = False):
    """
    Decompress a PZP file and return the pixel data.

    With numpy:    returns ndarray shaped (height, width, channels)
                   dtype uint8  for  8-bit images
                   dtype uint16 for 16-bit images (native byte-order)
                   Single-channel images are squeezed to (height, width).

    Without numpy: returns dict with keys 'data', 'width', 'height',
                   'channels', 'bpp', 'configuration'.

    Parameters
    ----------
    return_flags : bool
        When True, return (array, flags) instead of just the array.
        flags is an int bitfield (USE_COMPRESSION | USE_RLE | USE_PALETTE …).
    """
    raw_buf, meta = _decode(filename)

    w     = meta["width"]
    h     = meta["height"]
    be    = meta["bpp"]
    ce    = meta["channels"]
    flags = meta["configuration"]

    if _NUMPY:
        if be == 8:
            arr = raw_buf.reshape(h, w, ce)
        elif be == 16:
            arr = raw_buf.view(dtype=">u2").reshape(h, w, ce).astype(np.uint16)
        else:
            raise ValueError(f"pzp: unsupported bit depth {be}")

        if ce == 1:
            arr = arr[:, :, 0]

        return (arr, flags) if return_flags else arr

    # Fallback: no numpy
    result = {
        "data":          raw_buf,
        "width":         w,
        "height":        h,
        "channels":      ce,
        "bpp":           be,
        "configuration": flags,
    }
    return (result, flags) if return_flags else result


def info(filename: str) -> dict:
    """
    Return metadata for a PZP file without retaining the pixel buffer.
    Keys: width, height, bpp, channels, bpp_internal, ch_internal, configuration.
    """
    _raw, meta = _decode(filename)
    return meta


def write(filename: str, data, *,
          width: int = 0, height: int = 0,
          bpp: int = 0, channels: int = 0,
          use_rle: bool = False,
          use_palette: bool = False,
          configuration: int = USE_COMPRESSION) -> None:
    """
    Compress pixel data and write a .pzp file.

    Parameters
    ----------
    filename : str
        Output .pzp file path.
    data : numpy ndarray  *or*  bytes / bytearray
        - ndarray (H, W)    → 1-channel uint8 or uint16
        - ndarray (H, W, C) → C-channel uint8 or uint16
        - bytes/bytearray   → raw interleaved bytes; supply width/height/bpp/channels.
    width, height, bpp, channels : int
        Required when data is raw bytes; inferred from ndarray shape otherwise.
    use_rle : bool
        Enable the delta pre-filter (USE_RLE).
    use_palette : bool
        Enable per-channel palette indexing (USE_PALETTE).
        Best for images with few unique values per channel (segmentation maps).
    configuration : int
        Raw bitfield. USE_COMPRESSION is always set. Prefer the bool helpers.

    Raises
    ------
    ValueError   on bad dtype, shape, or missing dimensions.
    RuntimeError if the C encoder fails.
    """
    cfg = configuration | USE_COMPRESSION
    if use_rle:
        cfg |= USE_RLE
    if use_palette:
        cfg |= USE_PALETTE

    if _NUMPY and isinstance(data, np.ndarray):
        arr = data
        if arr.ndim == 2:
            arr = arr[:, :, np.newaxis]
        if arr.ndim != 3:
            raise ValueError(f"pzp.write: expected 2-D or 3-D array, got {data.shape}")

        h, w, c = arr.shape

        if arr.dtype == np.uint8:
            pixel_bpp = 8
            raw = arr.tobytes()
        elif arr.dtype == np.uint16:
            pixel_bpp = 16
            raw = arr.astype(">u2").tobytes()
        else:
            raise ValueError(f"pzp.write: unsupported dtype {arr.dtype}. Use uint8 or uint16.")
    else:
        if not (width and height and bpp and channels):
            raise ValueError(
                "pzp.write: width, height, bpp, and channels are required "
                "when data is not a numpy array.")
        if bpp not in (8, 16):
            raise ValueError(f"pzp.write: bpp must be 8 or 16, got {bpp}")
        w, h, pixel_bpp, c = width, height, bpp, channels
        raw = bytes(data)

    expected = w * h * c * (pixel_bpp // 8)
    if len(raw) != expected:
        raise ValueError(
            f"pzp.write: pixel buffer is {len(raw)} bytes, "
            f"expected {expected} ({w}×{h}×{c}ch×{pixel_bpp//8}B)")

    buf   = (ctypes.c_ubyte * len(raw)).from_buffer_copy(raw)
    fname = filename.encode(sys.getfilesystemencoding())

    rc = _lib.pzp_compress_file(buf, w, h, pixel_bpp, c, cfg, fname)
    if rc == 0:
        raise RuntimeError(f"pzp.write: compression failed for '{filename}'")
