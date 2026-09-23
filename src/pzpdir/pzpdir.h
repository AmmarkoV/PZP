/*
PZPDIR PZP Directory Archives
Copyright (C) 2026 Ammar Qammaz

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/** @file pzpdir.h
 *  @brief  PZPD archives: millions of files (grouped into records) in a few self-contained shards.
 *
 *  Repository : https://github.com/AmmarkoV/PZP
 *  @author Ammar Qammaz (AmmarkoV)
 *
 *  The format is specified in doc/pzpd-spec.md (v0.4). This header is the public API; the
 *  implementation lives in pzpdir.c, which builds into libpzpdir.so / libpzpdir.a, or is
 *  vendored as-is into a program's own build (the DataLoader does this).
 *
 *  @section pzpd_overview Overview
 *  - An **archive** is a small manifest `<name>.pzpd` plus shards `<name>.NNNNN.pzpd`
 *    of at most a few GB each. Every shard is a complete archive on its own, so losing the
 *    manifest or other shards never loses the data of a surviving shard.
 *  - An archive holds **records** (one training sample each), numbered by **ordinal**
 *    0..pzpd_count()-1. A record has a **key** (arbitrary bytes, e.g. the image path) and
 *    at most one **blob** per **stream** (e.g. "rgb", "all", "geo", "depth", "seg").
 *    The blobs of a record are stored next to each other, so a record is one read.
 *  - Every blob keeps its original file name, its format (a FourCC such as `JPEG`) and
 *    index-resident metadata (width, height, channels, bits, line count, frame count).
 *  - Which files form a record, and in what order records go, is decided by the program
 *    that writes the archive (pzpd_writer_create() or a record list given to `pzpdir pack`).
 *  - Several archives can be opened **as one** (pzpd_open_many(), or pzpd_open() on a collection
 *    file written by pzpd_collection_write()). Their ordinals are concatenated in member order,
 *    their streams are merged by name, and duplicate keys are kept: pzpd_find() returns the first
 *    member's match, pzpd_find_in() / pzpd_find_all() disambiguate. Every other call works the same
 *    on one archive or many.
 *  - Annotations live in typed **tables** (spec §3.6): a *record table* gives each record 0..n rows
 *    (e.g. persons, descriptions), a *global table* holds rows for the whole archive (e.g. joints).
 *    Rows are fixed-width and laid out like a C struct, stored next to the index, and read with zero
 *    parsing (pzpd_table_rows(), pzpd_table_shard_view()). CSV is the text form for writers and tools.
 *
 *  @section pzpd_reading Reading
 *  @code
 *  pzpd *a = pzpd_open("coco_val2017.pzpd", 0);
 *  if (a == NULL) { fprintf(stderr,"%s\n",pzpd_last_error()); return 0; }
 *
 *  int rgb = pzpd_stream_id(a,"rgb");
 *  int64_t i = pzpd_find(a,"000000000139.jpg",16,NULL);     // record key -> ordinal
 *
 *  pzpd_blob_info info;
 *  if ( (i>=0) && pzpd_blob_info_get(a,(uint64_t)i,(unsigned)rgb,&info) && info.present )
 *  {
 *      // info.width, info.height, info.channels, info.bits : no data I/O needed
 *      size_t size = 0;
 *      const void *jpeg = pzpd_view(a,(uint64_t)i,(unsigned)rgb,&size); // zero-copy, valid until pzpd_close()
 *  }
 *
 *  // Several streams of one record with a single pread():
 *  uint32_t mask = (1u<<rgb) | (1u<<pzpd_stream_id(a,"all"));
 *  size_t need = pzpd_record_span(a,(uint64_t)i,mask);
 *  void *buf = malloc(need);
 *  pzpd_blob_ref refs[PZPD_MAX_STREAMS];
 *  if (pzpd_read_record(a,(uint64_t)i,mask,buf,need,refs) >= 0)
 *  {
 *      // refs[rgb].data, refs[rgb].size, refs[rgb].format ...
 *  }
 *  free(buf);
 *  pzpd_close(a);
 *  @endcode
 *
 *  @section pzpd_prefetching Prefetching
 *  @code
 *  pzpd_prefetch_opts po = { 0 };                           // all streams, 4 I/O threads, AUTO, window 256
 *  pzpd_prefetcher *pf = pzpd_prefetcher_create(a,&po);
 *  pzpd_prefetch_submit(pf,epochOrder,epochMasks,n);        // the whole epoch; I/O runs `window` records ahead
 *  // any worker thread, any order:
 *  pzpd_blob_ref refs[PZPD_MAX_STREAMS];
 *  pzpd_ticket t;
 *  if (pzpd_prefetch_get(pf,epochOrder[pos],epochMasks[pos],refs,&t) >= 0) { decode(refs); }
 *  pzpd_prefetch_release(pf,&t);                            // or pzpd_prefetch_discard() for a claim never fetched
 *  // on reshuffle: pzpd_prefetch_clear(pf), then submit the new order
 *  pzpd_prefetcher_destroy(pf);
 *  @endcode
 *
 *  @section pzpd_writing Writing
 *  @code
 *  const char *streams[] = { "rgb", "depth" };
 *  pzpd_writer_opts o = { streams, 2, 0, 0 };                 // 0 = defaults (4 GiB shards, 4 KiB records)
 *  pzpd_writer *w = pzpd_writer_create("out.pzpd",&o);
 *  pzpd_writer_begin(w,"000000000139.jpg",16,PZPD_NO_GROUP,0);
 *  pzpd_writer_blob_file(w,0,"val2017/000000000139.jpg",24,"/data/coco/val2017/000000000139.jpg");
 *  pzpd_writer_blob_file(w,1,"depth_val2017/000000000139.png",30,"/data/coco/depth_val2017/000000000139.png");
 *  pzpd_writer_end(w);
 *  if (!pzpd_writer_finish(w)) { fprintf(stderr,"%s\n",pzpd_last_error()); }   // frees w in both cases
 *  @endcode
 *
 *  @section pzpd_threads Threads
 *  - A pzpd handle may be used by any number of threads at once for lookups and reads.
 *    Shards are opened lazily; the first touch of a shard takes a mutex once.
 *  - A pzpd_writer must be used by one thread at a time.
 *  - A pzpd_prefetcher may be used by any number of threads at once (a call locks only the lane of its record: the schedule is split by ordinal into lanes with their own locks);
 *    it runs its own I/O threads.
 *  - pzpd_last_error() is per thread.
 *  - The library has no global mutable state besides that thread-local error.
 *
 *  @section pzpd_build Build switches
 *  - PZPDIR_WITH_PZP (default 1) : 1 enables pzpd_read_pzp() and includes pzp.h. Programs that
 *    decode PZP themselves (and vendor their own pzp.h) build with 0. Format detection of PZP
 *    blobs works either way, since it only needs zstd / lz4.
 *  - PZPDIR_DEBUG (default 0) : 1 prints internal debug messages on stderr.
 */

#ifndef PZPDIR_H_INCLUDED
#define PZPDIR_H_INCLUDED

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/** @brief Library version, printed by programs that vendor pzpdir so copies can be told apart. */
static const char pzpdirVersion[]="0.11"; //0.11: phase 8, word index (per-source + merged sub-indexes, synonyms, reindex); 0.10: prefetcher lanes (a lock per lane, atomic window / budget), batched I/O-thread takes and wake-ups, stream names checked for the metadata JSON, faster resumed edit-stream; 0.9: review fixes (section-scan recovery after table edits, writer finish cleanup, NPY probe bound, faster edit-stream name check); 0.8: phase 4, video groups (names, early shard cut, read_range); 0.7: phase 3, stream / table edits and compact; 0.6: phase 2, recovery (section scan, rebuild-manifest, salvage); 0.5: phase 7, BUFFERS + O_DIRECT prefetch; 0.4: phase 6, prefetcher (PAGECACHE, MAP, AUTO) and storage detection; 0.3: phase 1c, typed annotation tables; 0.2: phase 1b, collections; 0.1: phase 1 (format, writer, reader)

#ifndef PZPDIR_WITH_PZP
/** @brief 1 enables pzpd_read_pzp() (includes pzp.h). Build with 0 when the program decodes PZP itself. */
#define PZPDIR_WITH_PZP 1
#endif

/** @brief On-disk format version written into every superblock, manifest and record header.
 *  Readers refuse files with a higher version instead of misreading fields at the wrong offsets. */
#define PZPD_FORMAT_VERSION 1

/** @brief Maximum number of streams in one archive. Stream masks (uint32_t) have one bit per stream. */
#define PZPD_MAX_STREAMS 32

/** @brief Longest stream name in bytes. Names live in fixed 24-byte slots (23 + NUL). */
#define PZPD_MAX_STREAM_NAME 23

/** @brief Longest record key or blob name in bytes (lengths are stored as u16). */
#define PZPD_MAX_NAME 65535

/** @brief Group id meaning "this record is not part of a video clip". */
#define PZPD_NO_GROUP 0xFFFFFFFFu

/** @brief Default maximum shard size: 4 GiB. A shard can exceed it by at most one record. */
#define PZPD_DEFAULT_SHARD_BYTES (4ull*1024ull*1024ull*1024ull)

/** @brief Default record alignment: 4096 bytes, so records can be read with O_DIRECT. */
#define PZPD_DEFAULT_ALIGN 4096u

/** @brief pzpd_open() flag: check the record header and payload checksums on every read. Slower; off by default. */
#define PZPD_O_VERIFY 1u

/** @brief pzpd_open_many() flag: open even if some member archives can't be opened (they then count as
 *  0 records). Collection files don't need it: they record each member's size, so a missing member keeps
 *  its ordinal range and only its reads fail (PZPD_E_MEMBER_MISSING). */
#define PZPD_O_ALLOW_MISSING 4u

