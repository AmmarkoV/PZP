# PZP — Portable Zipped PNM

An experimental, minimal, header-only image compression library written in C.

---

## Overview

PZP stores images as zstd-compressed PNM data with a compact binary header.
It is designed for applications that need fast, lossless image storage with
better compression than raw PNM/PPM, and a simpler implementation than PNG.

Goals:
1. Header-only C implementation (`pzp.h`) — drop `#include "pzp.h"` and go
2. Supports 8-bit and 16-bit Monochrome / RGB images
3. Decode speed competitive with PNG
4. Compression ratio better than PNG on many image types
5. Python bindings via ctypes (`PZP.py`) for use in data-loading pipelines

Similar projects: [QOI](https://github.com/phoboslab/qoi), [ZPNG](https://github.com/catid/Zpng)

---

## File format

```
[ 4 bytes  ] uncompressed payload size (uint32, little-endian)
[ N bytes  ] zstd-compressed payload:
    [ 40 bytes ] header  (10 × uint32)
                   magic · bpp_ext · channels_ext · width · height
                   bpp_int · channels_int · checksum · config · reserved
    [ W×H×C bytes ] interleaved pixel data (8-bit internal channels)
```

16-bit images are stored as two 8-bit internal channels per original channel
(high byte plane / low byte plane), which improves zstd's compression ratio.

An optional delta pre-filter (`USE_RLE`) can be enabled at compression time
to further improve ratios on smooth / gradient images.

---

## Dependencies

```
sudo apt install libzstd-dev
```

---

## Building

```bash
make              # release (pzp), SIMD/AVX2 (spzp), debug (dpzp), shared lib (libpzp.so)
make test         # compress + decompress the bundled samples and verify output
make debug        # valgrind memory-check run
```

| Target | Binary | Flags |
|---|---|---|
| release | `pzp` | `-O3 -march=native` |
| SIMD | `spzp` | `-O3 -mavx2 -DINTEL_OPTIMIZATIONS` |
| debug | `dpzp` | `-O0 -g3` |
| shared lib | `libpzp.so` | release flags + `-shared -fPIC` |

---

## Command-line usage

```bash
# Compress
./pzp compress input.ppm  output.pzp
./pzp compress input.pnm  output.pzp   # 16-bit supported

# Decompress
./pzp decompress output.pzp reconstructed.ppm
```

---

## C API (`pzp.h`)

Include the header and link against zstd (`-lzstd`).

### Decompress from file

```c
unsigned char *pzp_decompress_combined(
    const char   *input_filename,
    unsigned int *width,  unsigned int *height,
    unsigned int *bpp_ext,     unsigned int *channels_ext,
    unsigned int *bpp_int,     unsigned int *channels_int,
    unsigned int *configuration);

// Returns a malloc'd pixel buffer — caller must free().
// NULL on error.
```

### Decompress from memory

```c
unsigned char *pzp_decompress_combined_from_memory(
    const void   *file_data,  size_t file_size,
    unsigned int *width,  unsigned int *height,
    unsigned int *bpp_ext,     unsigned int *channels_ext,
    unsigned int *bpp_int,     unsigned int *channels_int,
    unsigned int *configuration);
```

### Compress

Compression is driven by `pzp_compress_combined()` — see `pzp.h` for the full
internal API.  The recommended entry point for library consumers is the
exported C function in `pzp_lib.c` (see below).

### Configuration flags

```c
typedef enum {
    USE_COMPRESSION = 1 << 0,  // zstd entropy coding (always set)
    USE_RLE         = 1 << 1,  // delta pre-filter
} PZPFlags;
```

---

## Shared library (`libpzp.so`) and exported C API

`pzp_lib.c` exposes a stable ABI suitable for ctypes / FFI:

```c
// Decompress a .pzp file → malloc'd pixel buffer (caller frees with pzp_free)
unsigned char *pzp_decompress_file(
    const char *filename,
    unsigned int *width,  unsigned int *height,
    unsigned int *bpp_ext,  unsigned int *channels_ext,
    unsigned int *bpp_int,  unsigned int *channels_int,
    unsigned int *configuration);

// Compress raw pixel data → .pzp file.  Returns 1 on success, 0 on failure.
int pzp_compress_file(
    const unsigned char *pixels,
    unsigned int width,  unsigned int height,
    unsigned int bpp,        // 8 or 16
    unsigned int channels,
    unsigned int configuration,
    const char *output_filename);

void pzp_free(void *ptr);
```

Build the shared library with:

```bash
make libpzp.so
```

---

## Python bindings (`PZP.py`)

Requires `libpzp.so` in the same directory and numpy (optional but recommended).

### Read (decompress)

```python
import PZP

img  = PZP.read("image.pzp")   # numpy array (H, W, C) uint8
                                 # or (H, W) uint16 for 16-bit grayscale
meta = PZP.info("image.pzp")   # dict: width, height, bpp, channels, …
```

Returned array shapes match cv2 conventions:
- 8-bit colour  → `(H, W, C)` `uint8`
- 16-bit colour → `(H, W, C)` `uint16`
- grayscale     → `(H, W)` (channel axis squeezed, any bit depth)

### Write (compress)

```python
import PZP, cv2

# From a numpy array
img = cv2.imread("photo.ppm")
PZP.write("photo.pzp", img)                # default: USE_COMPRESSION only
PZP.write("photo.pzp", img, use_rle=True)  # add delta pre-filter

# 16-bit grayscale
depth = cv2.imread("depth.pnm", cv2.IMREAD_ANYDEPTH | cv2.IMREAD_ANYCOLOR)
PZP.write("depth.pzp", depth)

# From raw bytes (all metadata required)
PZP.write("out.pzp", raw_bytes, width=640, height=360, bpp=8, channels=3)
```

### Configuration constants

```python
PZP.USE_COMPRESSION  # = 1  (always active)
PZP.USE_RLE          # = 2  (delta pre-filter)
```

### Without numpy

`read()` falls back to a plain dict:

```python
{"data": bytes, "width": int, "height": int, "channels": int, "bpp": int}
```

---

## Benchmark script

`scripts/benchmark.py` times compression, decompression, and pixel-level
correctness across all build targets and formats.

```bash
source venv/bin/activate

# Sample mode — bundled samples/, multiple timing runs
python3 scripts/benchmark.py
python3 scripts/benchmark.py --no-debug --runs 3

# Directory mode — any folder of images
python3 scripts/benchmark.py \
    --source-dir test/segment_val2017 \
    --pzp-dir    test/segment_val2017PZP \
    --compare 30

# Compress from scratch, limit to 500 files
python3 scripts/benchmark.py \
    --source-dir test/segment_val2017 \
    --max-files 500 --no-debug
```

**Flags**

| Flag | Default | Description |
|---|---|---|
| `--source-dir DIR` | — | Directory of source images (directory mode) |
| `--pzp-dir DIR` | — | Pre-existing `.pzp` files to benchmark decompression of |
| `--max-files N` | all | Process at most N files |
| `--compare N` | 20 | Random files to pixel-verify in directory mode |
| `--runs N` | 5 | Timing repetitions (sample mode) |
| `--no-debug` | off | Skip `dpzp` (slow) |
| `--no-build` | off | Skip `make all` |

PNG source files are automatically converted to PPM before compression since
the PZP binary reads PNM/PPM only.  Both `raw/PZP` (vs uncompressed PPM) and
`PNG/PZP` (vs original source format) ratios are reported.

---

## SIMD / optimisation notes

The decode path (`pzp_extractAndReconstruct`) has three implementations:

| Implementation | Selected when |
|---|---|
| `_Naive` | default (no flags) |
| `_SSE2` | `-DINTEL_OPTIMIZATIONS` (Kogge-Stone prefix scan) |
| `_AVX2` | defined but currently disabled (known bugs) |

The non-RLE decode path always uses a single `memcpy` regardless of channel
count.  For the RLE (delta) path, the SSE2 implementation uses a Kogge-Stone
parallel prefix scan with cross-block carry propagation for 1-channel and
2-channel images; 3-channel and wider fall back to scalar.

For large datasets the dominant cost is usually subprocess startup when calling
the binary.  Use `PZP.py` (ctypes, in-process) for lowest latency.
