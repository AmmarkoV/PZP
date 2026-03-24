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

# Container API signatures
_lib.pzp_container_frame_count.restype  = ctypes.c_uint
_lib.pzp_container_frame_count.argtypes = [ctypes.c_char_p]

_lib.pzp_container_get_loop_count.restype  = ctypes.c_uint
_lib.pzp_container_get_loop_count.argtypes = [ctypes.c_char_p]

_lib.pzp_container_get_delays.restype  = ctypes.c_int
_lib.pzp_container_get_delays.argtypes = [
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_uint),  # delays_out
    ctypes.c_uint,                  # max_frames
]

_lib.pzp_container_get_frame.restype  = ctypes.POINTER(ctypes.c_ubyte)
_lib.pzp_container_get_frame.argtypes = [
    ctypes.c_char_p,               # filename
    ctypes.c_uint,                 # frame_index
    ctypes.POINTER(ctypes.c_uint), # width
    ctypes.POINTER(ctypes.c_uint), # height
    ctypes.POINTER(ctypes.c_uint), # bpp_ext
    ctypes.POINTER(ctypes.c_uint), # channels_ext
    ctypes.POINTER(ctypes.c_uint), # bpp_int
    ctypes.POINTER(ctypes.c_uint), # channels_int
    ctypes.POINTER(ctypes.c_uint), # configuration
]

_lib.pzp_container_read_metadata.restype  = ctypes.POINTER(ctypes.c_ubyte)
_lib.pzp_container_read_metadata.argtypes = [
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_uint), # bytes_out
]

_lib.pzp_container_read_audio.restype  = ctypes.POINTER(ctypes.c_ubyte)
_lib.pzp_container_read_audio.argtypes = [
    ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_uint), # bytes_out
    ctypes.POINTER(ctypes.c_uint), # format_out
]

_lib.pzp_write_frames.restype  = ctypes.c_int
_lib.pzp_write_frames.argtypes = [
    ctypes.c_char_p,                                # output_filename
    ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte)), # pixel_arrays
    ctypes.c_uint,                                  # frame_count
    ctypes.POINTER(ctypes.c_uint),                  # widths
    ctypes.POINTER(ctypes.c_uint),                  # heights
    ctypes.POINTER(ctypes.c_uint),                  # bpps
    ctypes.POINTER(ctypes.c_uint),                  # channels_arr
    ctypes.POINTER(ctypes.c_uint),                  # configurations
    ctypes.POINTER(ctypes.c_uint),                  # delay_ms_arr
    ctypes.c_uint,                                  # loop_count
    ctypes.POINTER(ctypes.c_ubyte),                 # metadata (nullable)
    ctypes.c_uint,                                  # metadata_bytes
    ctypes.POINTER(ctypes.c_ubyte),                 # audio (nullable)
    ctypes.c_uint,                                  # audio_bytes
    ctypes.c_uint,                                  # audio_format
]

# ---------------------------------------------------------------------------
# Configuration flag constants (mirror of PZPFlags in pzp.h)
# ---------------------------------------------------------------------------

USE_COMPRESSION = 1
USE_RLE         = 2
USE_PALETTE     = 4

# Audio format four-char tags (mirror of PZP_AUDIO_* in pzp.h)
AUDIO_WAVE = 0x57415645  # "WAVE"
AUDIO_MPEG = 0x4D504547  # "MPEG"
AUDIO_OGG  = 0x4F474758  # "OGGX"
AUDIO_FLAC = 0x464C4143  # "FLAC"

