# PZP — Portable Zipped PNM

An experimental, minimal, header-only image compression library written in C,
with Python bindings and a pip-installable package.

---

## Overview

PZP stores images as zstd- or LZ4-compressed PNM data with a compact binary
header.  It is designed for applications that need fast, lossless image storage
with better compression than raw PNM/PPM, and a simpler implementation than PNG.

Goals:
1. Header-only C implementation (`pzp.h`) — drop `#include "pzp.h"` and go
2. Supports 8-bit and 16-bit Monochrome / RGB images
3. Decode speed faster than PNG for real-world datasets
4. Compression ratio better than PNG on many image types
5. Optional per-channel palette indexing for label / segmentation maps
6. Optional LZ4 codec for even faster decompression (at lower compression ratio)
7. Multi-frame animated container with per-frame delays, loop count, embedded audio and metadata
8. Python bindings via ctypes — installable with `pip`

Similar projects: [QOI](https://github.com/phoboslab/qoi), [ZPNG](https://github.com/catid/Zpng)

---

## Performance

Benchmarked against `cv2.imread` (libpng) on 500 COCO val2017 panoptic
segmentation images (3-channel uint8 RGB label maps, varying resolutions).
All times are in-process (no subprocess overhead) using `libpzp.so` + ctypes.

```
Pass   PNG total   PZP total    PNG/img    PZP/img         Δ
------------------------------------------------------------
   1     1152.8ms      513.0ms     2.306ms     1.026ms  +55.5%
   2     1147.2ms      510.4ms     2.294ms     1.021ms  +55.5%
   3     1141.2ms      500.6ms     2.282ms     1.001ms  +56.1%
------------------------------------------------------------
best     1141.2ms      500.6ms     2.282ms     1.001ms  +56.1%

  Best PNG total : 1141.2 ms  (2.282 ms/img)
  Best PZP total :  500.6 ms  (1.001 ms/img)
  Winner         : PZP  (2.28× faster, 56% improvement)

  PNG size : 4.1 MB
  PZP size : 2.0 MB  (ratio=2.08×, saving=52%)
```

PZP is **2.28× faster** to load than PNG and **52% smaller** on disk for this
dataset. The speedup comes from zstd's faster decompressor compared to zlib
(used by PNG) combined with PZP's simple flat binary layout. Segmentation maps
benefit most because their limited palette of label values compresses
extremely well with zstd.

---

## File format

### Single-frame `.pzp`

```
[ 4 bytes  ] size prefix (uint32, little-endian):
               bit 31 = codec flag  (0 = ZSTD, 1 = LZ4)
               bits 0–30 = uncompressed payload size
[ N bytes  ] compressed payload (ZSTD or LZ4):
    [ 40 bytes ] header  (10 × uint32)
                   magic · bpp_ext · channels_ext · width · height
                   bpp_int · channels_int · checksum · config · palette_bytes
    [ P bytes  ] palette data (optional, when USE_PALETTE is set)
    [ W×H×C bytes ] interleaved pixel / index data
```

16-bit images are stored as two 8-bit internal channels per original channel
(high-byte plane / low-byte plane), which improves compression ratio.

### Container `.pzp` (animated / multi-frame)

```
[ 48 bytes ] PZPContainerHeader  (12 × uint32)
               magic · version · frame_count · loop_count
               metadata_offset · metadata_bytes
               audio_offset · audio_bytes · audio_format
               container_flags · header_checksum · reserved
[ frame_count × 16 bytes ] frame index (PZPFrameEntry, 4 × uint32 each)
               frame_offset · compressed_size · delay_ms · reserved
[ frame data  ] per-frame blocks (each = size-prefixed single-frame payload)
[ metadata    ] optional opaque blob
[ audio       ] optional raw audio bytes (WAV / MP3 / OGG / FLAC)
```

### Compression flags

| Flag | Value | Effect |
|---|---|---|
| `USE_COMPRESSION` | 1 | ZSTD entropy coding (always set) |
| `USE_RLE` | 2 | Left-pixel delta pre-filter — improves ratio on smooth / gradient images |
| `USE_PALETTE` | 4 | Per-channel palette indexing — best for images with few unique values per channel (e.g. segmentation maps) |
| `USE_INTER_DELTA` | 8 | Inter-frame delta — each frame stores `frame[N] − frame[N−1]`; useful only when consecutive frames are very similar (slow pan, static background) |
| `USE_LZ4` | 16 | Use LZ4 instead of ZSTD — faster decompression, larger output; codec is stored per-frame in bit 31 of the size prefix |

Flags can be combined with `|`.  Recommended combinations:

| Use case | Flags |
|---|---|
| Natural images / photos | `USE_COMPRESSION \| USE_RLE` |
| Segmentation / label maps | `USE_COMPRESSION \| USE_RLE \| USE_PALETTE` |
| Latency-critical loader | `USE_COMPRESSION \| USE_RLE \| USE_LZ4` |
| Slow-motion animation | `USE_COMPRESSION \| USE_RLE \| USE_INTER_DELTA` |

---

## Dependencies

```bash
sudo apt install libzstd-dev liblz4-dev   # Ubuntu / Debian
sudo dnf install libzstd-devel lz4-devel  # Fedora / RHEL
brew install zstd lz4                     # macOS
```

Both `libzstd` and `liblz4` are required.  All build targets and the shared
library link against both.

---

## Building

```bash
make              # builds all targets: pzp, spzp, dpzp, libpzp.so
make libpzp.so    # shared library only (needed for Python bindings)
make test         # compress + decompress all bundled samples, verify output
make debug        # valgrind memory-check run
make clean        # remove all build artefacts
```

### Build targets

| Target | Binary | Flags |
|---|---|---|
| release | `pzp` | `-O3 -march=native` |
| SIMD/AVX2 | `spzp` | `-O3 -mavx2 -DINTEL_OPTIMIZATIONS` |
| debug | `dpzp` | `-O0 -g3` |
| shared lib | `libpzp.so` | release flags + `-shared -fPIC` |

### System install / uninstall

```bash
sudo make install              # → /usr/local/bin, /usr/local/lib, /usr/local/include
sudo make install PREFIX=/usr  # custom prefix
sudo make uninstall            # remove all installed files
```

`DESTDIR` is supported for packaging (`.deb`, `.rpm`, etc.):

```bash
make install DESTDIR=/tmp/pkg PREFIX=/usr
```

### Desktop integration (thumbnails)

`make install-desktop` registers `.pzp` as a recognised image type on
freedesktop.org-compatible desktops (LXQt, XFCE, GNOME, KDE) and installs a
thumbnailer so file managers display previews automatically.

```bash
# 1. Install the C library and binary first
sudo make install

# 2. Install MIME type + thumbnailer
sudo make install-desktop

# 3. Install the Python thumbnailer dependency
pip install Pillow   # or: sudo apt install python3-pil

# 4. Restart the thumbnail daemon to pick up the new handler
pkill tumbler; tumbler &   # or log out / log in
```

What `install-desktop` puts on the system:

| File | Destination | Purpose |
|---|---|---|
| `data/pzp.xml` | `PREFIX/share/mime/packages/pzp.xml` | Registers `image/x-pzp` MIME type; detected by the zstd magic bytes at offset 4 |
| `data/pzp.thumbnailer` | `PREFIX/share/thumbnailers/pzp.thumbnailer` | Tells `tumbler` to call `pzp-thumbnailer` for `.pzp` files |
| `scripts/pzp-thumbnailer` | `PREFIX/bin/pzp-thumbnailer` | Decompresses `.pzp` → resizes → saves PNG thumbnail |

To install per-user (no `sudo` required):

```bash
make install-desktop PREFIX=~/.local
update-mime-database ~/.local/share/mime
pkill tumbler; tumbler &
```

To remove:

```bash
sudo make uninstall-desktop
```

---

## Command-line usage

The `pzp` binary reads and writes PNM/PPM files (P5 grayscale, P6 colour).

```bash
# Compress (zstd + delta filter)
./pzp compress          input.ppm  output.pzp
./pzp compress          input.pnm  output.pzp   # 16-bit depth supported

# Compress with LZ4 (faster decompress, larger file)
./pzp compress          input.ppm  output.pzp  --lz4

# Compress with palette mode (best for segmentation / label maps)
./pzp compress-palette  input.ppm  output.pzp
./pzp compress-palette  input.ppm  output.pzp  --lz4

# Pack (zstd only, no delta filter)
./pzp pack              input.ppm  output.pzp
./pzp pack              input.ppm  output.pzp  --lz4

# Decompress (any mode — codec and flags are stored in the file)
./pzp decompress        output.pzp  reconstructed.ppm

# Inspect a container (frames, loop count, audio, metadata)
./pzp info              file.pzp

# Extract a single frame from a container
./pzp extract-frame     file.pzp  frame0.ppm  0

# Pack multiple PNM frames into an animated container
./pzp pack-frames  out.pzp  <loop_count> <delay_ms>  frame*.ppm
./pzp pack-frames  out.pzp  0 100  --delta  frame*.ppm   # inter-frame delta
./pzp pack-frames  out.pzp  0 100  --lz4    frame*.ppm   # LZ4 codec
./pzp pack-frames  out.pzp  0 100  --delta --lz4  frame*.ppm  # both

# Attach audio to an existing container
./pzp attach-audio  input.pzp  sound.wav  output.pzp
./pzp attach-audio  input.pzp  music.mp3  output.pzp

# Attach metadata string to an existing container
./pzp attach-meta   input.pzp  '{"fps":24}' output.pzp
```

Codec flags available on `compress`, `compress-palette`, `pack`, `pack-frames`:

| Flag | Effect |
|---|---|
| *(none)* | ZSTD compression (default) |
| `--lz4` | LZ4 compression — faster decompress, larger output |
| `--delta` | Inter-frame delta (`pack-frames` only) |

PNG and JPEG source files must be converted to PNM/PPM first (the binary has
no libpng / libjpeg dependency by design):

```bash
convert photo.png photo.ppm       # ImageMagick
ffmpeg -i photo.jpg photo.ppm     # FFmpeg
```

---

## C API (`pzp.h`)

Include the header and link with `-lzstd -llz4`.  All functions are `static`
inline; no separate compilation step is needed.

### Decompress from file

```c
unsigned char *pzp_decompress_combined(
    const char   *input_filename,
    unsigned int *width,         unsigned int *height,
    unsigned int *bpp_ext,       unsigned int *channels_ext,
    unsigned int *bpp_int,       unsigned int *channels_int,
    unsigned int *configuration);

// Returns a malloc'd pixel buffer — caller must free().
// Returns NULL on error.
```

### Decompress from memory (zero-copy file loading)

```c
unsigned char *pzp_decompress_combined_from_memory(
    const void   *file_data,     size_t file_size,
    unsigned int *width,         unsigned int *height,
    unsigned int *bpp_ext,       unsigned int *channels_ext,
    unsigned int *bpp_int,       unsigned int *channels_int,
    unsigned int *configuration);
```

### Compress

```c
// Internal entry point — use the exported API below for new code.
void pzp_compress_combined(
    unsigned char **buffers,      // planar per-channel pixel data
    unsigned int width,           unsigned int height,
    unsigned int bpp_ext,         unsigned int channels_ext,
    unsigned int bpp_int,         unsigned int channels_int,
    unsigned int configuration,   // PZPFlags bitfield
    const char  *output_filename);
```

### Configuration flags

```c
typedef enum {
    USE_COMPRESSION = 1 << 0,  // ZSTD entropy coding (always set)
    USE_RLE         = 1 << 1,  // left-pixel delta pre-filter
    USE_PALETTE     = 1 << 2,  // per-channel palette indexing
    USE_INTER_DELTA = 1 << 3,  // inter-frame delta (container only)
    USE_LZ4         = 1 << 4,  // use LZ4 instead of ZSTD
} PZPFlags;
```

### Thread lifecycle (worker threads / data loaders)

The decompression path keeps a per-thread `ZSTD_DCtx` to amortise context
allocation across calls.  For clean shutdown or eager warm-up:

```c
// Call at thread start to eagerly allocate the ZSTD context.
// No-op if already initialised.
static inline void pzp_thread_init(void);

// Call at thread exit to free the per-thread context.
// Prevents leak-sanitiser noise (valgrind, LSAN).
static inline void pzp_thread_cleanup(void);
```

LZ4 decompression is stateless and requires no context management.

---

## Shared library (`libpzp.so`) and exported C API

`pzp_lib.c` exposes a stable ABI for ctypes / FFI consumers:

```c
/* ── Single-frame I/O ──────────────────────────────────────────────────────── */

// Decompress a .pzp file → malloc'd pixel buffer (caller frees with pzp_free).
unsigned char *pzp_decompress_file(
    const char   *filename,
    unsigned int *width,         unsigned int *height,
    unsigned int *bpp_ext,       unsigned int *channels_ext,
    unsigned int *bpp_int,       unsigned int *channels_int,
    unsigned int *configuration);

// Compress raw interleaved pixel data → .pzp file.
// configuration may include USE_LZ4 to select the LZ4 codec.
// Returns 1 on success, 0 on failure.
int pzp_compress_file(
    const unsigned char *pixels,
    unsigned int width,          unsigned int height,
    unsigned int bpp,            // 8 or 16
    unsigned int channels,
    unsigned int configuration,  // PZPFlags bitfield
    const char  *output_filename);

void pzp_free(void *ptr);

/* ── Container (multi-frame) API ───────────────────────────────────────────── */

unsigned int   pzp_container_frame_count(const char *filename);
unsigned int   pzp_container_get_loop_count(const char *filename);

// Fill delays_out[0..frame_count-1] with per-frame delay in ms.
// Returns frame count on success, -1 on error.
int pzp_container_get_delays(const char *filename,
                              unsigned int *delays_out, unsigned int max_frames);

// Decompress one frame from a container.
unsigned char *pzp_container_get_frame(
    const char   *filename,      unsigned int  frame_index,
    unsigned int *width,         unsigned int *height,
    unsigned int *bpp_ext,       unsigned int *channels_ext,
    unsigned int *bpp_int,       unsigned int *channels_int,
    unsigned int *configuration);

// Read the embedded metadata blob (caller frees with pzp_free).
unsigned char *pzp_container_read_metadata(const char *filename,
                                            unsigned int *bytes_out);

// Read the embedded audio blob (caller frees with pzp_free).
// format_out receives the PZP_AUDIO_* four-char tag.
unsigned char *pzp_container_read_audio(const char *filename,
                                         unsigned int *bytes_out,
                                         unsigned int *format_out);

// Write a multi-frame container.  configurations[] is per-frame and may
// mix ZSTD and LZ4 frames.  Returns 1 on success, 0 on failure.
int pzp_write_frames(
    const char          *output_filename,
    const unsigned char **pixel_arrays,  unsigned int frame_count,
    unsigned int *widths,                unsigned int *heights,
    unsigned int *bpps,                  unsigned int *channels_arr,
    unsigned int *configurations,        unsigned int *delay_ms_arr,
    unsigned int  loop_count,
    const unsigned char *metadata,       unsigned int metadata_bytes,
    const unsigned char *audio,          unsigned int audio_bytes,
    unsigned int  audio_format);

/* ── Thread lifecycle ──────────────────────────────────────────────────────── */

// Exported wrappers around the static-inline pzp_thread_init / cleanup.
void pzp_thread_init_export(void);
void pzp_thread_cleanup_export(void);
```

```bash
make libpzp.so
```

---

## Python package (`pzp`)

The Python package wraps `libpzp.so` via ctypes with zero additional
dependencies (numpy is optional but recommended).

### Installation

**Editable install (development):**

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
target machine as long as it has `libzstd` installed.

**System-wide C install + Python package:**

```bash
sudo make install        # installs pzp binary and libpzp.so to /usr/local
pip install -e .         # or pip install dist/pzp-*.whl
```

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
                             use_palette=True)            # all filters
