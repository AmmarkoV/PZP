#!/usr/bin/env python3
"""
encode_directory.py — Batch-encode all PNG files in a source directory to PZP.

Usage:
    python3 scripts/encode_directory.py <source_dir> <target_dir> [options]

Options:
    --rle           Enable delta pre-filter (USE_RLE)
    --palette       Enable per-channel palette indexing (USE_PALETTE)
    --workers N     Parallel worker processes (default: CPU count)
    --ext EXT       Source extension to scan for (default: png)

Examples:
    # Standard compression
    python3 scripts/encode_directory.py test/segment_val2017 test/segment_val2017PZP

    # With RLE + palette (best ratio for segmentation maps)
    python3 scripts/encode_directory.py test/segment_val2017 test/segment_val2017PZP --rle --palette

    # Also works with .ppm/.pgm sources
    python3 scripts/encode_directory.py samples/ output/ --ext ppm
"""

import argparse
import os
import sys
import time
from pathlib import Path
from multiprocessing import Pool, cpu_count

# Make sure PZP.py is importable from repo root
_repo_root = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_repo_root))

import PZP


def _encode_one(args):
    src_path, dst_path, flags = args
    try:
        import cv2
        img = cv2.imread(str(src_path), cv2.IMREAD_UNCHANGED)
        if img is None:
            return src_path, False, "cv2 could not read file"

        # cv2 loads BGR; PZP expects RGB for colour images — reorder
        if img.ndim == 3 and img.shape[2] == 3:
            import numpy as np
            img = img[:, :, ::-1]   # BGR → RGB
        elif img.ndim == 3 and img.shape[2] == 4:
            import numpy as np
            img = img[:, :, 2::-1]  # BGRA → RGB (drop alpha)

        PZP.write(
            str(dst_path),
            img,
            configuration=flags,
        )
        return src_path, True, None
    except Exception as exc:
        return src_path, False, str(exc)


def main():
    ap = argparse.ArgumentParser(
        description="Batch-encode image files to PZP format.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("source_dir", help="Directory containing source images")
    ap.add_argument("target_dir", help="Output directory for .pzp files")
    ap.add_argument("--rle",     action="store_true", help="Enable delta pre-filter")
    ap.add_argument("--palette", action="store_true", help="Enable palette indexing")
    ap.add_argument("--workers", type=int, default=cpu_count(),
                    help=f"Parallel workers (default: {cpu_count()})")
    ap.add_argument("--ext", default="png",
                    help="Source file extension to scan for (default: png)")
    args = ap.parse_args()

    src_dir = Path(args.source_dir)
    dst_dir = Path(args.target_dir)

    if not src_dir.is_dir():
        sys.exit(f"ERROR: source directory does not exist: {src_dir}")

    dst_dir.mkdir(parents=True, exist_ok=True)

    # Build configuration bitfield
    flags = PZP.USE_COMPRESSION
    if args.rle:
        flags |= PZP.USE_RLE
    if args.palette:
        flags |= PZP.USE_PALETTE

    flag_names = []
    if flags & PZP.USE_RLE:     flag_names.append("RLE")
    if flags & PZP.USE_PALETTE: flag_names.append("PALETTE")
    flag_str = "+".join(flag_names) if flag_names else "none"

    ext = args.ext.lstrip(".")
    sources = sorted(src_dir.glob(f"**/*.{ext}"))
    if not sources:
        sys.exit(f"ERROR: no .{ext} files found in {src_dir}")

    print(f"Source : {src_dir}  ({len(sources)} files, ext=.{ext})")
    print(f"Target : {dst_dir}")
    print(f"Flags  : {flag_str}  (bitfield={flags:#x})")
    print(f"Workers: {args.workers}")
    print()

    # Build task list — preserve sub-directory structure
    tasks = []
    for src in sources:
        rel   = src.relative_to(src_dir)
        dst   = (dst_dir / rel).with_suffix(".pzp")
        dst.parent.mkdir(parents=True, exist_ok=True)
        tasks.append((src, dst, flags))

    t0 = time.perf_counter()
    ok = err = 0

    with Pool(processes=args.workers) as pool:
        for i, (src, success, msg) in enumerate(
                pool.imap_unordered(_encode_one, tasks), 1):
            if success:
                ok += 1
            else:
                err += 1
                print(f"  FAIL  {src}: {msg}")

            # Progress every 100 files
            if i % 100 == 0 or i == len(tasks):
                elapsed = time.perf_counter() - t0
                rate = i / elapsed
                print(f"  {i:>6}/{len(tasks)}  {elapsed:6.1f}s  {rate:.0f} files/s",
                      end="\r", flush=True)

    elapsed = time.perf_counter() - t0
    print()
    print(f"\nDone: {ok} encoded, {err} failed  in {elapsed:.2f}s  "
          f"({ok/elapsed:.0f} files/s)")

    # Compression ratio summary
    if ok:
        total_src = sum(s.stat().st_size for s, _, _ in tasks)
        total_dst = sum((dst_dir / s.relative_to(src_dir)).with_suffix(".pzp").stat().st_size
                        for s, _, _ in tasks
                        if (dst_dir / s.relative_to(src_dir)).with_suffix(".pzp").exists())
        if total_dst:
            ratio = total_src / total_dst
            saving = (1 - total_dst / total_src) * 100
            print(f"Size   : {total_src/1e6:.1f} MB → {total_dst/1e6:.1f} MB  "
                  f"ratio={ratio:.2f}×  saving={saving:.1f}%")


if __name__ == "__main__":
    main()
