# PZPD — PZP Directory Archive (spec v0.4, frozen for implementation)

Status: **frozen as the reference for implementation** (2026-09-21). Nothing
is implemented yet. Decisions are in §11. Changes from here on are explicit,
dated revisions, recorded in the changelog below.

**Changelog**
- **v0.2:** one archive per modality ("aligned sets"), self-contained shards + manifest, arbitrary filenames, `pzpdir` tool naming.
- **v0.3:** files of one sample **co-located in one record** (one read per sample), replacing per-modality archives.
- **v0.4:**
  - grouping decided by the writing application (Writer API / record list), with no grouping rules in the archive; `.db` sample order kept for `.db`-backed datasets;
  - collections (several archives as one; absolute member paths);
  - 8-byte file magics + versions; FourCC blob formats with index-resident metadata;
  - storage awareness (MAP mode for RAM disks, AUTO per shard);
  - typed annotation tables (`joints`, `image`, `persons` u16, `descriptions` as text, `descriptor_<model>` with schema-defined D) replacing `.db` and descriptor files, with cheap table edits;
  - DataLoader access plan;
  - packaging as `pzpdir.h` + `pzpdir.c` (`libpzpdir.so` / `.a`, CLI `pzpdir_cli.c`), vendored xxHash;
  - stream/table names ≤ 23 bytes;
  - collection write/refresh C API, `find_all` returns the total match count.
- **v0.4 revision 1 (2026-09-21):** compile switch `PZPDIR_WITH_PZP` (default 1). With 0,
  `pzpdir.c` doesn't include `pzp.h`. `pzpd_read_pzp()` is then unavailable, and PZP
  header probing is skipped when writing (the metadata comes from `blob_ex` or stays
  invalid). Reading is unaffected, since FourCC metadata is stored at pack time.
  Needed by the first client (the DataLoader), which vendors its own `pzp.h`.
- **v0.4 revision 2 (2026-09-21), from the phase-1 implementation (`src/pzpdir/`):**
  - **API names:** `pzpd_blob_info_get()` and `pzpd_shard_info_get()`. C can't have a function
    and a typedef with the same name (`pzpd_blob_info`).
  - **Non-const handles:** `pzpd_find()`, `pzpd_record_key()`, `pzpd_record_span()` and
    `pzpd_blob_info_get()` take a non-const `pzpd *`, because they may open a shard lazily.
  - **Format detection:** `pzpd_detect_format(data, size, name, name_len, meta)` takes the file name,
    used only for CSV/TSV and as the fallback for unrecognised content.
  - **PZP probing:** reads the inner 40-byte frame header by partially decompressing with zstd / lz4
    directly, so it works with `PZPDIR_WITH_PZP=0` too. The revision-1 note that probing is skipped
    no longer applies; `PZPDIR_WITH_PZP` only gates `pzpd_read_pzp()`.
  - **Index sections:** every one starts with a 32-byte header
    `{ "PZPDSECT", u32 version, u32 kind, u64 bytes, u64 XXH64 of the data }`. The kinds are
    record table, blob table, hash, heap, meta; for manifests, shard table, shard names, global
    hash. Superblock and manifest `*_offset` fields point at the section **data**, right after
    its header. The shard `index_checksum` covers the data of the record, blob, hash, heap and meta
    sections, in that order.
  - **Exact byte layouts** are defined, and size-checked with `_Static_assert`, in `pzpdir.c`:
    - shard superblock: adds `hash_count` and `file_bytes`; `sb_checksum` at byte 1728;
    - record header: 40 bytes, followed by one 40-byte descriptor per blob
      `{stream, meta_flags, bits, format, rel_offset, size, width, height, channels, frames, name_len, xxh32}`,
      then the key and the names, zero-padded to a multiple of 64;
    - manifest header, shard entry (48 B), global hash entry (24 B).
  - **Standalone shards:** a shard opened on its own numbers its records 0..n-1 (local ordinals).
  - **Writer semantics:**
    - a rejected blob leaves the record open with its earlier blobs;
    - `pzpd_writer_end()` rejects a duplicate key or name by dropping the whole record, and the
      writer stays usable;
    - a failure that leaves the index half-updated (out of memory, a failed shard switch) marks the
      writer broken, and every later call fails.
  - **Limits:** a blob is < 4 GiB - 4 KiB, a record < 4 GiB, and a shard's string heap < 4 GiB.
  - **Record lists:** a trailing `\r` is data, not stripped. Lists must use `\n` line endings.
- **v0.4 revision 3 (2026-09-21), from the phase-1b implementation (collections):**
  - **Collection file layout** (sections `PZPDSECT` kinds 9–11): a 4 KiB header
    `{"PZPDCOLL", version, total_records, member_count, stream_count, merged stream table, section offsets, checksums}`,
    a member table of 56-byte entries `{first_ordinal, record_count, archive_uuid, path, alias, flags (bit0 absolute path)}`,
    a heap with paths and aliases, and per member 32 bytes mapping merged stream → member stream (0xFF = none).
  - **Stored paths:** relative to the collection file's directory when the member lives under the same
    top-level directory (e.g. both under `/home`), otherwise absolute (e.g. `/dev/shm`). `PZPD_COLL_ABSOLUTE` /
    `--absolute` forces absolute paths; `refresh` keeps each member's choice.
  - **Staleness** = the member's uuid, record count or stream mapping differs from the collection file.
    Opening fails with `PZPD_E_STALE_COLLECTION` (not a per-member error), because ordinals could otherwise shift silently.
  - **Lookup order:** `pzpd_find()` searches record keys in all members (in order) before blob names;
    `pzpd_find_all()` lists key matches first, then name matches.
  - **Additions:** `pzpd_member_count()`, and `member` fields in `pzpd_blob_info` / `pzpd_shard_info`
    (shard indices run over all members' shards). `pzpd_member_has_stream()` was not added (D19).
  - **CLI:** with several members, `ls` prefixes `alias<TAB>` and `unpack` writes into `<dir>/<alias>/`.
- **v0.4 revision 4 (2026-09-22), from the phase-1c implementation (tables):**
  - **Table directory:** 16 slots of 48 bytes in the shard superblock and in the manifest header,
    `{name[23], u8 flags, u64 section_offset, u64 section_bytes, u32 row_stride, u32 pad}`; unused slots are zero.
    `section_offset` points at the section data (as for every other index section).
  - **Table section** (`PZPDSECT` kind 12): a 96-byte head
    `{name[24], u32 flags, u32 ncols, u32 row_stride, u32 pad, u64 records, u64 rows, u64 index_offset, u64 rows_offset, u64 heap_offset, u64 heap_bytes, u64 reserved}`,
    then `ncols` 32-byte columns `{name[24], u8 type, u8 pad, u16 count, u32 offset}`, then the row index
    (record tables only: `records + 1` × u32, CSR), the rows and the string heap. Offsets are relative to the
    section data and each part is 8-byte aligned. Global tables have no index (`index_offset` = 0, `records` = 0).
    The manifest holds full copies of global tables and **schema-only** copies of record tables (`rows` = 0).
  - **Checksums:** a shard's `index_checksum` covers the record, blob, hash, heap and meta section data and then each
    table section in directory order; a manifest's covers its shard table, names, hash and then its table sections.
  - **Record-header row copies:** `table_bytes` in `PZPDRECD` (was reserved) = bytes of copied rows. After the key and
    names, at an 8-aligned offset, one block per non-bulk record table with rows for that record:
    `{u32 table, u32 rows, u32 rows_bytes, u32 str_bytes}` + rows + strings, padded to 8. `str` offsets in copied rows
    are relative to the block's strings. The header's XXH32 covers them. Bulk and global tables are not copied.
  - **Row-index validation is lazy:** at open only `index[0] == 0` and `index[records] == rows` are checked (O(1));
    every per-record lookup checks `start ≤ end ≤ rows`; `pzpd_table_shard_view()` checks the whole index once per shard.
    A malformed index therefore fails the lookup / view that meets it (`PZPD_E_FORMAT`), not the open.
  - **Collections:** tables merge by name over members (first-seen order); a same-named table with a different schema
    (columns, types, counts, flags) fails the open with `PZPD_E_FORMAT`. A member without a table reads as 0 rows and its
    shards give `row_index = NULL` in `pzpd_table_shard_view()`.
  - **API additions / changes:**
    - `pzpd_writer_rows(w, table, rows, nrows, strings, strings_len)` and `pzpd_writer_global_rows(...)`: rows in C layout;
      a `str` field's `{offset, len}` refers to the caller's `strings` buffer (the writer copies into the heap).
      `pzpd_writer_global_rows_csv()`. Tables must be declared, and global rows added, before the first record
      (`PZPD_E_STATE` otherwise). A rejected row batch leaves the record open without those rows.
    - Reader: `pzpd_global_str()`, `pzpd_global_csv()`; `pzpd_table_shard_view()` fills a `pzpd_table_view`
      `{first_ordinal, records, row_index, rows, total_rows, strings, strings_len}`; `pzpd_table_rows()` /
      `pzpd_global_rows()` return 0 both for "no rows" and on error (then `pzpd_last_error_code() != PZPD_OK`).
  - **CSV:** RFC 4180 (`"` quoting with `""`; quoted fields may hold commas, newlines, `\r`). A quote inside an unquoted
    field is an error. Integers are range-checked, never wrapped. `f32` / `f64` are written as the shortest `%g` text
    that reads back bit-exactly (`-0` stays `-0`); `str` fields are quoted only when needed (empty, `,` `"` `\n` `\r`,
    or leading / trailing space). Errors name the column and, in record lists, the line.
  - **Limits:** ≤ 16 tables, ≤ 64 columns, table and column names ≤ 23 bytes; per shard and table < 4 G rows and
    < 4 GiB of strings.
  - **CLI:** list directives `@table NAME schema [bulk]`, `@global NAME schema`, `@row NAME csv`, and
    `key<TAB>table<TAB>csv` row lines; `cat --table T`; `info` lists each table's kind, rows, stride and schema;
    `export-table` writes re-packable lines (`key<TAB>table<TAB>csv` for record tables, `@row table csv` for global
    ones; with several members each line is prefixed by `alias<TAB>`). A CSV row containing newlines is escaped (`\n`)
    so each exported row stays on one line.
- **v0.4 revision 5 (2026-09-22), from the phase-6 implementation (prefetcher PAGECACHE + MAP + AUTO):**
  - **Names:** `pzpd_prefetch_stats_get()` (a function can't share the name of the `pzpd_prefetch_stats` typedef,
    as in revision 2). `pzpd_storage_kind(pzpd *a, ordinal)` takes a non-const handle (it may open the shard) and returns
    `int`: an `enum pzpd_storage` value or a negative error. `pzpd_shard_info` gains `storage`.
    New: `pzpd_prefetch_auto_mode(a, shard)`, the mode AUTO gives a shard (used by `pzpdir info`).
  - **Options** `pzpd_prefetch_opts { stream_mask, io_threads, mode, budget_bytes, window }`; zero = default
    (all streams, 4 threads, AUTO, 512 MiB, 256). `PZPD_PF_BUFFERS` is rejected until phase 7; AUTO gives such shards
    PAGECACHE and counts them in `stats.auto_fallbacks`.
  - **MAP and PAGECACHE both hand out mmap views** (valid until `pzpd_close()`); the I/O threads only make the
    records resident ahead of the consumers. MAP: `MADV_POPULATE_READ` on the record's ranges (a read per page
    before Linux 5.14). PAGECACHE: `MADV_WILLNEED` on all ranges first (queue depth), then `MADV_POPULATE_READ`, so a
    prefetched record is resident *and* its page tables are filled when the get arrives.
  - **Window semantics:** `window` bounds the entries whose prefetch was started and whose claim was not yet released
    or discarded. Claims held by workers count, so a worker that never releases stalls the I/O threads (counted in
    `producer_stalls`), never the other workers.
  - **Claims:** a get takes the ordinal's first free claim, in submit order. Prefetched → hit; in flight → wait;
    still queued → `sync_miss` (the I/O threads then skip it); no free claim → `unscheduled`. Tickets carry the
    schedule generation, so releasing a ticket from before `clear` does nothing; a second release does nothing.
    `get` returns the number of present wanted streams, or a negative error (the claim is then released).
  - **Over-read:** requested blobs of a record closer than 256 KB are prefetched as one range; the gap bytes are
    counted in `bytes_over_read`.
  - **Open flags:** `PZPD_O_HUGEPAGE` = 8 (`MADV_HUGEPAGE` on shard mappings), `PZPD_O_POPULATE` = 16 (every shard is
    opened at open time and fully pre-faulted).
  - **CLI:** `info` prints each shard's storage (`ram` / `block`) and the mode AUTO gives it.
