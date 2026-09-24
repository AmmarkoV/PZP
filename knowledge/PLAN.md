# PZPD: implementation plan

Working plan for **PZPD**: `.pzpd` archives, built and inspected with the `pzpdir` tool.
Byte layouts and the full API are in the spec, [`doc/pzpd-spec.md`](../doc/pzpd-spec.md)
(**v0.4, frozen for implementation**; its changelog lists what changed since v0.2). This file covers *what gets built, in which
order, and how each step is verified*.

Status: **phase 1 implemented in `src/pzpdir/` and verified, except the early performance gate:
it passes at 1 thread but not at 8 (§6, phase 1 results). After the DataLoader profile (§6b),
the decision (2026-09-21) is to **continue with the plan in its current order and leave the RGB
size as is** (no pre-scaled RGB stream). All phases (1, 1b, 1c, 6, 7, 2, 3, 5, 4) are implemented (2026-09-22). Still unmeasured: huge pages (needs a `huge=` tmpfs mount, i.e. sudo). DataLoader integration (§7, D22–D26): converter (Part A) done; DataLoader steps 0–5 done and verified on branch `pzpdir`; step 6 (removing the `.db` path) waits until the datasets are converted on the training PC.**
Spec v0.4 + revisions 1–11. A review pass after the phases (§6, "Review fixes") fixed three bugs and the benchmarks.
**First client:** the Y-MAP-Net DataLoader (`RGBToPoseDetect2D/datasets/DataLoader`). Its needs set the order of work (§5). Last updated 2026-09-22.

---

## 1. Why

Y-MAP-Net reads each sample from several places:
- an RGB image plus derived modalities (combined depth+seg, geolocation, or separate
  depth + segmentation), each in its own directory;
- annotations from a `.db` text file (`DB1`: joints, image size, persons with
  bbox + keypoints, description token IDs);
- DINOv2/v3 descriptors from separate binary files (`*.db.dinov2`, `*.db.dinov3`),
  matched to samples by position and basename.

ImageNet (~1.28 M images × 3–5 files) and, later, video frames overload the
filesystem. Every sample also costs 3–5 `open/read/close` calls plus `fadvise`
calls, and there is no real read-ahead.

**Goal:** a "directory in a file" archive that:
- stores a sample's files **next to each other** (one read per sample);
- opens instantly and looks up by position or by name;
- prefetches the known shuffled order in the background;
- is read-heavy / write-rare;
- carries the annotations as **typed tables**, so the `.db` and descriptor files,
  and the code that parses and matches them, go away.

## 2. Decisions (settled)

| # | Topic | Decision |
|---|---|---|
| D1 | Layout | **Co-located records**: one record = one sample, and its files (streams) are contiguous, so a sample is one `pread`. Stream order `rgb, all, geo, depth, seg` makes every real DataLoader read set one contiguous span |
| D2 | Names | Record keys and blob names are **arbitrary bytes** (≤ 65 535 B, no NUL). Every original filename is kept and indexed |
| D3 | Sharding | Self-contained **~4 GB shards** plus a rebuildable manifest. Any shard is recoverable alone (backup superblock, per-record headers, `salvage`) |
| D4 | Grouping and order | **Decided by the writing application**, not the archive. A dataset-specific script says "record K = these files (+ these table rows)" through the Writer API or a record list piped to `pzpdir pack`. A record's files may come from **any directories**. Record order = write order. For `.db`-backed datasets that is the **`.db` sample order**, so ordinal *i* = today's sample number *i*. The archive has no stem, regex or pairing rules |
| D5 | Prefetch | **Raw bytes only**, one record span per I/O. Modes: MAP (RAM disk: mmap views + page-table pre-fault), PAGECACHE (`fadvise`), BUFFERS (slab arena + `O_DIRECT`), AUTO (default, chosen per shard) |
| D6 | Checksums and hashing | **Vendored xxHash** (`third_party/xxhash.h`, BSD-2, single header): XXH32 for blobs and record headers, XXH64 for index sections, superblocks and key hashes. No new system dependency. Checksums are verified only on request or by `verify` |
| D7 | Platform | **Linux only** |
| D8 | Video | Groups = consecutive records, range reads; a group never spans shards |
| D9 | v1 scope | Format, reader/writer, CLI, collections, annotation tables, recovery, stream and table edits, video groups, Python bindings, all prefetch modes. **Not:** appending records. The DataLoader integration (first client) is done in the DataLoader's own repo and branch, starting after phase 6 (§5) |
| D10 | Naming and packaging | All sources in **`src/pzpdir/`** (own Makefile; root Makefile delegates: `make pzpdir`, `test-pzpdir`, …). Tool `pzpdir` / `dpzpdir` / `spzpdir` (from `pzpdir_cli.c`). Library: **`pzpdir.h` (declarations only) + the `pzpdir_*.c` files** (implementation; shared internals in `pzpdir_internal.h`, hidden), built as `libpzpdir.so` and `libpzpdir.a`; consumers include the header and link the library (changed 2026-09-23 from one `pzpdir.c` translation unit made of `#include`d `.inc.c` parts). Python `pzp.pzpdir`; files `.pzpd`; C prefix `pzpd_`. Existing `pzp` / `libpzp.so` are **untouched** |
| D11 | Alignment | Per **record** (default 4096, so `O_DIRECT` works); blobs inside a record at 64 B |
| D12 | Required streams | **None.** A record holds whatever the writer lists for it; any stream may be missing |
| D13 | Multiple archives | Opened **as one** (`pzpd_open_many` or a collection file). Ordinals are concatenated, streams and tables unioned by name, duplicate keys kept; `find` = first member, `find_in` / `find_all` to disambiguate; the member is always a separate argument, never parsed from the key |
| D14 | Identification and metadata | Every PZPDIR file starts with an 8-byte magic (`PZPDSHRD` / `PZPDMANI` / `PZPDCOLL`; `PZPDRECD` / `PZPDSECT` inside shards) + version, and unknown versions are refused. Every blob has a **FourCC format** (`JPEG`, `PNG `, `PZP `, `PZPC`, `PNM `, `PFM `, `NPY `, `JSON`, `TEXT`, …) detected from its magic bytes, plus **index-resident metadata**: width, height, channels, bits, line count for text, frame count for PZP containers. No data I/O is needed to read it |
| D15 | Storage awareness | Storage detected per shard (`statfs`: tmpfs/ramfs → RAM, else BLOCK). AUTO: RAM → **MAP** (zero-copy views, `MADV_POPULATE_READ` ahead, optional huge pages via `PZPD_O_HUGEPAGE`, `PZPD_O_POPULATE` to pre-fault whole shards); BLOCK → PAGECACHE or BUFFERS. Collection member paths may be **absolute**, so RAM-disk and NVMe members mix in one collection |
| D16 | Annotations | **Typed tables** in the archive. *Record tables* hold 0..n rows per record; *global tables* hold rows for the whole archive. They are stored in per-shard table sections next to the index, **not** scattered through the records: bulk-loaded at startup, zero parsing, zero-copy pointers. They are written and inspected as **CSV**. Tables: `joints` (global: `name:str, parent:u16`), `image` (`width:u16, height:u16`), `persons` (`id:u16, bbox:u16[4], kp:u16[3J]`, **all coordinates packed u16**), `descriptions` (`source:str, text:str`, stored as **text**; token IDs are computed at load because they depend on the whole dataset mix; a pinned `index_to_word.json` is supported for existing checkpoints), `descriptor_<model>` (`v:f32[D]`, `bulk`, **D from the schema, never hardcoded**). SuperPoint stays a separate file (too large). Table edits append sections and flip the superblock generation, so no image data is rewritten |
| D17 | Name slots | Stream and table names ≤ **23 bytes** (fixed superblock slots). Keys and blob names stay unlimited |
| D18 | Annotation backup | **Kept:** non-bulk table rows (persons, image, descriptions) are also copied into each `PZPDRECD` record header (~0.4 KB/record), never read normally, so `salvage` recovers annotations if a table section is lost |
| D19 | Collection API extras | Adopted: `pzpd_collection_write()` / `pzpd_collection_refresh()` (C API for `pzpdir collect`), and `pzpd_find_all()` returns the **total** match count even beyond the buffer. **Not** adopted: `pzpd_member_has_stream()` |
| D20 | Coding and documentation style | Follow **PThreadWorkerPool**, **SharedMemoryVideoBuffers** and this **PZP** repo: Doxygen-ready comments on every public item, a `doc/doxyfile` + `scripts/refreshDoxygen.sh` that builds HTML + PDF, and PZP's `pzp_`-style snake_case prefix (`pzpd_`). Details in §4. PThreadWorkerPool is a **style** reference only: its batch kick/wait model doesn't fit the prefetcher's continuous queue, which gets its own small pthread queue in the same style |
| D21 | First client | The **DataLoader** (`RGBToPoseDetect2D/datasets/DataLoader`) is the first consumer. It **vendors** the pzpdir sources into `pzpdir/`, builds them into its own `pzpdir/libpzpdir.so` and links `libDataLoader.so` (and its test executables) against it with `DT_RPATH $ORIGIN/pzpdir` (decided 2026-09-23; before, it compiled `pzpdir.c` into `libDataLoader.so`). The work order follows its critical path (§5): format → collections → tables → prefetcher, then its integration branch can start, while recovery, edits, video and Python bindings follow |
| D22 | DataLoader transition (2026-09-22) | **Full switch to `.pzpd`.** No mixed `.db` / archive sources in one DB instance. The `.db` + directory read path is kept only until the §7 equivalence gates pass (it is what they compare against), then removed |
| D23 | Descriptors in DataLoader archives | **L2-normalised at conversion** (as the DataLoader's `L2_NORMALIZE_DESCRIPTORS` does at load today), so `descriptor_<model>` stays zero-copy. A global `descriptor_info` table (`model:str, dim:u32, l2norm:u8`) records it; the loader aborts if it disagrees with its build. **Only DINOv3 is converted** (`descriptor_dinov3`); `.dinov2` files are ignored. A source without a `.dinov3` file (e.g. BG-20k, AM-2k, 300w, generated) gets no descriptor table, and its samples get an empty vector, as today |
| D24 | Captions | **Every sample has a caption**; the converter aborts on a sample without one. **Only the current caption file** (DeepSeek-VL2) is converted, one `descriptions` row per sample; `descriptionsOLD.json` and other older caption sets are not. Caption files are not always `<images>/descriptions.json` (e.g. `coco/train2017DeepSeekVL2descriptions.json`, `300w/indoorDeepSeekVL2descriptions.json`), so the converter takes them from `--descriptions`, else `<images>/descriptions.json`, else `<db dir>/<images dir name>DeepSeekVL2descriptions.json` (e.g. `train2017`, `indoor`). **The format carries only the caption strings**: no token IDs, no vocabulary. Turning strings into tokens is the DataLoader's own dynamic step and does not depend on PZPD. The `.db` token IDs are not migrated |
| D25 | Geolocation and SuperPoint | **Not in DataLoader archives.** The converter writes no `geo` stream and no SuperPoint data; with archive sources the DataLoader rejects `addGeolocation` / `addSuperpoint`. Stream order: `rgb, all, depth, seg` |
| D26 | Converter scope | This work delivers the **conversion code** (`datasets/convertToPZPD.py` + `checkPZPD.py` in RGBToPoseDetect2D). Running it over the real datasets happens on the PC that holds them; here the code is tested on COCO val2017 and small fixtures |

## 3. Architecture at a glance

```
writer:   dataset-specific script decides grouping, keys, order and table rows
            → Writer API (begin / blob_file / rows / end)  or  record list | pzpdir pack out.pzpd -
            → format + metadata detected per blob from magic bytes / headers (or given via blob_ex)

training.pzpd  "PZPDCOLL"  collection: coco_train, imagenet, bg20k, …           ← optional
   └─ coco_train.pzpd  "PZPDMANI"  shard table + global name hash + global tables ← rebuildable
        ├─ coco_train.00000.pzpd  "PZPDSHRD"  shard ≤ 4 GB, standalone
        │    [superblock]
        │    [record][record]…    record = ["PZPDRECD" header: key, per-blob name/stream/format/
        │                                   meta/size/xxh32, copy of non-bulk table rows]
        │                                  [rgb][all][geo][depth][seg]          (4 KiB aligned)
        │    [record table]       32 B per record
        │    [blob table]         32 B per blob: offset, size, name, FourCC, w, h, channels, bits, frames|lines
        │    [hash][names][groups]
        │    [table sections]     "PZPDSECT": schema + CSR row index + fixed-width rows + string heap
        │                         image, persons (u16), descriptions (text), descriptor_<model> (f32[D], bulk);
        │                         global: joints
        │    [backup superblock]
        └─ coco_train.00001.pzpd …

reader:   pzpd_open / pzpd_open_many → find / blob_info (index only) / read_record (1 pread) / view (mmap)
tables:   pzpd_table_rows / pzpd_global_rows / pzpd_table_shard_view → pointers into mmap
prefetch: submit(epoch order + per-sample stream masks) → I/O threads → get / release / discard
          AUTO per shard: RAM disk → MAP | NVMe → PAGECACHE (fits in RAM) or BUFFERS + O_DIRECT
```

Index overhead for 1.28 M ImageNet records × 3 streams is ≈ 325 MB (≈ 0.2 % of the data),
plus ≤ 4 KB of alignment padding per record. Annotation tables come on top: the
`.db` data is ~0.1–0.4 KB per record, and a DINOv2 descriptor is 3 KB per record (≈ 3.9 GB for ImageNet).

## 4. Coding and documentation style

References: `~/Documents/Programming/PThreadWorkerPool/pthreadWorkerPool.h`,
`~/Documents/Programming/SharedMemoryVideoBuffers/src/c/sharedMemoryVideoBuffers.{h,c}`
(+ its `doc/doxyfile`, `scripts/refreshDoxygen.sh`), and `pzp.h` / `pzp_lib.c` / `PZP.py` here.

**File headers**
- The GPLv3 notice block as in `pzp.h` (same license as this repo), then a Doxygen file block:
  ```c
  /** @file pzpdir.h
   *  @brief  PZPD archives: many files (and annotation tables) in a few self-contained shards.
   *
   *  Repository : https://github.com/AmmarkoV/PZP
   *  @author Ammar Qammaz (AmmarkoV)
   *
   *  @section pzpd_overview Overview
   *  ... archive / shard / manifest / collection / record / stream / table ...
   *
   *  @section pzpd_reading Reading a sample
   *  @code
   *  pzpd *a = pzpd_open("coco_val2017.pzpd", 0);
   *  ...
   *  @endcode
   *
   *  @section pzpd_threads Threads
   *  ... which calls are thread-safe, prefetcher thread model ...
   *
   *  @section pzpd_environment Environment variables
   *  ...
   */
  ```
- `pzpdir.h` has `@section` overviews with `@code` examples (open / read / tables / prefetch / write), like `sharedMemoryVideoBuffers.h`.
- `pzpdir_internal.h`'s `@file` block (and each `pzpdir_*.c`'s) documents the **internal** data and helpers only ("The public functions are documented in the header; this file documents …").