pzp.write("photo.pzp", img, use_lz4=True)               # LZ4 codec
pzp.write("photo.pzp", img, use_rle=True, use_lz4=True) # delta + LZ4

# 16-bit grayscale
depth = cv2.imread("depth.pnm", cv2.IMREAD_ANYDEPTH | cv2.IMREAD_ANYCOLOR)
pzp.write("depth.pzp", depth)

# From raw bytes (all metadata required)
pzp.write("out.pzp", raw_bytes, width=640, height=360, bpp=8, channels=3)

# Full bitfield control
pzp.write("out.pzp", img, configuration=pzp.USE_COMPRESSION | pzp.USE_RLE)
```

### Write animated container

```python
import PZP

# Encode GIF frames + embedded audio
frames = [...]   # list of (H, W, 3) uint8 numpy arrays
delays = [100] * len(frames)   # ms per frame

PZP.write_container("anim.pzp", frames,
                    delays=delays, loop_count=0,  # 0 = loop forever
                    use_rle=True)

# With LZ4 for faster loading
PZP.write_container("anim_fast.pzp", frames,
                    delays=delays, loop_count=0,
                    use_rle=True, use_lz4=True)

# With inter-frame delta (for slow-motion / nearly-identical frames)
PZP.write_container("anim_delta.pzp", frames,
                    delays=delays, loop_count=0,
                    use_rle=True, use_inter_delta=True)

