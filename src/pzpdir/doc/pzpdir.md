# PZPDIR

PZPD archives store many files, grouped into **records** (one training sample each), in a few
self-contained **shards** plus a small manifest. The files of a record sit next to each other,
so a record is one read, and every file keeps its original name, a FourCC format and
index-resident metadata (dimensions, channels, bits, line count, frame count).

- Format specification: `doc/pzpd-spec.md` (v0.4) in the repository root.
- Implementation plan: `knowledge/PLAN.md`.
- API: pzpdir.h. Command line tool: pzpdir_cli.c (`pzpdir pack | ls | cat | info | verify | unpack`).

## Building

    make                  # pzpdir, dpzpdir (debug + ASan), spzpdir (SIMD), libpzpdir.so, libpzpdir.a
    make test             # unit tests + CLI tests (ASan + UBSan)
    make fuzz             # truncation / bit-flip fuzzing
    make check-client     # compile with the DataLoader's exact flag sets
    make doc              # this documentation (HTML + PDF)

Dependencies: zstd, lz4, pthreads. xxHash is vendored in `third_party/xxhash.h` (BSD-2).

## Packing a dataset

A dataset-specific script decides which files form a record and writes a record list
(one blob per line, `key<TAB>stream<TAB>source-path[<TAB>stored-name]`):

    python3 scripts/pzpdir_list_from_db.py coco/cocoVal.db coco/cache/coco > val2017.tsv
    pzpdir pack coco_val2017.pzpd val2017.tsv
    pzpdir info coco_val2017.pzpd
