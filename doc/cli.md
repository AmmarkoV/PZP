# PZP Command-line Interface

The `pzp` binary reads and writes PNM/PPM files (P5 grayscale, P6 colour).
All compression flags and the codec choice are stored inside the file; the
decompressor reads them automatically — no flags are needed on decompress.

---

## Single-frame operations

```bash
# Compress (ZSTD + delta filter)
./pzp compress          input.ppm  output.pzp
./pzp compress          input.pnm  output.pzp   # 16-bit depth supported

# Compress with LZ4 (faster decompress, larger file)
./pzp compress          input.ppm  output.pzp  --lz4

# Compress with palette mode (best for segmentation / label maps)
./pzp compress-palette  input.ppm  output.pzp
./pzp compress-palette  input.ppm  output.pzp  --lz4

# Pack (ZSTD only, no delta filter)
./pzp pack              input.ppm  output.pzp
./pzp pack              input.ppm  output.pzp  --lz4

# Decompress (any mode — codec and flags are stored in the file)
./pzp decompress        output.pzp  reconstructed.ppm
```

---

## Container operations

```bash
# Inspect a container (frames, loop count, audio, metadata, per-frame index)
./pzp info              file.pzp

# Extract a single frame from a container
./pzp extract-frame     file.pzp  frame0.ppm  0

# Pack multiple PNM frames into an animated container
./pzp pack-frames  out.pzp  <loop_count> <delay_ms>  frame*.ppm
./pzp pack-frames  out.pzp  0 100  --delta  frame*.ppm        # inter-frame delta
./pzp pack-frames  out.pzp  0 100  --lz4    frame*.ppm        # LZ4 codec
./pzp pack-frames  out.pzp  0 100  --delta --lz4  frame*.ppm  # both (any order)

# Attach audio to an existing container (rewrites to a new file)
./pzp attach-audio  input.pzp  sound.wav  output.pzp
./pzp attach-audio  input.pzp  music.mp3  output.pzp

# Attach a metadata string to an existing container
./pzp attach-meta   input.pzp  '{"fps":24,"label":"demo"}' output.pzp
```

`loop_count` 0 means loop forever.  `delay_ms` is the global per-frame delay
in milliseconds; individual frame delays cannot be set from the CLI (use the
Python API or `encode_animation_with_sound.py` for per-frame control).

---

## Codec / encoding flags

| Flag | Applies to | Effect |
|---|---|---|
| `--lz4` | compress, compress-palette, pack, pack-frames | LZ4 codec instead of ZSTD |
| `--delta` | pack-frames | Inter-frame delta encoding |

---

## PNM conversion

The binary has no libpng / libjpeg dependency by design.  Convert sources
with any standard tool:

```bash
convert photo.png photo.ppm       # ImageMagick
ffmpeg  -i photo.jpg photo.ppm    # FFmpeg
python3 -c "from PIL import Image; Image.open('a.png').save('a.ppm')"
```