# Embed audio
with open("music.mp3", "rb") as f:
    audio = f.read()
PZP.write_container("anim_sound.pzp", frames,
                    delays=delays, loop_count=0,
                    audio=audio, audio_format="MPEG")
```

### Read animated container

```python
import PZP

n          = PZP.frame_count("anim.pzp")
loop       = PZP.get_loop_count("anim.pzp")   # 0 = forever
delays     = PZP.get_delays("anim.pzp")        # list of ms per frame

frame0     = PZP.read_frame("anim.pzp", 0)    # numpy (H, W, C) uint8
audio, fmt = PZP.get_audio("anim.pzp")         # bytes, format str (e.g. "MPEG")
meta       = PZP.get_metadata("anim.pzp")      # bytes or None
```

### Thread lifecycle

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

# DataLoader integration (PyTorch / similar)
loader = DataLoader(dataset, num_workers=8,
                    worker_init_fn=lambda _: PZP.thread_init())
```

### Configuration constants

```python
pzp.USE_COMPRESSION  # = 1   always active
pzp.USE_RLE          # = 2   left-pixel delta pre-filter
pzp.USE_PALETTE      # = 4   per-channel palette indexing
pzp.USE_INTER_DELTA  # = 8   inter-frame delta (container only)
pzp.USE_LZ4          # = 16  LZ4 codec instead of ZSTD
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

## Scripts

### `scripts/encode_directory.py` — batch encode a directory to PZP

Encodes every PNG (or other format) in a source directory to a matching PZP
file in a target directory, preserving sub-directory structure, in parallel.

```bash
# Standard compression
python3 scripts/encode_directory.py test/segment_val2017 test/segment_val2017PZP

