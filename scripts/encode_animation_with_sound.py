#!/usr/bin/env python3
"""
encode_animation_with_sound.py — encode a GIF + audio file into a .pzp container.

Usage:
    encode_animation_with_sound.py <animation.gif> <audio.(wav|mp3|ogg|flac)> [output.pzp]
                                   [--rle] [--palette] [--loop N] [--fps FPS]

Arguments:
    animation.gif   Input GIF (animated or static).
    audio file      WAV, MP3, OGG or FLAC to embed verbatim in the container.
    output.pzp      Output path.  Defaults to <gif_stem>.pzp beside the GIF.

Options:
    --rle           Enable delta pre-filter (good for smooth / photographic frames).
    --palette       Enable per-channel palette (good for limited-colour frames).
    --loop N        Override loop count (0 = loop forever; default: read from GIF).
    --fps FPS       Override per-frame delay (default: read from GIF).

Dependencies:
    libpzp.so  — sudo make install  (or  make  in the repo root)
    Pillow     — pip install Pillow
    numpy      — pip install numpy
"""

import argparse
import os
import sys
from pathlib import Path

# ── Locate PZP module ─────────────────────────────────────────────────────────
_SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(_SCRIPT_DIR.parent))  # repo root (dev mode)

try:
    import PZP
except ImportError:
    print("error: PZP module not found.\n"
          "  Run from the PZP repo root or install with:  pip install -e .",
          file=sys.stderr)
    sys.exit(1)

try:
    import numpy as np
except ImportError:
    print("error: numpy is required.  pip install numpy", file=sys.stderr)
    sys.exit(1)

try:
    from PIL import Image
except ImportError:
    print("error: Pillow is required.  pip install Pillow", file=sys.stderr)
    sys.exit(1)

# ─────────────────────────────────────────────────────────────────────────────

_AUDIO_EXT_TO_FORMAT = {
    ".wav":  "WAVE",
    ".wave": "WAVE",
    ".mp3":  "MPEG",
    ".mpeg": "MPEG",
    ".ogg":  "OGGX",
    ".flac": "FLAC",
}


def read_gif(gif_path: str, override_fps: float = 0.0):
    """
    Read all frames from a GIF.

    Returns
    -------
    frames     : list of (H, W, 3) uint8 numpy arrays
    delays     : list of int (ms per frame)
    loop_count : int  (0 = loop forever, as in PZP convention)
    """
    img = Image.open(gif_path)
    n_frames = getattr(img, "n_frames", 1)

    # GIF NETSCAPE loop count: 0 = infinite (same convention as PZP)
    loop_count = img.info.get("loop", 0)

    frames = []
    delays = []

    for i in range(n_frames):
        img.seek(i)

        if override_fps > 0.0:
            delay = int(1000.0 / override_fps)
        else:
            # GIF 'duration' is in ms; absent on static frames → default 100 ms
            delay = int(img.info.get("duration", 100))

        # Composite palette/transparency frames onto a black background so that
        # every output frame is a clean (H, W, 3) RGB array.
        rgba = img.convert("RGBA")
        bg   = Image.new("RGBA", rgba.size, (0, 0, 0, 255))
        bg.paste(rgba, mask=rgba.split()[3])
        arr  = np.array(bg.convert("RGB"), dtype=np.uint8)

        frames.append(arr)
        delays.append(delay)

        print(f"\r  reading frame {i + 1}/{n_frames} …", end="", flush=True)

    print()
    return frames, delays, loop_count


def read_audio(audio_path: str):
    """Return (raw_bytes, format_str) for the audio file."""
    ext    = Path(audio_path).suffix.lower()
    fmt    = _AUDIO_EXT_TO_FORMAT.get(ext)
    if fmt is None:
        print(f"  warning: unrecognised audio extension {ext!r}; treating as WAVE",
              file=sys.stderr)
        fmt = "WAVE"
    with open(audio_path, "rb") as f:
        data = f.read()
    return data, fmt


def encode(gif_path: str, audio_path: str, output_path: str,
           use_rle: bool, use_palette: bool,
           loop_override: int, fps_override: float):

    print(f"GIF   : {gif_path}")
    print(f"Audio : {audio_path}")
    print(f"Output: {output_path}")

    # ── Read GIF ──────────────────────────────────────────────────────────────
    frames, delays, loop_count = read_gif(gif_path, fps_override)

    if loop_override >= 0:
        loop_count = loop_override

    n = len(frames)
    h, w = frames[0].shape[:2]
    print(f"  {n} frame(s)  {w}×{h}  "
          f"loop={'∞' if loop_count == 0 else loop_count}  "
          f"delays={delays[:4]}{'…' if n > 4 else ''} ms")

    # ── Read audio ────────────────────────────────────────────────────────────
    audio_data, audio_fmt = read_audio(audio_path)
    print(f"  audio: {len(audio_data):,} bytes  format={audio_fmt}")

    # ── Encode ────────────────────────────────────────────────────────────────
    PZP.write_container(
        output_path, frames,
        delays      = delays,
        loop_count  = loop_count,
        use_rle     = use_rle,
        use_palette = use_palette,
        audio       = audio_data,
        audio_format= audio_fmt,
    )

    size = os.path.getsize(output_path)
    print(f"  written {size:,} bytes → {output_path}")


# ── CLI ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        prog="encode_animation_with_sound.py",
        description="Encode a GIF + audio file into a single .pzp container.")

    parser.add_argument("gif",
                        help="Input GIF file")
    parser.add_argument("audio",
                        help="Audio file to embed (WAV, MP3, OGG or FLAC)")
    parser.add_argument("output", nargs="?", default=None,
                        help="Output .pzp path (default: <gif_stem>.pzp)")
    parser.add_argument("--rle", action="store_true",
                        help="Enable delta pre-filter")
    parser.add_argument("--palette", action="store_true",
                        help="Enable per-channel palette encoding")
    parser.add_argument("--loop", type=int, default=-1, metavar="N",
                        help="Override loop count (0=forever; default: read from GIF)")
    parser.add_argument("--fps", type=float, default=0.0, metavar="FPS",
                        help="Override per-frame delay (default: read from GIF)")

    args = parser.parse_args()

    if not os.path.exists(args.gif):
        print(f"error: GIF not found: {args.gif}", file=sys.stderr)
        sys.exit(1)
    if not os.path.exists(args.audio):
        print(f"error: audio file not found: {args.audio}", file=sys.stderr)
        sys.exit(1)

    output = args.output
    if output is None:
        output = str(Path(args.gif).with_suffix(".pzp"))

    encode(args.gif, args.audio, output,
           use_rle     = args.rle,
           use_palette = args.palette,
           loop_override = args.loop,
           fps_override  = args.fps)


if __name__ == "__main__":
    main()