/** @brief pzpd_open() flag: `madvise(MADV_HUGEPAGE)` on the shard mappings. Only effective on tmpfs mounted
 *  with `huge=within_size` (or `always`): a record then spans 0-1 2 MiB pages instead of ~160 small ones. */
#define PZPD_O_HUGEPAGE 8u

/** @brief pzpd_open() flag: open every shard at open time and pre-fault its whole mapping
 *  (`MADV_POPULATE_READ`). For RAM-disk archives that are mostly read every epoch (4.8 GB ≈ 1 s). */
#define PZPD_O_POPULATE 16u

/** @brief pzpd_collection_write() flag: store absolute member paths (default: relative to the collection file when possible). */
#define PZPD_COLL_ABSOLUTE 1u

/** @brief Most member archives in one collection. */
#define PZPD_MAX_MEMBERS 4096

/** @brief Most tables in one archive (or merged over a collection). */
#define PZPD_MAX_TABLES 16

/** @brief Most columns in one table schema. */
#define PZPD_MAX_COLUMNS 64

/** @brief Longest table or column name in bytes. */
#define PZPD_MAX_TABLE_NAME 23

/** @brief Most word indexes in one archive (spec §3.7). */
#define PZPD_MAX_WORD_INDEXES 4

/** @brief Most distinct source values (per-source sub-indexes) in one word index. */
#define PZPD_MAX_WORD_SOURCES 255

/** @brief Word index tokenizer v1: `re.findall(r'\w+', text.lower())` of Python, all-ASCII tokens only (spec §3.7). */
#define PZPD_TOKENIZER_V1 1

/** @brief pzpd_words_open() flag: apply the `synonyms` global table (canonical view); without it, surface words. */
#define PZPD_WORDS_CANONICAL 1u

/** @brief Name of the reserved global table with the word index's merge rules (schema `word:str,canonical:str`). */
#define PZPD_SYNONYMS_TABLE "synonyms"

/** @brief pzpd_writer_table() flag: the table holds rows for the whole archive, not per record (e.g. joints). */
#define PZPD_TABLE_GLOBAL 1u

/** @brief pzpd_writer_table() flag: large, re-derivable data (e.g. descriptors) that is **not** copied into the
 *  record headers for salvage. */
#define PZPD_TABLE_BULK 2u

/** @brief Where a shard lives, as far as reading it is concerned (pzpd_storage_kind()). */
enum pzpd_storage
{
    PZPD_STORAGE_BLOCK = 0,  ///< Anything backed by a device or network: ext4, xfs, btrfs, NFS, FUSE, brd, zram
    PZPD_STORAGE_RAM   = 1   ///< tmpfs / ramfs (e.g. /dev/shm): the file's pages are the page cache
};

/** @brief Prefetcher modes (spec §6). The mode is applied per shard. */
enum pzpd_pf_mode
{
    PZPD_PF_AUTO      = 0,  ///< Per shard: RAM disk → MAP; block device, member < ½ of the memory limit (RAM, or the cgroup's if lower) → PAGECACHE; else → BUFFERS
    PZPD_PF_MAP       = 1,  ///< mmap views, page tables pre-faulted ahead (`MADV_POPULATE_READ`); for RAM disks
    PZPD_PF_PAGECACHE = 2,  ///< mmap views, reads started ahead (`MADV_WILLNEED`) and completed + pre-faulted; for data that fits in RAM
    PZPD_PF_BUFFERS   = 3   ///< Private buffers filled with O_DIRECT reads (buffered where refused), within budget_bytes; the page cache doesn't grow. For data larger than RAM
};

/** @brief Column types of a table schema (`name:type[count]` in schema strings). */
enum pzpd_type
{
    PZPD_TYPE_U8  = 1,   ///< `u8`, 1 byte
    PZPD_TYPE_I8  = 2,   ///< `i8`, 1 byte
    PZPD_TYPE_U16 = 3,   ///< `u16`, 2 bytes
    PZPD_TYPE_I16 = 4,   ///< `i16`, 2 bytes
    PZPD_TYPE_U32 = 5,   ///< `u32`, 4 bytes
    PZPD_TYPE_I32 = 6,   ///< `i32`, 4 bytes
    PZPD_TYPE_U64 = 7,   ///< `u64`, 8 bytes
    PZPD_TYPE_I64 = 8,   ///< `i64`, 8 bytes
    PZPD_TYPE_F32 = 9,   ///< `f32`, 4 bytes (CSV: shortest text that reads back bit-exactly)
    PZPD_TYPE_F64 = 10,  ///< `f64`, 8 bytes
    PZPD_TYPE_STR = 11   ///< `str`, 8 bytes: a pzpd_str {offset, length} into the table's string heap
};

/** @brief Error codes returned by pzpd_last_error_code(). 0 means "no error". */
enum pzpd_error
{
    PZPD_OK               =   0,  ///< No error
    PZPD_E_IO             =  -1,  ///< A system call (open, read, write, mmap, rename, ...) failed; errno text is in the message
    PZPD_E_FORMAT         =  -2,  ///< Not a PZPD file, bad magic, bad checksum, or an offset pointing outside the file
    PZPD_E_VERSION        =  -3,  ///< The file was written with a newer PZPD_FORMAT_VERSION than this library knows
    PZPD_E_ARG            =  -4,  ///< Invalid argument (NULL pointer, ordinal or stream out of range, name too long, ...)
    PZPD_E_NOMEM          =  -5,  ///< Allocation failed
    PZPD_E_NOTFOUND       =  -6,  ///< Key, name or stream not found
    PZPD_E_DUPLICATE      =  -7,  ///< The writer was given a record key or blob name that already exists
    PZPD_E_STATE          =  -8,  ///< Call out of order (e.g. pzpd_writer_blob() outside begin/end)
    PZPD_E_CHECKSUM       =  -9,  ///< Stored data does not match its checksum
    PZPD_E_SHARD_MISSING  = -11,  ///< The shard holding the requested ordinal can't be opened
    PZPD_E_MEMBER_MISSING = -12,  ///< The collection member holding the requested ordinal can't be opened
    PZPD_E_STALE_MANIFEST = -13,  ///< A shard disagrees with the manifest (other archive, other generation)
    PZPD_E_STALE_COLLECTION = -14 ///< A member archive disagrees with the collection file (rebuilt, other size or streams); run `pzpdir collect --refresh`
};

/** @brief Build a FourCC format code from four characters, e.g. PZPD_FMT('J','P','E','G').
 *  The first character goes to the lowest byte, so the code reads left to right in a hex dump. */
#define PZPD_FMT(a,b,c,d) ( (uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b)<<8) | ((uint32_t)(uint8_t)(c)<<16) | ((uint32_t)(uint8_t)(d)<<24) )

#define PZPD_FORMAT_RAW   PZPD_FMT('R','A','W',' ')  ///< Unrecognised content
#define PZPD_FORMAT_JPEG  PZPD_FMT('J','P','E','G')  ///< JPEG (FF D8 FF)
#define PZPD_FORMAT_PNG   PZPD_FMT('P','N','G',' ')  ///< PNG (89 50 4E 47 0D 0A 1A 0A)
#define PZPD_FORMAT_PZP   PZPD_FMT('P','Z','P',' ')  ///< Single-frame PZP (size prefix + zstd/lz4 payload)
#define PZPD_FORMAT_PZPC  PZPD_FMT('P','Z','P','C')  ///< Multi-frame PZP container ("PZP0" magic)
#define PZPD_FORMAT_PNM   PZPD_FMT('P','N','M',' ')  ///< PBM / PGM / PPM (P1..P6)
#define PZPD_FORMAT_PFM   PZPD_FMT('P','F','M',' ')  ///< Portable float map (Pf / PF)
#define PZPD_FORMAT_NPY   PZPD_FMT('N','P','Y',' ')  ///< NumPy .npy array
#define PZPD_FORMAT_JSON  PZPD_FMT('J','S','O','N')  ///< UTF-8 text starting with { or [
#define PZPD_FORMAT_CSV   PZPD_FMT('C','S','V',' ')  ///< UTF-8 text with a .csv name
#define PZPD_FORMAT_TSV   PZPD_FMT('T','S','V',' ')  ///< UTF-8 text with a .tsv name
#define PZPD_FORMAT_TEXT  PZPD_FMT('T','E','X','T')  ///< Other valid UTF-8 text without NUL bytes

#define PZPD_META_VALID      0x01u  ///< The metadata fields were filled (detected or given by the writer)
#define PZPD_META_FLOAT      0x02u  ///< Samples are floating point (PFM, float NPY)
#define PZPD_META_BIG_ENDIAN 0x04u  ///< Multi-byte samples are big-endian (PFM with positive scale, '>' NPY)
#define PZPD_META_INDEXED    0x08u  ///< Palette / indexed image (e.g. PNG colour type 3); channels = 1 index
#define PZPD_META_USER       0x10u  ///< Metadata was given by the writer (pzpd_writer_blob_ex()), not detected

/** @brief Format and metadata of one blob, as detected by pzpd_detect_format() or given to pzpd_writer_blob_ex(). */
typedef struct
{
    uint32_t format;     ///< FourCC (PZPD_FORMAT_*), PZPD_FORMAT_RAW when unknown
    uint32_t width;      ///< Pixels; for text formats the number of lines; for NPY the first dimension; 0 if n/a
    uint32_t height;     ///< Pixels; for NPY the second dimension; 0 if n/a
    uint16_t channels;   ///< Channels per pixel; for NPY the third dimension (saturating at 65535); 0 if n/a
    uint16_t frames;     ///< Frames of a PZP container (saturating at 65535); 1 for still images; 0 if n/a
    uint8_t  bits;       ///< Bits per channel (1, 8, 16, 32, 64); 0 if n/a
    uint8_t  meta_flags; ///< PZPD_META_* bits
} pzpd_blob_meta;

