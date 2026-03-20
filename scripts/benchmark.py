#!/usr/bin/env python3
"""
PZP Benchmark — timing, compression ratio, and correctness.

Two modes
─────────
Sample mode (default)
    Benchmarks the bundled samples/ files with multiple timing runs and
    per-file pixel comparisons.  All three build targets are exercised.

Directory mode (--source-dir DIR)
    Processes every image in DIR in a single pass, timing the complete
    batch and reporting aggregate compression-ratio statistics.
    Supply --pzp-dir to benchmark decompression of pre-existing .pzp
    files instead of compressing from scratch.

Examples
────────
    python3 scripts/benchmark.py                                  # sample mode
    python3 scripts/benchmark.py --no-debug --runs 3             # skip dpzp
    python3 scripts/benchmark.py \\
        --source-dir test/segment_val2017 \\
        --pzp-dir    test/segment_val2017PZP \\
        --max-files  500 --compare 20                            # directory mode
    python3 scripts/benchmark.py \\
        --source-dir test/segment_val2017                        # compress+decompress
"""

import argparse
import os
import random
import subprocess
import sys
import tempfile
import time

import cv2
import numpy as np

# ─────────────────────────────────────────────────────────────────────────────
# Paths / constants
# ─────────────────────────────────────────────────────────────────────────────

ROOT        = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAMPLES_DIR = os.path.join(ROOT, "samples")

BINARIES = {
    "pzp  (release)": os.path.join(ROOT, "pzp"),
    "spzp (SIMD)   ": os.path.join(ROOT, "spzp"),
    "dpzp (debug)  ": os.path.join(ROOT, "dpzp"),
}

# Built-in sample files: name → (cv2 imread flag, description)
SAMPLES = {
    "sample.ppm":  (cv2.IMREAD_COLOR,                           "256×256  3ch  8-bit  PPM"),
    "segment.ppm": (cv2.IMREAD_COLOR,                           "480×640  3ch  8-bit  PPM"),
    "rgb8.pnm":    (cv2.IMREAD_COLOR,                           "640×360  3ch  8-bit  PNM"),
    "depth16.pnm": (cv2.IMREAD_ANYDEPTH | cv2.IMREAD_ANYCOLOR, "640×360  1ch 16-bit  PNM"),
}

# Files that have equivalent other-format siblings for cross-format timing
EQUIVALENT_FORMATS = {
    "rgb8.pnm": [
        ("PNG", os.path.join(SAMPLES_DIR, "rgb8.png")),
        ("JPG", os.path.join(SAMPLES_DIR, "rgb8.jpg")),
    ],
}

IMAGE_EXTENSIONS = {".ppm", ".pnm", ".png", ".jpg", ".jpeg"}

# ─────────────────────────────────────────────────────────────────────────────
# ANSI colour helpers
# ─────────────────────────────────────────────────────────────────────────────

RESET  = "\033[0m"
BOLD   = "\033[1m"
GREEN  = "\033[32m"
YELLOW = "\033[33m"
RED    = "\033[31m"
CYAN   = "\033[36m"


def col(text, code):  return f"{code}{text}{RESET}"
def fmt_ms(ms):       return f"{ms:8.1f} ms"
def fmt_bytes(n):
    if n >= 1_000_000_000: return f"{n/1_000_000_000:.3f} GB"
    if n >= 1_000_000:     return f"{n/1_000_000:.2f} MB"
    if n >= 1_000:         return f"{n/1_000:.1f} KB"
    return f"{n} B"

# ─────────────────────────────────────────────────────────────────────────────
# Utilities
# ─────────────────────────────────────────────────────────────────────────────

def build():
    print(col("Building targets …", BOLD))
    r = subprocess.run(["make", "all"], cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout); print(r.stderr)
        sys.exit("make all failed")
    print(col("  Build OK\n", GREEN))


def time_fn(fn, runs):
    """Return (mean_ms, last_result)."""
    result, elapsed = None, []
    for _ in range(runs):
        t0 = time.perf_counter()
        result = fn()
        elapsed.append(time.perf_counter() - t0)
    return (sum(elapsed) / len(elapsed)) * 1e3, result