# RLE + palette (best ratio for segmentation maps)
python3 scripts/encode_directory.py test/segment_val2017 test/segment_val2017PZP \
    --rle --palette

# LZ4 for fastest possible load time
python3 scripts/encode_directory.py frames/ frames_pzp/ --lz4

# LZ4 + RLE, source extension, parallelism
python3 scripts/encode_directory.py samples/ output/ --ext ppm --workers 8 --lz4 --rle
```

| Flag | Default | Description |
|---|---|---|
| `--rle` | off | Enable delta pre-filter |
| `--palette` | off | Enable palette indexing |
| `--lz4` | off | Use LZ4 codec instead of ZSTD |
| `--workers N` | CPU count | Parallel encoding processes |
| `--ext EXT` | `png` | Source file extension |

---

### `scripts/encode_animation_with_sound.py` — GIF + audio → PZP container

Reads a GIF (animated or static) and an audio file, and packs them into a
single self-contained `.pzp` container.  Loop count and per-frame delays are
read automatically from the GIF and can be overridden.

```bash
# Basic usage — output defaults to <gif_stem>.pzp
python3 scripts/encode_animation_with_sound.py animation.gif music.mp3

# Specify output path
python3 scripts/encode_animation_with_sound.py animation.gif sound.wav out.pzp

