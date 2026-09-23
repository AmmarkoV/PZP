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
    [ 1 + 4×G bytes ] channel group table ("PZP1" frames only, see below)
    [ P bytes  ] palette data (optional, when USE_PALETTE is set)
    [ W×H×C bytes ] pixel / index data, laid out per channel group
```

The inner header magic tells the two frame layouts apart:

| Magic | Written by | Pixel data |
|---|---|---|
| `PZP1` | v0.03+ | channel group table follows the header |
| `PZP0` | before v0.03 | one implied 8-bit group over all channels, LEFT predictor if `USE_RLE` is set |

Decoders older than v0.03 reject `PZP1` frames rather than misreading them.
`scripts/migrate_directory.py` re-encodes existing files in place (verifying
every pixel before replacing a file); once no `PZP0` frames are left, the
`PZP0` branch in `pzp_frame_decode_from_memory()` can be removed.

### Channel group table

```
[ 1 byte ] group_count (1–8)
[ group_count × 4 bytes ] { channels, sample_bits, predictor, reserved=0 }
```

Groups take consecutive internal channels in order and together cover all of
them.  The pixel data holds each group's block one after another:

| sample_bits | channels | predictor | Block layout |
|---|---|---|---|
| 8 | any | 0 none / 1 left | the group's channels interleaved pixel by pixel; left = previous pixel of the same channel in raster order |
| 16 | 2 (high, low byte) | 2 gradient | one big-endian 16-bit sample predicted as left + up − upleft (mod 2^16); zigzag residuals stored as a high-byte plane then a low-byte plane |

`USE_PALETTE` is applied to the channels before prediction and is only allowed
when every group is 8-bit.  `USE_RLE` is not read for `PZP1` frames: the
predictors in the table replace it.

Python spec strings (`PZP.write(..., groups=...)`, `encode_directory.py
--groups`, `migrate_directory.py --groups`): comma-separated `u8[xN]:none|left`
and `u16:gradient`.  Examples:

| Image | Spec | Why |
|---|---|---|
| label + 16-bit depth packed into 3 bytes | `u8:left,u16:gradient` | depth predicted as one 16-bit value, label kept apart |
| 16-bit depth map | `u16:gradient` | |
| RGB segmentation map | `u8x3:left` | its channels share edges, so interleaved compresses best |

On COCO val2017 (label + DA3 depth, 200 images) `u8:left,u16:gradient` is 51%
of the previous LZ4 size and 54% of the previous ZSTD size, and decodes
1.6–1.7× faster with `INTEL_OPTIMIZATIONS` (AVX2).

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