/** @brief Everything the index knows about one blob. Filled by pzpd_blob_info_get() without data I/O. */
typedef struct
{
    int            present;   ///< 1 if the record has a blob in this stream, 0 if the stream is missing for it
    uint64_t       size;      ///< Payload bytes
    pzpd_blob_meta meta;      ///< Format and metadata
    const char    *name;      ///< Original file name (NOT NUL-terminated), points into the mapped index; valid until pzpd_close()
    size_t         name_len;  ///< Length of name in bytes
    uint32_t       group;     ///< Video group of the record, PZPD_NO_GROUP if none
    uint32_t       frame;     ///< Frame index of the record within its group
    unsigned       member;    ///< Collection member holding the record (0 for a single archive)
    unsigned       shard;     ///< Shard holding the record (index over all members' shards, as pzpd_shard_info_get())
} pzpd_blob_info;

/** @brief One blob inside a buffer filled by pzpd_read_record(). */
typedef struct
{
    const void *data;    ///< Start of the blob inside the caller's buffer, NULL if absent or not requested
    size_t      size;    ///< Payload bytes, 0 if absent or not requested
    uint32_t    format;  ///< FourCC of the blob, 0 if absent or not requested
} pzpd_blob_ref;

/** @brief Location and size of one shard, for tools. Filled by pzpd_shard_info_get(). */
typedef struct
{
    const char *path;          ///< Shard file path as opened (NUL-terminated), valid until pzpd_close()
    uint64_t    first_ordinal; ///< Ordinal of the shard's first record
    uint64_t    record_count;  ///< Records in the shard
    uint64_t    file_bytes;    ///< Shard file size in bytes (0 if the shard could not be opened)
    int         available;     ///< 1 if the shard opened correctly, 0 if missing or damaged
    unsigned    member;        ///< Collection member the shard belongs to
    int         storage;       ///< enum pzpd_storage of the file system holding the shard (PZPD_STORAGE_BLOCK if unavailable)
    int         recovery;      ///< How its superblock was found: 0 primary, 1 backup at EOF, 2 rebuilt from the index sections (spec §4.1)
} pzpd_shard_info;

/** @brief Value of a `str` column inside a row: bytes in the table's string heap (resolve with pzpd_table_str()). */
typedef struct
{
    uint32_t offset;  ///< Offset in the string heap (in rows given to pzpd_writer_rows(): offset in the caller's strings buffer)
    uint32_t len;     ///< Length in bytes (strings are arbitrary bytes, not NUL-terminated)
} pzpd_str;

/** @brief One column of a table schema. */
typedef struct
{
    const char *name;   ///< Column name (NUL-terminated)
    uint8_t     type;   ///< enum pzpd_type
    uint16_t    count;  ///< Array length (1 for a scalar), e.g. 51 for `kp:u16[51]`
    uint32_t    offset; ///< Byte offset of the column in a row
} pzpd_column;

/** @brief A table schema. Rows follow C struct layout: columns in order, each at its natural alignment,
 *  the stride padded to the largest alignment, so a C struct with the same members maps onto a row. */
typedef struct
{
    const char        *name;       ///< Table name
    unsigned           flags;      ///< PZPD_TABLE_GLOBAL, PZPD_TABLE_BULK
    uint32_t           row_stride; ///< Bytes per row
    unsigned           ncols;      ///< Number of columns
    const pzpd_column *cols;       ///< Columns
} pzpd_schema;

/** @brief All rows of one record table in one shard, for bulk loading (pzpd_table_shard_view()). */
typedef struct
{
    uint64_t        first_ordinal; ///< Ordinal (over the whole handle) of the shard's first record
    uint64_t        records;       ///< Records in the shard
    const uint32_t *row_index;     ///< records + 1 entries: rows of record i are [row_index[i], row_index[i+1]); NULL if the member lacks the table
    const void     *rows;          ///< total_rows × row_stride bytes (mapped)
    uint64_t        total_rows;    ///< Rows in the shard
    const char     *strings;       ///< String heap of the table in this shard (`str` offsets point here)
    uint64_t        strings_len;   ///< Its size
} pzpd_table_view;

/** @brief Options for pzpd_prefetcher_create(). Zero values mean "use the default". */
typedef struct
{
    uint32_t stream_mask;  ///< Streams fetched for submitted ordinals without their own mask (0 = all streams)
    unsigned io_threads;   ///< I/O threads (0 = 4)
    unsigned mode;         ///< enum pzpd_pf_mode (0 = PZPD_PF_AUTO)
    uint64_t budget_bytes; ///< BUFFERS memory cap: prefetched buffers plus those held by tickets (0 = 512 MiB; one record may exceed it)
    unsigned window;       ///< Most prefetched records not yet released or discarded (0 = 256)
} pzpd_prefetch_opts;

/** @brief A claim returned by pzpd_prefetch_get(), given back with pzpd_prefetch_release(). Contents are private. */
typedef struct
{
    uint64_t pos;    ///< Private: schedule position, UINT64_MAX when the get matched no submitted claim
    uint64_t gen;    ///< Private: schedule generation (pzpd_prefetch_clear() starts a new one)
    void    *buf;    ///< Private: the record's buffer (BUFFERS shards), freed by pzpd_prefetch_release()
    uint64_t bytes;  ///< Private: its size as counted against the budget
} pzpd_ticket;

/** @brief Counters of a prefetcher since it was created (pzpd_prefetch_stats_get()). */
typedef struct
{
    uint64_t submitted;        ///< Claims submitted
    uint64_t prefetched;       ///< Records the I/O threads made resident ahead of their get
    uint64_t hits;             ///< Gets whose record was already prefetched
    uint64_t waits;            ///< Gets that waited for their record's prefetch to finish
    uint64_t sync_misses;      ///< Gets that came before the I/O threads reached their claim (read on demand; the claim is skipped)
    uint64_t unscheduled;      ///< Gets of an ordinal with no pending claim (read on demand)
    uint64_t discarded;        ///< Claims dropped with pzpd_prefetch_discard()
    uint64_t released;         ///< Tickets released
    uint64_t producer_stalls;  ///< Times an I/O thread waited because `window` records were outstanding
    uint64_t bytes_prefetched; ///< Bytes made resident by the I/O threads (including over-read gaps)
    uint64_t bytes_over_read;  ///< Gap bytes between requested blobs that were read to keep one range per record
    double   io_seconds;       ///< Time spent in prefetch I/O, summed over I/O threads
    double   elapsed_seconds;  ///< Wall time since pzpd_prefetcher_create()
    unsigned shards_map;       ///< Shards prefetched in MAP mode
    unsigned shards_pagecache; ///< Shards prefetched in PAGECACHE mode
    unsigned shards_buffers;   ///< Shards prefetched in BUFFERS mode
    unsigned direct_fallbacks; ///< BUFFERS shards read with buffered pread() because O_DIRECT was refused
    uint64_t buffer_bytes;     ///< BUFFERS memory in use now (prefetched and held by tickets)
    uint64_t buffer_bytes_peak;///< Its highest value
} pzpd_prefetch_stats;

/** @brief One blob found by pzpd_salvage(). Pointers are valid during the callback only. */
typedef struct
{
    unsigned       stream;      ///< Stream id within the shard
    const char    *stream_name; ///< Its name, or NULL when the names couldn't be recovered
    const char    *name;        ///< Original file name (NOT NUL-terminated)
    size_t         name_len;    ///< Its length
    const void    *data;        ///< Payload
    size_t         size;        ///< Payload bytes
    pzpd_blob_meta meta;        ///< Format and metadata, as stored in the record header
    int            intact;      ///< 1 if the payload matches its XXH32, 0 if it is damaged
} pzpd_salvaged_blob;

/** @brief One record's copy of its rows of a (non-bulk) record table, found by pzpd_salvage(). */
typedef struct
{
    unsigned    table;          ///< Table id (writer order, as in the archive)
    const char *table_name;     ///< Table name, NULL when no schema source was given
    uint32_t    rows;           ///< Rows
    const void *row_data;       ///< rows × row_stride bytes; `str` offsets are relative to `strings`
    size_t      row_bytes;      ///< Its size
    const char *strings;        ///< The rows' strings
    size_t      strings_len;    ///< Their size
    const char *csv;            ///< The rows as CSV text, one line per row (NUL-terminated), NULL without a schema
    size_t      csv_len;        ///< Its length
} pzpd_salvaged_rows;

/** @brief One record found by pzpd_salvage(). Pointers are valid during the callback only. */
typedef struct
{
    uint64_t                  file_offset; ///< Offset of its header in the shard file
    const char               *key;         ///< Record key (NOT NUL-terminated)
    size_t                    key_len;     ///< Its length
    uint32_t                  group;       ///< Video group, PZPD_NO_GROUP if none
    uint32_t                  frame;       ///< Frame index in the group
    unsigned                  blob_count;  ///< Blobs
    const pzpd_salvaged_blob *blobs;       ///< The blobs, in on-disk order
    unsigned                  table_count; ///< Row copies
    const pzpd_salvaged_rows *tables;      ///< The row copies
} pzpd_salvaged_record;

/** @brief pzpd_salvage() callback. @return Nonzero to continue, 0 to stop the scan. */
typedef int (*pzpd_salvage_fn)(const pzpd_salvaged_record *rec, void *user);

/** @brief What pzpd_salvage() found. */
typedef struct
{
    uint64_t records;          ///< Intact record headers
    uint64_t blobs;            ///< Blobs whose payload matches its checksum
    uint64_t damaged_blobs;    ///< Blobs whose payload doesn't (still reported, with intact = 0)
    uint64_t damaged_headers;  ///< Record magics whose header is inconsistent or fails its checksum (skipped)
    unsigned stream_count;     ///< Stream names known (0 if none)
    char     streams[PZPD_MAX_STREAMS][24]; ///< Their names
    int      names_from;       ///< Where the names came from: 0 nowhere, 1 a superblock, 2 the metadata section, 3 the schemas handle
    int      schemas_from;     ///< 0 no table schemas, 2 the schemas handle
} pzpd_salvage_info;

