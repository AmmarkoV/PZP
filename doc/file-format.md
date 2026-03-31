# PZP File Format

## Single-frame `.pzp`

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

The codec is selected per-frame via bit 31 of the size prefix.  All existing
files written without `USE_LZ4` have bit 31 = 0 and are backward-compatible.

---

## Container `.pzp` (animated / multi-frame)

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

Frame index entries store absolute byte offsets from the start of the file,
so individual frames can be seeked to directly without scanning.  The codec
bit in each frame's size prefix is independent, so a container may freely mix
ZSTD and LZ4 frames.

---

## Compression flags

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

### Notes on flag interactions

- **`USE_PALETTE` + `USE_INTER_DELTA`** — avoid this combination.  Palette
  encoding requires few unique values per channel; delta subtraction spreads
  values and destroys sparsity, making palette encoding ineffective.

- **`USE_INTER_DELTA`** — only improves compression when consecutive frames are
  highly similar.  For high-motion content (rotation, cuts) the delta signal
  has high entropy and the result is *larger* than keyframes.  The
  `PZP_VERBOSE=1` build prints per-frame delta statistics (unchanged%,
  near-zero%, MAD, max|Δ|) to help diagnose this.

- **`USE_LZ4`** — decompresses roughly 2–4× faster than ZSTD at the cost of a
  larger output file.  Best for ramdisk / NVMe workloads where decompression
  time dominates over I/O.
