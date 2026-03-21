"""
PZP.py — Python wrapper for reading and writing PZP image files via ctypes.

Usage:
    import PZP

    # Decompress
    img  = PZP.read("image.pzp")         # numpy array, or dict of raw bytes
    meta = PZP.info("image.pzp")         # metadata dict without decoding pixels

    # Compress
    PZP.write("out.pzp", img)                            # default: zstd only
    PZP.write("out.pzp", img, use_rle=True)              # + delta pre-filter
    PZP.write("out.pzp", img, use_palette=True)          # + palette indexing
    PZP.write("out.pzp", img, use_rle=True, use_palette=True)  # all filters

    # Inspect which flags were used when the file was written
    arr, flags = PZP.read("image.pzp", return_flags=True)
    if flags & PZP.USE_PALETTE:
        print("palette mode")

    # Without numpy — pass raw bytes explicitly
    PZP.write("out.pzp", raw_bytes, width=640, height=360, bpp=8, channels=3)

Returned numpy array shape:
    8-bit  → (height, width, channels)   dtype uint8
    16-bit → (height, width, channels)   dtype uint16  (native byte-order)

If numpy is not available, read() returns a dict:
    {
        "data":         bytes,
        "width":        int,
        "height":       int,
        "channels":     int,
        "bpp":          int,   # bits-per-pixel of the original image
    }

Configuration flags (pass to write() as configuration=):
    USE_COMPRESSION = 1   # always enabled; zstd entropy coding
    USE_RLE         = 2   # delta pre-filter (improves ratio for smooth images)
    USE_PALETTE     = 4   # per-channel palette indexing (best for images with few
                          # unique values per channel, e.g. segmentation maps)
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

# pzp_compress_file signature
_lib.pzp_compress_file.restype  = ctypes.c_int
_lib.pzp_compress_file.argtypes = [
    ctypes.POINTER(ctypes.c_ubyte),    # pixels
    ctypes.c_uint,                     # width
    ctypes.c_uint,                     # height
    ctypes.c_uint,                     # bpp  (8 or 16)
    ctypes.c_uint,                     # channels
    ctypes.c_uint,                     # configuration
    ctypes.c_char_p,                   # output_filename
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

def read(filename: str, *, return_flags: bool = False):
    """
    Decompress a PZP file and return the pixel data.

    With numpy:    returns ndarray shaped (height, width, channels)
                   dtype uint8  for  8-bit images
                   dtype uint16 for 16-bit images (native byte-order)

    Without numpy: returns dict with keys 'data', 'width', 'height',
                   'channels', 'bpp'.

    Parameters
    ----------
    return_flags : bool
        When True, return a (array, flags) tuple instead of just the array.
        flags is an int bitfield (USE_COMPRESSION | USE_RLE | USE_PALETTE …).
    """
    raw, meta = _decode(filename)

    w    = meta["width"]
    h    = meta["height"]
    be   = meta["bpp"]          # external bits-per-pixel (per channel)
    ce   = meta["channels"]     # external channel count
    flags = meta["configuration"]

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

        # Squeeze the channel axis for single-channel images to match cv2's
        # convention (grayscale → (H, W), not (H, W, 1)).
        if ce == 1:
            arr = arr[:, :, 0]

        # Return a writable copy so the caller can modify it freely
        result = arr.copy()
        return (result, flags) if return_flags else result

    # Fallback: no numpy
    result = {
        "data":          raw,
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
        Pixel data.
        - ndarray (H, W)        → treated as 1-channel uint8 or uint16
        - ndarray (H, W, C)     → C-channel uint8 or uint16
        - bytes / bytearray     → raw interleaved bytes; width/height/bpp/channels
                                  must all be supplied explicitly.
    width, height, bpp, channels : int
        Required when data is raw bytes; ignored when data is an ndarray
        (dimensions are inferred from the array shape and dtype).
    use_rle : bool
        Enable the delta pre-filter (improves ratio for smooth / gradient images).
        Adds USE_RLE to the configuration bitfield.
    use_palette : bool
        Enable per-channel palette indexing.  Best for images with few unique
        values per channel (e.g. segmentation maps, label images).
        Adds USE_PALETTE to the configuration bitfield.
    configuration : int
        Full configuration bitfield.  USE_COMPRESSION (1) is always or'd in.
        Prefer the convenience booleans (use_rle, use_palette) for common cases.

    Raises
    ------
    ValueError  if the array dtype is unsupported or dimensions are missing.
    RuntimeError if the C encoder returns an error.
    """
    # Always ensure USE_COMPRESSION is set
    cfg = configuration | USE_COMPRESSION
    if use_rle:
        cfg |= USE_RLE
    if use_palette:
        cfg |= USE_PALETTE

    if _NUMPY and isinstance(data, np.ndarray):
        arr = data

        # Normalise shape to (H, W, C)
        if arr.ndim == 2:
            arr = arr[:, :, np.newaxis]
        if arr.ndim != 3:
            raise ValueError(f"PZP.write: expected 2-D or 3-D array, got shape {data.shape}")

        h, w, c = arr.shape

        if arr.dtype == np.uint8:
            pixel_bpp = 8
            raw = arr.tobytes()
        elif arr.dtype == np.uint16:
            # PZP/PNM byte order is big-endian; convert if the array is native-endian
            pixel_bpp = 16
            raw = arr.astype(">u2").tobytes()
        else:
            raise ValueError(
                f"PZP.write: unsupported dtype {arr.dtype}. Use uint8 or uint16.")

    else:
        # Raw bytes path — caller must supply all metadata
        if not (width and height and bpp and channels):
            raise ValueError(
                "PZP.write: width, height, bpp, and channels are required "
                "when data is not a numpy array.")
        if bpp not in (8, 16):
            raise ValueError(f"PZP.write: bpp must be 8 or 16, got {bpp}")

        w, h, pixel_bpp, c = width, height, bpp, channels
        raw = bytes(data)

    expected = w * h * c * (pixel_bpp // 8)
    if len(raw) != expected:
        raise ValueError(
            f"PZP.write: pixel buffer is {len(raw)} bytes, "
            f"expected {expected} ({w}×{h}×{c}ch×{pixel_bpp//8}B)")

    buf   = (ctypes.c_ubyte * len(raw)).from_buffer_copy(raw)
    fname = filename.encode(sys.getfilesystemencoding())

    rc = _lib.pzp_compress_file(buf, w, h, pixel_bpp, c, cfg, fname)
    if rc == 0:
        raise RuntimeError(f"PZP.write: compression failed for '{filename}'")