/** @brief Edit operations (pzpd_edit_table(), pzpd_edit_stream()). */
#define PZPD_EDIT_ADD     1u  ///< Add a table / stream that doesn't exist yet
#define PZPD_EDIT_REPLACE 2u  ///< Replace an existing table's rows / stream's blobs
#define PZPD_EDIT_DROP    3u  ///< Remove a table / stream

/** @brief pzpd_edit_table() flag: records without new rows keep their old rows (`--missing keep`; default: they get none). */
#define PZPD_EDIT_KEEP_MISSING 1u
/** @brief pzpd_edit_stream() flag: records without a new file lose their old blob (`--missing drop`; default: kept). */
#define PZPD_EDIT_DROP_MISSING 2u

/** @brief Rows of one record for pzpd_edit_table(): CSV text for the record with this key (one or more rows). */
typedef struct
{
    const char *key;      ///< Record key
    size_t      key_len;  ///< Its length
    const char *csv;      ///< Rows as CSV (one line per row)
    size_t      csv_len;  ///< Its length
} pzpd_edit_rows;

/** @brief The new file of one record for pzpd_edit_stream(). */
typedef struct
{
    const char *key;      ///< Record key
    size_t      key_len;  ///< Its length
    const char *path;     ///< Source file (NUL-terminated); format and metadata are detected from it
    const char *name;     ///< Stored name
    size_t      name_len; ///< Its length
} pzpd_edit_blob;

/** @brief A video group (pzpd_group_info()): consecutive records in one shard, e.g. a clip's frames. */
typedef struct
{
    uint64_t    first_ordinal; ///< Ordinal of its first record (also the group's handle, see pzpd_group_find())
    uint32_t    frames;        ///< Records in the group
    uint32_t    id;            ///< Group id as written (pzpd_blob_info.group)
    uint32_t    index;         ///< Position of the queried record in the group (0-based)
    const char *name;          ///< Group name (NOT NUL-terminated; may be empty), valid until pzpd_close()
    size_t      name_len;      ///< Its length
} pzpd_group;

/** @brief Opaque read handle: one archive, a single shard, or several archives opened as one. Thread-safe for reads. */
typedef struct pzpd pzpd;

/** @brief Opaque prefetcher: I/O threads that make the records of a submitted schedule resident ahead of use. */
typedef struct pzpd_prefetcher pzpd_prefetcher;

/** @brief Opaque writer handle. Used by one thread at a time. */
typedef struct pzpd_writer pzpd_writer;

/** @brief Options for pzpd_writer_create(). Zero values mean "use the default". */
typedef struct
{
    const char *const *streams;  ///< Stream names, in on-disk order (e.g. { "rgb","all","geo","depth","seg" }); each ≤ PZPD_MAX_STREAM_NAME bytes
    unsigned           stream_count; ///< Number of streams, 1..PZPD_MAX_STREAMS
    uint64_t           shard_max_bytes; ///< Shard size limit in bytes (0 = PZPD_DEFAULT_SHARD_BYTES)
    uint32_t           align;    ///< Record alignment: 64 or 4096 (0 = PZPD_DEFAULT_ALIGN)
} pzpd_writer_opts;

//-----------------------------------------------------------------------------------------------
// Errors
//-----------------------------------------------------------------------------------------------

/**
 * @brief Message describing the last error of the calling thread.
 * @return A NUL-terminated string, "" if the last call succeeded. Valid until the next pzpdir call on this thread.
 */
const char *pzpd_last_error(void);

/**
 * @brief Code of the last error of the calling thread.
 * @return One of enum pzpd_error, PZPD_OK if the last call succeeded.
 */
int pzpd_last_error_code(void);

//-----------------------------------------------------------------------------------------------
// Opening
//-----------------------------------------------------------------------------------------------

/**
 * @brief Open an archive manifest or a single shard for reading.
 *
 * Only the manifest (or the shard's superblock and index) is mapped; the shards of an archive
 * are opened lazily, the first time one of their records is touched.
 * @param path  Path of `<name>.pzpd` (manifest) or of one `<name>.NNNNN.pzpd` shard (opened standalone).
 * @param flags 0 or PZPD_O_VERIFY.
 * @return A handle, or NULL on failure (see pzpd_last_error()).
 * @see pzpd_close()
 */
pzpd *pzpd_open(const char *path, unsigned int flags);

/**
 * @brief Open several archives as one.
 *
 * Ordinals are concatenated in the order given, streams are merged by name (first-seen order,
 * at most PZPD_MAX_STREAMS), and duplicate keys are kept. Members are archive manifests or single
 * shards; collection files can't be nested.
 * @param paths   n paths of manifests or shards.
 * @param aliases n member names, or NULL to use each file name without ".pzpd". Must be unique.
 * @param n       Number of members, 1..PZPD_MAX_MEMBERS.
 * @param flags   0, PZPD_O_VERIFY, PZPD_O_ALLOW_MISSING.
 * @return A handle, or NULL on failure (a missing member fails unless PZPD_O_ALLOW_MISSING is set).
 * @see pzpd_collection_write() to save the set as a collection file.
 */
pzpd *pzpd_open_many(const char *const *paths, const char *const *aliases, unsigned n, unsigned int flags);

/**
 * @brief Close a handle and unmap everything it mapped.
 * @param a Handle from pzpd_open(), may be NULL.
 * @warning All pointers returned by pzpd_view(), pzpd_record_key() and pzpd_blob_info_get() become invalid.
 */
void pzpd_close(pzpd *a);

/**
 * @brief Number of records in the archive.
 * @param a Open handle.
 * @return Record count (0 if a is NULL).
 */
uint64_t pzpd_count(const pzpd *a);

/**
 * @brief Number of streams declared in the archive.
 * @param a Open handle.
 * @return 1..PZPD_MAX_STREAMS (0 if a is NULL).
 */
unsigned pzpd_stream_count(const pzpd *a);

/**
 * @brief Look up a stream by name.
 * @param a    Open handle.
 * @param name NUL-terminated stream name, e.g. "rgb".
 * @return Stream id 0..pzpd_stream_count()-1, or -1 if there is no such stream.
 */
int pzpd_stream_id(const pzpd *a, const char *name);

/**
 * @brief Name of a stream.
 * @param a      Open handle.
 * @param stream Stream id.
 * @return NUL-terminated name, or NULL if stream is out of range. Valid until pzpd_close().
 */
const char *pzpd_stream_name(const pzpd *a, unsigned stream);

/**
 * @brief Number of member archives (1 unless several archives were opened as one).
 * @param a Open handle.
 * @return Member count (0 if a is NULL).
 */
unsigned pzpd_member_count(const pzpd *a);

/**
 * @brief Name of a member (its alias).
 * @param a      Open handle.
 * @param member Member index.
 * @return NUL-terminated alias, or NULL if member is out of range. Valid until pzpd_close().
 */
const char *pzpd_member_alias(const pzpd *a, unsigned member);

/**
 * @brief Look up a member by alias.
 * @param a     Open handle.
 * @param alias NUL-terminated alias.
 * @return Member index, or -1 if there is no such member.
 */
int pzpd_member_id(const pzpd *a, const char *alias);

/**
 * @brief Which member holds an ordinal.
 * @param a             Open handle.
 * @param ordinal       Ordinal over the whole handle.
 * @param local_ordinal Optional: set to the ordinal within the member.
 * @return Member index, or -1 if the ordinal is out of range.
 */
int pzpd_member_of(const pzpd *a, uint64_t ordinal, uint64_t *local_ordinal);

/**
 * @brief Ordinal range of a member.
 * @param a      Open handle.
 * @param member Member index.
 * @param first  Set to the member's first ordinal.
 * @param count  Set to the member's record count.
 * @return 1 on success, 0 if member is out of range.
 */
int pzpd_member_range(const pzpd *a, unsigned member, uint64_t *first, uint64_t *count);

/**
 * @brief Number of shards of all members (1 for a shard opened standalone).
 * @param a Open handle.
 * @return Shard count (0 if a is NULL).
 */
unsigned pzpd_shard_count(const pzpd *a);

/**
 * @brief Describe one shard (opens it if it wasn't open yet).
 * @param a     Open handle.
 * @param shard Shard index 0..pzpd_shard_count()-1.
 * @param out   Filled on success.
 * @return 1 on success (also when the shard is unavailable: out->available is then 0), 0 on bad arguments.
 */
int pzpd_shard_info_get(pzpd *a, unsigned shard, pzpd_shard_info *out);

/**
 * @brief Storage kind of the shard holding a record: `fstatfs()` of the shard, tmpfs / ramfs → RAM,
 *        anything else → BLOCK.
 * @param a       Open handle.
 * @param ordinal Record ordinal.
 * @return An enum pzpd_storage value, or a negative enum pzpd_error.
 */
int pzpd_storage_kind(pzpd *a, uint64_t ordinal);

//-----------------------------------------------------------------------------------------------
// Lookup and index-only information
//-----------------------------------------------------------------------------------------------

/**
 * @brief Find a record by key, or a blob by its original file name.
 *
 * Record keys are looked up first; if no record has this key, blob names are searched.
 * Comparison is exact, byte for byte.
 * @param a          Open handle.
 * @param key        Bytes of the key or name (not necessarily NUL-terminated).
 * @param len        Length of key in bytes.
 * @param stream_out Optional: set to -1 when a record key matched, or to the blob's stream when a blob name matched.
 * @return The ordinal, or -1 if nothing matched (or on error; see pzpd_last_error_code()).
 */
int64_t pzpd_find(pzpd *a, const char *key, size_t len, int *stream_out);

