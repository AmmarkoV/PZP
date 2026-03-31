# PZP Scripts

All scripts live in `scripts/`.  They import `PZP.py` from the repo root
automatically, so they work from a development clone without installing the
package.

---

## `encode_directory.py` — batch encode a directory to PZP

Encodes every image (PNG by default, or any extension) in a source directory
to a matching `.pzp` file in a target directory, preserving sub-directory
structure.  Uses multiprocessing for parallelism.

```bash
# Standard compression
python3 scripts/encode_directory.py test/segment_val2017 test/segment_val2017PZP

# RLE + palette (best ratio for segmentation maps)
python3 scripts/encode_directory.py test/segment_val2017 test/segment_val2017PZP \
    --rle --palette

# LZ4 for fastest possible load time
python3 scripts/encode_directory.py frames/ frames_pzp/ --lz4

# LZ4 + RLE, custom source extension, explicit parallelism
python3 scripts/encode_directory.py samples/ output/ --ext ppm --workers 8 --lz4 --rle
```

| Flag | Default | Description |
|---|---|---|
| `--rle` | off | Enable delta pre-filter |
| `--palette` | off | Enable palette indexing |
| `--lz4` | off | Use LZ4 codec instead of ZSTD |
| `--workers N` | CPU count | Parallel encoding processes |
| `--ext EXT` | `png` | Source file extension to scan for |

Outputs a summary line with total input size, output size, compression ratio,
saving percentage, and encoding rate (files/s).

Dependencies: `pip install opencv-python`  (for `cv2.imread`)

---

## `encode_animation_with_sound.py` — GIF + audio → PZP container

Reads a GIF (animated or static) and an audio file (WAV, MP3, OGG, FLAC)
and packs them into a single self-contained `.pzp` container.  Loop count and
per-frame delays are read automatically from the GIF NETSCAPE extension and
can be overridden on the command line.

```bash
# Output defaults to <gif_stem>.pzp beside the GIF
python3 scripts/encode_animation_with_sound.py animation.gif music.mp3

# Specify output path
python3 scripts/encode_animation_with_sound.py animation.gif sound.wav out.pzp

# With compression options
python3 scripts/encode_animation_with_sound.py animation.gif music.mp3 \
    --rle --palette

# Inter-frame delta — only helps when consecutive frames are very similar
# (slow pan, static background).  Avoid for high-motion content.
python3 scripts/encode_animation_with_sound.py animation.gif music.mp3 --delta

# Override loop count and frame rate
python3 scripts/encode_animation_with_sound.py animation.gif music.mp3 \
    --loop 3 --fps 12
```

| Flag | Default | Description |
|---|---|---|
| `--rle` | off | Intra-frame delta pre-filter |
| `--palette` | off | Per-channel palette encoding |
| `--delta` | off | Inter-frame delta — only beneficial for near-identical consecutive frames |
| `--loop N` | from GIF | Loop count (0 = forever) |
| `--fps FPS` | from GIF | Override per-frame delay (computed as `1000/FPS` ms) |

GIF frames are composited onto a black background and converted to RGB before
encoding, so transparent / palette GIFs are handled correctly.

Dependencies: `pip install Pillow numpy`

---

## `pzp-player` — animated PZP playback with audio

Interactive player for animated `.pzp` containers.  Renders frames at their
stored per-frame delays and plays back embedded audio synchronously via
`pygame.mixer`.  Audio is extracted to a temp file and cleaned up on exit.

```bash
# Install to PATH after  sudo make install
pzp-player animation.pzp

# Or run directly from repo
python3 scripts/pzp-player animation.pzp
python3 scripts/pzp-player animation.pzp --scale 2.0   # 2× zoom window
python3 scripts/pzp-player animation.pzp --fps 24      # override playback speed
```

| Flag | Default | Description |
|---|---|---|
| `--scale FACTOR` | 1.0 | Window scale factor (uses smooth scaling) |
| `--fps FPS` | from file | Override playback speed; ignores stored per-frame delays |

| Key | Action |
|---|---|
| `Space` | Pause / resume (audio pauses too) |
| `←` / `→` | Step one frame (while paused) |
| `R` | Restart from frame 0 (rewinds audio) |
| `Q` / `Esc` | Quit |

After the last loop the final frame is held until the window is closed.

Dependencies: `pip install numpy pygame`

---

## `compare_load_speed.py` — compare load speed: PNG vs PZP

Loads every matched pair from two directories (one PNG, one PZP) using
`cv2.imread` and `pzp.read` respectively, reporting total time, per-image
time, and winner across multiple passes.  Optionally verifies pixel identity
between the two decoders.

```bash
python3 scripts/compare_load_speed.py \
    test/segment_val2017 test/segment_val2017PZP \
    --max 500 --passes 3
```

| Flag | Default | Description |
|---|---|---|
| `--max N` | all | Load at most N matched pairs |
| `--warmup N` | 1 | Untimed warm-up passes before measurement |
| `--passes N` | 3 | Number of timed measurement passes |
| `--no-verify` | off | Skip pixel-identity check (faster startup) |

---

## `benchmark.py` — general benchmark

Times all build targets (`pzp`, `spzp`, `dpzp`) for compress and decompress
and compares them against PNG and JPEG for the bundled samples or any source
directory.  Two modes:

**Sample mode** — uses the bundled `samples/` directory, runs multiple
compress + decompress cycles, and prints a per-format table.

**Directory mode** — encodes or reuses a pre-encoded PZP directory and times
bulk decompression against PNG.

```bash
source venv/bin/activate

# Sample mode
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
| `--pzp-dir DIR` | — | Pre-existing `.pzp` directory (skip re-encoding) |
| `--max-files N` | all | Process at most N files |
| `--compare N` | 20 | Files to pixel-verify after decode |
| `--runs N` | 5 | Timing repetitions (sample mode) |
| `--no-debug` | off | Skip `dpzp` (slow debug target) |
| `--no-build` | off | Skip `make all` before benchmarking |

PNG and JPEG sources are automatically pre-converted to PPM for `pzp` /
`spzp` / `dpzp` (which read PNM/PPM only), keeping the comparison fair.
