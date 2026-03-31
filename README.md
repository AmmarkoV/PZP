# PZP — Portable Zipped PNM

A minimal, header-only lossless image compression library in C with Python
bindings.  PZP wraps PNM pixel data in a compact binary header and compresses
it with ZSTD or LZ4, achieving faster load times and better ratios than PNG
for many real-world datasets.

> **2.28× faster** to load than PNG, **52%** smaller on disk (COCO
> segmentation maps, `libpzp.so` + ctypes).  See [doc/performance.md](doc/performance.md).

Similar projects: [QOI](https://github.com/phoboslab/qoi), [ZPNG](https://github.com/catid/Zpng)

---

## Features

- Header-only C library (`pzp.h`) — `#include` and link, nothing to compile
- 8-bit and 16-bit, grayscale and RGB
- ZSTD (high ratio) or LZ4 (fastest decompress) codec, selectable per frame
- Optional delta pre-filter, palette indexing, and inter-frame delta
- Multi-frame animated container with per-frame delays, loop count,
  embedded audio (WAV/MP3/OGG/FLAC) and metadata
- Python ctypes bindings (`PZP.py`) installable via `pip`

---

## Dependencies

```bash
sudo apt install libzstd-dev liblz4-dev   # Ubuntu / Debian
sudo dnf install libzstd-devel lz4-devel  # Fedora / RHEL
brew install zstd lz4                     # macOS
```

---

## Building

```bash
make              # pzp  spzp  dpzp  libpzp.so
make libpzp.so    # shared library only (needed for Python bindings)
make test         # compress + decompress bundled samples, verify output
make clean
```

| Target | Binary | Flags |
|---|---|---|
| release | `pzp` | `-O3 -march=native` |
| SIMD/AVX2 | `spzp` | `-O3 -mavx2 -DINTEL_OPTIMIZATIONS` |
| debug | `dpzp` | `-O0 -g3` |
| shared lib | `libpzp.so` | release + `-shared -fPIC` |

### System install

```bash
sudo make install              # → /usr/local/bin, /usr/local/lib, /usr/local/include
sudo make install PREFIX=/usr  # custom prefix
sudo make uninstall
```

`DESTDIR` is supported for packaging (`.deb`, `.rpm`):

```bash
make install DESTDIR=/tmp/pkg PREFIX=/usr
```

### Desktop integration (thumbnails)

```bash
sudo make install          # C library + binary first
sudo make install-desktop  # MIME type + thumbnailer
pip install Pillow
pkill tumbler; tumbler &   # restart thumbnail daemon
```

Per-user (no `sudo`):

```bash
make install-desktop PREFIX=~/.local
update-mime-database ~/.local/share/mime
```

Remove: `sudo make uninstall-desktop`

---

## Quick start

```bash
# Single image — compress and decompress
./pzp compress   input.ppm  output.pzp
./pzp decompress output.pzp reconstructed.ppm

# LZ4 codec (faster decompress)
./pzp compress   input.ppm  output.pzp  --lz4

# Inspect a container
./pzp info  animation.pzp

# Python
python3 -c "import PZP; img = PZP.read('output.pzp'); print(img.shape)"
```

---

## Documentation

| Topic | File |
|---|---|
| File format, binary layout, codec/flag reference | [doc/file-format.md](doc/file-format.md) |
| CLI subcommands and flags | [doc/cli.md](doc/cli.md) |
| C API (`pzp.h`) and shared library (`libpzp.so`) ABI | [doc/c-api.md](doc/c-api.md) |
| Python package — read, write, container API, thread lifecycle | [doc/python-api.md](doc/python-api.md) |
| Scripts — encode_directory, encode_animation_with_sound, pzp-player, benchmark | [doc/scripts.md](doc/scripts.md) |
| Performance benchmarks and SIMD notes | [doc/performance.md](doc/performance.md) |