/**
 * @brief Like pzpd_find(), but only within one member.
 * @param a          Open handle.
 * @param member     Member index.
 * @param key        Bytes of the key or name.
 * @param len        Length of key in bytes.
 * @param stream_out Optional: -1 for a record key, the (merged) stream for a blob name.
 * @return The ordinal (over the whole handle), or -1 if nothing matched.
 */
int64_t pzpd_find_in(pzpd *a, unsigned member, const char *key, size_t len, int *stream_out);

/**
 * @brief Every match of a key or name, over all members (record keys first, in member order).
 * @param a            Open handle.
 * @param key          Bytes of the key or name.
 * @param len          Length of key in bytes.
 * @param ordinals_out Receives up to max ordinals (may be NULL when max is 0).
 * @param streams_out  Optional: receives the matching stream of each match, -1 for record keys.
 * @param max          Capacity of the output arrays.
 * @return The **total** number of matches, which may exceed max (only the first max are written).
 */
size_t pzpd_find_all(pzpd *a, const char *key, size_t len, int64_t *ordinals_out, int *streams_out, size_t max);

/**
 * @brief Key of a record.
 * @param a       Open handle.
 * @param ordinal Record ordinal.
 * @param len     Set to the key length in bytes.
 * @return Pointer to the key bytes (NOT NUL-terminated) inside the mapped index, valid until pzpd_close(); NULL on error.
 */
const char *pzpd_record_key(pzpd *a, uint64_t ordinal, size_t *len);

/**
 * @brief Everything the index knows about one blob: presence, size, name, format and metadata.
 *
 * Reads only the mapped index, never the blob data.
 * @param a       Open handle.
 * @param ordinal Record ordinal.
 * @param stream  Stream id.
 * @param out     Filled on success. When the record has no blob in this stream, out->present is 0.
 * @return 1 on success, 0 on error.
 */
int pzpd_blob_info_get(pzpd *a, uint64_t ordinal, unsigned stream, pzpd_blob_info *out);

//-----------------------------------------------------------------------------------------------
// Reading data
//-----------------------------------------------------------------------------------------------

/**
 * @brief Read one blob into a caller buffer (one pread()).
 * @param a       Open handle.
 * @param ordinal Record ordinal.
 * @param stream  Stream id.
 * @param buf     Destination.
 * @param cap     Capacity of buf in bytes.
 * @return Bytes read (the blob size), 0 if the stream is missing for this record,
 *         or a negative enum pzpd_error (e.g. PZPD_E_ARG if cap is too small).
 */
ssize_t pzpd_read_into(pzpd *a, uint64_t ordinal, unsigned stream, void *buf, size_t cap);

/**
 * @brief Read one blob into a newly allocated buffer.
 * @param a       Open handle.
 * @param ordinal Record ordinal.
 * @param stream  Stream id.
 * @param size    Set to the blob size.
 * @return A malloc'd buffer (free with pzpd_free()), or NULL on error or if the stream is missing (*size is then 0).
 */
void *pzpd_read_alloc(pzpd *a, uint64_t ordinal, unsigned stream, size_t *size);

/**
 * @brief Free a buffer returned by pzpd_read_alloc() or pzpd_read_pzp().
 * @param ptr Buffer, may be NULL.
 */
void pzpd_free(void *ptr);

/**
 * @brief Zero-copy view of one blob inside the memory-mapped shard.
 * @param a       Open handle.
 * @param ordinal Record ordinal.
 * @param stream  Stream id.
 * @param size    Set to the blob size.
 * @return Pointer to the blob bytes, valid until pzpd_close(); NULL on error or if the stream is missing (*size is then 0).
 * @note The first touch of each page may fault the data in from disk.
 */
const void *pzpd_view(pzpd *a, uint64_t ordinal, unsigned stream, size_t *size);

/**
 * @brief Buffer size needed by pzpd_read_record() for these streams.
 * @param a           Open handle.
 * @param ordinal     Record ordinal.
 * @param stream_mask Bit s set = stream s requested.
 * @return Bytes of the smallest span covering every requested, present blob; 0 if none is present or on error.
 */
size_t pzpd_record_span(pzpd *a, uint64_t ordinal, uint32_t stream_mask);

/**
 * @brief Read several streams of one record with a single pread().
 *
 * Reads the smallest byte span covering the requested blobs (the gaps between them are read too).
 * @param a           Open handle.
 * @param ordinal     Record ordinal.
 * @param stream_mask Bit s set = stream s requested.
 * @param buf         Destination, at least pzpd_record_span() bytes.
 * @param cap         Capacity of buf in bytes.
 * @param refs        Array of pzpd_stream_count() entries; refs[s] points into buf for every requested,
 *                    present stream, and is {NULL,0,0} otherwise.
 * @return Bytes read, 0 if no requested stream is present, or a negative enum pzpd_error.
 */
ssize_t pzpd_read_record(pzpd *a, uint64_t ordinal, uint32_t stream_mask, void *buf, size_t cap, pzpd_blob_ref *refs);

/**
 * @brief Check a record against its checksums: the record header (and the payloads if asked).
 * @param a           Open handle.
 * @param ordinal     Record ordinal.
 * @param check_blobs 1 = also read every payload and compare its XXH32.
 * @return 1 if everything matches, 0 otherwise (PZPD_E_CHECKSUM, or the error that prevented the check).
 */
int pzpd_verify_record(pzpd *a, uint64_t ordinal, int check_blobs);

/**
 * @brief Check a shard's superblock and index sections against their XXH64 checksums.
 * @param a     Open handle.
 * @param shard Shard index.
 * @return 1 if they match, 0 otherwise.
 */
int pzpd_verify_shard(pzpd *a, unsigned shard);

//-----------------------------------------------------------------------------------------------
// Tables (annotations): index-resident, zero-copy
//-----------------------------------------------------------------------------------------------

/**
 * @brief Number of tables (merged by name over a collection's members).
 * @param a Open handle.
 * @return 0..PZPD_MAX_TABLES.
 */
unsigned pzpd_table_count(const pzpd *a);

/**
 * @brief Look up a table by name.
 * @param a    Open handle.
 * @param name NUL-terminated table name.
 * @return Table id, or -1 if there is no such table.
 */
int pzpd_table_id(const pzpd *a, const char *name);

/**
 * @brief Schema of a table.
 * @param a     Open handle.
 * @param table Table id.
 * @return The schema, or NULL if table is out of range. Valid until pzpd_close().
 */
const pzpd_schema *pzpd_table_schema(const pzpd *a, unsigned table);

/**
 * @brief Rows of a record table for one record (zero-copy, no data I/O).
 * @param a        Open handle.
 * @param ordinal  Record ordinal.
 * @param table    Id of a record table.
 * @param rows_out Set to the first row (row_stride bytes each), or NULL when there are no rows.
 * @return Number of rows (0 is normal). On error also 0, with pzpd_last_error_code() != PZPD_OK.
 */
uint32_t pzpd_table_rows(pzpd *a, uint64_t ordinal, unsigned table, const void **rows_out);

/**
 * @brief Rows of a global table of one member.
 * @param a        Open handle.
 * @param member   Member index (0 for a single archive).
 * @param table    Id of a global table.
 * @param rows_out Set to the first row, or NULL when there are no rows.
 * @return Number of rows. On error also 0, with pzpd_last_error_code() != PZPD_OK.
 */
uint32_t pzpd_global_rows(pzpd *a, unsigned member, unsigned table, const void **rows_out);

/**
 * @brief Resolve a `str` field of a record-table row.
 * @param a       Open handle.
 * @param ordinal Record ordinal the row belongs to.
 * @param table   Table id.
 * @param field   Pointer to the pzpd_str inside the row.
 * @param len     Set to the string length.
 * @return The bytes (NOT NUL-terminated) in the mapped heap, or NULL on error.
 */
const char *pzpd_table_str(pzpd *a, uint64_t ordinal, unsigned table, const void *field, size_t *len);

/**
 * @brief Resolve a `str` field of a global-table row.
 * @param a      Open handle.
 * @param member Member the row belongs to.
 * @param table  Table id.
 * @param field  Pointer to the pzpd_str inside the row.
 * @param len    Set to the string length.
 * @return The bytes (NOT NUL-terminated), or NULL on error.
 */
const char *pzpd_global_str(pzpd *a, unsigned member, unsigned table, const void *field, size_t *len);

/**
 * @brief All rows of a record table in one shard, as CSR arrays, for bulk loading at startup.
 * @param a     Open handle.
 * @param shard Shard index (over all members, as pzpd_shard_info_get()).
 * @param table Id of a record table.
 * @param out   Filled on success; out->row_index is NULL when the shard's member lacks the table.
 * @return 1 on success, 0 on error.
 */
int pzpd_table_shard_view(pzpd *a, unsigned shard, unsigned table, pzpd_table_view *out);

/**
 * @brief A record's rows of a record table as CSV text (one line per row).
 * @param a       Open handle.
 * @param ordinal Record ordinal.
 * @param table   Table id.
 * @param out     Buffer (may be NULL when cap is 0).
 * @param cap     Buffer size; the text is truncated to cap-1 bytes and NUL-terminated.
 * @return Length of the full text (like snprintf), or a negative enum pzpd_error.
 */
ssize_t pzpd_table_csv(pzpd *a, uint64_t ordinal, unsigned table, char *out, size_t cap);

/**
 * @brief A member's global-table rows as CSV text (one line per row).
 * @param a      Open handle.
 * @param member Member index.
 * @param table  Id of a global table.
 * @param out    Buffer (may be NULL when cap is 0).
 * @param cap    Buffer size; truncated to cap-1 bytes and NUL-terminated.
 * @return Length of the full text, or a negative enum pzpd_error.
 */
ssize_t pzpd_global_csv(pzpd *a, unsigned member, unsigned table, char *out, size_t cap);

//-----------------------------------------------------------------------------------------------
// Word index (spec §3.7, §4.10): per word its records / count / postings, per record its words
//-----------------------------------------------------------------------------------------------