def run_binary(cmd):
    r = subprocess.run(cmd, capture_output=True)
    return r.returncode, r.stderr


def load_image(path, flag=cv2.IMREAD_ANYCOLOR | cv2.IMREAD_ANYDEPTH):
    img = cv2.imread(path, flag)
    if img is None:
        raise RuntimeError(f"cv2.imread failed: {path}")
    return img


def infer_flag(path):
    """Choose the right cv2 imread flag based on file content."""
    # Try reading as any depth; check actual dtype
    img = cv2.imread(path, cv2.IMREAD_ANYCOLOR | cv2.IMREAD_ANYDEPTH)
    if img is None:
        return cv2.IMREAD_COLOR
    return cv2.IMREAD_ANYCOLOR | cv2.IMREAD_ANYDEPTH


def compare(a, b):
    """Return (identical, max_diff, psnr_db|None)."""
    if a.shape != b.shape:
        return False, None, None
    diff = np.abs(a.astype(np.int32) - b.astype(np.int32))
    max_diff = int(diff.max())
    identical = max_diff == 0
    psnr = None
    if not identical and a.dtype == np.uint8:
        mse = float(np.mean(diff.astype(np.float64) ** 2))
        psnr = 10 * np.log10(255.0 ** 2 / mse) if mse > 0 else float("inf")
    return identical, max_diff, psnr


def pixel_label(identical, max_diff, psnr):
    if identical:
        return col("IDENTICAL", GREEN)
    tag = f"DIFF  max={max_diff}"
    if psnr is not None:
        tag += f"  PSNR={psnr:.1f} dB"
    return col(tag, YELLOW)


def ratio_stats(ratios):
    """Return (mean, min, max, stdev) for a list of floats."""
    if not ratios:
        return 0, 0, 0, 0
    mean = sum(ratios) / len(ratios)
    mn   = min(ratios)
    mx   = max(ratios)
    var  = sum((r - mean) ** 2 for r in ratios) / len(ratios)
    return mean, mn, mx, var ** 0.5


def discover_files(source_dir, max_files=None):
    """Return sorted list of image paths in source_dir."""
    files = sorted(
        p for p in (os.path.join(source_dir, f) for f in os.listdir(source_dir))
        if os.path.splitext(p)[1].lower() in IMAGE_EXTENSIONS
    )
    if max_files and len(files) > max_files:
        files = files[:max_files]
    return files


# ─────────────────────────────────────────────────────────────────────────────
# Sample-mode benchmark (original behaviour)
# ─────────────────────────────────────────────────────────────────────────────

def benchmark_sample(sample_name, imread_flag, runs, active_binaries):
    src      = os.path.join(SAMPLES_DIR, sample_name)
    raw_size = os.path.getsize(src)

    print(col("=" * 72, BOLD))
    print(col(f" {sample_name}  |  raw {fmt_bytes(raw_size)}", BOLD))
    print(col("=" * 72, BOLD))

    original = load_image(src, imread_flag)

    hdr = (f"  {'TARGET':<18}  {'COMPRESS':>10}  {'DECOMPRESS':>10}  "
           f"{'PZP SIZE':>10}  {'RATIO':>6}  PIXELS")
    print(f"\n{hdr}")
    print(f"  {'-'*18}  {'-'*10}  {'-'*10}  {'-'*10}  {'-'*6}  {'─'*30}")

    rows = []
    with tempfile.TemporaryDirectory(prefix="pzp_bench_") as tmp:
        pzp_path = os.path.join(tmp, "bench.pzp")
        out_ppm  = os.path.join(tmp, "bench_out.ppm")

        for label, binary in active_binaries.items():
            cmp_ms, (rc, _) = time_fn(
                lambda: run_binary([binary, "compress", src, pzp_path]), runs)
            if rc != 0:
                print(f"  {label}  COMPRESS FAILED"); continue

            pzp_size = os.path.getsize(pzp_path)
            ratio    = raw_size / pzp_size

            dmp_ms, (rc, _) = time_fn(
                lambda: run_binary([binary, "decompress", pzp_path, out_ppm]), runs)
            if rc != 0:
                print(f"  {label}  DECOMPRESS FAILED"); continue

            recon = load_image(out_ppm, imread_flag)
            identical, max_diff, psnr = compare(original, recon)
            px = pixel_label(identical, max_diff, psnr)

            print(f"  {label}  {fmt_ms(cmp_ms)}  {fmt_ms(dmp_ms)}  "
                  f"{fmt_bytes(pzp_size):>10}  {ratio:5.2f}×  {px}")
            rows.append((label, cmp_ms, dmp_ms, pzp_size, ratio, identical))

    # OpenCV native decode for the same image
    print()
    print(f"  {'FORMAT':<18}  {'DECODE':>10}  PIXELS vs original")
    print(f"  {'-'*18}  {'-'*10}  {'─'*40}")

    ext = os.path.splitext(sample_name)[1].upper().lstrip(".")
    cv_ms, _ = time_fn(lambda: load_image(src, imread_flag), runs * 2)
    print(f"  cv2  {ext:<13}  {fmt_ms(cv_ms)}  (ground truth)")

    for fmt_label, fmt_path in EQUIVALENT_FORMATS.get(sample_name, []):
        if not os.path.exists(fmt_path):
            continue
        eq_ms, _ = time_fn(lambda p=fmt_path: load_image(p), runs * 2)
        eq_img = load_image(fmt_path)
        identical, max_diff, psnr = compare(original, eq_img)
        print(f"  cv2  {fmt_label:<13}  {fmt_ms(eq_ms)}  {pixel_label(identical, max_diff, psnr)}")

    print()
    return rows


