# PZP Python API

The Python package wraps `libpzp.so` via ctypes with zero additional
dependencies (numpy is optional but recommended).

---

## Installation

**Editable install (development — builds from repo):**

```bash
# 1. Build the C library
make libpzp.so

# 2. Install the Python package in editable mode
pip install -e .
```

**Build and install a wheel:**

```bash
pip wheel . --no-deps -w dist/
pip install dist/pzp-*.whl
```

The wheel bundles `libpzp.so` — no separate `make` step is needed on the
target machine as long as `libzstd` and `liblz4` are installed.

**System-wide C install + Python package:**

```bash
sudo make install        # installs pzp binary and libpzp.so to /usr/local
pip install -e .         # or pip install dist/pzp-*.whl
```

---

## Single-frame read / write

### Read (decompress)

```python
import pzp

img  = pzp.read("image.pzp")   # numpy array (H, W, C) uint8
                                 # or (H, W) for single-channel
meta = pzp.info("image.pzp")   # dict: width, height, bpp, channels, configuration, …

# Inspect which flags the file was compressed with
img, flags = pzp.read("image.pzp", return_flags=True)
if flags & pzp.USE_PALETTE:
    print("palette mode")
if flags & pzp.USE_RLE:
    print("delta filter")
if flags & pzp.USE_LZ4:
    print("LZ4 codec")
```

Returned array shapes match OpenCV conventions:

| Image type | Shape | dtype |
|---|---|---|
| 8-bit colour | `(H, W, C)` | `uint8` |
| 16-bit colour | `(H, W, C)` | `uint16` |
| 8-bit grayscale | `(H, W)` | `uint8` |
| 16-bit grayscale | `(H, W)` | `uint16` |

### Write (compress)

```python
import pzp
import cv2

# From a numpy array (uint8 or uint16)
img = cv2.imread("photo.ppm")
pzp.write("photo.pzp", img)                              # ZSTD only
pzp.write("photo.pzp", img, use_rle=True)                # + delta pre-filter
pzp.write("photo.pzp", img, use_palette=True)            # + palette indexing
pzp.write("photo.pzp", img, use_rle=True,
                             use_palette=True)            # RLE + palette
pzp.write("photo.pzp", img, use_lz4=True)               # LZ4 codec
pzp.write("photo.pzp", img, use_rle=True, use_lz4=True) # delta pre-filter + LZ4

# 16-bit grayscale
depth = cv2.imread("depth.pnm", cv2.IMREAD_ANYDEPTH | cv2.IMREAD_ANYCOLOR)
pzp.write("depth.pzp", depth)

# From raw bytes (all metadata required)
pzp.write("out.pzp", raw_bytes, width=640, height=360, bpp=8, channels=3)

# Full bitfield control
pzp.write("out.pzp", img, configuration=pzp.USE_COMPRESSION | pzp.USE_RLE)
```

### Without numpy

`read()` returns a plain dict when numpy is not installed:

```python
{
    "data":          bytes,
    "width":         int,
    "height":        int,
    "channels":      int,
    "bpp":           int,
    "configuration": int,
}
```

---

## Animated container API

### Write container

```python
import PZP

frames = [...]             # list of (H, W, 3) uint8 numpy arrays
delays = [100] * len(frames)   # ms per frame

# Basic — ZSTD + delta pre-filter, loop forever
PZP.write_container("anim.pzp", frames,
                    delays=delays, loop_count=0,
                    use_rle=True)

# LZ4 for fastest possible load time
PZP.write_container("anim_fast.pzp", frames,
                    delays=delays, loop_count=0,
                    use_rle=True, use_lz4=True)

# Inter-frame delta (only beneficial when consecutive frames are very similar)
PZP.write_container("anim_delta.pzp", frames,
                    delays=delays, loop_count=0,
                    use_rle=True, use_inter_delta=True)

# Embed audio
with open("music.mp3", "rb") as f:
    audio = f.read()
PZP.write_container("anim_sound.pzp", frames,
                    delays=delays, loop_count=0,
                    audio=audio, audio_format="MPEG")

# Embed metadata
PZP.write_container("anim_meta.pzp", frames,
                    delays=delays, loop_count=3,
                    metadata=b'{"source":"cam0","fps":25}')
```

`write_container` keyword arguments:

| Argument | Default | Description |
|---|---|---|
| `delays` | `None` (all zero) | Per-frame display delay in ms |
| `loop_count` | `0` | Loop count; 0 = loop forever |
| `use_rle` | `False` | Intra-frame delta pre-filter |
| `use_palette` | `False` | Per-channel palette indexing |
| `use_inter_delta` | `False` | Inter-frame delta encoding |
| `use_lz4` | `False` | LZ4 codec instead of ZSTD |
| `configuration` | `USE_COMPRESSION` | Raw bitfield (OR'd with convenience flags) |
| `metadata` | `None` | Bytes or str to embed as opaque blob |
| `audio` | `None` | Raw audio file bytes to embed |
| `audio_format` | `"WAVE"` | `'WAVE'`, `'MPEG'`, `'OGG'`, or `'FLAC'` |

### Read container

```python
import PZP

n          = PZP.frame_count("anim.pzp")
loop       = PZP.get_loop_count("anim.pzp")   # 0 = forever
delays     = PZP.get_delays("anim.pzp")        # list of int (ms per frame)

frame0     = PZP.read_frame("anim.pzp", 0)    # numpy (H, W, C) uint8
audio, fmt = PZP.get_audio("anim.pzp")         # (bytes, format_str) or (None, None)
meta       = PZP.get_metadata("anim.pzp")      # bytes or None
```

---

## Thread lifecycle

The decompression path keeps a per-thread ZSTD context.  For worker threads
and DataLoaders, call `thread_init()` at startup and `thread_cleanup()` at exit.

```python
import PZP
import threading

def worker():
    PZP.thread_init()     # eagerly allocate per-thread ZSTD context
    try:
        for path in paths:
            img = PZP.read(path)
            # ...
    finally:
        PZP.thread_cleanup()   # free context at thread exit (valgrind-clean)

t = threading.Thread(target=worker)

# DataLoader integration (PyTorch / similar)
from torch.utils.data import DataLoader
loader = DataLoader(dataset, num_workers=8,
                    worker_init_fn=lambda _: PZP.thread_init())
```

---

## Configuration constants

```python
pzp.USE_COMPRESSION  # = 1   always active
pzp.USE_RLE          # = 2   left-pixel delta pre-filter
pzp.USE_PALETTE      # = 4   per-channel palette indexing
pzp.USE_INTER_DELTA  # = 8   inter-frame delta (container only)
pzp.USE_LZ4          # = 16  LZ4 codec instead of ZSTD

pzp.AUDIO_WAVE       # = 0x57415645  "WAVE"
pzp.AUDIO_MPEG       # = 0x4D504547  "MPEG"
pzp.AUDIO_OGG        # = 0x4F474758  "OGGX"
pzp.AUDIO_FLAC       # = 0x464C4143  "FLAC"
```

---

## Performance note

`pzp.read()` uses `ctypes.Array.from_address()` to wrap the C-allocated buffer
as a fixed-size ctypes Array, then copies to a numpy array via
`np.ctypeslib.as_array(...).copy()`.  This is a single C-level `memcpy`,
avoiding the O(n) Python-level iteration that occurs with naive POINTER slicing
(`ptr[:n]`) — the original bottleneck that caused 12× slower load times.