# With compression options
python3 scripts/encode_animation_with_sound.py animation.gif music.mp3 \
    --rle --palette

# With inter-frame delta (only beneficial for near-identical consecutive frames)
python3 scripts/encode_animation_with_sound.py animation.gif music.mp3 --delta

# Override loop count and frame rate
python3 scripts/encode_animation_with_sound.py animation.gif music.mp3 \
    --loop 3 --fps 12
```

| Flag | Default | Description |
|---|---|---|
| `--rle` | off | Intra-frame delta pre-filter |
| `--palette` | off | Per-channel palette encoding |
| `--delta` | off | Inter-frame delta — only helps when frames are very similar |
| `--loop N` | from GIF | Loop count (0 = forever) |
| `--fps FPS` | from GIF | Override per-frame delay |

Dependencies: `pip install Pillow numpy`

---

### `scripts/pzp-player` — animated PZP playback with audio

Interactive player for animated `.pzp` containers.  Renders frames at their
stored delays and plays back embedded audio synchronously.

```bash
pzp-player animation.pzp
pzp-player animation.pzp --scale 2.0   # 2× zoom
pzp-player animation.pzp --fps 24      # override playback speed
```

| Key | Action |
|---|---|
| `Space` | Pause / resume |
| `←` / `→` | Step one frame (while paused) |
| `R` | Restart from frame 0 |
| `Q` / `Esc` | Quit |

| Flag | Default | Description |
|---|---|---|
| `--scale FACTOR` | 1.0 | Window scale factor |
| `--fps FPS` | from file | Override playback speed |

Dependencies: `pip install numpy pygame`

---

### `scripts/compare_load_speed.py` — compare load speed: PNG vs PZP

Loads every matched pair from two directories (one PNG, one PZP) using
`cv2.imread` and `pzp.read` respectively, reporting total time, per-image
time, and winner across multiple passes.

```bash
python3 scripts/compare_load_speed.py \
    test/segment_val2017 test/segment_val2017PZP \
    --max 500 --passes 3