# ─────────────────────────────────────────────────────────────────────────────
# Directory-mode benchmark
# ─────────────────────────────────────────────────────────────────────────────

def benchmark_directory(source_dir, pzp_dir, active_binaries, max_files, compare_count):
    """
    Batch-process all image files in source_dir.

    If pzp_dir is given the PZP files are taken from there (compress step
    is skipped for that pre-existing set, and decompression is timed from
    those files).  Compression from scratch is always attempted via tmpdir.
    """
    files = discover_files(source_dir, max_files)
    if not files:
        sys.exit(f"No image files found in {source_dir}")

    n = len(files)
    total_src = sum(os.path.getsize(f) for f in files)

    print(col("=" * 72, BOLD))
    print(col(f" Directory: {source_dir}", BOLD))
    print(col(f" {n} files  |  total source {fmt_bytes(total_src)}"
              f"  |  avg {fmt_bytes(total_src // n)}", BOLD))
    print(col("=" * 72, BOLD))

    # Probe imread flag from the first file
    probe_flag = cv2.IMREAD_ANYCOLOR | cv2.IMREAD_ANYDEPTH

    compare_indices = sorted(random.sample(range(n), min(compare_count, n)))
    compare_files   = [files[i] for i in compare_indices]

    all_rows = []

    # ── 1. Benchmark pre-existing PZP directory (decompress only) ─────────────
    if pzp_dir:
        # Match source files to PZP files by stem
        matched = []
        for src in files:
            stem   = os.path.splitext(os.path.basename(src))[0]
            pzp_p  = os.path.join(pzp_dir, stem + ".pzp")
            if os.path.exists(pzp_p):
                matched.append((src, pzp_p))

        if not matched:
            print(col(f"  [warn] No matching .pzp files found in {pzp_dir}", YELLOW))
        else:
            total_pzp = sum(os.path.getsize(p) for _, p in matched)
            ratios    = [os.path.getsize(s) / os.path.getsize(p) for s, p in matched]
            mean_r, min_r, max_r, std_r = ratio_stats(ratios)
            overall_r = total_src / total_pzp

            print(col(f"\n── Pre-existing PZP files: {pzp_dir}", BOLD))
            print(f"   {len(matched)} matched  |  total PZP {fmt_bytes(total_pzp)}")
            print(f"   Compression ratio  overall {overall_r:.3f}×  "
                  f"per-file avg {mean_r:.3f}×  "
                  f"min {min_r:.3f}×  max {max_r:.3f}×  σ {std_r:.3f}")

            hdr = (f"\n  {'TARGET':<18}  {'DECOMP TOTAL':>13}"
                   f"  {'DECOMP/FILE':>12}  {'THROUGHPUT':>14}  PIXELS")
            print(hdr)
            print(f"  {'-'*18}  {'-'*13}  {'-'*12}  {'-'*14}  {'─'*20}")

            with tempfile.TemporaryDirectory(prefix="pzp_bench_") as tmp:
                for label, binary in active_binaries.items():
                    out_ppms = [os.path.join(tmp, f"d_{i}.ppm")
                                for i in range(len(matched))]

                    t0 = time.perf_counter()
                    for (_, pzp_p), out_p in zip(matched, out_ppms):
                        run_binary([binary, "decompress", pzp_p, out_p])
                    total_ms = (time.perf_counter() - t0) * 1e3
                    per_ms   = total_ms / len(matched)
                    tput     = total_pzp / (total_ms / 1e3) / 1e6  # MB/s

                    # Pixel comparison on the random subset
                    n_ok = n_fail = 0
                    max_global = 0
                    for src_p in compare_files:
                        stem  = os.path.splitext(os.path.basename(src_p))[0]
                        out_p = os.path.join(tmp, f"d_{files.index(src_p)}.ppm")
                        if not os.path.exists(out_p):
                            continue
                        try:
                            orig  = load_image(src_p, probe_flag)
                            recon = load_image(out_p, probe_flag)
                            identical, max_diff, _ = compare(orig, recon)
                            if identical:
                                n_ok += 1
                            else:
                                n_fail += 1
                                max_global = max(max_global, max_diff or 0)
                        except Exception:
                            n_fail += 1

                    if n_fail == 0:
                        px = col(f"IDENTICAL ({n_ok}/{min(compare_count,n)})", GREEN)
                    else:
                        px = col(f"{n_fail} DIFF  max_diff={max_global}", YELLOW)

                    print(f"  {label}  {total_ms:>10.0f} ms  "
                          f"{per_ms:>9.2f} ms  "
                          f"{tput:>11.1f} MB/s  {px}")

                    all_rows.append((label, None, per_ms, total_pzp / len(matched),
                                     mean_r, n_fail == 0))

    # ── 2. Compress from scratch + decompress ─────────────────────────────────
    # PZP's binary only reads PNM/PPM.  Convert PNG/JPG sources to temp PPM
    # files first so the compression step always gets a format it understands.
    # We record the raw PPM size as "source" and the original file size
    # separately so both ratios can be reported.

    pnm_exts = {".ppm", ".pnm", ".pgm"}

    def needs_conversion(path):
        return os.path.splitext(path)[1].lower() not in pnm_exts

    print(col(f"\n── Compress from source → PZP  (scratch)", BOLD))

    src_ext = os.path.splitext(files[0])[1].upper().lstrip(".") if files else "SRC"
    hdr = (f"\n  {'TARGET':<18}  {'COMP TOTAL':>11}"
           f"  {'COMP/FILE':>10}  {'DECOMP TOTAL':>13}"
           f"  {'DECOMP/FILE':>12}  {'PZP TOTAL':>10}"
           f"  {'raw/PZP':>8}  {src_ext+'/PZP':>8}  PIXELS")
    print(hdr)
    sep = (f"  {'-'*18}  {'-'*11}  {'-'*10}  {'-'*13}"
           f"  {'-'*12}  {'-'*10}  {'-'*8}  {'-'*8}  {'─'*20}")
    print(sep)

    ratios_scratch = []
    overall_r = 0.0

    with tempfile.TemporaryDirectory(prefix="pzp_bench_") as tmp:
        # Pre-convert any PNG/JPG sources to PPM once (shared across binaries)
        ppm_paths = []
        ppm_sizes = []
        for i, src_p in enumerate(files):
            if needs_conversion(src_p):
                ppm_p = os.path.join(tmp, f"src_{i}.ppm")
                img = cv2.imread(src_p, probe_flag)
                cv2.imwrite(ppm_p, img)
            else:
                ppm_p = src_p  # already PNM/PPM, use directly
            ppm_paths.append(ppm_p)
            ppm_sizes.append(os.path.getsize(ppm_p))

        src_sizes = [os.path.getsize(f) for f in files]  # original format sizes
        total_ppm = sum(ppm_sizes)
        total_src = sum(src_sizes)

        for label, binary in active_binaries.items():
            pzp_paths = [os.path.join(tmp, f"c_{i}.pzp") for i in range(n)]
            out_ppms  = [os.path.join(tmp, f"d_{i}.ppm") for i in range(n)]

            # Compress all (from the PPM form)
            t0 = time.perf_counter()
            ok_compress = []
            for ppm_p, pzp_p in zip(ppm_paths, pzp_paths):
                rc, _ = run_binary([binary, "compress", ppm_p, pzp_p])
                ok_compress.append(rc == 0 and os.path.exists(pzp_p))
            comp_ms = (time.perf_counter() - t0) * 1e3

            pzp_sizes_ok  = [os.path.getsize(p) for p, ok in zip(pzp_paths, ok_compress) if ok]
            ppm_sizes_ok  = [s for s, ok in zip(ppm_sizes, ok_compress) if ok]
            src_sizes_ok  = [s for s, ok in zip(src_sizes, ok_compress) if ok]
            n_ok_c        = sum(ok_compress)
            total_pzp     = sum(pzp_sizes_ok)

            ratios_vs_ppm = [pp / pz for pp, pz in zip(ppm_sizes_ok, pzp_sizes_ok) if pz > 0]
            ratios_vs_src = [ss / pz for ss, pz in zip(src_sizes_ok, pzp_sizes_ok) if pz > 0]
            ratios_scratch = ratios_vs_ppm

            r_ppm = sum(ppm_sizes_ok) / total_pzp if total_pzp else 0
            r_src = sum(src_sizes_ok) / total_pzp if total_pzp else 0
            overall_r = r_ppm

            # Decompress all successfully compressed files
            t0 = time.perf_counter()
            for pzp_p, out_p, ok in zip(pzp_paths, out_ppms, ok_compress):
                if ok:
                    run_binary([binary, "decompress", pzp_p, out_p])
            decomp_ms = (time.perf_counter() - t0) * 1e3

            # Pixel comparison on random subset (compare against original source)
            n_pass = n_diff = 0
            max_global = 0
            for src_p in compare_files:
                idx   = files.index(src_p)
                out_p = out_ppms[idx]
                if not ok_compress[idx] or not os.path.exists(out_p):
                    n_diff += 1; continue
                try:
                    orig  = load_image(src_p, probe_flag)
                    recon = load_image(out_p, probe_flag)
                    identical, max_diff, _ = compare(orig, recon)
                    if identical:
                        n_pass += 1
                    else:
                        n_diff += 1
                        max_global = max(max_global, max_diff or 0)
                except Exception:
                    n_diff += 1

            px = (col(f"IDENTICAL ({n_pass}/{min(compare_count,n)})", GREEN)
                  if n_diff == 0
                  else col(f"{n_diff} DIFF  max={max_global}", YELLOW))

            comp_per   = comp_ms   / n_ok_c if n_ok_c else 0
            decomp_per = decomp_ms / n_ok_c if n_ok_c else 0

            print(f"  {label}  {comp_ms:>8.0f} ms  "
                  f"{comp_per:>7.2f} ms  "
                  f"{decomp_ms:>10.0f} ms  "
                  f"{decomp_per:>9.2f} ms  "
                  f"{fmt_bytes(total_pzp):>10}  "
                  f"{r_ppm:>6.3f}×  "
                  f"{r_src:>6.3f}×  "
                  f"{px}")

            all_rows.append((label, comp_per, decomp_per,
                             total_pzp / n_ok_c if n_ok_c else 0,
                             r_ppm, n_diff == 0))

    # Ratio detail lines (based on last binary run)
    if ratios_scratch:
        mean_r, min_r, max_r, std_r = ratio_stats(ratios_scratch)
        print(f"\n  raw/PZP  (uncompressed PPM → PZP):  "
              f"overall {overall_r:.3f}×  "
              f"avg {mean_r:.3f}×  min {min_r:.3f}×  max {max_r:.3f}×  σ {std_r:.3f}")
    if ratios_vs_src:
        mean_s, min_s, max_s, std_s = ratio_stats(ratios_vs_src)
        print(f"  {src_ext}/PZP  (source format → PZP):        "
              f"overall {r_src:.3f}×  "
              f"avg {mean_s:.3f}×  min {min_s:.3f}×  max {max_s:.3f}×  σ {std_s:.3f}")

    print()
    return all_rows


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="PZP benchmark — timing, compression ratio, correctness")
    parser.add_argument("--source-dir", metavar="DIR",
                        help="Directory of source images to benchmark (directory mode)")
    parser.add_argument("--pzp-dir", metavar="DIR",
                        help="Directory of pre-existing .pzp files to time decompression of")
    parser.add_argument("--max-files", type=int, default=None, metavar="N",
                        help="Process at most N files in directory mode")
    parser.add_argument("--compare", type=int, default=20, metavar="N",
                        help="Number of random files to pixel-compare in directory mode (default 20)")
    parser.add_argument("--no-build", action="store_true",
                        help="Skip make all")
    parser.add_argument("--runs", type=int, default=5, metavar="N",
                        help="Timing repetitions per test in sample mode (default 5)")
    parser.add_argument("--no-debug", action="store_true",
                        help="Skip dpzp (debug build is slow)")
    args = parser.parse_args()

    os.chdir(ROOT)

    if not args.no_build:
        build()

    active = {k: v for k, v in BINARIES.items()
              if not (args.no_debug and "debug" in k)}

    missing = [k for k, p in active.items() if not os.path.exists(p)]
    if missing:
        sys.exit(f"Missing binaries: {missing}\nRun: make all")

    # ── Directory mode ────────────────────────────────────────────────────────
    if args.source_dir:
        if not os.path.isdir(args.source_dir):
            sys.exit(f"Not a directory: {args.source_dir}")

        print(col(f"\nPZP Benchmark  —  directory mode\n", BOLD + CYAN))
        benchmark_directory(
            source_dir    = args.source_dir,
            pzp_dir       = args.pzp_dir,
            active_binaries = active,
            max_files     = args.max_files,
            compare_count = args.compare,
        )
        return

    # ── Sample mode ───────────────────────────────────────────────────────────
    print(col(f"\nPZP Benchmark  —  sample mode  ({args.runs} run(s))\n", BOLD + CYAN))

    all_rows = {}
    for sample_name, (imread_flag, _) in SAMPLES.items():
        if not os.path.exists(os.path.join(SAMPLES_DIR, sample_name)):
            print(f"  [skip] {sample_name} not found\n")
            continue
        all_rows[sample_name] = benchmark_sample(
            sample_name, imread_flag, args.runs, active)

    # Summary
    print(col("=" * 72, BOLD))
    print(col(" SUMMARY — mean times across all samples", BOLD))
    print(col("=" * 72, BOLD))

    totals = {}
    for rows in all_rows.values():
        for (label, cmp_ms, dmp_ms, pzp_sz, ratio, identical) in rows:
            if label not in totals:
                totals[label] = [[], [], [], True]
            totals[label][0].append(cmp_ms)
            totals[label][1].append(dmp_ms)
            totals[label][2].append(ratio)
            totals[label][3] = totals[label][3] and identical

    print(f"\n  {'TARGET':<18}  {'AVG COMPRESS':>13}  {'AVG DECOMPRESS':>14}"
          f"  {'AVG RATIO':>10}  CORRECTNESS")
    print(f"  {'-'*18}  {'-'*13}  {'-'*14}  {'-'*10}  {'─'*15}")

    for label, (cmps, dmps, ratios, all_ok) in totals.items():
        avg_c = sum(cmps)   / len(cmps)   if cmps   else 0
        avg_d = sum(dmps)   / len(dmps)   if dmps   else 0
        avg_r = sum(ratios) / len(ratios) if ratios else 0
        ok    = col("ALL IDENTICAL", GREEN) if all_ok else col("DIFFERENCES", RED)
        print(f"  {label}  {avg_c:>10.1f} ms  {avg_d:>11.1f} ms"
              f"  {avg_r:>8.3f}×  {ok}")

    print()


if __name__ == "__main__":
    main()