_AUDIO_FORMAT_MAP = {
    "wav":  AUDIO_WAVE, "wave": AUDIO_WAVE,
    "mp3":  AUDIO_MPEG, "mpeg": AUDIO_MPEG,
    "ogg":  AUDIO_OGG,
    "flac": AUDIO_FLAC,
}

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

    raw_buf is:
      - a numpy uint8 ndarray of shape (n_bytes,) when numpy is available
      - a bytes object otherwise
    The C buffer is freed before this function returns.

    Performance note: ctypes POINTER slicing (ptr[:n]) creates a Python list of
    integer objects — O(n) Python-level work for large images.  Instead we cast
    to a ctypes Array and use the buffer protocol, so the copy is a single
    C-level memcpy regardless of image size.
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

    # Fast copy: reinterpret the C pointer as a fixed-size Array so Python can
    # use the buffer protocol (single C-level memcpy) instead of iterating.
    addr  = ctypes.cast(ptr, ctypes.c_void_p).value
    c_arr = (ctypes.c_ubyte * n_bytes).from_address(addr)

    if _NUMPY:
        # np.ctypeslib.as_array gives a zero-copy VIEW of C memory.
        # .copy() triggers one C-level memcpy into a Python-owned buffer.
        raw_buf = np.ctypeslib.as_array(c_arr).copy()
    else:
        # bytes() on a ctypes Array uses the buffer protocol → one C memcpy.
        raw_buf = bytes(c_arr)

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

    Without numpy: returns dict with keys 'data', 'width', 'height',
                   'channels', 'bpp'.

    Parameters
    ----------
    return_flags : bool
        When True, return a (array, flags) tuple instead of just the array.
        flags is an int bitfield (USE_COMPRESSION | USE_RLE | USE_PALETTE …).
    """
    raw_buf, meta = _decode(filename)

    w     = meta["width"]
    h     = meta["height"]
    be    = meta["bpp"]          # external bits-per-pixel (per channel)
    ce    = meta["channels"]     # external channel count
    flags = meta["configuration"]

    if _NUMPY:
        # raw_buf is already an owned numpy uint8 array (single memcpy in _decode).
        # Reshape / reinterpret without any additional copy where possible.
        if be == 8:
            # reshape is O(1) — returns a view of the already-owned buffer.
            arr = raw_buf.reshape(h, w, ce)
        elif be == 16:
            # PNM stores 16-bit values big-endian; the PZP internal split
            # preserves that byte order (hi-byte channel first, lo-byte second).
            # .view() is O(1); .astype() makes one copy to convert byte-order.
            arr = raw_buf.view(dtype=">u2").reshape(h, w, ce).astype(np.uint16)
        else:
            raise ValueError(f"PZP: unsupported bit depth {be}")

        # Squeeze the channel axis for single-channel images to match cv2's
        # convention (grayscale → (H, W), not (H, W, 1)).
        if ce == 1:
            arr = arr[:, :, 0]

        # For 8-bit the array already owns its memory (from the copy in _decode).
        # For 16-bit astype() already produced an owned array.
        # Callers receive a writable, owned array in both cases.
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


# ---------------------------------------------------------------------------
# Container API
# ---------------------------------------------------------------------------

def frame_count(filename: str) -> int:
    """Return the number of frames in a PZP container (1 for single-frame files)."""
    return _lib.pzp_container_frame_count(
        filename.encode(sys.getfilesystemencoding()))


def get_loop_count(filename: str) -> int:
    """Return the animation loop count (0 = loop forever)."""
    return _lib.pzp_container_get_loop_count(
        filename.encode(sys.getfilesystemencoding()))


def get_delays(filename: str) -> list:
    """Return a list of per-frame delay values in milliseconds."""
    n = frame_count(filename)
    if n == 0:
        return []
    arr = (ctypes.c_uint * n)()
    _lib.pzp_container_get_delays(
        filename.encode(sys.getfilesystemencoding()), arr, n)
    return list(arr)


def read_frame(filename: str, index: int = 0, *, return_flags: bool = False):
    """
    Decompress frame `index` from a multi-frame PZP container.
    Returns the same types as read().
    """
    filename_b = filename.encode(sys.getfilesystemencoding())
    width   = ctypes.c_uint(0)
    height  = ctypes.c_uint(0)
    bpp_ext = ctypes.c_uint(0)
    ch_ext  = ctypes.c_uint(0)
    bpp_int = ctypes.c_uint(0)
    ch_int  = ctypes.c_uint(0)
    config  = ctypes.c_uint(0)

    ptr = _lib.pzp_container_get_frame(
        filename_b, index,
        ctypes.byref(width),   ctypes.byref(height),
        ctypes.byref(bpp_ext), ctypes.byref(ch_ext),
        ctypes.byref(bpp_int), ctypes.byref(ch_int),
        ctypes.byref(config),
    )
    if not ptr:
        raise RuntimeError(f"PZP: failed to read frame {index} from '{filename}'")

    w, h, be, ce, bi, ci = (width.value, height.value,
                             bpp_ext.value, ch_ext.value,
                             bpp_int.value, ch_int.value)
    n_bytes = w * h * ci * (bi // 8)
    addr    = ctypes.cast(ptr, ctypes.c_void_p).value
    c_arr   = (ctypes.c_ubyte * n_bytes).from_address(addr)

    if _NUMPY:
        raw_buf = np.ctypeslib.as_array(c_arr).copy()
    else:
        raw_buf = bytes(c_arr)

    _lib.pzp_free(ptr)

    flags = config.value
    if _NUMPY:
        if be == 8:
            arr = raw_buf.reshape(h, w, ce)
        elif be == 16:
            arr = raw_buf.view(dtype=">u2").reshape(h, w, ce).astype(np.uint16)
        else:
            raise ValueError(f"PZP: unsupported bit depth {be}")
        if ce == 1:
            arr = arr[:, :, 0]
        return (arr, flags) if return_flags else arr
    else:
        result = {"data": raw_buf, "width": w, "height": h,
                  "channels": ce, "bpp": be, "configuration": flags}
        return (result, flags) if return_flags else result


def get_metadata(filename: str):
    """
    Return the metadata blob from a PZP container as bytes, or None if absent.
    """
    bytes_out = ctypes.c_uint(0)
    ptr = _lib.pzp_container_read_metadata(
        filename.encode(sys.getfilesystemencoding()),
        ctypes.byref(bytes_out),
    )
    if not ptr:
        return None
    n     = bytes_out.value
    addr  = ctypes.cast(ptr, ctypes.c_void_p).value
    c_arr = (ctypes.c_ubyte * n).from_address(addr)
    data  = bytes(c_arr)
    _lib.pzp_free(ptr)
    return data


def get_audio(filename: str):
    """
    Return (audio_bytes, format_str) from a PZP container, or (None, None) if absent.
    format_str is one of 'WAVE', 'MPEG', 'OGGX', 'FLAC'.
    """
    bytes_out  = ctypes.c_uint(0)
    format_out = ctypes.c_uint(0)
    ptr = _lib.pzp_container_read_audio(
        filename.encode(sys.getfilesystemencoding()),
        ctypes.byref(bytes_out),
        ctypes.byref(format_out),
    )
    if not ptr:
        return None, None
    n     = bytes_out.value
    addr  = ctypes.cast(ptr, ctypes.c_void_p).value
    c_arr = (ctypes.c_ubyte * n).from_address(addr)
    data  = bytes(c_arr)
    _lib.pzp_free(ptr)
    fmt_int = format_out.value
    fmt_str = (chr((fmt_int >> 24) & 0xFF) + chr((fmt_int >> 16) & 0xFF) +
               chr((fmt_int >>  8) & 0xFF) + chr(fmt_int & 0xFF))
    return data, fmt_str


def write_container(filename: str, frames, *,
                    delays=None,
                    loop_count: int = 0,
                    use_rle: bool = False,
                    use_palette: bool = False,
                    configuration: int = USE_COMPRESSION,
                    metadata=None,
                    audio=None,
                    audio_format: str = "WAVE") -> None:
    """
    Compress multiple frames and write a PZP container file.

    Parameters
    ----------
    filename : str
        Output .pzp path.
    frames : list of numpy ndarray  or  list of (bytes, width, height, bpp, channels)
        Pixel data for each frame.  ndarray shapes follow the same convention
        as write() — (H, W) for single-channel, (H, W, C) for multi-channel.
    delays : list of int, optional
        Per-frame display delay in milliseconds.  None = all zeros.
    loop_count : int
        Animation loop count; 0 = loop forever.
    use_rle, use_palette, configuration
        Same meaning as write(); applied uniformly to all frames.
    metadata : bytes or str, optional
        Opaque metadata blob to embed.  str is UTF-8 encoded.
    audio : bytes, optional
        Raw audio file bytes to embed.
    audio_format : str
        Audio format hint: 'WAVE', 'MP3'/'MPEG', 'OGG', 'FLAC'. Default 'WAVE'.
    """
    cfg = configuration | USE_COMPRESSION
    if use_rle:     cfg |= USE_RLE
    if use_palette: cfg |= USE_PALETTE

    n = len(frames)
    if n == 0:
        raise ValueError("PZP.write_container: frames list is empty")

    pixel_bufs = []
    widths  = (ctypes.c_uint * n)()
    heights = (ctypes.c_uint * n)()
    bpps    = (ctypes.c_uint * n)()
    chans   = (ctypes.c_uint * n)()
    cfgs    = (ctypes.c_uint * n)()
    delays_arr = (ctypes.c_uint * n)(*([0] * n if delays is None else delays))

    for i, frame in enumerate(frames):
        if _NUMPY and isinstance(frame, np.ndarray):
            arr = frame
            if arr.ndim == 2:
                arr = arr[:, :, np.newaxis]
            if arr.ndim != 3:
                raise ValueError(
                    f"PZP.write_container: frame {i} has unsupported shape {frame.shape}")
            h, w, c = arr.shape
            if arr.dtype == np.uint8:
                bpp = 8
                raw = arr.tobytes()
            elif arr.dtype == np.uint16:
                bpp = 16
                raw = arr.astype(">u2").tobytes()
            else:
                raise ValueError(
                    f"PZP.write_container: frame {i} dtype {arr.dtype} unsupported")
        else:
            # Tuple form: (bytes, width, height, bpp, channels)
            raw, w, h, bpp, c = frame

        pixel_bufs.append((ctypes.c_ubyte * len(raw)).from_buffer_copy(raw))
        widths[i] = w
        heights[i] = h
        bpps[i] = bpp
        chans[i] = c
        cfgs[i] = cfg

    ptr_arr = (ctypes.POINTER(ctypes.c_ubyte) * n)(
        *[ctypes.cast(pb, ctypes.POINTER(ctypes.c_ubyte)) for pb in pixel_bufs]
    )

    # Metadata
    meta_buf = ctypes.cast(None, ctypes.POINTER(ctypes.c_ubyte))
    meta_len = 0
    if metadata is not None:
        if isinstance(metadata, str):
            metadata = metadata.encode("utf-8")
        _mb = (ctypes.c_ubyte * len(metadata)).from_buffer_copy(metadata)
        meta_buf = ctypes.cast(_mb, ctypes.POINTER(ctypes.c_ubyte))
        meta_len = len(metadata)

    # Audio
    audio_buf = ctypes.cast(None, ctypes.POINTER(ctypes.c_ubyte))
    audio_len = 0
    audio_fmt = 0
    if audio is not None:
        _ab = (ctypes.c_ubyte * len(audio)).from_buffer_copy(audio)
        audio_buf = ctypes.cast(_ab, ctypes.POINTER(ctypes.c_ubyte))
        audio_len = len(audio)
        audio_fmt = _AUDIO_FORMAT_MAP.get(audio_format.lower().lstrip("."),
                                          AUDIO_WAVE)

    fname = filename.encode(sys.getfilesystemencoding())
    rc = _lib.pzp_write_frames(
        fname, ptr_arr, n,
        widths, heights, bpps, chans, cfgs, delays_arr,
        loop_count,
        meta_buf, meta_len,
        audio_buf, audio_len, audio_fmt,
    )
    if rc == 0:
        raise RuntimeError(f"PZP.write_container: failed to write '{filename}'")