```

| Flag | Default | Description |
|---|---|---|
| `--max N` | all | Load at most N pairs |
| `--warmup N` | 1 | Untimed warm-up passes |
| `--passes N` | 3 | Timed measurement passes |
| `--no-verify` | off | Skip pixel-identity check |

---

### `scripts/benchmark.py` — general benchmark

Times all build targets (`pzp`, `spzp`, `dpzp`) and compares them against
PNG and JPEG for the bundled samples or any source directory.

```bash
source venv/bin/activate

# Sample mode — bundled samples/, multiple timing runs
python3 scripts/benchmark.py
python3 scripts/benchmark.py --no-debug --runs 3

# Directory mode — pre-encoded PZP directory
python3 scripts/benchmark.py \
    --source-dir test/segment_val2017 \
    --pzp-dir    test/segment_val2017PZP \
    --compare 30

# Directory mode — compress from scratch, limit files
python3 scripts/benchmark.py \
    --source-dir test/segment_val2017 \
    --max-files 500 --no-debug
```

| Flag | Default | Description |
|---|---|---|
| `--source-dir DIR` | — | Source image directory |
| `--pzp-dir DIR` | — | Pre-existing `.pzp` directory |
| `--max-files N` | all | Process at most N files |
| `--compare N` | 20 | Files to pixel-verify |
| `--runs N` | 5 | Timing repetitions (sample mode) |
| `--no-debug` | off | Skip `dpzp` (slow valgrind target) |
| `--no-build` | off | Skip `make all` |

PNG and JPEG sources are automatically pre-converted to PPM for the PZP binary
(which reads PNM/PPM only), keeping the comparison fair.

---

## Compile-time options

| Macro | Default | Effect |
|---|---|---|
| `PZP_VERBOSE=1` | 0 | Print per-frame encoding stats: delta coverage, compression ratio, end-of-container summary |
| `PZP_VERIFY_CHECKSUM=1` | 0 | Verify pixel-data and container-header checksums on read (adds ~9% CPU; off by default for data-loader use) |
| `INTEL_OPTIMIZATIONS` | off | Enable SSE2 / AVX2 SIMD prefix-scan on the decode path (`spzp` build target) |

```bash
# Verbose build — shows per-frame stats during encode
gcc pzp.c -DPZP_VERBOSE=1 -lzstd -llz4 -lm -o pzp_verbose

# Checksum-verifying build — use for integrity checks, not production loading
gcc pzp.c -DPZP_VERIFY_CHECKSUM=1 -lzstd -llz4 -lm -o pzp_checked
```

---

## SIMD / optimisation notes

The decode path (`pzp_extractAndReconstruct`) selects an implementation at
compile time:

| Implementation | Compiled when | Notes |
|---|---|---|
| `_Naive` | default | Portable scalar |
| `_SSE2` | `-DINTEL_OPTIMIZATIONS` | Kogge-Stone prefix scan (16 bytes/iter) |
| `_AVX2` | `-DINTEL_OPTIMIZATIONS` | Kogge-Stone prefix scan (32 bytes/iter) |

The SSE2 / AVX2 implementations use a two-step carry propagation to work
around the lane-isolation constraint of `_mm256_slli_si256` / `_mm_slli_si128`:
an intra-lane Kogge-Stone scan followed by an explicit cross-lane carry
broadcast.  1-channel and 2-channel images use SIMD prefix sums; 3-channel
images use a scalar loop (stride-3 serial dependency makes SIMD not worthwhile
at typical image sizes).

The non-RLE decode path uses a single `memcpy` regardless of channel count.

### Python-side performance note

The Python `pzp.read()` implementation uses `ctypes.Array.from_address()` to
wrap the C-allocated buffer as a fixed-size ctypes Array, then copies to a
numpy array via `np.ctypeslib.as_array(...).copy()`.  This performs a single
C-level `memcpy` — avoiding the O(n) Python-level iteration that would occur
with naive POINTER slicing (`ptr[:n]`), which was the original bottleneck
causing 12× slower load times before this fix.