- **v0.4 revision 6 (2026-09-22), from the phase-7 implementation (BUFFERS + O_DIRECT):**
  - `PZPD_PF_BUFFERS` is accepted (revision 5's rejection and `stats.auto_fallbacks` are gone); AUTO gives large
    block-device members BUFFERS.
  - **Buffers are per record**, page-aligned (`posix_memalign`), not a pre-carved slab arena: same budget semantics,
    no fragmentation tuning. `budget_bytes` counts prefetched buffers **and** buffers held by tickets; a record larger
    than the budget is still read when nothing else is held.
  - **Reads:** each range is rounded out to 4 KiB and read with `O_DIRECT` (whole blocks; the last may end at EOF);
    a shard whose `O_DIRECT` read fails with `EINVAL` switches to buffered `pread` for good (`stats.direct_fallbacks`).
    BUFFERS never touches the mapping, so the page cache doesn't grow.
  - **Tickets own buffers:** `pzpd_ticket` gains `buf` / `bytes`. On BUFFERS shards `refs` point into the ticket's buffer
    and are valid until `pzpd_prefetch_release()`, which also works for tickets from before a `clear`. Release every
    ticket before `pzpd_prefetcher_destroy()`.
  - A get whose mask is wider than the submitted one, a get before the I/O threads reached its claim, and a claim
    whose prefetch read failed are served by a synchronous read into a new ticket buffer. `PZPD_O_VERIFY` also checks
    BUFFERS blobs.
  - **Stats:** `shards_buffers`, `direct_fallbacks`, `buffer_bytes` (now), `buffer_bytes_peak`.
- **v0.4 revision 7 (2026-09-22), from the phase-2 implementation (recovery):**
  - **Metadata JSON** also holds `first_ordinal`, `align` and `generation`, so a shard with both superblocks gone can
    place itself; for shards written before, `first_ordinal` comes from the manifest (standalone: 0).
  - **Step 2 details:** the section scan runs backwards from EOF over 4 KiB boundaries, accepts a section only if its
    XXH64 matches, and stops at the first record-table section it meets. Kinds 2–5 are the first of each after it.
    Tables: per name, the newest section (table edits append and leave the old one until `compact`) in the slot of
    that name's first section; a table dropped since the last `compact` comes back. Stream names come from the metadata (placeholders `streamN`
    when they're lost); `index_checksum` is recomputed. It runs automatically on open; `pzpd_shard_info.recovery`
    reports 0 (primary), 1 (backup) or 2 (sections). A file whose first bytes aren't a known magic is tried as a shard.
  - **Table directory check:** a table section's name, flags and row stride must equal its directory slot.
  - **API:** `pzpd_manifest_rebuild(manifest, shards, n)` (the result is byte-identical to the writer's: same manifest
    writer, generations and index checksums from the shards); `pzpd_salvage(shard, schemas, fn, user, info)` with
    `pzpd_salvaged_record` / `_blob` / `_rows` and `pzpd_salvage_info`. The record-header scan steps by 64 bytes,
    trusts a header only when its XXH32 matches and then skips the record's payloads.
  - **CLI:** `salvage <shard> <dir> [--schemas <archive>] [--list out.tsv]` writes intact blobs plus a record list for
    `pack` (unsafe names are written under a sanitised path and keep their original name in the list; damaged blobs
    are reported, not written); `rebuild-manifest <name>.*.pzpd [--out <name>.pzpd]`.
- **v0.4 revision 8 (2026-09-22), from the phase-3 implementation (edits):**
  - **Generations on read:** a shard's primary superblock is used unless the superblock in the file's last 4 KiB is
    valid, of the same archive, of a **higher** generation, and ends exactly at EOF; then that one is used (an edit
    that stopped between appending and flipping). The next edit or compact rewrites the primary to match.
  - **Table edits** append the new section(s) and the new backup superblock at the old generation's `file_bytes`,
    truncate any leftover tail, fsync, write the primary, fsync. Unchanged tables keep their section offsets.
    **Compact** does the same with copies of all live sections (generation + 1), then writes them right after the
    last non-table index section as generation + 2 and cuts the file after the flip. Neither touches record data.
  - **Stream edits** rewrite each shard as `<shard>.rewrite.00000.pzpd` (generation + 1; uuid, shard index, first
    ordinal, shard count and total unchanged), verify it (index and every payload checksum), and rename it over the
    shard. Copied blobs keep their stored metadata bits exactly; new files are detected. The new stream is appended
    last; a dropped one is removed from the stream table (stream ids after it shift).
  - **Resume:** rerunning the same command finishes an interrupted edit: add / drop are recognised by content, a
    replaced stream by the shard's generation being ahead of the manifest's, a replaced table is simply redone.
    Until then the manifest may be stale for edited shards (their tables / streams differ).
  - **Limits:** record-header row copies keep the packed rows (salvage after a table edit recovers the packed
    version; after `drop-table`, the copies' table ids refer to the packed table list). Metadata JSON keeps the
    pack-time generation. Collections containing the archive need `collect --refresh` after a stream edit.
  - **API:** `pzpd_edit_table()`, `pzpd_edit_stream()`, `pzpd_compact()`, `PZPD_EDIT_ADD / _REPLACE / _DROP`,
    `PZPD_EDIT_KEEP_MISSING`, `PZPD_EDIT_DROP_MISSING`, `pzpd_edit_rows`, `pzpd_edit_blob`.
  - **CLI:** as §9: `add-table | replace-table <archive> T <list|->` (the list's `@table` / `@global` line gives the
    schema; `key<TAB>T<TAB>csv` and `@row T csv` lines), `drop-table`, `add-stream | replace-stream <archive> S <list|->`
    (`key<TAB>S<TAB>path[<TAB>name]` lines), `drop-stream`, `compact`, each with `--missing` where it applies.
- **v0.4 revision 9 (2026-09-22), from the phase-5 implementation (Python):** `src/pzp/pzpdir.py` as §8, with these
  details: `find()` raises `KeyError` (keys only; `find_name()` / `find_all()` for names); `table()` / `global_table()` /
  `table_all()` return **copies** (numpy structured arrays, `str` columns decoded), so no array can outlive its archive;
  `Prefetcher.get()` returns a `Record` (mapping stream → memoryview) released by `with` / `release()` / garbage
  collection; `prefetcher(..., mode="auto|map|pagecache|buffers", budget_mb, window)`; errors raise `PzpdError` with
  `.code`. `read_group()` comes with phase 4. `libpzpdir.so` is packaged next to the module by `setup.py`.
- **v0.4 revision 10 (2026-09-22), from the phase-4 implementation (video groups):**
  - **Group table** (§4.6) is section kind 13, **24-byte** entries `{u32 group_id, u32 first_local, u32 frame_count,
    u32 name_offset, u32 name_len, u32 pad}` in record order (names in the shard heap). It follows the metadata section and
    is hashed into `index_checksum` after it (shards without groups are unchanged). `superblock.flags` bit0 = has groups.
    There is no manifest group table: group **names** are hash entries of kind 2 (`local_ordinal` / `ordinal` = the
    group's first record) in the shard and global hashes.
  - **Groups are identified by their first ordinal** (unique in a collection): `int64_t pzpd_group_find(a, name, len)`
    returns it; `int pzpd_group_info(a, ordinal, pzpd_group *)` takes any record of the group
    (`{first_ordinal, frames, id, index, name, name_len}`; returns 0 with no error for a record in no group).
  - **Writer:** `int64_t pzpd_writer_group(w, name, len, bytes_hint)` → id. Unregistered ids stay valid (unnamed, no
    hint). Records of a group must be consecutive, frames increasing. Cut rule: never inside a group; before a new group
    whose hint doesn't fit; a group whose hint exceeds the shard limit gets a shard of its own.
  - **Range reads:** `size_t pzpd_range_span(a, first, count, mask)`, `ssize_t pzpd_read_range(a, first, count, mask,
    buf, cap, refs[count × S])`; refused across a member, shard or group boundary (a range is all in one group or all
    in none).
  - **CLI / lists:** `@group NAME` names are stored (escapes allowed); `pack` uses the group's file sizes as the hint;
    `pzpdir groups`; `salvage` emits `@group` lines. **Python:** `Writer.group()`, `Archive.group_find / group /
    read_range / read_group`.
- **v0.4 revision 11 (2026-09-22), from a review of the implementation:**
  - **AUTO memory limit** (§6): "half of RAM" is half of the memory the process may use: physical RAM, or the limit of
    its cgroup or an ancestor when lower (v2 `memory.max`, v1 `memory.limit_in_bytes`; systemd `MemoryMax`, Slurm,
    containers). A member's size is the sum of its manifest's shard sizes.
  - **Manifest header:** `groups_offset` / `group_count` are reserved and written as 0 (revision 10: there is no manifest
    group table).

---

## 1. Problem

Y-MAP-Net training reads, per sample, an RGB image plus several derived
modalities (combined, geolocation, or depth + segmentation) from sibling
directories (`val2017/`, `all_val2017/`, `geo_val2017/`, `depth_val2017/`,
`segment_val2017/`). Adding ImageNet (~1.28 M images) means ~4–6 M small
files; video (consecutive frames) multiplies that further. Costs today:

- inode / dentry pressure, slow `ls`, `rsync`, `du`, backups, uploads
- 3–5 separate `open()+fstat()+read()+close()` per sample, at unrelated disk
  locations, plus `fileExists()` probing of up to 3 name variants per modality
  (`PrepareBatch.c: resolvePathToRequestedFiles`)
- no way to tell the OS "these are the next 10 000 samples"

## 2. Goals / non-goals

**Goals**

1. **Records.** The files of one sample sit next to each other on disk, so
   reading a sample (or a contiguous subset of its modalities) is **one `pread`**.
2. **Arbitrary filenames, grouping decided by the writer.** No naming convention
   is assumed. The *application* that writes the archive decides which files
   form a record, and they may come from any directories. The archive tool just
   takes "record K = these files" (§3.3).
3. Millions of records in **self-contained ~4 GB shards**. Any single shard
   can be read on its own, so after a catastrophic loss of other shards or of
   the manifest its data is still recoverable.
4. O(1) open: read the header and `mmap` the index. There is no parsing and no allocation proportional to N.
5. O(1) lookup by **ordinal**, and fast lookup by **record key** or by **any original filename**.
6. Read-heavy, write-rare. Swapping one modality (a new depth teacher) is
   done by rewriting **shard by shard**, which needs about one shard of free space.
7. Background **prefetch** of raw bytes, driven by the known (shuffled) read
   order, including a bounded memory-pool mode with `O_DIRECT`.
8. **Video groups:** a clip's frames are consecutive records and are read as a range.
8a. **Multiple archives open as one.** Several `.pzpd` archives (e.g. coco_train +
   imagenet + bg20k) are presented through the same API as a single archive,
   either passed as a list or saved as a small collection file (§3.4).
9. C API (`pzpdir.h` declarations + `pzpdir.c` implementation, built as `libpzpdir.so` / `libpzpdir.a`), Python ctypes,
   and a separate `pzpdir` CLI. The existing `pzp` binary and `libpzp.so` are left untouched.
10. Linux only (`pread`, `posix_fadvise`, `O_DIRECT`, `madvise`; `io_uring` later).
11. **Annotations as first-class data.** Per-sample annotations (persons with
   bbox + keypoints, description **texts**, image size, DINOv2/v3 descriptors) and
   archive-wide tables (joint names/parents) live **in the archive** as typed
   tables (§3.6). They replace the `.db` file and the separate descriptor file,
   bulk-load with zero parsing, and are written and inspected as CSV.
12. **Storage-aware.** The same archive works on NVMe/SSD and on a RAM disk
   (tmpfs). The reader detects the storage per shard and picks the right access
   method: zero-copy mmap on a RAM disk, prefetched reads on block devices (§6.1).

**Non-goals (v1)**

- Transcoding. Blobs are **opaque bytes**: JPG stays JPG, `.pzp` stays `.pzp`.
  Decoding stays in existing codecs (`ReadPZPMemory`, jpg/png readers).
- Decoding in the prefetcher (raw bytes only; DataLoader workers decode).
- Appending new records / general in-place modification (future work, §7).
- DataLoader integration (follow-up project; the design constraints are in §10).
- Compression of the archive as a whole, concurrent writers, macOS or Windows.

## 3. Concepts

| Term | Meaning |
|---|---|
| archive | one logical collection = manifest `<name>.pzpd` + shards `<name>.NNNNN.pzpd` |
| stream | a named slot declared when the archive is created (`rgb`, `all`, `geo`, `depth`, `seg`, …). At most 32 streams, identified by a u8 id |
| record | one sample: a **record key** plus 0..1 blob per stream, stored contiguously |
| ordinal | 0..N-1 global position of a record; shard *s* holds a contiguous range |
| record key | arbitrary bytes identifying the sample (e.g. `000000000009`) |
| blob | one file's bytes inside a record, together with its **original filename** (arbitrary bytes), its **format** and its **metadata** |
| format | a **FourCC** (u32) identifying the blob's file format (`JPEG`, `PNG `, `PZP `, `PZPC`, `PNM `, `PFM `, `NPY `, `TEXT`, `JSON`, `CSV `, `RAW `, …), detected from the content's magic bytes (§3.5) |
| metadata | small fixed fields per blob, in the index: dimensions, channels, bit depth, line count, frame count (§3.5) |
| group | optional u32 clip id; a clip's frames are consecutive records |
| table | a named, **typed** set of rows. A **record table** gives each record 0..n rows (e.g. `persons`, `descriptions`). A **global table** holds rows for the whole archive (e.g. `joints`). Stored in compact per-shard sections, **not** inside the record payload (§3.6) |

A **plain archive** (no pairing, e.g. packing an arbitrary directory tree) is
the special case of one stream (`data`) where record key = file path.

### 3.1 Record layout

```
record i:  [record header][record key][blob names ...][table rows copy, unless bulk][pad to 64]
           [payload stream a][pad 64][payload stream b][pad 64] ...
           [pad to `align`]
```

- Payloads are stored in **stream declaration order**, so choosing the order
  defines which subsets are contiguous. For Y-MAP-Net, the default order is
  `rgb, all, geo, depth, seg`. The usual read set rgb+all+geo is then one
  contiguous prefix of the record, and depth+seg (used only when there is no
  combined file) comes after.
- The whole **record** is aligned to `align` (4096 for `O_DIRECT`), not each
  blob. Alignment waste is therefore ≤ 4 KB per *record*: about 0.4% of an
  average COCO record (~960 KB), instead of 25–30% for per-blob alignment on
  small seg and geo files. (This resolves v0.2 Q3.)
- A stream missing from a record costs 0 bytes on disk and is flagged in the index.

### 3.2 Filenames are arbitrary

- **Record keys and blob names** are opaque byte strings of 1–65 535 bytes. Any
  byte except NUL is allowed; UTF-8 is expected but not enforced. They are stored
  **exactly** as given: no case folding, no normalisation, no extension
  stripping. Lookup is an exact byte comparison.
- Record keys are unique within an archive, and so are blob names. The writer
  rejects duplicates. The two are separate namespaces.
- `unpack` / `salvage` write blob names back as paths, so they refuse or
  sanitise absolute paths, `..` components and empty components. The name stored
  in the archive is not changed.
- Archive names are arbitrary. The manifest stores shard filenames in a string
  heap, with no fixed-length field.

### 3.3 Grouping is the writer's job

The archive has **no grouping logic**: no stem matching, regexes or pairing
rules. A dataset-specific script (the "smart" writer) decides which files belong
to a record, in what order records go, and what the keys are. It then either:

1. calls the Writer API directly (`pzpd_writer_begin` / `blob_file` / `end` in C,
   `w.record(key)` + `w.add_file(stream, path)` in Python), or
2. emits a **record list** and pipes it to `pzpdir pack`.

A record's blobs may come from **any directories** (different disks, different
trees, different naming). Each blob is just `(stream, source path, stored name)`.

**Record list format** (UTF-8 text, one blob per line, `-` = stdin):

```
# key <TAB> stream <TAB> source-path [<TAB> stored-name]
000000000009	rgb	/data/coco/val2017/000000000009.jpg	val2017/000000000009.jpg
000000000009	all	/mnt/big/all_val2017PZPF/000000000009.pzp
000000000009	geo	/data/coco/geo_val2017/000000000009.jpg.pzp
000000000025	rgb	/data/coco/val2017/000000000025.jpg
# following records are frames of one clip
@group clip_0042
clip_0042/0000	rgb	/video/a/0000.png
clip_0042/0001	rgb	/video/a/0001.png
@group
```

- Consecutive lines with the same key form one record. A key that reappears
  later, or a stream repeated within a record, is an error.
- `stored-name` defaults to `source-path` verbatim. The escapes `\t`, `\n` and `\\`
  allow any character in keys and names.
- Lines starting with `#` are comments (whole line only). `@group NAME` starts
  a group of consecutive records and a bare `@group` ends it. A key that itself
  starts with `#`, `@` or `\` is written with a leading `\`.
- **Tables** (§3.6) in the same list:
  ```
  @table  persons      id:u16 bbox:u16[4] kp:u16[51]
  @table  descriptions source:str text:str
  @table  descriptor_dinov2 v:f32[768] bulk
  @global joints       name:str parent:u16
  @row    joints       head,0
  @row    joints       endsite_eye.l,0
  000000397133	persons	1,388,69,498,347,433,94,2,…
  000000397133	persons	2,0,262,62,299,0,0,0,…
  000000397133	descriptions	deepseekvl2,"A man in a kitchen, holding a knife."
  000000397133	descriptor_dinov2	0.0132,-0.2211,…
  ```
  A record line whose second field names a declared table carries **one CSV row**
  (RFC 4180 quoting for `str`). Rows of a record keep their line order.
- Stream order (and therefore on-disk order) comes from `--streams rgb,all,geo,depth,seg`,
  or from the order of first appearance.
- **Record order = line order.** The writer decides it; nothing is sorted.
  For datasets that come from a `.db` today, writers keep the **`.db` sample
  order**, so ordinal *i* = today's sample number *i* and loss logs, statistics
  and debug references stay comparable.
- No stream is required. Whatever a record lists is what it holds.
- Byte-exact non-UTF-8 names go through the C / Python API instead of the text list.

The only built-in convenience is `pzpdir pack <out.pzpd> <dir>`: a plain
single-stream archive of a directory tree, with key = relative path. It does no
pairing.

Example writers live **outside** the library, as scripts (e.g.
`scripts/pzpdir_list_from_db.py`: take the sample list and order from a
`.db`, map each `imagePath` stem to the modality dirs, add the annotation
tables, key = `imagePath`, and emit the list). A new dataset means a new small
script, not new archive features.

### 3.4 Opening several archives as one (collections)

`pzpd_open_many()`, or `pzpd_open()` on a collection file, returns an ordinary
`pzpd *`. Every other call (read, record, range, prefetch, Python) works on it
unchanged. A single archive is just a collection with one member internally,
so there is one code path.

- **Ordinals are concatenated** in member order: member 0 has `0..N0-1`,
  member 1 has `N0..N0+N1-1`, and so on. ordinal → member is a binary search
  over K prefix sums. `pzpd_member_of()` returns the member and the local ordinal.
- **Streams are unioned by name.** The union's stream ids are assigned in
  first-seen order, and each member keeps a remap table. A member without a
  stream (e.g. imagenet has no `geo`) reports those blobs as missing, just like
  a missing stream in one record. The union may hold at most 32 streams
  (`stream_mask` is u32).
- **Duplicate keys are kept.** Every record stays reachable by ordinal and
  nothing is hidden. The same rules apply to record keys and blob names:
  - `pzpd_find(key)` returns the match in the **first** member (member order),
  - `pzpd_find_in(member, key)` looks in one member, and
  - `pzpd_find_all(key)` returns every match.
  The member is a separate argument, never a prefix parsed out of the key,
  because keys are arbitrary and may contain `:` or `/`.
  `pzpdir info --dups` lists clashing keys.
- **Member aliases** default to the member file's name without `.pzpd`
  (`coco_val`). A collection file can override them. Aliases must be unique.
- **Lookup cost:** `find` probes each member's hash table (K probes, with K
  small). Open stays O(K) and does no merged index build.
- **Missing member:** its ordinal range stays **reserved**. The collection file
  stores each member's record count, so later ordinals don't shift, and its
  reads fail with `PZPD_E_MEMBER_MISSING`, as with a missing shard. A plain
  `pzpd_open_many()` without a collection file can't know the count, so it
  fails to open unless `PZPD_O_ALLOW_MISSING` is given (then the missing
  member counts as 0 records).
- **Staleness:** a member whose `archive_uuid` or record count differs from
  the collection file (e.g. after `replace-stream` changed its streams) is
  reported (`PZPD_E_STALE_COLLECTION`), and `pzpdir collect --refresh` fixes it.
- **Read-only:** writes (`pack`, `replace-stream`, …) always target one member
  archive, never a collection.
- **No nesting in v1:** a collection's members are archives or single shards,
  not other collections.
- **Mixed placement:** member paths may be **relative** (to the collection
  file) **or absolute**. One collection can therefore combine
  `/dev/shm/coco_train.pzpd` (RAM disk) with `/data/imagenet.pzpd` (NVMe), and
  each member gets its own access mode automatically (§6.1). The shards of one
  archive always sit next to their manifest (relative names), so an archive is
  staged with a plain `cp name.*.pzpd /dev/shm/`.
- **Prefetch and range reads** work across members. A range read cannot span
  two members, just as it cannot span two shards or groups.

### 3.5 Format identification and blob metadata

**Archive files.** Every file PZPDIR writes begins with an 8-byte magic: the
common prefix `PZPD` plus a 4-character kind. `file(1)`, `pzpd_open()` and
other tools can therefore tell them apart from each other and from `.pzp`
images without trusting the extension.

| Magic | File |
|---|---|
| `PZPDSHRD` | shard (also its backup superblock at EOF) |
| `PZPDMANI` | manifest |
| `PZPDCOLL` | collection |
| `PZPDRECD` | start of every record header inside a shard (used by `salvage`) |
| `PZPDSECT` | start of every index section (used to find the index when both superblocks are damaged) |

Every magic is followed by a u32 format version. A newer version that the
reader doesn't know is refused with a clear error; the reader never guesses.

**Blob format.** Each blob stores a **FourCC** format code. The writer detects
it from the payload's **magic bytes**, never from the name. The extension is
only a fallback when the content is not recognised, and `RAW ` is the final
default. A writer can also set the format explicitly for custom types. FourCCs
are open-ended, so no central registry is needed. The existing `pzp` code
already uses four-char tags for audio (`PZP_AUDIO_*`), so this follows that style.

| FourCC | Detected by | Metadata filled in |
|---|---|---|
| `JPEG` | `FF D8 FF` | width, height, channels (SOF), bits = 8 |
| `PNG ` | `89 50 4E 47` | width, height, bits, channels (IHDR colour type), `INDEXED` flag for palette PNGs |
| `PZP ` | PZP size prefix + decodable header | width, height, channels, bits (external) |
| `PZPC` | PZP container magic | width, height, channels, bits of frame 0; `frames` = frame count |
| `PNM ` | `P1`–`P6` | width, height, channels (1/3), bits (8/16 from maxval) |
| `PFM ` | `Pf` / `PF` | width, height, channels, bits = 32, `FLOAT` flag, `BIG_ENDIAN` flag |
| `NPY ` | `\x93NUMPY` | first 3 shape dims (as `width`, `height`, `channels`), bits from dtype, `FLOAT` flag |
| `JSON`, `TEXT` (`CSV `, `TSV ` by extension) | valid UTF-8 without NUL (JSON: first non-space char `{` or `[`); CSV/TSV look like text, so the extension picks them | `lines` = number of `\n`-terminated lines (+1 if the last line is unterminated) |
| `RAW ` | anything else | none (`META_VALID` flag clear) |

Metadata is computed **once at pack time**, from headers only. There is no
full image decode, except `PZP `, whose 40-byte header sits inside the
compressed payload and needs a partial decompress. It is stored in the blob
table, so reading it costs **no I/O** beyond the already-mmapped index. That
enables:
- `ls --long` and `info` summaries without touching data;
- filtering and sanity checks (e.g. "all depth blobs are 1 ch / 16 bit")
  before training starts;
- pre-allocation of decode buffers of the right size.

Custom formats: `pzpd_writer_blob_ex()` accepts a caller-filled `pzpd_blob_meta`
(format + fields), which overrides detection. A "smart" writer that already
knows the dimensions can skip probing entirely.

### 3.6 Tables: annotations as first-class data

**Why they live apart from the record payload.** The DataLoader needs *every*
sample's annotations at startup (skeleton counts for `ignoreNoSkeletonSamples`,
tokens, loss-based shuffling, label queries from Python), but a sample's
*pixels* only when that sample is trained. If annotations sat inside each
record next to the JPEG, loading them would take one scattered read per sample
(1.28 M for ImageNet across ~150 GB). So each table is stored in its own
contiguous **table section** per shard, next to the index. It is `mmap`ped and
bulk-read in a few sequential MB.

**Typed rows, CSV at the edges.** Each table declares a schema. Rows are
stored as fixed-width binary, so C gets a pointer with **zero parsing**. The
writer's record list, `pzpdir cat --table` and `pzpdir export-table` use
CSV text, which is converted by the schema at pack time and rendered back on output.

Schema = ordered columns `name:type[count]`:

| Type | Bytes | Notes |
|---|---|---|
| `u8 i8 u16 i16 u32 i32 u64 i64` | 1–8 | range-checked when parsing CSV; out of range is an error, never a wrap-around |
| `f32 f64` | 4 / 8 | CSV uses shortest round-trip text (bit-exact) |
| `str` | 8 | `{u32 offset, u32 len}` into the table's string heap (arbitrary bytes, CSV-quoted on output) |
| `T[n]` | n × size(T) | fixed-length array, e.g. `u16[51]`, `f32[768]`. In CSV it's n consecutive fields |

- **Row layout follows C struct rules:** columns in order, natural alignment,
  stride padded to the largest alignment. A C `struct` with the same members maps
  1:1 onto a row. `pzpd_table_schema()` also gives each column's offset for generic readers.
- **Table names** are ≤ 23 bytes and share the namespace with stream names. At most 16 tables per archive.
- **Record tables:** rows of record *i* are `rows[idx[i] .. idx[i+1])` (CSR
  layout). Zero rows is normal (e.g. no persons, no descriptions).
- **Global tables:** a single row set for the archive, copied into **every
  shard and the manifest** so each shard stays self-contained.
- **Recovery:** unless a table is marked `bulk` (large, re-derivable data such as
  descriptors), a record's rows are also copied into its `PZPDRECD` record header.
  `salvage` then recovers annotations even if the table section is destroyed. For
  the `.db` data this costs ~0.4 KB per record.
- **Updates are cheap** (§7): a table can be replaced without rewriting any image data.

**Y-MAP-Net mapping** (replaces `DB1` `.db` files and the descriptor file; SuperPoint `SPT1` files stay separate):

| Table | Kind | Schema | From today's source |
|---|---|---|---|
| `joints` | global | `name:str, parent:u16` | `.db` header: joint names + parents (17 for COCO) |
| `image` | record, 1 row | `width:u16, height:u16` | `.db` `w,h,numPersons` line (kept explicitly even though rgb metadata has dims, because keypoints are in *this* space, e.g. for rescaled datasets) |
| `persons` | record, 0..n rows | `id:u16, bbox:u16[4], kp:u16[3*J]` (**all coordinates packed as u16**: 112 B per person for J = 17, no padding) | `.db` `SKn,bx,by,bw,bh,x,y,v,…` lines (n = `numPersons`; J from `joints`) |
| `descriptions` | record, 0..n rows | `source:str, text:str` | the caption **text** from `descriptions.json` (e.g. `source = deepseekvl2`, `old`). **Not** the `.db` token IDs, see below |
| `descriptor_<model>` | record, 0..1 row, `bulk` | `v:f32[D]`. D comes from the schema and is **never hardcoded** (DINOv2 = 768; DINOv3 and other backbones differ) | descriptor file, today matched by position + basename. That matching goes away |

**Coordinates as u16.** Bboxes and keypoint triplets `(x, y, v)` are packed
`u16`, the same as `struct Skeleton` and the `.db`'s `%hu` parsing today, so
values are 0..65535 and never negative. A row of `persons` is a flat
`u16[1 + 4 + 3J]`, which C can index directly.

**Descriptions are stored as text, not token IDs.** `buildVocabulary.py` builds
the vocabulary from the sorted unique words of *all* description sets loaded
together. A token ID therefore depends on which datasets are combined for a
training run, so baking IDs into an archive would freeze one vocabulary.
Instead:
- the archive stores the caption text (UTF-8, arbitrary length, in the table's string heap);
- the **loader tokenizes at load time** over all loaded members:
  `re.findall(r'\w+|[.,()]', text.lower())`, drop non-ASCII tokens, add the
  predefined symbols `~ , . ( )`, sort the unique words, and assign IDs. It then
  applies the token blacklist and synonym map as today, and looks up embeddings
  (GloVe) by word;
- a vocabulary change or a new dataset mix needs **no archive edit at all**;
- **pinned vocabulary:** a trained model's embedding layer is tied to its token
  IDs, so the loader can instead take a fixed vocabulary file (today's
  `index_to_word.json`) and map words through it. Words not in it are dropped or
  mapped to an unknown token, matching the current behaviour. Fine-tuning and
  evaluating existing checkpoints need this mode;
- several captioners coexist as rows with different `source` values, and the loader picks one (or several).

The C tokenizer must reproduce Python's `\w` / `lower()` behaviour for these
rules exactly. Non-ASCII tokens are dropped, but non-ASCII characters still
split words the way `\w` does. Parity with `buildVocabulary.py` is a test
gate. Tokenizing ~130 M words for ImageNet at startup is expected to take seconds in C;
caching the vocabulary per (member UUIDs, generations, source) is a possible later optimisation.

**Descriptor length is per table.** Each `descriptor_<model>` table carries its
own `D` in its schema (`pzpd_table_schema()->cols[0].count`). DINOv2 and
DINOv3 tables can sit side by side in one archive and are selected by name.
Collection members must agree on D for the same table name; this is checked at open.

The sample list and order come **from the archive itself**. The record key is
the DB's `imagePath`, and sample *i* = ordinal *i*. There is no separate `.db`
to keep in sync.

## 4. On-disk format (all little-endian)

### 4.1 Shard file `<name>.NNNNN.pzpd`

```
offset 0      [ 4 KiB ] Superblock (primary)
offset 4096   [ ...   ] Records (§3.1), each aligned to `align`
              [ ...   ] Record table   n × 32 B           (ordinal order)
              [ ...   ] Blob table     n × S × 32 B       (S = stream_count; includes format + metadata)
              [ ...   ] Hash table     (n + blobs) × 16 B (sorted by hash; shard-local)
              [ ...   ] String heap    record keys + blob names (no NUL)
              [ ...   ] Group table    g × 16 B           (optional)
              [ ...   ] Table sections one per table (§3.6), each "PZPDSECT":
                                       schema, row index (n+1) × u32, rows, string heap
              [ ...   ] Metadata blob  (optional, opaque JSON)
end - 4 KiB   [ 4 KiB ] Superblock (backup copy)
```

- Index sections come **after** the records, so the writer streams in one pass.
  Each section starts on a 4 KiB boundary (`mmap`-able).
- **Recovery ladder**, in case of damage:
  1. The primary superblock is bad → use the backup at EOF.
  2. Both are bad but the index is intact → find the sections by scanning 4 KiB boundaries for section magics.
  3. The index is lost → scan for **record header** magics at `align`
     boundaries (`PZPDRECD`). Each record header carries the key, and for every
     blob its name, stream, format, metadata, size and XXH32, so `pzpdir salvage` recovers every intact blob
     under its original filename. The stream names needed to interpret record
     headers are in the superblock *and* repeated in the metadata blob.

### 4.2 Shard superblock (4 KiB slot)

| Field | Type | Notes |
|---|---|---|
| magic | char[8] | `"PZPDSHRD"` (§3.5) |
| version | u32 | 1 |
| flags | u32 | bit0 has groups |
| archive_uuid | u8[16] | same across all shards of an archive |
| generation | u64 | incremented when the shard is rewritten (replace-stream, §7) |
| shard_index / shard_count | u32 ×2 | |
| first_ordinal / record_count / total_records | u64 ×3 | |
| stream_count | u32 | S ≤ 32 |
| streams | 32 × { char[23] name, u8 flags } | stream table (768 B); names ≤ 23 bytes |
| tables | 16 × { char[23] name, u8 flags (global, bulk), u64 section_offset, u64 section_bytes, u32 row_stride, u32 pad } | table directory (768 B); the schema itself lives in the section header |
| group_count | u64 | |
| align | u32 | 64 or 4096 |
| records_offset / records_bytes | u64 ×2 | |
| rtab_offset, btab_offset, hash_offset | u64 ×3 | |
| heap_offset / heap_bytes | u64 ×2 | |
| groups_offset, meta_offset / meta_bytes | u64 ×3 | |
| index_checksum | u64 | XXH64 over all index sections |
| sb_checksum | u64 | XXH64 over the preceding superblock bytes |

### 4.3 Record table entry (32 B)

| Field | Type | Notes |
|---|---|---|
| offset | u64 | shard-relative offset of the record header |
| bytes | u32 | total record bytes including padding (≤ 4 GiB) |
| key_offset | u32 | into the string heap |
| key_len | u16 | |
| present | u16 | reserved; presence is in the blob table |
| group | u32 | 0xFFFFFFFF if none |
| frame | u32 | index within the group |
| checksum | u32 | XXH32 of the record header |

### 4.4 Blob table entry (32 B; row-major `[ordinal][stream]`)

| Field | Type | Notes |
|---|---|---|
| rel_offset | u32 | payload offset from the record start; 0xFFFFFFFF = stream missing |
| size | u32 | payload bytes |
| name_offset | u32 | into the string heap |
| name_len | u16 | |
| meta_flags | u8 | bit0 `META_VALID`, bit1 `FLOAT`, bit2 `BIG_ENDIAN`, bit3 `INDEXED` (palette), bit4 `USER_META` (set by the writer, not detected) |
| bits | u8 | bits per channel (1, 8, 16, 32, 64), 0 if n/a |
| format | u32 | FourCC (§3.5) |
| width | u32 | pixels; **or** `lines` for text formats; first dim for `NPY ` |
| height | u32 | pixels; second dim for `NPY `; 0 if n/a |
| channels | u16 | 1, 3, 4, … ; third dim for `NPY ` (saturating); 0 if n/a |
| frames | u16 | frame count for `PZPC` (saturating at 65 535; exact count via the blob itself); 1 for still images; 0 if n/a |

The payload XXH32 lives in the record header, not here. `verify` and
`PZPD_O_VERIFY` read it from the header.

### 4.5 Hash table (16 B per key)

A sorted array of `{ u64 xxh64(bytes), u32 local_ordinal, u8 stream, u8 kind, u16 pad }`.
It indexes record keys (`kind = 0`, `stream = 0xFF`) and blob names (`kind = 1`).
Lookup is an interpolation search (~2–3 probes), then a byte compare against the
heap to rule out collisions.

### 4.6 Group table (16 B per group)

`{ u32 group_id, u32 first_local_ordinal, u32 frame_count, u32 name_offset }`.
**A group never spans shards.** The writer starts a new shard early rather than
split a clip; a clip larger than the shard limit gets its own oversize shard.
Frames *k..k+n* of a clip form one contiguous byte range, so they are read with one `pread`.

### 4.7 Manifest `<name>.pzpd`

```
[ 4 KiB ] Header: magic "PZPDMANI" (§3.5), version, archive_uuid, stream table,
          total_records, shard_count, checksums
[ ...   ] Shard table: shard_count × { u64 first_ordinal, u64 record_count,
          u64 file_bytes, u64 shard_generation, u64 index_checksum,
          u32 name_offset, u32 name_len }
[ ...   ] Shard-name heap
[ ...   ] Global hash table: { u64 hash, u64 ordinal, u8 stream, u8 kind }   (≈16–24 B each)
[ ...   ] Global group table
[ ...   ] Table schemas + global-table sections (copies of the shards' global tables)
```

- ordinal → shard: binary search in the shard table.
- key or name → (ordinal, stream): global hash, then byte compare in the shard's heap.
- `pzpdir rebuild-manifest <name>.*.pzpd` regenerates the manifest from the
  shard superblocks. The manifest is never the only copy of anything.
- Shards are opened **lazily** on first touch. An unavailable shard fails only
  its own ordinals (`PZPD_E_SHARD_MISSING`), and the rest of the archive keeps
  working. A shard whose generation or checksum disagrees with the manifest is
  reported (`PZPD_E_STALE_MANIFEST`), and `rebuild-manifest` fixes it.

### 4.8 Collection file `<name>.pzpd`

It shares the `.pzpd` extension. `pzpd_open()` tells a shard, manifest and
collection apart by magic.

```
[ 4 KiB ] Header: magic "PZPDCOLL" (§3.5), version, member_count, total_records,
          union stream table, checksum
[ ...   ] Member table: member_count × { u64 first_ordinal, u64 record_count,
          u8 archive_uuid[16], u32 path_offset, u32 path_len,
          u32 alias_offset, u32 alias_len, u32 stream_remap_offset }
[ ...   ] String heap: member paths (relative to the collection file, or absolute) and aliases
[ ...   ] Stream remap tables
```

The file is tiny (≈100 B per member) and can be regenerated at any time with `pzpdir collect`.

### 4.9 Size estimate (1.28 M ImageNet records, 3 streams, 4 GB shards)

Record table 41 MB + blob table 123 MB + hash ~60 MB + heap ~100 MB ≈ 325 MB of
index, plus ~0.2 KB of record header per record. Total overhead is about 0.3% of
~150+ GB of data, plus ≤ 4 KB per record of `O_DIRECT` alignment (≈1–2%).

## 5. C API (`pzpdir.h` + `pzpdir.c`; built as `libpzpdir.so` / `libpzpdir.a`)

Packaging: `pzpdir.h` holds only declarations, types and constants. The whole
implementation (format, writer, reader, tables, prefetcher with pthreads) is in
`pzpdir.c`, which builds into `libpzpdir.so` (Python, DataLoader) and
`libpzpdir.a` (static linking). Consumers link the library or vendor both files.
Hashing uses the official single-header **xxHash** (BSD-2), vendored as
`third_party/xxhash.h`: XXH32 for blobs and record headers, XXH64 for index
sections, superblocks and key hashes. There is no new system dependency beyond zstd / lz4.


```c
typedef struct pzpd pzpd;                 // opaque; thread-safe for reads

// ---- open / info ----
pzpd *     pzpd_open(const char *path, unsigned int flags);
           // path = manifest or a single shard (opened standalone)
           // flags: PZPD_O_VERIFY (checksum each read), PZPD_O_DIRECT,
           //        PZPD_O_HUGEPAGE  (MADV_HUGEPAGE on data mappings; effective on tmpfs mounted huge=…)
           //        PZPD_O_POPULATE  (pre-fault whole shards at open; for RAM-disk archives that are fully used)
void       pzpd_close(pzpd *a);
uint64_t   pzpd_count(const pzpd *a);                        // records
unsigned   pzpd_stream_count(const pzpd *a);
int        pzpd_stream_id(const pzpd *a, const char *name);  // -1 if unknown
const char*pzpd_stream_name(const pzpd *a, unsigned stream);
const char*pzpd_last_error(void);                            // thread-local

typedef enum { PZPD_STORAGE_BLOCK = 0, PZPD_STORAGE_RAM = 1 } pzpd_storage;
pzpd_storage pzpd_storage_kind(const pzpd *a, uint64_t ordinal);
           // statfs() of the shard holding `ordinal`: TMPFS_MAGIC / RAMFS_MAGIC → RAM,
           // anything else (ext4, xfs, brd, zram, NFS, FUSE) → BLOCK

// ---- several archives as one (§3.4); result is a normal pzpd* ----
pzpd *     pzpd_open_many(const char **paths, const char **aliases /*NULL=default*/,
                          unsigned n, unsigned int flags);   // + PZPD_O_ALLOW_MISSING
unsigned   pzpd_member_count(const pzpd *a);
const char*pzpd_member_alias(const pzpd *a, unsigned member);
int        pzpd_member_id(const pzpd *a, const char *alias);  // -1 if unknown
int        pzpd_member_of(const pzpd *a, uint64_t ordinal, uint64_t *local_ordinal);
int        pzpd_member_range(const pzpd *a, unsigned member, uint64_t *first, uint64_t *count);

// ---- collection files (what `pzpdir collect` calls; usable from Python / the DataLoader) ----
int        pzpd_collection_write(const char *out_path, const char *const *paths,
                                 const char *const *aliases /*NULL=default*/, unsigned n,
                                 unsigned flags /* PZPD_COLL_ABSOLUTE */);
int        pzpd_collection_refresh(const char *path);   // re-read member counts / streams / tables

// ---- lookup ----
int64_t    pzpd_find(const pzpd *a, const char *key, size_t len, int *stream_out);
           // record key → ordinal, *stream_out = -1; blob name → ordinal + stream
           // with several members: first member (in order) that has it
int64_t    pzpd_find_in(const pzpd *a, unsigned member, const char *key, size_t len, int *stream_out);
size_t     pzpd_find_all(const pzpd *a, const char *key, size_t len,
                         int64_t *ordinals_out, int *streams_out, size_t max);
           // returns the TOTAL number of matches, which may exceed `max`
           // (only the first `max` are written); callers retry with a larger buffer
const char*pzpd_record_key(const pzpd *a, uint64_t ordinal, size_t *len);
int        pzpd_blob_info(const pzpd *a, uint64_t ordinal, unsigned stream, pzpd_blob_info *out);
           // index only: no data I/O. Fills the struct below.

typedef struct {
    int         present;
    uint64_t    size;
    uint32_t    format;          // FourCC, e.g. PZPD_FMT('J','P','E','G')
    uint32_t    width;           // or line count for text formats
    uint32_t    height;
    uint16_t    channels;
    uint16_t    frames;
    uint8_t     bits;            // per channel
    uint8_t     meta_flags;      // PZPD_META_VALID | FLOAT | BIG_ENDIAN | INDEXED | USER_META
    const char *name; size_t name_len;   // original filename, points into mmap
    uint32_t    group, frame;    // video group / frame index
    unsigned    member, shard;
} pzpd_blob_info;

typedef struct {                 // what detection produces / what blob_ex accepts
    uint32_t format;
    uint32_t width, height;      // width = line count for text formats
    uint16_t channels, frames;
    uint8_t  bits, meta_flags;
} pzpd_blob_meta;

#define PZPD_FMT(a,b,c,d) ((uint32_t)(a) | (uint32_t)(b)<<8 | (uint32_t)(c)<<16 | (uint32_t)(d)<<24)
const char *pzpd_format_name(uint32_t fourcc, char out[5]);   // "JPEG", "PNG ", …
uint32_t    pzpd_detect_format(const void *data, size_t size, pzpd_blob_meta *meta_out);
            // the probe the writer uses; exposed for tools and tests

// ---- read one blob ----
ssize_t    pzpd_read_into(pzpd *a, uint64_t ordinal, unsigned stream, void *buf, size_t cap);
void *     pzpd_read_alloc(pzpd *a, uint64_t ordinal, unsigned stream, size_t *size); // pzpd_free
const void*pzpd_view(pzpd *a, uint64_t ordinal, unsigned stream, size_t *size);       // mmap; not with O_DIRECT

// ---- read several streams of one record in ONE pread ----
ssize_t    pzpd_read_record(pzpd *a, uint64_t ordinal, uint32_t stream_mask,
                            void *buf, size_t cap, pzpd_blob_ref refs[/*S*/]);
           // reads the smallest byte span covering the requested streams;
           // refs[s] = {ptr into buf, size, type} or {NULL,0} if absent / not requested
size_t     pzpd_record_span(const pzpd *a, uint64_t ordinal, uint32_t stream_mask); // buffer size needed

// ---- tables (§3.6): index-resident, no data I/O, zero-copy pointers into mmap ----
typedef struct { const char *name; uint8_t type; uint16_t count; uint32_t offset; } pzpd_column;
typedef struct { const char *name; int global, bulk; uint32_t row_stride;
                 unsigned ncols; const pzpd_column *cols; } pzpd_schema;

int        pzpd_table_id(const pzpd *a, const char *name);                  // -1 if unknown
const pzpd_schema *pzpd_table_schema(const pzpd *a, unsigned table);
uint32_t   pzpd_table_rows(const pzpd *a, uint64_t ordinal, unsigned table,
                           const void **rows_out);                          // n rows, stride = row_stride
uint32_t   pzpd_global_rows(const pzpd *a, unsigned member, unsigned table,
                            const void **rows_out);                         // per collection member
const char*pzpd_table_str(const pzpd *a, uint64_t ordinal_or_global, unsigned table,
                          const void *str_field, size_t *len);              // resolve a `str` column
// bulk access for startup loaders: one contiguous CSR view per shard
int        pzpd_table_shard_view(const pzpd *a, unsigned shard, unsigned table,
                                 uint64_t *first_ordinal, uint64_t *records,
                                 const uint32_t **row_index, const void **rows);
// CSV rendering (tools, debugging)
ssize_t    pzpd_table_csv(const pzpd *a, uint64_t ordinal, unsigned table, char *out, size_t cap);

// ---- convenience decode (wraps pzp_decompress_combined_from_memory); only with PZPDIR_WITH_PZP=1 ----
unsigned char *pzpd_read_pzp(pzpd *a, uint64_t ordinal, unsigned stream,
                             unsigned int *w, unsigned int *h,
                             unsigned int *bpp, unsigned int *channels);

// ---- groups / video ----
int64_t    pzpd_group_find(const pzpd *a, const char *name, size_t len);
int        pzpd_group_info(const pzpd *a, int64_t group, uint64_t *first_ordinal, uint32_t *frames);
ssize_t    pzpd_read_range(pzpd *a, uint64_t first_ordinal, uint32_t count,
                           uint32_t stream_mask, void *buf, size_t cap,
                           pzpd_blob_ref refs[/*count*S*/]);
           // one pread over consecutive records; fails across a group or shard boundary

// ---- writer (single-threaded builder) ----
pzpd_writer *pzpd_writer_create(const char *manifest_path, const pzpd_writer_opts *o);
           // o: stream names (order = on-disk order), shard_max_bytes (4 GiB), align
int  pzpd_writer_begin(pzpd_writer *w, const char *key, size_t key_len,
                       uint32_t group, uint32_t frame);
int  pzpd_writer_blob(pzpd_writer *w, unsigned stream, const char *name, size_t name_len,
                      const void *data, size_t size);            // format + metadata detected
int  pzpd_writer_blob_file(pzpd_writer *w, unsigned stream, const char *name, size_t name_len,
                           const char *src_path);
int  pzpd_writer_blob_ex(pzpd_writer *w, unsigned stream, const char *name, size_t name_len,
                         const void *data, size_t size, const pzpd_blob_meta *meta);
     // meta != NULL overrides detection (custom formats, or a writer that already knows dims)
int  pzpd_writer_table(pzpd_writer *w, const char *name, const char *schema, unsigned flags);
     // declare before the first record, e.g. ("persons", "id:u16,bbox:u16[4],kp:u16[51]", 0)
     // flags: PZPD_TABLE_GLOBAL, PZPD_TABLE_BULK
int  pzpd_writer_rows(pzpd_writer *w, unsigned table, const void *rows, uint32_t nrows);   // binary, row_stride each
int  pzpd_writer_rows_csv(pzpd_writer *w, unsigned table, const char *csv, size_t len);    // parsed by the schema
int  pzpd_writer_global_rows(pzpd_writer *w, unsigned table, const void *rows, uint32_t nrows);
int  pzpd_writer_end(pzpd_writer *w);       // record done; stream order enforced here
int  pzpd_writer_finish(pzpd_writer *w);
void pzpd_writer_abort(pzpd_writer *w);
```

Writer semantics:
- Records are stored **in call order**, which is the ordinal order. Frames of a
  group must be consecutive.
- Each shard is written as `.tmp`, fsync'd and renamed when it closes. `finish`
  writes the manifest (temp + rename) and patches `shard_count` in the shard
  superblocks.
- A crash leaves complete shards, which are usable standalone, plus at most one `.tmp`.
- Errors: return NULL or -1 and set `pzpd_last_error()`. Never `exit()` or
  `abort()` (following commit `6652a16`).

## 6. Prefetcher (raw bytes only)

The DataLoader knows the full shuffled order ahead of time, so prefetch is
**explicit**. Each prefetched item is **one record span**, i.e. one I/O per sample.

```c
typedef struct pzpd_prefetcher pzpd_prefetcher;

pzpd_prefetcher *pzpd_prefetcher_create(pzpd *a, const pzpd_prefetch_opts *o);
    // o->stream_mask    which streams to fetch (default: all)
    // o->io_threads     default 4 (NVMe benefits from queue depth)
    // o->mode           PZPD_PF_AUTO (default) | PZPD_PF_MAP | PZPD_PF_PAGECACHE | PZPD_PF_BUFFERS
    // o->budget_bytes   BUFFERS memory cap, default 512 MiB
    // o->window         max records in flight ahead of the consumer

int  pzpd_prefetch_submit(pzpd_prefetcher *p, const uint64_t *ordinals,
                          const uint32_t *masks /* NULL = o->stream_mask */, size_t n);
     // Appends to the schedule. The whole epoch order may be submitted at once;
     // `window` / `budget_bytes` throttle how far ahead I/O actually runs.
     // Per-ordinal masks: the stream set can differ per sample (§10.3).
     // An ordinal may appear more than once; each occurrence is a separate claim.
void pzpd_prefetch_clear(pzpd_prefetcher *p);        // on reshuffle; waits for in-flight I/O

int  pzpd_prefetch_get(pzpd_prefetcher *p, uint64_t ordinal, uint32_t mask,
                       pzpd_blob_ref refs[/*S*/], pzpd_ticket *t);
     // Any thread, in ANY order within the window (workers take strided positions).
     // prefetched → immediate; not yet → waits if in flight, else a synchronous read.
     // A miss never fails; it only costs latency. mask must be a subset of the
     // submitted mask, otherwise the missing streams are read synchronously.
void pzpd_prefetch_release(pzpd_prefetcher *p, pzpd_ticket *t);
void pzpd_prefetch_discard(pzpd_prefetcher *p, uint64_t ordinal);
     // Drop a scheduled claim without reading it (e.g. the sample was erased or
     // skipped). The slot is freed immediately, or cancelled if still queued.

void pzpd_prefetch_stats(const pzpd_prefetcher *p, pzpd_prefetch_stats *s);
     // hits, in-flight waits, sync misses, bytes/s, producer stalls, bytes over-read
void pzpd_prefetcher_destroy(pzpd_prefetcher *p);
```

Modes (all in v1):

0. **AUTO** (default). The mode is chosen **per shard** (and so per collection
   member) from `pzpd_storage_kind()` and size: RAM disk → MAP; block device
   with the archive smaller than half of the process's memory limit (RAM, or its
   cgroup's limit when lower; revision 11) → PAGECACHE; otherwise → BUFFERS. The chosen modes are reported in `stats` and by `pzpdir info`.
1. **MAP** (RAM disk). Details in §6.1.
2. **PAGECACHE.** I/O threads run `window` records ahead and call
   `posix_fadvise(WILLNEED)` on each record span. `get` returns `mmap` views.
   Use it when the working set fits in RAM.
3. **BUFFERS + O_DIRECT.** I/O threads `pread` each record span with
   `O_DIRECT` (records are 4 KiB-aligned) into a fixed slab arena of
   `budget_bytes`. Slots are refcounted and returned on `release`. When the
   arena is full, producers stall, which gives backpressure. This bypasses the page cache
   so a 150 GB epoch does not evict everything else. If `O_DIRECT` is
   refused (tmpfs, some FUSE), it falls back to buffered `pread` and records that in stats.

If `stream_mask` selects non-adjacent streams (e.g. rgb + seg with depth in
between), the prefetcher reads the covering span when the skipped bytes are below
a threshold (default 256 KB). Otherwise it issues separate reads. The
over-read is reported in stats.

`io_uring` can replace the thread pool later without changing the API.

### 6.1 RAM disks (tmpfs)

On tmpfs/ramfs the archive's pages **are** the page cache. A read costs
microseconds, so hiding latency no longer matters. What changes:

| Aspect | Block device (NVMe) | RAM disk (tmpfs) |
|---|---|---|
| Cost of reading a record (~650 KB) | 100 µs – ms, random-read / queue-depth bound | ~50–100 µs memcpy via `pread`; ~0 via mmap view |
| `posix_fadvise(WILLNEED / DONTNEED)` | useful | no-op (already resident; the only copy can't be evicted) |
| BUFFERS mode | needed when data > RAM | **harmful**: RAM→RAM copies double the footprint |
| `O_DIRECT` | bypasses the page cache | refused or a plain copy, depending on the kernel; pointless |
| Dominant hidden cost | I/O latency | **first-touch page faults** (~160 × 4 KB faults per record) and TLB misses on random access |
| Bottleneck after the prefetcher | disk | decode (JPEG / PNG / zstd) + augmentation, i.e. CPU |
| Sharing between processes | page cache (shared) or private buffers | the same pages are mapped by every process (train, val, several runs); no duplication |

**MAP mode** (chosen by AUTO on RAM disks):
- `get` returns **mmap views** straight into the archive: no copy, no budget and no slab arena.
- I/O threads run `window` records ahead and call `madvise(MADV_POPULATE_READ)`
  (Linux ≥ 5.14) on each record span. This pre-faults the page tables so workers
  never take first-touch faults. On older kernels they touch one byte per page instead.
- `release` / `discard` only drop the claim; there is nothing to free.
- With `PZPD_O_HUGEPAGE`, data mappings get `MADV_HUGEPAGE`. On tmpfs mounted
  with `huge=within_size` (or `always`) a record then spans 0–1 **2 MiB** pages
  instead of ~160 small ones, which cuts faults and TLB misses.
- With `PZPD_O_POPULATE`, whole shards are pre-faulted at open (`MAP_POPULATE`).
  4.8 GB takes about a second. This is worth it when most of the archive is read every epoch.

**Recommended RAM-disk setup** (documented and benchmarked, not enforced):

```
sudo mount -t tmpfs -o size=8G,huge=within_size tmpfs /mnt/pzpd-ram
cp /data/pzpd/coco_val2017.*.pzpd /mnt/pzpd-ram/     # a few big sequential copies, seconds
pzpdir verify /mnt/pzpd-ram/coco_val2017.pzpd        # optional
```

`/dev/shm` works too but is mounted without huge pages by default.

**Not a fit:**
- `brd` (`/dev/ramN` + a filesystem) and `zram` are block devices, so AUTO
  treats them as BLOCK. Their data would sit in RAM twice (device + page cache)
  unless `O_DIRECT` is used, and zram adds decompression. Prefer tmpfs.
- Archives on a RAM disk occupy RAM permanently, so ImageNet (~150 GB) doesn't fit.
  The intended setup keeps hot, small datasets in RAM and large ones on NVMe, in
  one collection with absolute member paths; AUTO picks MAP or BUFFERS per member.
- tmpfs is volatile. The NVMe copy stays the source of truth, and re-staging is a `cp`.

**Why decode matters more here:** with I/O effectively free, the workers'
time is decode and augmentation. Background decode in the prefetcher (a v1
non-goal, D5) is the next lever for RAM-disk setups. It is listed as future work.

## 7. Writes

- **Build:** `pzpdir pack` → `finish` → atomic renames.
- **Stream edits (v1)**, e.g. swapping in a new depth teacher:
  `pzpdir replace-stream <a.pzpd> depth <record-list>` (lines for that stream only:
  `key<TAB>depth<TAB>path`). Related commands are
  `add-stream` (a new modality, e.g. normals) and `drop-stream`. All three work
  **shard by shard**:
  1. Stream shard *k* into `<name>.k.pzpd.tmp`, taking the other streams from the old shard and the edited stream from the new source.
  2. Verify it.
  3. fsync, then `rename` over the old shard. Its `generation` increases by 1.
  4. After the last shard, rewrite the manifest.

  Peak extra space is **one shard** (plus the new source files). After a
  crash, completed shards are new, the rest are old, and each shard is valid on
  its own. Rerunning the command resumes from the first shard with the old
  generation (the manifest records the target generation). Records whose key
  has no file in the new source keep the old blob (`--missing keep`, the
  default) or drop it (`--missing drop`). Keys in the new source that match no
  record are reported, not added.
- **Table edits (v1), cheap:** `pzpdir replace-table <a.pzpd> descriptions <list>`,
  plus `add-table` and `drop-table`. Typical edits are new captions (a new VLM
  run), relabelled persons, or adding `descriptor_dinov3` next to
  `descriptor_dinov2`. None of these should rewrite image data. (Vocabulary
  changes need no edit at all, since descriptions are stored as text.) Per shard:
  1. Append the new table section(s) and a new backup superblock (generation+1) at the end of the shard, then fsync.
  2. Rewrite the primary superblock with generation+1, then fsync.

  A crash between the steps leaves either the old or the new generation fully
  valid; the reader takes the highest valid generation. Old sections become
  dead space until `pzpdir compact`. The manifest's copies of global tables
  are refreshed last. Record keys don't change, so the name hashes stay valid.
  Rows for keys missing from the new list become empty (`--missing empty`), or
  keep their old rows (`--missing keep`).
- **Future:** appending new records and replacing single blobs in place. Not in v1.

## 8. Python API (ctypes; `pzp.pzpdir` module over `libpzpdir.so`)

```python
import pzp.pzpdir as pzpdir

with pzpdir.open(["coco_train.pzpd", "imagenet.pzpd"]) as a:   # several archives as one
    a.members                                         # ['coco_train', 'imagenet']
    i = a.find("000000000009", member="coco_train")   # disambiguate duplicate keys
    member, local = a.member_of(i)

with pzpdir.open("coco_val2017.pzpd") as a:           # manifest, single shard, or collection file
    len(a); a.streams                                 # ['rgb','all','geo','depth','seg']
    i = a.find("000000000009")                        # record key → ordinal
    i, s = a.find_name("geo_val2017/000000000009.jpg.pzp")   # blob name → (ordinal, stream)
    raw = a.read(i, "rgb")                            # bytes
    rec = a.read_record(i, ["rgb", "all", "geo"])     # dict of memoryviews, one pread
    img = a.read_image(i, "all")                      # numpy; PZP native, JPG/PNG via cv2/PIL if present
    a.info(i, "depth")        # {'format': 'PNG ', 'width': 640, 'height': 480, 'channels': 1,
                              #  'bits': 16, 'size': 311234, 'name': 'depth_val2017/…png'}  (no data I/O)
    a.stream_stats("depth")   # formats, dimension histogram, channels/bits combos across all records
    clip = a.read_group("videos/clip_0042", ["rgb"])  # list of dicts, one pread
    a.table(i, "persons")        # numpy structured array view: id, bbox[4], kp[51]  (no data I/O)
    a.table(i, "descriptions")["text"]   # caption strings (decoded); tokenization is the loader's job
    a.table(i, "descriptor_dinov3")["v"]  # float32[D], D taken from the schema
    a.global_table("joints")     # names + parents (member= for collections)
    idx, rows = a.table_all("persons")   # CSR arrays over all records, for bulk loading

pf = a.prefetcher(streams=["rgb", "all", "geo"], io_threads=4, mode="buffers", budget_mb=1024)
pf.submit(order[pos:pos+8192])
rec = pf.get(order[pos])                              # dict of memoryviews; released on del / exit

with pzpdir.Writer("x.pzpd", streams=["rgb", "depth"], align=4096, shard_size="4G") as w:
    with w.record("000000000009"):
        w.add_file("rgb", "val2017/000000000009.jpg", path)
```

ctypes releases the GIL during foreign calls, so the C I/O threads run
alongside Python.

## 9. CLI: separate `pzpdir` executable

`pzpdir` is its own binary, built from `pzpdir_cli.c` and linked against the
library in `pzpdir.c` (new Makefile targets: `pzpdir`, `dpzpdir`, `spzpdir`,
`libpzpdir.so`, `libpzpdir.a`). The library `#include`s `pzp.h` only for `pzpd_read_pzp`. The `pzp` binary,
`libpzp.so` and their docs are not modified.

Naming: the tool is `pzpdir` rather than `pzpd`, so it can't be misread as
"pzp debug". Builds follow the existing prefix pattern: `pzpdir` (release),
`dpzpdir` (debug), `spzpdir` (SIMD). Archive files use the `.pzpd` extension,
and C symbols use the `pzpd_` prefix.

```
pzpdir pack  <out.pzpd> <record-list|->  [--streams rgb,all,geo,depth,seg]
             [--align 64|4096] [--shard-size 4G]       # grouping comes from the list (§3.3)
pzpdir pack  <out.pzpd> <dir>                  # plain single-stream archive, key = relative path

# typical use: the dataset-specific writer script produces the list
python3 scripts/pzpdir_list_from_db.py coco/cocoVal.db coco/cache/coco | pzpdir pack coco_val2017.pzpd - --streams rgb,all,geo,depth,seg
pzpdir ls    <a.pzpd> [--long] [--names]       # records, or every blob name
             # --long: key  stream  FORMAT  WxHxC@bits | N lines | F frames  size  name
pzpdir cat   <a.pzpd> <record-key|blob-name> [--stream S] > out
pzpdir cat   <a.pzpd> <record-key> --table T    # rows as CSV
pzpdir export-table <a.pzpd> T > t.csv          # key column + rows, all records (global: just rows)
pzpdir replace-table | add-table | drop-table  <a.pzpd> T [LIST|-] [--missing empty|keep]
pzpdir compact <a.pzpd>                         # drop dead table sections left by table edits
pzpdir info  <a.pzpd> [--stream S]            # streams, coverage, shards (+ storage kind: RAM/BLOCK and the AUTO mode), sizes, and per stream:
             # format mix, WxH / channels / bits combos, min/max/avg — all from the index
pzpdir verify  <a.pzpd|shard> [--blobs]
pzpdir unpack  <a.pzpd|shard> <dir>            # restores every blob under its original name
pzpdir replace-stream | add-stream | drop-stream  <a.pzpd> STREAM [LIST|-] [--missing keep|drop]
pzpdir salvage <damaged shard> <dir>           # record-header scan (§4.1 recovery step 3)
pzpdir rebuild-manifest <name>.*.pzpd
pzpdir collect <out.pzpd> [alias=]<a.pzpd> [alias=]<b.pzpd> ...   # write a collection file (member paths stored relative when possible, absolute otherwise, or --absolute)
pzpdir collect --refresh <out.pzpd>                               # re-read member counts / streams
pzpdir info --dups <a.pzpd|collection>                            # list keys present in >1 member
```

Every read command (`ls`, `cat`, `info`, `verify`, `unpack`) accepts a
collection file or several archive paths. `ls` and `unpack` prefix output with
the member alias when there is more than one member (`unpack` writes each
member into `<dir>/<alias>/`).

## 10. DataLoader integration: access patterns (follow-up; plan only)

The integration itself is not part of v1. This section records how
`RGBToPoseDetect2D/datasets/DataLoader` reads files today and how the archive
and prefetcher map onto that. The v1 API has to support it without changes.

### 10.1 How files are read today

Summarised from `PrepareBatch.c` (`workerThread`), `DataLoader.c` and `DataLoader.py`:

| Aspect | Current behaviour |
|---|---|
| Epoch order | `db->indices[]`: a permutation of sample numbers, reshuffled by `db_shuffle_indices` / `db_shuffle_indices_via_loss` after `db_pipeline_drain`. It is fixed for the whole epoch, so the **full future order is known** |
| Batch | Contiguous range of positions `[start, end)` in shuffled order. Batches advance sequentially |
| Pipelining | Python double buffer (`get_partial_update_IO_array`): while the GPU consumes batch *k*, `db_StartUpdate` fills batch *k+1*. It never runs across the epoch boundary. A seek or reshuffle drains the pipeline |
| Work split | Worker *t* of *T* handles positions `start+t, start+t+T, …`, so within a batch reads go out **strided and in parallel**, not in position order |
| Position → sample | `db_resolve_sample_and_source(pos)` → `sampleNumber = indices[pos]`, `sourceID` (dataset source) |
| Path resolution | `resolveSampleFiles` → `resolvePathToRequestedFiles`: builds 5 paths from the source's dirs and `imagePath`, with a `fileExists()` probe on first touch only ([B5], cached `DL_FileKind`) |
| "Prefetch" | `signalPrefetchFile` = `open` + `posix_fadvise(WILLNEED)` on rgb and combined (or depth/seg), issued **microseconds** before the read of the same sample. `freeFileDescriptor` then does `fadvise(DONTNEED)` + `close`. It gives no real look-ahead and costs 2 extra syscalls per file |
| Read | `cachedReadImage` → `read_file_to_common_memory_of_cache` (open/read/close into a per-thread buffer; `READ_WHOLE_IMAGE_FILE_BEFORE_DECODING=1`) → `readImageFromMemory(name, buf, size, codec)`. For rgb the codec is `NO_CODEC`, which is guessed from the **filename extension** |
| RAM cache | `preloadAllFiles` / `USE_RAM_CACHE` (currently 0): reads every file once into `struct cache` |

**Per-sample read sequence and conditions** (in program order):

| # | File | Read when | Notes |
|---|---|---|---|
| 1 | combined (`all_`) | `multiplexed && (DO_DEPTH \|\| DO_SEGMENTATION)` | read **before** the erase decision, so it is read even for erased samples |
| – | *erase decision* | `numberOfSkeletons == 0 && eventOccurs(chanceDestroy)` | random at run time and can't be predicted; skips 2–5. Default `chanceDestroy = 0` |
| 2 | rgb | always (unless erased) | a sample whose rgb doesn't load aborts training |
| 3 | depth | `!multiplexed && addDepthHeatmap && DO_DEPTH && depthFound` | |
| 4 | seg | `!multiplexed && addSegmentationHeatmaps && DO_SEGMENTATION && segFound` | |
| 5 | geo | `addGeolocationHeatmap && geoFound` | read last, after all augmentation |

`DO_*` all equal `doHeatmapOutput`. With heatmaps off (token-only training)
only rgb (+geo) is read.

### 10.2 Opening and mapping (once, at DB load): the archive *is* the DB

1. Each dataset source (`dbSources[sourceID]`) is one archive. Today each source
   is a `.db` file, a descriptor file and 5 directories; with PZPD it's one
   `.pzpd`. All sources are opened as one collection (§3.4) in `sourceID` order,
   so `pzpd_member_of(ordinal) == sourceID` and the `startOffset` concatenation
   in `readPoseDatabase` is just the collection's ordinal concatenation.
   Directory + `.db` sources stay supported, and a DB may mix both kinds.
2. **Samples = records.** `PoseDatabase` is filled from tables instead of parsing `DB1` text:
   - `numberOfSamples = pzpd_count()`, and sample *i* = ordinal *i*. With
     `ignoreNoSkeletonSamples`, a filter map of ordinals whose `persons` row count > 0.
   - `imagePath` = record key. `width` / `height` come from `image`.
     `numberOfSkeletons` = `persons` row count, and `sk[]` from `persons` rows
     (a copy of ~112 B per person, milliseconds in total; zero-copy is possible
     if `struct Skeleton` is laid out like the schema).
   - `joint[]` names / parents from the global `joints` table; `keypointsForEachSample` = its row count.
   - `descriptionTokens` / `numberOfTokens` are **computed at load**: tokenize
     every `descriptions.text` (of the chosen `source`) across all collection
     members, build the sorted vocabulary, map words to IDs, then apply the
     blacklist and synonym map as today (§3.6). Token IDs therefore always match the
     current dataset mix. `maxTokenValue` = vocabulary size. With a **pinned
     vocabulary** (`index_to_word.json`, e.g. the checkpoint's), words are mapped
     through it instead of building a new one.
   - `descriptor` = a **pointer into the mmapped `descriptor_<model>` table**, like
     today's pointer into `DescriptorDataset`, but without the positional
     basename matching. The length comes from the table schema (like today's
     `value_count`, capped by `MAX_DESCRIPTOR_LENGTH`), and `db_get_batch_descriptors`
     uses it instead of any compile-time constant.
   - SuperPoint (`SPT1`) stays a separate file, matched as today.
   The `DB1` parser, `fastReadDatabaseNumberOfSamples` / `…KeypointsPerSample`
   and the descriptor matching in `DBLoader.c` collapse into a ~100-line archive adapter.
3. There is no `ordinalOf[]` lookup: the sample number *is* the ordinal (via the
   filter map when filtering). `resolveSampleFiles` disappears entirely.
4. From the blob table, derive per-sample presence (`hasAll`, `hasDepth`,
   `hasSeg`, `hasGeo`). This replaces `DL_FileKind` / `depthFound` / `segFound` /
   `multiplexed` / `geoFound` with **zero** `stat()` calls.
5. Codec per blob comes from the FourCC (`PZP `→`PZP_CODEC`, `PNG `→`PNG_CODEC`,
   `JPEG`→`JPG_CODEC`). Passing the blob's original name to
   `readImageFromMemory` keeps the existing extension-guess working too.
6. **Validate at load, not mid-epoch:** the checks `PrepareBatch.c` does today
   with `abort()` during training become index-only checks at DB load:
   depth must be 1 ch / 16 bit (`:982–983`), geo must be 1 ch / 16 bit (`:1249`),
   and rgb must have 3 channels (`makeSureImageHas3Channels`). Also: rgb present,
   `persons.kp` length = 3 × `joints` rows, and joint sets identical across
   collection members. A bad sample is reported by key and name before the first batch.

**Integration gates** (follow-up project):
- the C tokenizer reproduces `buildVocabulary.py` exactly (same words, same
  sorted vocabulary, same IDs) on all description sets;
- a `PoseDatabase` built from the archive **with the vocabulary pinned to the
  one the `.db` was made with** equals one built by `readPoseDatabase`, field by
  field (incl. `ignoreNoSkeletonSamples`);
- `db_get_batch_descriptors` works for D = 768 and D = 1024 without recompiling.

### 10.3 Which streams to fetch (per-sample mask)

The mask is a pure function of run config (fixed per run) and presence (fixed
per sample), so it is computed once per epoch and submitted with the order:

```
mask(s) = rgb
        | (hasAll(s) && (DO_DEPTH || DO_SEG)          ? all   : 0)
        | (!hasAll(s) && addDepth && DO_DEPTH && hasDepth(s) ? depth : 0)
        | (!hasAll(s) && addSeg   && DO_SEG   && hasSeg(s)   ? seg   : 0)
        | (addGeo && hasGeo(s)                         ? geo   : 0)
```

With the stream order `rgb, all, geo, depth, seg`, **every real mask is one
contiguous span**:
- multiplexed sample → rgb+all+geo is the record prefix;
- non-multiplexed sample → `all` is absent and costs 0 bytes, so rgb+geo+depth+seg is contiguous;
- a sample with *both* `all` and depth/seg present (COCO val2017 has both) → only the prefix is read, and depth/seg are never touched.

So one sample = **one `pread`** in every case. The erase decision is random, so the
record is still fetched; this wastes a `chanceDestroy × background-fraction`
share of I/O (0 by default), and the worker then calls `pzpd_prefetch_discard`.

### 10.4 Prefetch schedule (the look-ahead)

```
 epoch e order (db->indices)  ─────────────────────────────────────────────▶
 [ batch k: workers decoding ][ batch k+1: StartUpdate ][ k+2 … k+n: prefetcher I/O ][ not yet read ]
            ▲ GPU consumes k-1       ▲ Python double buffer        ▲ window / budget
```

| Event | Action |
|---|---|
| DB created / after each shuffle (`db_shuffle_indices*`, which already drains) | `pzpd_prefetch_clear`; build `ords[pos] = ordinalOf[indices[pos]]` and `masks[pos]` for the **whole epoch**; `pzpd_prefetch_submit(ords, masks, N)`. The prefetcher itself throttles to `window` / `budget` |
| `db_StartUpdate(start, end)` where `start` ≠ expected next position (seek, validation jump, Python cold start) | `clear` + resubmit from `start` (re-anchor) |
| Worker processes position *p* | `pzpd_prefetch_get(pf, ords[p], masks[p], refs, &t)`, then decode each blob from `refs[]` in the existing order (combined → rgb → depth → seg → geo), then `release(t)` after geo, the last use. The worker holds ≤ 1 record (~1 MB) at a time |
| Sample erased | `release` if already fetched, otherwise `discard` |
| Epoch end | The schedule runs dry at the last position. Nothing crosses the boundary (as today), and the next shuffle resubmits. Optional later: build the next permutation early so the first batches of epoch e+1 are warm |
| Validation DB (a separate `ImageDatabase`, sequential `update(0, N)`) | Its own prefetcher on the **same** `pzpd *` handle, submitting `0..N-1` in order, with its own smaller budget |

Sizing:
- **Look-ahead** must cover at least batch k+2, i.e. `window ≥ 2 × batchSize`,
  since k+1 is already being consumed by the StartUpdate workers.
- **BUFFERS mode:** `budget_bytes / avg span` sets the real depth, e.g.
  1 GiB / 650 KB ≈ 1 600 records ≈ 12–50 batches ahead for batch 32–128.
- **PAGECACHE mode:** `window` ≈ the same record count, bounded by free RAM.
- **Mode choice:** `PZPD_PF_AUTO` per member: COCO staged on a RAM disk → MAP
  (zero-copy views, pre-faulted); COCO on NVMe → PAGECACHE; ImageNet on NVMe →
  BUFFERS + O_DIRECT, which avoids evicting the page cache every epoch.
- **RAM disk replaces `USE_RAM_CACHE`:** `preloadAllFiles` makes a private
  in-process copy of every file. A tmpfs-staged archive with MAP views gives the
  same "everything in RAM" behaviour, shared across processes, with no load phase
  beyond a `cp`.
- **Threads:** I/O threads (default 4) are separate from the *T* decode workers.
  The workers stay CPU-bound, and their "loading" time in
  `logThreadProgress(..."rgb_loading")` should drop to buffer hand-off time.

**Removed per sample:** `resolveSampleFiles`, 3–5 × `signalPrefetchFile` (open +
fadvise), 3–5 × `open/fstat/read/close`, and 3–5 × `freeFileDescriptor`
(fadvise + close). In their place: one `pread` done ahead of time by an I/O thread, and a
lock-light `get`/`release`.

### 10.5 What this requires of the v1 API

These are the reason §6 has these features:
- `submit` accepts a **per-ordinal mask**, because multiplexed and plain samples differ.
- `get` works **out of order and from any thread** within the window, because workers are strided.
- `discard` exists for erased or skipped samples, so slots don't leak.
- The whole-epoch `submit` is O(N) memory (16 B per entry) with throttled I/O.
- Several prefetchers can share one `pzpd *` (train + validation).
- `stats` replaces `cache_readSpeedMBPerSecond`: it reports hits, waits, sync misses and bytes/s, per prefetcher.

### 10.6 Measuring it (extends §13.1)

Add a `dataloader-replay` workload to the benchmark. It takes the exact access
trace of `workerThread`: shuffled positions, strided over *T* workers, per-sample
masks, and batches of *B* with the k+1 double buffer. It replays that trace
against directories (current code path, including `signalPrefetchFile`) and
against the archive with and without the prefetcher. The trace runs without
decoding, so the I/O effect is isolated, and a `--decode` variant gives the
end-to-end number.

## 11. Decisions

| # | Question | Decision |
|---|---|---|
| D1 | Modalities layout | **Co-located records**: one archive, one read per sample; stream order chosen for contiguity (§3.1). *(Replaces v0.2 per-modality sets.)* |
| D2 | Naming | **Arbitrary bytes** for record keys and blob names (§3.2); every original filename kept and indexed |
| D3 | Sharding | **Self-contained ~4 GB shards** + rebuildable manifest; each shard recoverable alone |
| D4 | Grouping and order | **Decided by the writing application** (Writer API or record list); blobs of a record may come from any directories; record order = write order (`.db` sample order for `.db`-backed datasets); the archive has no grouping rules (§3.3) |
| D5 | Prefetch payload | **Raw bytes only**; one record span per I/O |
| D6 | Checksums on read | Off by default; `PZPD_O_VERIFY`, `verify`, `salvage`; hashing via vendored **xxHash** (`third_party/xxhash.h`): XXH32 for blobs / record headers, XXH64 for index sections, superblocks and keys |
| D7 | Platform | **Linux only** |
| D8 | Video | **Groups with range reads in v1**; a group never spans shards |
| D9 | v1 scope | Read/write/CLI, annotation tables + table replace/add/drop, stream replace/add/drop, prefetch **all modes (AUTO, MAP, PAGECACHE, BUFFERS incl. O_DIRECT)**, video groups. **Not** record append, **not** DataLoader integration |
| D10 | Naming of artifacts | `pzpdir` / `dpzpdir` / `spzpdir`, `libpzpdir.so`, `pzpdir.h`, Python `pzp.pzpdir`; files `.pzpd`; C prefix `pzpd_`; packaging: `pzpdir.h` (declarations) + `pzpdir.c` (implementation) → `libpzpdir.so` / `libpzpdir.a`, CLI `pzpdir_cli.c` |
| D11 | Alignment | Per **record** (default 4096 → `O_DIRECT` capable); blobs inside a record at 64 |
| D12 | Required streams | **None.** A record holds whatever files the writer lists for it; any stream may be missing, including rgb. A record needs at least one blob (a key with no files never exists). |
| D13 | Multiple archives | Opened as **one** (`pzpd_open_many` or a collection file); ordinals concatenated; streams unioned by name; duplicate keys **kept**, `find` = first member, `find_in(member)` / `find_all` to disambiguate; member given as an argument, never parsed from the key |
| D14 | Identification and metadata | 8-byte magics on every PZPDIR file (`PZPD` + kind) plus version; every blob has a **FourCC format** detected from its magic bytes and **index-resident metadata** (width/height/channels/bits, line count for text, frame count for PZP containers), readable with no data I/O (§3.5) |
| D15 | Storage awareness | Storage detected per shard (`statfs`: tmpfs/ramfs → RAM, else BLOCK). `PZPD_PF_AUTO` picks **MAP** (mmap views + `MADV_POPULATE_READ` ahead, optional huge pages) on RAM disks, and PAGECACHE or BUFFERS on block devices. Collection member paths may be absolute, so RAM-disk and NVMe members can be mixed (§6.1) |
| D16 | Annotations | **Typed tables** inside the archive (§3.6): record tables (0..n rows per record) and global tables, stored binary in per-shard table sections (bulk-loadable, zero-parse) and written and read as **CSV**. They replace the `.db` file (joints, image size, persons; coordinates packed u16) and the DINOv2/v3 descriptor files (`descriptor_<model>` `bulk` tables, length D from the schema, never hardcoded). Descriptions are stored as **text** and tokenized at load, because token IDs depend on the vocabulary of the whole dataset mix. SuperPoint stays separate (too large). Table edits append sections and flip the superblock generation, with no image rewrite |
| D17 | Name slots | Stream and table names ≤ **23 bytes** (fixed superblock slots). Keys and blob names stay unlimited |
| D18 | Annotation backup | Non-bulk table rows are also copied into each `PZPDRECD` record header (~0.4 KB/record) so `salvage` recovers annotations |
| D19 | Collection API extras | Adopted: `pzpd_collection_write()` / `pzpd_collection_refresh()`, and `pzpd_find_all()` returns the **total** match count. Not adopted: `pzpd_member_has_stream()` |

No open questions remain for v0.4.

## 12. Phased plan

| Phase | Deliverable | Verify |
|---|---|---|
| 1 | `pzpdir.h`: shard format, records, writer (sharding, record headers, backup SB), reader (manifest + lazy shards), `pack` (record list + plain dir), format detection and metadata probes (§3.5), `ls`, `cat`, `info`, `verify`, `unpack` | 100 k synthetic records × 3 streams with **adversarial names** (spaces, unicode, many dots, 1-byte and 65 535-byte names, 30-deep paths, blobs of one record from different directories, missing streams) round-trip byte-identical across ≥3 shards; repeated keys / repeated stream in a record rejected; list escapes round-trip; `unpack` refuses `../x` and `/abs`; COCO val2017 5-stream `unpack` → `diff -r` identical; metadata of every COCO blob equals what the existing codecs report after a full decode (w/h/channels/bits), and text line counts equal `wc -l`; magic of each file kind checked by `file(1)`-style probe tests; open < 5 ms for 1 M records; truncation / bit-flip fuzz never crashes (ASAN) |
| 1b | Collections: `pzpd_open_many`, collection file, `collect`, `find_in` / `find_all`, stream union | Open coco_val2017 + a synthetic archive with overlapping keys and different streams: `count` = sum; every ordinal reads the same bytes as via its member; `find` picks the first member, `find_all` returns both; a stream missing from one member reads as missing; delete a member → its range fails with `PZPD_E_MEMBER_MISSING` and the other ordinals are unchanged; `collect --refresh` fixes a stale collection |
| 1c | Tables: schema parser, record/global tables, CSR sections, CSV in/out, record-header copies, `cat --table`, `export-table`; example writer `scripts/pzpdir_list_from_db.py` (`.db` + `descriptions.json` (+ `descriptionsOLD.json`) + descriptor file + dirs → record list) | COCO val2017 from `cocoVal.db` + `descriptions.json` + descriptors: the joints header, image and persons lines regenerated from tables are **byte-identical** to `cocoVal.db`; caption texts read back from `descriptions` are byte-identical to `descriptions.json`, and `buildVocabulary.py`'s tokenization of the texts read from the archive, with `descriptionsVL2/index_to_word.json`, reproduces `val2017/vocabulary.json` IDs for all 5 000 samples; tables `descriptor_dinov2` (D = 768) and a synthetic `descriptor_x` (D = 1024) coexist and read back with their own D; every descriptor equals the descriptor file bit-for-bit;  CSV out-of-range / malformed rows rejected with line numbers; bulk load of all tables for 1 M synthetic records < 1 s |
| 2 | Recovery: `salvage`, `rebuild-manifest`, backup-superblock fallback | Delete the manifest → rebuild matches; delete shard k → other ordinals still read, k fails cleanly; zero a shard's index → `salvage` recovers every blob with its name |
| 3 | `replace-stream` / `add-stream` / `drop-stream`; `replace-table` / `add-table` / `drop-table`; `compact` | Replace depth on COCO val2017 → every other stream byte-identical, depth equals the new source; kill -9 mid-run then rerun → correct final state; peak extra disk ≤ 1 shard + slack. Replace `descriptions` / add `descriptor_dinov3` → no record data rewritten (shard data bytes unchanged, only the tail grows), kill -9 between the append and the superblock flip → old or new generation valid, never mixed; `compact` removes the dead sections |
| 4 | Video groups + `pzpd_read_range` | Range read equals concatenated single reads; no group crosses a shard; an oversize clip gets its own shard |
| 5 | `libpzpdir.so` + Python `pzp.pzpdir` | pytest round-trip; `read_record` equals per-stream `read`; writer from Python equals the CLI `pack` byte-for-byte |
| 6 | Prefetcher PAGECACHE + MAP + AUTO; storage detection; `PZPD_O_HUGEPAGE` / `PZPD_O_POPULATE`; absolute member paths | Benchmark §13.1: NVMe cold cache, and the RAM-disk column (§13.2). AUTO picks MAP on tmpfs and PAGECACHE/BUFFERS on NVMe, per member of a mixed collection; MAP gets ≈ 0 minor faults in workers when pre-faulting is on |
| 7 | Prefetcher BUFFERS + `O_DIRECT` | Benchmark §13.1 under a cgroup memory limit; RSS ≤ budget + index; page cache does not grow; ASAN/valgrind clean; `clear` mid-epoch is safe |

## 13. Test and benchmark dataset: COCO val2017

Root: `RGBToPoseDetect2D/datasets/coco/cache/coco` (Samsung 980 PRO NVMe,
ext4, 16 cores, 31 GB RAM). All 5 000 stems are present in every modality.

| Stream | Directory | Files | Size | Avg | Name pattern |
|---|---|---|---|---|---|
| rgb | `val2017` | 5 000 (+4 `.json`, skipped) | 818 MB | 164 KB | `x.jpg` |
| all | `all_val2017` → `all_val2017PZPF` | 5 000 | 2.39 GB | 478 KB | `x.pzp` |
| geo | `geo_val2017` | 5 000 | 32 MB | 6.5 KB | `x.jpg.pzp` |
| depth | `depth_val2017` | 5 000 | 1.52 GB | 304 KB | `x.png` |
| seg | `segment_val2017` | 5 000 | 44 MB | 8.8 KB | `x.png` |

A record averages ~960 KB and the full archive is ~4.8 GB. The usual
DataLoader read set (rgb+all+geo, since `multiplexed` skips depth/seg:
`PrepareBatch.c:604/974/1064`) is ~650 KB per record and is the contiguous prefix.

Consequences for the tests:

- **Sharding at small sizes:** tests use `--shard-size 256M` (≈19 shards)
  and `64M` (≈75 shards) to exercise shard boundaries.
- **Grouping via the example writer:** `scripts/pzpdir_list_from_db.py` takes
  the sample list and order from `cocoVal.db`, maps each stem to the 5 dirs (so
  `x.jpg.pzp` and `x.jpg` pair up; the `.json` files are never listed), and uses
  `imagePath` as the key. Its output list is checked in as a
  regression fixture. That grouping logic is test tooling, not archive code.
- **Cold cache is mandatory:** the whole set fits in RAM. Before each cold run
  the benchmark evicts every source file and shard with
  `posix_fadvise(POSIX_FADV_DONTNEED)` (no root needed; `drop_caches` if run with
  sudo), and a residency check confirms <1% is cached.
- **BUFFERS / O_DIRECT** runs under `systemd-run --user --scope -p MemoryMax=1G`.
- **Disk space:** 8.2 GB free on `/home`. The 5-stream archive (~4.8 GB)
  plus one shard of headroom for `replace-stream` fits, but only just.
  The archive must be on the same NVMe as the source dirs for a fair
  comparison: `coco/cache/coco/pzpd/`. If space runs short, a 3-stream archive
  (rgb+all+geo, ~3.2 GB) is the fallback benchmark target.
- **RAM disk:** `/dev/shm` (tmpfs, 16 GB) holds unit tests **and** the RAM-disk
  benchmark column (§13.2). A separate `tmpfs -o huge=within_size` mount tests
  huge pages. It can't host the NVMe-vs-directory comparison, since there is no
  `O_DIRECT` and it isn't the same medium.

### 13.1 Benchmark matrix (`scripts/benchmark_pzpdir.py`, C driver for the hot loop)

Workload: for 5 000 samples in a **shuffled** order (fixed seed), read the
bytes of stream set A = rgb+all+geo (DataLoader default) and set B =
rgb+depth+seg+geo (non-multiplexed path), with T ∈ {1, 4, 8, 16} threads.
Decode is excluded by default; `--decode` adds it.

| Variant | How |
|---|---|
| `fs-open` | per-file `open/fstat/read/close` (today's `read_file_to_common_memory_of_cache`) |
| `fs-probe` | as above plus `fileExists()` probing as in `resolvePathToRequestedFiles` |
| `pzpd-blob` | `pzpd_read_into` per stream (separate preads, same file) |
| `pzpd-record` | `pzpd_read_record` (one pread per sample) |
| `pzpd-view` | `pzpd_view` (mmap) |
| `pzpd-pf-pagecache` | prefetcher PAGECACHE, window 256/1024 |
| `pzpd-pf-buffers` | prefetcher BUFFERS + O_DIRECT, budget 256 MB / 1 GB |

### 13.2 RAM-disk column

The archive (and, for comparison, the 5 source directories) is copied to tmpfs.
Variants: `fs-open` (dirs on tmpfs), `pzpd-record` (pread = memcpy),
`pzpd-view` (mmap, cold page tables), `pzpd-pf-map` (MAP mode, `POPULATE_READ`
ahead), and `pzpd-pf-map` + huge pages (`huge=within_size`). Cold cache does not
apply. Instead, measure **minor faults** and dTLB misses
(`perf stat -e minor-faults,dTLB-load-misses`) per sample. With `--decode`, this
column shows how much of the per-sample time is left for I/O at all. Staging
time (`cp` of shards vs `cp -r` of 25 000 files) is reported too.

Reported (13.1 and 13.2): samples/s, MB/s, p50/p99 latency per sample, syscalls per sample
(`strace -c`), bytes over-read (set B is not contiguous), open time (archive
vs directory listing), cold and warm, `pack` time, archive size vs `du -sb`,
and `ls`/`du`/`rsync --dry-run` time for directories vs the archive.

Table gates: `cocoVal.db` joints/image/persons regenerated from the archive are
byte-identical; caption texts byte-identical to `descriptions.json`;
`buildVocabulary.py` tokenization of the archived texts reproduces
`val2017/vocabulary.json`; descriptors bit-identical. (Archive-built vs
`.db`-built `PoseDatabase` equality is an integration gate, §10.2.) Also report the startup time to build `PoseDatabase`
from `cocoTrain.db` (49 MB text parse) vs from the archive tables (mmap + copy).

Correctness gates: `unpack` → `diff -r` against the original dirs (json
excluded); SHA-256 of every blob equals its source file; `find(name)` resolves
every one of the 25 000 original filenames.
