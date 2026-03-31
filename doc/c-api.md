# PZP C API

## Header-only API (`pzp.h`)

Include the header and link with `-lzstd -llz4`.  All functions are `static`
inline; no separate compilation step is needed.

```c
#include "pzp.h"
// cc myapp.c -lzstd -llz4 -lm -o myapp
```

---

### Decompress from file

```c
unsigned char *pzp_decompress_combined(
    const char   *input_filename,
    unsigned int *width,         unsigned int *height,
    unsigned int *bpp_ext,       unsigned int *channels_ext,
    unsigned int *bpp_int,       unsigned int *channels_int,
    unsigned int *configuration);

// Returns a malloc'd interleaved pixel buffer — caller must free().
// Returns NULL on error.
// *configuration is filled with the PZPFlags bitfield stored in the file.
```

### Decompress from memory (zero-copy / mmap friendly)

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
// Internal entry point used by pzp.c and pzp_lib.c.
void pzp_compress_combined(
    unsigned char **buffers,      // planar per-channel pixel data
    unsigned int width,           unsigned int height,
    unsigned int bpp_ext,         unsigned int channels_ext,
    unsigned int bpp_int,         unsigned int channels_int,
    unsigned int configuration,   // PZPFlags bitfield — may include USE_LZ4
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

---

### Thread lifecycle (worker threads / data loaders)

The decompression path keeps a per-thread `ZSTD_DCtx` to amortise context
allocation across calls (~6 KB alloc/init/free saved per call).

```c
// Call at thread start to eagerly allocate the per-thread ZSTD context.
// No-op if already initialised.  Lazy init happens automatically on the
// first decompress call if you skip this.
static inline void pzp_thread_init(void);

// Call at thread exit to free the per-thread context.
// Prevents leak-sanitiser noise (valgrind, LSAN).
static inline void pzp_thread_cleanup(void);
```

LZ4 decompression is stateless and requires no context management.

---

## Shared library (`libpzp.so`) — exported C ABI

`pzp_lib.c` wraps the header-only API into a stable ABI for ctypes / FFI
consumers.  Build with `make libpzp.so`.

### Single-frame I/O

```c
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
```

### Container (multi-frame) API

```c
unsigned int pzp_container_frame_count(const char *filename);
unsigned int pzp_container_get_loop_count(const char *filename);

// Fill delays_out[0..frame_count-1] with per-frame delay in ms.
// Returns frame count on success, -1 on error.
int pzp_container_get_delays(const char *filename,
                              unsigned int *delays_out, unsigned int max_frames);

// Decompress one frame from a container (caller frees with pzp_free).
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
// format_out receives the PZP_AUDIO_* four-char tag (e.g. PZP_AUDIO_MPEG).
unsigned char *pzp_container_read_audio(const char *filename,
                                         unsigned int *bytes_out,
                                         unsigned int *format_out);

// Write a multi-frame container.
// configurations[] is per-frame and may freely mix ZSTD and LZ4 frames.
// Returns 1 on success, 0 on failure.
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
```

### Thread lifecycle exports

```c
// Exported wrappers around the static-inline functions in pzp.h.
// Use these when calling from ctypes / other FFI — the static-inline
// versions are not visible in the shared library symbol table.
void pzp_thread_init_export(void);
void pzp_thread_cleanup_export(void);
```

---

## Compile-time options

| Macro | Default | Effect |
|---|---|---|
| `PZP_VERBOSE=1` | 0 | Print per-frame encoding stats: delta coverage, compression ratio, end-of-container summary |
| `PZP_VERIFY_CHECKSUM=1` | 0 | Verify pixel-data and container-header checksums on read (adds ~9% CPU; off by default for data-loader use) |
| `INTEL_OPTIMIZATIONS` | off | Enable SSE2 / AVX2 SIMD prefix-scan on the decode path |

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