**Declarations**
- Include guard `PZPDIR_H_INCLUDED`, and `extern "C"` blocks as in all three references.
- Version constant with an inline changelog comment, as in PThreadWorkerPool:
  `static const char pzpdirVersion[]="0.1"; //0.1: first implementation of spec v0.4`
- Every macro: `/** @brief … */`, explaining the *why* and the units, e.g. `PZPD_MAGIC_SHARD`, `PZPD_FORMAT_VERSION`, `PZPD_MAX_STREAMS`. Magic/version pairs are documented like `SHMVB_CONTEXT_MAGIC` / `SHMVB_CONTEXT_VERSION` ("bump when a layout changes, so readers refuse instead of misreading").
- Every struct: `/** @brief … */` with a paragraph on invariants. **Every field** gets a trailing `///<` comment (units, sentinel values, who writes it). On-disk structs state "little-endian, packed, no pointers".
- Every public function: `@brief`, one `@param` per parameter, and `@return` stating the exact success and failure values. Add `@note` / `@warning` for threading and lifetime (e.g. "the pointer stays valid until `pzpd_close()`"), and `@see` for companion calls (`get` ↔ `release`).

**Conventions**
- Names: `pzpd_` + snake_case for functions and types, `PZPD_` + UPPER_CASE for macros and enums (this repo's `pzp_` convention; the reference repos' camelCase is not adopted, to stay consistent inside PZP).
- Return values:
  - `int` functions: 1 = success, 0 = failure;
  - lookups return an index, or -1 for "not found";
  - sizes are `ssize_t`, with < 0 meaning error;
  - pointer returns: NULL on failure.

  Every failure sets `pzpd_last_error()`. Never `exit()` / `abort()` (commit `6652a16`).
- Guard clauses first (`if (a==0) { return 0; }`), then the body. `//----------` separators between logical blocks. Comments explain *why* (races, crash windows, layout choices), not what.
- Debug output behind compile-time switches (`PZPDIR_DEBUG 0`, `PZPD_VERBOSE`), with the `RED` / `GREEN` / `NORMAL` ANSI macros (each `///<`-documented).
- Atomics only through small documented helper wrappers, as `threadpoolAtomic*` does in PThreadWorkerPool.
- Python (`pzp.pzpdir`): module and function docstrings in PZP.py's numpy style (`Parameters` / `Returns` sections).

**Doxygen tooling** (phase 1)
- `doc/doxyfile` modelled on SharedMemoryVideoBuffers':
  - `OPTIMIZE_OUTPUT_FOR_C = YES`, `EXTRACT_ALL = YES`, `EXTRACT_STATIC = NO`, `WARN_IF_UNDOCUMENTED = YES`, `JAVADOC_AUTOBRIEF = NO`;
  - input/output from `DOXYGEN_INPUT` / `DOXYGEN_OUTPUT`;
  - HTML + LaTeX, and `USE_MDFILE_AS_MAINPAGE` pointing at a short `doc/pzpdir.md`.
- `scripts/refreshDoxygen.sh`: cleans `doc/html` and `doc/latex`, runs doxygen, builds the PDF (`PZPDIR.pdf`). Generated output is not committed.
- **Gate:** doxygen over `pzpdir.h`, `pzpdir_internal.h`, the `pzpdir_*.c` files and `pzpdir_cli.c` produces **zero warnings**, i.e. no undocumented public items (internal functions shared between files are `PZPD_INTERNAL`, which doxygen reads as `static`, so they are exempt as before).

## 5. First client: the DataLoader

The DataLoader (`RGBToPoseDetect2D/datasets/DataLoader`) is where `pzpdir` is used
first. Facts that constrain the library, taken from its build and code:

| Fact | Consequence for `pzpdir` |
|---|---|
| Built by **one gcc command** listing every `.c` (`Makefile`, `makeLibrary.sh`) into `libDataLoader.so`, loaded by `DataLoader.py` via ctypes | Integration = **vendor** `pzpdir.h`, `pzpdir_internal.h`, the `pzpdir_*.c` files, `pzpdir_unicode.h`, `third_party/xxhash.h` into `DataLoader/pzpdir/`; `makeLibrary.sh` / `Makefile` build `pzpdir/libpzpdir.so` first and link `-Lpzpdir -lpzpdir -Wl,-rpath,$ORIGIN/pzpdir -Wl,--disable-new-dtags` (DT_RPATH: the vendored copy wins over `LD_LIBRARY_PATH`, which on this machine starts with an empty entry, i.e. the current directory). No install step (2026-09-23; was: `pzpdir.c` compiled into `libDataLoader.so`) |
| Flags: `-D_GNU_SOURCE -O3 -fPIC -march=native -mtune=native -pthread -Wno-unused-function`, optional `-DINTEL_OPTIMIZATIONS -mavx2`; debug build with `-O0 -g3 -fsanitize=address -pg`; links `-lm -lpng -ljpeg -lzstd -llz4` | The `pzpdir_*.c` files compile **warning-free** under exactly these flag sets (release, ASan, `-pg`) and needs **no extra libraries** beyond these (xxHash is vendored, pthreads already linked) |
| Vendors its **own, older `codecs/pzp.h`** (differs from this repo's) | pzpdir must not require a specific `pzp.h` version. The PZP header probe and `pzpd_read_pzp()` are behind `PZPDIR_WITH_PZP` (default 1); the DataLoader builds with it **0**, since it decodes through its own `codecs/` and only needs raw bytes. The FourCC `PZP ` metadata is written at pack time, so readers don't need `pzp.h` |
| `pzp.h` keeps a **per-TU `static` thread-local ZSTD context**; `PrepareBatch.c` calls `pzp_thread_cleanup()` only for its own TU | `pzpdir` never decodes on the DataLoader's worker threads; with `PZPDIR_WITH_PZP 0` there's no second context to leak |
| Aborts on data problems "to protect training consistency"; never wants silent fallbacks | `pzpdir` reports every problem (`pzpd_last_error()`); the **adapter** decides to abort. Validation that needs only the index (streams present, dims / channels / bits, schema, joint count) runs at DB load |
| Its own worker pool (vendored `pthreadWorkerPool.h`) decodes; batches are strided across T workers; Python double-buffers k+1 | The prefetcher is called from those workers: `get` / `release` from any thread, in any order within the window. Its own I/O threads are separate from the decode pool |
| Train and validation are **separate `ImageDatabase` instances** in one Python process | Several `pzpd *` handles and prefetchers per process are normal. Each has its own state, while the mmapped pages are shared by the kernel. No global mutable state in the library besides the thread-local `pzpd_last_error()` |
| `db_create`'s ctypes signature is kept stable; new options go through setter globals (e.g. `db_set_embeddings_path`) | Integration takes the `.pzpd` path through the **existing DB-path argument** (detected by magic, not by extension), and new knobs (prefetch mode / budget, description source, pinned vocabulary) through new `db_set_*` setters |
| Descriptors already have a dynamic length (`value_count`, cap `MAX_DESCRIPTOR_LENGTH` = 4096) | `descriptor_<model>` D from the schema fits; the adapter rejects D > 4096 at load |
| `db_destroy` / `db_pipeline_drain` / the shuffles define the lifecycle | Prefetcher lifecycle: create after DB load; `clear` + `submit` after each shuffle (already drained); `destroy` before `pzpd_close()` in `db_destroy` |

**Order of work (critical path of the first client):**

1. **Phase 1**: format, writer, reader, CLI, early performance gate. `pzpdir pack` can then build archives from `.db` datasets.
2. **Phase 1b**: collections. One collection = the DataLoader's list of dataset sources.
3. **Phase 1c**: tables. `PoseDatabase` can be built without the `.db`.
4. **Phase 6**: prefetcher PAGECACHE + MAP + AUTO. **→ DataLoader integration can start here**, on COCO (NVMe or RAM disk), in its own repo/branch.
5. **Phase 7**: BUFFERS + O_DIRECT, needed once ImageNet (larger than RAM) is added.
6. **Phase 2** (recovery), **phase 3** (stream / table edits), **phase 5** (Python bindings: the writer scripts use the record list + CLI, so the DataLoader doesn't need them), **phase 4** (video groups).

Phase numbers stay as they are, and this list defines the order.

**Client-facing checks, from phase 1 on:**
- `scripts/check_client_build.sh`: compiles every `pzpdir_*.c` with the DataLoader's exact flag sets (release, AVX2, ASan, `-pg`) and `PZPDIR_WITH_PZP=0` with zero warnings, builds `libpzpdir.so` (exports `pzpd_*` only), links the DataLoader's sources against it with `--no-undefined`, and loads the result with ctypes to check the vendored `libpzpdir.so` is the one found.
- `scripts/client_smoke.c`: a tiny program written the way the DataLoader adapter will be. It opens a collection, builds a `PoseDatabase`-like array from the tables, and reads records from several threads with `get` / `release`. Run under ASan.
- The `dataloader-replay` benchmark (§8) is the acceptance test for phases 6 and 7.

## 6. Phases

Each phase ends with its checks passing. Nothing is committed without an explicit request.

### Phase 1: core format, writer, reader, CLI basics (implemented, `src/pzpdir/`)
Paths below are relative to `src/pzpdir/`.
- [x] `pzpdir.h` (API: types, constants, declarations) + `pzpdir.c` (implementation): superblocks (primary + backup), record header, record / blob tables, hash table, string heap, 32-byte `PZPDSECT` section headers
- [x] Vendor `third_party/xxhash.h` (0.8.2, BSD-2 license in the header; used with `XXH_INLINE_ALL`, so no symbols are exported)
- [x] Writer: `begin` / `blob` / `blob_file` / `blob_ex` / `end` / `finish` / `abort`; sharding at `shard_max_bytes`; `.tmp` + fsync + rename; archive-wide duplicate check
- [x] Format detection + header-only metadata probes (`pzpd_detect_format`): JPEG (SOF), PNG (IHDR), PZP (partial zstd / lz4 decompress of the header), PZP container, PNM, PFM, NPY, JSON / TEXT (CSV / TSV by extension), RAW fallback
- [x] File magics + version check on open; `pzpd_blob_info_get`, `pzpd_format_name`
- [x] Reader: open manifest or single shard, lazy shard open, backup-superblock fallback, `find`, `record_key`, `blob_info_get`, `read_into`, `read_alloc`, `view`, `read_record`, `record_span`, `verify_record`, `verify_shard`, `read_pzp`
- [x] CLI (`pzpdir_cli.c`): `pack <out> <record-list|->`, `pack <out> <dir>`, `ls [--long] [--names]`, `cat`, `info [--stream S]`, `verify [--blobs]`, `unpack`
- [x] Example writer: `scripts/pzpdir_list_from_db.py` (sample list and order from `cocoVal.db`, key = `imagePath`, templates per stream)
- [x] Makefile targets (`src/pzpdir/Makefile`): `pzpdir`, `dpzpdir` (ASan + UBSan), `spzpdir`, `libpzpdir.so`, `libpzpdir.a`, `test`, `fuzz`, `bench`, `doc`, `check-client`; root Makefile delegates
- [x] Doxygen tooling (§4): `doc/doxyfile` (with `EXTRACT_ALL = NO` so undocumented items warn), `doc/unicodeCharacters.sty`, `scripts/refreshDoxygen.sh`, `doc/pzpdir.md` main page
- [x] `PZPDIR_WITH_PZP` compile switch (default 1); `scripts/check_client_build.sh`
- [x] Tests: `tests/test_pzpdir.c`, `tests/run_cli_tests.sh`, `tests/fuzz_pzpdir.c`, `tests/check_metadata.py`
- [x] Early performance benchmark: `scripts/bench_pzpdir_early.c`
- [ ] Optional: `data/pzpd.xml` MIME type and a `file(1)` magic snippet (not done)

**Verify** (✅ = passed on 2026-09-21):
- ✅ 100 k synthetic records × 3 streams with adversarial names round-trip byte-identical across 7 shards (2 086 748 checks, ASan + UBSan). The names cover spaces, unicode, many dots, tabs / newlines, 1 B and 65 535 B names, 30-deep paths, the same stem with different extensions, and missing streams
- ✅ Duplicate keys / names, a repeated stream, empty or NUL-containing keys, records without blobs, bad streams / alignments are rejected; list escapes round-trip; `unpack` refuses `../x`, `/abs`, `a//b`, `./x`
- ✅ COCO val2017, 5 streams (4.8 GB, 2 shards): `verify --blobs` OK; `unpack` → `diff -r` identical for all 25 000 files (byte-for-byte, stronger than the planned SHA-256 check)
- ✅ Metadata of all 25 000 COCO blobs equals full decodes (PIL for JPEG / PNG, `PZP.py` for PZP / PZPC); text line counts checked in unit tests; each file kind recognised by its magic; a newer format version is refused with `PZPD_E_VERSION`
- ✅ `blob_ex` metadata override (`USER_META`); unknown content → `RAW ` with `META_VALID` clear; the extension only names the format
- ✅ Open + find + read on 1 M records: **0.19 ms** (target < 5 ms)
- ✅ `scripts/refreshDoxygen.sh`: **0 doxygen warnings**, HTML + `PZPDIR.pdf`
- ✅ `scripts/check_client_build.sh`: zero warnings with `-Werror` under the DataLoader's release, AVX2, ASan and `-pg` flags; links together with the DataLoader's own sources into one `.so` with `--no-undefined`; no xxHash / zstd / lz4 symbols exported
- ✅ Fuzzing: 3 000 truncation / bit-flip iterations over shards and the manifest, 0 crashes, 0 ASan / UBSan reports (UBSan with `halt_on_error=1`)
- ⚠️ **Early performance gate: not met at T = 8**, see the results below

**Phase 1 results: early performance gate** (COCO val2017, rgb+all+geo = ~650 KB per sample, 5 000
samples, shuffled, cold cache verified at 0.00 % resident with `mincore`; source files on the
`/home` partition, archive on the `/` partition of the same Samsung 980 PRO)

| Threads | fs-open samples/s | pzpd-record samples/s | ratio | pzpd-view samples/s |
|---|---|---|---|---|
| 1  | 1 051 | 1 614 | **1.54×** | 1 517 |
| 4  | 3 432 | 4 372 | 1.27× | 4 458 |
| 8  | 5 513 | 5 916 | **1.07×** | 5 588 |
| 16 | 7 331 | 6 555 | 0.89× | 6 153 |

- **Syscalls per sample:** fs-open ≈ 12 (3 × open / fstat / read / read / close, 0.51 s of syscall time
  for 500 samples); pzpd-record 1 (one `pread`, 0.08 s); pzpd-view 0 (page faults instead).
- **CPU per sample (cold):** about the same (360–580 µs; page-cache copies dominate). Warm, T=1: 60 vs 75 µs.
- **Reading:** the archive wins when a thread waits on latency and system calls. At T ≥ 8 both approaches
  saturate the drive's buffered-read bandwidth (~4–4.7 GB/s) and move the same bytes, so one read per
  sample buys little, and at T = 16 the three smaller per-file reads keep more requests in flight.
- **Not measured yet:** `posix_fadvise(DONTNEED)` evicts file data but **not** the dentry / inode caches,
  so fs-open's `open()` path lookups were always cached. At ImageNet scale (millions of files) that
  metadata cost grows; measuring it needs `drop_caches` (root).
- **Learned for the prefetcher:** pages a process has mmapped can't be evicted with `DONTNEED`; the
  benchmark must close the archive before a cold run (fixed). MAP mode will hold pages the same way.

### Phase 1b: collections (several archives as one) (implemented, 2026-09-21)
- [x] `pzpd_open_many`, collection file (`PZPDCOLL`), `pzpd_collection_write` / `pzpd_collection_refresh`, CLI `pzpdir collect [alias=]a.pzpd ... [--absolute]` / `collect --refresh`, `info --dups`
- [x] Member paths relative to the collection file (when both are under the same top-level directory) or absolute (e.g. a `/dev/shm` member, or `--absolute`)
- [x] Ordinal concatenation, stream union by name (first-seen order), `member_of` / `member_range` / `member_id` / `member_alias` / `member_count`, `find_in`, `find_all` (returns the total count); `pzpd_find` searches record keys in all members before blob names
- [x] Missing member → reserved range + `PZPD_E_MEMBER_MISSING`; stale (other uuid, record count or streams) → open fails with `PZPD_E_STALE_COLLECTION`, message suggests `collect --refresh`; nested collections rejected; duplicate aliases rejected
- [x] Every read command accepts one archive, a collection, or several archives; with several members `ls` prefixes `alias<TAB>` and `unpack` writes `<dir>/<alias>/`
- [x] Internals: the phase-1 reader became `struct pzpd_archive` + static `arch_*()`; the public handle routes ordinals / streams to members (a single archive is a 1-member collection, so there's one code path)

**Verify** (✅ = passed):
- ✅ coco_val2017 (5 streams, 5 000 records) + a synthetic archive sharing 20 of its keys with another stream (`extra`): count 5 030, 6 merged streams, `info --dups` lists the 20 shared keys, `verify --blobs` OK through the collection, COCO RGB read via the collection byte-identical, `extra` of a COCO record reads as missing
- ✅ Unit tests (127 new checks): every ordinal of a 2-member set reads the same bytes as its member (`read_into`, `view`, `read_record`); `find` = first member, `find_all` = both, `max` = 1 still reports 2; a stream a member lacks reads as missing; shard / member mapping; `verify_shard` / `verify_record` through the set
- ✅ Deleting a member leaves the other ordinals unchanged, its range fails with `PZPD_E_MEMBER_MISSING`; `open_many` fails on a missing member unless `PZPD_O_ALLOW_MISSING` (then 0 records)
- ✅ Rebuilt member → `PZPD_E_STALE_COLLECTION`; `pzpd_collection_refresh` / `collect --refresh` fixes it
- ✅ A collection with one member on `/dev/shm` (stored absolute) and one on NVMe (stored relative) reads correctly
- ✅ CLI tests: 12 new (collect, alias-prefixed ls, unpack into alias dirs, --dups, multi-archive ls / cat / verify, stale + refresh, missing member)
- ✅ Fuzzing now also damages the collection file: 3 000 iterations, 0 crashes, 0 ASan / UBSan reports; 0 doxygen warnings; client build check passes

### Phase 1c: tables (annotations) (implemented, 2026-09-22)
- [x] Schema parser (`name:type[count]`: `u8…u64`, `i8…i64`, `f32`, `f64`, `str`, arrays), C-struct row layout, ≤ 16 tables, names ≤ 23 B
- [x] Record tables (CSR row index per shard) and global tables (copied into every shard + manifest); `bulk` flag; non-bulk rows also copied into `PZPDRECD` record headers for `salvage`
- [x] Writer: `pzpd_writer_table`, `_rows` (with a strings buffer for `str` columns), `_rows_csv`, `_global_rows`, `_global_rows_csv`; record list directives `@table`, `@global`, `@row` and `key<TAB>table<TAB>csv-row` lines (RFC 4180 quoting for `str`)
- [x] Reader: `pzpd_table_id` / `_schema` / `_rows` / `_str` / `_shard_view` / `_csv`, `pzpd_global_rows` / `_str` / `_csv` (per member); table union and schema check (same name ⇒ same schema) across collection members
- [x] CLI: `cat --table`, `export-table` (output is re-packable record-list lines: `key<TAB>table<TAB>csv`, `@row table csv`), table schemas + row counts in `info`
- [x] Example writer: `scripts/pzpdir_list_from_db.py` reads:
  - `cocoVal.db` → `joints`, `image`, `persons`. Its token lines are **not** migrated.
  - `val2017/descriptions.json` (source `deepseekvl2`) + `descriptionsOLD.json` (source `old`) → `descriptions`
  - `cocoVal.db.dinov2` / `cocoVal.db.dinov3` → `descriptor_dinov2` / `descriptor_dinov3` (12-byte header, or the legacy 8-byte one with D inferred)
- [x] Row-index validation is lazy: O(1) at open (first / last entries), per lookup (`end ≥ start`, `end ≤ rows`), full monotonic check once per shard in `pzpd_table_shard_view` (checking everything at open cost 4.6 ms → now 0.23 ms)

**Verify** on COCO val2017 (✅ = passed; `tests/check_tables_coco.py`, archive `cocot.pzpd`, index 754 KB vs 732 KB without tables):
- ✅ joints header, image and persons lines regenerated from the tables are **byte-identical** to `cocoVal.db`: 21 041 lines, 0 differ (the 5 000 token lines are skipped, as planned). Tables: joints 17 rows, image 5 000, persons 11 004 (112 B/row, `kp:u16[51]`), descriptions 10 000, two 768-D descriptor tables (3 072 B/row, bulk)
- ✅ caption texts read back are byte-identical to `descriptions.json` and `descriptionsOLD.json` (5 000 + 5 000, 0 differ); tokenization of the archived texts with `descriptionsVL2/index_to_word.json` reproduces `val2017/vocabulary.json` IDs for all 5 000 samples **with the legacy tokenizer** (`--legacy-tokenizer`: keeps non-ASCII words). With today's `buildVocabulary.py` (ASCII filter) 7 samples differ, only by non-ASCII words: `vocabulary.json` predates the filter. See §9 (tokenizer parity)
- ✅ every descriptor is bit-identical to `cocoVal.db.dinov2` / `.dinov3` (5 000 + 5 000, 0 missing). D = 768 and D = 1024 tables coexist and read back with their own D (unit test)
- ✅ malformed or out-of-range CSV rows are rejected with line numbers (unit + CLI tests); f32 CSV round-trips bit-exactly (shortest text, incl. -0, denormals, 3.4e38)
- ✅ bulk-loading 1 M persons rows over 1 M synthetic records: 0.029 s (< 1 s); open + find + read with a persons table: 0.21 ms
- ✅ `scripts/client_smoke.c` (`make client_smoke`) builds a `PoseDatabase`-like array from a collection (COCO tables archive + a synthetic member without tables) using only table APIs: 5 030 samples, 11 004 persons, 17 joints, 5 000 descriptors, 9 ms; then 8 threads read all 25 030 blobs (4.8 GB) with `read_record`, verify every 16th record's payloads and re-check every sample's tables. ASan / UBSan clean, TSan clean (`setarch -R`: TSan needs ASLR off on this kernel)
- ✅ Unit tests 2 086 933 checks, 0 failures; CLI tests: 13 new (6 list errors, 7 table round trips incl. `export-table` → `pack` → `export-table` identical for 4 tables with hostile strings); fuzzing with tables (global, record, `str`, bulk) in the archive: 3 000 iterations, 0 crashes, 0 ASan / UBSan reports; 0 doxygen warnings; client build check passes

### Phase 2: recovery (implemented 2026-09-22)
- [x] Backup-superblock fallback (step 1) and **index discovery by section magic** (step 2): both superblocks bad → scan 4 KiB boundaries backwards from EOF for `PZPDSECT` headers with a matching XXH64, stop at the record table (so an archive stored as a blob is never taken for the index), and rebuild the superblock from the sections + the metadata JSON (which now also records `first_ordinal`, `align`, `generation`; older shards take `first_ordinal` from the manifest). Automatic on open; `pzpd_shard_info.recovery` = 0 / 1 / 2, shown by `info`. A file without a known magic at offset 0 is tried as a shard too
- [x] `pzpd_salvage()` / `pzpdir salvage <shard> <dir> [--schemas <archive>] [--list out.tsv]` (step 3): record-header scan at 64-byte steps; a header counts only if its XXH32 matches, and after one the scan jumps over the record's payloads. Each blob comes with name, stream, format, metadata and an `intact` flag (payload XXH32); non-bulk table rows come from the header copies (CSV when a schema source is given). The CLI writes the intact blobs under `<dir>` (unsafe names under a sanitised path, the original name kept) and a record list — table declarations, global rows, blob and row lines — that `pzpdir pack` turns back into an archive
- [x] `pzpd_manifest_rebuild()` / `pzpdir rebuild-manifest <name>.*.pzpd [--out]`: shards read on their own (the ladder applies), checked for one uuid, a complete contiguous set, equal streams and schemas, and the manifest's directory; the manifest writer is now shared with `pzpd_writer_finish()`, so the result is byte-identical
- Fixed on the way (found by the extended fuzzer): a table section whose header disagreed with its directory slot (e.g. a flipped global flag) was accepted and could make a record lookup read a NULL row index; the directory and the section must now agree, and lookups check for the index
- Spec: revision 7

**Verify** (✅ passed):
- ✅ Delete the manifest → the rebuilt manifest is **byte-identical** (unit test with shards given in reverse order; CLI test; COCO `cocot.pzpd`, 31 ms). Missing shard, shards of two archives, a manifest in another directory: rejected
- ✅ Delete shard k → every other ordinal reads, all of k's report `PZPD_E_SHARD_MISSING`
- ✅ Damaged primary superblock → backup (recovery 1); both superblocks zeroed → rebuilt from the sections (recovery 2), standalone too: every record, key lookup and table row reads identically, `verify_shard` passes on both
- ✅ Zero a shard's index and superblocks → `salvage` recovers every record: key, names, bytes, format, metadata and persons rows (unit test compares against the undamaged archive; without a schema source: raw rows, no names). A flipped payload byte → reported `intact = 0`. A shard holding another shard as a blob salvages as its own 2 records
- ✅ Real data: COCO shard 1 (533 records, 524 MB) with its index and superblocks zeroed → salvaged in 0.6 s; 2 665 files byte-identical to the dataset; re-packed, `persons` / `descriptions` / `image` rows identical to the original archive (the bulk descriptor tables are, by design, not in record headers)
- ✅ Unit tests 53 new checks; CLI tests 6 new; fuzzing now also zeroes tails / superblocks and runs `salvage` and `rebuild-manifest` on every damaged shard (3 000 iterations, clean); 0 doxygen warnings; client build check passes

### Phase 3: stream and table edits (implemented 2026-09-22)
- [x] `replace-stream`, `add-stream`, `drop-stream` (`pzpd_edit_stream()`): shard-by-shard rewrite through the writer with the shard's identity (uuid, index, first ordinal) and generation + 1, written as `<shard>.rewrite.00000.pzpd`, fully verified (index + every payload checksum), then renamed over the shard; manifest rewritten last. Other streams' blobs are copied byte for byte with their stored metadata (no re-detection), group / frame and all table rows (record + global) are kept; new files get format + metadata detected. `--missing keep|drop`. A dry run over all shards first refuses clashing names (anywhere in the archive), duplicate keys / names in the list, and records that would be left without blobs. Resumable: shards whose generation is ahead of the manifest and that already hold the listed files (replace; since 2026-09-24 the content is checked too, because an interrupted table edit or compact also leaves a shard ahead, and `replace-stream` used to skip it silently), or that already have / lack the stream (add / drop), are skipped
- [x] `replace-table`, `add-table`, `drop-table` (`pzpd_edit_table()`): per shard, the new table section + a backup superblock of generation + 1 are appended, fsynced, then the primary is flipped and fsynced; the reader now takes the **higher valid generation** of the primary and the EOF backup. Kept tables' sections stay where they are. `--missing empty|keep`; global tables from `@row` lines; a replacement may change the schema (not with `keep`); every row is parsed before any shard is touched. Resumable (add / drop by content, replace redone); an unflipped primary left by a crash is finished on the next run
- [x] `compact` (`pzpd_compact()`): per shard, the live sections are appended as generation + 1 and flipped, then written right after the index as generation + 2, flipped, and only then is the file cut; a crash leaves one valid generation. A rerun also trims a tail left by a crash after the last flip
- [x] Crash test hook `PZPDIR_TEST_CRASH=<point>:<n>` (`flip`, `compact`, `shard`, `rewrite`): the process exits at that point, as a kill -9 would
- Known limits (spec revision 8): record-header row copies (salvage) keep the rows as packed; a crashed stream edit leaves the manifest stale for the rewritten shards until the command is rerun; peak RSS of a stream rewrite includes both shards' mapped (file-backed) pages
- Spec: revision 8

**Verify** (✅ passed):
- ✅ Replacing depth on COCO val2017 (a copy; `segment_val2017` PNGs as the stand-in "new teacher", 5 000 files): 7.7 s; the other 20 000 blobs (rgb, all, geo, seg) byte-identical with the same names and metadata, every depth blob equals its new source, `verify --blobs` OK; **peak extra disk 2 979 MB = one (rewritten) shard**
- ✅ Crash + rerun → the same state as the uninterrupted edit, for: replace-table killed between append and flip, add-table likewise, compact between its generations, add-stream after 2 of 14 shards, replace-stream before its first rename (archive unchanged, exactly one extra shard file); after each crash every shard verifies on its own
- ✅ `replace-table` / `add-table descriptor-like bulk table` rewrite no record data: the record area of all 14 shards is byte-identical (SHA-256) before and after; only the tail grows; `compact` then reclaims it (1 073 312 → 884 896 bytes, same as without the crash)
- ✅ Unit tests (21 new: errors, keep-missing, global table replace, bad row refused before any change naming its key, duplicate keys, empty-record guard, add / drop stream with tables surviving, compact before / after rewrites, shard path refused); CLI tests 14 new; fuzzing, ASan / UBSan, 0 doxygen warnings, client build check: clean

### Phase 4: video groups (implemented 2026-09-22)
- [x] Writer: `pzpd_writer_group(w, name, len, bytes_hint)` registers a group (name unique in the archive; unnamed groups and unregistered ids work too) and records carry `(group, frame)`; a group's records must be consecutive (a closed group can't get more) with increasing frames
- [x] Shard cut: never inside a group; a new group whose `bytes_hint` doesn't fit in the open shard starts a new shard early; a group larger than the shard limit gets a shard of its own (cut before it and after it)
- [x] Group table per shard (section kind 13, 24-byte entries `{id, first_local, frame_count, name_offset, name_len}`, in the index checksum after the metadata section); group names in the shard and manifest hashes as a third kind, so `rebuild-manifest`, the section scan and stream rewrites keep them
- [x] Reader: `pzpd_group_find(name)` → first ordinal (a group's handle, unique over a collection), `pzpd_group_info(ordinal)` → first, frames, id, index, name; `pzpd_range_span` / `pzpd_read_range`: one pread over consecutive records, refused across a group, shard or member boundary
- [x] Record lists: `@group NAME` names are kept; `pack` registers each group with the size of its files as the hint. CLI `pzpdir groups`; `salvage` writes `@group` lines (names from `--schemas`). Python: `Writer.group`, `group_find`, `group`, `read_range`, `read_group(name_or_ordinal, streams, start, count)`
- Fixed on the way: the shard loader's bound on hash entries didn't count group names (every one-stream shard with a group was rejected); found by the CLI test

**Verify** (✅ passed):
- ✅ A range read equals the concatenated single reads: every clip of the unit test (named, oversize, unnamed; some frames lack a stream), in C and in Python (`read_group`, also with `start` / `count`)
- ✅ No group spans a shard (unit, CLI: 3 clips over 7 shards of 32 KiB); the oversize clip (100 × ~2.8 KB > 64 KiB, or 60 × 1.5 KB > 32 KiB) is alone in its shard; a clip whose hint doesn't fit after 20 plain records starts a new shard early
- ✅ Misuse refused: duplicate group name, a closed group reopened, non-increasing frames, ranges across group / shard boundaries, a too-small buffer
- ✅ Groups survive `rebuild-manifest` (byte-identical), `add-stream` (shard rewrites keep ids, names, unnamed groups), the section-scan fallback (both superblocks zeroed), and salvage → pack
- ✅ Unit tests (33 new, also under TSan), CLI tests (3 new), Python test (1 new), fuzzing with a named and an unnamed group plus random `read_range` / `group_info` on every damaged archive (3 000 iterations, clean), 0 doxygen warnings, client build check

### Phase 5: Python bindings (implemented 2026-09-22)
- [x] `libpzpdir.so` exports the whole C API (internals are `static`, xxHash is `static inline`); `src/pzp/pzpdir.py` (`import pzp.pzpdir as pzpdir`, ctypes, GIL released during every call) provides:
  - `open` (path / list with aliases / collection file; `verify`, `allow_missing`, `hugepage`, `populate`), `Archive`: `len`, `streams`, `members`, `member_of`, `member_range`, `shards()` (storage, recovery, AUTO mode)
  - `find(key, member=)` (KeyError if none), `find_name`, `find_all`, `key`, `read` (bytes), `read_record` (dict of memoryviews over one pread), `read_image` (PZP / PZP containers natively via `pzpd_read_pzp`, JPEG / PNG / … via PIL), `verify`
  - `info` (format + metadata, no data I/O), `stream_stats` (formats, WxHxC@bits combinations)
  - `table` (numpy structured array; `str` columns decoded to Python strings; a copy, so it stays valid after `close()`), `table_all` (CSR `index` + rows over all records), `global_table(member=)`, `schema`, `tables`, `table_csv`
  - `Writer` (context manager: finish / abort; `record()` context, `add` with metadata override, `add_file`, `table`, `rows_csv`, `rows` from numpy, `global_rows(_csv)`)
  - `Archive.prefetcher(streams, io_threads, mode, budget_mb, window)` → `Prefetcher` (`submit` with per-ordinal stream lists, `get` → `Record` (mapping of memoryviews, `with` / `release()`), `discard`, `clear`, `stats`)
  - `collection_write` / `collection_refresh`, `rebuild_manifest`, `salvage`, `edit_table` / `edit_stream` / `compact`, `detect_format`, `PzpdError` (`.code` = enum pzpd_error)
- [x] Packaging: `setup.py` also builds `src/pzpdir/libpzpdir.so` and copies it into the package; `pyproject.toml` lists it as package data; the module finds it via `$PZPDIR_LIB`, next to itself, or in `src/pzpdir/`. (A wheel wasn't built here: that would rebuild the untracked root `libpzp.so` too.)
- `read_group` came with phase 4 (see there)

**Verify** (✅ passed):
- ✅ Round trip (`src/pzpdir/tests/test_pzpdir_py.py`, pytest-compatible, also runs as `python3 …` / `make test-py`; pytest isn't installed here): 8 tests — ctypes struct sizes equal the C `sizeof`s (17 structs), Writer → every read path with 200 records incl. PNG decode and metadata override, all three table kinds (str, arrays, bulk f32, global) incl. `table_all` = per-record rows, the prefetcher in MAP / PAGECACHE / BUFFERS / AUTO consumed by 4 Python threads (counters add up, no buffer memory left), per-ordinal masks and discard, collections with duplicate keys, rebuild-manifest (byte-identical), salvage of a zeroed shard, table / stream edits + compact, error codes, native PZP `read_image` (written with the `pzp` package). Passes with the venv's Python 3.12 / numpy 2.4 and the system Python / numpy 1.26
- ✅ COCO val2017 (tables archive): `read_image` of JPEG `rgb` and PZP-container `all`, `table_all("persons")` 11 004 rows in ~1 ms, 5 000 × 768 DINOv2 descriptors in 20 ms, captions / joint names as strings, prefetcher streams 3.2 GB of rgb+all+geo in 0.66 s (warm)

### Phase 6: prefetcher: PAGECACHE + MAP + AUTO (implemented 2026-09-22; huge pages unmeasured)
- [x] I/O thread pool, whole-epoch `submit(ordinals, masks)`, window throttle, out-of-order `get` from any thread, `release`, `discard`, `clear`, `stats` (`pzpd_prefetch_stats_get`)
- [x] Storage detection per shard (`pzpd_storage_kind`, `pzpd_shard_info.storage`); `pzpdir info` shows storage kind + the mode AUTO picks (`pzpd_prefetch_auto_mode`)
- [x] MAP: `get` returns mmap views; I/O threads `madvise(MADV_POPULATE_READ)` each record a window ahead (read-one-byte-per-page fallback before 5.14); no budget, no arena
- [x] PAGECACHE: also mmap views; `MADV_WILLNEED` on all of a record's ranges, then `MADV_POPULATE_READ`, so a prefetched record is resident *and* pre-faulted
- [x] Open flags `PZPD_O_HUGEPAGE`, `PZPD_O_POPULATE`
- [x] AUTO: RAM → MAP; BLOCK and member < ½ RAM (since the review: ½ of the process's memory limit, i.e. the cgroup's when lower) → PAGECACHE; else → BUFFERS, which runs as PAGECACHE until phase 7 (`stats.auto_fallbacks`); `PZPD_PF_BUFFERS` itself is rejected until then
- Spec: revision 5 (names, window / claim semantics, both mmap modes)

**Verify** (✅ passed, ⚠️ partly, ⬜ open). COCO val2017, rgb+all+geo, 5 000 samples shuffled, strided workers; `scripts/bench_pzpdir_early.c` now has `pzpd-pf-{pagecache,map,auto}`, `--work US` (busy CPU per sample standing in for decode), and per-worker minor faults (`RUSAGE_THREAD`):
- ✅ NVMe, cold cache, 5 ms work/sample — prefetch beats `pzpd-record` and `fs-open`, and reaches the CPU bound:

  | | fs-open | pzpd-record | pf-pagecache | bound |
  |---|---|---|---|---|
  | T=1 | 169 /s | 178 /s | **200 /s** (get p50 0.00 ms, 4 999 / 5 000 hits) | 200 |
  | T=8 | 1 317 /s | 1 406 /s | **1 593 /s** (4 992 hits) | 1 600 |

  With **no** work every method is bandwidth-bound (3.5–3.9 GB/s, ~0.9 s per epoch at T=8) and the prefetcher gains nothing: it can't beat the device. Its value is overlapping I/O with decode.
- ⚠️ RAM disk (`/dev/shm`, warm, each mode in its own process): with 2 ms work/sample MAP gives **0.00 worker-side minor faults / sample** (vs 10.85 for `pzpd-view`) and get p50 0.00 ms; throughput T=1 499 /s (record 481, view 488), T=8 3 904 /s (record 3 837, view 3 898), i.e. MAP ≥ view ≥ record. These numbers, and the first no-work ones (T=8 "MAP 96 k/s < view 138 k/s, the I/O threads can't keep up"), were measured while views and the prefetcher were only touched one byte per 4 KiB page, and are **superseded** by the corrected run in "Review fixes" below: with every byte read, MAP is 2× faster at T=1 and all paths tie at T=8 (memory bandwidth). **Huge pages not measured**: needs a tmpfs mounted with `huge=within_size` (sudo); `PZPD_O_HUGEPAGE` is implemented and tested for correctness only (until the review it never reached the archive, see below)
- ✅ AUTO picks MAP for a `/dev/shm` member and PAGECACHE for an NVMe member of the same collection (unit test + `pzpdir info` on COCO + a `/dev/shm` member)
- ✅ `dataloader-replay` (`scripts/bench_dataloader_replay.c`, `make bench_dataloader_replay`): the DataLoader's exact trace — shuffled epoch, batches of 40, worker t takes positions ≡ t (mod T), batch barrier, k+1 double buffer with the main thread "consuming" 26 ms per batch (Python collect + copy, §6b), per-sample rgb+all+geo; today's path replayed with `signalPrefetchFile` (open + WILLNEED), open/fstat/read/close ×3 and `freeFileDescriptor` (DONTNEED + close); decode + augmentation modelled as busy CPU. COCO val2017 on NVMe, cold:

  | per-sample CPU, workers | today (dirs) | pzpd-record | prefetcher AUTO | prefetcher BUFFERS |
  |---|---|---|---|---|
  | 11.8 ms (profiled at 6 threads), T=6 | 430 /s | 448 /s | **482 /s** (+12 %) | 481 /s |
  | 11.8 ms, T=30 | 1 048 /s | 1 111 /s | 1 130 /s | **1 182 /s** (+13 %) |
  | 2 ms, T=6 / T=16 | 1 526 / 1 523 /s | 1 527 / 1 530 /s | 1 530 / 1 530 /s | 1 522 / 1 523 /s |

  - Worker-bound pipelines gain 12–13 %: the prefetcher serves 4 966–4 994 of 5 000 samples as hits, so workers never wait for I/O; the prefetched path sits at the CPU bound (6 workers / ~12.4 ms ≈ 482 /s).
  - With light work every path is capped at ~1 530 /s by the main thread's 26 ms per 40-sample batch (§6b's ≈ 1 500 /s ceiling): I/O stops mattering there.
  - **Warm cache changes nothing for today's path** (432–436 /s vs 482 /s warm): `freeFileDescriptor()` does `posix_fadvise(DONTNEED)` after every read, so the DataLoader evicts each file right after use and every epoch reads from disk, whatever the RAM. (A finding about today's code, independent of PZPD.)
  - Caveat: busy-CPU "work" has none of the memory-bandwidth contention the real loader shows at 30 threads (§6b: 316 /s real), so absolute T=30 numbers are optimistic; the comparison between paths is what this measures. The real end-to-end number still comes with the DataLoader integration.
- ✅ `client_smoke.c` with a shuffled whole-epoch schedule and strided multi-threaded `get` / `release` (8 and 16 threads, COCO tables archive + `/dev/shm` member): ASan / UBSan clean, TSan clean; views equal `pread` copies
- ✅ Unit tests (44 new: 20 000-claim shuffled schedule with repeats and per-ordinal masks over 8 strided consumers in MAP and PAGECACHE, views = `pread` copies; counters add up; window 8 with 8 held claims keeps exactly 8 prefetched; clear with old tickets; unscheduled gets; masks wider than submitted; bad ordinals; destroy with held / queued claims; AUTO per shard; `O_POPULATE | O_HUGEPAGE` reads) — also TSan clean; fuzzer now drives a prefetcher on every damaged archive (3 000 iterations, clean); 0 doxygen warnings; client build check passes
- **Milestone: the DataLoader integration can start** (in its own repo / branch; not started here)

### Phase 7: prefetcher: BUFFERS + O_DIRECT (implemented 2026-09-22)
- [x] BUFFERS mode: I/O threads `pread` each record's ranges with `O_DIRECT` (4 KiB-rounded offsets / lengths, page-aligned buffers) into private buffers; buffered `pread` fallback when a file system refuses `O_DIRECT` (per shard, counted in `stats.direct_fallbacks`); the mmap is never touched, so the page cache doesn't grow
- [x] `budget_bytes` bounds prefetched buffers **plus** those held by tickets (one record may exceed it, so progress is guaranteed); producers stall on budget or window (backpressure, `producer_stalls`)
- [x] Buffers are per record (`posix_memalign`) rather than a pre-carved slab: at most `window` are in flight / ready (slot table), a get moves the buffer into the ticket, `release` frees it. Tickets stay valid across `clear` (their memory stays counted until released)
- [x] Non-contiguous masks: ranges closer than 256 KB are read as one (over-read counted), others separately, into one buffer
- [x] Wider-than-submitted masks, failed prefetch reads, and gets before the I/O threads reach a claim: synchronous `O_DIRECT` read into a new ticket buffer; `PZPD_O_VERIFY` checks the XXH32 of BUFFERS blobs too
- [x] AUTO now gives large block-device members BUFFERS (no more PAGECACHE fallback); `stats` gains `shards_buffers`, `direct_fallbacks`, `buffer_bytes`, `buffer_bytes_peak`
- Spec: revision 6

**Verify** (✅ passed; COCO val2017 on NVMe, cold, rgb+all+geo, T=8, window 256):
- ✅ `systemd-run --user --scope -p MemoryMax=1G`: 5 ms work, budget 256 MB → 1 588 samples/s (CPU bound 1 600), peak buffers 174.5 MB (the window bound first), RSS 207 MB; budget 64 MB + window 1 024 → peak buffers **67.1 MB = 64 MiB**, RSS 98 MB, still 1 587 samples/s
- ✅ The page cache does not grow: archive pages resident after an epoch **0.0 %** with BUFFERS vs 87–90 % with PAGECACHE / `pzpd-record` (PAGECACHE's RSS shows 3.3 GB of shared file pages)
- ✅ Bonus: with no work, BUFFERS + `O_DIRECT` is the fastest path measured: **9 707 samples/s, 6.3 GB/s** vs 5 700 / 3.7 GB/s for PAGECACHE and 5 442 / 3.5 GB/s for `fs-open` (cold)
- ✅ ASan / UBSan (unit tests, 15 new checks: budget stops the I/O threads before the window, held BUFFERS ticket readable after `clear` and freed on release, wider masks, destroy with prefetched buffers, BUFFERS over a disk + `/dev/shm` collection), TSan (unit tests + `client_smoke --mode buffers`, 16 threads), valgrind memcheck with leak check (`client_smoke` in BUFFERS / MAP / PAGECACHE) all clean; `clear` mid-epoch is safe; fuzzing with random modes and 4–32 KiB budgets: 3 000 iterations clean
- ⚠️ The `O_DIRECT`-refused fallback isn't exercised on this machine: kernel 6.8's tmpfs accepts `O_DIRECT` (0 of 33 shards fell back)

### Review fixes (2026-09-22)
A review of `pzpdir.c` after all phases. Every fix has a unit test that fails (or crashes under ASan) on the code before it.
- **Bugs:**
  - `PZPD_O_HUGEPAGE` and `PZPD_O_POPULATE` never took effect: `pzpd_open_many` / collection members passed only `PZPD_O_VERIFY` on to each archive. Now every flag but `PZPD_O_ALLOW_MISSING` does (`strace` shows the `madvise` calls)
  - CSV `" -1"` (minus after whitespace) was stored as 2^64 − 1 in a `u64` column; now out of range, like `"-1"`
  - a crafted manifest whose `hash_count` × 24 wraps around crashed `pzpd_find()` (read past the mapping); `hash_count` is now bounded by the file size. Random bit flips can't hit this (checksum), so the fuzzer never did
- **Robustness:** PNM / PFM sizes must fit u32 and be numbers (`strtod` reads `nan`, `inf`); the section scan refuses a record entry whose offset + size wraps past 2^64; the writer stops global rows at the reader's 4 G limit (it read past the caller's buffer before) and reports the limit for record rows
- **Prefetcher:** BUFFERS record sizes are computed at submit, outside the lock (locating a record may open a shard, which blocked every get / release); MAP / PAGECACHE gets use the offsets they already located instead of a second route + resolve per stream; one checksum loop for all modes. Entries grow to 32 B; submitting 1.28 M entries takes 0.13 s instead of 0.06 s (the same work, moved out of the lock). Throughput is unchanged (medians of 5–7 runs, old and new alternating): RAM-disk MAP T=1 / T=8, NVMe cold BUFFERS 9 822 → 9 813 /s and PAGECACHE 5 599 → 5 620 /s at T=8, `dataloader-replay` T=30 BUFFERS ≈ 1 140–1 180 /s both. So the lock is not a bottleneck at these rates; replacing it with atomics is not needed
- **AUTO** uses the process's memory limit: physical RAM, or the cgroup's (v2 `memory.max` / v1 `memory.limit_in_bytes`, own group or ancestor) when lower; member sizes come from the manifest, so no shard is opened for it (spec revision 11). COCO (4.8 GB): no limit or `MemoryMax=12G` → PAGECACHE, `MemoryMax=4G` → BUFFERS (before: PAGECACHE in all three)
- **Benchmarks** (`bench_pzpdir_early`, `bench_dataloader_replay`): every mode now reads every byte it delivers once, as a decoder does (views and the prefetcher touched one byte per 4 KiB page, record / fs-open copied everything, so view vs record compared different work; `bench_pzpdir_early --touch pages` reproduces the old way), and every run opens the archive afresh (runs in one process inherited each other's page tables). Corrected RAM-disk run (`/dev/shm`, warm, no work, rgb+all+geo, samples/s, median of 5):

  | T | pzpd-record | pzpd-view | pf-map |
  |---|---|---|---|
  | 1 | 12 528 | 14 151 | **28 859** (0.00 worker faults / sample) |
  | 8 | 43 406 | 43 813 | 43 755 (≈ 28 GB/s: memory bandwidth) |

- **Still open:** huge pages unmeasured (needs the sudo mount); `libpzpdir.so` stays `-march=native` (the Python package is built on the machine that uses it)

### Review fixes (2026-09-23)
A second review of `pzpdir.c`. Each bug fix has a unit test that fails (or trips ASan) on the code before it.
- **Recovery (section scan) after a table edit:** an edited shard that was not compacted since keeps the replaced table section next to its replacement; with both superblocks lost, the scan took both (4 tables instead of 3: refused as stale through the manifest, the old rows possible standalone). It now takes each table's newest section, in the slot of its first one (spec revision 7 text updated). A table dropped since the last `compact` still comes back (no generation is stored in sections)
- **`pzpd_writer_finish()` failing** on the last shard (e.g. disk full while writing its index) left the shard's descriptor open and its `.tmp` file behind; it now cleans up as `pzpd_writer_abort()` does
- **NPY probe:** a header cut right after the dtype's opening quote (≥ 4 KiB headers) read past the probe's stack copy
- **`edit-stream` dry run:** the name-clash check merges each shard's sorted hash with the sorted input names instead of one lookup per name per shard. 200 k new files into 785 shards: `add-stream` 55.8 s → 34.9 s, output byte-identical
- `scripts/check_client_build.sh` links the DataLoader's `PZPDLoader.c` when present (its branch `pzpdir` calls it, so the link check failed)
- **v0.10, prefetcher lanes:** a contention benchmark (1 M × 256 B records on `/dev/shm`, MAP, consumers only get + release) showed the single prefetcher mutex was the ceiling: unscheduled gets fell from 4.1 M/s at T=1 to 3.0 M/s at T=16, and 16 I/O threads were 2.5–4× slower than 4 (their wake-ups and lock traffic; removing the `madvise` changed nothing). Now:
  - the schedule is split by ordinal into `PZPD_PF_LANES` (8) lanes, each with its own lock, entries, ordinal hash and buffer slots (grown on demand); window and budget are atomic counters shared by the lanes, reserved by compare-and-swap before an entry starts. Every entry keeps its position in the whole schedule and may start only when it is < claims done + window, so no lane runs ahead of the global order. Tickets carry the lane in their private `pos`
  - I/O threads serve lanes round-robin (4 threads → 2 lanes each), start up to 8 MAP / PAGECACHE entries per lock hold (BUFFERS: one) and park on their own condition variable; they announce why before a last look, and releases wake only threads parked with queued entries, once per batch of window room
  - synthetic, T=16: unscheduled gets 3.0 → 18.5 M/s, prefetched gets 1.0 → 4.9 M/s (4 I/O threads), 0.77 → 3.8 M/s (16 I/O threads)
  - `dataloader-replay` (COCO val2017, NVMe, cold, 11.8 ms work): T=6 unchanged (479 / 478 samples/s, ~5 000 hits); T=30 PAGECACHE 1 171 → 1 118–1 136, BUFFERS 1 157–1 179 → 1 235–1 244; 17–22 sync misses of 5 000 (0 before). Neutral for the DataLoader, whose rates are far below either ceiling
  - checks: unit tests (ASan / UBSan, TSan twice), CLI, Python, fuzzing, `client_smoke` on COCO in MAP / PAGECACHE / BUFFERS at 16 threads (ASan, TSan)
- Also (second pass, after the word index and the file split): a shard's word index with more than 255 distinct source values overran a stack array while failing (ASan: stack-buffer-overflow in `pzpd_words_build`), now refused cleanly; a (re-sealed) superblock whose metadata section points outside the file is refused at open (`verify_shard` / `compact` / table edits read it unchecked: SEGV); `pzpdir groups` checks its blob lookup. **`edit_table` 3× faster** (100 k records × `u16[256]`, the histogram tables: 2.02 s → 0.67 s, output byte-identical): the rows parsed by the up-front check are kept for the shard pass instead of being parsed again, and integer CSV fields that are plain digits skip `strtoull` (same results; anything else takes the old path). Open: the 43 doxygen warnings of the word-index code (the gate is 0)
- **Layout (2026-09-23):** the `pzpdir_*.inc.c` parts became ordinary translation units `pzpdir_*.c` with `pzpdir_internal.h` (on-disk structures, shared types, 85 internal prototypes; `PZPD_INTERNAL` = hidden visibility, so `libpzpdir.so` exports exactly the 92 public functions); `pzpdir.c` is gone. The DataLoader now links its own `pzpdir/libpzpdir.so` (see D21, §5). Verified: unit / CLI / fuzz / Python tests, doxygen unchanged (the 43 word-index warnings), client check, and the DataLoader from Python on COCO val2017 (converter output, 5 000 samples): 6 batches byte-identical to the previous built-in pzpdir in prefetch modes auto / buffers / off
- Also: stream names with `"`, `\` or control characters are refused (the shard metadata JSON lists them unescaped); `pzpd_writer_blob_file()` refuses a file over 4 GiB before reading it; a resumed `edit-stream` matches the done shards' keys in the same record pass (no lookup per file per shard); error context through `pzpd_error_wrap()` / `pzpd_error_save()`; the CLI's list parsing and `unpack` reuse `split_fields()` / `write_file()`

### Review fixes (2026-09-24)
- **`replace-stream` after an interrupted table edit / compact skipped shards silently:** its resume rule took any shard whose generation was ahead of the manifest as already rewritten, but table edits and compact raise generations too. The stream kept its old blobs in those shards, the command succeeded, and `verify` passed. A shard ahead of the manifest now counts as done only if it already holds every listed file (name, size, XXH32) and, with `--missing drop`, no other blob of the stream; fresh runs read no files for this. CLI tests: the crash case (fails on the old code), and a `replace-stream` killed after 2 shards whose rerun skips them (same inode) and ends like the uninterrupted edit
- **`pzpd_blob_ref` carries the blob's stored metadata** (`meta`: format, width, height, channels, bits, frames, flags, as `pzpd_blob_info_get()`), so `pzpd_read_record()` / `pzpd_read_range()` / `pzpd_prefetch_get()` hand workers the dimensions with the bytes, before any header is parsed. The struct grows from 24 to 40 bytes (`format` kept); Python `BlobRef` updated, prefetched `Record.meta[stream]`; the DataLoader's vendored `pzpdir/` updated and rebuilt. Found on the way: `pzpd_read_record()` through a handle reported a record's present-but-empty blobs as absent when every requested blob was empty (refs were copied only for a non-zero span)
- Doxygen gate back at **0 warnings** (the 43 of the word-index code: undocumented parameters, struct members, a `\w` in a brief)

### DataLoader integration (first client; separate repo and branch)
Covered by §5 (constraints) and §7 (plan). It starts after phase 6 (§5 order of work) and lives in
`RGBToPoseDetect2D/datasets/DataLoader`: vendored `pzpdir/` sources, a ~100-line `PZPDLoader.c`
adapter, prefetcher hooks in `PrepareBatch.c` / `DataLoader.c`, new
`db_set_*` setters and `DataLoader.py` wrappers, plus the converter `datasets/convertToPZPD.py`.
Full switch, no mixed sources (D22–D26).

## 6b. Rethink input: DataLoader profile (2026-09-21)

**Why:** the phase-1 early gate was met at 1 thread but not at 8. Decision: pause and first measure
where the real DataLoader spends its time. Success is now **both** fewer files and a measurable
end-to-end speed-up. Storage setups that matter: local NVMe and RAM disk.

**Setup:**
- The training DataLoader, built exactly as `ymapnet/training/trainYMAPNet.py` builds it: 30 threads,
  batch 40, double buffer, augmentation parameters and heatmap settings from `configuration.json`.
- COCO val2017 as the training set, because this machine's `depth_/segment_/all_train2017` folders
  are empty; val2017 has all 5 modalities plus descriptors.
- `libDataLoader.so` built in scratch from a copy of the sources with the `makeLibrary.sh` flags.
  Variants: `PROFILE_THREAD_ACTIVITY=1` for stage timers, and `-g` for callgrind.
- The RGBToPoseDetect2D repository is only read. No sudo, so no `perf`.
- Scripts are in the session scratchpad: `dlprof/profile_dataloader.py` (driver), `dlprof/stages.py`
  (stage-log summary).

### Throughput and thread scaling (wall clock)

1. **I/O is not the bottleneck.** 30 threads: **316 samples/s cold, 317 warm** (every one of the
   25 000 files evicted before the cold pass). Cold adds ~7 ms of I/O wait per sample, fully hidden
   by other threads. Phase 1 showed raw reads deliver 5 500–7 300 samples/s, so storage has ~15× more
   bandwidth than the pipeline uses.
2. **More threads make it slower** (AMD Ryzen 7 3800X, 8 cores / 16 threads, warm):

   | threads | 2 | 4 | **6** | 8 | 10 | 12 | 16 | 20 | **30 (training)** | 40 | 60 |
   |---|---|---|---|---|---|---|---|---|---|---|---|
   | samples/s | 352 | 497 | **521** | 448 | 401 | 360 | 336 | 323 | **316** | 300 | 299 |

   - CPU per sample: **11.8 ms at 6 threads vs 34.8 ms at 30** (609 % vs 1102 % CPU).
   - Involuntary context switches: 2.4 k vs 58 k.
   - Stage wall time per sample (`PROFILE_THREAD_ACTIVITY`) inflates **7×** from 4 to 30 threads.
     The stages that write big output buffers inflate most:
     - depth_processing 0.97 → 15.0 ms
     - keypoints 0.08 → 1.8 ms
     - segmentation_processing 0.22 → 2.6 ms
     - PZP read + decode 0.84 → 5.9 ms
     - JPEG read + decode 1.9 → 6.2 ms
   - At 30 threads, workers also spend ~40 ms per sample waiting at the batch barrier
     (`thread_sync`): 40 samples over 30 threads gives 10 threads 2 samples and 20 threads 1.
   - **The biggest end-to-end win found is outside PZPD:** 6 threads instead of 30 = **+65 %**.
3. **Serial per-batch overhead:** at 4 threads a batch takes 78.6 ms, but its work is only ~52 ms
   spread over the threads. The ~26 ms per batch that is left is main-thread Python: collect / swap
   and the `.copy()` of ~210 MB of outputs per batch. That caps the pipeline at ≈ 1 500 samples/s no
   matter how fast storage or workers are.

### Where the instructions go (callgrind)

`-O3 -g` build, 4 threads, 13 batches = 520 samples.
- Worker threads account for 95.5 % of 33.6 G instructions, **≈ 62 M instructions per sample**.
- Valgrind runs threads one at a time, so these are pure CPU costs with no contention; section 2
  above covers contention.

| Cost (% of all instructions) | What |
|---|---|
| **30.0 %** | **JPEG decode** (`ReadJPEGMem` → libjpeg-turbo `jpeg_read_scanlines`), at full source resolution (~640×480) for a 256×256 input. `jpgInput.c` never sets `scale_num` / `scale_denom` (DCT-domain downscaling) |
| **24.3 %** | **augmentations** (`processing/augmentations.h`), of which `_applyBlurSparse` (gaussian / defocus / motion blur) is **10.0 %** |
| **9.0 %** | **PZP decode of `all`**: `LZ4_decompress_safe` 6.7 % (the val2017 `all` files are **already LZ4**, not zstd), plus prefix-sum / interleave |
| 9.1 % | `memset`, mostly `cleanHeatmapsOfTargetSample` (7.7 %): clearing ~5 MB of output heatmaps per sample before they're written again |
| ~8 % | resize (`processing/resize.h` 5.9 % + `resizeImage` 2.2 %) |
| 3.1 % / 2.8 % | depth recalibration / conversions |
| ~2 % | normals (AVX2), Sobel, heatmap drawing, flip / rotate90 combined |
| ~2.5 % | `memcpy` |
| 4.5 % | Python (interpreter, start-up, DB load; the main thread) |

Depth processing costs only ~4–5 % of the instructions, yet it inflates the most under 30 threads.
That confirms the 30-thread slowdown is memory / cache contention, not compute.

### Implications for PZPD (proposed, not yet decided)

- **"Fewer files"** is already met by phase 1. 1b (collections) and 1c (tables, replacing `.db`
  and descriptor files) serve it too.
- **I/O-speed features can't produce an end-to-end speed-up** with this pipeline on NVMe or tmpfs,
  because I/O is already hidden. That covers the prefetcher, O_DIRECT and the MAP / RAM-disk modes
  (phases 6–7). Candidates to defer until I/O becomes visible again.
- **End-to-end speed from the storage side = cheaper CPU per sample.** Multi-stream records make it
  cheap to store extra, cheaper representations; I/O has ~10× headroom:
  - **RGB pre-scaled towards the training input** (e.g. short side ≈ 288 px for 256 px input with
    zoom ≤ 1.1), as `datasets/rescaleRGBDataset.py` already does for other sets. JPEG decode (30 %)
    and resize (8 %) scale with the pixel count, so ~4–5× fewer pixels would save roughly
    **25–30 % of worker instructions**. Keypoints must stay consistent with the stored size (the
    `image` table keeps the original size).
  - **`all` stored at reduced resolution** (it's full-res today): ~5× fewer pixels in the 9 % LZ4 /
    PZP decode.
  - A DataLoader-only alternative without new data: libjpeg-turbo DCT scaling
    (`scale_denom = 2`) decodes at half size for a fraction of the cost.
- **The largest speed-ups are in the DataLoader itself** (outside PZPD, measurable with the same
  harness):
  - thread count / contention: +65 % at 6 threads;
  - batch-barrier idle time;
  - blur augmentations (10 %);
  - clearing heatmaps that are fully overwritten afterwards (~8 %);
  - the per-batch Python copies.

## 7. DataLoader integration plan (from `PrepareBatch.c`, `DataLoader.c`, `DBLoader.c`, `DataLoader.py`)

Revised 2026-09-22 with decisions D22–D26: a full switch to archives, descriptors normalised
at conversion, a caption for every sample, no geolocation, no SuperPoint. The deliverable is
code. The real datasets are converted on the PC that holds them (D26).

**Today:**
- `readPoseDatabase` parses each source's `DB1` `.db` at startup and matches descriptors (`<db>.dinov2` / `.dinov3`) by position + basename. Sources are concatenated via `startOffset`.
- The epoch order `db->indices` is shuffled once per epoch. Batches are consecutive slices of it, and Python double-buffers batch k+1 while the GPU trains on k.
- Worker *t* takes positions `start+t, start+t+T, …`.
- Files are resolved by `resolvePathToRequestedFiles` (cached per sample as `DL_FileKind`):
  - rgb: `<images>/<file>`
  - depth: `<stem>.pzp`, then `.png`, then `_depth.png`
  - seg: `<stem>.pzp`, then `.png`
  - combined: `<stem>.pzp` / `.png`
- Per sample the reads are, in order, each gated by config flags and presence:
  1. combined (before the erase decision)
  2. rgb
  3. depth, if not multiplexed
  4. seg, if not multiplexed
- `signalPrefetchFile` only warms the same file microseconds before reading it, then drops it with `DONTNEED` and closes it.
- The codecs already decode from memory (`readImageFromMemory`, `ReadJPEGMem`, `ReadPNGMem`, `ReadPZPMemory`), and `cachedReadImage()` is already the point where a different byte source plugs in.

### Part A: the converter (`RGBToPoseDetect2D/datasets/convertToPZPD.py`) (implemented 2026-09-22)

Files (in RGBToPoseDetect2D, uncommitted): `datasets/convertToPZPD.py` (converter), `datasets/checkPZPD.py` (checker), `datasets/testConvertToPZPD.py` (fixture tests). It uses the phase 5 `pzp.pzpdir.Writer` directly (needs `pip install -e` of this repo on the converting PC). A text record list is not used, because at COCO-train scale the tables list was 1.1 GB and took 394 s (§8).

1. **Input:** `--source DB IMAGES DEPTH SEG ALL [--descriptions FILE]`, or `--config configuration.json [--set TrainingDataset|ValidationDataset …] [--all] [--root DIR]` (paths relative to the config's directory, 5- or 6-column entries, enabled values as `DataLoader.py`). A source listed in both sets is converted once; two different sources with the same `.db` name are refused. Output: **one directory per dataset**, named after its `.db`: `OUT/cocoTrain/cocoTrain.pzpd` + shards `cocoTrain.NNNNN.pzpd` (`--shard-size`, default 4 GB). After each source it prints the config entry to use (`["enabled", ".../cocoTrain.pzpd"]`).
   - **Changed from the plan:** no collection files. The DataLoader opens its enabled sources itself (`pzpd_open_many`, step 1), and a collection file would go stale whenever an entry is enabled or disabled.
2. **Records:**
   - One record per `.db` sample, in `.db` order, with key = `imagePath`; blob names are the paths as given in the config (e.g. `datasets/coco/cache/coco/val2017/000000000139.jpg`).
   - Streams `rgb, all, depth, seg` (D25), found with exactly `resolvePathToRequestedFiles`'s rules (stem = `imagePath` minus its last 4 characters; `all` `.pzp`/`.png`, depth `.pzp`/`.png`/`_depth.png`, seg `.pzp`/`.png`). `checkPZPD.py` uses the same function.
   - **Changed from the plan:** a sample with an `all` file gets **no depth / seg blobs**, because the DataLoader never reads them then (`multiplexed`). So every record holds `rgb + all` or `rgb + depth? + seg?`, and every DataLoader read is one contiguous span. COCO val2017: 3.24 GB instead of 4.8 GB.
   - A missing rgb aborts. Missing depth / seg / all is allowed, as today.
3. **Tables:**
   - `joints` (global), `image`, `persons` from the `.db` (a keypoint count ≠ 3J aborts)
   - `descriptions` (`source:str, text:str`, one row per sample, source `deepseekvl2`) from the caption file (D24); a sample without a caption, or a caption file with two different texts for one file name, aborts
   - `descriptor_dinov3` (`v:f32[D]`, bulk) when `<db>.dinov3` exists, plus the global `descriptor_info` (`dinov3, D, 1`); `.dinov2` is ignored. The file is read as `load_descriptor_bin` reads it (12-byte header, or legacy 8-byte with D inferred), matched as `readPoseDatabase` matches it (by position, basename checked; fewer descriptors than samples or a wrong name aborts, extra ones are ignored), and normalised as `l2_normalize_descriptor_dataset` does (double sum in order, float inverse, all-zero vectors stay zero)
4. **Safety:**
   - `--dry-run` prints per-stream presence counts, the caption / descriptor files and the estimated size, and writes nothing.
   - Free space in the output directory is checked before writing.
   - After writing, the archive is opened with verification and every blob is verified; any failure (incl. Ctrl-C) removes the source's archive files.
   - On a rerun, a source whose archive exists with the same record count and keys is skipped; one that doesn't match is refused unless `--force` (which rewrites it).
   - **Added 2026-09-23:** the word index `descriptions.text` per `source`, and the histogram tables `hist_rgb` / `hist_seg` / `hist_depth` + `_global` (spec §3.8, computed in worker processes while the records are written, through `pzp.histograms`, the code `pzpdir_histograms.py` now shares; `--no-histograms` skips them). `seg` / `depth` come from the `all` file (channel layout `ALL_SEG_CHANNEL` / `ALL_DEPTH_HIGH_CHANNEL` / `ALL_DEPTH_LOW_CHANNEL` = 0 / 1 / 2 in the script header, `--all-seg-channel`, `--all-depth-bytes`), else from the seg / depth files (16-bit depth required). An older archive without them is refused on a rerun until `--force`. COCO val2017: 13.6 s instead of 6.2 s; every histogram row and global row and the `deepseekvl2` vocabulary identical to `coco_val2017_rgb_all.pzpd` (built from a record list + `pzpdir_histograms.py`); DataLoader batches unchanged. `checkPZPD.py` checks both (`--hist-sample N`, 0 = all + the global rows); `testConvertToPZPD.py`: 43 checks
   - **Fixed 2026-09-23 (DataLoader, branch `pzpdir`): depth from combined `all` files was byte-swapped.** `processing/conversions.h` `splitSegmentationAndDepthFromSingleFileGeneric()` built depth as channel 2 × 256 + channel 1 (it assumed an `[R,B,G]` memory order), while `depthanythingv3/append16bitToSAM3.py` / `compressSegmentDepth.py` write label, high, low in channels 0, 1, 2 (cv2 BGR order). On COCO val2017 the old depth correlated 0.008 with `depth_val2017`, the fixed one 0.967–0.997 (PNG, PZPS, PZPF alike). The layout is now a parameter: `db_set_combined_layout(label, high, low)` / `DataLoader.py combinedChannels=(0, 1, 2)` (default; `(0, 2, 1)` reproduces the old batches byte for byte). The split also steps by the image's channel count (4-channel RGBA combined files, e.g. 300w `combined_output`, were read with stride 3); the AVX2 path handles any layout via `deinterleave3()` and equals the scalar path for all 6 orders. Models trained before this fix learned depth from byte-swapped combined files
5. **`datasets/checkPZPD.py`** (same source options) checks per source: streams and record keys; every blob is exactly the file the rules find (bytes and name, and no blob where no file is expected); the `.db` rebuilt from `joints` / `image` / `persons` equals the `.db` text line for line (token lines excluded); one caption row per record equal to the caption file; `descriptor_dinov3` bit-identical to the normalised `.dinov3` with unit norms, `descriptor_info` consistent, and no descriptor tables without a `.dinov3`.

**Verify** (✅ passed, 2026-09-22):
- ✅ `--dry-run --all` over every `configuration.json` source present here (11 sources, 257 k samples, 24 s): all parse, every sample has a caption (`train2017` and 300w from the `<db dir>/<images dir>DeepSeekVL2descriptions.json` files), every `.dinov3` matches; `generatedTrain.db` is refused because its images are not on this machine (`00000-1022848691.png` missing), which is the intended abort
- ✅ COCO val2017 from the config: written + verified in 10 s (3.24 GB, 5 000 records, rgb + all); `checkPZPD.py`: 10 000 blobs byte-identical, 21 041 `.db` lines identical, 5 000 captions, descriptors bit-identical, 0 failures (7 s)
- ✅ openposeFactory (depth + seg, DINOv3, 64 MB shards → 3 shards) and 300w indoor (PNG rgb, no descriptors) with `--source`: `checkPZPD.py` 0 failures
- ✅ Descriptors of COCO val2017 in the archive are **bit-identical** to the DataLoader's own `load_descriptor_bin` + `l2_normalize_descriptor_dataset` built with its release flags (5 000 × 768, 0 floats differ)
- ✅ `testConvertToPZPD.py`: 31 checks on synthetic sources, 0 failures. They cover three layouts (captions in `<images>/`, next to the `.db` with an extra `prompt` key, legacy 8-byte descriptors), several shards, depth `.png` over `_depth.png`, depth dropped when `all` exists, an all-zero descriptor kept, captions with quotes / newlines / non-ASCII, `.dinov2` ignored; the checker reporting a changed rgb file, a changed caption and an extra source file; every abort above; a failed write leaving no files; config parsing, rerun skip, `--dry-run`, `--force`, and name clashes

### Part B: the DataLoader moves to archives (branch `pzpdir` in RGBToPoseDetect2D, from `refactor`)

**Status (2026-09-22): steps 0–5 implemented and verified on branch `pzpdir` (uncommitted); step 6 not started** (it removes the `.db` path, which is still the only way to train until the datasets are converted on the training PC, and which the equivalence gates compare against). Files: `pzpdir/` (vendored `pzpdir.h`, `pzpdir.c`, `third_party/xxhash.h`, as committed in cffd865), new `PZPDLoader.{h,c}`, edits in `DataLoader.{h,c}`, `DBLoader.h`, `PrepareBatch.c`, `DataLoader.py`, `Makefile`, `makeLibrary.sh`. Deviations from the steps below: one pzpdir handle per DatabaseList, opened in `db_create` (sample counts come from a quick `pzpd_open` in `db_set_source_entry`); the schedule is (re)submitted in `db_start_threads` when the order changed (shuffle counter) or the batch isn't the expected next one; every sample does one `get` with its mask before its first decode (also erased ones, whose combined file is read before the erase decision) and releases after its last; without a prefetcher (`prefetchMode="off"`) the workers decode straight from mmap views. Archive samples fill the existing per-sample kind cache (`depthKind` / `segKind` / `combinedKind`) at load, so the worker's presence logic is unchanged; `PZPC` (PZP containers, e.g. `all_val2017PZPF`) is accepted as PZP. Captions: `PoseEntry.description` points into the archive, `db_get_sample_description()` / `DataLoader.get_sample_description()`; tokens stay empty for archive samples (tokenization is out of scope, step 3).

**Verify** (✅ passed; harness `eqtest.c` in the session scratchpad links `libDataLoader.so` and compares a `.db` + directories DataLoader with an archive one, augmentations off):
- ✅ COCO val2017: `PoseDatabase` identical field by field except tokens (5 000 samples, 11 004 persons, 17 joints, descriptors 768-D bit-identical incl. `db_get_sample_descriptors` per position, 5 000 captions); **165 batches byte-identical** (rgb, 8-bit and 16-bit heatmaps): one sequential epoch through `db_update` + 40 shuffled batches through `StartUpdate` / `CollectUpdate`; prefetcher 6 598 / 6 600 hits
- ✅ openposeFactory (depth + seg, DINOv3) + 300w indoor (PNG rgb, no descriptors) as two sources: identical, in prefetch modes off / auto / map / pagecache / buffers and at 3 and 6 threads
- ✅ AddressSanitizer build (both sources sets, buffers / auto / off; COCO with mmap views only): clean, identical. Found on the way: `db_allocate_source_list` doesn't zero the struct, so the new `archive` pointer is now initialised explicitly
- ✅ Python (`DataLoader.py`, double buffer, short entry `["enabled", "cocoVal.pzpd"]` + a disabled one): 12 batches identical to the `.db` DataLoader, descriptor length 768, captions returned, bad `prefetchMode` refused
- ✅ Throughput, COCO val2017, augmentations on, samples/s (single runs): warm T=6 633 dirs / 638 archive, T=30 398 / 402; cold T=6 574 / 629, T=30 393 / 415. No loss, as expected from §6b (I/O is hidden)
- ⚠ `ignoreNoSkeletonSamples=1` with more than one source aborts **in the existing `.db` path** too: `readPoseDatabase` returns the next free slot including `startOffset` as the source's count and `totalNumberOfSamples` is never reduced, so empty slots get read. The archive loader reproduces the same accounting on purpose (its `PoseDatabase` equals the `.db` one in this mode as well). Training doesn't use the option (`ignoreNoSkeletonTrainingSamples` is false and not passed). Not fixed: pre-existing
- ⚠ Pre-existing, found with the viewer: the `.db` files store person boxes as **corners** (`x1,y1,x2,y2`; all 11 004 COCO val2017 boxes satisfy x1 ≤ x2 ≤ width, y1 ≤ y2 ≤ height, and `genericDatasetParser.get_training_bbox` writes min / max), while `HeatmapGenerator.c:843` reads them as `x, y, w, h` (`x2 = bboxX + bboxW`), so the person-blob centres / sizes are off. Also `genericDatasetParser.py:431` takes y from the x slot (openpose / 300w / generated boxes are wrong either way). The `HeatmapGenerator.c` side is **fixed on branch `pzpdir`** (fields renamed `bboxX2` / `bboxY2`, read as corners; blobs verified on COCO val2017); the generic parser is still open. Written up with the per-.db check and the fix as **A9 in `RGBToPoseDetect2D/knowledge/DATALOADER.md`**
- Archive browser: `scripts/pzpdir_viewer.py` (wxPython) lists records (filter by key), shows every blob and table row of a record, previews any stream (PZP / PZPC natively, others via PIL; 16-bit and 1-channel stretched; persons drawn from the `persons` table), an archive summary (members, shards, storage, AUTO mode, streams, schemas), and saves a blob to a file, the previewed image as .png and the record / archive text as .txt

Every step keeps training usable. The `.db` path stays until step 5's gates pass (D22).

0. **Vendor the library.** Copy `pzpdir.h`, `pzpdir.c` and `third_party/xxhash.h` into `DataLoader/pzpdir/`, built with `PZPDIR_WITH_PZP=0`. Add `pzpdir.c` to `Makefile` and `makeLibrary.sh`, and print `pzpdirVersion` at startup.
   → **gate:** the release, ASan and `-pg` builds all compile with zero warnings, and training output is unchanged.
1. **Sources.** `db_set_source_entry` recognises a `.pzpd` DB path by its magic bytes. Config entries become `['enabled', 'datasets/pzpd/coco_train2017.pzpd']`, and `DataLoader.py` accepts the short form. A set is either all `.pzpd` or all `.db`, and a mix is rejected. All sources open as one handle (`pzpd_open_many`, aliases = source names), and member index = `sourceID`.
2. **`PZPDLoader.c` (new, ~100 lines).** It fills `PoseDatabase` from the tables:
   - `imagePath` = key; `width` / `height` from `image`
   - `sk[]` from `persons`; `joint[]` from `joints`
   - `descriptor` = a pointer into `descriptor_<model>` (zero-copy), with D from the schema

   `ignoreNoSkeletonSamples` goes through a filter map (sample → ordinal). Presence (`hasAll`, `hasDepth`, `hasSeg`) and dims / channels / bits are checked from the index at load; today these checks abort mid-epoch. It aborts when:
   - `descriptor_info.l2norm` ≠ `L2_NORMALIZE_DESCRIPTORS`
   - D > 4096
   - the sources' joint sets differ
   - `addGeolocation` or `addSuperpoint` is set

   Caption strings are handed on as strings (pointers into the `descriptions` table); the archive has nothing to do with tokens.
   → **gate:** it equals the `.db`-built `PoseDatabase` field by field, incl. `ignoreNoSkeletonSamples`, except for tokens: the `.db` carries pre-computed token IDs, the archive the caption strings, which are compared with the caption files instead.
3. **Tokenization: out of scope for this work.** The archive and the adapter deliver caption strings only. Resolving them to tokens (vocabulary, tokenizer, train / validation consistency) belongs to the DataLoader and is handled separately. No legacy tokenizer mode is needed (existing checkpoints are not kept compatible).
4. **Read path (no prefetcher yet).** For archive sources, `PrepareBatch.c` skips `resolveSampleFiles` / `signalPrefetchFile`: it takes the stream's bytes (`pzpd_view` / `read_record`) and decodes with `readImageFromMemory()`, with the codec chosen from the blob's FourCC. Per-sample mask: `rgb | (hasAll && (DO_DEPTH || DO_SEG) ? all : depth? | seg?)`; since the converter stores no depth / seg next to an `all` file, every mask is one contiguous span.
   → **gate:** identical batches for a fixed seed, `.db` + directories vs archive, incl. the erase / background paths (image and heatmap outputs; token outputs are excluded, tokenization is out of scope, step 3).
5. **Prefetcher.**
   - Created after the DB load.
   - After every shuffle (already drained): `clear`, then `submit` the whole epoch (`indices[pos]` → filter map → ordinal, plus masks). A non-sequential `StartUpdate` re-anchors the schedule.
   - Workers call `get` → decode → `release`; erased samples are `discard`ed.
   - Destroyed in `db_destroy` before `pzpd_close()`.
   - The validation DB gets its own prefetcher on its own handle.
   - New setters: `db_set_prefetch_mode`, `db_set_prefetch_budget`, `db_set_prefetch_io_threads`. Look-ahead ≥ batch k+2; AUTO picks the mode per member (§5, D15).

   → **gate:** batches still identical, ASan clean, and throughput with the §6b harness at 6 and 30 threads, cold and warm, is no worse than directories.
6. **Remove the legacy path.** Delete:
   - the DB1 parser and descriptor matching
   - `resolveSampleFiles` / `resolvePathToRequestedFiles` / `DL_FileKind`
   - `signalPrefetchFile`
   - `USE_RAM_CACHE` / `cache.c` / `preloadAllFiles` (confirmed 2026-09-22; replaced by staging the archive to `/dev/shm`)
   - geolocation and SuperPoint loading
   - the directory columns of `db_set_source_entry`

   Then update `checkDatasets.py`, the `configuration.json` dataset entries, the other training scripts' dataset handling if any differs, and the README (convert, check, stage).
   → **gate:** training and validation run from archives only, and the gates above still pass against a saved reference (batch hashes and `PoseDatabase` dump from before the removal).

**Removed per sample:** 2–4 × (probe + open + fadvise + read + DONTNEED + close). In their place: one pread done ahead of time by an I/O thread.

## 8. Test and benchmark dataset

COCO val2017 at `RGBToPoseDetect2D/datasets/coco/cache/coco` (Samsung 980 PRO NVMe,
ext4, 16 cores, 31 GB RAM). There are 5 000 samples, and every stem is present in every stream.

| Source | Location | Size | Avg per sample |
|---|---|---|---|
| rgb | `val2017` (+4 json, skipped) | 818 MB | 164 KB |
| all | `all_val2017` → `all_val2017PZPF` | 2.39 GB | 478 KB |
| geo | `geo_val2017` (`x.jpg.pzp`) | 32 MB | 6.5 KB |
| depth | `depth_val2017` | 1.52 GB | 304 KB |
| seg | `segment_val2017` | 44 MB | 8.8 KB |
| annotations | `coco/cocoVal.db` (`DB1`, 17 joints) | 2 MB | ~0.4 KB |
| captions | `val2017/descriptions.json` (DeepSeek-VL2) + `descriptionsOLD.json` | 0.4 MB+ | ~0.1 KB |
| descriptors | `coco/cocoVal.db.dinov2`, `coco/cocoVal.db.dinov3` (768 × f32 each) | 15 MB each | 3 KB |

- **Archive location:** planned as `coco/cache/coco/pzpd/` on the same NVMe; they were built in a session scratch directory under `/tmp` instead and **moved on 2026-09-22 to `/media/ammar/games/PZPD_Test/`** (a 7200 rpm WD1001FALS hard disk, ext4): `coco_val2017.pzpd` (5 streams, 4.8 GB, 2 shards), `cocot.pzpd` (the same + tables), `sub60`, `syn`, the collections `cocoset` / `smokeset` (members stored relative, so they moved along), and the record lists `val2017.tsv` / `val2017_tables.tsv`. All verified with `verify --blobs` after the copy. NVMe measurements need a copy on the NVMe first (4.8 GB; `/` has 30 GB free, `/home` 8 GB).
- **Shard boundaries:** `--shard-size 256M` / `64M` to exercise them.
- **Cold runs (NVMe):** evict with `posix_fadvise(DONTNEED)` and confirm < 1% is still cached.
- **Variants (NVMe):** `fs-open`, `fs-probe`, `pzpd-blob`, `pzpd-record`, `pzpd-view`, `pzpd-pf-pagecache`, `pzpd-pf-buffers`.
- **RAM-disk column:** archive and source dirs copied to `/dev/shm` (16 GB tmpfs) and to a `tmpfs -o huge=within_size` mount. Variants: `fs-open`, `pzpd-record`, `pzpd-view`, `pzpd-pf-map` (± huge pages). Measure minor faults and dTLB misses (`perf stat`) per sample, and staging time (`cp` shards vs `cp -r` of 25 000 files).
- **Workloads:** read sets rgb+all+geo and rgb+depth+seg+geo, T ∈ {1, 4, 8, 16}; `dataloader-replay` = the exact worker trace (strided, per-sample masks, k+1 double buffer); `--decode` for end-to-end.
- **Reported:** samples/s, MB/s, p50/p99 latency, syscalls per sample, over-read bytes, open time, pack time, size vs `du`. Also startup time to build `PoseDatabase` from `cocoTrain.db` (49 MB text parse) vs from archive tables.
- **Correctness:** `diff -r` after `unpack`, SHA-256 of every blob, `find()` resolves all 25 000 original names, metadata matches full decodes, and the table gates of phase 1c.
- **Regression fixture:** the record list produced by `scripts/pzpdir_list_from_db.py` for val2017 (5 streams, 25 000 lines, 3.9 MB, with this machine's absolute source paths) is kept **outside git**, next to the archives: `/media/ammar/games/PZPD_Test/val2017.tsv` (decided 2026-09-22: data lists don't belong in the tree). The tables list (91 MB, with descriptors) is there too, as `val2017_tables.tsv`.
- **Hard disk (2026-09-22, the WD1001FALS above, cold, rgb+all+geo, 1 000 of the 5 000 shuffled samples):** copying the 9.8 GB of test archives onto it: 68 MB/s including `sync`; sequential read (`verify --blobs`, 4.8 GB): 73 MB/s ≈ 112 samples/s. Shuffled reads are seek-bound, about 60 % of that, and more consumer threads don't help (one spindle):

  | T | pzpd-record | pf-pagecache | pf-buffers |
  |---|---|---|---|
  | 1 | 58 /s (p50 16.9 ms) | 68 /s | 69 /s |
  | 8 | 71 /s | 52 /s (457 sync misses) | 73 /s |

  More I/O threads let the drive's queue (mq-deadline, NCQ depth 32) reorder requests: BUFFERS at T=1 gives 60 / 69 / 72 / 72 /s with 1 / 4 / 16 / 32 I/O threads. At ~70 samples/s the DataLoader (≈ 480 /s at 6 workers, §6) would be I/O-bound 7×, so a hard disk only suits a first epoch whose member fits in RAM (AUTO → PAGECACHE, later epochs come from the page cache) or cold storage that is staged to NVMe / RAM with one sequential copy (4.8 GB ≈ 66 s here). Reading each window of the schedule in file order could at most approach the sequential ~112 /s.
- **Results of the remaining §8 items (2026-09-22; NVMe = a copy of `coco_val2017` in `/tmp` on `/`, sources on `/home`, same Samsung 980 PRO):**
  - **`find(name)`:** all 25 000 original names resolve to the right record and stream, all 5 000 keys to the right record (`val2017.tsv`, Python, 8 µs per lookup + check)
  - **Pack time:** 14.3 s to the NVMe (336 MB/s, sources evicted first), 80.9 s to the hard disk (59 MB/s), incl. `sync`. **Size:** archive 4.816 GB = sources' data + 0.33 % (4.800 GB, `du -sb`); on disk 0.74 % *smaller* than the 25 000 files (4.852 GB, `du`: 4 KiB block rounding)
  - **Staging to `/dev/shm`** (cold, 4.8 GB): `cp -rL` of the 5 source dirs (25 004 files) 9.2 s (520 MB/s); `cp` of the 3 archive files 4.4 s (1.09 GB/s), **2.1× faster**; from the hard disk 65.5 s (74 MB/s)
  - **`PoseDatabase` startup, COCO train** (118 287 samples, 262 465 persons, 17 joints, DINOv3 768-D; the DataLoader's own `createPoseDatabase` / `load_descriptor_bin` / `readPoseDatabase` from a scratch `libDataLoader.so` vs the same structs filled from archive tables the way the planned adapter would; GloVe embeddings excluded, same for both at ~0.05 s):

    | | cold | warm |
    |---|---|---|
    | `.db` (49 MB parse 0.50 s + 365 MB `.dinov3` load 0.62 s) | 1.15 s | 0.96 s |
    | archive, descriptors zero-copy | **0.05 s** (22×) | **0.03 s** (32×) |
    | archive, descriptors L2-normalised into a copy at load | 0.48 s (2.4×) | 0.32 s (3×) |

    Joints, image paths, sizes and every skeleton are identical (per-field hashes); descriptors are identical once normalised as the DataLoader does (see §7, caveat). Not compared: the `.db` also carries pre-computed token IDs, the archive caption *text* (tokenizer not written yet; COCO train has no captions file here). Zero-copy descriptors are read lazily during the epoch instead of at startup. The train archive holds empty stand-ins for the images (real names; startup never reads payloads): `startup/cocotrain.pzpd` on the hard disk, with the two drivers. Making the tables list took 394 s in Python (1.1 GB of text, mostly descriptors), packing 14 s
  - **Read matrix, cold, samples/s, 5 000 shuffled** (`fs-probe` = `fs-open` after the DataLoader's first-touch stat() probing, 6 stats / sample here; `pzpd-blob` = one `pread` per stream):

    | set A rgb+all+geo | T=1 | T=4 | T=8 | T=16 |
    |---|---|---|---|---|
    | fs-open | 1 089 | 3 510 | 5 581 | 7 427 |
    | fs-probe | 1 051 | 3 423 | 5 476 | 7 401 |
    | pzpd-blob | 1 567 | 4 353 | 5 890 | 6 522 |
    | pzpd-record | 1 643 | 4 440 | 5 796 | 6 560 |
    | pzpd-view | 1 535 | 4 277 | 5 558 | 6 148 |
    | pf-pagecache | 3 532 | 4 081 | 5 681 | 6 262 |
    | pf-buffers | **9 371** | **9 498** | **9 909** | **9 912** |

    | set B rgb+depth+seg+geo | T=1 | T=4 | T=8 | T=16 |
    |---|---|---|---|---|
    | fs-open | 1 053 | 3 601 | 5 932 | 8 624 |
    | fs-probe | 1 037 | 3 556 | 5 832 | 8 600 |
    | pzpd-blob | 1 291 | 4 173 | 6 370 | 7 798 |
    | pzpd-record | 1 432 | 4 161 | 5 121 | 5 648 |
    | pzpd-view | 1 473 | 4 377 | 6 258 | 7 344 |
    | pf-pagecache | 4 353 | 4 901 | 6 574 | 7 521 |
    | pf-buffers | **7 860** | **8 233** | **10 446** | **11 069** |

    Probing costs 1–3 % with warm dentries (cold ones need root). In set B one `pread` per record reads the ~478 KB `all` blob between rgb and depth, so `pzpd-record` falls behind `pzpd-blob` at T ≥ 8; the prefetcher reads ranges > 256 KB apart separately. At T = 16, plain files still beat single archive reads (as in phase 1); BUFFERS + `O_DIRECT` beats everything at every T
  - **End-to-end with decode** (`--decode`: libjpeg, libpng, this repo's `pzp.h`; stand-ins for the DataLoader's codecs, no augmentation), set A, cold: fs-open 317 / 2 122 / 3 341, pzpd-record 357 / 2 387 / 3 372, pzpd-view 374 / 2 427 / 3 517, **pf-buffers 450 / 2 787 / 4 243** samples/s at T = 1 / 8 / 16 (+42 / +31 / +27 % over fs-open); decode ≈ 2.5–3.6 ms CPU per sample in every mode
  - **Not measurable here:** dTLB misses (`perf` is installed but `perf_event_paranoid = 4`), cold dentry / inode caches (`drop_caches`), both need root

## 9. Risks and mitigations

| Risk | Mitigation |
|---|---|
| `/home` is 99% full (8.2 GB free) | Pack test archives one at a time; 3-stream fallback; pack ImageNet on another volume; delete sources only after `verify` passes |
| Whole COCO set fits in RAM, so warm numbers are meaningless | Mandatory cold-cache protocol + residency check; cgroup memory limit for BUFFERS |
| `O_DIRECT` unsupported (tmpfs, FUSE) | Buffered fallback, reported in stats; on tmpfs AUTO uses MAP |
| Wrong mode on a RAM disk (BUFFERS doubles RAM use, `fadvise` is a no-op) | AUTO detects tmpfs/ramfs per shard; `info` and `stats` show the chosen mode |
| Page faults / TLB misses dominate on tmpfs | `MADV_POPULATE_READ` ahead; huge-page tmpfs + `PZPD_O_HUGEPAGE`; `PZPD_O_POPULATE` |
| `brd` / `zram` keep data twice (device + page cache) | Treated as BLOCK; tmpfs documented as the supported RAM disk |
| RAM-disk archives occupy RAM; tmpfs is volatile | Only hot, small datasets on the RAM disk (mixed collections); the NVMe copy is the source of truth |
| Random erase decision wastes prefetched I/O | `discard`; cost = `chanceDestroy` × background fraction (0 by default) |
| First batches of each epoch are cold (next order unknown) | Accepted for v1; later: compute the next permutation early |
| Key collisions across dataset sources | Kept by design; `find_in(member)` with member = `sourceID` |
| Format mistakes found late | Magic + version on every file kind; fuzzing from phase 1 |
| Format detection wrong or ambiguous (CSV vs TEXT, truncated headers) | Magic bytes first, extension only as fallback, `RAW ` when unsure; `blob_ex` override; metadata checked against full decodes |
| Index growth from metadata (32 B per blob) | ≈ 0.2 % at ImageNet scale; mmapped and paged lazily |
| Token IDs go stale when the dataset mix changes | Descriptions stored as text; vocabulary built at load, or pinned for existing checkpoints |
| C tokenizer diverges from `buildVocabulary.py` (Unicode `\w`, `lower()`) | Exact-parity integration gate on all description sets. Found in phase 1c: COCO val2017's `vocabulary.json` was built before `buildVocabulary.py` dropped non-ASCII words (7 samples differ). The DataLoader's tokenizer must match the tokenizer version of the checkpoint's vocabulary (legacy = keep non-ASCII) **Out of scope since 2026-09-22 (§7 step 3): tokenization is handled on the DataLoader side, and no legacy mode is needed** |
| Descriptor length differs per model (DINOv2 / v3 / others) | D from each table's schema; one table per model; members must agree per table name |
| Losing annotations is costly (keypoints can't be regenerated) | Non-bulk rows copied into every record header (`salvage`); global tables in every shard + manifest |
| Schema mismatch across collection members (e.g. different joint sets) | Checked at open; per-member `pzpd_global_rows`; the loader checks joint sets match |
| DataLoader's vendored `codecs/pzp.h` differs from this repo's | `pzpdir` needs no `pzp.h` on the read path (`PZPDIR_WITH_PZP=0`); FourCC metadata is written at pack time |
| Per-TU `static` ZSTD context in `pzp.h` leaks if `pzpdir` decodes on DataLoader threads | `pzpdir` never decodes there (raw bytes only; the DataLoader decodes via its own codecs) |
| Vendored copies drift from this repo (as `codecs/pzp.h` did) | `pzpdirVersion` string printed by the DataLoader at startup (as it prints the pool version); spec v0.4 magic + version checked on open; `check_client_build.sh` in this repo |
| Grouping logic spread across many writer scripts | Intended: the archive stays simple; each dataset gets a small script, and the record list is the one shared interface |

## 10. Open questions

None. All earlier questions are resolved (D17–D19). New ones will be added here
as they come up during implementation, and spec changes are recorded as revisions in its changelog.

**Later (not v1):** SuperPoint as a `bulk` table; appending records; per-blob
in-place replace; `io_uring` backend; decode inside the prefetcher (the next lever
on RAM disks, where workers become decode-bound); next-epoch warm-up; vocabulary
caching; `pzpd_member_has_stream()` if a use appears; macOS / Windows.
