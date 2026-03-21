#!/usr/bin/env python3
"""
compare_load_speed.py — Compare bulk load speed of PNG vs PZP for a matched
image set.

Given a directory of .png files and a directory of matching .pzp files the
script loads every image in both directories, times each load, and prints a
summary table.

Usage:
    python3 scripts/compare_load_speed.py <png_dir> <pzp_dir> [options]

Options:
    --max N         Load at most N files (default: all)
    --warmup N      Warm-up passes before timing (default: 1)
    --passes N      Timed measurement passes (default: 3)
    --no-verify     Skip pixel-identity check (faster, useful for large sets)

Example:
    python3 scripts/compare_load_speed.py \\
        test/segment_val2017 test/segment_val2017PZP \\
        --max 500 --passes 3
"""

import argparse
import sys
import time
from pathlib import Path

import cv2
import numpy as np

_repo_root = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_repo_root))

import PZP


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _collect_pairs(png_dir: Path, pzp_dir: Path, max_files: int):
    """Return list of (png_path, pzp_path) for files present in both dirs."""
    pairs = []
    for png in sorted(png_dir.glob("**/*.png")):
        rel  = png.relative_to(png_dir)
        pzp  = (pzp_dir / rel).with_suffix(".pzp")
        if pzp.exists():
            pairs.append((png, pzp))
        if max_files and len(pairs) >= max_files:
            break
    return pairs


def _load_png(path: Path) -> np.ndarray:
    img = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
    if img is None:
        raise RuntimeError(f"cv2 could not read {path}")
    # Normalise to RGB / mono (same convention as PZP.read)
    if img.ndim == 3 and img.shape[2] == 3:
        img = img[:, :, ::-1]   # BGR → RGB
    elif img.ndim == 3 and img.shape[2] == 4:
        img = img[:, :, 2::-1]  # BGRA → RGB
    return img


def _load_pzp(path: Path) -> np.ndarray:
    return PZP.read(str(path))


def _time_pass(pairs, load_fn, idx):
    """Load all files with load_fn(pairs[i][idx]) and return total seconds."""
    t0 = time.perf_counter()
    for pair in pairs:
        load_fn(pair[idx])
    return time.perf_counter() - t0


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Compare bulk load speed: PNG vs PZP.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("png_dir", help="Directory with .png files")
    ap.add_argument("pzp_dir", help="Directory with .pzp files")
    ap.add_argument("--max",       type=int, default=0,
                    help="Max files to load (0 = all)")
    ap.add_argument("--warmup",    type=int, default=1,
                    help="Warm-up passes (not timed, default 1)")
    ap.add_argument("--passes",    type=int, default=3,
                    help="Timed passes (default 3)")
    ap.add_argument("--no-verify", action="store_true",
                    help="Skip per-pixel correctness check")
    args = ap.parse_args()

    png_dir = Path(args.png_dir)
    pzp_dir = Path(args.pzp_dir)

    for d in (png_dir, pzp_dir):
        if not d.is_dir():
            sys.exit(f"ERROR: directory not found: {d}")

    pairs = _collect_pairs(png_dir, pzp_dir, args.max)
    if not pairs:
        sys.exit("ERROR: no matching png/pzp pairs found.")

    n = len(pairs)
    print(f"Files  : {n} matched pairs")
    print(f"PNG dir: {png_dir}")
    print(f"PZP dir: {pzp_dir}")
    print()

    # ------------------------------------------------------------------
    # Optional correctness check (first file only unless --no-verify)
    # ------------------------------------------------------------------
    if not args.no_verify:
        print("Verifying pixel identity on first 10 pairs...", end=" ", flush=True)
        mismatches = 0
        check_n = min(10, n)
        for png_p, pzp_p in pairs[:check_n]:
            img_png = _load_png(png_p)
            img_pzp = _load_pzp(pzp_p)
            if img_png.shape != img_pzp.shape or not np.array_equal(img_png, img_pzp):
                print(f"\n  MISMATCH: {png_p.name}")
                mismatches += 1
        if mismatches == 0:
            print(f"OK ({check_n} checked)")
        else:
            print(f"{mismatches} mismatches found — results may be unreliable")
        print()

    # ------------------------------------------------------------------
    # Warm-up (fills OS page cache, warms JIT / libzstd state)
    # ------------------------------------------------------------------
    if args.warmup:
        print(f"Warm-up ({args.warmup} pass{'es' if args.warmup != 1 else ''})...",
              end=" ", flush=True)
        for _ in range(args.warmup):
            _time_pass(pairs, _load_png, 0)
            _time_pass(pairs, _load_pzp, 1)
        print("done")
        print()

    # ------------------------------------------------------------------
    # Timed passes
    # ------------------------------------------------------------------
    png_times = []
    pzp_times = []

    print(f"{'Pass':>4}  {'PNG total':>10}  {'PZP total':>10}  "
          f"{'PNG/img':>9}  {'PZP/img':>9}  {'Δ':>8}")
    print("-" * 60)

    for p in range(1, args.passes + 1):
        t_png = _time_pass(pairs, _load_png, 0)
        t_pzp = _time_pass(pairs, _load_pzp, 1)
        png_times.append(t_png)
        pzp_times.append(t_pzp)

        delta_pct = (t_png - t_pzp) / t_png * 100
        sign = "+" if delta_pct >= 0 else ""
        print(f"{p:>4}  {t_png*1e3:>9.1f}ms  {t_pzp*1e3:>9.1f}ms  "
              f"{t_png/n*1e3:>8.3f}ms  {t_pzp/n*1e3:>8.3f}ms  "
              f"{sign}{delta_pct:.1f}%")

    print("-" * 60)

    # Best of N
    best_png = min(png_times)
    best_pzp = min(pzp_times)
    delta_pct = (best_png - best_pzp) / best_png * 100
    sign = "+" if delta_pct >= 0 else ""
    print(f"{'best':>4}  {best_png*1e3:>9.1f}ms  {best_pzp*1e3:>9.1f}ms  "
          f"{best_png/n*1e3:>8.3f}ms  {best_pzp/n*1e3:>8.3f}ms  "
          f"{sign}{delta_pct:.1f}%")

    print()

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    winner   = "PZP" if best_pzp < best_png else "PNG"
    speedup  = best_png / best_pzp if best_pzp < best_png else best_pzp / best_png

    print("=" * 60)
    print(f"  Files loaded  : {n}")
    print(f"  Best PNG total: {best_png*1e3:.1f} ms  ({best_png/n*1e3:.3f} ms/img)")
    print(f"  Best PZP total: {best_pzp*1e3:.1f} ms  ({best_pzp/n*1e3:.3f} ms/img)")
    print(f"  Winner        : {winner}  ({speedup:.2f}× faster)")
    if winner == "PZP":
        print(f"  PZP is {delta_pct:.1f}% faster than PNG for this dataset.")
    else:
        print(f"  PNG is {-delta_pct:.1f}% faster than PZP for this dataset.")
    print("=" * 60)

    # ------------------------------------------------------------------
    # Compression ratio (informational)
    # ------------------------------------------------------------------
    total_png = sum(p.stat().st_size for p, _ in pairs)
    total_pzp = sum(p.stat().st_size for _, p in pairs)
    ratio     = total_png / total_pzp if total_pzp else float("inf")
    saving    = (1 - total_pzp / total_png) * 100 if total_png else 0
    print()
    print(f"  PNG size : {total_png/1e6:.1f} MB")
    print(f"  PZP size : {total_pzp/1e6:.1f} MB  (ratio={ratio:.2f}×, saving={saving:.1f}%)")


if __name__ == "__main__":
    main()
