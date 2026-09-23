#!/usr/bin/env python3
"""
migrate_directory.py — Re-encode existing .pzp files in place with a channel group table.

Each file is decoded, re-encoded with --groups into a temporary file next to it,
decoded again and compared pixel for pixel with the original, and only then
atomically replaces the original.  A file that fails any step is left untouched
and reported.  Re-running is safe: files already migrated are just re-encoded
to the same content.

The codec of each file is kept (LZ4 stays LZ4, ZSTD stays ZSTD) unless --lz4 or
--zstd is given.  USE_PALETTE is kept only when every group is 8-bit.

Usage:
    python3 scripts/migrate_directory.py <directory> --groups SPEC [--lz4|--zstd] [--workers N]

Example (label channel + 16-bit depth packed as high/low bytes):
    python3 scripts/migrate_directory.py all_val2017PZPF --groups u8:left,u16:gradient
"""

import argparse
import os
import sys
import time
from pathlib import Path
from multiprocessing import Pool, cpu_count

_repo_root = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_repo_root))

import PZP


def _migrate_one(args):
    path, groups, codec = args
    tmp = path.with_name(path.name + ".migrating")
    try:
        with open(path, "rb") as f:
            is_container = f.read(4) == b"0PZP"  # bare pre-container frames have no frame count
        if is_container and PZP.frame_count(str(path)) > 1:
            return path, 0, 0, "multi-frame container, skipped"
        old_size = path.stat().st_size
        img, flags = PZP.read(str(path), return_flags=True)

        use_lz4 = bool(flags & PZP.USE_LZ4) if codec is None else (codec == "lz4")
        cfg = PZP.USE_COMPRESSION
        if (flags & PZP.USE_PALETTE) and "u16" not in groups:
            cfg |= PZP.USE_PALETTE

        PZP.write(str(tmp), img, groups=groups, use_lz4=use_lz4, configuration=cfg)
        if not (PZP.read(str(tmp)) == img).all():
            tmp.unlink()
            return path, 0, 0, "re-encoded pixels differ, original kept"
        new_size = tmp.stat().st_size
        os.replace(tmp, path)
        return path, old_size, new_size, None
    except Exception as exc:
        if tmp.exists():
            tmp.unlink()
        return path, 0, 0, str(exc)


def main():
    ap = argparse.ArgumentParser(
        description="Re-encode .pzp files in place with a channel group table.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("directory", help="Directory searched recursively for .pzp files")
    ap.add_argument("--groups", required=True,
                    help='Channel group spec, e.g. "u8:left,u16:gradient"')
    codec = ap.add_mutually_exclusive_group()
    codec.add_argument("--lz4",  action="store_true", help="Re-encode every file with LZ4")
    codec.add_argument("--zstd", action="store_true", help="Re-encode every file with ZSTD")
    ap.add_argument("--workers", type=int, default=cpu_count(),
                    help=f"Parallel workers (default: {cpu_count()})")
    args = ap.parse_args()

    PZP.parse_groups(args.groups)  # reject a bad spec before touching any file
    codec_choice = "lz4" if args.lz4 else ("zstd" if args.zstd else None)

    files = sorted(Path(args.directory).glob("**/*.pzp"))
    if not files:
        sys.exit(f"ERROR: no .pzp files found in {args.directory}")
    print(f"Migrating {len(files)} files in {args.directory}  groups={args.groups}  "
          f"codec={codec_choice or 'keep'}  workers={args.workers}")

    t0 = time.perf_counter()
    ok = err = 0
    old_total = new_total = 0
    tasks = [(f, args.groups, codec_choice) for f in files]
    with Pool(processes=args.workers) as pool:
        for i, (path, old, new, msg) in enumerate(pool.imap_unordered(_migrate_one, tasks), 1):
            if msg:
                err += 1
                print(f"  FAIL  {path}: {msg}")
            else:
                ok += 1
                old_total += old
                new_total += new
            if i % 100 == 0 or i == len(tasks):
                print(f"  {i:>6}/{len(tasks)}  {time.perf_counter() - t0:6.1f}s", end="\r", flush=True)

    print(f"\nDone: {ok} migrated, {err} failed in {time.perf_counter() - t0:.1f}s")
    if new_total:
        print(f"Size : {old_total/1e6:.1f} MB → {new_total/1e6:.1f} MB  "
              f"({100 * new_total / old_total:.0f}%)")
    sys.exit(1 if err else 0)


if __name__ == "__main__":
    main()