/** @brief One view (surface or canonical) of one sub-index (merged, or one source) of a word index.
 *  Word ids are valid only for this handle; they are never token ids. Thread-safe once opened. */
typedef struct pzpd_words pzpd_words;

/**
 * @brief Open a word index.
 *
 * The handle's vocabulary is the union of the members' vocabularies, merged by word bytes and sorted
 * by bytes (ids follow that order). With PZPD_WORDS_CANONICAL, words are first mapped through the
 * members' `synonyms` tables: a canonical word's records are the union of its surface words' records
 * (computed here from the postings, so every shard holding one of them is opened).
 * @param a          Open handle.
 * @param table      Indexed table.
 * @param column     Indexed column.
 * @param source     Source value of a per-source sub-index, or NULL for the merged sub-index.
 * @param source_len Its length.
 * @param flags      PZPD_WORDS_CANONICAL, or 0.
 * @param out        Receives the handle (NULL on failure).
 * @return 1 on success, 0 on failure (PZPD_E_NOTFOUND: no member has this word index).
 */
int pzpd_words_open(pzpd *a, const char *table, const char *column, const char *source, size_t source_len,
                    unsigned flags, pzpd_words **out);

/** @brief Close a word index handle (NULL is ignored). The pzpd handle must outlive it. */
void pzpd_words_close(pzpd_words *w);

/**
 * @brief Source values of a word index, over all members, sorted by bytes.
 * @param a      Open handle.
 * @param table  Indexed table.
 * @param column Indexed column.
 * @param names  Receives up to max pointers (into the archive; valid while it is open), may be NULL.
 * @param lens   Receives their lengths, may be NULL.
 * @param max    Capacity of names / lens.
 * @return The total number of source values (0 without a source column or without the index).
 */
size_t pzpd_words_sources(pzpd *a, const char *table, const char *column, const char **names, size_t *lens, size_t max);

/**
 * @brief The i-th word index of a handle (over all members, in first-seen order).
 * @param a             Open handle.
 * @param i             Index, from 0.
 * @param table         Receives the indexed table (valid while a is open), may be NULL.
 * @param column        Receives the indexed column, may be NULL.
 * @param source_column Receives the source column ("" when the index has none), may be NULL.
 * @return 1 if there is an i-th word index, 0 otherwise.
 */
int pzpd_words_index(pzpd *a, unsigned i, const char **table, const char **column, const char **source_column);

/** @brief Words in the handle's vocabulary. */
uint32_t pzpd_words_count(const pzpd_words *w);

/** @brief Bytes of word id (not NUL-terminated), or NULL if out of range. */
const char *pzpd_words_word(const pzpd_words *w, uint32_t id, size_t *len);

/** @brief Id of a word (exact bytes), or -1 if it is not in the vocabulary. */
int64_t pzpd_words_find(const pzpd_words *w, const char *word, size_t len);

/** @brief Records containing word id, and its total occurrences. No shard is opened. @return 1, or 0 if out of range. */
int pzpd_words_stats(const pzpd_words *w, uint32_t id, uint64_t *records, uint64_t *count);

/**
 * @brief The whole vocabulary at once, for bulk loaders (zero-copy; valid while the handle is open).
 * @param w        Handle.
 * @param records  Receives records[id] (may be NULL).
 * @param count    Receives count[id] (may be NULL).
 * @param heap     Receives the words' bytes, concatenated in id order (may be NULL).
 * @param offsets  Receives count+1 offsets into heap: word id = heap[offsets[id] .. offsets[id+1]) (may be NULL).
 * @return The number of words.
 */
uint32_t pzpd_words_arrays(const pzpd_words *w, const uint64_t **records, const uint64_t **count, const char **heap, const uint64_t **offsets);

/**
 * @brief Ordinals of the records containing word id, ascending.
 * @param w        Handle.
 * @param id       Word id.
 * @param ordinals Receives up to max ordinals (may be NULL when max is 0).
 * @param max      Capacity.
 * @return The total number of such records (as pzpd_find_all()); on error 0 with pzpd_last_error_code() set.
 */
size_t pzpd_words_records(pzpd_words *w, uint32_t id, uint64_t *ordinals, size_t max);

/**
 * @brief Word ids of one record, ascending.
 * @param w       Handle.
 * @param ordinal Record ordinal.
 * @param ids     Receives up to max ids (may be NULL when max is 0).
 * @param max     Capacity.
 * @return The total number of the record's words; on error 0 with pzpd_last_error_code() set.
 */
size_t pzpd_words_of_record(pzpd_words *w, uint64_t ordinal, uint32_t *ids, size_t max);

/**
 * @brief Tokenizer version of the index, and how many records are covered (records of the members that
 *        have this word index; the others have no words).
 * @return 1.
 */
int pzpd_words_info(const pzpd_words *w, unsigned *tokenizer, uint64_t *covered_records);

/**
 * @brief Split text into words with tokenizer v1 (the rule the word index uses; spec §3.7).
 * @param text  UTF-8 text (invalid bytes split words like a non-word character).
 * @param len   Its length.
 * @param emit  Called once per word with its bytes (lower-case ASCII) and length; a non-zero return stops.
 * @param user  Passed to emit.
 * @return The number of words emitted.
 */
size_t pzpd_tokenize(const char *text, size_t len, int (*emit)(const char *word, size_t len, void *user), void *user);

#if PZPDIR_WITH_PZP
/**
 * @brief Read a PZP blob and decode it to pixels (wraps pzp_decompress_combined_from_memory()).
 * @param a        Open handle.
 * @param ordinal  Record ordinal.
 * @param stream   Stream id holding a PZP blob.
 * @param width    Set to the image width.
 * @param height   Set to the image height.
 * @param bpp      Set to the bits per channel (8 or 16).
 * @param channels Set to the number of channels.
 * @return Interleaved pixels (free with pzpd_free()), or NULL on error.
 * @note Only available when built with PZPDIR_WITH_PZP=1. Uses pzp.h's per-thread ZSTD context:
 *       threads that call it should call pzp_thread_cleanup() before exiting.
 */
unsigned char *pzpd_read_pzp(pzpd *a, uint64_t ordinal, unsigned stream,
                             unsigned int *width, unsigned int *height,
                             unsigned int *bpp, unsigned int *channels);
#endif

//-----------------------------------------------------------------------------------------------
// Video groups (spec §3.3, §4.6)
//-----------------------------------------------------------------------------------------------

/**
 * @brief Find a video group by name.
 * @param a    Open handle.
 * @param name Group name bytes.
 * @param len  Length.
 * @return Ordinal of the group's first record (use it with pzpd_group_info() / pzpd_read_range()), or -1 (PZPD_E_NOTFOUND).
 */
int64_t pzpd_group_find(pzpd *a, const char *name, size_t len);

/**
 * @brief The group a record belongs to.
 * @param a       Open handle.
 * @param ordinal Any record of the group.
 * @param out     Filled when the record is in a group.
 * @return 1 if it is, 0 if it isn't (pzpd_last_error_code() == PZPD_OK) or on error.
 */
int pzpd_group_info(pzpd *a, uint64_t ordinal, pzpd_group *out);

/**
 * @brief Bytes pzpd_read_range() needs for records first .. first+count-1 and these streams.
 * @param a           Open handle.
 * @param first       First record.
 * @param count       Records.
 * @param stream_mask Streams wanted.
 * @return The span, 0 if no requested blob is present or on error.
 */
size_t pzpd_range_span(pzpd *a, uint64_t first, uint32_t count, uint32_t stream_mask);

/**
 * @brief Read several streams of consecutive records (e.g. frames k..k+n of a clip) with a single pread().
 *
 * The records must be in one shard and in one group (or all in none): a range never crosses a group or
 * shard boundary.
 * @param a           Open handle.
 * @param first       First record.
 * @param count       Records.
 * @param stream_mask Streams wanted.
 * @param buf         Destination, at least pzpd_range_span() bytes.
 * @param cap         Its capacity.
 * @param refs        count × pzpd_stream_count() entries: refs[k * S + s] for record first+k, stream s.
 * @return Bytes read, 0 if no requested blob is present, or a negative enum pzpd_error.
 */
ssize_t pzpd_read_range(pzpd *a, uint64_t first, uint32_t count, uint32_t stream_mask, void *buf, size_t cap, pzpd_blob_ref *refs);

//-----------------------------------------------------------------------------------------------
// Prefetcher (spec §6): explicit, whole-epoch schedules, raw bytes only
//-----------------------------------------------------------------------------------------------

/**
 * @brief Create a prefetcher on an open handle and start its I/O threads.
 *
 * Several prefetchers may share one handle (e.g. training + validation). The handle must outlive them.
 * @param a Open handle.
 * @param o Options, or NULL for the defaults.
 * @return The prefetcher, or NULL on error.
 */
pzpd_prefetcher *pzpd_prefetcher_create(pzpd *a, const pzpd_prefetch_opts *o);

/**
 * @brief Append records to the schedule. The whole epoch order can be submitted at once: the I/O
 *        threads work through it in order, at most `window` records ahead of the releases.
 *
 * An ordinal may appear several times; each occurrence is a separate claim, matched by gets in order.
 * @param p        Prefetcher.
 * @param ordinals Records, in the order they will be used.
 * @param masks    Stream mask per record, or NULL to use the options' stream_mask for all.
 * @param n        Number of records.
 * @return 1 on success, 0 on error (an ordinal out of range: nothing is appended).
 */
int pzpd_prefetch_submit(pzpd_prefetcher *p, const uint64_t *ordinals, const uint32_t *masks, size_t n);

/**
 * @brief Drop the whole schedule (e.g. after a reshuffle). Waits for in-flight I/O; tickets from
 *        before the clear stay valid to read and are ignored by pzpd_prefetch_release().
 * @param p Prefetcher.
 */
