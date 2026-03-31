# PZP Performance

## Benchmark: PNG vs PZP (segmentation maps)

Benchmarked against `cv2.imread` (libpng) on 500 COCO val2017 panoptic
segmentation images (3-channel uint8 RGB label maps, varying resolutions).
All times are in-process (no subprocess overhead) using `libpzp.so` + ctypes,
with `USE_COMPRESSION | USE_RLE | USE_PALETTE` flags.

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
dataset.  The speedup comes from zstd's faster decompressor compared to zlib
(used by PNG) combined with PZP's simple flat binary layout.  Segmentation
maps benefit most because their limited palette of label values compresses
extremely well with zstd.

---

## Codec trade-offs

| Codec | Ratio | Decompress speed | Best for |
|---|---|---|---|
| ZSTD (default) | best | fast | general use, cold-storage |
| ZSTD level 19 (palette mode) | best | fast | label maps, sparse data |
| LZ4 | ~1.5–2× larger than ZSTD | ~2–4× faster than ZSTD | ramdisk / NVMe, real-time streaming |

Use `USE_LZ4` when decompression throughput matters more than file size —
typically when data lives on a fast local device and the bottleneck is CPU
time in the decoder.

---

## SIMD / decode optimisation

The RLE decode path (`pzp_extractAndReconstruct`) selects an implementation
at compile time via `INTEL_OPTIMIZATIONS`:

| Implementation | Compiled when | Notes |
|---|---|---|
| `_Naive` | default | Portable scalar |
| `_SSE2` | `-DINTEL_OPTIMIZATIONS` | Kogge-Stone prefix scan (16 bytes/iter) |
| `_AVX2` | `-DINTEL_OPTIMIZATIONS` | Kogge-Stone prefix scan (32 bytes/iter) |

The SSE2 / AVX2 implementations use a two-step carry propagation to work
around the lane-isolation constraint of `_mm256_slli_si256` /
`_mm_slli_si128`: an intra-lane Kogge-Stone scan followed by an explicit
cross-lane carry broadcast.  1-channel and 2-channel images use SIMD prefix
sums; 3-channel images fall back to a scalar loop (stride-3 serial dependency
makes SIMD not worthwhile at typical image sizes).

The non-RLE decode path uses a single `memcpy` regardless of channel count.

---

## Python ctypes performance note

`pzp.read()` uses `ctypes.Array.from_address()` to wrap the C-allocated
buffer as a fixed-size ctypes Array, then copies to a numpy array via
`np.ctypeslib.as_array(...).copy()`.  This is a single C-level `memcpy`,
avoiding the O(n) Python-level iteration that occurs with naive POINTER
slicing (`ptr[:n]`) — the original bottleneck that caused 12× slower load
times before this fix.
