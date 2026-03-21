# PZP — Portable Zipped PNM

An experimental, minimal, header-only image compression library written in C,
with Python bindings and a pip-installable package.

---

## Overview

PZP stores images as zstd-compressed PNM data with a compact binary header.
It is designed for applications that need fast, lossless image storage with
better compression than raw PNM/PPM, and a simpler implementation than PNG.

Goals:
1. Header-only C implementation (`pzp.h`) — drop `#include "pzp.h"` and go
2. Supports 8-bit and 16-bit Monochrome / RGB images
3. Decode speed faster than PNG for real-world datasets
4. Compression ratio better than PNG on many image types
5. Optional per-channel palette indexing for label / segmentation maps
6. Python bindings via ctypes — installable with `pip`

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

```
[ 4 bytes  ] uncompressed payload size (uint32, little-endian)
[ N bytes  ] zstd-compressed payload:
    [ 40 bytes ] header  (10 × uint32)
                   magic · bpp_ext · channels_ext · width · height
                   bpp_int · channels_int · checksum · config · palette_bytes
    [ P bytes  ] palette data (optional, when USE_PALETTE is set)
    [ W×H×C bytes ] interleaved pixel / index data
```

16-bit images are stored as two 8-bit internal channels per original channel
(high-byte plane / low-byte plane), which improves zstd's compression ratio.

### Compression modes

| Flag | Value | Effect |
|---|---|---|
| `USE_COMPRESSION` | 1 | zstd entropy coding (always set) |
| `USE_RLE` | 2 | Left-pixel delta pre-filter — improves ratio on smooth / gradient images |
| `USE_PALETTE` | 4 | Per-channel palette indexing — best for images with few unique values per channel (e.g. segmentation maps) |

Flags can be combined with `|`.  The recommended combination for smooth images
is `USE_COMPRESSION | USE_RLE`; for label maps `USE_COMPRESSION | USE_RLE | USE_PALETTE`.

---

## Dependencies

```bash
sudo apt install libzstd-dev    # Ubuntu / Debian
sudo dnf install libzstd-devel  # Fedora / RHEL
brew install zstd               # macOS
```

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

---

## Command-line usage

The `pzp` binary reads and writes PNM/PPM files (P5 grayscale, P6 colour).

```bash
# Compress (zstd + delta filter)
./pzp compress      input.ppm  output.pzp
./pzp compress      input.pnm  output.pzp   # 16-bit depth supported

# Compress with palette mode (best for segmentation / label maps)
./pzp compress-palette  input.ppm  output.pzp

# Pack (zstd only, no delta filter)
./pzp pack          input.ppm  output.pzp

# Decompress (any mode — flags are stored in the file)
./pzp decompress    output.pzp  reconstructed.ppm
```

PNG and JPEG source files must be converted to PNM/PPM first (the binary has
no libpng / libjpeg dependency by design):

```bash
convert photo.png photo.ppm       # ImageMagick
ffmpeg -i photo.jpg photo.ppm     # FFmpeg
```

---

## C API (`pzp.h`)

Include the header and link with `-lzstd`.  All functions are `static` inline;
no separate compilation step is needed.

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
    USE_COMPRESSION = 1 << 0,  // zstd entropy coding (always set)
    USE_RLE         = 1 << 1,  // delta pre-filter
    USE_PALETTE     = 1 << 2,  // per-channel palette indexing
} PZPFlags;
```

---

## Shared library (`libpzp.so`) and exported C API

`pzp_lib.c` exposes a stable ABI for ctypes / FFI consumers:

```c
// Decompress a .pzp file → malloc'd pixel buffer (caller frees with pzp_free).
unsigned char *pzp_decompress_file(
    const char   *filename,
    unsigned int *width,         unsigned int *height,
    unsigned int *bpp_ext,       unsigned int *channels_ext,
    unsigned int *bpp_int,       unsigned int *channels_int,
    unsigned int *configuration);

// Compress raw interleaved pixel data → .pzp file.
// Returns 1 on success, 0 on failure.
int pzp_compress_file(
    const unsigned char *pixels,
    unsigned int width,          unsigned int height,
    unsigned int bpp,            // 8 or 16
    unsigned int channels,
    unsigned int configuration,  // PZPFlags bitfield
    const char  *output_filename);

void pzp_free(void *ptr);
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
pzp.write("photo.pzp", img)                              # zstd only
pzp.write("photo.pzp", img, use_rle=True)                # + delta pre-filter
pzp.write("photo.pzp", img, use_palette=True)            # + palette indexing
pzp.write("photo.pzp", img, use_rle=True,
                             use_palette=True)            # all filters

# 16-bit grayscale
depth = cv2.imread("depth.pnm", cv2.IMREAD_ANYDEPTH | cv2.IMREAD_ANYCOLOR)
pzp.write("depth.pzp", depth)

# From raw bytes (all metadata required)
pzp.write("out.pzp", raw_bytes, width=640, height=360, bpp=8, channels=3)

# Full bitfield control
pzp.write("out.pzp", img, configuration=pzp.USE_COMPRESSION | pzp.USE_RLE)
```

### Configuration constants

```python
pzp.USE_COMPRESSION  # = 1  always active
pzp.USE_RLE          # = 2  delta pre-filter
pzp.USE_PALETTE      # = 4  per-channel palette indexing
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

## Batch encoding and benchmarking scripts

### Encode a directory of images to PZP

`scripts/encode_directory.py` encodes every PNG (or other format) in a source
directory to a matching PZP file in a target directory, in parallel.

```bash
# Standard compression
python3 scripts/encode_directory.py test/segment_val2017 test/segment_val2017PZP

# With RLE + palette (best ratio for segmentation maps)
python3 scripts/encode_directory.py test/segment_val2017 test/segment_val2017PZP \
    --rle --palette

# Control source format and parallelism
python3 scripts/encode_directory.py samples/ output/ --ext ppm --workers 8
```

| Flag | Default | Description |
|---|---|---|
| `--rle` | off | Enable delta pre-filter |
| `--palette` | off | Enable palette indexing |
| `--workers N` | CPU count | Parallel encoding processes |
| `--ext EXT` | `png` | Source file extension |

### Compare load speed: PNG vs PZP

`scripts/compare_load_speed.py` loads every matched pair from two directories
(one PNG, one PZP) using `cv2.imread` and `pzp.read` respectively, reporting
total time, per-image time, and winner across multiple passes.

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

### General benchmark (samples + directory mode)

`scripts/benchmark.py` times all build targets (`pzp`, `spzp`, `dpzp`) and
compares them against PNG and JPEG for the bundled samples or any source
directory.

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