void pzpd_prefetch_clear(pzpd_prefetcher *p);

/**
 * @brief Get a record's blobs, from any thread and in any order.
 *
 * Takes the first pending claim of the ordinal. Already prefetched → returns at once; being prefetched
 * → waits for it; not reached yet → the claim is skipped by the I/O threads and the record is read on
 * demand. A get never fails because of the schedule, it only costs latency. On MAP / PAGECACHE shards
 * refs are views valid until pzpd_close(); on BUFFERS shards they point into the ticket's buffer and
 * are valid until pzpd_prefetch_release().
 * @param p       Prefetcher.
 * @param ordinal Record ordinal.
 * @param mask    Streams wanted (may differ from the submitted mask; extra streams are read on demand).
 * @param refs    Array of pzpd_stream_count() entries; refs[s] is filled for every wanted, present stream
 *                and is {NULL,0,0} otherwise.
 * @param t       Ticket to pass to pzpd_prefetch_release().
 * @return Number of present wanted streams, or a negative enum pzpd_error (the claim is then released).
 */
int pzpd_prefetch_get(pzpd_prefetcher *p, uint64_t ordinal, uint32_t mask, pzpd_blob_ref *refs, pzpd_ticket *t);

/**
 * @brief Give a claim back once its blobs are no longer used (frees a BUFFERS ticket's buffer), so the I/O threads can move ahead.
 * @param p Prefetcher.
 * @param t Ticket from pzpd_prefetch_get(); reset so a second release does nothing.
 */
void pzpd_prefetch_release(pzpd_prefetcher *p, pzpd_ticket *t);

/**
 * @brief Drop the first pending claim of an ordinal without getting it (e.g. the sample was erased).
 *        A queued claim is cancelled; a prefetched one frees its window slot.
 * @param p       Prefetcher.
 * @param ordinal Record ordinal (no effect if it has no pending claim).
 */
void pzpd_prefetch_discard(pzpd_prefetcher *p, uint64_t ordinal);

/**
 * @brief Read the prefetcher's counters.
 * @param p Prefetcher.
 * @param s Filled with the counters.
 */
void pzpd_prefetch_stats_get(const pzpd_prefetcher *p, pzpd_prefetch_stats *s);

/**
 * @brief Mode that PZPD_PF_AUTO gives a shard, from its storage kind and the size of its member.
 *
 * A shard on tmpfs / ramfs gets PZPD_PF_MAP. Otherwise its member gets PZPD_PF_PAGECACHE when it is smaller than half
 * the memory this process may use (physical RAM, or the limit of its cgroup or an ancestor when lower: systemd
 * MemoryMax, Slurm, containers), and PZPD_PF_BUFFERS when not.
 * @param a     Open handle.
 * @param shard Shard index (over all members, as pzpd_shard_info_get()).
 * @return An enum pzpd_pf_mode value, or a negative enum pzpd_error.
 */
int pzpd_prefetch_auto_mode(pzpd *a, unsigned shard);

/**
 * @brief Stop the I/O threads and free the prefetcher. Release every ticket first; views of MAP / PAGECACHE
 *        shards already handed out stay valid (they belong to the handle).
 * @param p Prefetcher, may be NULL.
 */
void pzpd_prefetcher_destroy(pzpd_prefetcher *p);

//-----------------------------------------------------------------------------------------------
// Recovery (spec §4.1): the ladder is automatic on open (backup superblock, then the index sections
// found by their magic; see pzpd_shard_info.recovery). These two cover the rest.
//-----------------------------------------------------------------------------------------------

/**
 * @brief Write the manifest of an archive from its shards (`pzpdir rebuild-manifest`).
 *
 * Every shard is read on its own, so a lost or stale manifest can be regenerated; the result is
 * byte-identical to the manifest the writer produced. The shards must all be given, must belong to
 * one archive, and must be in the manifest's directory (the manifest stores their file names).
 * @param manifest_path Manifest to write (replaced atomically).
 * @param shard_paths   The shard files, in any order.
 * @param n             Number of shards.
 * @return 1 on success, 0 on error.
 */
int pzpd_manifest_rebuild(const char *manifest_path, const char *const *shard_paths, unsigned n);

/**
 * @brief Recover the records of a shard whose index is damaged or gone (`pzpdir salvage`).
 *
 * Scans the shard for record headers (spec §4.1, step 3). Every record header carries the key and,
 * per blob, its stream, name, format, metadata, size and XXH32, plus copies of the record's rows of
 * non-bulk tables. A header counts only if its own checksum matches; after one, the scan skips the
 * record's payloads, so data that happens to contain a record magic is never mistaken for a record.
 * @param shard_path Shard file.
 * @param schemas    Optional open handle of the same archive (its manifest, or another shard) that
 *                   supplies stream names and table schemas (then rows also come as CSV). NULL:
 *                   stream names from the shard's own superblocks or metadata section, if intact.
 * @param fn         Called once per recovered record (may be NULL to only count).
 * @param user       Passed to fn.
 * @param info       Filled with counts and the stream names used (may be NULL).
 * @return 1 when the scan ran (even if nothing was found), 0 on error (file can't be read).
 */
int pzpd_salvage(const char *shard_path, pzpd *schemas, pzpd_salvage_fn fn, void *user, pzpd_salvage_info *info);

//-----------------------------------------------------------------------------------------------
// Edits (spec §7). The archive must not be open for reading elsewhere while it is edited. Every edit is
// resumable: rerunning the same command after a crash finishes it (shards already done are skipped),
// and the manifest is rewritten last. Collections that contain the archive need `collect --refresh`
// after a stream edit.
//-----------------------------------------------------------------------------------------------

/**
 * @brief Add, replace or drop a table, without rewriting any record data.
 *
 * Per shard, the new table section and a new superblock generation are appended at the end of the
 * file, then the primary superblock is switched to it; a crash leaves the old or the new generation,
 * never a mix. Old sections become dead space until pzpd_compact(). Record keys are matched to the
 * archive; keys that match no record are counted in *unmatched and otherwise ignored.
 * Record-header row copies (for salvage) keep the rows as packed.
 * @param manifest     The archive's manifest.
 * @param op           PZPD_EDIT_ADD / _REPLACE / _DROP.
 * @param table        Table name.
 * @param schema       Schema text (`name:type[n] ...`); required to add, optional to replace (NULL = keep).
 * @param table_flags  PZPD_TABLE_GLOBAL / PZPD_TABLE_BULK for an added table.
 * @param rows         Record rows (record tables), any order; several entries for one key are concatenated.
 * @param n            Number of entries.
 * @param global_csv   Rows of a global table (CSV), or NULL.
 * @param global_len   Its length.
 * @param flags        PZPD_EDIT_KEEP_MISSING.
 * @param unmatched    Receives the number of entries whose key is not in the archive (may be NULL).
 * @return 1 on success, 0 on error (shards already done stay done; rerun to finish).
 */
int pzpd_edit_table(const char *manifest, unsigned op, const char *table, const char *schema, unsigned table_flags,
                    const pzpd_edit_rows *rows, size_t n, const char *global_csv, size_t global_len, unsigned flags, uint64_t *unmatched);

/**
 * @brief Add, replace or drop a stream: each shard is rewritten next to itself (generation + 1), verified,
 *        and renamed over the old one, so the extra disk space needed is one shard.
 *
 * Other streams' blobs are copied byte for byte with their stored metadata; the new files get their
 * format and metadata detected. Record order, keys, groups and table rows are unchanged.
 * Before anything is written, the whole edit is checked: no new name may exist elsewhere in the archive,
 * keys and names must be unique in the input, and no record may be left without blobs.
 * @param manifest  The archive's manifest.
 * @param op        PZPD_EDIT_ADD / _REPLACE / _DROP.
 * @param stream    Stream name.
 * @param blobs     New files (add / replace), any order.
 * @param n         Number of entries (0 for drop).
 * @param flags     PZPD_EDIT_DROP_MISSING (replace: records without a new file lose the stream).
 * @param unmatched Receives the number of entries whose key is not in the archive (may be NULL).
 * @return 1 on success, 0 on error (shards already rewritten stay rewritten; rerun to finish).
 */
int pzpd_edit_stream(const char *manifest, unsigned op, const char *stream, const pzpd_edit_blob *blobs, size_t n, unsigned flags, uint64_t *unmatched);

/**
 * @brief Remove the dead table sections left by table edits (crash-safe, in place, per shard).
 * @param manifest  The archive's manifest.
 * @param reclaimed Receives the bytes freed (may be NULL).
 * @return 1 on success, 0 on error.
 */
int pzpd_compact(const char *manifest, uint64_t *reclaimed);

/**
 * @brief Add, rebuild or drop a word index of an existing archive (`pzpdir reindex`; spec §7). Appends a
 *        new generation to each shard like a table edit (crash-safe), then rewrites the manifest.
 * @param manifest      The archive's manifest.
 * @param op            PZPD_EDIT_ADD, PZPD_EDIT_REPLACE (rebuild, or add when missing) or PZPD_EDIT_DROP.
 * @param table         Indexed record table.
 * @param column        Indexed `str` column.
 * @param source_column `str` column naming each row's source (per-source sub-indexes), or NULL.
 * @return 1 on success, 0 on error.
 */
int pzpd_edit_words(const char *manifest, unsigned op, const char *table, const char *column, const char *source_column);

//-----------------------------------------------------------------------------------------------
// Formats
//-----------------------------------------------------------------------------------------------

/**
 * @brief Detect the format of a blob and read its metadata from its headers (never a full decode).
 *
 * Content is checked first (magic bytes). The name's extension is used only to tell CSV / TSV
 * from other text, and as a fallback when the content isn't recognised (the metadata is then
 * left invalid). Unknown content gives PZPD_FORMAT_RAW.
 * @param data     Blob bytes.
 * @param size     Blob size.
 * @param name     Original file name (for the extension), may be NULL.
 * @param name_len Length of name.
 * @param meta_out Filled with the format and metadata.
 * @return The detected FourCC.
 */
uint32_t pzpd_detect_format(const void *data, size_t size, const char *name, size_t name_len, pzpd_blob_meta *meta_out);

/**
 * @brief Printable form of a FourCC.
 * @param fourcc Format code.
 * @param out    At least 5 bytes; receives the 4 characters and a NUL (non-printable bytes become '?').
 * @return out.
 */
const char *pzpd_format_name(uint32_t fourcc, char out[5]);

//-----------------------------------------------------------------------------------------------
// Collection files
//-----------------------------------------------------------------------------------------------

/**
 * @brief Write a collection file: several archives saved as one openable set.
 *
 * Opens every member to record its identity (uuid), record count and streams, so that later opens
 * keep each member's ordinal range even if it is missing, and notice when it was rebuilt.
 * @param out_path Collection file to write (conventionally `<name>.pzpd`).
 * @param paths    n member paths (manifests or shards; not collections).
 * @param aliases  n member names, or NULL for each file name without ".pzpd".
 * @param n        Number of members, 1..PZPD_MAX_MEMBERS.
 * @param flags    0, or PZPD_COLL_ABSOLUTE to store absolute paths (default: relative to out_path's
 *                 directory, so the set can be moved together).
 * @return 1 on success, 0 on failure.
 */
int pzpd_collection_write(const char *out_path, const char *const *paths, const char *const *aliases, unsigned n, unsigned flags);

/**
 * @brief Rewrite a collection file from its members as they are now (after a member was rebuilt
 *        or edited). Keeps the aliases and the relative / absolute choice of every path.
 * @param path Collection file.
 * @return 1 on success, 0 on failure (e.g. a member is missing).
 */
int pzpd_collection_refresh(const char *path);

//-----------------------------------------------------------------------------------------------
// Writing
//-----------------------------------------------------------------------------------------------

/**
 * @brief Start writing a new archive.
 *
 * Shards are written as `<base>.NNNNN.pzpd.tmp` and renamed when complete; the manifest is written
 * last by pzpd_writer_finish(). `<base>` is manifest_path without its ".pzpd" extension.
 * @param manifest_path Path of the manifest to create, e.g. "coco_val2017.pzpd".
 * @param o             Stream names and options.
 * @return A writer, or NULL on failure.
 * @see pzpd_writer_finish(), pzpd_writer_abort()
 */
pzpd_writer *pzpd_writer_create(const char *manifest_path, const pzpd_writer_opts *o);

/**
 * @brief Begin a record. Records get consecutive ordinals in the order they are written.
 * @param w       Writer.
 * @param key     Record key bytes (1..PZPD_MAX_NAME, no NUL bytes); must be unique in the archive.
 * @param key_len Length of key.
 * @param group   Video group id, or PZPD_NO_GROUP. Frames of a group must be written consecutively.
 * @param frame   Frame index within the group (ignored for PZPD_NO_GROUP).
 * @return 1 on success, 0 on failure.
 */
int pzpd_writer_begin(pzpd_writer *w, const char *key, size_t key_len, uint32_t group, uint32_t frame);

/**
 * @brief Add a blob from memory to the current record. The data is copied.
 * @param w        Writer.
 * @param stream   Stream id (index into pzpd_writer_opts::streams); each stream at most once per record.
 * @param name     Original file name (1..PZPD_MAX_NAME bytes, no NUL); unique in the archive.
 * @param name_len Length of name.
 * @param data     Blob bytes.
 * @param size     Blob size (≤ 4 GiB - 1).
 * @return 1 on success, 0 on failure.
 */
int pzpd_writer_blob(pzpd_writer *w, unsigned stream, const char *name, size_t name_len, const void *data, size_t size);

/**
 * @brief Add a blob from a file to the current record.
 * @param w        Writer.
 * @param stream   Stream id.
 * @param name     Name to store (usually the path relative to the dataset root).
 * @param name_len Length of name.
 * @param src_path File to read.
 * @return 1 on success, 0 on failure.
 */
int pzpd_writer_blob_file(pzpd_writer *w, unsigned stream, const char *name, size_t name_len, const char *src_path);

/**
 * @brief Add a blob with caller-supplied format and metadata (no detection).
 * @param w        Writer.
 * @param stream   Stream id.
 * @param name     Original file name.
 * @param name_len Length of name.
 * @param data     Blob bytes (copied).
 * @param size     Blob size.
 * @param meta     Format and metadata to store; PZPD_META_USER and PZPD_META_VALID are set automatically.
 *                 NULL behaves like pzpd_writer_blob().
 * @return 1 on success, 0 on failure.
 */
int pzpd_writer_blob_ex(pzpd_writer *w, unsigned stream, const char *name, size_t name_len,
                        const void *data, size_t size, const pzpd_blob_meta *meta);

/**
 * @brief Declare a table. All tables must be declared before the first record.
 * @param w      Writer.
 * @param name   Table name (1..PZPD_MAX_TABLE_NAME bytes), distinct from the stream names.
 * @param schema Columns as `name:type[count]`, separated by commas and/or spaces, e.g.
 *               "id:u16, bbox:u16[4], kp:u16[51]"; types u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 str.
 * @param flags  0, PZPD_TABLE_GLOBAL, PZPD_TABLE_BULK.
 * @return The table id (0, 1, ... in declaration order), or -1 on failure.
 */
int pzpd_writer_table(pzpd_writer *w, const char *name, const char *schema, unsigned flags);

/**
 * @brief Add binary rows of a record table to the current record (between begin and end).
 * @param w           Writer.
 * @param table       Table id.
 * @param rows        nrows × row_stride bytes, laid out as pzpd_table_schema() describes.
 * @param nrows       Number of rows.
 * @param strings     Bytes that the `str` fields' {offset, len} point into (NULL if the table has no `str` column).
 * @param strings_len Size of strings.
 * @return 1 on success, 0 on failure.
 */
int pzpd_writer_rows(pzpd_writer *w, unsigned table, const void *rows, uint32_t nrows, const void *strings, size_t strings_len);

/**
 * @brief Add rows of a record table as CSV text (one row per line; RFC 4180 quoting for `str`;
 *        an array column takes `count` consecutive fields). Numbers are range-checked.
 * @param w     Writer.
 * @param table Table id.
 * @param csv   Text (not necessarily NUL-terminated).
 * @param len   Length of csv.
 * @return 1 on success, 0 on failure (the message names the row and column).
 */
int pzpd_writer_rows_csv(pzpd_writer *w, unsigned table, const char *csv, size_t len);

/**
 * @brief Add binary rows of a global table. Global rows must be added before the first record.
 * @param w           Writer.
 * @param table       Id of a global table.
 * @param rows        nrows × row_stride bytes.
 * @param nrows       Number of rows.
 * @param strings     Bytes the `str` fields point into (or NULL).
 * @param strings_len Size of strings.
 * @return 1 on success, 0 on failure.
 */
int pzpd_writer_global_rows(pzpd_writer *w, unsigned table, const void *rows, uint32_t nrows, const void *strings, size_t strings_len);

/**
 * @brief Add rows of a global table as CSV text. Global rows must be added before the first record.
 * @param w     Writer.
 * @param table Id of a global table.
 * @param csv   Text.
 * @param len   Length of csv.
 * @return 1 on success, 0 on failure.
 */
int pzpd_writer_global_rows_csv(pzpd_writer *w, unsigned table, const char *csv, size_t len);

/**
 * @brief Register a video group: its records are then written with pzpd_writer_begin(w, key, len, id, frame).
 *
 * A group's records must be consecutive and their frames increasing; a group never spans shards. When
 * the group's expected size (bytes_hint) doesn't fit in the open shard, a new shard starts before it;
 * a group larger than the shard limit gets a shard of its own. Records may also use group ids that
 * were never registered (unnamed groups, no hint).
 * @param w          Writer.
 * @param name       Group name, unique in the archive (NULL / 0 for an unnamed group).
 * @param len        Its length.
 * @param bytes_hint Expected bytes of all its records (0 = unknown: the group may overshoot the limit).
 * @return The group id, or -1 on error (e.g. PZPD_E_DUPLICATE name).
 */
int64_t pzpd_writer_group(pzpd_writer *w, const char *name, size_t len, uint64_t bytes_hint);

/**
 * @brief Declare a word index (spec §3.7), after its table and before the first record. Each shard gets
 *        the index built from the table's rows when it closes, and the manifest the merged vocabulary.
 * @param w             Writer.
 * @param table         A declared record table.
 * @param column        One of its `str` columns (a single value, not an array).
 * @param source_column Another `str` column naming each row's source: one sub-index per source value plus
 *                      the merged one. NULL: the merged sub-index only.
 * @return 1 on success, 0 on failure.
 */
int pzpd_writer_words(pzpd_writer *w, const char *table, const char *column, const char *source_column);

/**
 * @brief Finish the current record and write it.
 *
 * Checks that the key and every blob name are unique in the archive. On failure the record is
 * dropped and the writer stays usable.
 * @param w Writer.
 * @return 1 on success, 0 on failure.
 */
int pzpd_writer_end(pzpd_writer *w);

/**
 * @brief Complete the archive: close the last shard, record the shard count in every shard,
 *        and write the manifest. Frees the writer in every case.
 * @param w Writer.
 * @return 1 on success, 0 on failure (completed shards stay on disk and are readable standalone).
 */
int pzpd_writer_finish(pzpd_writer *w);

/**
 * @brief Abandon the archive: removes the unfinished shard's .tmp file and frees the writer.
 *        Shards that were already completed are left on disk.
 * @param w Writer, may be NULL.
 */
void pzpd_writer_abort(pzpd_writer *w);

#ifdef __cplusplus
}
#endif

#endif // PZPDIR_H_INCLUDED
