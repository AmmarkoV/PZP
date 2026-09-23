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

/** @file pzpdir.c
 *  @brief  Implementation of pzpdir.h (PZPD archives, spec doc/pzpd-spec.md v0.4).
 *
 *  The public functions are documented in the header; this file documents the internal
 *  helpers and the data they keep:
 *  - the on-disk structures (superblock, record header, index sections, manifest), all
 *    packed and little-endian, sizes checked with _Static_assert,
 *  - the thread-local error (pzpd_last_error()),
 *  - format detection (header-only probes, never a full decode),
 *  - the writer, which streams records into `.tmp` shards and keeps an archive-wide
 *    duplicate check of record keys and blob names,
 *  - the reader, which maps the manifest, opens shards lazily under a mutex, and
 *    bounds-checks every offset read from disk so damaged files fail cleanly.
 *
 *  Repository : https://github.com/AmmarkoV/PZP
 *  @author Ammar Qammaz (AmmarkoV)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE ///< pread / pwrite / mmap / O_CLOEXEC and friends (the DataLoader passes -D_GNU_SOURCE itself)
#endif

#include "pzpdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/vfs.h>
#include <time.h>
#include <sys/random.h>
#include <sys/types.h>

#include <zstd.h>
#include <lz4.h>

/** @brief Make every xxHash function static inline: no symbols are exported, so there is no
 *  clash with a system libxxhash or another vendored copy in the same program. */
#define XXH_INLINE_ALL
#include "third_party/xxhash.h"
#include "pzpdir_unicode.h"

#if PZPDIR_WITH_PZP
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wunused-function"
 #pragma GCC diagnostic ignored "-Wunused-variable"
 #include "pzp.h"
 #pragma GCC diagnostic pop
#endif

#ifndef PZPDIR_DEBUG
/** @brief Compile-time switch (0/1) for internal debug messages on stderr. */
#define PZPDIR_DEBUG 0
#endif

#define PZPD_NORMAL "\033[0m"   ///< ANSI escape: reset terminal color
#define PZPD_RED    "\033[31m"  ///< ANSI escape: red text
#define PZPD_GREEN  "\033[32m"  ///< ANSI escape: green text
#define PZPD_YELLOW "\033[33m"  ///< ANSI escape: yellow text

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "PZPD structures are little-endian and read in place");

//-----------------------------------------------------------------------------------------------
// Constants of the on-disk format
//-----------------------------------------------------------------------------------------------

/** @brief Superblocks, manifest headers and index sections live in 4 KiB slots / boundaries, so
 *  every section can be mmapped and found by scanning 4 KiB boundaries during recovery. */
#define PZPD_BLOCK 4096u

/** @brief Blob payloads inside a record start on 64-byte boundaries (cache line). */
#define PZPD_BLOB_ALIGN 64u

static const char PZPD_MAGIC_SHARD[8]    = {'P','Z','P','D','S','H','R','D'}; ///< Shard superblock (primary at 0, backup at EOF-4096)
static const char PZPD_MAGIC_MANIFEST[8] = {'P','Z','P','D','M','A','N','I'}; ///< Manifest header
static const char PZPD_MAGIC_COLL[8]     = {'P','Z','P','D','C','O','L','L'}; ///< Collection file (phase 1b)
static const char PZPD_MAGIC_RECORD[8]   = {'P','Z','P','D','R','E','C','D'}; ///< Start of every record header
static const char PZPD_MAGIC_SECTION[8]  = {'P','Z','P','D','S','E','C','T'}; ///< Start of every index section

/** @brief Kinds of index sections, stored in pzpd_disk_section::kind. */
enum pzpd_section_kind
{
    PZPD_SECT_RECORDS = 1, ///< Record table (pzpd_disk_record entries)
    PZPD_SECT_BLOBS   = 2, ///< Blob table (pzpd_disk_blob entries, row-major [record][stream])
    PZPD_SECT_HASH    = 3, ///< Shard-local key/name hash (pzpd_disk_hash entries, sorted)
    PZPD_SECT_HEAP    = 4, ///< String heap: record keys and blob names, no NULs
    PZPD_SECT_META    = 5, ///< Metadata JSON (stream names, uuid) for recovery tools
    PZPD_SECT_MSHARDS = 6, ///< Manifest: shard table
    PZPD_SECT_MNAMES  = 7, ///< Manifest: shard file names
    PZPD_SECT_MHASH   = 8, ///< Manifest: global key/name hash (pzpd_disk_global_hash entries, sorted)
    PZPD_SECT_CMEMBERS = 9, ///< Collection: member table (pzpd_disk_member entries)
    PZPD_SECT_CHEAP    = 10,///< Collection: member paths and aliases
    PZPD_SECT_CREMAP   = 11,///< Collection: per member, PZPD_MAX_STREAMS bytes mapping merged stream -> member stream (0xFF = none)
    PZPD_SECT_TABLE    = 12,///< One table: pzpd_disk_table_head, columns, row index, rows, string heap
    PZPD_SECT_GROUPS   = 13,///< Video groups of the shard (pzpd_disk_group entries, by first record)
    PZPD_SECT_WORDS    = 14,///< One word index of a shard (spec §4.10): pzpd_disk_words_head, sub-indexes
    PZPD_SECT_MWORDS   = 15 ///< Manifest: merged vocabularies of one word index (pzpd_disk_words_head, pzpd_disk_msubindex)
};

/** @brief Hash entry kind: the entry indexes a record key. */
#define PZPD_KIND_KEY  0
/** @brief Hash entry kind: the entry indexes a blob name. */
#define PZPD_KIND_NAME 1
/** @brief Hash entry kind: the entry indexes a video group name (local_ordinal = the group's first record). */
#define PZPD_KIND_GROUP 2
/** @brief Writer duplicate-check kind for group ids (a group's records must be consecutive). */
#define PZPD_KIND_GROUPID 3

/** @brief Blob table rel_offset value meaning "this record has no blob in this stream". */
#define PZPD_MISSING 0xFFFFFFFFu

//-----------------------------------------------------------------------------------------------
// On-disk structures (packed, little-endian, no pointers)
//-----------------------------------------------------------------------------------------------
#pragma pack(push,1)

/** @brief One stream slot of a superblock / manifest stream table. */
struct pzpd_disk_stream
{
    char    name[23]; ///< Stream name, NUL-padded (no NUL when exactly 23 bytes long)
    uint8_t flags;    ///< Reserved, 0
};

/** @brief One slot of the table directory (unused slots are all zero). */
struct pzpd_disk_table
{
    char     name[23];       ///< Table name, NUL-padded
    uint8_t  flags;          ///< Global / bulk flags
    uint64_t section_offset; ///< Offset of the table section
    uint64_t section_bytes;  ///< Bytes of the table section
    uint32_t row_stride;     ///< Bytes per row
    uint32_t pad;            ///< Reserved, 0
};

/** @brief One slot of the word index directory (64 bytes; spec revision 12). Unused slots are all zero. */
struct pzpd_disk_words
{
    char     table[24];       ///< Indexed table, NUL-terminated
    char     column[24];      ///< Indexed `str` column, NUL-terminated
    uint64_t section_offset;  ///< Section data (PZPD_SECT_WORDS in shards, PZPD_SECT_MWORDS in manifests)
    uint64_t section_bytes;   ///< Section bytes
};

/** @brief Shard superblock: a 4 KiB slot at offset 0, copied to the last 4 KiB of the file.
 *
 *  The *_offset fields point at the **data** of each index section; the section's
 *  pzpd_disk_section header sits in the 32 bytes before it. sb_checksum covers every byte
 *  before it; index_checksum covers the data of the record, blob, hash, heap and meta
 *  sections and then of each table section, in that order. */
struct pzpd_disk_superblock
{
    char     magic[8];          ///< PZPD_MAGIC_SHARD
    uint32_t version;           ///< PZPD_FORMAT_VERSION
    uint32_t flags;             ///< bit0: has groups
    uint8_t  archive_uuid[16];  ///< Same in every shard of an archive and in its manifest
    uint64_t generation;        ///< Incremented when the shard is rewritten (stream / table edits)
    uint32_t shard_index;       ///< Position of this shard in its archive
    uint32_t shard_count;       ///< Shards in the archive; 0 when not known (patched by pzpd_writer_finish())
    uint64_t first_ordinal;     ///< Archive ordinal of this shard's first record
    uint64_t record_count;      ///< Records in this shard
    uint64_t total_records;     ///< Records in the whole archive; 0 when not known
    uint32_t stream_count;      ///< Streams declared, 1..PZPD_MAX_STREAMS
    uint32_t align;             ///< Record alignment in bytes (64 or 4096)
    struct pzpd_disk_stream streams[PZPD_MAX_STREAMS]; ///< Stream names (768 B)
    struct pzpd_disk_table  tables[PZPD_MAX_TABLES];   ///< Table directory (768 B)
    uint64_t group_count;       ///< Video groups in this shard (phase 4)
    uint64_t records_offset;    ///< First record (always PZPD_BLOCK)
    uint64_t records_bytes;     ///< Bytes from records_offset to the end of the last record
    uint64_t rtab_offset;       ///< Record table data
    uint64_t btab_offset;       ///< Blob table data
    uint64_t hash_offset;       ///< Hash table data
    uint64_t hash_count;        ///< Hash entries (records + present blobs)
    uint64_t heap_offset;       ///< String heap data
    uint64_t heap_bytes;        ///< String heap size
    uint64_t groups_offset;     ///< Group table data, 0 if none (phase 4)
    uint64_t meta_offset;       ///< Metadata JSON data
    uint64_t meta_bytes;        ///< Metadata JSON size
    uint64_t file_bytes;        ///< Size of the complete shard file (the backup superblock ends here)
    uint64_t index_checksum;    ///< XXH64 of the record, blob, hash, heap, meta and table section data
    uint64_t sb_checksum;       ///< XXH64 of every byte of this structure before this field
    // Revision 12 extension, after sb_checksum: superblocks written before it (zeros here) stay valid, and
    // older readers ignore it. It is sealed on its own (pzpd_seal_superblock()).
    struct pzpd_disk_words words[PZPD_MAX_WORD_INDEXES]; ///< Word index directory (256 B)
    uint64_t words_checksum;    ///< XXH64 of the word index section data, in directory order
    uint64_t ext_checksum;      ///< XXH64 of the extension bytes before this field; 0 when the extension is all zero
};

/** @brief Header in front of every index section (32 bytes). */
struct pzpd_disk_section
{
    char     magic[8];  ///< PZPD_MAGIC_SECTION
    uint32_t version;   ///< PZPD_FORMAT_VERSION
    uint32_t kind;      ///< enum pzpd_section_kind
    uint64_t bytes;     ///< Bytes of data following this header
    uint64_t checksum;  ///< XXH64 of that data
};

/** @brief Record table entry (32 bytes), one per record, in ordinal order. */
struct pzpd_disk_record
{
    uint64_t offset;     ///< Shard offset of the record header
    uint32_t bytes;      ///< Record bytes including header and padding
    uint32_t key_offset; ///< Key position in the string heap
    uint16_t key_len;    ///< Key length
    uint16_t present;    ///< Reserved (presence is in the blob table), 0
    uint32_t group;      ///< Video group, PZPD_NO_GROUP if none
    uint32_t frame;      ///< Frame index in the group
    uint32_t checksum;   ///< XXH32 of the record header (with its checksum field zeroed)
};

/** @brief Blob table entry (32 bytes), row-major [record][stream]. */
struct pzpd_disk_blob
{
    uint32_t rel_offset;  ///< Payload offset from the record start, PZPD_MISSING if the stream is missing
    uint32_t size;        ///< Payload bytes
    uint32_t name_offset; ///< Name position in the string heap
    uint16_t name_len;    ///< Name length
    uint8_t  meta_flags;  ///< PZPD_META_* bits
    uint8_t  bits;        ///< Bits per channel
    uint32_t format;      ///< FourCC
    uint32_t width;       ///< Width / lines / first NPY dimension
    uint32_t height;      ///< Height / second NPY dimension
    uint16_t channels;    ///< Channels / third NPY dimension
    uint16_t frames;      ///< Frames of a PZP container, 1 for stills
};

/** @brief Shard-local hash entry (16 bytes), sorted by (hash, kind, ordinal, stream). */
struct pzpd_disk_hash
{
    uint64_t hash;          ///< XXH64 (seed 0) of the key or name bytes
    uint32_t local_ordinal; ///< Record within the shard
    uint8_t  stream;        ///< Stream of the blob, 0xFF for a record key
    uint8_t  kind;          ///< PZPD_KIND_KEY or PZPD_KIND_NAME
    uint16_t pad;           ///< 0
};

/** @brief Record header, at the start of every record (40 bytes), followed by blob_count
 *  pzpd_disk_record_blob descriptors, the key, the blob names (in descriptor order),
 *  and zero padding up to header_bytes. Lets `salvage` rebuild everything without the index. */
struct pzpd_disk_record_header
{
    char     magic[8];     ///< PZPD_MAGIC_RECORD
    uint32_t version;      ///< PZPD_FORMAT_VERSION
    uint32_t header_bytes; ///< Bytes of header + descriptors + key + names + padding (multiple of 64)
    uint32_t record_bytes; ///< Bytes of the whole record including trailing alignment padding
    uint32_t table_bytes;  ///< Bytes of copied non-bulk table rows (pzpd_disk_row_copy blocks) after the names, 8-aligned
    uint32_t group;        ///< Video group, PZPD_NO_GROUP if none
    uint32_t frame;        ///< Frame index in the group
    uint16_t key_len;      ///< Key length
    uint8_t  blob_count;   ///< Number of descriptors that follow
    uint8_t  flags;        ///< Reserved, 0
    uint32_t checksum;     ///< XXH32 of header_bytes bytes with this field zeroed
};

/** @brief Per-blob descriptor inside a record header (40 bytes). */
struct pzpd_disk_record_blob
{
    uint8_t  stream;      ///< Stream id
    uint8_t  meta_flags;  ///< PZPD_META_* bits
    uint8_t  bits;        ///< Bits per channel
    uint8_t  reserved;    ///< 0
    uint32_t format;      ///< FourCC
    uint32_t rel_offset;  ///< Payload offset from the record start
    uint32_t size;        ///< Payload bytes
    uint32_t width;       ///< As pzpd_disk_blob::width
    uint32_t height;      ///< As pzpd_disk_blob::height
    uint16_t channels;    ///< As pzpd_disk_blob::channels
    uint16_t frames;      ///< As pzpd_disk_blob::frames
    uint16_t name_len;    ///< Name length
    uint16_t reserved2;   ///< 0
    uint32_t xxh32;       ///< XXH32 of the payload
    uint32_t reserved3;   ///< 0
};

/** @brief Manifest header: a 4 KiB slot at offset 0 of `<name>.pzpd`. */
struct pzpd_disk_manifest
{
    char     magic[8];          ///< PZPD_MAGIC_MANIFEST
    uint32_t version;           ///< PZPD_FORMAT_VERSION
    uint32_t flags;             ///< Reserved, 0
    uint8_t  archive_uuid[16];  ///< Same as in every shard
    uint64_t total_records;     ///< Records in the archive
    uint32_t shard_count;       ///< Shards
    uint32_t stream_count;      ///< Streams
    struct pzpd_disk_stream streams[PZPD_MAX_STREAMS]; ///< Stream names
    struct pzpd_disk_table  tables[PZPD_MAX_TABLES];   ///< Table directory (phase 1c)
    uint64_t shards_offset;     ///< Shard table data (pzpd_disk_manifest_shard entries)
    uint64_t names_offset;      ///< Shard file names data
    uint64_t names_bytes;       ///< Shard file names size
    uint64_t hash_offset;       ///< Global hash data (pzpd_disk_global_hash entries)
    uint64_t hash_count;        ///< Global hash entries
    uint64_t groups_offset;     ///< Reserved, 0: there is no manifest group table (group names are in the global hash; spec revision 10)
    uint64_t group_count;       ///< Reserved, 0
    uint64_t file_bytes;        ///< Manifest file size
    uint64_t index_checksum;    ///< XXH64 of the shard table, names, hash and table section data
    uint64_t sb_checksum;       ///< XXH64 of every byte of this structure before this field
    // Revision 12 extension, as in the shard superblock
    struct pzpd_disk_words words[PZPD_MAX_WORD_INDEXES]; ///< Merged vocabulary sections (PZPD_SECT_MWORDS)
    uint64_t words_checksum;    ///< XXH64 of those sections' data, in directory order
    uint64_t ext_checksum;      ///< XXH64 of the extension bytes before this field; 0 when the extension is all zero
};

/** @brief Manifest shard table entry (48 bytes). */
struct pzpd_disk_manifest_shard
{
    uint64_t first_ordinal;  ///< Ordinal of the shard's first record
    uint64_t record_count;   ///< Records in the shard
    uint64_t file_bytes;     ///< Shard file size
    uint64_t generation;     ///< Shard generation the manifest was written for
    uint64_t index_checksum; ///< Shard index checksum the manifest was written for
    uint32_t name_offset;    ///< File name position in the names section (relative to the manifest's directory)
    uint32_t name_len;       ///< File name length
};

/** @brief Header of a table section (96 bytes). Offsets are relative to the start of the section data;
 *  each part is 8-byte aligned. A record table has records + 1 u32 row starts (CSR); a global table
 *  has no index (index_offset 0, records 0). Manifests store record tables schema-only (rows 0). */
struct pzpd_disk_table_head
{
    char     name[24];     ///< Table name, NUL-terminated
    uint32_t flags;        ///< PZPD_TABLE_GLOBAL, PZPD_TABLE_BULK
    uint32_t ncols;        ///< Columns that follow (pzpd_disk_column)
    uint32_t row_stride;   ///< Bytes per row
    uint32_t pad;          ///< 0
    uint64_t records;      ///< Records covered by the row index
    uint64_t rows;         ///< Rows stored
    uint64_t index_offset; ///< Row index (records + 1 × u32), 0 for global tables
    uint64_t rows_offset;  ///< Rows (rows × row_stride bytes)
    uint64_t heap_offset;  ///< String heap
    uint64_t heap_bytes;   ///< String heap size
    uint64_t reserved;     ///< 0
};

/** @brief One column of a table section (32 bytes). */
struct pzpd_disk_column
{
    char     name[24];     ///< Column name, NUL-terminated
    uint8_t  type;         ///< enum pzpd_type
    uint8_t  pad;          ///< 0
    uint16_t count;        ///< Array length (1 for scalars)
    uint32_t offset;       ///< Byte offset in the row
};

/** @brief Copy of one table's rows for one record inside its record header (16-byte head, then
 *  rows with `str` offsets relative to the strings that follow them, then the strings; padded to 8).
 *  Not written for PZPD_TABLE_BULK tables. Lets `salvage` recover annotations without the index. */
struct pzpd_disk_row_copy
{
    uint32_t table;        ///< Table id
    uint32_t rows;         ///< Rows
    uint32_t rows_bytes;   ///< rows × row_stride
    uint32_t str_bytes;    ///< Bytes of strings after the rows
};

/** @brief One video group of a shard (24 bytes). Entries are in record order; a group never spans shards. */
struct pzpd_disk_group
{
    uint32_t group_id;     ///< Group id (as in the record table)
    uint32_t first_local;  ///< First record of the group in the shard
    uint32_t frame_count;  ///< Records in the group
    uint32_t name_offset;  ///< Name in the string heap
    uint32_t name_len;     ///< Name length (0 = unnamed)
    uint32_t pad;          ///< 0
};

/** @brief Head of a word index section (64 bytes), shard (kind 14) or manifest (kind 15). Offsets are
 *  relative to the section data; each part is 8-byte aligned. Sub-index 0 is the merged one, 1..k one per
 *  source value, sorted by its bytes. */
struct pzpd_disk_words_head
{
    uint32_t tokenizer;         ///< PZPD_TOKENIZER_V1
    uint32_t flags;             ///< 0
    uint32_t subindex_count;    ///< Sub-index heads that follow
    uint32_t pad;               ///< 0
    char     source_column[24]; ///< Column naming each row's source, "" for none
    uint64_t names_offset;      ///< Source values (bytes, no NUL)
    uint64_t names_bytes;       ///< Their size
    uint64_t reserved;          ///< 0
};

/** @brief One sub-index of a shard word index section (96 bytes). */
struct pzpd_disk_subindex
{
    uint32_t source_offset;     ///< Source value in the names (source_len 0: the merged sub-index)
    uint32_t source_len;        ///< Its length
    uint64_t records;           ///< Records of the shard (forward index covers them all)
    uint64_t words;             ///< Vocabulary size
    uint64_t postings;          ///< Record-word pairs (= forward entries)
    uint64_t vocab_offset;      ///< words × pzpd_disk_word, sorted by word bytes
    uint64_t post_index_offset; ///< (words + 1) × u32
    uint64_t post_offset;       ///< postings × u32 shard-local ordinals, ascending per word
    uint64_t fwd_index_offset;  ///< (records + 1) × u32
    uint64_t fwd_offset;        ///< postings × u32 word ids, ascending per record
    uint64_t heap_offset;       ///< Word bytes
    uint64_t heap_bytes;        ///< Their size
    uint64_t reserved;          ///< 0
};

/** @brief One word of a shard vocabulary (16 bytes). */
struct pzpd_disk_word
{
    uint32_t heap_offset;       ///< Word bytes in the sub-index heap
    uint16_t len;               ///< Word length
    uint16_t pad;               ///< 0
    uint32_t records;           ///< Records of the shard containing it
    uint32_t count;             ///< Occurrences in the shard
};

/** @brief One sub-index of a manifest word section (48 bytes): the merged vocabulary of every shard. */
struct pzpd_disk_msubindex
{
    uint32_t source_offset;     ///< Source value in the names (source_len 0: the merged sub-index)
    uint32_t source_len;        ///< Its length
    uint64_t words;             ///< Vocabulary size
    uint64_t vocab_offset;      ///< words × pzpd_disk_mword, sorted by word bytes
    uint64_t heap_offset;       ///< Word bytes
    uint64_t heap_bytes;        ///< Their size
    uint64_t reserved;          ///< 0
};

/** @brief One word of a manifest vocabulary (24 bytes): totals over the archive. */
struct pzpd_disk_mword
{
    uint32_t heap_offset;       ///< Word bytes in the sub-index heap
    uint16_t len;               ///< Word length
    uint16_t pad;               ///< 0
    uint64_t records;           ///< Records containing it
    uint64_t count;             ///< Occurrences
};

/** @brief Manifest global hash entry (24 bytes), sorted by (hash, kind, ordinal, stream). */
struct pzpd_disk_global_hash
{
    uint64_t hash;    ///< XXH64 of the key or name
    uint64_t ordinal; ///< Archive ordinal
    uint8_t  stream;  ///< Stream of the blob, 0xFF for a record key
    uint8_t  kind;    ///< PZPD_KIND_KEY or PZPD_KIND_NAME
    uint8_t  pad[6];  ///< 0
};

#pragma pack(pop)

_Static_assert(sizeof(struct pzpd_disk_stream)          == 24,  "stream slot");
_Static_assert(sizeof(struct pzpd_disk_table)           == 48,  "table slot");
_Static_assert(sizeof(struct pzpd_disk_superblock)      <= PZPD_BLOCK, "superblock fits its slot");
_Static_assert(sizeof(struct pzpd_disk_section)         == 32,  "section header");
_Static_assert(sizeof(struct pzpd_disk_record)          == 32,  "record entry");
_Static_assert(sizeof(struct pzpd_disk_blob)            == 32,  "blob entry");
_Static_assert(sizeof(struct pzpd_disk_hash)            == 16,  "hash entry");
_Static_assert(sizeof(struct pzpd_disk_record_header)   == 40,  "record header");
_Static_assert(sizeof(struct pzpd_disk_record_blob)     == 40,  "record blob descriptor");
_Static_assert(sizeof(struct pzpd_disk_manifest)        <= PZPD_BLOCK, "manifest header fits its slot");
_Static_assert(sizeof(struct pzpd_disk_manifest_shard)  == 48,  "manifest shard entry");
_Static_assert(sizeof(struct pzpd_disk_global_hash)     == 24,  "global hash entry");
_Static_assert(sizeof(struct pzpd_disk_table_head)      == 96,  "table section header");
_Static_assert(sizeof(struct pzpd_disk_column)          == 32,  "table column");
_Static_assert(sizeof(struct pzpd_disk_row_copy)        == 16,  "record-header row copy");
_Static_assert(sizeof(struct pzpd_disk_group)           == 24,  "group entry");
_Static_assert(sizeof(struct pzpd_disk_words)           == 64,  "word index slot");
_Static_assert(sizeof(struct pzpd_disk_words_head)      == 64,  "word section head");
_Static_assert(sizeof(struct pzpd_disk_subindex)        == 96,  "word sub-index head");
_Static_assert(sizeof(struct pzpd_disk_word)            == 16,  "shard vocabulary entry");
_Static_assert(sizeof(struct pzpd_disk_msubindex)       == 48,  "manifest sub-index head");
_Static_assert(sizeof(struct pzpd_disk_mword)           == 24,  "manifest vocabulary entry");
_Static_assert(offsetof(struct pzpd_disk_superblock, sb_checksum) == 1728, "the revision-12 extension must not move sb_checksum");

//-----------------------------------------------------------------------------------------------
// Errors (thread-local)
//-----------------------------------------------------------------------------------------------

static __thread char pzpd_errorText[512]; ///< Message of the last error on this thread, "" after success
static __thread int  pzpd_errorCode;      ///< enum pzpd_error of the last error on this thread

/** @brief Record an error for pzpd_last_error() on this thread.
 *  @param code enum pzpd_error value.
 *  @param fmt  printf-style message. */
static void pzpd_set_error(int code, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(pzpd_errorText, sizeof(pzpd_errorText), fmt, args);
    va_end(args);
    pzpd_errorCode = code;
    #if PZPDIR_DEBUG
     fprintf(stderr, PZPD_RED "pzpdir: %s" PZPD_NORMAL "\n", pzpd_errorText);
    #endif
}

/** @brief Clear the error state of this thread (called at the start of public functions). */
static void pzpd_clear_error(void)
{
    pzpd_errorText[0] = 0;
    pzpd_errorCode    = PZPD_OK;
}

/** @brief An error kept across cleanup calls that clear or overwrite it (pzpd_error_save() / pzpd_error_restore()). */
struct pzpd_saved_error
{
    int  code;       ///< enum pzpd_error
    char text[512];  ///< Message
};

/** @brief Save this thread's error. */
static void pzpd_error_save(struct pzpd_saved_error *e)
{
    e->code = pzpd_errorCode;
    memcpy(e->text, pzpd_errorText, sizeof(e->text));
}

/** @brief Make a saved error this thread's error again. */
static void pzpd_error_restore(const struct pzpd_saved_error *e)
{
    memcpy(pzpd_errorText, e->text, sizeof(pzpd_errorText));
    pzpd_errorCode = e->code;
}

/** @brief Put printf-formatted context in front of this thread's error ("context: message").
 *  @param code New enum pzpd_error, or PZPD_OK to keep the current one. */
static void pzpd_error_wrap(int code, const char *fmt, ...)
{
    struct pzpd_saved_error e;
    pzpd_error_save(&e);
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(pzpd_errorText, sizeof(pzpd_errorText), fmt, args);
    va_end(args);
    size_t o = (n < 0) ? 0 : ((size_t) n < sizeof(pzpd_errorText)) ? (size_t) n : sizeof(pzpd_errorText) - 1;
    const char *sep = ": ";
    for (const char *q = sep; (*q != 0) && (o + 1 < sizeof(pzpd_errorText)); q++) { pzpd_errorText[o++] = *q; }
    for (const char *q = e.text; (*q != 0) && (o + 1 < sizeof(pzpd_errorText)); q++) { pzpd_errorText[o++] = *q; }   // cut to fit, like snprintf
    pzpd_errorText[o] = 0;
    pzpd_errorCode = (code != PZPD_OK) ? code : e.code;
    #if PZPDIR_DEBUG
     fprintf(stderr, PZPD_RED "pzpdir: %s" PZPD_NORMAL "\n", pzpd_errorText);
    #endif
}

const char *pzpd_last_error(void)
{
    return pzpd_errorText;
}

int pzpd_last_error_code(void)
{
    return pzpd_errorCode;
}

//-----------------------------------------------------------------------------------------------
// Small helpers
//-----------------------------------------------------------------------------------------------

/** @brief Check that [off, off+bytes) lies inside a file of the given size, without overflow. */
static inline int pzpd_in_file(uint64_t off, uint64_t bytes, uint64_t file)
{
    return (off <= file) && (bytes <= file - off);
}

/** @brief Round v up to a multiple of a (a is a power of two). */
static inline uint64_t pzpd_align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/** @brief Growable byte buffer used by the writer for index sections and the heap. */
struct pzpd_buf
{
    unsigned char *data; ///< Contents
    size_t         len;  ///< Bytes used
    size_t         cap;  ///< Bytes allocated
};

/** @brief Append bytes to a buffer.
 *  @return 1 on success, 0 on allocation failure. */
static int pzpd_buf_append(struct pzpd_buf *b, const void *src, size_t n)
{
    if (b->len + n > b->cap)
    {
        size_t ncap = (b->cap == 0) ? 4096 : b->cap;
        while (ncap < b->len + n) { ncap *= 2; }
        unsigned char *p = (unsigned char *) realloc(b->data, ncap);
        if (p == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory growing a buffer to %zu bytes", ncap); return 0; }
        b->data = p;
        b->cap  = ncap;
    }
    if (n > 0) { memcpy(b->data + b->len, src, n); }
    b->len += n;
    return 1;
}

/** @brief Free a buffer's memory and reset it. */
static void pzpd_buf_free(struct pzpd_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

/** @brief pwrite() the whole range, retrying on short writes and EINTR.
 *  @return 1 on success, 0 on failure (error set). */
static int pzpd_pwrite_all(int fd, const void *src, size_t n, uint64_t off)
{
    const unsigned char *p = (const unsigned char *) src;
    while (n > 0)
    {
        ssize_t w = pwrite(fd, p, n, (off_t) off);
        if (w < 0)
        {
            if (errno == EINTR) { continue; }
            pzpd_set_error(PZPD_E_IO, "write failed at offset %llu: %s", (unsigned long long) off, strerror(errno));
            return 0;
        }
        p   += w;
        n   -= (size_t) w;
        off += (uint64_t) w;
    }
    return 1;
}

/** @brief pread() the whole range, retrying on short reads and EINTR.
 *  @return 1 on success, 0 on failure or premature end of file (error set). */
static int pzpd_pread_all(int fd, void *dst, size_t n, uint64_t off)
{
    unsigned char *p = (unsigned char *) dst;
    while (n > 0)
    {
        ssize_t r = pread(fd, p, n, (off_t) off);
        if (r < 0)
        {
            if (errno == EINTR) { continue; }
            pzpd_set_error(PZPD_E_IO, "read failed at offset %llu: %s", (unsigned long long) off, strerror(errno));
            return 0;
        }
        if (r == 0)
        {
            pzpd_set_error(PZPD_E_FORMAT, "unexpected end of file at offset %llu", (unsigned long long) off);
            return 0;
        }
        p   += r;
        n   -= (size_t) r;
        off += (uint64_t) r;
    }
    return 1;
}

/** @brief fsync() the directory containing path, so renames inside it are durable. */
static void pzpd_fsync_dir_of(const char *path)
{
    char dir[4096];
    const char *slash = strrchr(path, '/');
    if (slash == NULL) { snprintf(dir, sizeof(dir), "."); }
    else if (slash == path) { snprintf(dir, sizeof(dir), "/"); }
    else { snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path); }
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) { fsync(fd); close(fd); }
}

/** @brief Copy a stream name into a 23-byte on-disk slot (NUL-padded). */
static void pzpd_put_slot_name(char slot[23], const char *name)
{
    memset(slot, 0, 23);
    size_t n = strlen(name);
    if (n > 23) { n = 23; }
    memcpy(slot, name, n);
}

/** @brief Copy a 23-byte on-disk slot into a NUL-terminated 24-byte buffer. */
static void pzpd_get_slot_name(char out[24], const char slot[23])
{
    memcpy(out, slot, 23);
    out[23] = 0;
}

/** @brief Validate a key or name: 1..PZPD_MAX_NAME bytes without NUL.
 *  @return 1 if valid, 0 otherwise (error set). */
static int pzpd_check_name(const char *what, const char *s, size_t len)
{
    if ( (s == NULL) || (len == 0) ) { pzpd_set_error(PZPD_E_ARG, "%s is empty", what); return 0; }
    if (len > PZPD_MAX_NAME) { pzpd_set_error(PZPD_E_ARG, "%s is %zu bytes, the limit is %u", what, len, PZPD_MAX_NAME); return 0; }
    if (memchr(s, 0, len) != NULL) { pzpd_set_error(PZPD_E_ARG, "%s contains a NUL byte", what); return 0; }
    return 1;
}

/** @brief Validate a stream name: 1..PZPD_MAX_STREAM_NAME bytes without `"`, `\` or control characters (the shard
 *  metadata JSON lists stream names unescaped, and recovery reads them back from it).
 *  @return 1 if valid, 0 otherwise (error set). */
static int pzpd_check_stream_name(const char *nm)
{
    size_t n = (nm != NULL) ? strlen(nm) : 0;
    if ( (n == 0) || (n > PZPD_MAX_STREAM_NAME) ) { pzpd_set_error(PZPD_E_ARG, "stream names must be 1..%d bytes", PZPD_MAX_STREAM_NAME); return 0; }
    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char) nm[i];
        if ( (c < 0x20) || (c == 0x7F) || (c == '"') || (c == '\\') ) { pzpd_set_error(PZPD_E_ARG, "stream name \"%s\": no quotes, backslashes or control characters", nm); return 0; }
    }
    return 1;
}

const char *pzpd_format_name(uint32_t fourcc, char out[5])
{
    for (int i = 0; i < 4; i++)
    {
        unsigned char c = (unsigned char)((fourcc >> (8 * i)) & 0xFF);
        out[i] = ( (c >= 32) && (c < 127) ) ? (char) c : '?';
    }
    out[4] = 0;
    return out;
}

//-----------------------------------------------------------------------------------------------
// Format detection (header-only probes)
//-----------------------------------------------------------------------------------------------

/** @brief Read a big-endian u16. */
static inline uint32_t pzpd_be16(const unsigned char *p) { return ((uint32_t)p[0] << 8) | p[1]; }
/** @brief Read a big-endian u32. */
static inline uint32_t pzpd_be32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
/** @brief Read a little-endian u32. */
static inline uint32_t pzpd_le32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/** @brief Clamp a value to u16 (saturating). */
static inline uint16_t pzpd_sat16(uint64_t v) { return (v > 65535) ? 65535 : (uint16_t) v; }

/** @brief JPEG: walk the marker segments until a start-of-frame marker. */
static int pzpd_probe_jpeg(const unsigned char *d, size_t n, pzpd_blob_meta *m)
{
    if ( (n < 4) || (d[0] != 0xFF) || (d[1] != 0xD8) || (d[2] != 0xFF) ) { return 0; }
    m->format = PZPD_FORMAT_JPEG;
    size_t i = 2;
    while (i + 4 <= n)
    {
        if (d[i] != 0xFF) { return 1; }                  // not a marker: format known, metadata not found
        unsigned int marker = d[i + 1];
        if (marker == 0xFF) { i++; continue; }           // fill byte
        if ( (marker == 0xD8) || (marker == 0x01) || ((marker >= 0xD0) && (marker <= 0xD7)) ) { i += 2; continue; } // standalone markers
        if ( (marker == 0xD9) || (marker == 0xDA) ) { return 1; } // end of image / start of scan before any frame header
        uint32_t len = pzpd_be16(d + i + 2);
        int isSOF = (marker >= 0xC0) && (marker <= 0xCF) && (marker != 0xC4) && (marker != 0xC8) && (marker != 0xCC);
        if (isSOF)
        {
            if (i + 10 > n) { return 1; }
            m->bits       = d[i + 4];
            m->height     = pzpd_be16(d + i + 5);
            m->width      = pzpd_be16(d + i + 7);
            m->channels   = d[i + 9];
            m->frames     = 1;
            m->meta_flags |= PZPD_META_VALID;
            return 1;
        }
        if (len < 2) { return 1; }
        i += 2 + len;
    }
    return 1;
}

/** @brief PNG: signature + IHDR chunk. */
static int pzpd_probe_png(const unsigned char *d, size_t n, pzpd_blob_meta *m)
{
    static const unsigned char sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    if ( (n < 8) || (memcmp(d, sig, 8) != 0) ) { return 0; }
    m->format = PZPD_FORMAT_PNG;
    if ( (n < 8 + 8 + 13) || (memcmp(d + 12, "IHDR", 4) != 0) ) { return 1; }
    m->width  = pzpd_be32(d + 16);
    m->height = pzpd_be32(d + 20);
    m->bits   = d[24];
    switch (d[25])  // colour type
    {
        case 0: m->channels = 1; break;
        case 2: m->channels = 3; break;
        case 3: m->channels = 1; m->meta_flags |= PZPD_META_INDEXED; break;
        case 4: m->channels = 2; break;
        case 6: m->channels = 4; break;
        default: return 1;
    }
    m->frames     = 1;
    m->meta_flags |= PZPD_META_VALID;
    return 1;
}

/** @brief Read the ASCII header tokens of a PNM / PFM file (skipping '#' comments).
 *  @param count Number of unsigned integers to read after the 2-byte magic (or signed float for PFM).
 *  @param vals  Output values as doubles.
 *  @return Offset after the last token, 0 on failure. */
static size_t pzpd_pnm_tokens(const unsigned char *d, size_t n, int count, double *vals)
{
    size_t i = 2;
    for (int t = 0; t < count; t++)
    {
        while (i < n)
        {
            if (d[i] == '#') { while ( (i < n) && (d[i] != '\n') ) { i++; } }
            else if ( (d[i] == ' ') || (d[i] == '\t') || (d[i] == '\r') || (d[i] == '\n') ) { i++; }
            else { break; }
        }
        if (i >= n) { return 0; }
        char tmp[64];
        size_t k = 0;
        while ( (i < n) && (k < sizeof(tmp) - 1) && (d[i] != ' ') && (d[i] != '\t') && (d[i] != '\r') && (d[i] != '\n') && (d[i] != '#') )
            { tmp[k++] = (char) d[i++]; }
        tmp[k] = 0;
        char *end = NULL;
        vals[t] = strtod(tmp, &end);
        if ( (k == 0) || (end == NULL) || (*end != 0) ) { return 0; }
    }
    return i;
}

/** @brief PBM / PGM / PPM (P1..P6). */
static int pzpd_probe_pnm(const unsigned char *d, size_t n, pzpd_blob_meta *m)
{
    if ( (n < 3) || (d[0] != 'P') || (d[1] < '1') || (d[1] > '6') ) { return 0; }
    if ( (d[2] != ' ') && (d[2] != '\t') && (d[2] != '\r') && (d[2] != '\n') && (d[2] != '#') ) { return 0; }
    int bitmap = (d[1] == '1') || (d[1] == '4');
    double v[3] = {0,0,0};
    if (pzpd_pnm_tokens(d, n, bitmap ? 2 : 3, v) == 0) { return 0; }
    if ( !((v[0] > 0) && (v[0] <= 4294967295.0)) || !((v[1] > 0) && (v[1] <= 4294967295.0)) ) { return 0; }   // also refuses nan, inf
    m->format   = PZPD_FORMAT_PNM;
    m->width    = (uint32_t) v[0];
    m->height   = (uint32_t) v[1];
    m->channels = ( (d[1] == '3') || (d[1] == '6') ) ? 3 : 1;
    m->bits     = bitmap ? 1 : ( (v[2] > 255) ? 16 : 8 );
    m->frames   = 1;
    m->meta_flags |= PZPD_META_VALID;
    return 1;
}

/** @brief Portable float map (PF = 3 channels, Pf = 1 channel). */
static int pzpd_probe_pfm(const unsigned char *d, size_t n, pzpd_blob_meta *m)
{
    if ( (n < 3) || (d[0] != 'P') || ( (d[1] != 'F') && (d[1] != 'f') ) ) { return 0; }
    if ( (d[2] != ' ') && (d[2] != '\t') && (d[2] != '\r') && (d[2] != '\n') ) { return 0; }
    double v[3] = {0,0,0};
    if (pzpd_pnm_tokens(d, n, 3, v) == 0) { return 0; }
    if ( !((v[0] > 0) && (v[0] <= 4294967295.0)) || !((v[1] > 0) && (v[1] <= 4294967295.0)) || (v[2] == 0) ) { return 0; }   // also refuses nan, inf
    m->format   = PZPD_FORMAT_PFM;
    m->width    = (uint32_t) v[0];
    m->height   = (uint32_t) v[1];
    m->channels = (d[1] == 'F') ? 3 : 1;
    m->bits     = 32;
    m->frames   = 1;
    m->meta_flags |= PZPD_META_VALID | PZPD_META_FLOAT;
    if (v[2] > 0) { m->meta_flags |= PZPD_META_BIG_ENDIAN; }
    return 1;
}

/** @brief NumPy .npy: magic, header dict with 'descr' and 'shape'. */
static int pzpd_probe_npy(const unsigned char *d, size_t n, pzpd_blob_meta *m)
{
    if ( (n < 10) || (d[0] != 0x93) || (memcmp(d + 1, "NUMPY", 5) != 0) ) { return 0; }
    m->format = PZPD_FORMAT_NPY;
    size_t hlen, hstart;
    if (d[6] == 1) { hlen = (size_t)d[8] | ((size_t)d[9] << 8); hstart = 10; }   // v1: little-endian u16 header length
    else
    {
        if (n < 12) { return 1; }
        hlen = pzpd_le32(d + 8);
        hstart = 12;
    }
    if (hstart + hlen > n) { return 1; }
    char hdr[4096];
    size_t k = (hlen < sizeof(hdr) - 1) ? hlen : sizeof(hdr) - 1;
    memcpy(hdr, d + hstart, k);
    hdr[k] = 0;

    char *descr = strstr(hdr, "'descr'");
    char *shape = strstr(hdr, "'shape'");
    if ( (descr == NULL) || (shape == NULL) ) { return 1; }
    char *q = strchr(descr + 7, '\'');                 // opening quote of the dtype string
    if ( (q == NULL) || (q[1] == 0) || (q[2] == 0) ) { return 1; }   // a header cut inside the dtype string
    char order = q[1];                                  // '<', '>', '|' or '='
    char kind  = q[2];                                  // 'f', 'i', 'u', 'b', 'c', ...
    int itemsize = atoi(q + 3);
    if ( (itemsize <= 0) || (itemsize > 16) ) { return 1; }
    m->bits = (uint8_t)(itemsize * 8);
    if ( (kind == 'f') || (kind == 'c') ) { m->meta_flags |= PZPD_META_FLOAT; }
    if ( (order == '>') && (itemsize > 1) ) { m->meta_flags |= PZPD_META_BIG_ENDIAN; }

    char *p = strchr(shape, '(');
    if (p == NULL) { return 1; }
    p++;
    uint64_t dims[3] = {0,0,0};
    int nd = 0;
    while ( (*p != 0) && (*p != ')') )
    {
        while ( (*p == ' ') || (*p == ',') ) { p++; }
        if ( (*p == ')') || (*p == 0) ) { break; }
        char *end = NULL;
        unsigned long long v = strtoull(p, &end, 10);
        if (end == p) { return 1; }
        if (nd < 3) { dims[nd] = v; }
        nd++;
        p = end;
    }
    m->width    = (dims[0] > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t) dims[0];
    m->height   = (dims[1] > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t) dims[1];
    m->channels = pzpd_sat16(dims[2]);
    m->frames   = 0;
    m->meta_flags |= PZPD_META_VALID;
    return 1;
}

/** @brief Decompress the first 40 bytes (the inner PZP frame header) of a size-prefixed
 *  zstd / lz4 PZP frame. Uses zstd / lz4 directly, so it works without pzp.h.
 *  @param frame Start of the frame (the 4-byte size prefix).
 *  @param n     Bytes available from frame.
 *  @param hdr   Receives 10 × u32.
 *  @return 1 if 40 bytes were decoded and the magic matches, 0 otherwise. */
static int pzpd_pzp_inner_header(const unsigned char *frame, size_t n, uint32_t hdr[10])
{
    if (n < 8) { return 0; }
    uint32_t prefix = pzpd_le32(frame);
    uint32_t usize  = prefix & 0x7FFFFFFFu;
    if (usize < 40) { return 0; }
    unsigned char out[40];
    if (prefix & 0x80000000u)
    {
        int cs = (n - 4 > 0x7FFFFFFF) ? 0x7FFFFFFF : (int)(n - 4);
        int got = LZ4_decompress_safe_partial((const char *)(frame + 4), (char *) out, cs, 40, 40);
        if (got < 40) { return 0; }
    }
    else
    {
        ZSTD_DCtx *ctx = ZSTD_createDCtx();
        if (ctx == NULL) { return 0; }
        ZSTD_inBuffer  in  = { frame + 4, n - 4, 0 };
        ZSTD_outBuffer ob  = { out, 40, 0 };
        while ( (ob.pos < 40) && (in.pos < in.size) )
        {
            size_t r = ZSTD_decompressStream(ctx, &ob, &in);
            if (ZSTD_isError(r)) { break; }
            if ( (r == 0) && (ob.pos < 40) ) { break; }   // frame ended early
        }
        ZSTD_freeDCtx(ctx);
        if (ob.pos < 40) { return 0; }
    }
    memcpy(hdr, out, 40);
    // PZP writes convert_header("PZP0") natively: 'P'<<24 | 'Z'<<16 | 'P'<<8 | '0'; frames with a
    // channel group table ( PZP v0.03 ) use "PZP1", with the same 40-byte header
    const uint32_t magic  = ((uint32_t)'P' << 24) | ((uint32_t)'Z' << 16) | ((uint32_t)'P' << 8) | (uint32_t)'0';
    const uint32_t magic1 = ((uint32_t)'P' << 24) | ((uint32_t)'Z' << 16) | ((uint32_t)'P' << 8) | (uint32_t)'1';
    return (hdr[0] == magic) || (hdr[0] == magic1);
}

/** @brief Fill metadata from an inner PZP frame header (bpp_ext is bits per channel). */
static void pzpd_pzp_fill(const uint32_t hdr[10], pzpd_blob_meta *m)
{
    m->bits     = (uint8_t) hdr[1];
    m->channels = pzpd_sat16(hdr[2]);
    m->width    = hdr[3];
    m->height   = hdr[4];
    if ( (m->width > 0) && (m->height > 0) && (m->channels > 0) ) { m->meta_flags |= PZPD_META_VALID; }
}

/** @brief PZP: container ("PZP0" magic) or single frame (size prefix + compressed header). */
static int pzpd_probe_pzp(const unsigned char *d, size_t n, pzpd_blob_meta *m)
{
    if (n < 8) { return 0; }
    const uint32_t magic = ((uint32_t)'P' << 24) | ((uint32_t)'Z' << 16) | ((uint32_t)'P' << 8) | (uint32_t)'0';
    uint32_t hdr[10];
    if (pzpd_le32(d) == magic)
    {
        m->format = PZPD_FORMAT_PZPC;
        if (n < 48 + 16) { return 1; }
        uint32_t frames = pzpd_le32(d + 12);
        uint32_t off0   = pzpd_le32(d + 48);   // first PZPFrameEntry: frame_offset, compressed_size
        uint32_t size0  = pzpd_le32(d + 52);
        if ( (off0 < 48) || ((uint64_t)off0 + size0 > n) ) { return 1; }
        if (pzpd_pzp_inner_header(d + off0, size0, hdr))
        {
            pzpd_pzp_fill(hdr, m);
            m->frames = pzpd_sat16(frames);
        }
        return 1;
    }
    if (pzpd_pzp_inner_header(d, n, hdr))
    {
        m->format = PZPD_FORMAT_PZP;
        pzpd_pzp_fill(hdr, m);
        m->frames = 1;
        return 1;
    }
    return 0;
}

/** @brief Check that the bytes are valid UTF-8 without NUL; count lines.
 *  @return 1 if text, 0 otherwise. */
static int pzpd_probe_text(const unsigned char *d, size_t n, uint64_t *lines)
{
    if (n == 0) { return 0; }
    uint64_t nl = 0;
    size_t i = 0;
    while (i < n)
    {
        unsigned char c = d[i];
        if (c == 0) { return 0; }
        if (c < 0x80) { if (c == '\n') { nl++; } i++; continue; }
        int extra;
        if      ( (c & 0xE0) == 0xC0 ) { extra = 1; if (c < 0xC2) { return 0; } }
        else if ( (c & 0xF0) == 0xE0 ) { extra = 2; }
        else if ( (c & 0xF8) == 0xF0 ) { extra = 3; if (c > 0xF4) { return 0; } }
        else { return 0; }
        if (i + (size_t) extra >= n) { return 0; }                     // truncated sequence
        for (int k = 1; k <= extra; k++) { if ( (d[i + k] & 0xC0) != 0x80 ) { return 0; } }
        i += 1 + (size_t) extra;
    }
    if (d[n - 1] != '\n') { nl++; }
    *lines = nl;
    return 1;
}

/** @brief FourCC implied by a file name's extension (case-insensitive), 0 if unknown. */
static uint32_t pzpd_format_from_extension(const char *name, size_t len)
{
    if (name == NULL) { return 0; }
    size_t dot = len;
    for (size_t i = len; i > 0; i--)
    {
        if (name[i - 1] == '.') { dot = i - 1; break; }
        if (name[i - 1] == '/') { break; }
    }
    if (dot == len) { return 0; }
    char ext[8];
    size_t el = len - dot - 1;
    if ( (el == 0) || (el >= sizeof(ext)) ) { return 0; }
    for (size_t i = 0; i < el; i++)
    {
        char c = name[dot + 1 + i];
        ext[i] = ( (c >= 'A') && (c <= 'Z') ) ? (char)(c - 'A' + 'a') : c;
    }
    ext[el] = 0;
    if (!strcmp(ext,"jpg") || !strcmp(ext,"jpeg")) { return PZPD_FORMAT_JPEG; }
    if (!strcmp(ext,"png"))  { return PZPD_FORMAT_PNG; }
    if (!strcmp(ext,"pzp"))  { return PZPD_FORMAT_PZP; }
    if (!strcmp(ext,"pnm") || !strcmp(ext,"ppm") || !strcmp(ext,"pgm") || !strcmp(ext,"pbm")) { return PZPD_FORMAT_PNM; }
    if (!strcmp(ext,"pfm"))  { return PZPD_FORMAT_PFM; }
    if (!strcmp(ext,"npy"))  { return PZPD_FORMAT_NPY; }
    if (!strcmp(ext,"json")) { return PZPD_FORMAT_JSON; }
    if (!strcmp(ext,"csv"))  { return PZPD_FORMAT_CSV; }
    if (!strcmp(ext,"tsv"))  { return PZPD_FORMAT_TSV; }
    if (!strcmp(ext,"txt"))  { return PZPD_FORMAT_TEXT; }
    return 0;
}

uint32_t pzpd_detect_format(const void *data, size_t size, const char *name, size_t name_len, pzpd_blob_meta *meta_out)
{
    pzpd_blob_meta m;
    memset(&m, 0, sizeof(m));
    const unsigned char *d = (const unsigned char *) data;
    uint32_t byExt = pzpd_format_from_extension(name, name_len);

    if ( (d != NULL) && (size > 0) )
    {
        if ( pzpd_probe_jpeg(d, size, &m) || pzpd_probe_png(d, size, &m) || pzpd_probe_pzp(d, size, &m) ||
             pzpd_probe_npy(d, size, &m)  || pzpd_probe_pfm(d, size, &m) || pzpd_probe_pnm(d, size, &m) )
        {
            if (meta_out != NULL) { *meta_out = m; }
            return m.format;
        }
        uint64_t lines = 0;
        if (pzpd_probe_text(d, size, &lines))
        {
            size_t i = 0;
            if ( (size >= 3) && (d[0] == 0xEF) && (d[1] == 0xBB) && (d[2] == 0xBF) ) { i = 3; } // UTF-8 BOM
            while ( (i < size) && ( (d[i] == ' ') || (d[i] == '\t') || (d[i] == '\r') || (d[i] == '\n') ) ) { i++; }
            if      ( (byExt == PZPD_FORMAT_CSV) || (byExt == PZPD_FORMAT_TSV) ) { m.format = byExt; }
            else if ( (i < size) && ( (d[i] == '{') || (d[i] == '[') ) )    { m.format = PZPD_FORMAT_JSON; }
            else                                                            { m.format = PZPD_FORMAT_TEXT; }
            m.width = (lines > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t) lines;
            m.meta_flags |= PZPD_META_VALID;
            if (meta_out != NULL) { *meta_out = m; }
            return m.format;
        }
    }

    // Content not recognised: the extension names the format, but the metadata stays invalid
    m.format = (byExt != 0) ? byExt : PZPD_FORMAT_RAW;
    if (meta_out != NULL) { *meta_out = m; }
    return m.format;
}

//-----------------------------------------------------------------------------------------------
// Table schemas and CSV
//-----------------------------------------------------------------------------------------------

/** @brief Parsed table schema. Lives at a fixed address (inside a writer / archive), because
 *  `cols` / `pub` point into it; call pzpd_schema_publish() after it is in place. */
struct pzpd_tschema
{
    char        name[24];                        ///< Table name
    unsigned    flags;                           ///< PZPD_TABLE_GLOBAL, PZPD_TABLE_BULK
    unsigned    ncols;                           ///< Columns
    uint32_t    stride;                          ///< Bytes per row
    char        colname[PZPD_MAX_COLUMNS][24];   ///< Column names
    uint8_t     type[PZPD_MAX_COLUMNS];          ///< enum pzpd_type
    uint16_t    count[PZPD_MAX_COLUMNS];         ///< Array lengths
    uint32_t    offset[PZPD_MAX_COLUMNS];        ///< Byte offsets in a row
    int         has_str;                         ///< 1 if a column is `str`
    pzpd_column cols[PZPD_MAX_COLUMNS];          ///< Public column view
    pzpd_schema pub;                             ///< Public schema view
};

/** @brief Type table: name, size and alignment of every enum pzpd_type. */
static const struct { const char *name; uint8_t type; uint8_t size; uint8_t align; } pzpd_types[] =
{
    { "u8",  PZPD_TYPE_U8,  1, 1 }, { "i8",  PZPD_TYPE_I8,  1, 1 },
    { "u16", PZPD_TYPE_U16, 2, 2 }, { "i16", PZPD_TYPE_I16, 2, 2 },
    { "u32", PZPD_TYPE_U32, 4, 4 }, { "i32", PZPD_TYPE_I32, 4, 4 },
    { "u64", PZPD_TYPE_U64, 8, 8 }, { "i64", PZPD_TYPE_I64, 8, 8 },
    { "f32", PZPD_TYPE_F32, 4, 4 }, { "f64", PZPD_TYPE_F64, 8, 8 },
    { "str", PZPD_TYPE_STR, 8, 4 },
};

/** @brief Size in bytes of one element of a type, 0 if unknown. */
static unsigned pzpd_type_size(uint8_t t)
{
    for (size_t i = 0; i < sizeof(pzpd_types) / sizeof(pzpd_types[0]); i++) { if (pzpd_types[i].type == t) { return pzpd_types[i].size; } }
    return 0;
}

/** @brief Alignment of a type, 0 if unknown. */
static unsigned pzpd_type_align(uint8_t t)
{
    for (size_t i = 0; i < sizeof(pzpd_types) / sizeof(pzpd_types[0]); i++) { if (pzpd_types[i].type == t) { return pzpd_types[i].align; } }
    return 0;
}

/** @brief Name of a type ("u16", ...), "?" if unknown. */
static const char *pzpd_type_name(uint8_t t)
{
    for (size_t i = 0; i < sizeof(pzpd_types) / sizeof(pzpd_types[0]); i++) { if (pzpd_types[i].type == t) { return pzpd_types[i].name; } }
    return "?";
}

/** @brief Point the public views of a schema at its own arrays (after it reached its final address). */
static void pzpd_schema_publish(struct pzpd_tschema *sc)
{
    for (unsigned c = 0; c < sc->ncols; c++)
    {
        sc->cols[c].name   = sc->colname[c];
        sc->cols[c].type   = sc->type[c];
        sc->cols[c].count  = sc->count[c];
        sc->cols[c].offset = sc->offset[c];
    }
    sc->pub.name       = sc->name;
    sc->pub.flags      = sc->flags;
    sc->pub.row_stride = sc->stride;
    sc->pub.ncols      = sc->ncols;
    sc->pub.cols       = sc->cols;
}

/** @brief Compute offsets and stride with C struct rules; check sizes.
 *  @return 1 if the layout is valid, 0 otherwise (error set). */
static int pzpd_schema_layout(struct pzpd_tschema *sc)
{
    uint64_t off = 0;
    unsigned maxAlign = 1;
    sc->has_str = 0;
    for (unsigned c = 0; c < sc->ncols; c++)
    {
        unsigned al = pzpd_type_align(sc->type[c]), sz = pzpd_type_size(sc->type[c]);
        if ( (al == 0) || (sc->count[c] == 0) ) { pzpd_set_error(PZPD_E_ARG, "table %s: column %s has a bad type or count", sc->name, sc->colname[c]); return 0; }
        off = pzpd_align_up(off, al);
        sc->offset[c] = (uint32_t) off;
        off += (uint64_t) sz * sc->count[c];
        if (al > maxAlign) { maxAlign = al; }
        if (sc->type[c] == PZPD_TYPE_STR) { sc->has_str = 1; }
    }
    off = pzpd_align_up(off, maxAlign);
    if ( (off == 0) || (off > (1u << 20)) ) { pzpd_set_error(PZPD_E_ARG, "table %s: rows of %llu bytes (limit 1 MiB)", sc->name, (unsigned long long) off); return 0; }
    sc->stride = (uint32_t) off;
    return 1;
}

/** @brief Parse "name:type[count], ..." into a schema.
 *  @return 1 on success, 0 on failure (error set). */
static int pzpd_schema_parse(const char *name, const char *text, unsigned flags, struct pzpd_tschema *sc)
{
    memset(sc, 0, sizeof(*sc));
    if ( (name == NULL) || (name[0] == 0) || (strlen(name) > PZPD_MAX_TABLE_NAME) ) { pzpd_set_error(PZPD_E_ARG, "table names must be 1..%d bytes", PZPD_MAX_TABLE_NAME); return 0; }
    if (text == NULL) { pzpd_set_error(PZPD_E_ARG, "table %s: no schema", name); return 0; }
    if (flags & ~(PZPD_TABLE_GLOBAL | PZPD_TABLE_BULK)) { pzpd_set_error(PZPD_E_ARG, "table %s: unknown flags", name); return 0; }
    snprintf(sc->name, sizeof(sc->name), "%s", name);
    sc->flags = flags;
    const char *p = text;
    while (1)
    {
        while ( (*p == ' ') || (*p == ',') || (*p == '\t') ) { p++; }
        if (*p == 0) { break; }
        const char *tok = p;
        while ( (*p != 0) && (*p != ' ') && (*p != ',') && (*p != '\t') ) { p++; }
        size_t tl = (size_t)(p - tok);
        const char *colon = memchr(tok, ':', tl);
        if ( (colon == NULL) || (colon == tok) ) { pzpd_set_error(PZPD_E_ARG, "table %s: \"%.*s\" is not name:type[count]", name, (int) tl, tok); return 0; }
        if (sc->ncols == PZPD_MAX_COLUMNS) { pzpd_set_error(PZPD_E_ARG, "table %s: more than %d columns", name, PZPD_MAX_COLUMNS); return 0; }
        size_t nl = (size_t)(colon - tok);
        if (nl > PZPD_MAX_TABLE_NAME) { pzpd_set_error(PZPD_E_ARG, "table %s: column names must be 1..%d bytes", name, PZPD_MAX_TABLE_NAME); return 0; }
        unsigned c = sc->ncols;
        memcpy(sc->colname[c], tok, nl);
        sc->colname[c][nl] = 0;
        for (unsigned d = 0; d < c; d++) { if (strcmp(sc->colname[d], sc->colname[c]) == 0) { pzpd_set_error(PZPD_E_ARG, "table %s: column %s given twice", name, sc->colname[c]); return 0; } }
        const char *ty = colon + 1, *br = memchr(ty, '[', (size_t)(tok + tl - ty));
        size_t tyl = (br != NULL) ? (size_t)(br - ty) : (size_t)(tok + tl - ty);
        sc->type[c] = 0;
        for (size_t i = 0; i < sizeof(pzpd_types) / sizeof(pzpd_types[0]); i++)
        {
            if ( (strlen(pzpd_types[i].name) == tyl) && (memcmp(pzpd_types[i].name, ty, tyl) == 0) ) { sc->type[c] = pzpd_types[i].type; }
        }
        if (sc->type[c] == 0) { pzpd_set_error(PZPD_E_ARG, "table %s: unknown type \"%.*s\" (u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 str)", name, (int) tyl, ty); return 0; }
        sc->count[c] = 1;
        if (br != NULL)
        {
            char *e = NULL;
            unsigned long n = strtoul(br + 1, &e, 10);
            if ( (e == NULL) || (*e != ']') || (e + 1 != tok + tl) || (n == 0) || (n > 65535) ) { pzpd_set_error(PZPD_E_ARG, "table %s: bad array length in \"%.*s\"", name, (int) tl, tok); return 0; }
            sc->count[c] = (uint16_t) n;
        }
        sc->ncols++;
    }
    if (sc->ncols == 0) { pzpd_set_error(PZPD_E_ARG, "table %s: schema has no columns", name); return 0; }
    return pzpd_schema_layout(sc);
}

/** @brief Same schema? (name, flags, columns, stride) */
static int pzpd_schema_equal(const struct pzpd_tschema *a, const struct pzpd_tschema *b)
{
    if ( strcmp(a->name, b->name) || (a->flags != b->flags) || (a->ncols != b->ncols) || (a->stride != b->stride) ) { return 0; }
    for (unsigned c = 0; c < a->ncols; c++)
    {
        if ( strcmp(a->colname[c], b->colname[c]) || (a->type[c] != b->type[c]) || (a->count[c] != b->count[c]) || (a->offset[c] != b->offset[c]) ) { return 0; }
    }
    return 1;
}

/** @brief Parse one CSV field at *p (stops at ',', '\n' or end). Quoted fields follow RFC 4180.
 *  @param out  Receives the unquoted bytes.
 *  @return 1 on success, 0 on a malformed quote. *p is left on the separator. */
static int pzpd_csv_field(const char **p, const char *end, struct pzpd_buf *out)
{
    out->len = 0;
    const char *q = *p;
    if ( (q < end) && (*q == '"') )
    {
        q++;
        while (1)
        {
            if (q >= end) { return 0; }                               // unterminated quote
            if (*q == '"')
            {
                if ( (q + 1 < end) && (q[1] == '"') ) { if (!pzpd_buf_append(out, "\"", 1)) { return 0; } q += 2; continue; }
                q++;
                break;
            }
            if (!pzpd_buf_append(out, q, 1)) { return 0; }
            q++;
        }
        if ( (q < end) && (*q != ',') && (*q != '\n') ) { return 0; }     // text after the closing quote
    }
    else
    {
        const char *s0 = q;
        while ( (q < end) && (*q != ',') && (*q != '\n') ) { if (*q == '"') { return 0; } q++; }
        if (!pzpd_buf_append(out, s0, (size_t)(q - s0))) { return 0; }
    }
    *p = q;
    return 1;
}

/** @brief Store one parsed CSV value into a row.
 *  @return 1 on success, 0 on a bad or out-of-range value (error set). */
static int pzpd_csv_store(const struct pzpd_tschema *sc, unsigned c, unsigned k, const struct pzpd_buf *f, unsigned char *row, struct pzpd_buf *heap, uint64_t rowNo)
{
    unsigned char *dst = row + sc->offset[c] + (size_t) k * pzpd_type_size(sc->type[c]);
    uint8_t t = sc->type[c];
    if (t == PZPD_TYPE_STR)
    {
        if ( (f->len > 0xFFFFFFFFull) || (heap->len + f->len > 0xFFFFFFFFull) ) { pzpd_set_error(PZPD_E_ARG, "row %llu: string too long", (unsigned long long) rowNo); return 0; }
        pzpd_str sv = { (uint32_t) heap->len, (uint32_t) f->len };
        if ( (f->len > 0) && !pzpd_buf_append(heap, f->data, f->len) ) { return 0; }
        memcpy(dst, &sv, sizeof(sv));
        return 1;
    }
    char tmp[128];
    if ( (f->len == 0) || (f->len >= sizeof(tmp)) ) { pzpd_set_error(PZPD_E_ARG, "row %llu, column %s: %s value", (unsigned long long) rowNo, sc->colname[c], f->len ? "too long a" : "empty"); return 0; }
    memcpy(tmp, f->data, f->len);
    tmp[f->len] = 0;
    char *e = NULL;
    errno = 0;
    if ( (t == PZPD_TYPE_F32) || (t == PZPD_TYPE_F64) )
    {
        if (t == PZPD_TYPE_F32) { float v = strtof(tmp, &e); if ( (e == NULL) || (*e != 0) ) { goto bad; } memcpy(dst, &v, 4); }
        else                    { double v = strtod(tmp, &e); if ( (e == NULL) || (*e != 0) ) { goto bad; } memcpy(dst, &v, 8); }
        return 1;
    }
    if ( (t == PZPD_TYPE_U8) || (t == PZPD_TYPE_U16) || (t == PZPD_TYPE_U32) || (t == PZPD_TYPE_U64) )
    {
        if (tmp[strspn(tmp, " \t\n\v\f\r")] == '-') { goto range; }   // strtoull skips this whitespace, then negates without ERANGE
        unsigned long long v = strtoull(tmp, &e, 10);
        if ( (e == NULL) || (*e != 0) ) { goto bad; }
        if (errno == ERANGE) { goto range; }
        switch (t)
        {
            case PZPD_TYPE_U8:  if (v > 0xFFull) { goto range; }       { uint8_t  x = (uint8_t)  v; memcpy(dst, &x, 1); } break;
            case PZPD_TYPE_U16: if (v > 0xFFFFull) { goto range; }     { uint16_t x = (uint16_t) v; memcpy(dst, &x, 2); } break;
            case PZPD_TYPE_U32: if (v > 0xFFFFFFFFull) { goto range; } { uint32_t x = (uint32_t) v; memcpy(dst, &x, 4); } break;
            default:                                                   { uint64_t x = (uint64_t) v; memcpy(dst, &x, 8); } break;
        }
        return 1;
    }
    {
        long long v = strtoll(tmp, &e, 10);
        if ( (e == NULL) || (*e != 0) ) { goto bad; }
        if (errno == ERANGE) { goto range; }
        switch (t)
        {
            case PZPD_TYPE_I8:  if ( (v < -128) || (v > 127) ) { goto range; }                { int8_t  x = (int8_t)  v; memcpy(dst, &x, 1); } break;
            case PZPD_TYPE_I16: if ( (v < -32768) || (v > 32767) ) { goto range; }            { int16_t x = (int16_t) v; memcpy(dst, &x, 2); } break;
            case PZPD_TYPE_I32: if ( (v < -2147483648LL) || (v > 2147483647LL) ) { goto range; } { int32_t x = (int32_t) v; memcpy(dst, &x, 4); } break;
            default:                                                                          { int64_t x = (int64_t) v; memcpy(dst, &x, 8); } break;
        }
        return 1;
    }
bad:
    pzpd_set_error(PZPD_E_ARG, "row %llu, column %s: \"%s\" is not a valid %s", (unsigned long long) rowNo, sc->colname[c], tmp, pzpd_type_name(t));
    return 0;
range:
    pzpd_set_error(PZPD_E_ARG, "row %llu, column %s: %s is out of range for %s", (unsigned long long) rowNo, sc->colname[c], tmp, pzpd_type_name(t));
    return 0;
}

/** @brief Parse CSV text (one row per line) into rows appended to `rows`, strings to `heap`.
 *  @return Rows parsed, or -1 on error (error set, naming the row and column). */
static int64_t pzpd_csv_parse(const struct pzpd_tschema *sc, const char *csv, size_t len, struct pzpd_buf *rows, struct pzpd_buf *heap)
{
    const char *p = csv, *end = csv + len;
    struct pzpd_buf f = {0};
    unsigned char *row = (unsigned char *) calloc(1, sc->stride);
    int64_t n = 0;
    if (row == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return -1; }
    while (p < end)
    {
        if (*p == '\n') { p++; continue; }                                  // blank line
        memset(row, 0, sc->stride);
        for (unsigned c = 0; c < sc->ncols; c++)
        {
            for (unsigned k = 0; k < sc->count[c]; k++)
            {
                int first = (c == 0) && (k == 0);
                if (!first)
                {
                    if ( (p >= end) || (*p != ',') ) { pzpd_set_error(PZPD_E_ARG, "row %llu: too few fields (table %s needs one per column element)", (unsigned long long) n + 1, sc->name); goto fail; }
                    p++;
                }
                if (!pzpd_csv_field(&p, end, &f)) { pzpd_set_error(PZPD_E_ARG, "row %llu, column %s: malformed quoted field", (unsigned long long) n + 1, sc->colname[c]); goto fail; }
                if (!pzpd_csv_store(sc, c, k, &f, row, heap, (uint64_t) n + 1)) { goto fail; }
            }
        }
        if ( (p < end) && (*p != '\n') ) { pzpd_set_error(PZPD_E_ARG, "row %llu: too many fields for table %s", (unsigned long long) n + 1, sc->name); goto fail; }
        if (!pzpd_buf_append(rows, row, sc->stride)) { goto fail; }
        n++;
        if (p < end) { p++; }
    }
    free(row);
    pzpd_buf_free(&f);
    return n;
fail:
    free(row);
    pzpd_buf_free(&f);
    return -1;
}

/** @brief Append a printf-formatted string to a buffer. */
static int pzpd_buf_printf(struct pzpd_buf *b, const char *fmt, ...)
{
    char tmp[128];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if ( (n < 0) || ((size_t) n >= sizeof(tmp)) ) { return 0; }
    return pzpd_buf_append(b, tmp, (size_t) n);
}

/** @brief Shortest decimal text of a float that reads back bit-exactly. */
static int pzpd_fmt_f32(struct pzpd_buf *b, float v)
{
    char tmp[64];
    for (int p = 1; p <= 9; p++)
    {
        snprintf(tmp, sizeof(tmp), "%.*g", p, (double) v);
        float back = strtof(tmp, NULL);
        if ( (memcmp(&back, &v, 4) == 0) || ((v != v) && (back != back)) ) { break; }
    }
    return pzpd_buf_append(b, tmp, strlen(tmp));
}

/** @brief Shortest decimal text of a double that reads back bit-exactly. */
static int pzpd_fmt_f64(struct pzpd_buf *b, double v)
{
    char tmp[64];
    for (int p = 1; p <= 17; p++)
    {
        snprintf(tmp, sizeof(tmp), "%.*g", p, v);
        double back = strtod(tmp, NULL);
        if ( (memcmp(&back, &v, 8) == 0) || ((v != v) && (back != back)) ) { break; }
    }
    return pzpd_buf_append(b, tmp, strlen(tmp));
}

/** @brief Render one row as a CSV line (with '\n'). `str` values are resolved in heap; a value that
 *  points outside the heap is rendered empty (damaged input never reads out of bounds).
 *  @return 1 on success, 0 on allocation failure. */
static int pzpd_csv_render(const struct pzpd_tschema *sc, const unsigned char *row, const char *heap, uint64_t heapLen, struct pzpd_buf *out)
{
    int first = 1;
    for (unsigned c = 0; c < sc->ncols; c++)
    {
        unsigned sz = pzpd_type_size(sc->type[c]);
        for (unsigned k = 0; k < sc->count[c]; k++)
        {
            const unsigned char *v = row + sc->offset[c] + (size_t) k * sz;
            if (!first && !pzpd_buf_append(out, ",", 1)) { return 0; }
            first = 0;
            int ok = 1;
            switch (sc->type[c])
            {
                case PZPD_TYPE_U8:  { uint8_t  x; memcpy(&x, v, 1); ok = pzpd_buf_printf(out, "%u", (unsigned) x); } break;
                case PZPD_TYPE_I8:  { int8_t   x; memcpy(&x, v, 1); ok = pzpd_buf_printf(out, "%d", (int) x); } break;
                case PZPD_TYPE_U16: { uint16_t x; memcpy(&x, v, 2); ok = pzpd_buf_printf(out, "%u", (unsigned) x); } break;
                case PZPD_TYPE_I16: { int16_t  x; memcpy(&x, v, 2); ok = pzpd_buf_printf(out, "%d", (int) x); } break;
                case PZPD_TYPE_U32: { uint32_t x; memcpy(&x, v, 4); ok = pzpd_buf_printf(out, "%u", x); } break;
                case PZPD_TYPE_I32: { int32_t  x; memcpy(&x, v, 4); ok = pzpd_buf_printf(out, "%d", x); } break;
                case PZPD_TYPE_U64: { uint64_t x; memcpy(&x, v, 8); ok = pzpd_buf_printf(out, "%llu", (unsigned long long) x); } break;
                case PZPD_TYPE_I64: { int64_t  x; memcpy(&x, v, 8); ok = pzpd_buf_printf(out, "%lld", (long long) x); } break;
                case PZPD_TYPE_F32: { float    x; memcpy(&x, v, 4); ok = pzpd_fmt_f32(out, x); } break;
                case PZPD_TYPE_F64: { double   x; memcpy(&x, v, 8); ok = pzpd_fmt_f64(out, x); } break;
                case PZPD_TYPE_STR:
                {
                    pzpd_str sv;
                    memcpy(&sv, v, sizeof(sv));
                    const char *s = "";
                    size_t l = 0;
                    if ( (heap != NULL) && pzpd_in_file(sv.offset, sv.len, heapLen) ) { s = heap + sv.offset; l = sv.len; }
                    int quote = (l == 0);
                    for (size_t i = 0; i < l; i++) { if ( (s[i] == ',') || (s[i] == '"') || (s[i] == '\n') || (s[i] == '\r') ) { quote = 1; } }
                    if ( (l > 0) && ((s[0] == ' ') || (s[l - 1] == ' ')) ) { quote = 1; }
                    if (quote) { ok = pzpd_buf_append(out, "\"", 1); }
                    for (size_t i = 0; ok && (i < l); i++) { ok = (s[i] == '"') ? pzpd_buf_append(out, "\"\"", 2) : pzpd_buf_append(out, s + i, 1); }
                    if (ok && quote) { ok = pzpd_buf_append(out, "\"", 1); }
                }
                break;
                default: ok = 0;
            }
            if (!ok) { return 0; }
        }
    }
    return pzpd_buf_append(out, "\n", 1);
}

/** @brief Serialise a table section: header, columns, [row index], rows, string heap.
 *  @param index records + 1 row starts, or NULL for global / schema-only tables. */
static int pzpd_table_section(struct pzpd_buf *out, const struct pzpd_tschema *sc, uint64_t records, const uint32_t *index,
                              const void *rows, uint64_t nrows, const void *heap, uint64_t heapLen)
{
    out->len = 0;
    struct pzpd_disk_table_head h;
    memset(&h, 0, sizeof(h));
    snprintf(h.name, sizeof(h.name), "%s", sc->name);
    h.flags      = sc->flags;
    h.ncols      = sc->ncols;
    h.row_stride = sc->stride;
    h.records    = (index != NULL) ? records : 0;
    h.rows       = nrows;
    uint64_t off = sizeof(h) + (uint64_t) sc->ncols * sizeof(struct pzpd_disk_column);
    off = pzpd_align_up(off, 8);
    h.index_offset = (index != NULL) ? off : 0;
    if (index != NULL) { off = pzpd_align_up(off + (records + 1) * 4, 8); }
    h.rows_offset = off;
    off = pzpd_align_up(off + nrows * sc->stride, 8);
    h.heap_offset = off;
    h.heap_bytes  = heapLen;
    int ok = pzpd_buf_append(out, &h, sizeof(h));
    for (unsigned c = 0; ok && (c < sc->ncols); c++)
    {
        struct pzpd_disk_column dc;
        memset(&dc, 0, sizeof(dc));
        snprintf(dc.name, sizeof(dc.name), "%s", sc->colname[c]);
        dc.type   = sc->type[c];
        dc.count  = sc->count[c];
        dc.offset = sc->offset[c];
        ok = pzpd_buf_append(out, &dc, sizeof(dc));
    }
    static const unsigned char zeros[8] = {0};
    if (ok && (index != NULL)) { ok = pzpd_buf_append(out, zeros, h.index_offset - out->len) && pzpd_buf_append(out, index, (records + 1) * 4); }
    ok = ok && pzpd_buf_append(out, zeros, h.rows_offset - out->len) && pzpd_buf_append(out, rows, nrows * sc->stride);
    ok = ok && pzpd_buf_append(out, zeros, h.heap_offset - out->len) && pzpd_buf_append(out, heap, heapLen);
    return ok;
}

/** @brief A validated view of one table section (pointers into a mapping). */
struct pzpd_tview
{
    int                  present;     ///< 1 if the section exists
    uint64_t             records;     ///< Records covered by the index
    uint64_t             rows;        ///< Rows
    const uint32_t      *index;       ///< records + 1 row starts, NULL for global tables
    const unsigned char *rowdata;     ///< Rows
    const char          *heap;        ///< Strings
    uint64_t             heap_bytes;  ///< Strings size
    int                  checked;     ///< 1 once the whole row index was verified monotonic (pzpd_table_shard_view()); atomic
};

/** @brief Validate a table section and extract its schema (sc may be NULL) and view.
 *  @param expectRecords For record tables in shards: the shard's record count (the index must cover it).
 *  @return 1 if valid, 0 otherwise (error set). */
static int pzpd_table_parse(const unsigned char *d, uint64_t bytes, struct pzpd_tschema *sc, struct pzpd_tview *v, int64_t expectRecords)
{
    struct pzpd_disk_table_head h;
    if (bytes < sizeof(h)) { pzpd_set_error(PZPD_E_FORMAT, "table section too small"); return 0; }
    memcpy(&h, d, sizeof(h));
    if ( (memchr(h.name, 0, sizeof(h.name)) == NULL) || (h.name[0] == 0) || (h.ncols == 0) || (h.ncols > PZPD_MAX_COLUMNS) || (h.row_stride == 0) ||
         (h.row_stride > (1u << 20)) || (h.flags & ~(PZPD_TABLE_GLOBAL | PZPD_TABLE_BULK)) ||
         ((uint64_t) sizeof(h) + (uint64_t) h.ncols * sizeof(struct pzpd_disk_column) > bytes) )
        { pzpd_set_error(PZPD_E_FORMAT, "table section header is damaged"); return 0; }
    struct pzpd_tschema tmp;
    struct pzpd_tschema *s = (sc != NULL) ? sc : &tmp;
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", h.name);
    s->flags = h.flags;
    s->ncols = h.ncols;
    for (unsigned c = 0; c < h.ncols; c++)
    {
        struct pzpd_disk_column dc;
        memcpy(&dc, d + sizeof(h) + c * sizeof(dc), sizeof(dc));
        if ( (memchr(dc.name, 0, sizeof(dc.name)) == NULL) || (dc.name[0] == 0) || (pzpd_type_size(dc.type) == 0) || (dc.count == 0) )
            { pzpd_set_error(PZPD_E_FORMAT, "table %s: column %u is damaged", s->name, c); return 0; }
        snprintf(s->colname[c], sizeof(s->colname[c]), "%s", dc.name);
        s->type[c]  = dc.type;
        s->count[c] = dc.count;
    }
    // The stored layout must equal the one the schema implies
    if (!pzpd_schema_layout(s) || (s->stride != h.row_stride)) { pzpd_set_error(PZPD_E_FORMAT, "table %s: row layout is damaged", s->name); return 0; }
    for (unsigned c = 0; c < h.ncols; c++)
    {
        struct pzpd_disk_column dc;
        memcpy(&dc, d + sizeof(h) + c * sizeof(dc), sizeof(dc));
        if (dc.offset != s->offset[c]) { pzpd_set_error(PZPD_E_FORMAT, "table %s: column offsets are damaged", s->name); return 0; }
    }
    int global = (h.flags & PZPD_TABLE_GLOBAL) != 0;
    if (v != NULL)
    {
        memset(v, 0, sizeof(*v));
        if ( (h.rows > 0xFFFFFFFFull) || !pzpd_in_file(h.rows_offset, h.rows * h.row_stride, bytes) || !pzpd_in_file(h.heap_offset, h.heap_bytes, bytes) ||
             ((h.rows_offset & 7) != 0) )
            { pzpd_set_error(PZPD_E_FORMAT, "table %s: rows or strings are damaged", s->name); return 0; }
        if (!global && (expectRecords >= 0))
        {
            if ( (h.records != (uint64_t) expectRecords) || (h.index_offset == 0) || ((h.index_offset & 3) != 0) || !pzpd_in_file(h.index_offset, (h.records + 1) * 4, bytes) )
                { pzpd_set_error(PZPD_E_FORMAT, "table %s: row index is damaged", s->name); return 0; }
            const uint32_t *ix = (const uint32_t *) (d + h.index_offset);
            // O(1) here, so opening a shard stays cheap: the ends of the CSR index. Single lookups check
            // their own two entries; pzpd_table_shard_view() checks the whole index once before handing it out.
            if ( (ix[0] != 0) || (ix[h.records] != h.rows) ) { pzpd_set_error(PZPD_E_FORMAT, "table %s: row index is damaged", s->name); return 0; }
            v->index = ix;
        }
        v->present    = 1;
        v->records    = h.records;
        v->rows       = h.rows;
        v->rowdata    = d + h.rows_offset;
        v->heap       = (const char *) (d + h.heap_offset);
        v->heap_bytes = h.heap_bytes;
    }
    return 1;
}

//-----------------------------------------------------------------------------------------------
// Word index (spec §3.7, §4.10): tokenizer v1, shard section builder, section views
//-----------------------------------------------------------------------------------------------

/** @brief 1 for the ASCII bytes that are word characters for Python's `re` (\w: letters, digits, '_'). */
static const unsigned char pzpd_ascii_word[128] =
{
    ['0']=1,['1']=1,['2']=1,['3']=1,['4']=1,['5']=1,['6']=1,['7']=1,['8']=1,['9']=1,['_']=1,
    ['a']=1,['b']=1,['c']=1,['d']=1,['e']=1,['f']=1,['g']=1,['h']=1,['i']=1,['j']=1,['k']=1,['l']=1,['m']=1,
    ['n']=1,['o']=1,['p']=1,['q']=1,['r']=1,['s']=1,['t']=1,['u']=1,['v']=1,['w']=1,['x']=1,['y']=1,['z']=1,
    ['A']=1,['B']=1,['C']=1,['D']=1,['E']=1,['F']=1,['G']=1,['H']=1,['I']=1,['J']=1,['K']=1,['L']=1,['M']=1,
    ['N']=1,['O']=1,['P']=1,['Q']=1,['R']=1,['S']=1,['T']=1,['U']=1,['V']=1,['W']=1,['X']=1,['Y']=1,['Z']=1
};

/** @brief 1 if a non-ASCII code point is a word character for Python's `re` (str.isalnum() or '_'). */
static int pzpd_word_cp(uint32_t c)
{
    size_t lo = 0, hi = sizeof(pzpd_unicode_word) / sizeof(pzpd_unicode_word[0]);
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (pzpd_unicode_word[mid][1] < c) { lo = mid + 1; } else { hi = mid; } }
    return (lo < sizeof(pzpd_unicode_word) / sizeof(pzpd_unicode_word[0])) && (pzpd_unicode_word[lo][0] <= c);
}

/** @brief Python's str.lower() of one non-ASCII code point: 1 or 2 code points in out. @return Their number. */
static int pzpd_lower_cp(uint32_t c, uint32_t out[2])
{
    size_t lo = 0, hi = sizeof(pzpd_unicode_lower) / sizeof(pzpd_unicode_lower[0]);
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (pzpd_unicode_lower[mid][0] < c) { lo = mid + 1; } else { hi = mid; } }
    if ( (lo < sizeof(pzpd_unicode_lower) / sizeof(pzpd_unicode_lower[0])) && (pzpd_unicode_lower[lo][0] == c) )
    {
        out[0] = pzpd_unicode_lower[lo][1];
        out[1] = pzpd_unicode_lower[lo][2];
        return (out[1] != 0) ? 2 : 1;
    }
    out[0] = c;
    return 1;
}

/** @brief Decode one UTF-8 code point at p (n bytes left). Invalid or truncated sequences give U+FFFD over one byte.
 *  @return Bytes consumed (≥ 1). */
static size_t pzpd_utf8_next(const unsigned char *p, size_t n, uint32_t *cp)
{
    unsigned char b = p[0];
    size_t need = (b >= 0xF0 && b <= 0xF4) ? 4 : (b >= 0xE0) && (b <= 0xEF) ? 3 : (b >= 0xC2) && (b <= 0xDF) ? 2 : 0;
    if ( (need == 0) || (need > n) ) { *cp = 0xFFFD; return 1; }
    uint32_t c = (need == 2) ? (b & 0x1Fu) : (need == 3) ? (b & 0x0Fu) : (b & 0x07u);
    for (size_t k = 1; k < need; k++)
    {
        if ((p[k] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        c = (c << 6) | (p[k] & 0x3Fu);
    }
    // No overlong forms, surrogates or values past U+10FFFF (strict UTF-8, as Python's decoder)
    if ( ((need == 3) && ((c < 0x800) || ((c >= 0xD800) && (c <= 0xDFFF)))) || ((need == 4) && ((c < 0x10000) || (c > 0x10FFFF))) ) { *cp = 0xFFFD; return 1; }
    *cp = c;
    return need;
}

/** @brief Tokenizer v1 state: the word being assembled. */
struct pzpd_tok
{
    struct pzpd_buf word;   ///< Its bytes (ASCII, lower-case)
    int  foreign;           ///< 1 once a non-ASCII word character joined it: the word is dropped (spec §3.7)
    int  (*emit)(const char *, size_t, void *);
    void *user;
    size_t emitted;         ///< Words emitted
    int  stop;              ///< 1 after emit asked to stop
};

/** @brief End the current word: emit it if it is all ASCII. */
static void pzpd_tok_flush(struct pzpd_tok *t)
{
    if ( (t->word.len > 0) && !t->foreign && !t->stop )
    {
        t->emitted++;
        if (t->emit((const char *) t->word.data, t->word.len, t->user) != 0) { t->stop = 1; }
    }
    t->word.len = 0;
    t->foreign  = 0;
}

/** @brief Feed one (already lower-cased) code point. @return 0 on out of memory. */
static int pzpd_tok_cp(struct pzpd_tok *t, uint32_t c)
{
    if (c < 128)
    {
        if (!pzpd_ascii_word[c]) { pzpd_tok_flush(t); return 1; }
        unsigned char b = (unsigned char) c;
        return pzpd_buf_append(&t->word, &b, 1);
    }
    if (pzpd_word_cp(c)) { t->foreign = 1; } else { pzpd_tok_flush(t); }
    return 1;
}

/** @brief Tokenizer v1 over text, with the caller's word buffer (reused across calls). @return Words emitted, or -1 on out of memory. */
static int64_t pzpd_tokenize_buf(const char *text, size_t len, struct pzpd_buf *scratch, int (*emit)(const char *, size_t, void *), void *user)
{
    struct pzpd_tok t;
    memset(&t, 0, sizeof(t));
    t.word = *scratch;
    t.word.len = 0;
    t.emit = emit;
    t.user = user;
    const unsigned char *p = (const unsigned char *) text;
    int ok = 1;
    for (size_t i = 0; ok && !t.stop && (i < len); )
    {
        unsigned char b = p[i];
        if (b < 128)
        {
            // str.lower() of ASCII only changes A-Z
            ok = pzpd_tok_cp(&t, ((b >= 'A') && (b <= 'Z')) ? (uint32_t)(b + 32) : b);
            i++;
            continue;
        }
        uint32_t c, low[2];
        i += pzpd_utf8_next(p + i, len - i, &c);
        int nl = pzpd_lower_cp(c, low);
        for (int k = 0; ok && (k < nl); k++) { ok = pzpd_tok_cp(&t, low[k]); }
    }
    if (ok) { pzpd_tok_flush(&t); }
    *scratch = t.word;
    if (!ok) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return -1; }
    return (int64_t) t.emitted;
}

size_t pzpd_tokenize(const char *text, size_t len, int (*emit)(const char *word, size_t len, void *user), void *user)
{
    pzpd_clear_error();
    if ( (emit == NULL) || ((text == NULL) && (len > 0)) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    struct pzpd_buf scratch = {0};
    int64_t n = pzpd_tokenize_buf(text, len, &scratch, emit, user);
    pzpd_buf_free(&scratch);
    return (n < 0) ? 0 : (size_t) n;
}

/** @brief Order of words everywhere in the word index: bytes (memcmp), a prefix first. */
static int pzpd_word_cmp(const char *a, size_t al, const char *b, size_t bl)
{
    int c = memcmp(a, b, (al < bl) ? al : bl);
    if (c != 0) { return c; }
    return (al < bl) ? -1 : (al > bl);
}

/** @brief Distinct byte strings with dense ids (open addressing), for the word index builder. */
struct pzpd_sdict
{
    uint32_t *slot;          ///< id + 1, 0 = empty
    uint64_t  cap;           ///< Slots (power of two)
    uint64_t *off;           ///< Bytes of id in `bytes`
    uint32_t *len;           ///< Their length
    uint64_t *hash;          ///< Their XXH64
    uint32_t  n, ecap;       ///< Ids, capacity of off / len / hash
    struct pzpd_buf bytes;   ///< Every string
};

static void pzpd_sdict_free(struct pzpd_sdict *d)
{
    free(d->slot); free(d->off); free(d->len); free(d->hash);
    pzpd_buf_free(&d->bytes);
    memset(d, 0, sizeof(*d));
}

/** @brief Id of a string without inserting it. @return The id, or -1 if absent. */
static int64_t pzpd_sdict_find(const struct pzpd_sdict *d, const char *s, size_t n)
{
    if (d->cap == 0) { return -1; }
    uint64_t h = XXH64(s, n, 0), k = h & (d->cap - 1);
    while (d->slot[k] != 0)
    {
        uint32_t id = d->slot[k] - 1;
        if ( (d->hash[id] == h) && (d->len[id] == n) && (memcmp(d->bytes.data + d->off[id], s, n) == 0) ) { return id; }
        k = (k + 1) & (d->cap - 1);
    }
    return -1;
}

/** @brief Id of a string, inserting it if new. @return The id, or -1 on out of memory. */
static int64_t pzpd_sdict_id(struct pzpd_sdict *d, const char *s, size_t n)
{
    if ( (d->cap == 0) || ((uint64_t)(d->n + 1) * 2 > d->cap) )
    {
        uint64_t cap = d->cap ? d->cap * 2 : 1024;
        uint32_t *ns = (uint32_t *) calloc(cap, sizeof(uint32_t));
        if (ns == NULL) { return -1; }
        for (uint32_t id = 0; id < d->n; id++)
        {
            uint64_t k = d->hash[id] & (cap - 1);
            while (ns[k] != 0) { k = (k + 1) & (cap - 1); }
            ns[k] = id + 1;
        }
        free(d->slot);
        d->slot = ns;
        d->cap  = cap;
    }
    uint64_t h = XXH64(s, n, 0), k = h & (d->cap - 1);
    while (d->slot[k] != 0)
    {
        uint32_t id = d->slot[k] - 1;
        if ( (d->hash[id] == h) && (d->len[id] == n) && (memcmp(d->bytes.data + d->off[id], s, n) == 0) ) { return id; }
        k = (k + 1) & (d->cap - 1);
    }
    if (d->n == 0xFFFFFFFFu) { return -1; }
    if (d->n == d->ecap)
    {
        uint32_t ec = d->ecap ? d->ecap * 2 : 1024;
        uint64_t *no = (uint64_t *) realloc(d->off, ec * sizeof(uint64_t));   if (no == NULL) { return -1; } d->off = no;
        uint32_t *nl = (uint32_t *) realloc(d->len, ec * sizeof(uint32_t));   if (nl == NULL) { return -1; } d->len = nl;
        uint64_t *nh = (uint64_t *) realloc(d->hash, ec * sizeof(uint64_t));  if (nh == NULL) { return -1; } d->hash = nh;
        d->ecap = ec;
    }
    uint32_t id = d->n;
    d->off[id]  = d->bytes.len;
    d->len[id]  = (uint32_t) n;
    d->hash[id] = h;
    if ( (n > 0) && !pzpd_buf_append(&d->bytes, s, n) ) { return -1; }
    d->slot[k] = id + 1;
    d->n++;
    return id;
}

/** @brief A record table's rows in one shard, as the word index builder reads them. */
struct pzpd_wsrc
{
    uint64_t             records;     ///< Records of the shard
    const uint32_t      *index;       ///< records + 1 row starts (CSR)
    const unsigned char *rows;        ///< Rows
    uint64_t             nrows;       ///< Rows
    uint32_t             stride;      ///< Bytes per row
    const char          *heap;        ///< Strings
    uint64_t             heap_bytes;  ///< Strings size
    uint32_t             text_off;    ///< Offset of the indexed `str` field in a row
    int64_t              source_off;  ///< Offset of the source `str` field, -1 for none
};

/** @brief One word occurrence in a record: (word id, source id; 0 = no source). */
struct pzpd_wocc { uint32_t tid; uint32_t sid; };

/** @brief One (word, record, occurrences) pair of a sub-index, in record order. */
struct pzpd_wpair { uint32_t tid; uint32_t rec; uint32_t cnt; };

static int pzpd_cmp_wocc(const void *a, const void *b)
{
    const struct pzpd_wocc *x = (const struct pzpd_wocc *) a, *y = (const struct pzpd_wocc *) b;
    if (x->tid != y->tid) { return (x->tid < y->tid) ? -1 : 1; }
    return (x->sid < y->sid) ? -1 : (x->sid > y->sid);
}

/** @brief Context of the id comparators below (qsort has none): the dictionary whose strings are sorted. */
static __thread const struct pzpd_sdict *pzpd_sort_dict;

static int pzpd_cmp_dict_ids(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *) a, y = *(const uint32_t *) b;
    const struct pzpd_sdict *d = pzpd_sort_dict;
    return pzpd_word_cmp((const char *) d->bytes.data + d->off[x], d->len[x], (const char *) d->bytes.data + d->off[y], d->len[y]);
}

static int pzpd_cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *) a, y = *(const uint32_t *) b;
    return (x < y) ? -1 : (x > y);
}

/** @brief Tokenizer callback of the builder: append (word id, current source) to the record's occurrences. */
struct pzpd_wbuild
{
    struct pzpd_sdict *words;   ///< Word dictionary
    struct pzpd_buf   *occ;     ///< The record's occurrences (pzpd_wocc)
    uint32_t           sid;     ///< Source of the row being tokenized
    int                failed;  ///< 1 after an error (set)
};

static int pzpd_wbuild_emit(const char *word, size_t len, void *user)
{
    struct pzpd_wbuild *b = (struct pzpd_wbuild *) user;
    if (len > 0xFFFF) { pzpd_set_error(PZPD_E_ARG, "a word of %zu bytes is longer than the word index allows (65535)", len); b->failed = 1; return 1; }
    int64_t id = pzpd_sdict_id(b->words, word, len);
    struct pzpd_wocc o = { (uint32_t) id, b->sid };
    if ( (id < 0) || !pzpd_buf_append(b->occ, &o, sizeof(o)) ) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); b->failed = 1; return 1; }
    return 0;
}

/** @brief Append zero bytes up to the next multiple of 8. */
static int pzpd_buf_pad8(struct pzpd_buf *b)
{
    static const unsigned char zeros[8] = {0};
    return pzpd_buf_append(b, zeros, (size_t)(pzpd_align_up(b->len, 8) - b->len));
}

/** @brief One sub-index serialized: pzpd_disk_subindex offsets relative to the start of `parts`. */
struct pzpd_wsub_out
{
    struct pzpd_disk_subindex head;
    struct pzpd_buf parts;
};

/** @brief Build one sub-index from its pairs (record order, word ids ascending within a record). */
static int pzpd_words_build_sub(const struct pzpd_sdict *words, const struct pzpd_wpair *pairs, uint64_t np, uint64_t records,
                                uint32_t *df, uint32_t *cnt, uint32_t *fid, struct pzpd_wsub_out *o)
{
    if (np > 0xFFFFFFFFull) { pzpd_set_error(PZPD_E_ARG, "word index: more than 4 G record-word pairs in one shard"); return 0; }
    // Vocabulary: the words present, sorted by bytes
    uint32_t *present = (uint32_t *) malloc(sizeof(uint32_t) * (words->n ? words->n : 1));
    if (present == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    uint32_t W = 0;
    for (uint64_t i = 0; i < np; i++)
    {
        uint32_t t = pairs[i].tid;
        if (df[t]++ == 0) { present[W++] = t; }
        cnt[t] += pairs[i].cnt;
    }
    pzpd_sort_dict = words;
    qsort(present, W, sizeof(uint32_t), pzpd_cmp_dict_ids);
    for (uint32_t w = 0; w < W; w++) { fid[present[w]] = w; }

    struct pzpd_buf vocab = {0}, heap = {0}, pidx = {0}, post = {0}, fidx = {0}, fwd = {0};
    uint32_t *cursor = (uint32_t *) calloc((size_t) W + 1, sizeof(uint32_t));
    int ok = (cursor != NULL);
    uint32_t acc = 0;
    for (uint32_t w = 0; ok && (w < W); w++)
    {
        uint32_t t = present[w];
        struct pzpd_disk_word dw = { (uint32_t) heap.len, (uint16_t) words->len[t], 0, df[t], cnt[t] };
        if (heap.len + words->len[t] > 0xFFFFFFFFull) { pzpd_set_error(PZPD_E_ARG, "word index: more than 4 GiB of words in one shard"); ok = 0; break; }
        ok = pzpd_buf_append(&vocab, &dw, sizeof(dw)) && pzpd_buf_append(&heap, words->bytes.data + words->off[t], words->len[t]) &&
             pzpd_buf_append(&pidx, &acc, 4);
        cursor[w] = acc;
        acc += df[t];
    }
    ok = ok && pzpd_buf_append(&pidx, &acc, 4);
    // Postings: pairs are in record order, so every word's list comes out ascending
    if (ok)
    {
        post.data = (unsigned char *) realloc(post.data, (size_t)(np ? np : 1) * 4);
        ok = (post.data != NULL);
        if (ok) { post.cap = (size_t)(np ? np : 1) * 4; post.len = (size_t) np * 4; }
    }
    for (uint64_t i = 0; ok && (i < np); i++) { ((uint32_t *) post.data)[cursor[fid[pairs[i].tid]]++] = pairs[i].rec; }
    // Forward lists: per record, the sub-index word ids, ascending
    uint64_t i = 0;
    for (uint64_t r = 0; ok && (r < records); r++)
    {
        uint32_t start = (uint32_t)(fwd.len / 4);
        ok = pzpd_buf_append(&fidx, &start, 4);
        uint64_t j = i;
        while ( ok && (j < np) && (pairs[j].rec == r) ) { uint32_t f = fid[pairs[j].tid]; ok = pzpd_buf_append(&fwd, &f, 4); j++; }
        if (ok && (j - i > 1)) { qsort(fwd.data + (size_t) start * 4, (size_t)(j - i), 4, pzpd_cmp_u32); }
        i = j;
    }
    uint32_t endf = (uint32_t)(fwd.len / 4);
    ok = ok && pzpd_buf_append(&fidx, &endf, 4);
    for (uint32_t w = 0; w < W; w++) { df[present[w]] = 0; cnt[present[w]] = 0; }   // reset for the next sub-index

    // Serialize the parts, each 8-aligned
    memset(&o->head, 0, sizeof(o->head));
    o->head.records  = records;
    o->head.words    = W;
    o->head.postings = np;
    struct pzpd_buf *pp = &o->parts;
    pp->len = 0;
    o->head.vocab_offset      = pp->len; ok = ok && pzpd_buf_append(pp, vocab.data, vocab.len) && pzpd_buf_pad8(pp);
    o->head.post_index_offset = pp->len; ok = ok && pzpd_buf_append(pp, pidx.data,  pidx.len)  && pzpd_buf_pad8(pp);
    o->head.post_offset       = pp->len; ok = ok && pzpd_buf_append(pp, post.data,  post.len)  && pzpd_buf_pad8(pp);
    o->head.fwd_index_offset  = pp->len; ok = ok && pzpd_buf_append(pp, fidx.data,  fidx.len)  && pzpd_buf_pad8(pp);
    o->head.fwd_offset        = pp->len; ok = ok && pzpd_buf_append(pp, fwd.data,   fwd.len)   && pzpd_buf_pad8(pp);
    o->head.heap_offset       = pp->len; ok = ok && pzpd_buf_append(pp, heap.data,  heap.len)  && pzpd_buf_pad8(pp);
    o->head.heap_bytes        = heap.len;
    if (!ok && (pzpd_errorCode == PZPD_OK)) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); }
    pzpd_buf_free(&vocab); pzpd_buf_free(&heap); pzpd_buf_free(&pidx); pzpd_buf_free(&post); pzpd_buf_free(&fidx); pzpd_buf_free(&fwd);
    free(cursor);
    free(present);
    return ok;
}

/** @brief Build a shard's word index section (kind 14) from a record table's rows.
 *  @return 1 on success (section data in out), 0 on failure (error set). */
static int pzpd_words_build(struct pzpd_buf *out, const struct pzpd_wsrc *in, const char *source_column)
{
    out->len = 0;
    if (in->records > 0xFFFFFFFFull) { pzpd_set_error(PZPD_E_ARG, "word index: more than 4 G records in one shard"); return 0; }
    struct pzpd_sdict words, sources;
    memset(&words, 0, sizeof(words));
    memset(&sources, 0, sizeof(sources));
    struct pzpd_buf occ = {0}, scratch = {0};
    struct pzpd_buf pairs[PZPD_MAX_WORD_SOURCES + 1];
    memset(pairs, 0, sizeof(pairs));
    struct pzpd_wbuild wb = { &words, &occ, 0, 0 };
    int ok = 1;
    for (uint64_t r = 0; ok && (r < in->records); r++)
    {
        occ.len = 0;
        uint32_t a = in->index[r], b = in->index[r + 1];
        if ( (a > b) || (b > in->nrows) ) { pzpd_set_error(PZPD_E_FORMAT, "word index: the table's row index is damaged"); ok = 0; break; }
        for (uint32_t row = a; ok && (row < b); row++)
        {
            const unsigned char *rp = in->rows + (uint64_t) row * in->stride;
            pzpd_str tf;
            memcpy(&tf, rp + in->text_off, sizeof(tf));
            wb.sid = 0;
            if (in->source_off >= 0)
            {
                pzpd_str sf;
                memcpy(&sf, rp + in->source_off, sizeof(sf));
                if (!pzpd_in_file(sf.offset, sf.len, in->heap_bytes)) { pzpd_set_error(PZPD_E_FORMAT, "word index: a source string lies outside the table's strings"); ok = 0; break; }
                if (sf.len > 0)   // rows with an empty source count in the merged sub-index only
                {
                    int64_t sid = pzpd_sdict_id(&sources, in->heap + sf.offset, sf.len);
                    if (sid < 0) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; break; }
                    if (sid >= PZPD_MAX_WORD_SOURCES) { pzpd_set_error(PZPD_E_ARG, "word index: more than %d distinct source values", PZPD_MAX_WORD_SOURCES); ok = 0; break; }
                    wb.sid = (uint32_t) sid + 1;
                }
            }
            if (!pzpd_in_file(tf.offset, tf.len, in->heap_bytes)) { pzpd_set_error(PZPD_E_FORMAT, "word index: a text string lies outside the table's strings"); ok = 0; break; }
            if ( (pzpd_tokenize_buf(in->heap + tf.offset, tf.len, &scratch, pzpd_wbuild_emit, &wb) < 0) || wb.failed ) { ok = 0; break; }
        }
        if (!ok || (occ.len == 0)) { continue; }
        // Sort by (word, source): runs of a word give the merged pair, sub-runs of a source the per-source pairs
        struct pzpd_wocc *o = (struct pzpd_wocc *) occ.data;
        size_t no = occ.len / sizeof(*o);
        qsort(o, no, sizeof(*o), pzpd_cmp_wocc);
        for (size_t i = 0; ok && (i < no); )
        {
            size_t j = i;
            while ( (j < no) && (o[j].tid == o[i].tid) ) { j++; }
            struct pzpd_wpair mp = { o[i].tid, (uint32_t) r, (uint32_t)(j - i) };
            ok = pzpd_buf_append(&pairs[0], &mp, sizeof(mp));
            for (size_t k = i; ok && (k < j); )
            {
                size_t l = k;
                while ( (l < j) && (o[l].sid == o[k].sid) ) { l++; }
                if (o[k].sid != 0)
                {
                    struct pzpd_wpair sp = { o[k].tid, (uint32_t) r, (uint32_t)(l - k) };
                    ok = pzpd_buf_append(&pairs[o[k].sid], &sp, sizeof(sp));
                }
                k = l;
            }
            i = j;
        }
        if (!ok && (pzpd_errorCode == PZPD_OK)) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); }
    }

    // Sub-index order: the merged one, then the sources by bytes
    unsigned K = sources.n;
    uint32_t order[PZPD_MAX_WORD_SOURCES];
    for (unsigned k = 0; k < K; k++) { order[k] = k; }
    pzpd_sort_dict = &sources;
    qsort(order, K, sizeof(uint32_t), pzpd_cmp_dict_ids);

    uint32_t *df = NULL, *cnt = NULL, *fid = NULL;
    if (ok)
    {
        size_t n = words.n ? words.n : 1;
        df  = (uint32_t *) calloc(n, sizeof(uint32_t));
        cnt = (uint32_t *) calloc(n, sizeof(uint32_t));
        fid = (uint32_t *) calloc(n, sizeof(uint32_t));
        if ( (df == NULL) || (cnt == NULL) || (fid == NULL) ) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
    }
    struct pzpd_wsub_out *subs = ok ? (struct pzpd_wsub_out *) calloc(K + 1, sizeof(struct pzpd_wsub_out)) : NULL;
    if (ok && (subs == NULL)) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
    for (unsigned k = 0; ok && (k <= K); k++)
    {
        const struct pzpd_buf *pb = &pairs[(k == 0) ? 0 : order[k - 1] + 1];
        ok = pzpd_words_build_sub(&words, (const struct pzpd_wpair *) pb->data, pb->len / sizeof(struct pzpd_wpair), in->records, df, cnt, fid, &subs[k]);
    }

    // Serialize: head, sub-index heads, source names, then each sub-index's parts
    if (ok)
    {
        struct pzpd_disk_words_head h;
        memset(&h, 0, sizeof(h));
        h.tokenizer      = PZPD_TOKENIZER_V1;
        h.subindex_count = K + 1;
        if (source_column != NULL) { snprintf(h.source_column, sizeof(h.source_column), "%s", source_column); }
        struct pzpd_buf names = {0};
        for (unsigned k = 1; ok && (k <= K); k++)
        {
            uint32_t s = order[k - 1];
            subs[k].head.source_offset = (uint32_t) names.len;
            subs[k].head.source_len    = sources.len[s];
            ok = pzpd_buf_append(&names, sources.bytes.data + sources.off[s], sources.len[s]);
        }
        uint64_t off = sizeof(h) + (uint64_t)(K + 1) * sizeof(struct pzpd_disk_subindex);
        h.names_offset = off;
        h.names_bytes  = names.len;
        off = pzpd_align_up(off + names.len, 8);
        for (unsigned k = 0; k <= K; k++)
        {
            struct pzpd_disk_subindex *sh = &subs[k].head;
            sh->vocab_offset += off; sh->post_index_offset += off; sh->post_offset += off;
            sh->fwd_index_offset += off; sh->fwd_offset += off; sh->heap_offset += off;
            off += subs[k].parts.len;
        }
        ok = ok && pzpd_buf_append(out, &h, sizeof(h));
        for (unsigned k = 0; ok && (k <= K); k++) { ok = pzpd_buf_append(out, &subs[k].head, sizeof(subs[k].head)); }
        ok = ok && pzpd_buf_append(out, names.data, names.len) && pzpd_buf_pad8(out);
        for (unsigned k = 0; ok && (k <= K); k++) { ok = pzpd_buf_append(out, subs[k].parts.data, subs[k].parts.len); }
        if (!ok && (pzpd_errorCode == PZPD_OK)) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); }
        pzpd_buf_free(&names);
    }
    for (unsigned k = 0; (subs != NULL) && (k <= K); k++) { pzpd_buf_free(&subs[k].parts); }
    free(subs);
    free(df); free(cnt); free(fid);
    for (unsigned k = 0; k <= PZPD_MAX_WORD_SOURCES; k++) { pzpd_buf_free(&pairs[k]); }
    pzpd_buf_free(&occ);
    pzpd_buf_free(&scratch);
    pzpd_sdict_free(&words);
    pzpd_sdict_free(&sources);
    return ok;
}

/** @brief Tokenizer callback that counts the words and keeps the first. */
struct pzpd_one_word { const char *w; size_t len; unsigned n; };

static int pzpd_one_word_emit(const char *w, size_t len, void *user)
{
    struct pzpd_one_word *o = (struct pzpd_one_word *) user;
    if (o->n++ == 0) { o->w = w; o->len = len; }
    return 0;
}

/** @brief 1 if s is exactly one word under tokenizer v1 (so it is already lower-case ASCII). */
static int pzpd_is_one_word(const char *s, size_t len)
{
    struct pzpd_one_word o = { NULL, 0, 0 };
    struct pzpd_buf scratch = {0};
    int ok = (pzpd_tokenize_buf(s, len, &scratch, pzpd_one_word_emit, &o) == 1) && (o.len == len) && (memcmp(o.w, s, len) == 0);
    pzpd_buf_free(&scratch);
    return ok;
}

/** @brief Check a `synonyms` table (spec §3.7): schema `word:str,canonical:str` (global), each value a single
 *  word under tokenizer v1, a word at most once, one step (no canonical is also a word), no word mapped to itself.
 *  @param rows / n / heap  Its rows, or NULL / 0 to check the schema only.
 *  @return 1 if valid, 0 otherwise (error set, naming the row). */
static int pzpd_synonyms_check(const struct pzpd_tschema *sc, const unsigned char *rows, uint64_t n, const char *heap, uint64_t heap_bytes)
{
    if ( !(sc->flags & PZPD_TABLE_GLOBAL) || (sc->ncols != 2) || strcmp(sc->colname[0], "word") || strcmp(sc->colname[1], "canonical") ||
         (sc->type[0] != PZPD_TYPE_STR) || (sc->type[1] != PZPD_TYPE_STR) || (sc->count[0] != 1) || (sc->count[1] != 1) )
        { pzpd_set_error(PZPD_E_ARG, "table %s is reserved for the word index: it must be a global table \"word:str,canonical:str\"", PZPD_SYNONYMS_TABLE); return 0; }
    struct pzpd_sdict words;
    memset(&words, 0, sizeof(words));
    int ok = 1;
    for (uint64_t r = 0; ok && (r < n); r++)
    {
        pzpd_str f[2];
        memcpy(f, rows + r * sc->stride + sc->offset[0], 8);
        memcpy(&f[1], rows + r * sc->stride + sc->offset[1], 8);
        for (int c = 0; ok && (c < 2); c++)
        {
            if (!pzpd_in_file(f[c].offset, f[c].len, heap_bytes) || !pzpd_is_one_word(heap + f[c].offset, f[c].len))
                { pzpd_set_error(PZPD_E_ARG, "%s row %llu: \"%.*s\" is not a single lower-case word", PZPD_SYNONYMS_TABLE, (unsigned long long)(r + 1), (int)(f[c].len > 100 ? 100 : f[c].len), heap + f[c].offset); ok = 0; }
        }
        if (ok && (f[0].len == f[1].len) && !memcmp(heap + f[0].offset, heap + f[1].offset, f[0].len))
            { pzpd_set_error(PZPD_E_ARG, "%s row %llu: \"%.*s\" maps to itself", PZPD_SYNONYMS_TABLE, (unsigned long long)(r + 1), (int) f[0].len, heap + f[0].offset); ok = 0; }
        if (ok)
        {
            uint32_t before = words.n;
            int64_t id = pzpd_sdict_id(&words, heap + f[0].offset, f[0].len);
            if (id < 0) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
            else if (words.n == before) { pzpd_set_error(PZPD_E_ARG, "%s row %llu: \"%.*s\" is listed twice", PZPD_SYNONYMS_TABLE, (unsigned long long)(r + 1), (int) f[0].len, heap + f[0].offset); ok = 0; }
        }
    }
    // One step: a canonical word may not itself be mapped
    for (uint64_t r = 0; ok && (r < n); r++)
    {
        pzpd_str c;
        memcpy(&c, rows + r * sc->stride + sc->offset[1], 8);
        if (pzpd_sdict_find(&words, heap + c.offset, c.len) >= 0)
            { pzpd_set_error(PZPD_E_ARG, "%s row %llu: \"%.*s\" is both a canonical word and mapped (rules are one step)", PZPD_SYNONYMS_TABLE, (unsigned long long)(r + 1), (int) c.len, heap + c.offset); ok = 0; }
    }
    pzpd_sdict_free(&words);
    return ok;
}

/** @brief A validated view of one word index section (shard kind 14 or manifest kind 15). */
struct pzpd_wsec
{
    const unsigned char *d;          ///< Section data
    uint64_t             bytes;      ///< Its size
    uint32_t             tokenizer;  ///< Tokenizer version
    unsigned             nsub;       ///< Sub-indexes
    char                 source_column[24]; ///< Source column ("" for none)
    const char          *names;      ///< Source values
    uint64_t             names_bytes;///< Their size
    int                  manifest;   ///< 1 for kind 15
};

/** @brief One sub-index of a section: shard (postings, forward lists) or manifest (vocabulary with totals). */
struct pzpd_wsub
{
    const char *source;              ///< Source value (NULL / 0 for the merged sub-index)
    uint32_t    source_len;          ///< Its length
    uint64_t    records;             ///< Shard: records covered by the forward index
    uint64_t    words;               ///< Vocabulary size
    uint64_t    postings;            ///< Shard: record-word pairs
    const struct pzpd_disk_word  *vocab;   ///< Shard vocabulary
    const struct pzpd_disk_mword *mvocab;  ///< Manifest vocabulary
    const uint32_t *post_index, *post, *fwd_index, *fwd;  ///< Shard CSR parts
    const char *heap;                ///< Word bytes
    uint64_t    heap_bytes;          ///< Their size
};

/** @brief Validate a word index section's head (O(1) plus the sub-index heads).
 *  @return 1 if valid, 0 otherwise (error set). */
static int pzpd_wsec_parse(const unsigned char *d, uint64_t bytes, int manifest, struct pzpd_wsec *v)
{
    struct pzpd_disk_words_head h;
    if (bytes < sizeof(h)) { pzpd_set_error(PZPD_E_FORMAT, "word index section too small"); return 0; }
    memcpy(&h, d, sizeof(h));
    size_t subSize = manifest ? sizeof(struct pzpd_disk_msubindex) : sizeof(struct pzpd_disk_subindex);
    if ( (h.subindex_count == 0) || (h.subindex_count > PZPD_MAX_WORD_SOURCES + 1) || (memchr(h.source_column, 0, sizeof(h.source_column)) == NULL) ||
         (sizeof(h) + (uint64_t) h.subindex_count * subSize > bytes) || !pzpd_in_file(h.names_offset, h.names_bytes, bytes) )
        { pzpd_set_error(PZPD_E_FORMAT, "word index section header is damaged"); return 0; }
    if (h.tokenizer != PZPD_TOKENIZER_V1) { pzpd_set_error(PZPD_E_VERSION, "word index tokenizer %u is unknown to this library (%d)", h.tokenizer, PZPD_TOKENIZER_V1); return 0; }
    memset(v, 0, sizeof(*v));
    v->d = d; v->bytes = bytes; v->tokenizer = h.tokenizer; v->nsub = h.subindex_count; v->manifest = manifest;
    memcpy(v->source_column, h.source_column, sizeof(v->source_column));
    v->names = (const char *) (d + h.names_offset);
    v->names_bytes = h.names_bytes;
    return 1;
}

/** @brief Check that a u32 array of n entries at off lies in the section, 4-aligned. */
static int pzpd_wsec_u32s(const struct pzpd_wsec *v, uint64_t off, uint64_t n)
{
    return ((off & 3) == 0) && (n <= v->bytes / 4) && pzpd_in_file(off, n * 4, v->bytes);
}

/** @brief View of sub-index k (lazy validation: bounds and CSR ends; spec §4.10).
 *  @param expectRecords For shard sections: the shard's record count; -1 for manifests.
 *  @return 1 if valid, 0 otherwise (error set). */
static int pzpd_wsec_sub(const struct pzpd_wsec *v, unsigned k, int64_t expectRecords, struct pzpd_wsub *s)
{
    memset(s, 0, sizeof(*s));
    if (k >= v->nsub) { pzpd_set_error(PZPD_E_ARG, "sub-index %u out of range", k); return 0; }
    if (v->manifest)
    {
        struct pzpd_disk_msubindex m;
        memcpy(&m, v->d + sizeof(struct pzpd_disk_words_head) + k * sizeof(m), sizeof(m));
        if ( ((k == 0) != (m.source_len == 0)) || !pzpd_in_file(m.source_offset, m.source_len, v->names_bytes) || ((m.vocab_offset & 7) != 0) ||
             (m.words > v->bytes / sizeof(struct pzpd_disk_mword)) || !pzpd_in_file(m.vocab_offset, m.words * sizeof(struct pzpd_disk_mword), v->bytes) ||
             !pzpd_in_file(m.heap_offset, m.heap_bytes, v->bytes) || (m.words > 0xFFFFFFFFull) )
            { pzpd_set_error(PZPD_E_FORMAT, "word index: manifest sub-index %u is damaged", k); return 0; }
        s->source = m.source_len ? v->names + m.source_offset : NULL;
        s->source_len = m.source_len;
        s->words  = m.words;
        s->mvocab = (const struct pzpd_disk_mword *) (v->d + m.vocab_offset);
        s->heap   = (const char *) (v->d + m.heap_offset);
        s->heap_bytes = m.heap_bytes;
        return 1;
    }
    struct pzpd_disk_subindex h;
    memcpy(&h, v->d + sizeof(struct pzpd_disk_words_head) + k * sizeof(h), sizeof(h));
    if ( ((k == 0) != (h.source_len == 0)) || !pzpd_in_file(h.source_offset, h.source_len, v->names_bytes) ||
         ((expectRecords >= 0) && (h.records != (uint64_t) expectRecords)) || (h.words > 0xFFFFFFFFull) || (h.postings > 0xFFFFFFFFull) ||
         ((h.vocab_offset & 3) != 0) || (h.words > v->bytes / sizeof(struct pzpd_disk_word)) ||
         !pzpd_in_file(h.vocab_offset, h.words * sizeof(struct pzpd_disk_word), v->bytes) ||
         !pzpd_wsec_u32s(v, h.post_index_offset, h.words + 1) || !pzpd_wsec_u32s(v, h.post_offset, h.postings) ||
         !pzpd_wsec_u32s(v, h.fwd_index_offset, h.records + 1) || !pzpd_wsec_u32s(v, h.fwd_offset, h.postings) ||
         !pzpd_in_file(h.heap_offset, h.heap_bytes, v->bytes) )
        { pzpd_set_error(PZPD_E_FORMAT, "word index: sub-index %u is damaged", k); return 0; }
    s->source     = h.source_len ? v->names + h.source_offset : NULL;
    s->source_len = h.source_len;
    s->records    = h.records;
    s->words      = h.words;
    s->postings   = h.postings;
    s->vocab      = (const struct pzpd_disk_word *) (v->d + h.vocab_offset);
    s->post_index = (const uint32_t *) (v->d + h.post_index_offset);
    s->post       = (const uint32_t *) (v->d + h.post_offset);
    s->fwd_index  = (const uint32_t *) (v->d + h.fwd_index_offset);
    s->fwd        = (const uint32_t *) (v->d + h.fwd_offset);
    s->heap       = (const char *) (v->d + h.heap_offset);
    s->heap_bytes = h.heap_bytes;
    if ( (s->post_index[0] != 0) || (s->post_index[s->words] != s->postings) || (s->fwd_index[0] != 0) || (s->fwd_index[s->records] != s->postings) )
        { pzpd_set_error(PZPD_E_FORMAT, "word index: sub-index %u is damaged", k); return 0; }
    return 1;
}

/** @brief Sub-index of a section for a source value (NULL = merged). @return Its index, or -1 if the section lacks it. */
static int pzpd_wsec_find(const struct pzpd_wsec *v, const char *source, size_t len, int64_t expectRecords)
{
    if (source == NULL) { return 0; }
    for (unsigned k = 1; k < v->nsub; k++)
    {
        struct pzpd_wsub s;
        if (!pzpd_wsec_sub(v, k, expectRecords, &s)) { return -2; }
        if ( (s.source_len == len) && (memcmp(s.source, source, len) == 0) ) { return (int) k; }
    }
    return -1;
}

/** @brief Word k of a sub-index (bounds-checked). @return 1, or 0 if damaged (error set). */
static int pzpd_wsub_word(const struct pzpd_wsub *s, uint64_t k, const char **w, size_t *len)
{
    uint32_t off = s->vocab ? s->vocab[k].heap_offset : s->mvocab[k].heap_offset;
    uint16_t l   = s->vocab ? s->vocab[k].len         : s->mvocab[k].len;
    if (!pzpd_in_file(off, l, s->heap_bytes)) { pzpd_set_error(PZPD_E_FORMAT, "word index: word %llu is damaged", (unsigned long long) k); return 0; }
    *w = s->heap + off;
    *len = l;
    return 1;
}

/** @brief Binary search of a word in a sub-index vocabulary. @return Its id, -1 if absent, -2 if damaged (error set). */
static int64_t pzpd_wsub_find(const struct pzpd_wsub *s, const char *word, size_t len)
{
    uint64_t lo = 0, hi = s->words;
    while (lo < hi)
    {
        uint64_t mid = lo + (hi - lo) / 2;
        const char *w; size_t l;
        if (!pzpd_wsub_word(s, mid, &w, &l)) { return -2; }
        int c = pzpd_word_cmp(w, l, word, len);
        if (c == 0) { return (int64_t) mid; }
        if (c < 0) { lo = mid + 1; } else { hi = mid; }
    }
    return -1;
}

/** @brief Full check of a shard sub-index (verify): sorted vocabulary, ascending postings and forward lists,
 *  per-word counts, and forward lists = transpose of the postings. @return 1 if valid, 0 otherwise (error set). */
static int pzpd_wsub_check_full(const struct pzpd_wsub *s)
{
    const char *pw = NULL; size_t pl = 0;
    for (uint64_t w = 0; w < s->words; w++)
    {
        const char *cw; size_t cl;
        if (!pzpd_wsub_word(s, w, &cw, &cl)) { return 0; }
        if ( (cl == 0) || ((w > 0) && (pzpd_word_cmp(pw, pl, cw, cl) >= 0)) ) { pzpd_set_error(PZPD_E_FORMAT, "word index: vocabulary not sorted at word %llu", (unsigned long long) w); return 0; }
        pw = cw; pl = cl;
        uint32_t a = s->post_index[w], b = s->post_index[w + 1];
        if ( (a > b) || (b > s->postings) || (b - a != s->vocab[w].records) || (s->vocab[w].count < s->vocab[w].records) )
            { pzpd_set_error(PZPD_E_FORMAT, "word index: postings of word %llu are damaged", (unsigned long long) w); return 0; }
        for (uint32_t p = a; p < b; p++)
        {
            if ( (s->post[p] >= s->records) || ((p > a) && (s->post[p] <= s->post[p - 1])) ) { pzpd_set_error(PZPD_E_FORMAT, "word index: postings of word %llu are damaged", (unsigned long long) w); return 0; }
        }
    }
    uint32_t *cur = (uint32_t *) calloc(s->records ? s->records : 1, sizeof(uint32_t));
    if (cur == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    int ok = 1;
    for (uint64_t r = 0; ok && (r < s->records); r++)
    {
        uint32_t a = s->fwd_index[r], b = s->fwd_index[r + 1];
        if ( (a > b) || (b > s->postings) ) { ok = 0; break; }
        for (uint32_t p = a; ok && (p < b); p++) { if ( (s->fwd[p] >= s->words) || ((p > a) && (s->fwd[p] <= s->fwd[p - 1])) ) { ok = 0; } }
    }
    // Transpose: walking the words in order, each posting must be the next entry of its record's forward list
    for (uint64_t w = 0; ok && (w < s->words); w++)
    {
        for (uint32_t p = s->post_index[w]; ok && (p < s->post_index[w + 1]); p++)
        {
            uint32_t r = s->post[p], at = s->fwd_index[r] + cur[r]++;
            if ( (at >= s->fwd_index[r + 1]) || (s->fwd[at] != w) ) { ok = 0; }
        }
    }
    for (uint64_t r = 0; ok && (r < s->records); r++) { if (s->fwd_index[r] + cur[r] != s->fwd_index[r + 1]) { ok = 0; } }
    free(cur);
    if (!ok) { pzpd_set_error(PZPD_E_FORMAT, "word index: forward lists disagree with the postings"); }
    return ok;
}

//-----------------------------------------------------------------------------------------------
// Writer
//-----------------------------------------------------------------------------------------------

/** @brief One blob of the record being assembled by the writer. */
struct pzpd_wblob
{
    int            present;   ///< 1 if this stream has a blob in the current record
    char          *name;      ///< Copy of the name
    size_t         name_len;  ///< Name length
    unsigned char *data;      ///< Copy of the payload
    size_t         size;      ///< Payload bytes
    pzpd_blob_meta meta;      ///< Format and metadata
};

/** @brief Entry of the writer's archive-wide duplicate check (open addressing). */
struct pzpd_wdedup
{
    uint64_t hash;   ///< XXH64 of the bytes
    uint64_t str;    ///< Offset of the bytes in pzpd_writer::strings
    uint32_t len;    ///< Length of the bytes
    uint8_t  kind;   ///< PZPD_KIND_KEY or PZPD_KIND_NAME
    uint8_t  used;   ///< 1 if the slot is occupied
};

/** @brief A group registered with a name and a size hint (pzpd_writer_group()). */
struct pzpd_wgroup
{
    uint32_t id;       ///< Group id
    uint32_t used;     ///< 1 if the slot is occupied
    uint64_t name;     ///< Offset of the name in pzpd_writer::strings
    uint32_t name_len; ///< Name length
    uint64_t hint;     ///< Expected bytes of the whole group (0 = unknown)
};

/** @brief A completed shard, remembered for the manifest. */
struct pzpd_wshard
{
    char    *path;           ///< Final shard path
    uint64_t first_ordinal;  ///< First record ordinal
    uint64_t record_count;   ///< Records
    uint64_t file_bytes;     ///< File size
    uint64_t index_checksum; ///< Index checksum
};

/** @brief Writer state of one table. */
struct pzpd_wtable
{
    struct pzpd_tschema sc;     ///< Schema
    struct pzpd_buf index;      ///< Open shard: u32 first row of every record so far
    struct pzpd_buf rows;       ///< Open shard: rows
    struct pzpd_buf heap;       ///< Open shard: strings
    uint64_t        nrows;      ///< Open shard: rows so far
    struct pzpd_buf cur_rows;   ///< Current record: rows (str offsets relative to cur_heap)
    struct pzpd_buf cur_heap;   ///< Current record: strings
    uint32_t        cur_n;      ///< Current record: rows
    struct pzpd_buf g_rows;     ///< Global table: rows
    struct pzpd_buf g_heap;     ///< Global table: strings
    uint64_t        g_n;        ///< Global table: rows
};

/** @brief A word index declared on a writer (pzpd_writer_words()). */
struct pzpd_wwords
{
    char     table[24];    ///< Indexed table
    char     column[24];   ///< Indexed column
    char     source[24];   ///< Source column, "" for none
    unsigned t;            ///< Table id
    uint32_t text_off;     ///< Offset of the indexed field in a row
    int64_t  source_off;   ///< Offset of the source field, -1 for none
};

/** @brief Writer state. Records are appended to the open shard; index sections are kept in
 *  memory until the shard closes. */
struct pzpd_writer
{
    char    *manifest_path;              ///< Path of the manifest to write
    char    *base;                       ///< manifest_path without ".pzpd"
    unsigned S;                          ///< Stream count
    char     streams[PZPD_MAX_STREAMS][24]; ///< Stream names
    uint64_t shard_max;                  ///< Shard size limit
    uint32_t align;                      ///< Record alignment
    uint8_t  uuid[16];                   ///< Archive uuid

    // Current record
    int      in_record;                  ///< 1 between begin and end
    char    *key;                        ///< Current key (copy)
    size_t   key_len;                    ///< Current key length
    uint32_t group;                      ///< Current group
    uint32_t frame;                      ///< Current frame
    struct pzpd_wblob blobs[PZPD_MAX_STREAMS]; ///< Current blobs, indexed by stream

    // Open shard
    int      fd;                         ///< Open shard file, -1 if none
    char    *tmp_path;                   ///< "<final>.tmp"
    char    *final_path;                 ///< Final shard path
    unsigned shard_index;                ///< Index of the open shard
    uint64_t cur_off;                    ///< Where the next record goes
    uint64_t shard_first;                ///< Archive ordinal of the open shard's first record
    uint64_t shard_records;              ///< Records in the open shard
    uint32_t shard_last_group;           ///< Group of the last record in the open shard
    struct pzpd_buf rtab;                ///< Record table of the open shard
    struct pzpd_buf btab;                ///< Blob table of the open shard
    struct pzpd_buf hash;                ///< Hash entries of the open shard (unsorted)
    struct pzpd_buf heap;                ///< String heap of the open shard

    // Archive-wide
    uint64_t total_records;              ///< Records written so far
    struct pzpd_wshard *shards;          ///< Completed shards
    unsigned shard_count;                ///< Completed shards
    struct pzpd_buf ghash;               ///< Global hash entries for the manifest (unsorted)
    struct pzpd_buf strings;             ///< Every key and name, for the duplicate check
    struct pzpd_wdedup *dedup;           ///< Duplicate-check table
    uint64_t dedup_cap;                  ///< Slots in dedup (power of two)
    uint64_t dedup_used;                 ///< Occupied slots
    int      broken;                     ///< 1 after a failure that left the index inconsistent: every later call fails
    unsigned T;                          ///< Tables declared
    struct pzpd_wtable *tables;          ///< Tables (PZPD_MAX_TABLES entries, allocated once)
    uint64_t generation;                 ///< Generation written into the shards (0 = 1; shard rewrites use old + 1)
    struct pzpd_buf gtab;                ///< Group table of the open shard (pzpd_disk_group entries)
    struct pzpd_wgroup *groups;          ///< Registered groups (open addressing by id)
    uint64_t groups_cap;                 ///< Slots (power of two, 0 = none)
    uint64_t groups_used;                ///< Registered groups
    uint32_t next_group;                 ///< Next id pzpd_writer_group() tries
    uint32_t last_group;                 ///< Group of the previous record (archive-wide), PZPD_NO_GROUP if none
    uint32_t last_frame;                 ///< Its frame
    int      shard_oversize;             ///< 1 once a group larger than the shard limit started in the open shard
    unsigned W;                          ///< Word indexes declared
    struct pzpd_wwords words[PZPD_MAX_WORD_INDEXES]; ///< Their declarations
};

/** @brief Drop the blobs of the record being assembled. */
static void pzpd_writer_reset_record(pzpd_writer *w)
{
    free(w->key);
    w->key       = NULL;
    w->key_len   = 0;
    w->in_record = 0;
    for (unsigned s = 0; s < PZPD_MAX_STREAMS; s++)
    {
        free(w->blobs[s].name);
        free(w->blobs[s].data);
        memset(&w->blobs[s], 0, sizeof(w->blobs[s]));
    }
    for (unsigned t = 0; (w->tables != NULL) && (t < w->T); t++)
    {
        w->tables[t].cur_rows.len = 0;
        w->tables[t].cur_heap.len = 0;
        w->tables[t].cur_n = 0;
    }
}

/** @brief Look up bytes in the duplicate-check table.
 *  @return 1 if already present, 0 if not. */
static int pzpd_dedup_contains(pzpd_writer *w, uint64_t h, uint8_t kind, const char *s, size_t len)
{
    if (w->dedup_cap == 0) { return 0; }
    uint64_t mask = w->dedup_cap - 1;
    for (uint64_t i = h & mask; ; i = (i + 1) & mask)
    {
        struct pzpd_wdedup *e = &w->dedup[i];
        if (!e->used) { return 0; }
        if ( (e->hash == h) && (e->kind == kind) && (e->len == len) && (memcmp(w->strings.data + e->str, s, len) == 0) ) { return 1; }
    }
}

/** @brief Insert bytes into the duplicate-check table (the caller checked they're new).
 *  @return 1 on success, 0 on allocation failure. */
static int pzpd_dedup_insert(pzpd_writer *w, uint64_t h, uint8_t kind, const char *s, size_t len)
{
    if ( (w->dedup_used + 1) * 2 > w->dedup_cap )
    {
        uint64_t ncap = (w->dedup_cap == 0) ? 65536 : w->dedup_cap * 2;
        struct pzpd_wdedup *nt = (struct pzpd_wdedup *) calloc(ncap, sizeof(struct pzpd_wdedup));
        if (nt == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory growing the duplicate check"); return 0; }
        for (uint64_t i = 0; i < w->dedup_cap; i++)
        {
            if (!w->dedup[i].used) { continue; }
            uint64_t j = w->dedup[i].hash & (ncap - 1);
            while (nt[j].used) { j = (j + 1) & (ncap - 1); }
            nt[j] = w->dedup[i];
        }
        free(w->dedup);
        w->dedup     = nt;
        w->dedup_cap = ncap;
    }
    uint64_t off = w->strings.len;
    if (!pzpd_buf_append(&w->strings, s, len)) { return 0; }
    uint64_t mask = w->dedup_cap - 1;
    uint64_t i = h & mask;
    while (w->dedup[i].used) { i = (i + 1) & mask; }
    w->dedup[i].hash = h;
    w->dedup[i].str  = off;
    w->dedup[i].len  = (uint32_t) len;
    w->dedup[i].kind = kind;
    w->dedup[i].used = 1;
    w->dedup_used++;
    return 1;
}

/** @brief Open the next shard file (<base>.NNNNN.pzpd.tmp).
 *  @return 1 on success, 0 on failure. */
static int pzpd_writer_open_shard(pzpd_writer *w)
{
    size_t n = strlen(w->base) + 32;
    w->final_path = (char *) malloc(n);
    w->tmp_path   = (char *) malloc(n + 4);
    if ( (w->final_path == NULL) || (w->tmp_path == NULL) ) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    snprintf(w->final_path, n, "%s.%05u.pzpd", w->base, w->shard_index);
    snprintf(w->tmp_path, n + 4, "%s.tmp", w->final_path);
    w->fd = open(w->tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (w->fd < 0)
    {
        pzpd_set_error(PZPD_E_IO, "cannot create %s: %s", w->tmp_path, strerror(errno));
        return 0;
    }
    w->cur_off          = PZPD_BLOCK;
    w->shard_first      = w->total_records;
    w->shard_records    = 0;
    w->shard_last_group = PZPD_NO_GROUP;
    w->rtab.len = w->btab.len = w->hash.len = w->heap.len = w->gtab.len = 0;
    w->shard_oversize = 0;
    for (unsigned t = 0; (w->tables != NULL) && (t < w->T); t++)
    {
        w->tables[t].index.len = w->tables[t].rows.len = w->tables[t].heap.len = 0;
        w->tables[t].nrows = 0;
    }
    return 1;
}

/** @brief qsort comparator for shard-local hash entries: (hash, kind, ordinal, stream). */
static int pzpd_cmp_hash(const void *a, const void *b)
{
    const struct pzpd_disk_hash *x = (const struct pzpd_disk_hash *) a;
    const struct pzpd_disk_hash *y = (const struct pzpd_disk_hash *) b;
    if (x->hash != y->hash) { return (x->hash < y->hash) ? -1 : 1; }
    if (x->kind != y->kind) { return (x->kind < y->kind) ? -1 : 1; }
    if (x->local_ordinal != y->local_ordinal) { return (x->local_ordinal < y->local_ordinal) ? -1 : 1; }
    return (x->stream < y->stream) ? -1 : (x->stream > y->stream);
}

/** @brief qsort comparator for global hash entries: (hash, kind, ordinal, stream). */
static int pzpd_cmp_ghash(const void *a, const void *b)
{
    const struct pzpd_disk_global_hash *x = (const struct pzpd_disk_global_hash *) a;
    const struct pzpd_disk_global_hash *y = (const struct pzpd_disk_global_hash *) b;
    if (x->hash != y->hash) { return (x->hash < y->hash) ? -1 : 1; }
    if (x->kind != y->kind) { return (x->kind < y->kind) ? -1 : 1; }
    if (x->ordinal != y->ordinal) { return (x->ordinal < y->ordinal) ? -1 : 1; }
    return (x->stream < y->stream) ? -1 : (x->stream > y->stream);
}

/** @brief Write one index section (header + data) at a 4 KiB boundary.
 *  @param off  In: where the previous section ended; out: where this one ended.
 *  @param data_off Set to the offset of the data (after the 32-byte header).
 *  @param idx  Streaming index checksum, updated with the data (may be NULL).
 *  @return 1 on success, 0 on failure. */
static int pzpd_write_section(int fd, uint64_t *off, uint32_t kind, const void *data, uint64_t bytes, uint64_t *data_off, XXH64_state_t *idx)
{
    uint64_t start = pzpd_align_up(*off, PZPD_BLOCK);
    struct pzpd_disk_section sh;
    memset(&sh, 0, sizeof(sh));
    memcpy(sh.magic, PZPD_MAGIC_SECTION, 8);
    sh.version  = PZPD_FORMAT_VERSION;
    sh.kind     = kind;
    sh.bytes    = bytes;
    sh.checksum = XXH64(data, (size_t) bytes, 0);
    if (!pzpd_pwrite_all(fd, &sh, sizeof(sh), start)) { return 0; }
    if ( (bytes > 0) && !pzpd_pwrite_all(fd, data, (size_t) bytes, start + sizeof(sh)) ) { return 0; }
    if (idx != NULL) { XXH64_update(idx, data, (size_t) bytes); }
    *data_off = start + sizeof(sh);
    *off      = start + sizeof(sh) + bytes;
    return 1;
}

/** @brief Fill the stream table of a superblock or manifest. */
static void pzpd_fill_streams(struct pzpd_disk_stream *slots, unsigned S, char names[][24])
{
    memset(slots, 0, sizeof(struct pzpd_disk_stream) * PZPD_MAX_STREAMS);
    for (unsigned s = 0; s < S; s++) { pzpd_put_slot_name(slots[s].name, names[s]); }
}

/** @brief Checksum of a revision-12 header extension (the `len` bytes before ext_checksum): 0 when they are
 *  all zero, so a header without word indexes is byte-identical to one written before revision 12. */
static uint64_t pzpd_ext_checksum(const void *ext, size_t len)
{
    const unsigned char *p = (const unsigned char *) ext;
    size_t i = 0;
    while ( (i < len) && (p[i] == 0) ) { i++; }
    if (i == len) { return 0; }
    uint64_t h = XXH64(ext, len, 0);
    return (h == 0) ? 1 : h;
}

/** @brief Seal a superblock: sb_checksum over the bytes before it, then the extension's ext_checksum. */
static void pzpd_seal_superblock(struct pzpd_disk_superblock *sb)
{
    sb->sb_checksum  = XXH64(sb, offsetof(struct pzpd_disk_superblock, sb_checksum), 0);
    sb->ext_checksum = pzpd_ext_checksum(sb->words, offsetof(struct pzpd_disk_superblock, ext_checksum) - offsetof(struct pzpd_disk_superblock, words));
}

/** @brief Write the index sections, the superblocks, fsync and rename the open shard.
 *  @return 1 on success, 0 on failure. */
static int pzpd_writer_close_shard(pzpd_writer *w)
{
    struct pzpd_disk_superblock sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, PZPD_MAGIC_SHARD, 8);
    sb.version        = PZPD_FORMAT_VERSION;
    memcpy(sb.archive_uuid, w->uuid, 16);
    sb.generation     = (w->generation != 0) ? w->generation : 1;
    sb.shard_index    = w->shard_index;
    sb.shard_count    = 0;                  // patched by pzpd_writer_finish()
    sb.first_ordinal  = w->shard_first;
    sb.record_count   = w->shard_records;
    sb.total_records  = 0;                  // patched by pzpd_writer_finish()
    sb.stream_count   = w->S;
    sb.align          = w->align;
    pzpd_fill_streams(sb.streams, w->S, w->streams);
    sb.records_offset = PZPD_BLOCK;
    sb.records_bytes  = w->cur_off - PZPD_BLOCK;

    qsort(w->hash.data, w->hash.len / sizeof(struct pzpd_disk_hash), sizeof(struct pzpd_disk_hash), pzpd_cmp_hash);

    // Metadata JSON for recovery tools: stream names and identity, readable without this code
    char meta[4096];
    int mlen = snprintf(meta, sizeof(meta), "{\"pzpd\":%d,\"shard_index\":%u,\"archive_uuid\":\"", PZPD_FORMAT_VERSION, w->shard_index);
    for (int i = 0; i < 16; i++) { mlen += snprintf(meta + mlen, sizeof(meta) - (size_t) mlen, "%02x", w->uuid[i]); }
    mlen += snprintf(meta + mlen, sizeof(meta) - (size_t) mlen, "\",\"first_ordinal\":%llu,\"align\":%u,\"generation\":%llu,\"streams\":[",
                     (unsigned long long) sb.first_ordinal, sb.align, (unsigned long long) sb.generation);
    for (unsigned s = 0; s < w->S; s++) { mlen += snprintf(meta + mlen, sizeof(meta) - (size_t) mlen, "%s\"%s\"", s ? "," : "", w->streams[s]); }
    mlen += snprintf(meta + mlen, sizeof(meta) - (size_t) mlen, "]}\n");

    XXH64_state_t *idx = XXH64_createState();
    if (idx == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    XXH64_reset(idx, 0);
    uint64_t off = w->cur_off;
    int ok = pzpd_write_section(w->fd, &off, PZPD_SECT_RECORDS, w->rtab.data, w->rtab.len, &sb.rtab_offset, idx) &&
             pzpd_write_section(w->fd, &off, PZPD_SECT_BLOBS,   w->btab.data, w->btab.len, &sb.btab_offset, idx) &&
             pzpd_write_section(w->fd, &off, PZPD_SECT_HASH,    w->hash.data, w->hash.len, &sb.hash_offset, idx) &&
             pzpd_write_section(w->fd, &off, PZPD_SECT_HEAP,    w->heap.data, w->heap.len, &sb.heap_offset, idx) &&
             pzpd_write_section(w->fd, &off, PZPD_SECT_META,    meta, (uint64_t) mlen, &sb.meta_offset, idx);
    if (ok && (w->gtab.len > 0))
    {
        ok = pzpd_write_section(w->fd, &off, PZPD_SECT_GROUPS, w->gtab.data, w->gtab.len, &sb.groups_offset, idx);
        sb.group_count = w->gtab.len / sizeof(struct pzpd_disk_group);
        sb.flags |= 1u;
    }
    // Tables: one section each, after the meta section, included in the index checksum in table order
    struct pzpd_buf sec = {0};
    for (unsigned t = 0; ok && (t < w->T); t++)
    {
        struct pzpd_wtable *wt = &w->tables[t];
        int global = (wt->sc.flags & PZPD_TABLE_GLOBAL) != 0;
        if (!global)
        {
            uint32_t last = (uint32_t) wt->nrows;
            ok = pzpd_buf_append(&wt->index, &last, 4);          // records + 1 entries
        }
        ok = ok && pzpd_table_section(&sec, &wt->sc, w->shard_records, global ? NULL : (const uint32_t *) wt->index.data,
                                      global ? wt->g_rows.data : wt->rows.data, global ? wt->g_n : wt->nrows,
                                      global ? wt->g_heap.data : wt->heap.data, global ? wt->g_heap.len : wt->heap.len);
        uint64_t dataOff = 0;
        ok = ok && pzpd_write_section(w->fd, &off, PZPD_SECT_TABLE, sec.data, sec.len, &dataOff, idx);
        pzpd_put_slot_name(sb.tables[t].name, wt->sc.name);
        sb.tables[t].flags          = (uint8_t) wt->sc.flags;
        sb.tables[t].section_offset = dataOff;
        sb.tables[t].section_bytes  = sec.len;
        sb.tables[t].row_stride     = wt->sc.stride;
    }
    pzpd_buf_free(&sec);
    sb.index_checksum = XXH64_digest(idx);
    // Word indexes: built from the tables' rows of this shard, after the tables, with their own checksum
    XXH64_reset(idx, 0);
    struct pzpd_buf wsec = {0};
    for (unsigned k = 0; ok && (k < w->W); k++)
    {
        const struct pzpd_wwords *d = &w->words[k];
        const struct pzpd_wtable *wt = &w->tables[d->t];
        struct pzpd_wsrc src = { w->shard_records, (const uint32_t *) wt->index.data, wt->rows.data, wt->nrows, wt->sc.stride,
                                 (const char *) wt->heap.data, wt->heap.len, d->text_off, d->source_off };
        uint64_t dataOff = 0;
        ok = pzpd_words_build(&wsec, &src, d->source[0] ? d->source : NULL) &&
             pzpd_write_section(w->fd, &off, PZPD_SECT_WORDS, wsec.data, wsec.len, &dataOff, idx);
        snprintf(sb.words[k].table, sizeof(sb.words[k].table), "%s", d->table);
        snprintf(sb.words[k].column, sizeof(sb.words[k].column), "%s", d->column);
        sb.words[k].section_offset = dataOff;
        sb.words[k].section_bytes  = wsec.len;
    }
    pzpd_buf_free(&wsec);
    sb.words_checksum = (w->W > 0) ? XXH64_digest(idx) : 0;
    XXH64_freeState(idx);
    if (!ok) { return 0; }

    sb.hash_count = w->hash.len / sizeof(struct pzpd_disk_hash);
    sb.heap_bytes = w->heap.len;
    sb.meta_bytes = (uint64_t) mlen;
    sb.file_bytes = pzpd_align_up(off, PZPD_BLOCK) + PZPD_BLOCK;
    pzpd_seal_superblock(&sb);

    unsigned char block[PZPD_BLOCK];
    memset(block, 0, sizeof(block));
    memcpy(block, &sb, sizeof(sb));
    // Backup first, then the primary: a crash leaves either no valid primary (file is still .tmp) or both
    if (!pzpd_pwrite_all(w->fd, block, PZPD_BLOCK, sb.file_bytes - PZPD_BLOCK)) { return 0; }
    if (!pzpd_pwrite_all(w->fd, block, PZPD_BLOCK, 0)) { return 0; }
    if (fsync(w->fd) != 0) { pzpd_set_error(PZPD_E_IO, "fsync %s: %s", w->tmp_path, strerror(errno)); return 0; }
    close(w->fd);
    w->fd = -1;
    if (rename(w->tmp_path, w->final_path) != 0)
    {
        pzpd_set_error(PZPD_E_IO, "rename %s -> %s: %s", w->tmp_path, w->final_path, strerror(errno));
        return 0;
    }
    pzpd_fsync_dir_of(w->final_path);

    struct pzpd_wshard *ns = (struct pzpd_wshard *) realloc(w->shards, sizeof(struct pzpd_wshard) * (w->shard_count + 1));
    if (ns == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    w->shards = ns;
    w->shards[w->shard_count].path           = w->final_path;
    w->shards[w->shard_count].first_ordinal  = w->shard_first;
    w->shards[w->shard_count].record_count   = w->shard_records;
    w->shards[w->shard_count].file_bytes     = sb.file_bytes;
    w->shards[w->shard_count].index_checksum = sb.index_checksum;
    w->shard_count++;
    w->final_path = NULL;
    free(w->tmp_path);
    w->tmp_path = NULL;
    w->shard_index++;
    return 1;
}

pzpd_writer *pzpd_writer_create(const char *manifest_path, const pzpd_writer_opts *o)
{
    pzpd_clear_error();
    if ( (manifest_path == NULL) || (o == NULL) || (o->streams == NULL) || (o->stream_count == 0) || (o->stream_count > PZPD_MAX_STREAMS) )
    {
        pzpd_set_error(PZPD_E_ARG, "pzpd_writer_create: need a path and 1..%d stream names", PZPD_MAX_STREAMS);
        return NULL;
    }
    uint32_t align = (o->align == 0) ? PZPD_DEFAULT_ALIGN : o->align;
    if ( (align != 64) && (align != 4096) ) { pzpd_set_error(PZPD_E_ARG, "record alignment must be 64 or 4096, not %u", align); return NULL; }

    pzpd_writer *w = (pzpd_writer *) calloc(1, sizeof(pzpd_writer));
    if (w == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return NULL; }
    w->last_group = PZPD_NO_GROUP;
    w->fd        = -1;
    w->S         = o->stream_count;
    w->align     = align;
    w->shard_max = (o->shard_max_bytes == 0) ? PZPD_DEFAULT_SHARD_BYTES : o->shard_max_bytes;

    for (unsigned s = 0; s < w->S; s++)
    {
        const char *nm = o->streams[s];
        if (!pzpd_check_stream_name(nm)) { pzpd_error_wrap(PZPD_OK, "stream %u", s); free(w); return NULL; }
        for (unsigned t = 0; t < s; t++)
        {
            if (strcmp(w->streams[t], nm) == 0) { pzpd_set_error(PZPD_E_ARG, "stream name \"%s\" given twice", nm); free(w); return NULL; }
        }
        snprintf(w->streams[s], sizeof(w->streams[s]), "%s", nm);
    }

    w->manifest_path = strdup(manifest_path);
    w->base          = strdup(manifest_path);
    if ( (w->manifest_path == NULL) || (w->base == NULL) ) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); pzpd_writer_abort(w); return NULL; }
    size_t bl = strlen(w->base);
    if ( (bl > 5) && (strcmp(w->base + bl - 5, ".pzpd") == 0) ) { w->base[bl - 5] = 0; }

    if (getrandom(w->uuid, sizeof(w->uuid), 0) != (ssize_t) sizeof(w->uuid))
    {
        pzpd_set_error(PZPD_E_IO, "getrandom failed: %s", strerror(errno));
        pzpd_writer_abort(w);
        return NULL;
    }
    w->uuid[6] = (uint8_t)((w->uuid[6] & 0x0F) | 0x40);   // RFC 4122 version 4
    w->uuid[8] = (uint8_t)((w->uuid[8] & 0x3F) | 0x80);   // RFC 4122 variant

    if (!pzpd_writer_open_shard(w)) { pzpd_writer_abort(w); return NULL; }
    return w;
}

/** @brief Registered group of an id, or NULL. */
static struct pzpd_wgroup *pzpd_wgroup_find(pzpd_writer *w, uint32_t id)
{
    if (w->groups_cap == 0) { return NULL; }
    for (uint64_t i = (id * 0x9E3779B1u) & (w->groups_cap - 1); w->groups[i].used; i = (i + 1) & (w->groups_cap - 1))
    {
        if (w->groups[i].id == id) { return &w->groups[i]; }
    }
    return NULL;
}

/** @brief Register group `id` with a name (unique in the archive) and a size hint.
 *  @return 1 on success, 0 on failure (error set). */
static int pzpd_writer_group_as(pzpd_writer *w, uint32_t id, const char *name, size_t len, uint64_t hint)
{
    if (id == PZPD_NO_GROUP) { pzpd_set_error(PZPD_E_ARG, "group id 0xFFFFFFFF is reserved"); return 0; }
    if (pzpd_wgroup_find(w, id) != NULL) { pzpd_set_error(PZPD_E_DUPLICATE, "group %u is registered twice", id); return 0; }
    uint64_t h = 0;
    if (len > 0)
    {
        if (!pzpd_check_name("group name", name, len)) { return 0; }
        h = XXH64(name, len, 0);
        if (pzpd_dedup_contains(w, h, PZPD_KIND_GROUP, name, len)) { pzpd_set_error(PZPD_E_DUPLICATE, "group name \"%.*s\" already exists", (int)(len > 200 ? 200 : len), name); return 0; }
    }
    if ( (w->groups_used + 1) * 2 > w->groups_cap )
    {
        uint64_t nc = (w->groups_cap == 0) ? 1024 : w->groups_cap * 2;
        struct pzpd_wgroup *ng = (struct pzpd_wgroup *) calloc(nc, sizeof(struct pzpd_wgroup));
        if (ng == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
        for (uint64_t i = 0; i < w->groups_cap; i++)
        {
            if (!w->groups[i].used) { continue; }
            uint64_t j = (w->groups[i].id * 0x9E3779B1u) & (nc - 1);
            while (ng[j].used) { j = (j + 1) & (nc - 1); }
            ng[j] = w->groups[i];
        }
        free(w->groups);
        w->groups = ng;
        w->groups_cap = nc;
    }
    uint64_t off = w->strings.len;
    if ( (len > 0) && !pzpd_dedup_insert(w, h, PZPD_KIND_GROUP, name, len) ) { return 0; }
    uint64_t i = (id * 0x9E3779B1u) & (w->groups_cap - 1);
    while (w->groups[i].used) { i = (i + 1) & (w->groups_cap - 1); }
    w->groups[i].id = id; w->groups[i].used = 1; w->groups[i].name = off; w->groups[i].name_len = (uint32_t) len; w->groups[i].hint = hint;
    w->groups_used++;
    return 1;
}

int64_t pzpd_writer_group(pzpd_writer *w, const char *name, size_t len, uint64_t bytes_hint)
{
    pzpd_clear_error();
    if (w == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL writer"); return -1; }
    if ( (name == NULL) && (len > 0) ) { pzpd_set_error(PZPD_E_ARG, "NULL name"); return -1; }
    while ( (pzpd_wgroup_find(w, w->next_group) != NULL) && (w->next_group != PZPD_NO_GROUP) ) { w->next_group++; }
    if (w->next_group == PZPD_NO_GROUP) { pzpd_set_error(PZPD_E_ARG, "out of group ids"); return -1; }
    uint32_t id = w->next_group;
    if (!pzpd_writer_group_as(w, id, name, len, bytes_hint)) { return -1; }
    w->next_group++;
    return (int64_t) id;
}

int pzpd_writer_begin(pzpd_writer *w, const char *key, size_t key_len, uint32_t group, uint32_t frame)
{
    pzpd_clear_error();
    if (w == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL writer"); return 0; }
    if (w->in_record) { pzpd_set_error(PZPD_E_STATE, "pzpd_writer_begin called twice without pzpd_writer_end"); return 0; }
    if (!pzpd_check_name("record key", key, key_len)) { return 0; }
    w->key = (char *) malloc(key_len);
    if (w->key == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    memcpy(w->key, key, key_len);
    w->key_len   = key_len;
    w->group     = group;
    w->frame     = (group == PZPD_NO_GROUP) ? 0 : frame;
    w->in_record = 1;
    return 1;
}

/** @brief Common part of the blob-adding functions. Takes ownership of data (malloc'd).
 *  @return 1 on success, 0 on failure (data is freed). */
static int pzpd_writer_add(pzpd_writer *w, unsigned stream, const char *name, size_t name_len,
                           unsigned char *data, size_t size, const pzpd_blob_meta *meta)
{
    if ( !w->in_record ) { pzpd_set_error(PZPD_E_STATE, "blob added outside pzpd_writer_begin / pzpd_writer_end"); free(data); return 0; }
    if (stream >= w->S) { pzpd_set_error(PZPD_E_ARG, "stream %u out of range (%u streams)", stream, w->S); free(data); return 0; }
    if (w->blobs[stream].present)
    {
        pzpd_set_error(PZPD_E_DUPLICATE, "record \"%.*s\" already has a blob in stream \"%s\"", (int)(w->key_len > 200 ? 200 : w->key_len), w->key, w->streams[stream]);
        free(data);
        return 0;
    }
    if (!pzpd_check_name("blob name", name, name_len)) { free(data); return 0; }
    if (size > 0xFFFFFFFFull - PZPD_BLOCK) { pzpd_set_error(PZPD_E_ARG, "blob of %zu bytes exceeds the 4 GiB limit", size); free(data); return 0; }

    struct pzpd_wblob *b = &w->blobs[stream];
    b->name = (char *) malloc(name_len);
    if (b->name == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); free(data); return 0; }
    memcpy(b->name, name, name_len);
    b->name_len = name_len;
    b->data     = data;
    b->size     = size;
    if (meta != NULL)
    {
        b->meta = *meta;
        b->meta.meta_flags |= PZPD_META_VALID | PZPD_META_USER;
    }
    else
    {
        pzpd_detect_format(data, size, name, name_len, &b->meta);
    }
    b->present = 1;
    return 1;
}

int pzpd_writer_blob(pzpd_writer *w, unsigned stream, const char *name, size_t name_len, const void *data, size_t size)
{
    return pzpd_writer_blob_ex(w, stream, name, name_len, data, size, NULL);
}

int pzpd_writer_blob_ex(pzpd_writer *w, unsigned stream, const char *name, size_t name_len,
                        const void *data, size_t size, const pzpd_blob_meta *meta)
{
    pzpd_clear_error();
    if (w == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL writer"); return 0; }
    if ( (data == NULL) && (size > 0) ) { pzpd_set_error(PZPD_E_ARG, "NULL data"); return 0; }
    unsigned char *copy = (unsigned char *) malloc(size > 0 ? size : 1);
    if (copy == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory copying a %zu byte blob", size); return 0; }
    if (size > 0) { memcpy(copy, data, size); }
    return pzpd_writer_add(w, stream, name, name_len, copy, size, meta);
}

int pzpd_writer_blob_file(pzpd_writer *w, unsigned stream, const char *name, size_t name_len, const char *src_path)
{
    pzpd_clear_error();
    if ( (w == NULL) || (src_path == NULL) ) { pzpd_set_error(PZPD_E_ARG, "NULL argument"); return 0; }
    int fd = open(src_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { pzpd_set_error(PZPD_E_IO, "cannot open %s: %s", src_path, strerror(errno)); return 0; }
    struct stat st;
    if (fstat(fd, &st) != 0) { pzpd_set_error(PZPD_E_IO, "cannot stat %s: %s", src_path, strerror(errno)); close(fd); return 0; }
    if (!S_ISREG(st.st_mode)) { pzpd_set_error(PZPD_E_ARG, "%s is not a regular file", src_path); close(fd); return 0; }
    if ((uint64_t) st.st_size > 0xFFFFFFFFull - PZPD_BLOCK) { pzpd_set_error(PZPD_E_ARG, "%s: %llu bytes exceed the 4 GiB blob limit", src_path, (unsigned long long) st.st_size); close(fd); return 0; }   // before reading it all
    size_t size = (size_t) st.st_size;
    unsigned char *data = (unsigned char *) malloc(size > 0 ? size : 1);
    if (data == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory reading %s (%zu bytes)", src_path, size); close(fd); return 0; }
    if ( (size > 0) && !pzpd_pread_all(fd, data, size, 0) )
    {
        pzpd_error_wrap(PZPD_E_IO, "%s", src_path);
        free(data);
        close(fd);
        return 0;
    }
    close(fd);
    return pzpd_writer_add(w, stream, name, name_len, data, size, NULL);
}

int pzpd_writer_table(pzpd_writer *w, const char *name, const char *schema, unsigned flags)
{
    pzpd_clear_error();
    if (w == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL writer"); return -1; }
    if ( w->in_record || (w->total_records > 0) || (w->shard_index > 0) ) { pzpd_set_error(PZPD_E_STATE, "tables must be declared before the first record"); return -1; }
    if (w->T >= PZPD_MAX_TABLES) { pzpd_set_error(PZPD_E_ARG, "more than %d tables", PZPD_MAX_TABLES); return -1; }
    if (w->tables == NULL)
    {
        w->tables = (struct pzpd_wtable *) calloc(PZPD_MAX_TABLES, sizeof(struct pzpd_wtable));
        if (w->tables == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return -1; }
    }
    struct pzpd_wtable *wt = &w->tables[w->T];
    if (!pzpd_schema_parse(name, schema, flags, &wt->sc)) { return -1; }
    for (unsigned t = 0; t < w->T; t++) { if (!strcmp(w->tables[t].sc.name, name)) { pzpd_set_error(PZPD_E_ARG, "table \"%s\" declared twice", name); return -1; } }
    for (unsigned st = 0; st < w->S; st++) { if (!strcmp(w->streams[st], name)) { pzpd_set_error(PZPD_E_ARG, "\"%s\" is already a stream name", name); return -1; } }
    if ( !strcmp(name, PZPD_SYNONYMS_TABLE) && !pzpd_synonyms_check(&wt->sc, NULL, 0, NULL, 0) ) { return -1; }
    pzpd_schema_publish(&wt->sc);
    return (int)(w->T++);
}

int pzpd_writer_words(pzpd_writer *w, const char *table, const char *column, const char *source_column)
{
    pzpd_clear_error();
    if ( (w == NULL) || (table == NULL) || (column == NULL) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    if ( w->in_record || (w->total_records > 0) || (w->shard_index > 0) ) { pzpd_set_error(PZPD_E_STATE, "word indexes must be declared before the first record"); return 0; }
    if (w->W >= PZPD_MAX_WORD_INDEXES) { pzpd_set_error(PZPD_E_ARG, "more than %d word indexes", PZPD_MAX_WORD_INDEXES); return 0; }
    int t = -1;
    for (unsigned k = 0; k < w->T; k++) { if (!strcmp(w->tables[k].sc.name, table)) { t = (int) k; } }
    if (t < 0) { pzpd_set_error(PZPD_E_NOTFOUND, "word index: no table %s (declare it first)", table); return 0; }
    const struct pzpd_tschema *sc = &w->tables[t].sc;
    if (sc->flags & PZPD_TABLE_GLOBAL) { pzpd_set_error(PZPD_E_ARG, "word index: %s is a global table", table); return 0; }
    int ct = -1, cs = -1;
    for (unsigned c = 0; c < sc->ncols; c++)
    {
        if (!strcmp(sc->colname[c], column)) { ct = (int) c; }
        if ( (source_column != NULL) && !strcmp(sc->colname[c], source_column) ) { cs = (int) c; }
    }
    if ( (ct < 0) || (sc->type[ct] != PZPD_TYPE_STR) || (sc->count[ct] != 1) ) { pzpd_set_error(PZPD_E_ARG, "word index: %s.%s is not a str column", table, column); return 0; }
    if ( (source_column != NULL) && ((cs < 0) || (sc->type[cs] != PZPD_TYPE_STR) || (sc->count[cs] != 1) || (cs == ct)) )
        { pzpd_set_error(PZPD_E_ARG, "word index: source column %s.%s is not another str column", table, source_column); return 0; }
    for (unsigned k = 0; k < w->W; k++)
    {
        if ( !strcmp(w->words[k].table, table) && !strcmp(w->words[k].column, column) ) { pzpd_set_error(PZPD_E_DUPLICATE, "word index %s.%s declared twice", table, column); return 0; }
    }
    struct pzpd_wwords *d = &w->words[w->W];
    memset(d, 0, sizeof(*d));
    snprintf(d->table, sizeof(d->table), "%s", table);
    snprintf(d->column, sizeof(d->column), "%s", column);
    if (source_column != NULL) { snprintf(d->source, sizeof(d->source), "%s", source_column); }
    d->t          = (unsigned) t;
    d->text_off   = sc->offset[ct];
    d->source_off = (cs >= 0) ? (int64_t) sc->offset[cs] : -1;
    w->W++;
    return 1;
}

/** @brief Validate and stage binary rows (str offsets relative to `strings`) into rows / heap.
 *  @return 1 on success, 0 on failure (error set). */
static int pzpd_stage_rows(const struct pzpd_tschema *sc, const void *rows, uint32_t nrows, const void *strings, size_t strings_len,
                           struct pzpd_buf *outRows, struct pzpd_buf *outHeap)
{
    if ( (rows == NULL) && (nrows > 0) ) { pzpd_set_error(PZPD_E_ARG, "NULL rows"); return 0; }
    for (uint32_t r = 0; r < nrows; r++)
    {
        size_t base = outRows->len;
        if (!pzpd_buf_append(outRows, (const unsigned char *) rows + (size_t) r * sc->stride, sc->stride)) { return 0; }
        if (!sc->has_str) { continue; }
        for (unsigned c = 0; c < sc->ncols; c++)
        {
            if (sc->type[c] != PZPD_TYPE_STR) { continue; }
            for (unsigned k = 0; k < sc->count[c]; k++)
            {
                unsigned char *f = outRows->data + base + sc->offset[c] + k * 8;
                pzpd_str sv;
                memcpy(&sv, f, 8);
                if ( (sv.len > 0) && ( (strings == NULL) || !pzpd_in_file(sv.offset, sv.len, strings_len) ) )
                    { pzpd_set_error(PZPD_E_ARG, "table %s, row %u, column %s: string outside the strings buffer", sc->name, r + 1, sc->colname[c]); return 0; }
                if (outHeap->len + sv.len > 0xFFFFFFFFull) { pzpd_set_error(PZPD_E_ARG, "table %s: strings exceed 4 GiB", sc->name); return 0; }
                pzpd_str nv = { (uint32_t) outHeap->len, sv.len };
                if ( (sv.len > 0) && !pzpd_buf_append(outHeap, (const char *) strings + sv.offset, sv.len) ) { return 0; }
                memcpy(f, &nv, 8);
            }
        }
    }
    return 1;
}

/** @brief Common checks for the row-adding functions.
 *  @return The table, or NULL (error set). */
static struct pzpd_wtable *pzpd_writer_table_for(pzpd_writer *w, unsigned table, int global)
{
    if (w == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL writer"); return NULL; }
    if (table >= w->T) { pzpd_set_error(PZPD_E_ARG, "table %u out of range (%u tables)", table, w->T); return NULL; }
    struct pzpd_wtable *wt = &w->tables[table];
    int isGlobal = (wt->sc.flags & PZPD_TABLE_GLOBAL) != 0;
    if (isGlobal != global) { pzpd_set_error(PZPD_E_ARG, "table %s is a %s table", wt->sc.name, isGlobal ? "global" : "record"); return NULL; }
    if (global && ( w->in_record || (w->total_records > 0) || (w->shard_index > 0) )) { pzpd_set_error(PZPD_E_STATE, "global rows must be added before the first record"); return NULL; }
    if (!global && !w->in_record) { pzpd_set_error(PZPD_E_STATE, "record rows added outside pzpd_writer_begin / pzpd_writer_end"); return NULL; }
    return wt;
}

int pzpd_writer_rows(pzpd_writer *w, unsigned table, const void *rows, uint32_t nrows, const void *strings, size_t strings_len)
{
    pzpd_clear_error();
    struct pzpd_wtable *wt = pzpd_writer_table_for(w, table, 0);
    if (wt == NULL) { return 0; }
    size_t r0 = wt->cur_rows.len, h0 = wt->cur_heap.len;
    if ( ((uint64_t) wt->cur_n + nrows > 0xFFFFFFFFull) || !pzpd_stage_rows(&wt->sc, rows, nrows, strings, strings_len, &wt->cur_rows, &wt->cur_heap) )
        { wt->cur_rows.len = r0; wt->cur_heap.len = h0; if (pzpd_errorCode == PZPD_OK) { pzpd_set_error(PZPD_E_ARG, "too many rows"); } return 0; }
    wt->cur_n += nrows;
    return 1;
}

int pzpd_writer_rows_csv(pzpd_writer *w, unsigned table, const char *csv, size_t len)
{
    pzpd_clear_error();
    struct pzpd_wtable *wt = pzpd_writer_table_for(w, table, 0);
    if (wt == NULL) { return 0; }
    size_t r0 = wt->cur_rows.len, h0 = wt->cur_heap.len;
    int64_t n = pzpd_csv_parse(&wt->sc, csv, len, &wt->cur_rows, &wt->cur_heap);
    if ( (n >= 0) && ((uint64_t) wt->cur_n + (uint64_t) n > 0xFFFFFFFFull) ) { pzpd_set_error(PZPD_E_ARG, "table %s: more than 4 G rows", wt->sc.name); n = -1; }
    if (n < 0) { wt->cur_rows.len = r0; wt->cur_heap.len = h0; return 0; }
    wt->cur_n += (uint32_t) n;
    return 1;
}

int pzpd_writer_global_rows(pzpd_writer *w, unsigned table, const void *rows, uint32_t nrows, const void *strings, size_t strings_len)
{
    pzpd_clear_error();
    struct pzpd_wtable *wt = pzpd_writer_table_for(w, table, 1);
    if (wt == NULL) { return 0; }
    size_t r0 = wt->g_rows.len, h0 = wt->g_heap.len;
    if (wt->g_n + nrows > 0xFFFFFFFFull) { pzpd_set_error(PZPD_E_ARG, "table %s: more than 4 G rows", wt->sc.name); return 0; }   // the reader's limit
    if ( !pzpd_stage_rows(&wt->sc, rows, nrows, strings, strings_len, &wt->g_rows, &wt->g_heap) ||
         ( !strcmp(wt->sc.name, PZPD_SYNONYMS_TABLE) && !pzpd_synonyms_check(&wt->sc, wt->g_rows.data, wt->g_n + nrows, (const char *) wt->g_heap.data, wt->g_heap.len) ) )
        { wt->g_rows.len = r0; wt->g_heap.len = h0; return 0; }
    wt->g_n += nrows;
    return 1;
}

int pzpd_writer_global_rows_csv(pzpd_writer *w, unsigned table, const char *csv, size_t len)
{
    pzpd_clear_error();
    struct pzpd_wtable *wt = pzpd_writer_table_for(w, table, 1);
    if (wt == NULL) { return 0; }
    size_t r0 = wt->g_rows.len, h0 = wt->g_heap.len;
    int64_t n = pzpd_csv_parse(&wt->sc, csv, len, &wt->g_rows, &wt->g_heap);
    if ( (n >= 0) && (wt->g_n + (uint64_t) n > 0xFFFFFFFFull) ) { pzpd_set_error(PZPD_E_ARG, "table %s: more than 4 G rows", wt->sc.name); n = -1; }   // the reader's limit
    if ( (n >= 0) && !strcmp(wt->sc.name, PZPD_SYNONYMS_TABLE) && !pzpd_synonyms_check(&wt->sc, wt->g_rows.data, wt->g_n + (uint64_t) n, (const char *) wt->g_heap.data, wt->g_heap.len) ) { n = -1; }
    if (n < 0) { wt->g_rows.len = r0; wt->g_heap.len = h0; return 0; }
    wt->g_n += (uint64_t) n;
    return 1;
}

int pzpd_writer_end(pzpd_writer *w)
{
    pzpd_clear_error();
    if (w == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL writer"); return 0; }
    if (w->broken) { pzpd_set_error(PZPD_E_STATE, "the writer failed earlier and can't continue"); pzpd_writer_reset_record(w); return 0; }
    if (!w->in_record) { pzpd_set_error(PZPD_E_STATE, "pzpd_writer_end without pzpd_writer_begin"); return 0; }

    //----------------------------------------------------------------------
    // Uniqueness first, so a rejected record leaves nothing behind
    //----------------------------------------------------------------------
    unsigned blobCount = 0;
    size_t   namesBytes = 0;
    uint64_t keyHash = XXH64(w->key, w->key_len, 0);
    if (pzpd_dedup_contains(w, keyHash, PZPD_KIND_KEY, w->key, w->key_len))
    {
        pzpd_set_error(PZPD_E_DUPLICATE, "record key \"%.*s\" already exists", (int)(w->key_len > 200 ? 200 : w->key_len), w->key);
        pzpd_writer_reset_record(w);
        return 0;
    }
    uint64_t nameHash[PZPD_MAX_STREAMS];
    for (unsigned s = 0; s < w->S; s++)
    {
        struct pzpd_wblob *b = &w->blobs[s];
        if (!b->present) { continue; }
        nameHash[s] = XXH64(b->name, b->name_len, 0);
        int dupInRecord = 0;
        for (unsigned t = 0; t < s; t++)
        {
            if ( w->blobs[t].present && (w->blobs[t].name_len == b->name_len) && (memcmp(w->blobs[t].name, b->name, b->name_len) == 0) ) { dupInRecord = 1; }
        }
        if ( dupInRecord || pzpd_dedup_contains(w, nameHash[s], PZPD_KIND_NAME, b->name, b->name_len) )
        {
            pzpd_set_error(PZPD_E_DUPLICATE, "blob name \"%.*s\" already exists", (int)(b->name_len > 200 ? 200 : b->name_len), b->name);
            pzpd_writer_reset_record(w);
            return 0;
        }
        blobCount++;
        namesBytes += b->name_len;
    }
    if (blobCount == 0)
    {
        pzpd_set_error(PZPD_E_ARG, "record \"%.*s\" has no blobs", (int)(w->key_len > 200 ? 200 : w->key_len), w->key);
        pzpd_writer_reset_record(w);
        return 0;
    }
    // Groups: a group's records are consecutive, with increasing frames
    int newRun = (w->group != PZPD_NO_GROUP) && (w->group != w->last_group);
    if ( (w->group != PZPD_NO_GROUP) && !newRun && (w->frame <= w->last_frame) )
    {
        pzpd_set_error(PZPD_E_ARG, "record \"%.*s\": frame %u after frame %u of group %u (frames must increase)", (int)(w->key_len > 200 ? 200 : w->key_len), w->key, w->frame, w->last_frame, w->group);
        pzpd_writer_reset_record(w);
        return 0;
    }
    if ( newRun && pzpd_dedup_contains(w, XXH64(&w->group, 4, 0), PZPD_KIND_GROUPID, (const char *) &w->group, 4) )
    {
        pzpd_set_error(PZPD_E_ARG, "record \"%.*s\": group %u was already closed (a group's records must be consecutive)", (int)(w->key_len > 200 ? 200 : w->key_len), w->key, w->group);
        pzpd_writer_reset_record(w);
        return 0;
    }

    //----------------------------------------------------------------------
    // Layout: header, descriptors, key, names, pad to 64; payloads at 64-byte
    // boundaries in stream order; the record padded to the alignment
    //----------------------------------------------------------------------
    // Copies of the non-bulk record-table rows (for salvage), after the names, 8-byte aligned
    uint64_t copyStart = pzpd_align_up(sizeof(struct pzpd_disk_record_header) + (uint64_t) blobCount * sizeof(struct pzpd_disk_record_blob) + w->key_len + namesBytes, 8);
    uint64_t copyBytes = 0;
    for (unsigned t = 0; t < w->T; t++)
    {
        struct pzpd_wtable *wt = &w->tables[t];
        if ( (wt->cur_n == 0) || (wt->sc.flags & (PZPD_TABLE_BULK | PZPD_TABLE_GLOBAL)) ) { continue; }
        copyBytes += pzpd_align_up(sizeof(struct pzpd_disk_row_copy) + wt->cur_rows.len + wt->cur_heap.len, 8);
    }
    uint64_t headerBytes = pzpd_align_up(copyStart + copyBytes, PZPD_BLOB_ALIGN);
    uint64_t rel[PZPD_MAX_STREAMS];
    uint64_t end = headerBytes;
    for (unsigned s = 0; s < w->S; s++)
    {
        if (!w->blobs[s].present) { continue; }
        rel[s] = pzpd_align_up(end, PZPD_BLOB_ALIGN);
        end    = rel[s] + w->blobs[s].size;
    }
    uint64_t recordBytes = pzpd_align_up(end, w->align);
    if (recordBytes > 0xFFFFFFFFull)
    {
        pzpd_set_error(PZPD_E_ARG, "record \"%.*s\" is %llu bytes, the limit is 4 GiB", (int)(w->key_len > 200 ? 200 : w->key_len), w->key, (unsigned long long) recordBytes);
        pzpd_writer_reset_record(w);
        return 0;
    }

    //----------------------------------------------------------------------
    // Shard cut: never inside a video group (the group may overshoot the limit)
    //----------------------------------------------------------------------
    // A new group starts a new shard early when its size hint doesn't fit; a group larger than the limit
    // gets a shard of its own (cut before it, and before the first record after it)
    int sameGroup = (w->group != PZPD_NO_GROUP) && (w->group == w->shard_last_group);
    struct pzpd_wgroup *wg = (w->group != PZPD_NO_GROUP) ? pzpd_wgroup_find(w, w->group) : NULL;
    uint64_t hint = (newRun && (wg != NULL)) ? wg->hint : 0;
    if ( (w->shard_records > 0) && !sameGroup &&
         ( (w->cur_off + recordBytes > w->shard_max) || (newRun && (w->cur_off + hint > w->shard_max)) || w->shard_oversize ) )
    {
        if (!pzpd_writer_close_shard(w) || !pzpd_writer_open_shard(w)) { w->broken = 1; pzpd_writer_reset_record(w); return 0; }
    }
    if ( newRun && (PZPD_BLOCK + hint > w->shard_max) ) { w->shard_oversize = 1; }

    //----------------------------------------------------------------------
    // Record header
    //----------------------------------------------------------------------
    unsigned char *hdr = (unsigned char *) calloc(1, (size_t) headerBytes);
    if (hdr == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); pzpd_writer_reset_record(w); return 0; }
    struct pzpd_disk_record_header rh;
    memset(&rh, 0, sizeof(rh));
    memcpy(rh.magic, PZPD_MAGIC_RECORD, 8);
    rh.version      = PZPD_FORMAT_VERSION;
    rh.header_bytes = (uint32_t) headerBytes;
    rh.record_bytes = (uint32_t) recordBytes;
    rh.table_bytes  = (uint32_t) copyBytes;
    rh.group        = w->group;
    rh.frame        = w->frame;
    rh.key_len      = (uint16_t) w->key_len;
    rh.blob_count   = (uint8_t) blobCount;
    size_t p = sizeof(rh);
    unsigned char *names = hdr + sizeof(rh) + (size_t) blobCount * sizeof(struct pzpd_disk_record_blob) + w->key_len;
    memcpy(hdr + sizeof(rh) + (size_t) blobCount * sizeof(struct pzpd_disk_record_blob), w->key, w->key_len);
    size_t np = 0;
    for (unsigned s = 0; s < w->S; s++)
    {
        struct pzpd_wblob *b = &w->blobs[s];
        if (!b->present) { continue; }
        struct pzpd_disk_record_blob d;
        memset(&d, 0, sizeof(d));
        d.stream     = (uint8_t) s;
        d.meta_flags = b->meta.meta_flags;
        d.bits       = b->meta.bits;
        d.format     = b->meta.format;
        d.rel_offset = (uint32_t) rel[s];
        d.size       = (uint32_t) b->size;
        d.width      = b->meta.width;
        d.height     = b->meta.height;
        d.channels   = b->meta.channels;
        d.frames     = b->meta.frames;
        d.name_len   = (uint16_t) b->name_len;
        d.xxh32      = XXH32(b->data, b->size, 0);
        memcpy(hdr + p, &d, sizeof(d));
        p += sizeof(d);
        memcpy(names + np, b->name, b->name_len);
        np += b->name_len;
    }
    size_t cp = (size_t) copyStart;
    for (unsigned t = 0; t < w->T; t++)
    {
        struct pzpd_wtable *wt = &w->tables[t];
        if ( (wt->cur_n == 0) || (wt->sc.flags & (PZPD_TABLE_BULK | PZPD_TABLE_GLOBAL)) ) { continue; }
        struct pzpd_disk_row_copy rc = { t, wt->cur_n, (uint32_t) wt->cur_rows.len, (uint32_t) wt->cur_heap.len };
        memcpy(hdr + cp, &rc, sizeof(rc));
        memcpy(hdr + cp + sizeof(rc), wt->cur_rows.data, wt->cur_rows.len);
        if (wt->cur_heap.len > 0) { memcpy(hdr + cp + sizeof(rc) + wt->cur_rows.len, wt->cur_heap.data, wt->cur_heap.len); }
        cp += (size_t) pzpd_align_up(sizeof(rc) + wt->cur_rows.len + wt->cur_heap.len, 8);
    }
    memcpy(hdr, &rh, sizeof(rh));
    rh.checksum = XXH32(hdr, (size_t) headerBytes, 0);        // computed with the checksum field still 0
    memcpy(hdr + offsetof(struct pzpd_disk_record_header, checksum), &rh.checksum, sizeof(rh.checksum));

    //----------------------------------------------------------------------
    // Write header and payloads (gaps are file holes, read back as zeros)
    //----------------------------------------------------------------------
    int ok = pzpd_pwrite_all(w->fd, hdr, (size_t) headerBytes, w->cur_off);
    free(hdr);
    for (unsigned s = 0; ok && (s < w->S); s++)
    {
        if ( !w->blobs[s].present || (w->blobs[s].size == 0) ) { continue; }
        ok = pzpd_pwrite_all(w->fd, w->blobs[s].data, w->blobs[s].size, w->cur_off + rel[s]);
    }
    if (ok && (end < recordBytes))
    {
        // Make sure the file extends to the end of the record even for the last record
        unsigned char zero = 0;
        ok = pzpd_pwrite_all(w->fd, &zero, 1, w->cur_off + recordBytes - 1);
    }
    if (!ok) { pzpd_writer_reset_record(w); return 0; }

    //----------------------------------------------------------------------
    // Index entries (shard) and global hash + duplicate check (archive)
    //----------------------------------------------------------------------
    uint32_t local = (uint32_t) w->shard_records;
    uint64_t ordinal = w->total_records;
    struct pzpd_disk_record re;
    memset(&re, 0, sizeof(re));
    re.offset     = w->cur_off;
    re.bytes      = (uint32_t) recordBytes;
    re.key_offset = (uint32_t) w->heap.len;
    re.key_len    = (uint16_t) w->key_len;
    re.group      = w->group;
    re.frame      = w->frame;
    re.checksum   = rh.checksum;
    if (w->heap.len + w->key_len + namesBytes > 0xFFFFFFFFull) { pzpd_set_error(PZPD_E_ARG, "shard string heap exceeds 4 GiB"); pzpd_writer_reset_record(w); return 0; }
    ok = pzpd_buf_append(&w->rtab, &re, sizeof(re)) && pzpd_buf_append(&w->heap, w->key, w->key_len);

    struct pzpd_disk_hash he;
    memset(&he, 0, sizeof(he));
    he.hash = keyHash; he.local_ordinal = local; he.stream = 0xFF; he.kind = PZPD_KIND_KEY;
    struct pzpd_disk_global_hash ge;
    memset(&ge, 0, sizeof(ge));
    ge.hash = keyHash; ge.ordinal = ordinal; ge.stream = 0xFF; ge.kind = PZPD_KIND_KEY;
    ok = ok && pzpd_buf_append(&w->hash, &he, sizeof(he)) && pzpd_buf_append(&w->ghash, &ge, sizeof(ge)) &&
         pzpd_dedup_insert(w, keyHash, PZPD_KIND_KEY, w->key, w->key_len);

    // Group table: a new group (groups never span shards) or one more frame of the current one
    if ( ok && (w->group != PZPD_NO_GROUP) )
    {
        if (w->group == w->shard_last_group)
        {
            struct pzpd_disk_group *g = (struct pzpd_disk_group *)(w->gtab.data + w->gtab.len - sizeof(struct pzpd_disk_group));
            g->frame_count++;
        }
        else
        {
            struct pzpd_disk_group g;
            memset(&g, 0, sizeof(g));
            g.group_id    = w->group;
            g.first_local = (uint32_t) local;
            g.frame_count = 1;
            g.name_offset = (uint32_t) w->heap.len;
            if ( (wg != NULL) && (wg->name_len > 0) )
            {
                const char *gn = (const char *) w->strings.data + wg->name;
                uint64_t gh = XXH64(gn, wg->name_len, 0);
                g.name_len = wg->name_len;
                struct pzpd_disk_hash ghe;
                memset(&ghe, 0, sizeof(ghe));
                ghe.hash = gh; ghe.local_ordinal = (uint32_t) local; ghe.stream = 0xFF; ghe.kind = PZPD_KIND_GROUP;
                struct pzpd_disk_global_hash gge;
                memset(&gge, 0, sizeof(gge));
                gge.hash = gh; gge.ordinal = ordinal; gge.stream = 0xFF; gge.kind = PZPD_KIND_GROUP;
                ok = pzpd_buf_append(&w->heap, gn, wg->name_len) && pzpd_buf_append(&w->hash, &ghe, sizeof(ghe)) && pzpd_buf_append(&w->ghash, &gge, sizeof(gge));
            }
            ok = ok && pzpd_buf_append(&w->gtab, &g, sizeof(g));
            if ( ok && (w->group != w->last_group) ) { ok = pzpd_dedup_insert(w, XXH64(&w->group, 4, 0), PZPD_KIND_GROUPID, (const char *) &w->group, 4); }
        }
    }

    for (unsigned s = 0; ok && (s < w->S); s++)
    {
        struct pzpd_wblob *b = &w->blobs[s];
        struct pzpd_disk_blob be;
        memset(&be, 0, sizeof(be));
        if (!b->present)
        {
            be.rel_offset = PZPD_MISSING;
            ok = pzpd_buf_append(&w->btab, &be, sizeof(be));
            continue;
        }
        be.rel_offset  = (uint32_t) rel[s];
        be.size        = (uint32_t) b->size;
        be.name_offset = (uint32_t) w->heap.len;
        be.name_len    = (uint16_t) b->name_len;
        be.meta_flags  = b->meta.meta_flags;
        be.bits        = b->meta.bits;
        be.format      = b->meta.format;
        be.width       = b->meta.width;
        be.height      = b->meta.height;
        be.channels    = b->meta.channels;
        be.frames      = b->meta.frames;
        he.hash = nameHash[s]; he.stream = (uint8_t) s; he.kind = PZPD_KIND_NAME;
        ge.hash = nameHash[s]; ge.stream = (uint8_t) s; ge.kind = PZPD_KIND_NAME;
        ok = pzpd_buf_append(&w->btab, &be, sizeof(be)) && pzpd_buf_append(&w->heap, b->name, b->name_len) &&
             pzpd_buf_append(&w->hash, &he, sizeof(he)) && pzpd_buf_append(&w->ghash, &ge, sizeof(ge)) &&
             pzpd_dedup_insert(w, nameHash[s], PZPD_KIND_NAME, b->name, b->name_len);
    }
    // Table rows: row start of this record, rows (str offsets rebased into the shard heap), strings
    for (unsigned t = 0; ok && (t < w->T); t++)
    {
        struct pzpd_wtable *wt = &w->tables[t];
        if (wt->sc.flags & PZPD_TABLE_GLOBAL) { continue; }
        uint32_t start = (uint32_t) wt->nrows;
        if ( (wt->nrows + wt->cur_n > 0xFFFFFFFFull) || (wt->heap.len + wt->cur_heap.len > 0xFFFFFFFFull) )
            { pzpd_set_error(PZPD_E_ARG, "table %s: more than 4 G rows or 4 GiB of strings in one shard", wt->sc.name); ok = 0; break; }
        ok = pzpd_buf_append(&wt->index, &start, 4);
        if (ok && wt->sc.has_str)
        {
            for (uint32_t r = 0; r < wt->cur_n; r++)
            {
                unsigned char *row = wt->cur_rows.data + (size_t) r * wt->sc.stride;
                for (unsigned c = 0; c < wt->sc.ncols; c++)
                {
                    if (wt->sc.type[c] != PZPD_TYPE_STR) { continue; }
                    for (unsigned k = 0; k < wt->sc.count[c]; k++)
                    {
                        pzpd_str sv;
                        memcpy(&sv, row + wt->sc.offset[c] + k * 8, 8);
                        sv.offset += (uint32_t) wt->heap.len;
                        memcpy(row + wt->sc.offset[c] + k * 8, &sv, 8);
                    }
                }
            }
        }
        ok = ok && pzpd_buf_append(&wt->rows, wt->cur_rows.data, wt->cur_rows.len) && pzpd_buf_append(&wt->heap, wt->cur_heap.data, wt->cur_heap.len);
        wt->nrows += wt->cur_n;
    }
    if (!ok) { w->broken = 1; pzpd_writer_reset_record(w); return 0; }   // index half-updated: refuse all further work

    w->cur_off         += recordBytes;
    w->shard_records   += 1;
    w->total_records   += 1;
    w->shard_last_group = w->group;
    w->last_group       = w->group;
    w->last_frame       = w->frame;
    pzpd_writer_reset_record(w);
    return 1;
}

/** @brief Rewrite shard_count / total_records in both superblocks of a completed shard.
 *  @return 1 on success, 0 on failure. */
static int pzpd_patch_shard(const char *path, uint32_t shard_count, uint64_t total_records)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) { pzpd_set_error(PZPD_E_IO, "cannot reopen %s: %s", path, strerror(errno)); return 0; }
    unsigned char block[PZPD_BLOCK];
    if (!pzpd_pread_all(fd, block, PZPD_BLOCK, 0)) { close(fd); return 0; }
    struct pzpd_disk_superblock sb;
    memcpy(&sb, block, sizeof(sb));
    sb.shard_count   = shard_count;
    sb.total_records = total_records;
    pzpd_seal_superblock(&sb);
    memcpy(block, &sb, sizeof(sb));
    int ok = pzpd_pwrite_all(fd, block, PZPD_BLOCK, sb.file_bytes - PZPD_BLOCK) &&
             pzpd_pwrite_all(fd, block, PZPD_BLOCK, 0);
    if (ok && (fsync(fd) != 0)) { pzpd_set_error(PZPD_E_IO, "fsync %s: %s", path, strerror(errno)); ok = 0; }
    close(fd);
    return ok;
}

/** @brief Free everything a writer owns. */
static void pzpd_writer_free(pzpd_writer *w)
{
    pzpd_writer_reset_record(w);
    for (unsigned i = 0; i < w->shard_count; i++) { free(w->shards[i].path); }
    free(w->shards);
    free(w->manifest_path);
    free(w->base);
    free(w->final_path);
    free(w->tmp_path);
    free(w->dedup);
    pzpd_buf_free(&w->rtab);
    pzpd_buf_free(&w->btab);
    pzpd_buf_free(&w->hash);
    pzpd_buf_free(&w->heap);
    pzpd_buf_free(&w->ghash);
    pzpd_buf_free(&w->gtab);
    free(w->groups);
    pzpd_buf_free(&w->strings);
    for (unsigned t = 0; (w->tables != NULL) && (t < w->T); t++)
    {
        struct pzpd_wtable *wt = &w->tables[t];
        pzpd_buf_free(&wt->index); pzpd_buf_free(&wt->rows); pzpd_buf_free(&wt->heap);
        pzpd_buf_free(&wt->cur_rows); pzpd_buf_free(&wt->cur_heap);
        pzpd_buf_free(&wt->g_rows); pzpd_buf_free(&wt->g_heap);
    }
    free(w->tables);
    free(w);
}

/** @brief One table as a manifest holds it: the schema, plus the rows for global tables. */
struct pzpd_mtable
{
    const struct pzpd_tschema *sc;  ///< Schema
    const void *rows;               ///< Global rows (NULL for record tables: schema only)
    uint64_t    nrows;              ///< Global rows
    const void *heap;               ///< Their strings
    uint64_t    heap_len;           ///< Strings size
};

struct pzpd_archive;
static struct pzpd_archive *arch_open(const char *path, unsigned int flags);
static void arch_close(struct pzpd_archive *a);
static int pzpd_mwords_sections(struct pzpd_archive *const *shards, unsigned n, struct pzpd_buf secs[PZPD_MAX_WORD_INDEXES],
                                struct pzpd_disk_words dir[PZPD_MAX_WORD_INDEXES], unsigned *W);

/** @brief Write a manifest atomically (temp file, fsync, rename, fsync of the directory): header,
 *  shard table, shard names, global hash (sorted here), table sections and, when `wshards` (the shards,
 *  opened standalone, in order) is given, the word indexes' merged vocabularies. Used by the writer and by
 *  pzpd_manifest_rebuild(), so a rebuilt manifest is byte-identical to the original.
 *  @return 1 on success, 0 on failure (error set). */
static int pzpd_write_manifest(const char *path, const uint8_t *uuid, uint64_t total, unsigned S, char streams[][24], unsigned shard_count,
                               const struct pzpd_buf *shardTab, const struct pzpd_buf *names, struct pzpd_buf *ghash, unsigned T, const struct pzpd_mtable *tabs,
                               struct pzpd_archive *const *wshards)
{
    qsort(ghash->data, ghash->len / sizeof(struct pzpd_disk_global_hash), sizeof(struct pzpd_disk_global_hash), pzpd_cmp_ghash);
    int ok = 1, fd = -1;
    size_t n = strlen(path) + 8;
    char *tmp = (char *) malloc(n);
    if (tmp == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    snprintf(tmp, n, "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { pzpd_set_error(PZPD_E_IO, "cannot create %s: %s", tmp, strerror(errno)); ok = 0; }
    if (ok)
    {
        struct pzpd_disk_manifest mh;
        memset(&mh, 0, sizeof(mh));
        memcpy(mh.magic, PZPD_MAGIC_MANIFEST, 8);
        mh.version       = PZPD_FORMAT_VERSION;
        memcpy(mh.archive_uuid, uuid, 16);
        mh.total_records = total;
        mh.shard_count   = shard_count;
        mh.stream_count  = S;
        pzpd_fill_streams(mh.streams, S, streams);
        XXH64_state_t *idx = XXH64_createState();
        if (idx == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
        else
        {
            XXH64_reset(idx, 0);
            uint64_t off = PZPD_BLOCK;
            ok = pzpd_write_section(fd, &off, PZPD_SECT_MSHARDS, shardTab->data, shardTab->len, &mh.shards_offset, idx) &&
                 pzpd_write_section(fd, &off, PZPD_SECT_MNAMES,  names->data,    names->len,    &mh.names_offset,  idx) &&
                 pzpd_write_section(fd, &off, PZPD_SECT_MHASH,   ghash->data,    ghash->len,    &mh.hash_offset,   idx);
            // Tables: schema copies (record tables) and full copies (global tables)
            struct pzpd_buf sec = {0};
            for (unsigned t = 0; ok && (t < T); t++)
            {
                uint64_t dataOff = 0;
                ok = pzpd_table_section(&sec, tabs[t].sc, 0, NULL, tabs[t].rows, tabs[t].nrows, tabs[t].heap, tabs[t].heap_len) &&
                     pzpd_write_section(fd, &off, PZPD_SECT_TABLE, sec.data, sec.len, &dataOff, idx);
                pzpd_put_slot_name(mh.tables[t].name, tabs[t].sc->name);
                mh.tables[t].flags          = (uint8_t) tabs[t].sc->flags;
                mh.tables[t].section_offset = dataOff;
                mh.tables[t].section_bytes  = sec.len;
                mh.tables[t].row_stride     = tabs[t].sc->stride;
            }
            pzpd_buf_free(&sec);
            mh.index_checksum = XXH64_digest(idx);
            // Word indexes: the merged vocabularies, with their own checksum
            if (ok && (wshards != NULL))
            {
                struct pzpd_buf ws[PZPD_MAX_WORD_INDEXES];
                memset(ws, 0, sizeof(ws));
                unsigned W = 0;
                ok = pzpd_mwords_sections(wshards, shard_count, ws, mh.words, &W);
                XXH64_reset(idx, 0);
                for (unsigned j = 0; ok && (j < W); j++)
                {
                    uint64_t dataOff = 0;
                    ok = pzpd_write_section(fd, &off, PZPD_SECT_MWORDS, ws[j].data, ws[j].len, &dataOff, idx);
                    mh.words[j].section_offset = dataOff;
                    mh.words[j].section_bytes  = ws[j].len;
                }
                mh.words_checksum = (W > 0) ? XXH64_digest(idx) : 0;
                for (unsigned j = 0; j < PZPD_MAX_WORD_INDEXES; j++) { pzpd_buf_free(&ws[j]); }
            }
            XXH64_freeState(idx);
            mh.names_bytes = names->len;
            mh.hash_count  = ghash->len / sizeof(struct pzpd_disk_global_hash);
            mh.file_bytes  = off;
            mh.sb_checksum = XXH64(&mh, offsetof(struct pzpd_disk_manifest, sb_checksum), 0);
            mh.ext_checksum = pzpd_ext_checksum(mh.words, offsetof(struct pzpd_disk_manifest, ext_checksum) - offsetof(struct pzpd_disk_manifest, words));
            unsigned char block[PZPD_BLOCK];
            memset(block, 0, sizeof(block));
            memcpy(block, &mh, sizeof(mh));
            ok = ok && pzpd_pwrite_all(fd, block, PZPD_BLOCK, 0);
            if (ok && (fsync(fd) != 0)) { pzpd_set_error(PZPD_E_IO, "fsync %s: %s", tmp, strerror(errno)); ok = 0; }
        }
    }
    if (fd >= 0) { close(fd); }
    if (ok && (rename(tmp, path) != 0)) { pzpd_set_error(PZPD_E_IO, "rename %s -> %s: %s", tmp, path, strerror(errno)); ok = 0; }
    if (ok) { pzpd_fsync_dir_of(path); }
    else { unlink(tmp); }
    free(tmp);
    return ok;
}

int pzpd_writer_finish(pzpd_writer *w)
{
    pzpd_clear_error();
    if (w == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL writer"); return 0; }
    if (w->in_record) { pzpd_set_error(PZPD_E_STATE, "pzpd_writer_finish inside an unfinished record"); pzpd_writer_abort(w); return 0; }
    if (w->broken)    { pzpd_set_error(PZPD_E_STATE, "the writer failed earlier; the archive was not finished"); pzpd_writer_abort(w); return 0; }

    int ok = pzpd_writer_close_shard(w);
    for (unsigned i = 0; ok && (i < w->shard_count); i++) { ok = pzpd_patch_shard(w->shards[i].path, w->shard_count, w->total_records); }

    //----------------------------------------------------------------------
    // Manifest: header, shard table, shard names (relative), global hash
    //----------------------------------------------------------------------
    struct pzpd_buf shardTab = {0}, names = {0};
    for (unsigned i = 0; ok && (i < w->shard_count); i++)
    {
        const char *nm = strrchr(w->shards[i].path, '/');
        nm = (nm == NULL) ? w->shards[i].path : nm + 1;
        struct pzpd_disk_manifest_shard ms;
        memset(&ms, 0, sizeof(ms));
        ms.first_ordinal  = w->shards[i].first_ordinal;
        ms.record_count   = w->shards[i].record_count;
        ms.file_bytes     = w->shards[i].file_bytes;
        ms.generation     = 1;
        ms.index_checksum = w->shards[i].index_checksum;
        ms.name_offset    = (uint32_t) names.len;
        ms.name_len       = (uint32_t) strlen(nm);
        ok = pzpd_buf_append(&shardTab, &ms, sizeof(ms)) && pzpd_buf_append(&names, nm, strlen(nm));
    }
    struct pzpd_mtable tabs[PZPD_MAX_TABLES];
    for (unsigned t = 0; t < w->T; t++)
    {
        struct pzpd_wtable *wt = &w->tables[t];
        int global = (wt->sc.flags & PZPD_TABLE_GLOBAL) != 0;
        tabs[t].sc       = &wt->sc;
        tabs[t].rows     = global ? wt->g_rows.data : NULL;
        tabs[t].nrows    = global ? wt->g_n : 0;
        tabs[t].heap     = global ? wt->g_heap.data : NULL;
        tabs[t].heap_len = global ? wt->g_heap.len : 0;
    }
    // Word indexes: the manifest merges the finished shards' vocabularies
    struct pzpd_archive **wsh = NULL;
    if (ok && (w->W > 0))
    {
        wsh = (struct pzpd_archive **) calloc(w->shard_count ? w->shard_count : 1, sizeof(*wsh));
        if (wsh == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
        for (unsigned i = 0; ok && (i < w->shard_count); i++) { wsh[i] = arch_open(w->shards[i].path, 0); ok = (wsh[i] != NULL); }
    }
    ok = ok && pzpd_write_manifest(w->manifest_path, w->uuid, w->total_records, w->S, w->streams, w->shard_count, &shardTab, &names, &w->ghash, w->T, tabs, wsh);
    for (unsigned i = 0; (wsh != NULL) && (i < w->shard_count); i++) { if (wsh[i] != NULL) { arch_close(wsh[i]); } }
    free(wsh);
    pzpd_buf_free(&shardTab);
    pzpd_buf_free(&names);

    struct pzpd_saved_error e;
    pzpd_error_save(&e);
    pzpd_writer_abort(w);          // only frees after a complete finish; after a failed last shard it also closes and removes its .tmp
    if (!ok) { pzpd_error_restore(&e); }
    return ok;
}

void pzpd_writer_abort(pzpd_writer *w)
{
    if (w == NULL) { return; }
    if (w->fd >= 0) { close(w->fd); w->fd = -1; }
    if (w->tmp_path != NULL) { unlink(w->tmp_path); }
    pzpd_writer_free(w);
}

//-----------------------------------------------------------------------------------------------
// Reader
//-----------------------------------------------------------------------------------------------

/** @brief One shard of an open archive. Opened lazily; state goes 0 -> 1 (ok) or -1 (failed) once. */
struct pzpd_rshard
{
    char    *path;             ///< Shard file path
    uint64_t first_ordinal;    ///< Archive ordinal of the first record (from the manifest or superblock)
    uint64_t record_count;     ///< Records (from the manifest or superblock)
    int      state;            ///< 0 not opened yet, 1 open, -1 failed (read with acquire / written with release)
    char     error[512];       ///< Why opening failed, when state is -1
    int      error_code;       ///< enum pzpd_error of that failure
    int      fd;               ///< File descriptor for pread()
    unsigned char *map;        ///< Whole-file mapping
    size_t   map_len;          ///< Mapping length
    struct pzpd_disk_superblock sb; ///< Validated superblock
    const struct pzpd_disk_record *rtab; ///< Record table (in map)
    const struct pzpd_disk_blob   *btab; ///< Blob table (in map)
    const struct pzpd_disk_hash   *hash; ///< Hash table (in map)
    const char                    *heap; ///< String heap (in map)
    struct pzpd_tview tv[PZPD_MAX_TABLES]; ///< Table sections (in map), validated at load
    int      storage;          ///< enum pzpd_storage of the shard's file system
    int      recovery;         ///< 0 primary superblock, 1 backup superblock, 2 superblock rebuilt from the index sections
    const struct pzpd_disk_group  *groups; ///< Group table (in map), NULL when the shard has no groups
    int      words_bad;        ///< 1 if the word index directory (revision-12 extension) failed its checksum
};

/** @brief One open archive (manifest + shards) or a single shard opened standalone: the unit a
 *  collection member refers to. Ordinals and stream ids here are local to the archive. */
struct pzpd_archive
{
    unsigned flags;                     ///< Open flags (PZPD_O_VERIFY)
    pthread_mutex_t lock;               ///< Taken once per shard, for its lazy open
    unsigned S;                         ///< Stream count
    char     streams[PZPD_MAX_STREAMS][24]; ///< Stream names
    uint8_t  uuid[16];                  ///< Archive uuid
    uint64_t total;                     ///< Records
    unsigned shard_count;               ///< Shards
    struct pzpd_rshard *shards;         ///< Shards
    int      standalone;                ///< 1 when a single shard was opened (no manifest)
    unsigned char *mmap_manifest;       ///< Manifest mapping (NULL when standalone)
    size_t   manifest_len;              ///< Manifest mapping length
    const struct pzpd_disk_manifest_shard *mshards; ///< Manifest shard table
    const struct pzpd_disk_global_hash    *ghash;   ///< Manifest global hash
    uint64_t ghash_count;               ///< Global hash entries
    unsigned T;                         ///< Tables
    struct pzpd_tschema *tables;        ///< Table schemas (PZPD_MAX_TABLES entries, fixed address)
    struct pzpd_tview gview[PZPD_MAX_TABLES]; ///< Global tables' rows (manifest copy, or the standalone shard's)
    struct pzpd_disk_words mwords[PZPD_MAX_WORD_INDEXES]; ///< Manifest word directory (merged vocabularies), zero when none
    int      mwords_bad;                ///< 1 if the manifest's word directory failed its checksum
};

/** @brief Bytes of a header's revision-12 extension covered by ext_checksum. */
#define PZPD_SB_EXT_BYTES (offsetof(struct pzpd_disk_superblock, ext_checksum) - offsetof(struct pzpd_disk_superblock, words))
#define PZPD_MH_EXT_BYTES (offsetof(struct pzpd_disk_manifest, ext_checksum) - offsetof(struct pzpd_disk_manifest, words))

static int pzpd_check_section(const unsigned char *map, uint64_t file, uint64_t data_off, uint32_t kind, uint64_t bytes);

/** @brief Read a table directory (superblock or manifest) into schemas / views.
 *  @param a        Archive whose schemas are filled (define = 1) or compared (define = 0).
 *  @param views    Receives one view per table (may be NULL).
 *  @param records  Record count the record tables' indexes must cover, -1 for none (manifest).
 *  @return 1 on success, 0 on failure (error set). */
static int pzpd_read_table_dir(struct pzpd_archive *a, const struct pzpd_disk_table *dir, const unsigned char *map, uint64_t file,
                               struct pzpd_tview *views, int64_t records, int define, const char *what)
{
    unsigned T = 0;
    while ( (T < PZPD_MAX_TABLES) && (dir[T].name[0] != 0) ) { T++; }
    if (define)
    {
        if (a->tables == NULL) { a->tables = (struct pzpd_tschema *) calloc(PZPD_MAX_TABLES, sizeof(struct pzpd_tschema)); }
        if (a->tables == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
        a->T = T;
    }
    else if (T != a->T) { pzpd_set_error(PZPD_E_STALE_MANIFEST, "%s has %u tables, the manifest %u", what, T, a->T); return 0; }
    for (unsigned t = 0; t < T; t++)
    {
        struct pzpd_tschema sc;
        struct pzpd_tview v;
        if ( !pzpd_check_section(map, file, dir[t].section_offset, PZPD_SECT_TABLE, dir[t].section_bytes) ||
             !pzpd_table_parse(map + dir[t].section_offset, dir[t].section_bytes, &sc, &v, (dir[t].flags & PZPD_TABLE_GLOBAL) ? -1 : records) )
        {
            if (pzpd_errorText[0] == 0) { pzpd_set_error(PZPD_E_FORMAT, "section is damaged"); }   // pzpd_check_section() sets none
            pzpd_error_wrap(PZPD_E_FORMAT, "%s: table %u", what, t);
            return 0;
        }
        char dn[24];
        pzpd_get_slot_name(dn, dir[t].name);
        if ( strcmp(dn, sc.name) || ((unsigned) dir[t].flags != sc.flags) || (dir[t].row_stride != sc.stride) )
            { pzpd_set_error(PZPD_E_FORMAT, "%s: table %u: the directory and the table section disagree", what, t); return 0; }
        if (define) { a->tables[t] = sc; pzpd_schema_publish(&a->tables[t]); if (sc.flags & PZPD_TABLE_GLOBAL) { a->gview[t] = v; } }
        else if (!pzpd_schema_equal(&sc, &a->tables[t])) { pzpd_set_error(PZPD_E_STALE_MANIFEST, "%s: table %s differs from the manifest", what, sc.name); return 0; }
        if (views != NULL) { views[t] = v; }
    }
    return 1;
}

static void arch_close(struct pzpd_archive *a);   // used by arch_open() on failure

/** @brief Check a section header in front of data_off, of the expected kind and size. */
static int pzpd_check_section(const unsigned char *map, uint64_t file, uint64_t data_off, uint32_t kind, uint64_t bytes)
{
    if ( (data_off < sizeof(struct pzpd_disk_section)) || !pzpd_in_file(data_off, bytes, file) ) { return 0; }
    struct pzpd_disk_section sh;
    memcpy(&sh, map + data_off - sizeof(sh), sizeof(sh));
    return (memcmp(sh.magic, PZPD_MAGIC_SECTION, 8) == 0) && (sh.kind == kind) && (sh.bytes == bytes);
}

/** @brief Validate a superblock candidate (magic, version, checksum, geometry).
 *  @return 1 if valid, 0 otherwise (error set). */
static int pzpd_superblock_valid(const struct pzpd_disk_superblock *sb, uint64_t file)
{
    if (memcmp(sb->magic, PZPD_MAGIC_SHARD, 8) != 0) { pzpd_set_error(PZPD_E_FORMAT, "bad shard magic"); return 0; }
    if (sb->version > PZPD_FORMAT_VERSION) { pzpd_set_error(PZPD_E_VERSION, "shard format version %u is newer than this library (%d)", sb->version, PZPD_FORMAT_VERSION); return 0; }
    if (sb->sb_checksum != XXH64(sb, offsetof(struct pzpd_disk_superblock, sb_checksum), 0)) { pzpd_set_error(PZPD_E_CHECKSUM, "superblock checksum mismatch"); return 0; }
    if ( (sb->stream_count == 0) || (sb->stream_count > PZPD_MAX_STREAMS) ) { pzpd_set_error(PZPD_E_FORMAT, "bad stream count %u", sb->stream_count); return 0; }
    if (sb->file_bytes > file) { pzpd_set_error(PZPD_E_FORMAT, "shard is truncated (%llu of %llu bytes)", (unsigned long long) file, (unsigned long long) sb->file_bytes); return 0; }
    if (sb->record_count > 0xFFFFFFFFull) { pzpd_set_error(PZPD_E_FORMAT, "bad record count"); return 0; }
    return 1;
}

#ifndef MADV_POPULATE_READ
#define MADV_POPULATE_READ 22   ///< Linux 5.14+; older kernels return EINVAL and pzpd_populate() touches the pages instead
#endif
#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994  ///< statfs f_type of tmpfs (linux/magic.h)
#endif
#ifndef RAMFS_MAGIC
#define RAMFS_MAGIC 0x858458f6  ///< statfs f_type of ramfs (linux/magic.h)
#endif

/** @brief Storage kind of the file system holding an open file: tmpfs / ramfs → RAM, anything else → BLOCK. */
static int pzpd_storage_of_fd(int fd)
{
    struct statfs fs;
    if (fstatfs(fd, &fs) != 0) { return PZPD_STORAGE_BLOCK; }
    return ( ((unsigned long) fs.f_type == (unsigned long) TMPFS_MAGIC) || ((unsigned long) fs.f_type == (unsigned long) RAMFS_MAGIC) ) ? PZPD_STORAGE_RAM : PZPD_STORAGE_BLOCK;
}

/** @brief Make [p, p+len) of a read-only file mapping resident and its page tables filled:
 *  `MADV_POPULATE_READ`, or one read per page where the kernel lacks it. p need not be page-aligned. */
static void pzpd_populate(const void *p, size_t len)
{
    if (len == 0) { return; }
    uintptr_t page = (uintptr_t) sysconf(_SC_PAGESIZE);
    uintptr_t lo = (uintptr_t) p & ~(page - 1);
    uintptr_t hi = ((uintptr_t) p + len + page - 1) & ~(page - 1);   // whole pages holding the range: all mapped
    if (madvise((void *) lo, hi - lo, MADV_POPULATE_READ) == 0) { return; }
    if (errno != EINVAL) { return; }          // EFAULT / EIO: the reader will see the problem itself
    unsigned char sink = 0;
    for (uintptr_t q = lo; q < hi; q += page) { sink ^= *(volatile const unsigned char *) q; }
    (void) sink;
}

/** @brief Value of `"key":` in the shard metadata JSON written by pzpd_writer_close_shard().
 *  @return Pointer to the value (not terminated), or NULL if the key is absent. */
static const char *pzpd_meta_value(const char *json, size_t len, const char *key)
{
    char pat[48];
    int pl = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if ( (pl <= 0) || ((size_t) pl >= sizeof(pat)) ) { return NULL; }
    for (size_t i = 0; i + (size_t) pl <= len; i++) { if (memcmp(json + i, pat, (size_t) pl) == 0) { return json + i + pl; } }
    return NULL;
}

/** @brief Unsigned number at `"key":` in the metadata JSON, or dflt. */
static uint64_t pzpd_meta_u64(const char *json, size_t len, const char *key, uint64_t dflt)
{
    const char *v = pzpd_meta_value(json, len, key);
    if ( (v == NULL) || (v >= json + len) || (*v < '0') || (*v > '9') ) { return dflt; }
    uint64_t x = 0;
    while ( (v < json + len) && (*v >= '0') && (*v <= '9') ) { x = x * 10 + (uint64_t)(*v - '0'); v++; }
    return x;
}

/** @brief Stream names from the metadata JSON (`"streams":["a","b",...]`). @return Count found (0 if none). */
static unsigned pzpd_meta_streams(const char *json, size_t len, char names[][24])
{
    const char *v = pzpd_meta_value(json, len, "streams");
    const char *end = json + len;
    if ( (v == NULL) || (v >= end) || (*v != '[') ) { return 0; }
    unsigned n = 0;
    for (v++; (v < end) && (*v != ']') && (n < PZPD_MAX_STREAMS); )
    {
        if (*v != '"') { v++; continue; }
        const char *q = memchr(v + 1, '"', (size_t)(end - v - 1));
        if ( (q == NULL) || (q - v - 1 > 23) || (q == v + 1) ) { return 0; }
        memcpy(names[n], v + 1, (size_t)(q - v - 1));
        names[n][q - v - 1] = 0;
        n++;
        v = q + 1;
    }
    return n;
}

/** @brief Recovery ladder step 2 (spec §4.1): both superblocks are bad, the index is not. Find the index
 *  sections by their magic at 4 KiB boundaries, checking each one's XXH64, and rebuild the superblock from
 *  them and the metadata JSON. The scan goes backwards from the end of the file and stops at the first
 *  record-table section, so an archive stored as a blob inside this one is never mistaken for its index.
 *  @param hint_first First ordinal to use when the metadata lacks it (from the manifest, else 0).
 *  @return 1 on success (sealed superblock in *out), 0 if the index can't be found (error set). */
static int pzpd_sb_from_sections(const unsigned char *map, uint64_t file, uint64_t hint_first, struct pzpd_disk_superblock *out)
{
    struct { uint32_t kind; uint64_t off, bytes; } found[64];
    // Table edits append a table's new section and leave the old one until compact: per name, the newest
    // section (met first going backwards) holds the rows, the oldest one's position gives the directory slot
    struct pzpd_scan_table { char name[24]; uint32_t flags, stride; uint64_t off, bytes, slot; } tb[PZPD_MAX_TABLES];
    unsigned nf = 0, T = 0;
    int haveRecords = 0;
    if (file < 2 * PZPD_BLOCK) { pzpd_set_error(PZPD_E_FORMAT, "file too small"); return 0; }
    for (uint64_t off = (file - sizeof(struct pzpd_disk_section)) & ~(uint64_t)(PZPD_BLOCK - 1); (off >= PZPD_BLOCK) && !haveRecords; off -= PZPD_BLOCK)
    {
        struct pzpd_disk_section h;
        memcpy(&h, map + off, sizeof(h));
        if ( (memcmp(h.magic, PZPD_MAGIC_SECTION, 8) != 0) || (h.bytes > file - off - sizeof(h)) ) { continue; }
        if ( !(((h.kind >= PZPD_SECT_RECORDS) && (h.kind <= PZPD_SECT_META)) || (h.kind == PZPD_SECT_TABLE) || (h.kind == PZPD_SECT_GROUPS)) ) { continue; }
        if (XXH64(map + off + sizeof(h), (size_t) h.bytes, 0) != h.checksum) { continue; }
        if (h.kind == PZPD_SECT_TABLE)
        {
            struct pzpd_disk_table_head th;
            if (h.bytes < sizeof(th)) { continue; }
            memcpy(&th, map + off + sizeof(h), sizeof(th));
            if (memchr(th.name, 0, sizeof(th.name)) == NULL) { continue; }
            unsigned t = 0;
            while ( (t < T) && (strcmp(tb[t].name, th.name) != 0) ) { t++; }
            if (t < T) { tb[t].slot = off; continue; }                 // an older version: only its position counts
            if (T == PZPD_MAX_TABLES) { continue; }
            memcpy(tb[T].name, th.name, sizeof(tb[T].name));
            tb[T].flags = th.flags; tb[T].stride = th.row_stride; tb[T].off = off + sizeof(h); tb[T].bytes = h.bytes; tb[T].slot = off;
            T++;
            continue;
        }
        if (nf == 64) { break; }
        found[nf].kind = h.kind; found[nf].off = off + sizeof(h); found[nf].bytes = h.bytes; nf++;
        haveRecords = (h.kind == PZPD_SECT_RECORDS);
    }
    if (!haveRecords) { pzpd_set_error(PZPD_E_FORMAT, "no intact index sections found"); return 0; }

    // In file order: records, blobs, hash, heap, meta, groups (first of each kind after the record table)
    uint64_t off[6] = {0}, bytes[6] = {0};
    int have[6] = {0};
    uint64_t goff = 0, gbytes = 0;
    int haveGroups = 0;
    struct pzpd_disk_superblock sb;
    memset(&sb, 0, sizeof(sb));
    for (int i = (int) nf - 1; i >= 0; i--)
    {
        uint32_t k = found[i].kind;
        if ( (k <= PZPD_SECT_META) && !have[k] ) { have[k] = 1; off[k] = found[i].off; bytes[k] = found[i].bytes; }
        else if ( (k == PZPD_SECT_GROUPS) && !haveGroups && (found[i].bytes % sizeof(struct pzpd_disk_group) == 0) ) { haveGroups = 1; goff = found[i].off; gbytes = found[i].bytes; }
    }
    // Tables in directory order: by the position of each one's first section
    for (unsigned i = 1; i < T; i++)
    {
        for (unsigned j = i; (j > 0) && (tb[j - 1].slot > tb[j].slot); j--) { struct pzpd_scan_table x = tb[j]; tb[j] = tb[j - 1]; tb[j - 1] = x; }
    }
    for (unsigned t = 0; t < T; t++)
    {
        pzpd_put_slot_name(sb.tables[t].name, tb[t].name);
        sb.tables[t].flags          = (uint8_t) tb[t].flags;
        sb.tables[t].section_offset = tb[t].off;
        sb.tables[t].section_bytes  = tb[t].bytes;
        sb.tables[t].row_stride     = tb[t].stride;
    }
    if ( !have[PZPD_SECT_BLOBS] || !have[PZPD_SECT_HASH] || !have[PZPD_SECT_HEAP] ||
         (bytes[PZPD_SECT_RECORDS] % sizeof(struct pzpd_disk_record)) || (bytes[PZPD_SECT_HASH] % sizeof(struct pzpd_disk_hash)) )
        { pzpd_set_error(PZPD_E_FORMAT, "index sections are incomplete"); return 0; }

    const char *meta = have[PZPD_SECT_META] ? (const char *)(map + off[PZPD_SECT_META]) : "";
    size_t mlen = have[PZPD_SECT_META] ? (size_t) bytes[PZPD_SECT_META] : 0;
    char names[PZPD_MAX_STREAMS][24];
    unsigned metaS = pzpd_meta_streams(meta, mlen, names);
    uint64_t n = bytes[PZPD_SECT_RECORDS] / sizeof(struct pzpd_disk_record);
    uint64_t S = (n > 0) ? bytes[PZPD_SECT_BLOBS] / (n * sizeof(struct pzpd_disk_blob)) : metaS;
    if ( (S == 0) || (S > PZPD_MAX_STREAMS) || ((n > 0) && (bytes[PZPD_SECT_BLOBS] != n * S * sizeof(struct pzpd_disk_blob))) )
        { pzpd_set_error(PZPD_E_FORMAT, "record and blob tables disagree"); return 0; }
    if (metaS != S) { for (unsigned i = 0; i < S; i++) { snprintf(names[i], sizeof(names[i]), "stream%u", i); } }   // names lost: placeholders

    memcpy(sb.magic, PZPD_MAGIC_SHARD, 8);
    sb.version       = PZPD_FORMAT_VERSION;
    const char *uu = pzpd_meta_value(meta, mlen, "archive_uuid");
    for (int i = 0; (uu != NULL) && (uu + 33 <= meta + mlen) && (*uu == '"') && (i < 16); i++)
    {
        unsigned v = 0;
        if (sscanf(uu + 1 + 2 * i, "%2x", &v) != 1) { break; }
        sb.archive_uuid[i] = (uint8_t) v;
    }
    sb.generation    = pzpd_meta_u64(meta, mlen, "generation", 1);
    sb.shard_index   = (uint32_t) pzpd_meta_u64(meta, mlen, "shard_index", 0);
    sb.first_ordinal = pzpd_meta_u64(meta, mlen, "first_ordinal", hint_first);
    sb.record_count  = n;
    sb.stream_count  = (uint32_t) S;
    sb.align         = (uint32_t) pzpd_meta_u64(meta, mlen, "align", 64);
    pzpd_fill_streams(sb.streams, (unsigned) S, names);
    sb.records_offset = PZPD_BLOCK;
    const struct pzpd_disk_record *rt = (const struct pzpd_disk_record *)(map + off[PZPD_SECT_RECORDS]);
    uint64_t end = PZPD_BLOCK;
    for (uint64_t i = 0; i < n; i++)
    {
        struct pzpd_disk_record r;
        memcpy(&r, &rt[i], sizeof(r));
        if (!pzpd_in_file(r.offset, r.bytes, file)) { pzpd_set_error(PZPD_E_FORMAT, "record table points past the end of the file"); return 0; }
        if (r.offset + r.bytes > end) { end = r.offset + r.bytes; }
    }
    sb.records_bytes = end - PZPD_BLOCK;
    sb.rtab_offset   = off[PZPD_SECT_RECORDS];
    sb.btab_offset   = off[PZPD_SECT_BLOBS];
    sb.hash_offset   = off[PZPD_SECT_HASH];
    sb.hash_count    = bytes[PZPD_SECT_HASH] / sizeof(struct pzpd_disk_hash);
    sb.heap_offset   = off[PZPD_SECT_HEAP];
    sb.heap_bytes    = bytes[PZPD_SECT_HEAP];
    sb.meta_offset   = off[PZPD_SECT_META];
    sb.meta_bytes    = bytes[PZPD_SECT_META];
    if (haveGroups) { sb.groups_offset = goff; sb.group_count = gbytes / sizeof(struct pzpd_disk_group); sb.flags |= 1u; }
    sb.file_bytes    = file;
    XXH64_state_t *idx = XXH64_createState();
    if (idx == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    XXH64_reset(idx, 0);
    for (uint32_t k = PZPD_SECT_RECORDS; k <= PZPD_SECT_META; k++) { if (have[k]) { XXH64_update(idx, map + off[k], (size_t) bytes[k]); } }
    if (haveGroups) { XXH64_update(idx, map + goff, (size_t) gbytes); }
    for (unsigned t = 0; t < T; t++) { XXH64_update(idx, map + sb.tables[t].section_offset, (size_t) sb.tables[t].section_bytes); }
    sb.index_checksum = XXH64_digest(idx);
    XXH64_freeState(idx);
    pzpd_seal_superblock(&sb);
    *out = sb;
    return 1;
}

/** @brief Map and validate one shard. Called with the archive lock held.
 *  @return 1 on success, 0 on failure (error set). */
static int pzpd_shard_load(struct pzpd_archive *a, struct pzpd_rshard *s)
{
    s->fd = open(s->path, O_RDONLY | O_CLOEXEC);
    if (s->fd < 0) { pzpd_set_error(PZPD_E_SHARD_MISSING, "cannot open shard %s: %s", s->path, strerror(errno)); return 0; }
    struct stat st;
    if (fstat(s->fd, &st) != 0) { pzpd_set_error(PZPD_E_IO, "cannot stat %s: %s", s->path, strerror(errno)); return 0; }
    uint64_t file = (uint64_t) st.st_size;
    if (file < 2 * PZPD_BLOCK) { pzpd_set_error(PZPD_E_FORMAT, "%s is too small to be a shard", s->path); return 0; }
    s->map = (unsigned char *) mmap(NULL, (size_t) file, PROT_READ, MAP_SHARED, s->fd, 0);
    if (s->map == MAP_FAILED) { s->map = NULL; pzpd_set_error(PZPD_E_IO, "cannot map %s: %s", s->path, strerror(errno)); return 0; }
    s->map_len = (size_t) file;
    s->storage = pzpd_storage_of_fd(s->fd);
    if (a->flags & PZPD_O_HUGEPAGE) { (void) madvise(s->map, s->map_len, MADV_HUGEPAGE); }   // best effort
    if (a->flags & PZPD_O_POPULATE) { pzpd_populate(s->map, s->map_len); }

    // Primary superblock, or the backup at the end of the file. A valid backup of a higher generation wins:
    // a table edit / compact appends its new generation's backup first and flips the primary last (spec §7)
    struct pzpd_disk_superblock sb, eof;
    memcpy(&sb, s->map, sizeof(sb));
    memcpy(&eof, s->map + file - PZPD_BLOCK, sizeof(eof));
    if (pzpd_superblock_valid(&sb, file))
    {
        if ( (eof.generation > sb.generation) && (eof.file_bytes == file) && (memcmp(eof.archive_uuid, sb.archive_uuid, 16) == 0) && pzpd_superblock_valid(&eof, file) )
        {
            sb = eof;
            s->recovery = 1;
        }
        pzpd_clear_error();
    }
    else
    {
        if (pzpd_errorCode == PZPD_E_VERSION) { return 0; }
        memcpy(&sb, s->map + file - PZPD_BLOCK, sizeof(sb));
        s->recovery = 1;
        if (!pzpd_superblock_valid(&sb, file) &&
            !(pzpd_sb_from_sections(s->map, file, a->standalone ? 0 : s->first_ordinal, &sb) && pzpd_superblock_valid(&sb, file) && ((s->recovery = 2) != 0)) )
        {
            char msg[512];
            snprintf(msg, sizeof(msg), "%s", pzpd_errorText);
            pzpd_set_error(pzpd_errorCode, "%s: %s (primary and backup superblock)", s->path, msg);
            return 0;
        }
        pzpd_clear_error();                                       // recovered: the failed checks are not errors of the caller's call
    }

    // Geometry: every section inside the file, sized consistently with the record count
    uint64_t n = sb.record_count, S = sb.stream_count;
    if ( !pzpd_in_file(sb.records_offset, sb.records_bytes, file) ||
         !pzpd_check_section(s->map, file, sb.rtab_offset, PZPD_SECT_RECORDS, n * sizeof(struct pzpd_disk_record)) ||
         !pzpd_check_section(s->map, file, sb.btab_offset, PZPD_SECT_BLOBS,   n * S * sizeof(struct pzpd_disk_blob)) ||
         !pzpd_check_section(s->map, file, sb.hash_offset, PZPD_SECT_HASH,    sb.hash_count * sizeof(struct pzpd_disk_hash)) ||
         !pzpd_check_section(s->map, file, sb.heap_offset, PZPD_SECT_HEAP,    sb.heap_bytes) ||
         (sb.hash_count > n * (S + 1) + sb.group_count) || (sb.heap_bytes > 0xFFFFFFFFull) )
    {
        pzpd_set_error(PZPD_E_FORMAT, "%s: index sections are damaged", s->path);
        return 0;
    }

    // Video groups: inside the file, in record order, within the shard's records, names inside the heap
    if (sb.group_count > 0)
    {
        if ( (sb.group_count > n) || !pzpd_check_section(s->map, file, sb.groups_offset, PZPD_SECT_GROUPS, sb.group_count * sizeof(struct pzpd_disk_group)) )
            { pzpd_set_error(PZPD_E_FORMAT, "%s: group table is damaged", s->path); return 0; }
        const struct pzpd_disk_group *g = (const struct pzpd_disk_group *)(s->map + sb.groups_offset);
        uint64_t next = 0;
        for (uint64_t i = 0; i < sb.group_count; i++)
        {
            if ( (g[i].first_local < next) || (g[i].frame_count == 0) || ((uint64_t) g[i].first_local + g[i].frame_count > n) || !pzpd_in_file(g[i].name_offset, g[i].name_len, sb.heap_bytes) )
                { pzpd_set_error(PZPD_E_FORMAT, "%s: group %llu is damaged", s->path, (unsigned long long) i); return 0; }
            next = (uint64_t) g[i].first_local + g[i].frame_count;
        }
        s->groups = g;
    }

    // Tables (record tables: the index must cover every record of the shard)
    if (!pzpd_read_table_dir(a, sb.tables, s->map, file, s->tv, (int64_t) sb.record_count, a->standalone, s->path)) { return 0; }
    if (a->standalone) { for (unsigned t = 0; t < a->T; t++) { if (a->tables[t].flags & PZPD_TABLE_GLOBAL) { a->gview[t] = s->tv[t]; } } }

    // Consistency with the archive (manifest) or adopt the shard's identity (standalone)
    if (a->standalone)
    {
        a->S = sb.stream_count;
        for (unsigned i = 0; i < a->S; i++) { pzpd_get_slot_name(a->streams[i], sb.streams[i].name); }
        memcpy(a->uuid, sb.archive_uuid, 16);
        s->first_ordinal = 0;
        s->record_count  = sb.record_count;
        a->total         = sb.record_count;
    }
    else
    {
        if (memcmp(sb.archive_uuid, a->uuid, 16) != 0) { pzpd_set_error(PZPD_E_STALE_MANIFEST, "%s belongs to another archive", s->path); return 0; }
        if ( (sb.first_ordinal != s->first_ordinal) || (sb.record_count != s->record_count) || (sb.stream_count != a->S) )
            { pzpd_set_error(PZPD_E_STALE_MANIFEST, "%s disagrees with the manifest", s->path); return 0; }
    }
    // Word index directory: damage there only makes the word indexes unusable (they are re-derivable)
    s->words_bad = (sb.ext_checksum != pzpd_ext_checksum(sb.words, PZPD_SB_EXT_BYTES));
    s->sb   = sb;
    s->rtab = (const struct pzpd_disk_record *) (s->map + sb.rtab_offset);
    s->btab = (const struct pzpd_disk_blob *)   (s->map + sb.btab_offset);
    s->hash = (const struct pzpd_disk_hash *)   (s->map + sb.hash_offset);
    s->heap = (const char *)                    (s->map + sb.heap_offset);
    return 1;
}

/** @brief Get shard i, opening it on first use.
 *  @return The shard, or NULL if it can't be opened (error set, PZPD_E_SHARD_MISSING or the cause). */
static struct pzpd_rshard *pzpd_shard(struct pzpd_archive *a, unsigned i)
{
    struct pzpd_rshard *s = &a->shards[i];
    int st = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
    if (st == 1) { return s; }
    if (st == 0)
    {
        pthread_mutex_lock(&a->lock);
        if (s->state == 0)
        {
            if (pzpd_shard_load(a, s))
            {
                __atomic_store_n(&s->state, 1, __ATOMIC_RELEASE);
            }
            else
            {
                snprintf(s->error, sizeof(s->error), "%s", pzpd_errorText);
                s->error_code = pzpd_errorCode;
                if (s->map != NULL) { munmap(s->map, s->map_len); s->map = NULL; }
                if (s->fd >= 0) { close(s->fd); s->fd = -1; }
                __atomic_store_n(&s->state, -1, __ATOMIC_RELEASE);
            }
        }
        pthread_mutex_unlock(&a->lock);
        st = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
        if (st == 1) { return s; }
    }
    pzpd_set_error(PZPD_E_SHARD_MISSING, "shard %u unavailable: %s", i, s->error);
    return NULL;
}

/** @brief Find the shard holding an ordinal (binary search over first ordinals).
 *  @return Shard index, or -1 if the ordinal is out of range (error set). */
static int pzpd_shard_of(const struct pzpd_archive *a, uint64_t ordinal)
{
    if (ordinal >= a->total) { pzpd_set_error(PZPD_E_ARG, "ordinal %llu out of range (%llu records)", (unsigned long long) ordinal, (unsigned long long) a->total); return -1; }
    unsigned lo = 0, hi = a->shard_count;
    while (hi - lo > 1)
    {
        unsigned mid = lo + (hi - lo) / 2;
        if (a->shards[mid].first_ordinal <= ordinal) { lo = mid; } else { hi = mid; }
    }
    return (int) lo;
}

/** @brief Resolve an ordinal to its (open) shard and local record index, bounds-checking the record.
 *  @return The shard, or NULL on error (error set). */
static struct pzpd_rshard *pzpd_locate(struct pzpd_archive *a, uint64_t ordinal, uint64_t *local)
{
    int si = pzpd_shard_of(a, ordinal);
    if (si < 0) { return NULL; }
    struct pzpd_rshard *s = pzpd_shard(a, (unsigned) si);
    if (s == NULL) { return NULL; }
    uint64_t l = ordinal - s->first_ordinal;
    if (l >= s->sb.record_count) { pzpd_set_error(PZPD_E_FORMAT, "ordinal %llu missing from its shard", (unsigned long long) ordinal); return NULL; }
    const struct pzpd_disk_record *r = &s->rtab[l];
    if ( (r->offset < s->sb.records_offset) || !pzpd_in_file(r->offset, r->bytes, s->sb.records_offset + s->sb.records_bytes) )
    {
        pzpd_set_error(PZPD_E_FORMAT, "%s: record %llu points outside the record area", s->path, (unsigned long long) l);
        return NULL;
    }
    *local = l;
    return s;
}

/** @brief Blob entry of (local record, stream), bounds-checked against its record and the heap.
 *  @return The entry (rel_offset may be PZPD_MISSING), or NULL if damaged (error set). */
static const struct pzpd_disk_blob *pzpd_blob_entry(const struct pzpd_rshard *s, uint64_t local, unsigned stream)
{
    const struct pzpd_disk_blob *b = &s->btab[local * s->sb.stream_count + stream];
    if (b->rel_offset == PZPD_MISSING) { return b; }
    const struct pzpd_disk_record *r = &s->rtab[local];
    if ( !pzpd_in_file(b->rel_offset, b->size, r->bytes) || !pzpd_in_file(b->name_offset, b->name_len, s->sb.heap_bytes) )
    {
        pzpd_set_error(PZPD_E_FORMAT, "%s: blob entry of record %llu is damaged", s->path, (unsigned long long) local);
        return NULL;
    }
    return b;
}

/** @brief Single-archive part of pzpd_open(): ordinals, shards and stream ids are local to the archive. */
static struct pzpd_archive *arch_open(const char *path, unsigned int flags)
{
    pzpd_clear_error();
    if (path == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL path"); return NULL; }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { pzpd_set_error(PZPD_E_IO, "cannot open %s: %s", path, strerror(errno)); return NULL; }
    char magic[8];
    if (!pzpd_pread_all(fd, magic, 8, 0)) { close(fd); pzpd_set_error(PZPD_E_FORMAT, "%s is not a PZPD file", path); return NULL; }

    struct pzpd_archive *a = (struct pzpd_archive *) calloc(1, sizeof(struct pzpd_archive));
    if (a == NULL) { close(fd); pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return NULL; }
    a->flags = flags;
    pthread_mutex_init(&a->lock, NULL);

    int isShard = (memcmp(magic, PZPD_MAGIC_SHARD, 8) == 0);
    if ( isShard || ((memcmp(magic, PZPD_MAGIC_MANIFEST, 8) != 0) && (memcmp(magic, PZPD_MAGIC_COLL, 8) != 0)) )
    {
        //------------------------------------------------------------------
        // A single shard, opened standalone: it is its own archive. Without a
        // known magic at offset 0 it may still be a shard whose primary
        // superblock is gone (backup superblock, index sections)
        //------------------------------------------------------------------
        close(fd);
        a->standalone  = 1;
        a->shard_count = 1;
        a->shards = (struct pzpd_rshard *) calloc(1, sizeof(struct pzpd_rshard));
        if ( (a->shards == NULL) || ((a->shards[0].path = strdup(path)) == NULL) ) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); arch_close(a); return NULL; }
        a->shards[0].fd = -1;
        a->shards[0].record_count = 0xFFFFFFFFFFFFFFFFull;
        a->total = 0xFFFFFFFFFFFFFFFFull;                      // lets pzpd_shard_of() pick shard 0 before it is loaded
        if (pzpd_shard(a, 0) == NULL)
        {
            char msg[512];
            snprintf(msg, sizeof(msg), "%s", a->shards[0].error);
            int code = a->shards[0].error_code;
            arch_close(a);
            if (isShard) { pzpd_set_error(code, "%s", msg); }
            else         { pzpd_set_error(PZPD_E_FORMAT, "%s is not a PZPD file (bad magic)", path); }
            return NULL;
        }
        return a;
    }

    if (memcmp(magic, PZPD_MAGIC_COLL, 8) == 0)
    {
        close(fd);
        arch_close(a);
        pzpd_set_error(PZPD_E_FORMAT, "%s is a collection file; collections can't be members of collections", path);
        return NULL;
    }


    //----------------------------------------------------------------------
    // Manifest
    //----------------------------------------------------------------------
    struct stat st;
    if ( (fstat(fd, &st) != 0) || ((uint64_t) st.st_size < PZPD_BLOCK) ) { close(fd); arch_close(a); pzpd_set_error(PZPD_E_FORMAT, "%s: manifest is truncated", path); return NULL; }
    a->manifest_len  = (size_t) st.st_size;
    a->mmap_manifest = (unsigned char *) mmap(NULL, a->manifest_len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (a->mmap_manifest == MAP_FAILED) { a->mmap_manifest = NULL; arch_close(a); pzpd_set_error(PZPD_E_IO, "cannot map %s: %s", path, strerror(errno)); return NULL; }

    struct pzpd_disk_manifest mh;
    memcpy(&mh, a->mmap_manifest, sizeof(mh));
    uint64_t file = a->manifest_len;
    if (mh.version > PZPD_FORMAT_VERSION) { arch_close(a); pzpd_set_error(PZPD_E_VERSION, "%s: manifest format version %u is newer than this library (%d)", path, mh.version, PZPD_FORMAT_VERSION); return NULL; }
    if (mh.sb_checksum != XXH64(&mh, offsetof(struct pzpd_disk_manifest, sb_checksum), 0)) { arch_close(a); pzpd_set_error(PZPD_E_CHECKSUM, "%s: manifest header checksum mismatch", path); return NULL; }
    if ( (mh.stream_count == 0) || (mh.stream_count > PZPD_MAX_STREAMS) || (mh.shard_count == 0) ||
         !pzpd_check_section(a->mmap_manifest, file, mh.shards_offset, PZPD_SECT_MSHARDS, (uint64_t) mh.shard_count * sizeof(struct pzpd_disk_manifest_shard)) ||
         !pzpd_check_section(a->mmap_manifest, file, mh.names_offset,  PZPD_SECT_MNAMES,  mh.names_bytes) ||
         (mh.hash_count > file / sizeof(struct pzpd_disk_global_hash)) ||      // so the size below can't wrap around
         !pzpd_check_section(a->mmap_manifest, file, mh.hash_offset,   PZPD_SECT_MHASH,   mh.hash_count * sizeof(struct pzpd_disk_global_hash)) )
    {
        arch_close(a);
        pzpd_set_error(PZPD_E_FORMAT, "%s: manifest sections are damaged", path);
        return NULL;
    }
    a->S = mh.stream_count;
    for (unsigned i = 0; i < a->S; i++) { pzpd_get_slot_name(a->streams[i], mh.streams[i].name); }
    memcpy(a->uuid, mh.archive_uuid, 16);
    a->mwords_bad = (mh.ext_checksum != pzpd_ext_checksum(mh.words, PZPD_MH_EXT_BYTES));
    if (!a->mwords_bad) { memcpy(a->mwords, mh.words, sizeof(a->mwords)); }
    a->total       = mh.total_records;
    a->shard_count = mh.shard_count;
    a->mshards     = (const struct pzpd_disk_manifest_shard *) (a->mmap_manifest + mh.shards_offset);
    a->ghash       = (const struct pzpd_disk_global_hash *)    (a->mmap_manifest + mh.hash_offset);
    a->ghash_count = mh.hash_count;

    // Shard paths are relative to the manifest's directory
    const char *slash = strrchr(path, '/');
    size_t dirLen = (slash == NULL) ? 0 : (size_t)(slash - path + 1);
    const char *names = (const char *) (a->mmap_manifest + mh.names_offset);
    a->shards = (struct pzpd_rshard *) calloc(a->shard_count, sizeof(struct pzpd_rshard));
    if (a->shards == NULL) { arch_close(a); pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return NULL; }
    uint64_t expect = 0;
    for (unsigned i = 0; i < a->shard_count; i++)
    {
        const struct pzpd_disk_manifest_shard *ms = &a->mshards[i];
        struct pzpd_rshard *s = &a->shards[i];
        s->fd = -1;
        if ( !pzpd_in_file(ms->name_offset, ms->name_len, mh.names_bytes) || (ms->first_ordinal != expect) )
        {
            arch_close(a);
            pzpd_set_error(PZPD_E_FORMAT, "%s: shard table is damaged", path);
            return NULL;
        }
        expect += ms->record_count;
        s->first_ordinal = ms->first_ordinal;
        s->record_count  = ms->record_count;
        s->path = (char *) malloc(dirLen + ms->name_len + 1);
        if (s->path == NULL) { arch_close(a); pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return NULL; }
        memcpy(s->path, path, dirLen);
        memcpy(s->path + dirLen, names + ms->name_offset, ms->name_len);
        s->path[dirLen + ms->name_len] = 0;
    }
    if (expect != a->total) { arch_close(a); pzpd_set_error(PZPD_E_FORMAT, "%s: shard record counts don't add up", path); return NULL; }
    if (!pzpd_read_table_dir(a, mh.tables, a->mmap_manifest, file, NULL, -1, 1, path)) { struct pzpd_saved_error e; pzpd_error_save(&e); arch_close(a); pzpd_error_restore(&e); return NULL; }
    if (flags & PZPD_O_POPULATE)
    {
        for (unsigned i = 0; i < a->shard_count; i++) { (void) pzpd_shard(a, i); }   // missing shards fail later, on use
        pzpd_clear_error();
    }
    return a;
}

/** @brief Single-archive part of pzpd_close(): ordinals, shards and stream ids are local to the archive. */
static void arch_close(struct pzpd_archive *a)
{
    if (a == NULL) { return; }
    for (unsigned i = 0; (a->shards != NULL) && (i < a->shard_count); i++)
    {
        struct pzpd_rshard *s = &a->shards[i];
        if (s->map != NULL) { munmap(s->map, s->map_len); }
        if (s->fd >= 0) { close(s->fd); }
        free(s->path);
    }
    free(a->shards);
    if (a->mmap_manifest != NULL) { munmap(a->mmap_manifest, a->manifest_len); }
    pthread_mutex_destroy(&a->lock);
    free(a->tables);
    free(a);
}

/** @brief A shard's word index section for directory slot j, parsed. @return 1 on success, 0 on failure (error set). */
static int pzpd_shard_wsec(const struct pzpd_rshard *s, unsigned j, struct pzpd_wsec *v)
{
    if (s->words_bad) { pzpd_set_error(PZPD_E_CHECKSUM, "%s: the word index directory is damaged (rebuild it with reindex)", s->path); return 0; }
    const struct pzpd_disk_words *d = &s->sb.words[j];
    if ( !pzpd_check_section(s->map, s->map_len, d->section_offset, PZPD_SECT_WORDS, d->section_bytes) ||
         !pzpd_wsec_parse(s->map + d->section_offset, d->section_bytes, 0, v) )
    {
        if (pzpd_errorText[0] == 0) { pzpd_set_error(PZPD_E_FORMAT, "section is damaged"); }
        pzpd_error_wrap(PZPD_E_FORMAT, "%s: word index %s.%s", s->path, d->table, d->column);
        return 0;
    }
    return 1;
}

/** @brief Word directory slot of (table, column) in a directory, or -1. */
static int pzpd_words_slot(const struct pzpd_disk_words *dir, const char *table, const char *column)
{
    for (unsigned j = 0; (j < PZPD_MAX_WORD_INDEXES) && (dir[j].table[0] != 0); j++)
    {
        if ( !strncmp(dir[j].table, table, sizeof(dir[j].table)) && !strncmp(dir[j].column, column, sizeof(dir[j].column)) ) { return (int) j; }
    }
    return -1;
}

/** @brief Word directory entries in use. */
static unsigned pzpd_words_used(const struct pzpd_disk_words *dir)
{
    unsigned n = 0;
    while ( (n < PZPD_MAX_WORD_INDEXES) && (dir[n].table[0] != 0) ) { n++; }
    return n;
}

/** @brief One (word, stats) entry gathered from several vocabularies before they are merged. */
struct pzpd_went
{
    const char *w;       ///< Word bytes
    uint32_t    len;     ///< Length
    uint64_t    records; ///< Records containing it
    uint64_t    count;   ///< Occurrences
};

static int pzpd_cmp_went(const void *a, const void *b)
{
    const struct pzpd_went *x = (const struct pzpd_went *) a, *y = (const struct pzpd_went *) b;
    return pzpd_word_cmp(x->w, x->len, y->w, y->len);
}

/** @brief A byte string pointer + length (source values). */
struct pzpd_bstr { const char *s; uint32_t len; };

static int pzpd_cmp_bstr(const void *a, const void *b)
{
    const struct pzpd_bstr *x = (const struct pzpd_bstr *) a, *y = (const struct pzpd_bstr *) b;
    return pzpd_word_cmp(x->s, x->len, y->s, y->len);
}

/** @brief Sort and fold entries with equal words (summing their stats). @return The number left. */
static size_t pzpd_went_fold(struct pzpd_went *e, size_t n)
{
    if (n == 0) { return 0; }
    qsort(e, n, sizeof(*e), pzpd_cmp_went);
    size_t o = 0;
    for (size_t i = 1; i < n; i++)
    {
        if (pzpd_word_cmp(e[o].w, e[o].len, e[i].w, e[i].len) == 0) { e[o].records += e[i].records; e[o].count += e[i].count; }
        else { e[++o] = e[i]; }
    }
    return o + 1;
}

/** @brief Build the manifest's word sections (kind 15) from the shards of an archive, each opened standalone
 *  (spec §4.10): per word index and per sub-index (merged, then the union of the shards' source values sorted),
 *  the vocabulary with totals over every shard. Every shard must declare the same word indexes.
 *  @return 1 on success (W sections in secs, directory in dir), 0 on failure (error set). */
static int pzpd_mwords_sections(struct pzpd_archive *const *shards, unsigned n, struct pzpd_buf secs[PZPD_MAX_WORD_INDEXES],
                                struct pzpd_disk_words dir[PZPD_MAX_WORD_INDEXES], unsigned *W)
{
    memset(dir, 0, sizeof(struct pzpd_disk_words) * PZPD_MAX_WORD_INDEXES);
    *W = 0;
    if (n == 0) { return 1; }
    const struct pzpd_rshard *s0 = &shards[0]->shards[0];
    unsigned nw = pzpd_words_used(s0->sb.words);
    struct pzpd_wsec *views = (struct pzpd_wsec *) calloc(n ? n : 1, sizeof(struct pzpd_wsec));
    if (views == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    int ok = 1;
    for (unsigned k = 1; ok && (k < n); k++)
    {
        if (pzpd_words_used(shards[k]->shards[0].sb.words) != nw) { pzpd_set_error(PZPD_E_FORMAT, "%s has other word indexes than shard 0", shards[k]->shards[0].path); ok = 0; }
    }
    for (unsigned j = 0; ok && (j < nw); j++)
    {
        // Every shard: the same index, parsed
        for (unsigned k = 0; ok && (k < n); k++)
        {
            const struct pzpd_rshard *s = &shards[k]->shards[0];
            if ( (pzpd_words_used(s->sb.words) != nw) || (pzpd_words_slot(s->sb.words, s0->sb.words[j].table, s0->sb.words[j].column) != (int) j) )
                { pzpd_set_error(PZPD_E_FORMAT, "%s has other word indexes than shard 0", s->path); ok = 0; break; }
            ok = pzpd_shard_wsec(s, j, &views[k]);
            if (ok && strncmp(views[k].source_column, views[0].source_column, sizeof(views[0].source_column)))
                { pzpd_set_error(PZPD_E_FORMAT, "%s: word index %s.%s has another source column than shard 0", s->path, s0->sb.words[j].table, s0->sb.words[j].column); ok = 0; }
        }
        // Source values over all shards
        struct pzpd_bstr *src = NULL;
        size_t ns = 0, cap = 0;
        for (unsigned k = 0; ok && (k < n); k++)
        {
            for (unsigned q = 1; ok && (q < views[k].nsub); q++)
            {
                struct pzpd_wsub sub;
                ok = pzpd_wsec_sub(&views[k], q, (int64_t) shards[k]->shards[0].sb.record_count, &sub);
                if (!ok) { break; }
                if (ns == cap) { cap = cap ? cap * 2 : 16; struct pzpd_bstr *nsrc = (struct pzpd_bstr *) realloc(src, cap * sizeof(*src)); if (nsrc == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; break; } src = nsrc; }
                src[ns].s = sub.source; src[ns].len = sub.source_len; ns++;
            }
        }
        if (ok && (ns > 0))
        {
            qsort(src, ns, sizeof(*src), pzpd_cmp_bstr);
            size_t o = 0;
            for (size_t i = 1; i < ns; i++) { if (pzpd_cmp_bstr(&src[o], &src[i]) != 0) { src[++o] = src[i]; } }
            ns = o + 1;
        }
        if (ok && (ns > PZPD_MAX_WORD_SOURCES)) { pzpd_set_error(PZPD_E_FORMAT, "word index: more than %d source values over the shards", PZPD_MAX_WORD_SOURCES); ok = 0; }

        // Each sub-index: gather every shard's vocabulary, fold equal words
        struct pzpd_buf names = {0}, heads = {0}, parts = {0};
        struct pzpd_buf ents = {0};
        for (size_t q = 0; ok && (q <= ns); q++)
        {
            ents.len = 0;
            for (unsigned k = 0; ok && (k < n); k++)
            {
                int si = pzpd_wsec_find(&views[k], (q == 0) ? NULL : src[q - 1].s, (q == 0) ? 0 : src[q - 1].len, (int64_t) shards[k]->shards[0].sb.record_count);
                if (si == -2) { ok = 0; break; }
                if (si < 0) { continue; }                          // this shard has no rows of that source
                struct pzpd_wsub sub;
                ok = pzpd_wsec_sub(&views[k], (unsigned) si, (int64_t) shards[k]->shards[0].sb.record_count, &sub);
                for (uint64_t w = 0; ok && (w < sub.words); w++)
                {
                    struct pzpd_went e;
                    size_t l = 0;
                    ok = pzpd_wsub_word(&sub, w, &e.w, &l);
                    e.len = (uint32_t) l; e.records = sub.vocab[w].records; e.count = sub.vocab[w].count;
                    ok = ok && pzpd_buf_append(&ents, &e, sizeof(e));
                }
            }
            if (!ok) { break; }
            size_t ne = pzpd_went_fold((struct pzpd_went *) ents.data, ents.len / sizeof(struct pzpd_went));
            const struct pzpd_went *e = (const struct pzpd_went *) ents.data;
            struct pzpd_disk_msubindex mh;
            memset(&mh, 0, sizeof(mh));
            if (q > 0) { mh.source_offset = (uint32_t) names.len; mh.source_len = src[q - 1].len; ok = pzpd_buf_append(&names, src[q - 1].s, src[q - 1].len); }
            mh.words = ne;
            // vocabulary then heap, relative to `parts` for now
            struct pzpd_buf heap = {0};
            mh.vocab_offset = parts.len;
            for (size_t i = 0; ok && (i < ne); i++)
            {
                struct pzpd_disk_mword mw = { (uint32_t) heap.len, (uint16_t) e[i].len, 0, e[i].records, e[i].count };
                if (heap.len + e[i].len > 0xFFFFFFFFull) { pzpd_set_error(PZPD_E_ARG, "word index: more than 4 GiB of words"); ok = 0; break; }
                ok = pzpd_buf_append(&parts, &mw, sizeof(mw)) && pzpd_buf_append(&heap, e[i].w, e[i].len);
            }
            ok = ok && pzpd_buf_pad8(&parts);
            mh.heap_offset = parts.len;
            mh.heap_bytes  = heap.len;
            ok = ok && pzpd_buf_append(&parts, heap.data, heap.len) && pzpd_buf_pad8(&parts) && pzpd_buf_append(&heads, &mh, sizeof(mh));
            pzpd_buf_free(&heap);
        }
        // Section: head, sub-index heads, names, parts (offsets shifted to the section start)
        if (ok)
        {
            struct pzpd_disk_words_head h;
            memset(&h, 0, sizeof(h));
            h.tokenizer      = PZPD_TOKENIZER_V1;
            h.subindex_count = (uint32_t)(ns + 1);
            memcpy(h.source_column, views[0].source_column, sizeof(h.source_column));
            h.names_offset = sizeof(h) + heads.len;
            h.names_bytes  = names.len;
            uint64_t base = pzpd_align_up(h.names_offset + names.len, 8);
            struct pzpd_disk_msubindex *mh = (struct pzpd_disk_msubindex *) heads.data;
            for (size_t q = 0; q <= ns; q++) { mh[q].vocab_offset += base; mh[q].heap_offset += base; }
            secs[j].len = 0;
            ok = pzpd_buf_append(&secs[j], &h, sizeof(h)) && pzpd_buf_append(&secs[j], heads.data, heads.len) &&
                 pzpd_buf_append(&secs[j], names.data, names.len) && pzpd_buf_pad8(&secs[j]) && pzpd_buf_append(&secs[j], parts.data, parts.len);
            memcpy(dir[j].table, s0->sb.words[j].table, sizeof(dir[j].table));
            memcpy(dir[j].column, s0->sb.words[j].column, sizeof(dir[j].column));
        }
        if (!ok && (pzpd_errorCode == PZPD_OK)) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); }
        pzpd_buf_free(&names); pzpd_buf_free(&heads); pzpd_buf_free(&parts); pzpd_buf_free(&ents);
        free(src);
    }
    free(views);
    if (ok) { *W = nw; }
    return ok;
}




/** @brief Single-archive part of pzpd_shard_info_get(): ordinals, shards and stream ids are local to the archive. */
static int arch_shard_info_get(struct pzpd_archive *a, unsigned shard, pzpd_shard_info *out)
{
    pzpd_clear_error();
    if ( (a == NULL) || (out == NULL) || (shard >= a->shard_count) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    struct pzpd_rshard *s = pzpd_shard(a, shard);
    out->path          = a->shards[shard].path;
    out->first_ordinal = a->shards[shard].first_ordinal;
    out->record_count  = a->shards[shard].record_count;
    out->available     = (s != NULL);
    out->file_bytes    = (s != NULL) ? s->sb.file_bytes : 0;
    out->storage       = (s != NULL) ? s->storage : PZPD_STORAGE_BLOCK;
    out->recovery      = (s != NULL) ? s->recovery : 0;
    pzpd_clear_error();
    return 1;
}

//-----------------------------------------------------------------------------------------------
// Lookup
//-----------------------------------------------------------------------------------------------

/** @brief First index i in a sorted array (entries of `stride` bytes, u64 hash at offset 0)
 *  with hash >= key. A few interpolation steps (hashes are uniform) then binary search. */
static uint64_t pzpd_lower_bound(const unsigned char *base, uint64_t n, size_t stride, uint64_t key)
{
    uint64_t lo = 0, hi = n;       // answer in [lo, hi]
    for (int step = 0; (step < 4) && (hi - lo > 32); step++)
    {
        uint64_t klo, khi;
        memcpy(&klo, base + lo * stride, 8);
        memcpy(&khi, base + (hi - 1) * stride, 8);
        if ( (key <= klo) || (key > khi) ) { break; }
        long double frac = (long double)(key - klo) / (long double)(khi - klo);
        uint64_t guess = lo + (uint64_t)(frac * (long double)(hi - 1 - lo));
        if (guess <= lo) { guess = lo + 1; }
        if (guess >= hi) { guess = hi - 1; }
        uint64_t kg;
        memcpy(&kg, base + guess * stride, 8);
        if (kg < key) { lo = guess + 1; } else { hi = guess; }
    }
    while (lo < hi)
    {
        uint64_t mid = lo + (hi - lo) / 2;
        uint64_t km;
        memcpy(&km, base + mid * stride, 8);
        if (km < key) { lo = mid + 1; } else { hi = mid; }
    }
    return lo;
}

/** @brief Compare stored key / name bytes of (ordinal, stream, kind) with the query.
 *  @return 1 if they match, 0 if not or if the shard is unavailable. */
/** @brief Group-table entry holding a record of a shard (binary search), or NULL if the record is in no group. */
static const struct pzpd_disk_group *pzpd_group_at(const struct pzpd_rshard *s, uint64_t local)
{
    if (s->groups == NULL) { return NULL; }
    uint64_t lo = 0, hi = s->sb.group_count;          // first entry with first_local > local, then one back
    while (lo < hi) { uint64_t mid = lo + (hi - lo) / 2; if (s->groups[mid].first_local <= local) { lo = mid + 1; } else { hi = mid; } }
    if (lo == 0) { return NULL; }
    const struct pzpd_disk_group *g = &s->groups[lo - 1];
    return (local < (uint64_t) g->first_local + g->frame_count) ? g : NULL;
}

static int pzpd_match(struct pzpd_archive *a, uint64_t ordinal, uint8_t stream, uint8_t kind, const char *key, size_t len)
{
    uint64_t local;
    struct pzpd_rshard *s = pzpd_locate(a, ordinal, &local);
    if (s == NULL) { return 0; }
    if (kind == PZPD_KIND_KEY)
    {
        const struct pzpd_disk_record *r = &s->rtab[local];
        if (!pzpd_in_file(r->key_offset, r->key_len, s->sb.heap_bytes)) { return 0; }
        return (r->key_len == len) && (memcmp(s->heap + r->key_offset, key, len) == 0);
    }
    if (kind == PZPD_KIND_GROUP)
    {
        const struct pzpd_disk_group *g = pzpd_group_at(s, local);
        return (g != NULL) && (g->first_local == local) && (g->name_len == len) && (memcmp(s->heap + g->name_offset, key, len) == 0);
    }
    if (stream >= s->sb.stream_count) { return 0; }
    const struct pzpd_disk_blob *b = pzpd_blob_entry(s, local, stream);
    if ( (b == NULL) || (b->rel_offset == PZPD_MISSING) ) { return 0; }
    return (b->name_len == len) && (memcmp(s->heap + b->name_offset, key, len) == 0);
}

/** @brief Single-archive part of pzpd_find(): ordinals, shards and stream ids are local to the archive.
 *  @param only_kind PZPD_KIND_KEY or PZPD_KIND_NAME to search one namespace, -1 for keys then names. */
static int64_t arch_find(struct pzpd_archive *a, const char *key, size_t len, int *stream_out, int only_kind)
{
    pzpd_clear_error();
    if ( (a == NULL) || (key == NULL) || (len == 0) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return -1; }
    uint64_t h = XXH64(key, len, 0);

    // Two passes: record keys first, then blob names (entries of one hash are sorted by kind)
    int kFirst = (only_kind < 0) ? PZPD_KIND_KEY  : only_kind;
    int kLast  = (only_kind < 0) ? PZPD_KIND_NAME : only_kind;
    for (int kind = kFirst; kind <= kLast; kind++)
    {
        if (a->standalone)
        {
            struct pzpd_rshard *s = pzpd_shard(a, 0);
            if (s == NULL) { return -1; }
            uint64_t n = s->sb.hash_count;
            for (uint64_t i = pzpd_lower_bound((const unsigned char *) s->hash, n, sizeof(struct pzpd_disk_hash), h); (i < n) && (s->hash[i].hash == h); i++)
            {
                if (s->hash[i].kind != kind) { continue; }
                if (pzpd_match(a, s->hash[i].local_ordinal, s->hash[i].stream, (uint8_t) kind, key, len))
                {
                    if (stream_out != NULL) { *stream_out = (kind == PZPD_KIND_KEY) ? -1 : (int) s->hash[i].stream; }
                    return (int64_t) s->hash[i].local_ordinal;
                }
            }
        }
        else
        {
            uint64_t n = a->ghash_count;
            for (uint64_t i = pzpd_lower_bound((const unsigned char *) a->ghash, n, sizeof(struct pzpd_disk_global_hash), h); (i < n) && (a->ghash[i].hash == h); i++)
            {
                if (a->ghash[i].kind != kind) { continue; }
                if (pzpd_match(a, a->ghash[i].ordinal, a->ghash[i].stream, (uint8_t) kind, key, len))
                {
                    if (stream_out != NULL) { *stream_out = (kind == PZPD_KIND_KEY) ? -1 : (int) a->ghash[i].stream; }
                    return (int64_t) a->ghash[i].ordinal;
                }
            }
        }
    }
    if (pzpd_errorCode == PZPD_OK) { pzpd_set_error(PZPD_E_NOTFOUND, "\"%.*s\" not found", (int)(len > 200 ? 200 : len), key); }
    return -1;
}

/** @brief Single-archive part of pzpd_record_key(): ordinals, shards and stream ids are local to the archive. */
static const char *arch_record_key(struct pzpd_archive *a, uint64_t ordinal, size_t *len)
{
    pzpd_clear_error();
    if (a == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL handle"); return NULL; }
    uint64_t local;
    struct pzpd_rshard *s = pzpd_locate(a, ordinal, &local);
    if (s == NULL) { return NULL; }
    const struct pzpd_disk_record *r = &s->rtab[local];
    if (!pzpd_in_file(r->key_offset, r->key_len, s->sb.heap_bytes)) { pzpd_set_error(PZPD_E_FORMAT, "%s: record key out of the heap", s->path); return NULL; }
    if (len != NULL) { *len = r->key_len; }
    return s->heap + r->key_offset;
}

/** @brief Single-archive part of pzpd_blob_info_get(): ordinals, shards and stream ids are local to the archive. */
static int arch_blob_info_get(struct pzpd_archive *a, uint64_t ordinal, unsigned stream, pzpd_blob_info *out)
{
    pzpd_clear_error();
    if ( (a == NULL) || (out == NULL) || (stream >= a->S) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    uint64_t local;
    struct pzpd_rshard *s = pzpd_locate(a, ordinal, &local);
    if (s == NULL) { return 0; }
    const struct pzpd_disk_blob *b = pzpd_blob_entry(s, local, stream);
    if (b == NULL) { return 0; }
    memset(out, 0, sizeof(*out));
    const struct pzpd_disk_record *r = &s->rtab[local];
    out->group = r->group;
    out->frame = r->frame;
    out->shard = (unsigned)(s - a->shards);
    if (b->rel_offset == PZPD_MISSING) { return 1; }
    out->present          = 1;
    out->size             = b->size;
    out->meta.format      = b->format;
    out->meta.width       = b->width;
    out->meta.height      = b->height;
    out->meta.channels    = b->channels;
    out->meta.frames      = b->frames;
    out->meta.bits        = b->bits;
    out->meta.meta_flags  = b->meta_flags;
    out->name             = s->heap + b->name_offset;
    out->name_len         = b->name_len;
    return 1;
}

//-----------------------------------------------------------------------------------------------
// Reading data
//-----------------------------------------------------------------------------------------------

/** @brief Find a blob's descriptor in its record header (for checksums), bounds-checked.
 *  @return 1 and the payload XXH32 in *xxh, 0 if the header is damaged (error set). */
static int pzpd_header_blob_xxh(const struct pzpd_rshard *s, uint64_t local, unsigned stream, uint32_t *xxh)
{
    const struct pzpd_disk_record *r = &s->rtab[local];
    struct pzpd_disk_record_header rh;
    if (r->bytes < sizeof(rh)) { pzpd_set_error(PZPD_E_FORMAT, "%s: record %llu is too small", s->path, (unsigned long long) local); return 0; }
    memcpy(&rh, s->map + r->offset, sizeof(rh));
    if ( (memcmp(rh.magic, PZPD_MAGIC_RECORD, 8) != 0) || (rh.header_bytes > r->bytes) ||
         (sizeof(rh) + (uint64_t) rh.blob_count * sizeof(struct pzpd_disk_record_blob) > rh.header_bytes) )
    {
        pzpd_set_error(PZPD_E_FORMAT, "%s: record header %llu is damaged", s->path, (unsigned long long) local);
        return 0;
    }
    for (unsigned i = 0; i < rh.blob_count; i++)
    {
        struct pzpd_disk_record_blob d;
        memcpy(&d, s->map + r->offset + sizeof(rh) + i * sizeof(d), sizeof(d));
        if (d.stream == stream) { *xxh = d.xxh32; return 1; }
    }
    pzpd_set_error(PZPD_E_FORMAT, "%s: record header %llu lacks stream %u", s->path, (unsigned long long) local, stream);
    return 0;
}

/** @brief Resolve (ordinal, stream) to its shard, record and blob entry.
 *  @return The blob entry (NULL on error, error set). */
static const struct pzpd_disk_blob *pzpd_resolve(struct pzpd_archive *a, uint64_t ordinal, unsigned stream, struct pzpd_rshard **so, uint64_t *localOut)
{
    if (a == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL handle"); return NULL; }
    if (stream >= a->S) { pzpd_set_error(PZPD_E_ARG, "stream %u out of range (%u streams)", stream, a->S); return NULL; }
    uint64_t local;
    struct pzpd_rshard *s = pzpd_locate(a, ordinal, &local);
    if (s == NULL) { return NULL; }
    const struct pzpd_disk_blob *b = pzpd_blob_entry(s, local, stream);
    *so = s;
    *localOut = local;
    return b;
}

/** @brief Single-archive part of pzpd_read_into(): ordinals, shards and stream ids are local to the archive. */
static ssize_t arch_read_into(struct pzpd_archive *a, uint64_t ordinal, unsigned stream, void *buf, size_t cap)
{
    pzpd_clear_error();
    struct pzpd_rshard *s;
    uint64_t local;
    const struct pzpd_disk_blob *b = pzpd_resolve(a, ordinal, stream, &s, &local);
    if (b == NULL) { return (ssize_t) pzpd_errorCode; }
    if (b->rel_offset == PZPD_MISSING) { return 0; }
    if ( (buf == NULL) || (cap < b->size) ) { pzpd_set_error(PZPD_E_ARG, "buffer of %zu bytes is too small for a %u byte blob", cap, b->size); return PZPD_E_ARG; }
    if (!pzpd_pread_all(s->fd, buf, b->size, s->rtab[local].offset + b->rel_offset)) { return (ssize_t) pzpd_errorCode; }
    if (a->flags & PZPD_O_VERIFY)
    {
        uint32_t want;
        if (!pzpd_header_blob_xxh(s, local, stream, &want)) { return (ssize_t) pzpd_errorCode; }
        if (XXH32(buf, b->size, 0) != want) { pzpd_set_error(PZPD_E_CHECKSUM, "%s: checksum mismatch in record %llu stream %u", s->path, (unsigned long long) local, stream); return PZPD_E_CHECKSUM; }
    }
    return (ssize_t) b->size;
}

/** @brief Single-archive part of pzpd_read_alloc(): ordinals, shards and stream ids are local to the archive. */
static void *arch_read_alloc(struct pzpd_archive *a, uint64_t ordinal, unsigned stream, size_t *size)
{
    pzpd_clear_error();
    if (size != NULL) { *size = 0; }
    struct pzpd_rshard *s;
    uint64_t local;
    const struct pzpd_disk_blob *b = pzpd_resolve(a, ordinal, stream, &s, &local);
    if ( (b == NULL) || (b->rel_offset == PZPD_MISSING) ) { return NULL; }
    void *buf = malloc(b->size > 0 ? b->size : 1);
    if (buf == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory for a %u byte blob", b->size); return NULL; }
    ssize_t r = arch_read_into(a, ordinal, stream, buf, b->size);
    if (r < 0) { free(buf); return NULL; }
    if (size != NULL) { *size = (size_t) r; }
    return buf;
}

void pzpd_free(void *ptr)
{
    free(ptr);
}

/** @brief Single-archive part of pzpd_view(): ordinals, shards and stream ids are local to the archive. */
static const void *arch_view(struct pzpd_archive *a, uint64_t ordinal, unsigned stream, size_t *size)
{
    pzpd_clear_error();
    if (size != NULL) { *size = 0; }
    struct pzpd_rshard *s;
    uint64_t local;
    const struct pzpd_disk_blob *b = pzpd_resolve(a, ordinal, stream, &s, &local);
    if ( (b == NULL) || (b->rel_offset == PZPD_MISSING) ) { return NULL; }
    const unsigned char *p = s->map + s->rtab[local].offset + b->rel_offset;
    if (a->flags & PZPD_O_VERIFY)
    {
        uint32_t want;
        if (!pzpd_header_blob_xxh(s, local, stream, &want)) { return NULL; }
        if (XXH32(p, b->size, 0) != want) { pzpd_set_error(PZPD_E_CHECKSUM, "%s: checksum mismatch in record %llu stream %u", s->path, (unsigned long long) local, stream); return NULL; }
    }
    if (size != NULL) { *size = b->size; }
    return p;
}

/** @brief Span [start,end) relative to the record covering the requested present blobs.
 *  @return 1 if at least one requested blob is present, 0 if none; -1 on error. */
static int pzpd_span(struct pzpd_archive *a, uint64_t ordinal, uint32_t mask, struct pzpd_rshard **so, uint64_t *localOut, uint64_t *start, uint64_t *end)
{
    if (a == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL handle"); return -1; }
    uint64_t local;
    struct pzpd_rshard *s = pzpd_locate(a, ordinal, &local);
    if (s == NULL) { return -1; }
    uint64_t lo = UINT64_MAX, hi = 0;
    for (unsigned st = 0; st < a->S; st++)
    {
        if ( !(mask & (1u << st)) ) { continue; }
        const struct pzpd_disk_blob *b = pzpd_blob_entry(s, local, st);
        if (b == NULL) { return -1; }
        if (b->rel_offset == PZPD_MISSING) { continue; }
        if (b->rel_offset < lo) { lo = b->rel_offset; }
        if ((uint64_t) b->rel_offset + b->size > hi) { hi = (uint64_t) b->rel_offset + b->size; }
    }
    *so = s;
    *localOut = local;
    if (lo == UINT64_MAX) { return 0; }
    *start = lo;
    *end   = hi;
    return 1;
}

/** @brief Single-archive part of pzpd_record_span(): ordinals, shards and stream ids are local to the archive. */
static size_t arch_record_span(struct pzpd_archive *a, uint64_t ordinal, uint32_t stream_mask)
{
    pzpd_clear_error();
    struct pzpd_rshard *s;
    uint64_t local, start, end;
    if (pzpd_span(a, ordinal, stream_mask, &s, &local, &start, &end) != 1) { return 0; }
    return (size_t)(end - start);
}

/** @brief Single-archive part of pzpd_read_record(): ordinals, shards and stream ids are local to the archive. */
static ssize_t arch_read_record(struct pzpd_archive *a, uint64_t ordinal, uint32_t stream_mask, void *buf, size_t cap, pzpd_blob_ref *refs)
{
    pzpd_clear_error();
    struct pzpd_rshard *s;
    uint64_t local, start = 0, end = 0;
    int r = pzpd_span(a, ordinal, stream_mask, &s, &local, &start, &end);
    if (r < 0) { return (ssize_t) pzpd_errorCode; }
    if (refs != NULL) { memset(refs, 0, sizeof(pzpd_blob_ref) * a->S); }
    if (r == 0) { return 0; }
    if ( (buf == NULL) || (cap < end - start) ) { pzpd_set_error(PZPD_E_ARG, "buffer of %zu bytes is too small for a %llu byte span", cap, (unsigned long long)(end - start)); return PZPD_E_ARG; }
    if (!pzpd_pread_all(s->fd, buf, (size_t)(end - start), s->rtab[local].offset + start)) { return (ssize_t) pzpd_errorCode; }
    for (unsigned st = 0; st < a->S; st++)
    {
        if ( !(stream_mask & (1u << st)) ) { continue; }
        const struct pzpd_disk_blob *b = &s->btab[local * s->sb.stream_count + st];
        if (b->rel_offset == PZPD_MISSING) { continue; }
        const unsigned char *p = (const unsigned char *) buf + (b->rel_offset - start);
        if (a->flags & PZPD_O_VERIFY)
        {
            uint32_t want;
            if (!pzpd_header_blob_xxh(s, local, st, &want)) { return (ssize_t) pzpd_errorCode; }
            if (XXH32(p, b->size, 0) != want) { pzpd_set_error(PZPD_E_CHECKSUM, "%s: checksum mismatch in record %llu stream %u", s->path, (unsigned long long) local, st); return PZPD_E_CHECKSUM; }
        }
        if (refs != NULL)
        {
            refs[st].data   = p;
            refs[st].size   = b->size;
            refs[st].format = b->format;
        }
    }
    return (ssize_t)(end - start);
}

/** @brief Single-archive part of pzpd_verify_record(): ordinals, shards and stream ids are local to the archive. */
static int arch_verify_record(struct pzpd_archive *a, uint64_t ordinal, int check_blobs)
{
    pzpd_clear_error();
    if (a == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL handle"); return 0; }
    uint64_t local;
    struct pzpd_rshard *s = pzpd_locate(a, ordinal, &local);
    if (s == NULL) { return 0; }
    const struct pzpd_disk_record *r = &s->rtab[local];
    struct pzpd_disk_record_header rh;
    if (r->bytes < sizeof(rh)) { pzpd_set_error(PZPD_E_FORMAT, "record %llu is too small", (unsigned long long) ordinal); return 0; }
    memcpy(&rh, s->map + r->offset, sizeof(rh));
    if ( (memcmp(rh.magic, PZPD_MAGIC_RECORD, 8) != 0) || (rh.header_bytes > r->bytes) || (rh.header_bytes < sizeof(rh)) || (rh.record_bytes != r->bytes) )
    {
        pzpd_set_error(PZPD_E_FORMAT, "record %llu: header is damaged", (unsigned long long) ordinal);
        return 0;
    }
    // Header checksum: over header_bytes with the checksum field zeroed (hash a copy in one call;
    // streaming a 4-byte piece makes xxHash compute a pointer before the piece, which is UB)
    unsigned char *copy = (unsigned char *) malloc(rh.header_bytes);
    if (copy == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    memcpy(copy, s->map + r->offset, rh.header_bytes);
    memset(copy + offsetof(struct pzpd_disk_record_header, checksum), 0, sizeof(rh.checksum));
    uint32_t got = XXH32(copy, rh.header_bytes, 0);
    free(copy);
    if ( (got != rh.checksum) || (got != r->checksum) )
    {
        pzpd_set_error(PZPD_E_CHECKSUM, "record %llu: header checksum mismatch", (unsigned long long) ordinal);
        return 0;
    }
    if (!check_blobs) { return 1; }
    for (unsigned i = 0; i < rh.blob_count; i++)
    {
        struct pzpd_disk_record_blob d;
        if (sizeof(rh) + (uint64_t)(i + 1) * sizeof(d) > rh.header_bytes) { pzpd_set_error(PZPD_E_FORMAT, "record %llu: header is damaged", (unsigned long long) ordinal); return 0; }
        memcpy(&d, s->map + r->offset + sizeof(rh) + i * sizeof(d), sizeof(d));
        if (!pzpd_in_file(d.rel_offset, d.size, r->bytes)) { pzpd_set_error(PZPD_E_FORMAT, "record %llu: blob outside the record", (unsigned long long) ordinal); return 0; }
        if (XXH32(s->map + r->offset + d.rel_offset, d.size, 0) != d.xxh32)
        {
            pzpd_set_error(PZPD_E_CHECKSUM, "record %llu stream %u: payload checksum mismatch", (unsigned long long) ordinal, d.stream);
            return 0;
        }
    }
    return 1;
}

/** @brief Single-archive part of pzpd_verify_shard(): ordinals, shards and stream ids are local to the archive. */
static int arch_verify_shard(struct pzpd_archive *a, unsigned shard)
{
    pzpd_clear_error();
    if ( (a == NULL) || (shard >= a->shard_count) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    struct pzpd_rshard *s = pzpd_shard(a, shard);
    if (s == NULL) { return 0; }
    uint64_t offs[6 + PZPD_MAX_TABLES]  = { s->sb.rtab_offset, s->sb.btab_offset, s->sb.hash_offset, s->sb.heap_offset, s->sb.meta_offset };
    uint32_t kinds[6 + PZPD_MAX_TABLES] = { PZPD_SECT_RECORDS, PZPD_SECT_BLOBS, PZPD_SECT_HASH, PZPD_SECT_HEAP, PZPD_SECT_META };
    int nsec = 5;
    if (s->sb.group_count > 0) { offs[nsec] = s->sb.groups_offset; kinds[nsec] = PZPD_SECT_GROUPS; nsec++; }
    for (unsigned t = 0; t < a->T; t++) { offs[nsec] = s->sb.tables[t].section_offset; kinds[nsec] = PZPD_SECT_TABLE; nsec++; }
    XXH64_state_t *idx = XXH64_createState();
    if (idx == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    XXH64_reset(idx, 0);
    int ok = 1;
    for (int i = 0; ok && (i < nsec); i++)
    {
        struct pzpd_disk_section sh;
        if (offs[i] < sizeof(sh)) { ok = 0; break; }
        memcpy(&sh, s->map + offs[i] - sizeof(sh), sizeof(sh));
        if ( (memcmp(sh.magic, PZPD_MAGIC_SECTION, 8) != 0) || (sh.kind != kinds[i]) || !pzpd_in_file(offs[i], sh.bytes, s->map_len) ) { ok = 0; break; }
        if (XXH64(s->map + offs[i], (size_t) sh.bytes, 0) != sh.checksum) { ok = 0; break; }
        XXH64_update(idx, s->map + offs[i], (size_t) sh.bytes);
    }
    uint64_t digest = XXH64_digest(idx);
    if (!ok || (digest != s->sb.index_checksum))
    {
        XXH64_freeState(idx);
        pzpd_set_error(PZPD_E_CHECKSUM, "%s: index checksum mismatch", s->path);
        return 0;
    }
    // Word indexes: directory, section checksums, words_checksum, then the full structure of every sub-index
    if (s->words_bad) { XXH64_freeState(idx); pzpd_set_error(PZPD_E_CHECKSUM, "%s: word index directory checksum mismatch", s->path); return 0; }
    unsigned nw = pzpd_words_used(s->sb.words);
    XXH64_reset(idx, 0);
    for (unsigned j = 0; ok && (j < nw); j++)
    {
        const struct pzpd_disk_words *d = &s->sb.words[j];
        struct pzpd_disk_section sh;
        struct pzpd_wsec v;
        ok = pzpd_shard_wsec(s, j, &v);
        if (ok) { memcpy(&sh, s->map + d->section_offset - sizeof(sh), sizeof(sh)); }
        if (ok && (XXH64(s->map + d->section_offset, (size_t) d->section_bytes, 0) != sh.checksum))
            { pzpd_set_error(PZPD_E_CHECKSUM, "%s: word index %s.%s: section checksum mismatch", s->path, d->table, d->column); ok = 0; }
        if (ok) { XXH64_update(idx, s->map + d->section_offset, (size_t) d->section_bytes); }
        for (unsigned k = 0; ok && (k < v.nsub); k++)
        {
            struct pzpd_wsub sub;
            ok = pzpd_wsec_sub(&v, k, (int64_t) s->sb.record_count, &sub) && pzpd_wsub_check_full(&sub);
            if (!ok) { pzpd_error_wrap(PZPD_E_FORMAT, "%s: word index %s.%s", s->path, d->table, d->column); }
        }
    }
    digest = XXH64_digest(idx);
    XXH64_freeState(idx);
    if (ok && (((nw > 0) && (digest != s->sb.words_checksum)) || ((nw == 0) && (s->sb.words_checksum != 0))))
        { pzpd_set_error(PZPD_E_CHECKSUM, "%s: word index checksum mismatch", s->path); ok = 0; }
    return ok;
}

#if PZPDIR_WITH_PZP
/** @brief Single-archive part of pzpd_read_pzp(): ordinals, shards and stream ids are local to the archive. */
static unsigned char *arch_read_pzp(struct pzpd_archive *a, uint64_t ordinal, unsigned stream,
                             unsigned int *width, unsigned int *height,
                             unsigned int *bpp, unsigned int *channels)
{
    size_t size = 0;
    const void *data = arch_view(a, ordinal, stream, &size);
    if (data == NULL)
    {
        if (pzpd_errorCode == PZPD_OK) { pzpd_set_error(PZPD_E_NOTFOUND, "record %llu has no blob in stream %u", (unsigned long long) ordinal, stream); }
        return NULL;
    }
    unsigned int w = 0, h = 0, be = 0, ce = 0, bi = 0, ci = 0, cfg = 0;
    unsigned char *px = pzp_decompress_combined_from_memory(data, size, &w, &h, &be, &ce, &bi, &ci, &cfg);
    if (px == NULL) { pzpd_set_error(PZPD_E_FORMAT, "record %llu stream %u: not a decodable PZP blob", (unsigned long long) ordinal, stream); return NULL; }
    if (width    != NULL) { *width    = w;  }
    if (height   != NULL) { *height   = h;  }
    if (bpp      != NULL) { *bpp      = be; }
    if (channels != NULL) { *channels = ce; }
    return px;
}
#endif

/** @brief Rows of a record table for one record of an archive.
 *  @return Row count (0 with no error set when there are none), 0 with error set on failure. */
static uint32_t arch_table_rows(struct pzpd_archive *a, uint64_t ordinal, unsigned t, const void **rows_out)
{
    if (rows_out != NULL) { *rows_out = NULL; }
    if (t >= a->T) { pzpd_set_error(PZPD_E_ARG, "table %u out of range", t); return 0; }
    if (a->tables[t].flags & PZPD_TABLE_GLOBAL) { pzpd_set_error(PZPD_E_ARG, "table %s is global; use pzpd_global_rows()", a->tables[t].name); return 0; }
    uint64_t local;
    struct pzpd_rshard *s = pzpd_locate(a, ordinal, &local);
    if (s == NULL) { return 0; }
    const struct pzpd_tview *v = &s->tv[t];
    if (v->index == NULL) { pzpd_set_error(PZPD_E_FORMAT, "%s: table %s has no row index", s->path, a->tables[t].name); return 0; }
    uint32_t start = v->index[local], end = v->index[local + 1];
    if ( (end < start) || (end > v->rows) ) { pzpd_set_error(PZPD_E_FORMAT, "%s: table %s: row index of record %llu is damaged", s->path, a->tables[t].name, (unsigned long long) local); return 0; }
    uint32_t n = end - start;
    if ( (rows_out != NULL) && (n > 0) ) { *rows_out = v->rowdata + (size_t) start * a->tables[t].stride; }
    return n;
}

/** @brief Resolve a `str` field against a table view's heap.
 *  @return The bytes, or NULL if the field points outside the heap (error set). */
static const char *pzpd_view_str(const struct pzpd_tview *v, const void *field, size_t *len)
{
    pzpd_str sv;
    if (field == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL field"); return NULL; }
    memcpy(&sv, field, sizeof(sv));
    if (!pzpd_in_file(sv.offset, sv.len, v->heap_bytes)) { pzpd_set_error(PZPD_E_FORMAT, "string field points outside the table's strings"); return NULL; }
    if (len != NULL) { *len = sv.len; }
    return v->heap + sv.offset;
}

/** @brief Render a set of rows as CSV into a caller buffer (snprintf-like).
 *  @return Full length, or a negative enum pzpd_error. */
static ssize_t pzpd_rows_csv(const struct pzpd_tschema *sc, const unsigned char *rows, uint32_t n, const struct pzpd_tview *v, char *out, size_t cap)
{
    struct pzpd_buf b = {0};
    for (uint32_t r = 0; r < n; r++)
    {
        if (!pzpd_csv_render(sc, rows + (size_t) r * sc->stride, v->heap, v->heap_bytes, &b)) { pzpd_buf_free(&b); pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return PZPD_E_NOMEM; }
    }
    if ( (out != NULL) && (cap > 0) )
    {
        size_t k = (b.len < cap - 1) ? b.len : cap - 1;
        if (k > 0) { memcpy(out, b.data, k); }
        out[k] = 0;
    }
    ssize_t len = (ssize_t) b.len;
    pzpd_buf_free(&b);
    return len;
}

//-----------------------------------------------------------------------------------------------
// Collections: one or several archives behind one handle. Every public read call goes through
// here: an ordinal is routed to its member archive, stream ids are mapped between the merged
// ("union") stream list and the member's own, and the arch_* functions above do the work.
//-----------------------------------------------------------------------------------------------

#pragma pack(push,1)
/** @brief Collection file header: a 4 KiB slot at offset 0 of the collection file. */
struct pzpd_disk_collection
{
    char     magic[8];          ///< PZPD_MAGIC_COLL
    uint32_t version;           ///< PZPD_FORMAT_VERSION
    uint32_t flags;             ///< Reserved, 0
    uint64_t total_records;     ///< Sum of the members' record counts
    uint32_t member_count;      ///< Members
    uint32_t stream_count;      ///< Merged streams
    struct pzpd_disk_stream streams[PZPD_MAX_STREAMS]; ///< Merged stream names (first-seen order)
    uint64_t members_offset;    ///< Member table data
    uint64_t heap_offset;       ///< Paths and aliases
    uint64_t heap_bytes;        ///< Size of the heap
    uint64_t remap_offset;      ///< Stream remap tables (member_count × PZPD_MAX_STREAMS bytes)
    uint64_t file_bytes;        ///< Collection file size
    uint64_t index_checksum;    ///< XXH64 of the member, heap and remap section data
    uint64_t sb_checksum;       ///< XXH64 of every byte of this structure before this field
};

/** @brief Collection member entry (56 bytes). */
struct pzpd_disk_member
{
    uint64_t first_ordinal;     ///< First ordinal of the member in the collection
    uint64_t record_count;      ///< Records of the member when the collection was written
    uint8_t  archive_uuid[16];  ///< Identity of the member archive when the collection was written
    uint32_t path_offset;       ///< Member path in the heap (relative to the collection file's directory, or absolute)
    uint32_t path_len;          ///< Path length
    uint32_t alias_offset;      ///< Alias in the heap
    uint32_t alias_len;         ///< Alias length
    uint32_t flags;             ///< bit0: path is absolute
    uint32_t pad;               ///< 0
};
#pragma pack(pop)

_Static_assert(sizeof(struct pzpd_disk_collection) <= PZPD_BLOCK, "collection header fits its slot");
_Static_assert(sizeof(struct pzpd_disk_member) == 56, "collection member entry");

/** @brief One member of an open handle. */
struct pzpd_member
{
    char    *alias;                          ///< Member name
    char    *path;                           ///< Path the archive was opened from
    struct pzpd_archive *arch;               ///< The open archive, NULL when missing
    char     error[512];                     ///< Why it is missing
    uint64_t first;                          ///< First ordinal over the whole handle
    uint64_t count;                          ///< Records (from the archive, or the collection file when missing)
    unsigned shard_base;                     ///< Index of its shard 0 among all members' shards
    unsigned shards;                         ///< Its shard count (0 when missing)
    int      to_member[PZPD_MAX_STREAMS];    ///< Merged stream -> member stream, -1 if the member lacks it
    int      to_union[PZPD_MAX_STREAMS];     ///< Member stream -> merged stream
    int      to_mtable[PZPD_MAX_TABLES];     ///< Merged table -> member table, -1 if the member lacks it
};

/** @brief Public handle: members plus the merged stream list. A single archive is one member. */
struct pzpd
{
    unsigned flags;                          ///< Open flags
    unsigned S;                              ///< Merged stream count
    char     streams[PZPD_MAX_STREAMS][24];  ///< Merged stream names
    uint64_t total;                          ///< Records over all members
    unsigned member_count;                   ///< Members
    struct pzpd_member *m;                   ///< Members, in ordinal order
    unsigned shard_total;                    ///< Shards over all members
    unsigned T;                              ///< Merged tables
    const struct pzpd_tschema *tables[PZPD_MAX_TABLES]; ///< Their schemas (owned by the first member that has each)
};

/** @brief Merge a member's tables by name; a same-named table must have the same schema.
 *  @return 1 on success, 0 on failure (error set). */
static int pzpd_bind_tables(pzpd *a, struct pzpd_member *mb)
{
    for (unsigned u = 0; u < PZPD_MAX_TABLES; u++) { mb->to_mtable[u] = -1; }
    if (mb->arch == NULL) { return 1; }
    for (unsigned t = 0; t < mb->arch->T; t++)
    {
        const struct pzpd_tschema *sc = &mb->arch->tables[t];
        int u = -1;
        for (unsigned k = 0; k < a->T; k++) { if (!strcmp(a->tables[k]->name, sc->name)) { u = (int) k; } }
        if (u < 0)
        {
            if (a->T >= PZPD_MAX_TABLES) { pzpd_set_error(PZPD_E_ARG, "the members have more than %d distinct tables", PZPD_MAX_TABLES); return 0; }
            u = (int) a->T;
            a->tables[a->T++] = sc;
        }
        else if (!pzpd_schema_equal(a->tables[u], sc)) { pzpd_set_error(PZPD_E_FORMAT, "table \"%s\" has a different schema in member \"%s\"", sc->name, mb->alias); return 0; }
        mb->to_mtable[u] = (int) t;
    }
    return 1;
}

/** @brief Default alias: the file name without directories and without ".pzpd". Caller frees. */
static char *pzpd_default_alias(const char *path)
{
    const char *b = strrchr(path, '/');
    b = (b == NULL) ? path : b + 1;
    size_t n = strlen(b);
    if ( (n > 5) && (strcmp(b + n - 5, ".pzpd") == 0) ) { n -= 5; }
    char *r = (char *) malloc(n + 1);
    if (r != NULL) { memcpy(r, b, n); r[n] = 0; }
    return r;
}

/** @brief Find or add a merged stream.
 *  @return Merged stream id, or -1 when PZPD_MAX_STREAMS would be exceeded (error set). */
static int pzpd_union_stream(pzpd *a, const char *name)
{
    for (unsigned s = 0; s < a->S; s++) { if (strcmp(a->streams[s], name) == 0) { return (int) s; } }
    if (a->S >= PZPD_MAX_STREAMS) { pzpd_set_error(PZPD_E_ARG, "the members have more than %d distinct streams", PZPD_MAX_STREAMS); return -1; }
    snprintf(a->streams[a->S], sizeof(a->streams[a->S]), "%s", name);
    return (int)(a->S++);
}

/** @brief Map a member's streams into the merged list (adding new names).
 *  @return 1 on success, 0 on failure (error set). */
static int pzpd_bind_streams(pzpd *a, struct pzpd_member *mb)
{
    for (unsigned u = 0; u < PZPD_MAX_STREAMS; u++) { mb->to_member[u] = -1; mb->to_union[u] = -1; }
    if (mb->arch == NULL) { return 1; }
    for (unsigned s = 0; s < mb->arch->S; s++)
    {
        int u = pzpd_union_stream(a, mb->arch->streams[s]);
        if (u < 0) { return 0; }
        mb->to_member[u] = (int) s;
        mb->to_union[s]  = u;
    }
    return 1;
}

/** @brief Compute first ordinals, shard bases and totals after the members are set up. */
static void pzpd_layout(pzpd *a)
{
    uint64_t first = 0;
    unsigned shard = 0;
    for (unsigned i = 0; i < a->member_count; i++)
    {
        struct pzpd_member *mb = &a->m[i];
        if (mb->arch != NULL) { mb->count = mb->arch->total; mb->shards = mb->arch->shard_count; }
        mb->first      = first;
        mb->shard_base = shard;
        first += mb->count;
        shard += mb->shards;
    }
    a->total       = first;
    a->shard_total = shard;
}

/** @brief Allocate an empty handle with n members. */
static pzpd *pzpd_alloc(unsigned n, unsigned flags)
{
    pzpd *a = (pzpd *) calloc(1, sizeof(pzpd));
    if (a == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return NULL; }
    a->m = (struct pzpd_member *) calloc(n, sizeof(struct pzpd_member));
    if (a->m == NULL) { free(a); pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return NULL; }
    a->member_count = n;
    a->flags = flags;
    return a;
}

/** @brief Check that every alias is unique.
 *  @return 1 if they are, 0 otherwise (error set). */
static int pzpd_check_aliases(const pzpd *a)
{
    for (unsigned i = 0; i < a->member_count; i++)
    {
        if ( (a->m[i].alias == NULL) || (a->m[i].alias[0] == 0) ) { pzpd_set_error(PZPD_E_ARG, "member %u has an empty alias", i); return 0; }
        for (unsigned j = 0; j < i; j++)
        {
            if (strcmp(a->m[i].alias, a->m[j].alias) == 0) { pzpd_set_error(PZPD_E_ARG, "alias \"%s\" is used by two members; give explicit aliases", a->m[i].alias); return 0; }
        }
    }
    return 1;
}

void pzpd_close(pzpd *a)
{
    if (a == NULL) { return; }
    for (unsigned i = 0; (a->m != NULL) && (i < a->member_count); i++)
    {
        arch_close(a->m[i].arch);
        free(a->m[i].alias);
        free(a->m[i].path);
    }
    free(a->m);
    free(a);
}

pzpd *pzpd_open_many(const char *const *paths, const char *const *aliases, unsigned n, unsigned int flags)
{
    pzpd_clear_error();
    if ( (paths == NULL) || (n == 0) || (n > PZPD_MAX_MEMBERS) ) { pzpd_set_error(PZPD_E_ARG, "need 1..%d member paths", PZPD_MAX_MEMBERS); return NULL; }
    pzpd *a = pzpd_alloc(n, flags);
    if (a == NULL) { return NULL; }
    for (unsigned i = 0; i < n; i++)
    {
        struct pzpd_member *mb = &a->m[i];
        mb->path  = strdup(paths[i]);
        mb->alias = ( (aliases != NULL) && (aliases[i] != NULL) ) ? strdup(aliases[i]) : pzpd_default_alias(paths[i]);
        if ( (mb->path == NULL) || (mb->alias == NULL) ) { pzpd_close(a); pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return NULL; }
        mb->arch = arch_open(paths[i], flags & ~PZPD_O_ALLOW_MISSING);   // VERIFY, HUGEPAGE, POPULATE act per archive
        if (mb->arch == NULL)
        {
            snprintf(mb->error, sizeof(mb->error), "%s", pzpd_errorText);
            if ( !(flags & PZPD_O_ALLOW_MISSING) )
            {
                char msg[600];
                int code = pzpd_errorCode;
                snprintf(msg, sizeof(msg), "member %u (%s): %s", i, paths[i], mb->error);
                pzpd_close(a);
                pzpd_set_error(code, "%s", msg);
                return NULL;
            }
        }
        if (!pzpd_bind_streams(a, mb) || !pzpd_bind_tables(a, mb)) { struct pzpd_saved_error e; pzpd_error_save(&e); pzpd_close(a); pzpd_error_restore(&e); return NULL; }
    }
    if (!pzpd_check_aliases(a)) { pzpd_close(a); return NULL; }
    pzpd_layout(a);
    pzpd_clear_error();
    return a;
}

/** @brief Read a whole small file into memory.
 *  @return malloc'd buffer, NULL on failure (error set). */
static unsigned char *pzpd_slurp(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { pzpd_set_error(PZPD_E_IO, "cannot open %s: %s", path, strerror(errno)); return NULL; }
    struct stat st;
    if ( (fstat(fd, &st) != 0) || (st.st_size < (off_t) PZPD_BLOCK) || (st.st_size > (off_t)(1ull << 31)) )
    {
        close(fd);
        pzpd_set_error(PZPD_E_FORMAT, "%s: bad collection file size", path);
        return NULL;
    }
    unsigned char *b = (unsigned char *) malloc((size_t) st.st_size);
    if (b == NULL) { close(fd); pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return NULL; }
    int ok = pzpd_pread_all(fd, b, (size_t) st.st_size, 0);
    close(fd);
    if (!ok) { free(b); return NULL; }
    *len = (size_t) st.st_size;
    return b;
}

/** @brief A parsed collection file (pointers into its buffer). */
struct pzpd_coll_file
{
    unsigned char *buf;                        ///< Whole file
    size_t         len;                        ///< File size
    struct pzpd_disk_collection h;             ///< Header copy
    const struct pzpd_disk_member *members;    ///< Member table
    const char    *heap;                       ///< Paths and aliases
    const uint8_t *remap;                      ///< Remap tables
};

/** @brief Read and validate a collection file.
 *  @return 1 on success, 0 on failure (error set; c->buf freed). */
static int pzpd_coll_parse(const char *path, struct pzpd_coll_file *c)
{
    memset(c, 0, sizeof(*c));
    c->buf = pzpd_slurp(path, &c->len);
    if (c->buf == NULL) { return 0; }
    memcpy(&c->h, c->buf, sizeof(c->h));
    const struct pzpd_disk_collection *h = &c->h;
    int ok = 1;
    if (memcmp(h->magic, PZPD_MAGIC_COLL, 8) != 0) { pzpd_set_error(PZPD_E_FORMAT, "%s is not a collection file", path); ok = 0; }
    else if (h->version > PZPD_FORMAT_VERSION) { pzpd_set_error(PZPD_E_VERSION, "%s: collection format version %u is newer than this library (%d)", path, h->version, PZPD_FORMAT_VERSION); ok = 0; }
    else if (h->sb_checksum != XXH64(h, offsetof(struct pzpd_disk_collection, sb_checksum), 0)) { pzpd_set_error(PZPD_E_CHECKSUM, "%s: collection header checksum mismatch", path); ok = 0; }
    else if ( (h->member_count == 0) || (h->member_count > PZPD_MAX_MEMBERS) || (h->stream_count > PZPD_MAX_STREAMS) ||
              !pzpd_check_section(c->buf, c->len, h->members_offset, PZPD_SECT_CMEMBERS, (uint64_t) h->member_count * sizeof(struct pzpd_disk_member)) ||
              !pzpd_check_section(c->buf, c->len, h->heap_offset,    PZPD_SECT_CHEAP,    h->heap_bytes) ||
              !pzpd_check_section(c->buf, c->len, h->remap_offset,   PZPD_SECT_CREMAP,   (uint64_t) h->member_count * PZPD_MAX_STREAMS) )
        { pzpd_set_error(PZPD_E_FORMAT, "%s: collection sections are damaged", path); ok = 0; }
    if (ok)
    {
        c->members = (const struct pzpd_disk_member *) (c->buf + h->members_offset);
        c->heap    = (const char *) (c->buf + h->heap_offset);
        c->remap   = (const uint8_t *) (c->buf + h->remap_offset);
        uint64_t expect = 0;
        for (unsigned i = 0; ok && (i < h->member_count); i++)
        {
            const struct pzpd_disk_member *dm = &c->members[i];
            if ( !pzpd_in_file(dm->path_offset, dm->path_len, h->heap_bytes) || !pzpd_in_file(dm->alias_offset, dm->alias_len, h->heap_bytes) ||
                 (dm->path_len == 0) || (dm->alias_len == 0) || (dm->first_ordinal != expect) )
                { pzpd_set_error(PZPD_E_FORMAT, "%s: member table is damaged", path); ok = 0; }
            expect += dm->record_count;
        }
        if (ok && (expect != h->total_records)) { pzpd_set_error(PZPD_E_FORMAT, "%s: member record counts don't add up", path); ok = 0; }
    }
    if (!ok) { free(c->buf); c->buf = NULL; }
    return ok;
}

/** @brief Copy a heap string into a new NUL-terminated buffer. */
static char *pzpd_heap_str(const char *heap, uint32_t off, uint32_t len)
{
    char *r = (char *) malloc((size_t) len + 1);
    if (r != NULL) { memcpy(r, heap + off, len); r[len] = 0; }
    return r;
}

/** @brief Resolve a stored member path: absolute as is, relative to the collection file's directory. */
static char *pzpd_resolve_member_path(const char *collPath, const char *stored)
{
    if (stored[0] == '/') { return strdup(stored); }
    const char *slash = strrchr(collPath, '/');
    size_t dl = (slash == NULL) ? 0 : (size_t)(slash - collPath + 1);
    size_t sl = strlen(stored);
    char *r = (char *) malloc(dl + sl + 1);
    if (r != NULL) { memcpy(r, collPath, dl); memcpy(r + dl, stored, sl + 1); }
    return r;
}

/** @brief Open a collection file: members keep their recorded ordinal ranges even when missing. */
static pzpd *pzpd_coll_open(const char *path, unsigned flags)
{
    struct pzpd_coll_file c;
    if (!pzpd_coll_parse(path, &c)) { return NULL; }
    pzpd *a = pzpd_alloc(c.h.member_count, flags);
    if (a == NULL) { free(c.buf); return NULL; }
    for (unsigned u = 0; u < c.h.stream_count; u++) { pzpd_get_slot_name(a->streams[u], c.h.streams[u].name); }
    a->S = c.h.stream_count;
    int ok = 1;
    for (unsigned i = 0; ok && (i < c.h.member_count); i++)
    {
        const struct pzpd_disk_member *dm = &c.members[i];
        struct pzpd_member *mb = &a->m[i];
        char *stored = pzpd_heap_str(c.heap, dm->path_offset, dm->path_len);
        mb->alias = pzpd_heap_str(c.heap, dm->alias_offset, dm->alias_len);
        mb->path  = (stored != NULL) ? pzpd_resolve_member_path(path, stored) : NULL;
        free(stored);
        if ( (mb->alias == NULL) || (mb->path == NULL) ) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; break; }
        mb->count = dm->record_count;
        for (unsigned u = 0; u < PZPD_MAX_STREAMS; u++) { mb->to_member[u] = -1; mb->to_union[u] = -1; }
        for (unsigned u = 0; u < PZPD_MAX_TABLES; u++)  { mb->to_mtable[u] = -1; }
        mb->arch = arch_open(mb->path, flags & ~PZPD_O_ALLOW_MISSING);
        if (mb->arch == NULL)
        {
            // Missing member: its ordinal range stays reserved, only its reads fail
            snprintf(mb->error, sizeof(mb->error), "%s", pzpd_errorText);
            pzpd_clear_error();
            continue;
        }
        // Stale: rebuilt (other uuid), other size, or streams that don't match the recorded mapping
        int stale = (memcmp(mb->arch->uuid, dm->archive_uuid, 16) != 0) || (mb->arch->total != dm->record_count);
        const uint8_t *rm = c.remap + (size_t) i * PZPD_MAX_STREAMS;
        unsigned mapped = 0;
        for (unsigned u = 0; !stale && (u < PZPD_MAX_STREAMS); u++)
        {
            if (rm[u] == 0xFF) { continue; }
            if ( (u >= a->S) || (rm[u] >= mb->arch->S) || (strcmp(mb->arch->streams[rm[u]], a->streams[u]) != 0) ) { stale = 1; break; }
            mb->to_member[u]     = rm[u];
            mb->to_union[rm[u]]  = (int) u;
            mapped++;
        }
        if (mapped != mb->arch->S) { stale = 1; }
        if (!stale && !pzpd_bind_tables(a, mb)) { ok = 0; break; }
        if (stale)
        {
            pzpd_set_error(PZPD_E_STALE_COLLECTION, "%s: member \"%s\" (%s) changed since the collection was written; run `pzpdir collect --refresh %s`", path, mb->alias, mb->path, path);
            ok = 0;
        }
    }
    free(c.buf);
    if (ok) { ok = pzpd_check_aliases(a); }
    if (!ok) { struct pzpd_saved_error e; pzpd_error_save(&e); pzpd_close(a); pzpd_error_restore(&e); return NULL; }
    pzpd_layout(a);
    // Missing members keep their recorded size in pzpd_layout() (count stays as read)
    return a;
}

pzpd *pzpd_open(const char *path, unsigned int flags)
{
    pzpd_clear_error();
    if (path == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL path"); return NULL; }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { pzpd_set_error(PZPD_E_IO, "cannot open %s: %s", path, strerror(errno)); return NULL; }
    char magic[8];
    int got = pzpd_pread_all(fd, magic, 8, 0);
    close(fd);
    if (got && (memcmp(magic, PZPD_MAGIC_COLL, 8) == 0)) { return pzpd_coll_open(path, flags); }
    const char *paths[1] = { path };
    return pzpd_open_many(paths, NULL, 1, flags & ~PZPD_O_ALLOW_MISSING);
}

//-----------------------------------------------------------------------------------------------
// Routing
//-----------------------------------------------------------------------------------------------

/** @brief Member holding an ordinal (the last member whose first ordinal is <= ordinal; members
 *  with 0 records share their first ordinal with the next one and are skipped naturally).
 *  @return Member index, or -1 if out of range (error set). */
static int pzpd_member_index(const pzpd *a, uint64_t ordinal)
{
    if (ordinal >= a->total) { pzpd_set_error(PZPD_E_ARG, "ordinal %llu out of range (%llu records)", (unsigned long long) ordinal, (unsigned long long) a->total); return -1; }
    unsigned lo = 0, hi = a->member_count;
    while (hi - lo > 1)
    {
        unsigned mid = lo + (hi - lo) / 2;
        if (a->m[mid].first <= ordinal) { lo = mid; } else { hi = mid; }
    }
    return (int) lo;
}

/** @brief Route an ordinal to its member archive and local ordinal.
 *  @return The archive, or NULL (error set: out of range, or PZPD_E_MEMBER_MISSING). */
static struct pzpd_archive *pzpd_route(pzpd *a, uint64_t ordinal, unsigned *member, uint64_t *local)
{
    if (a == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL handle"); return NULL; }
    int mi = pzpd_member_index(a, ordinal);
    if (mi < 0) { return NULL; }
    struct pzpd_member *mb = &a->m[mi];
    if (mb->arch == NULL) { pzpd_set_error(PZPD_E_MEMBER_MISSING, "member \"%s\" (%s) is unavailable: %s", mb->alias, mb->path, mb->error); return NULL; }
    *member = (unsigned) mi;
    *local  = ordinal - mb->first;
    return mb->arch;
}

/** @brief Map a merged stream mask to a member's own stream mask. */
static uint32_t pzpd_member_mask(const struct pzpd_member *mb, uint32_t mask)
{
    uint32_t r = 0;
    for (unsigned u = 0; u < PZPD_MAX_STREAMS; u++) { if ( (mask & (1u << u)) && (mb->to_member[u] >= 0) ) { r |= 1u << mb->to_member[u]; } }
    return r;
}

uint64_t pzpd_count(const pzpd *a)          { return (a == NULL) ? 0 : a->total; }
unsigned pzpd_stream_count(const pzpd *a)   { return (a == NULL) ? 0 : a->S; }
unsigned pzpd_shard_count(const pzpd *a)    { return (a == NULL) ? 0 : a->shard_total; }
unsigned pzpd_member_count(const pzpd *a)   { return (a == NULL) ? 0 : a->member_count; }

int pzpd_stream_id(const pzpd *a, const char *name)
{
    if ( (a == NULL) || (name == NULL) ) { return -1; }
    for (unsigned s = 0; s < a->S; s++) { if (strcmp(a->streams[s], name) == 0) { return (int) s; } }
    return -1;
}

const char *pzpd_stream_name(const pzpd *a, unsigned stream)
{
    if ( (a == NULL) || (stream >= a->S) ) { return NULL; }
    return a->streams[stream];
}

const char *pzpd_member_alias(const pzpd *a, unsigned member)
{
    if ( (a == NULL) || (member >= a->member_count) ) { return NULL; }
    return a->m[member].alias;
}

int pzpd_member_id(const pzpd *a, const char *alias)
{
    if ( (a == NULL) || (alias == NULL) ) { return -1; }
    for (unsigned i = 0; i < a->member_count; i++) { if (strcmp(a->m[i].alias, alias) == 0) { return (int) i; } }
    return -1;
}

int pzpd_member_of(const pzpd *a, uint64_t ordinal, uint64_t *local_ordinal)
{
    if ( (a == NULL) || (ordinal >= a->total) ) { return -1; }
    int mi = pzpd_member_index(a, ordinal);
    if ( (mi >= 0) && (local_ordinal != NULL) ) { *local_ordinal = ordinal - a->m[mi].first; }
    return mi;
}

int pzpd_member_range(const pzpd *a, unsigned member, uint64_t *first, uint64_t *count)
{
    if ( (a == NULL) || (member >= a->member_count) ) { return 0; }
    if (first != NULL) { *first = a->m[member].first; }
    if (count != NULL) { *count = a->m[member].count; }
    return 1;
}

int pzpd_shard_info_get(pzpd *a, unsigned shard, pzpd_shard_info *out)
{
    pzpd_clear_error();
    if ( (a == NULL) || (out == NULL) || (shard >= a->shard_total) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    for (unsigned i = 0; i < a->member_count; i++)
    {
        struct pzpd_member *mb = &a->m[i];
        if ( (mb->arch == NULL) || (shard < mb->shard_base) || (shard >= mb->shard_base + mb->shards) ) { continue; }
        if (!arch_shard_info_get(mb->arch, shard - mb->shard_base, out)) { return 0; }
        out->first_ordinal += mb->first;
        out->member = i;
        return 1;
    }
    pzpd_set_error(PZPD_E_ARG, "shard %u not found", shard);
    return 0;
}

int64_t pzpd_find(pzpd *a, const char *key, size_t len, int *stream_out)
{
    pzpd_clear_error();
    if ( (a == NULL) || (key == NULL) || (len == 0) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return -1; }
    // Record keys in every member first, then blob names
    for (int kind = PZPD_KIND_KEY; kind <= PZPD_KIND_NAME; kind++)
    {
        for (unsigned i = 0; i < a->member_count; i++)
        {
            struct pzpd_member *mb = &a->m[i];
            if (mb->arch == NULL) { continue; }
            int ms = -1;
            int64_t r = arch_find(mb->arch, key, len, &ms, kind);
            if (r >= 0)
            {
                if (stream_out != NULL) { *stream_out = (ms < 0) ? -1 : mb->to_union[ms]; }
                pzpd_clear_error();
                return (int64_t)(mb->first + (uint64_t) r);
            }
        }
    }
    pzpd_set_error(PZPD_E_NOTFOUND, "\"%.*s\" not found", (int)(len > 200 ? 200 : len), key);
    return -1;
}

int64_t pzpd_find_in(pzpd *a, unsigned member, const char *key, size_t len, int *stream_out)
{
    pzpd_clear_error();
    if ( (a == NULL) || (member >= a->member_count) || (key == NULL) || (len == 0) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return -1; }
    struct pzpd_member *mb = &a->m[member];
    if (mb->arch == NULL) { pzpd_set_error(PZPD_E_MEMBER_MISSING, "member \"%s\" is unavailable: %s", mb->alias, mb->error); return -1; }
    int ms = -1;
    int64_t r = arch_find(mb->arch, key, len, &ms, -1);
    if (r < 0) { return -1; }
    if (stream_out != NULL) { *stream_out = (ms < 0) ? -1 : mb->to_union[ms]; }
    return (int64_t)(mb->first + (uint64_t) r);
}

size_t pzpd_find_all(pzpd *a, const char *key, size_t len, int64_t *ordinals_out, int *streams_out, size_t max)
{
    pzpd_clear_error();
    if ( (a == NULL) || (key == NULL) || (len == 0) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    size_t total = 0;
    for (int kind = PZPD_KIND_KEY; kind <= PZPD_KIND_NAME; kind++)
    {
        for (unsigned i = 0; i < a->member_count; i++)
        {
            struct pzpd_member *mb = &a->m[i];
            if (mb->arch == NULL) { continue; }
            int ms = -1;
            int64_t r = arch_find(mb->arch, key, len, &ms, kind);   // keys and names are unique within an archive
            if (r < 0) { continue; }
            if (total < max)
            {
                if (ordinals_out != NULL) { ordinals_out[total] = (int64_t)(mb->first + (uint64_t) r); }
                if (streams_out  != NULL) { streams_out[total]  = (ms < 0) ? -1 : mb->to_union[ms]; }
            }
            total++;
        }
    }
    pzpd_clear_error();
    if (total == 0) { pzpd_set_error(PZPD_E_NOTFOUND, "\"%.*s\" not found", (int)(len > 200 ? 200 : len), key); }
    return total;
}

const char *pzpd_record_key(pzpd *a, uint64_t ordinal, size_t *len)
{
    unsigned mi; uint64_t local;
    pzpd_clear_error();
    struct pzpd_archive *ar = pzpd_route(a, ordinal, &mi, &local);
    return (ar == NULL) ? NULL : arch_record_key(ar, local, len);
}

int pzpd_blob_info_get(pzpd *a, uint64_t ordinal, unsigned stream, pzpd_blob_info *out)
{
    unsigned mi; uint64_t local;
    pzpd_clear_error();
    if ( (a == NULL) || (out == NULL) || (stream >= a->S) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    struct pzpd_archive *ar = pzpd_route(a, ordinal, &mi, &local);
    if (ar == NULL) { return 0; }
    int ms = a->m[mi].to_member[stream];
    if (ms < 0)
    {
        // The member has no such stream: a missing blob, with the record's own fields
        uint64_t sl;
        struct pzpd_rshard *s = pzpd_locate(ar, local, &sl);
        if (s == NULL) { return 0; }
        memset(out, 0, sizeof(*out));
        out->group = s->rtab[sl].group;
        out->frame = s->rtab[sl].frame;
        out->shard = (unsigned)(s - ar->shards);
    }
    else if (!arch_blob_info_get(ar, local, (unsigned) ms, out)) { return 0; }
    out->member = mi;
    out->shard += a->m[mi].shard_base;
    return 1;
}

ssize_t pzpd_read_into(pzpd *a, uint64_t ordinal, unsigned stream, void *buf, size_t cap)
{
    unsigned mi; uint64_t local;
    pzpd_clear_error();
    if ( (a != NULL) && (stream >= a->S) ) { pzpd_set_error(PZPD_E_ARG, "stream %u out of range (%u streams)", stream, a->S); return PZPD_E_ARG; }
    struct pzpd_archive *ar = pzpd_route(a, ordinal, &mi, &local);
    if (ar == NULL) { return (ssize_t) pzpd_errorCode; }
    int ms = a->m[mi].to_member[stream];
    return (ms < 0) ? 0 : arch_read_into(ar, local, (unsigned) ms, buf, cap);
}

void *pzpd_read_alloc(pzpd *a, uint64_t ordinal, unsigned stream, size_t *size)
{
    unsigned mi; uint64_t local;
    pzpd_clear_error();
    if (size != NULL) { *size = 0; }
    if ( (a != NULL) && (stream >= a->S) ) { pzpd_set_error(PZPD_E_ARG, "stream %u out of range (%u streams)", stream, a->S); return NULL; }
    struct pzpd_archive *ar = pzpd_route(a, ordinal, &mi, &local);
    if (ar == NULL) { return NULL; }
    int ms = a->m[mi].to_member[stream];
    return (ms < 0) ? NULL : arch_read_alloc(ar, local, (unsigned) ms, size);
}

const void *pzpd_view(pzpd *a, uint64_t ordinal, unsigned stream, size_t *size)
{
    unsigned mi; uint64_t local;
    pzpd_clear_error();
    if (size != NULL) { *size = 0; }
    if ( (a != NULL) && (stream >= a->S) ) { pzpd_set_error(PZPD_E_ARG, "stream %u out of range (%u streams)", stream, a->S); return NULL; }
    struct pzpd_archive *ar = pzpd_route(a, ordinal, &mi, &local);
    if (ar == NULL) { return NULL; }
    int ms = a->m[mi].to_member[stream];
    return (ms < 0) ? NULL : arch_view(ar, local, (unsigned) ms, size);
}

size_t pzpd_record_span(pzpd *a, uint64_t ordinal, uint32_t stream_mask)
{
    unsigned mi; uint64_t local;
    pzpd_clear_error();
    struct pzpd_archive *ar = pzpd_route(a, ordinal, &mi, &local);
    if (ar == NULL) { return 0; }
    return arch_record_span(ar, local, pzpd_member_mask(&a->m[mi], stream_mask));
}

ssize_t pzpd_read_record(pzpd *a, uint64_t ordinal, uint32_t stream_mask, void *buf, size_t cap, pzpd_blob_ref *refs)
{
    unsigned mi; uint64_t local;
    pzpd_clear_error();
    struct pzpd_archive *ar = pzpd_route(a, ordinal, &mi, &local);
    if (ar == NULL) { return (ssize_t) pzpd_errorCode; }
    struct pzpd_member *mb = &a->m[mi];
    pzpd_blob_ref mine[PZPD_MAX_STREAMS];
    ssize_t r = arch_read_record(ar, local, pzpd_member_mask(mb, stream_mask), buf, cap, mine);
    if (refs != NULL)
    {
        memset(refs, 0, sizeof(pzpd_blob_ref) * a->S);
        if (r > 0)
        {
            for (unsigned u = 0; u < a->S; u++)
            {
                if ( (stream_mask & (1u << u)) && (mb->to_member[u] >= 0) ) { refs[u] = mine[mb->to_member[u]]; }
            }
        }
    }
    return r;
}

int pzpd_verify_record(pzpd *a, uint64_t ordinal, int check_blobs)
{
    unsigned mi; uint64_t local;
    pzpd_clear_error();
    struct pzpd_archive *ar = pzpd_route(a, ordinal, &mi, &local);
    return (ar == NULL) ? 0 : arch_verify_record(ar, local, check_blobs);
}

int pzpd_verify_shard(pzpd *a, unsigned shard)
{
    pzpd_clear_error();
    if ( (a == NULL) || (shard >= a->shard_total) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    for (unsigned i = 0; i < a->member_count; i++)
    {
        struct pzpd_member *mb = &a->m[i];
        if ( (mb->arch != NULL) && (shard >= mb->shard_base) && (shard < mb->shard_base + mb->shards) ) { return arch_verify_shard(mb->arch, shard - mb->shard_base); }
    }
    pzpd_set_error(PZPD_E_ARG, "shard %u not found", shard);
    return 0;
}

#if PZPDIR_WITH_PZP
unsigned char *pzpd_read_pzp(pzpd *a, uint64_t ordinal, unsigned stream,
                             unsigned int *width, unsigned int *height,
                             unsigned int *bpp, unsigned int *channels)
{
    unsigned mi; uint64_t local;
    pzpd_clear_error();
    if ( (a != NULL) && (stream >= a->S) ) { pzpd_set_error(PZPD_E_ARG, "stream %u out of range", stream); return NULL; }
    struct pzpd_archive *ar = pzpd_route(a, ordinal, &mi, &local);
    if (ar == NULL) { return NULL; }
    int ms = a->m[mi].to_member[stream];
    if (ms < 0) { pzpd_set_error(PZPD_E_NOTFOUND, "member \"%s\" has no stream \"%s\"", a->m[mi].alias, a->streams[stream]); return NULL; }
    return arch_read_pzp(ar, local, (unsigned) ms, width, height, bpp, channels);
}
#endif

//-----------------------------------------------------------------------------------------------
// Tables through the handle
//-----------------------------------------------------------------------------------------------

unsigned pzpd_table_count(const pzpd *a) { return (a == NULL) ? 0 : a->T; }

int pzpd_table_id(const pzpd *a, const char *name)
{
    if ( (a == NULL) || (name == NULL) ) { return -1; }
    for (unsigned t = 0; t < a->T; t++) { if (!strcmp(a->tables[t]->name, name)) { return (int) t; } }
    return -1;
}

const pzpd_schema *pzpd_table_schema(const pzpd *a, unsigned table)
{
    if ( (a == NULL) || (table >= a->T) ) { return NULL; }
    return &a->tables[table]->pub;
}

/** @brief Route (ordinal, merged table) to the member archive and its table id.
 *  @return The archive, or NULL (error set); *mt = -1 when the member lacks the table. */
static struct pzpd_archive *pzpd_route_table(pzpd *a, uint64_t ordinal, unsigned table, unsigned *mi, uint64_t *local, int *mt)
{
    if ( (a == NULL) || (table >= a->T) ) { pzpd_set_error(PZPD_E_ARG, "table %u out of range", table); return NULL; }
    struct pzpd_archive *ar = pzpd_route(a, ordinal, mi, local);
    if (ar != NULL) { *mt = a->m[*mi].to_mtable[table]; }
    return ar;
}

uint32_t pzpd_table_rows(pzpd *a, uint64_t ordinal, unsigned table, const void **rows_out)
{
    unsigned mi; uint64_t local; int mt;
    pzpd_clear_error();
    if (rows_out != NULL) { *rows_out = NULL; }
    struct pzpd_archive *ar = pzpd_route_table(a, ordinal, table, &mi, &local, &mt);
    if (ar == NULL) { return 0; }
    if (mt < 0) { return 0; }                     // the member has no such table: no rows
    return arch_table_rows(ar, local, (unsigned) mt, rows_out);
}

const char *pzpd_table_str(pzpd *a, uint64_t ordinal, unsigned table, const void *field, size_t *len)
{
    unsigned mi; uint64_t local; int mt;
    pzpd_clear_error();
    struct pzpd_archive *ar = pzpd_route_table(a, ordinal, table, &mi, &local, &mt);
    if (ar == NULL) { return NULL; }
    if (mt < 0) { pzpd_set_error(PZPD_E_NOTFOUND, "member \"%s\" has no table %s", a->m[mi].alias, a->tables[table]->name); return NULL; }
    uint64_t sl;
    struct pzpd_rshard *s = pzpd_locate(ar, local, &sl);
    return (s == NULL) ? NULL : pzpd_view_str(&s->tv[mt], field, len);
}

/** @brief Member archive and table id for a global-table call.
 *  @return The archive, or NULL (error set). */
static struct pzpd_archive *pzpd_global_target(pzpd *a, unsigned member, unsigned table, int *mt)
{
    if ( (a == NULL) || (member >= a->member_count) || (table >= a->T) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return NULL; }
    if ( !(a->tables[table]->flags & PZPD_TABLE_GLOBAL) ) { pzpd_set_error(PZPD_E_ARG, "table %s is a record table", a->tables[table]->name); return NULL; }
    struct pzpd_member *mb = &a->m[member];
    if (mb->arch == NULL) { pzpd_set_error(PZPD_E_MEMBER_MISSING, "member \"%s\" is unavailable: %s", mb->alias, mb->error); return NULL; }
    *mt = mb->to_mtable[table];
    if (*mt >= 0) { pzpd_shard(mb->arch, 0); pzpd_clear_error(); }   // a standalone shard learns its global rows when loaded
    return mb->arch;
}

uint32_t pzpd_global_rows(pzpd *a, unsigned member, unsigned table, const void **rows_out)
{
    int mt;
    pzpd_clear_error();
    if (rows_out != NULL) { *rows_out = NULL; }
    struct pzpd_archive *ar = pzpd_global_target(a, member, table, &mt);
    if ( (ar == NULL) || (mt < 0) ) { return 0; }
    const struct pzpd_tview *v = &ar->gview[mt];
    if ( (rows_out != NULL) && (v->rows > 0) ) { *rows_out = v->rowdata; }
    return (uint32_t) v->rows;
}

const char *pzpd_global_str(pzpd *a, unsigned member, unsigned table, const void *field, size_t *len)
{
    int mt;
    pzpd_clear_error();
    struct pzpd_archive *ar = pzpd_global_target(a, member, table, &mt);
    if (ar == NULL) { return NULL; }
    if (mt < 0) { pzpd_set_error(PZPD_E_NOTFOUND, "member has no table %s", a->tables[table]->name); return NULL; }
    return pzpd_view_str(&ar->gview[mt], field, len);
}

int pzpd_table_shard_view(pzpd *a, unsigned shard, unsigned table, pzpd_table_view *out)
{
    pzpd_clear_error();
    if ( (a == NULL) || (out == NULL) || (shard >= a->shard_total) || (table >= a->T) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    if (a->tables[table]->flags & PZPD_TABLE_GLOBAL) { pzpd_set_error(PZPD_E_ARG, "table %s is global", a->tables[table]->name); return 0; }
    memset(out, 0, sizeof(*out));
    for (unsigned i = 0; i < a->member_count; i++)
    {
        struct pzpd_member *mb = &a->m[i];
        if ( (mb->arch == NULL) || (shard < mb->shard_base) || (shard >= mb->shard_base + mb->shards) ) { continue; }
        struct pzpd_rshard *s = pzpd_shard(mb->arch, shard - mb->shard_base);
        if (s == NULL) { return 0; }
        out->first_ordinal = mb->first + s->first_ordinal;
        out->records       = s->sb.record_count;
        int mt = mb->to_mtable[table];
        if (mt < 0) { return 1; }                      // the member lacks the table: no rows
        struct pzpd_tview *tv = &s->tv[mt];
        if (tv->index == NULL) { pzpd_set_error(PZPD_E_ARG, "table %s has no row index (a global table?)", a->tables[table]->name); return 0; }
        if (!__atomic_load_n(&tv->checked, __ATOMIC_ACQUIRE))
        {
            // The caller will index rows with these entries directly: verify the whole index once
            for (uint64_t r = 0; r < tv->records; r++)
            {
                if (tv->index[r + 1] < tv->index[r]) { pzpd_set_error(PZPD_E_FORMAT, "%s: table %s: row index is damaged", s->path, a->tables[table]->name); return 0; }
            }
            __atomic_store_n(&tv->checked, 1, __ATOMIC_RELEASE);
        }
        out->row_index   = s->tv[mt].index;
        out->rows        = s->tv[mt].rowdata;
        out->total_rows  = s->tv[mt].rows;
        out->strings     = s->tv[mt].heap;
        out->strings_len = s->tv[mt].heap_bytes;
        return 1;
    }
    pzpd_set_error(PZPD_E_ARG, "shard %u not found", shard);
    return 0;
}

ssize_t pzpd_table_csv(pzpd *a, uint64_t ordinal, unsigned table, char *out, size_t cap)
{
    unsigned mi; uint64_t local; int mt;
    pzpd_clear_error();
    if ( (out != NULL) && (cap > 0) ) { out[0] = 0; }
    struct pzpd_archive *ar = pzpd_route_table(a, ordinal, table, &mi, &local, &mt);
    if (ar == NULL) { return (ssize_t) pzpd_errorCode; }
    if (mt < 0) { return 0; }
    const void *rows = NULL;
    uint32_t n = arch_table_rows(ar, local, (unsigned) mt, &rows);
    if (pzpd_errorCode != PZPD_OK) { return (ssize_t) pzpd_errorCode; }
    uint64_t sl;
    struct pzpd_rshard *s = pzpd_locate(ar, local, &sl);
    if (s == NULL) { return (ssize_t) pzpd_errorCode; }
    return pzpd_rows_csv(&ar->tables[mt], (const unsigned char *) rows, n, &s->tv[mt], out, cap);
}

ssize_t pzpd_global_csv(pzpd *a, unsigned member, unsigned table, char *out, size_t cap)
{
    int mt;
    pzpd_clear_error();
    if ( (out != NULL) && (cap > 0) ) { out[0] = 0; }
    struct pzpd_archive *ar = pzpd_global_target(a, member, table, &mt);
    if (ar == NULL) { return (ssize_t) pzpd_errorCode; }
    if (mt < 0) { return 0; }
    const struct pzpd_tview *v = &ar->gview[mt];
    return pzpd_rows_csv(&ar->tables[mt], v->rowdata, (uint32_t) v->rows, v, out, cap);
}

//-----------------------------------------------------------------------------------------------
// Word index handle (spec §3.7, §4.10)
//-----------------------------------------------------------------------------------------------

/** @brief Word index state of one shard inside a handle, loaded on first use (under pzpd_words::lock). */
struct pzpd_wstate
{
    int       state;        ///< 0 not loaded, 1 loaded, -1 failed (read with acquire / written with release)
    int       has;          ///< 1 if the shard has the handle's sub-index
    struct pzpd_wsub sub;   ///< That sub-index
    uint32_t *map;          ///< Shard word id -> handle word id
    uint64_t  first;        ///< Handle ordinal of the shard's first record
    char      error[512];   ///< Why loading failed
    int       error_code;   ///< Its enum pzpd_error
};

/** @brief One view of one sub-index of a word index over a pzpd handle (public: pzpd_words). */
struct pzpd_words
{
    pzpd     *a;                ///< The handle
    unsigned  flags;            ///< PZPD_WORDS_CANONICAL
    char      table[24];        ///< Indexed table
    char      column[24];       ///< Indexed column
    char     *source;           ///< Source value, NULL for the merged sub-index
    size_t    source_len;       ///< Its length
    uint64_t  covered;          ///< Records of the members that have this word index
    uint32_t  n;                ///< Vocabulary size
    char     *heap;             ///< Words, concatenated in id order
    uint64_t *offsets;          ///< n + 1 offsets into heap
    uint64_t *records;          ///< Records per word
    uint64_t *count;            ///< Occurrences per word
    uint32_t  ns;               ///< Surface words (the words the shards store)
    char     *sheap;            ///< Surface words, sorted
    uint64_t *soff;             ///< ns + 1 offsets into sheap
    uint32_t *sstart;           ///< n + 1: the surface words of word h are sidx[sstart[h] .. sstart[h+1])
    uint32_t *sidx;             ///< Surface word ids
    struct pzpd_wstate *st;     ///< One per shard of the handle (pzpd::shard_total)
    pthread_mutex_t lock;       ///< Taken once per shard, for its lazy load
};

/** @brief Surface word id of a word (binary search), or -1. */
static int64_t pzpd_words_sfind(const pzpd_words *w, const char *word, size_t len)
{
    uint64_t lo = 0, hi = w->ns;
    while (lo < hi)
    {
        uint64_t mid = lo + (hi - lo) / 2;
        int c = pzpd_word_cmp(w->sheap + w->soff[mid], (size_t)(w->soff[mid + 1] - w->soff[mid]), word, len);
        if (c == 0) { return (int64_t) mid; }
        if (c < 0) { lo = mid + 1; } else { hi = mid; }
    }
    return -1;
}

int64_t pzpd_words_find(const pzpd_words *w, const char *word, size_t len)
{
    if ( (w == NULL) || ((word == NULL) && (len > 0)) ) { return -1; }
    uint64_t lo = 0, hi = w->n;
    while (lo < hi)
    {
        uint64_t mid = lo + (hi - lo) / 2;
        int c = pzpd_word_cmp(w->heap + w->offsets[mid], (size_t)(w->offsets[mid + 1] - w->offsets[mid]), word, len);
        if (c == 0) { return (int64_t) mid; }
        if (c < 0) { lo = mid + 1; } else { hi = mid; }
    }
    return -1;
}

/** @brief The vocabulary a member contributes: its manifest's merged sub-index, or a standalone shard's own.
 *  @param has_index Receives 1 if the member has the word index at all (whatever the source).
 *  @return 1 with *sub filled, 0 if the member has no such (sub-)index, -1 on error (error set). */
static int pzpd_words_member_vocab(pzpd *a, unsigned mi, const char *table, const char *column, const char *source, size_t source_len,
                                   struct pzpd_wsec *v, struct pzpd_wsub *sub, int *has_index)
{
    *has_index = 0;
    struct pzpd_member *mb = &a->m[mi];
    if (mb->arch == NULL) { return 0; }                    // a missing member has no words
    struct pzpd_archive *ar = mb->arch;
    int64_t expect = -1;
    if (!ar->standalone)
    {
        if (ar->mwords_bad) { pzpd_set_error(PZPD_E_CHECKSUM, "member \"%s\": the manifest's word index directory is damaged (rebuild-manifest)", mb->alias); return -1; }
        int j = pzpd_words_slot(ar->mwords, table, column);
        if (j < 0) { return 0; }
        const struct pzpd_disk_words *d = &ar->mwords[j];
        if ( !pzpd_check_section(ar->mmap_manifest, ar->manifest_len, d->section_offset, PZPD_SECT_MWORDS, d->section_bytes) ||
             !pzpd_wsec_parse(ar->mmap_manifest + d->section_offset, d->section_bytes, 1, v) )
        {
            if (pzpd_errorText[0] == 0) { pzpd_set_error(PZPD_E_FORMAT, "section is damaged"); }
            pzpd_error_wrap(PZPD_E_FORMAT, "member \"%s\": manifest word index %s.%s", mb->alias, table, column);
            return -1;
        }
    }
    else
    {
        struct pzpd_rshard *s = pzpd_shard(ar, 0);
        if (s == NULL) { return -1; }
        int j = pzpd_words_slot(s->sb.words, table, column);
        if (j < 0) { return 0; }
        if (!pzpd_shard_wsec(s, (unsigned) j, v)) { return -1; }
        expect = (int64_t) s->sb.record_count;
    }
    *has_index = 1;
    int k = pzpd_wsec_find(v, source, source_len, expect);
    if (k == -2) { return -1; }
    if (k < 0) { return 0; }
    return pzpd_wsec_sub(v, (unsigned) k, expect, sub) ? 1 : -1;
}

/** @brief Load the word index state of handle shard g (once). @return The state, or NULL on error (error set). */
static struct pzpd_wstate *pzpd_words_state(pzpd_words *w, unsigned g)
{
    struct pzpd_wstate *st = &w->st[g];
    int state = __atomic_load_n(&st->state, __ATOMIC_ACQUIRE);
    if (state == 0)
    {
        pthread_mutex_lock(&w->lock);
        if (st->state == 0)
        {
            int ok = 1;
            pzpd *a = w->a;
            unsigned mi = 0;
            while ( (mi + 1 < a->member_count) && (g >= a->m[mi + 1].shard_base) ) { mi++; }
            struct pzpd_member *mb = &a->m[mi];
            struct pzpd_rshard *s = (mb->arch != NULL) ? pzpd_shard(mb->arch, g - mb->shard_base) : NULL;
            if (s == NULL) { ok = 0; if (pzpd_errorCode == PZPD_OK) { pzpd_set_error(PZPD_E_MEMBER_MISSING, "member \"%s\" is unavailable", mb->alias); } }
            int j = ok ? pzpd_words_slot(s->sb.words, w->table, w->column) : -1;
            if (ok && (j >= 0))
            {
                struct pzpd_wsec v;
                int k = -1;
                ok = pzpd_shard_wsec(s, (unsigned) j, &v);
                if (ok) { k = pzpd_wsec_find(&v, w->source, w->source_len, (int64_t) s->sb.record_count); ok = (k != -2); }
                if (ok && (k >= 0)) { ok = pzpd_wsec_sub(&v, (unsigned) k, (int64_t) s->sb.record_count, &st->sub); st->has = ok; }
            }
            if (ok) { st->first = mb->first + s->first_ordinal; }
            // Shard word id -> handle word id (surface word, then its canonical form)
            if (ok && st->has)
            {
                st->map = (uint32_t *) malloc(sizeof(uint32_t) * (st->sub.words ? st->sub.words : 1));
                if (st->map == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
                for (uint64_t k = 0; ok && (k < st->sub.words); k++)
                {
                    const char *word; size_t len;
                    ok = pzpd_wsub_word(&st->sub, k, &word, &len);
                    int64_t sid = ok ? pzpd_words_sfind(w, word, len) : -1;
                    uint32_t h = 0xFFFFFFFFu;
                    // Surface view: handle id = surface id; canonical view: sidx[ns + sid] (see pzpd_words_open())
                    if (sid >= 0) { h = (w->flags & PZPD_WORDS_CANONICAL) ? w->sidx[w->ns + sid] : (uint32_t) sid; }
                    if ( ok && (h == 0xFFFFFFFFu) ) { pzpd_set_error(PZPD_E_STALE_MANIFEST, "%s: word \"%.*s\" is missing from the manifest's vocabulary (rebuild-manifest)", s->path, (int)(len > 100 ? 100 : len), word); ok = 0; }
                    if (ok) { st->map[k] = h; }
                }
            }
            if (!ok)
            {
                snprintf(st->error, sizeof(st->error), "%s", pzpd_errorText);
                st->error_code = pzpd_errorCode;
                free(st->map);
                st->map = NULL;
                st->has = 0;
            }
            __atomic_store_n(&st->state, ok ? 1 : -1, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&w->lock);
        state = __atomic_load_n(&st->state, __ATOMIC_ACQUIRE);
    }
    if (state < 0) { pzpd_set_error(st->error_code ? st->error_code : PZPD_E_FORMAT, "%s", st->error); return NULL; }
    return st;
}

/** @brief Append the handle ordinals of the records containing a surface word (every shard, in order).
 *  @return 1 on success, 0 on failure (error set). */
static int pzpd_words_surface_records(pzpd_words *w, const char *word, size_t len, struct pzpd_buf *out)
{
    for (unsigned g = 0; g < w->a->shard_total; g++)
    {
        struct pzpd_member *mb = NULL;
        for (unsigned mi = 0; mi < w->a->member_count; mi++) { if ( (g >= w->a->m[mi].shard_base) && (g < w->a->m[mi].shard_base + w->a->m[mi].shards) ) { mb = &w->a->m[mi]; } }
        if (mb == NULL) { continue; }
        struct pzpd_wstate *st = pzpd_words_state(w, g);
        if (st == NULL) { return 0; }
        if (!st->has) { continue; }
        int64_t k = pzpd_wsub_find(&st->sub, word, len);
        if (k == -2) { return 0; }
        if (k < 0) { continue; }
        uint32_t a = st->sub.post_index[k], b = st->sub.post_index[k + 1];
        if ( (a > b) || (b > st->sub.postings) ) { pzpd_set_error(PZPD_E_FORMAT, "word index: postings are damaged"); return 0; }
        for (uint32_t p = a; p < b; p++)
        {
            if (st->sub.post[p] >= st->sub.records) { pzpd_set_error(PZPD_E_FORMAT, "word index: a posting points past the shard"); return 0; }
            uint64_t o = st->first + st->sub.post[p];
            if (!pzpd_buf_append(out, &o, sizeof(o))) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
        }
    }
    return 1;
}

static int pzpd_cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *) a, y = *(const uint64_t *) b;
    return (x < y) ? -1 : (x > y);
}

/** @brief Sort and deduplicate u64 values. @return The number left. */
static size_t pzpd_u64_unique(uint64_t *v, size_t n)
{
    if (n < 2) { return n; }
    qsort(v, n, sizeof(uint64_t), pzpd_cmp_u64);
    size_t o = 0;
    for (size_t i = 1; i < n; i++) { if (v[i] != v[o]) { v[++o] = v[i]; } }
    return o + 1;
}

/** @brief Records of handle word h (ascending, unique) into out (u64). @return 1, or 0 on error (error set). */
static int pzpd_words_records_buf(pzpd_words *w, uint32_t h, struct pzpd_buf *out)
{
    out->len = 0;
    for (uint32_t i = w->sstart[h]; i < w->sstart[h + 1]; i++)
    {
        uint32_t s = w->sidx[i];
        if (!pzpd_words_surface_records(w, w->sheap + w->soff[s], (size_t)(w->soff[s + 1] - w->soff[s]), out)) { return 0; }
    }
    if (w->sstart[h + 1] - w->sstart[h] > 1) { out->len = pzpd_u64_unique((uint64_t *) out->data, out->len / 8) * 8; }
    return 1;
}

void pzpd_words_close(pzpd_words *w)
{
    if (w == NULL) { return; }
    for (unsigned g = 0; (w->st != NULL) && (g < w->a->shard_total); g++) { free(w->st[g].map); }
    free(w->st);
    free(w->source);
    free(w->heap); free(w->offsets); free(w->records); free(w->count);
    free(w->sheap); free(w->soff); free(w->sstart); free(w->sidx);
    pthread_mutex_destroy(&w->lock);
    free(w);
}

/** @brief The union of the members' `synonyms` rules: surface word -> canonical word.
 *  @return 1 on success (from / to filled; to[id] = canonical bytes in `toheap`), 0 on failure (error set). */
static int pzpd_words_synonyms(pzpd *a, struct pzpd_sdict *from, struct pzpd_buf *toheap, struct pzpd_buf *tooff)
{
    int t = pzpd_table_id(a, PZPD_SYNONYMS_TABLE);
    if (t < 0) { return 1; }
    const struct pzpd_tschema *sc = a->tables[t];
    if (!pzpd_synonyms_check(sc, NULL, 0, NULL, 0)) { return 0; }
    for (unsigned mi = 0; mi < a->member_count; mi++)
    {
        const void *rows = NULL;
        uint32_t n = pzpd_global_rows(a, mi, (unsigned) t, &rows);
        if ( (n == 0) && (pzpd_errorCode != PZPD_OK) && (pzpd_errorCode != PZPD_E_MEMBER_MISSING) ) { return 0; }
        for (uint32_t r = 0; r < n; r++)
        {
            const unsigned char *row = (const unsigned char *) rows + (size_t) r * sc->stride;
            size_t wl = 0, cl = 0;
            const char *word = pzpd_global_str(a, mi, (unsigned) t, row + sc->offset[0], &wl);
            const char *canon = pzpd_global_str(a, mi, (unsigned) t, row + sc->offset[1], &cl);
            if ( (word == NULL) || (canon == NULL) ) { return 0; }
            uint32_t before = from->n;
            int64_t id = pzpd_sdict_id(from, word, wl);
            if (id < 0) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
            if ((uint32_t) id < before)
            {
                uint64_t o = ((const uint64_t *) tooff->data)[2 * id], l = ((const uint64_t *) tooff->data)[2 * id + 1];
                if ( (l != cl) || memcmp(toheap->data + o, canon, cl) )
                    { pzpd_set_error(PZPD_E_FORMAT, "synonyms: \"%.*s\" maps to \"%.*s\" in one member and to \"%.*s\" in member \"%s\"", (int) wl, word, (int) l, (const char *) toheap->data + o, (int) cl, canon, a->m[mi].alias); return 0; }
                continue;
            }
            uint64_t ol[2] = { toheap->len, cl };
            if ( !pzpd_buf_append(toheap, canon, cl) || !pzpd_buf_append(tooff, ol, sizeof(ol)) ) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
        }
    }
    // One step over the union too: no canonical word may itself be mapped
    for (uint32_t id = 0; id < from->n; id++)
    {
        uint64_t o = ((const uint64_t *) tooff->data)[2 * id], l = ((const uint64_t *) tooff->data)[2 * id + 1];
        if (pzpd_sdict_find(from, (const char *) toheap->data + o, l) >= 0)
            { pzpd_set_error(PZPD_E_FORMAT, "synonyms: \"%.*s\" is both a canonical word and mapped (over the members' rules)", (int) l, (const char *) toheap->data + o); return 0; }
    }
    return 1;
}

/** @brief A surface word with its canonical form, for grouping (pzpd_words_open()). */
struct pzpd_wcanon { const char *c; uint32_t clen; uint32_t sid; };

static int pzpd_cmp_wcanon(const void *a, const void *b)
{
    const struct pzpd_wcanon *x = (const struct pzpd_wcanon *) a, *y = (const struct pzpd_wcanon *) b;
    int c = pzpd_word_cmp(x->c, x->clen, y->c, y->clen);
    if (c != 0) { return c; }
    return (x->sid < y->sid) ? -1 : (x->sid > y->sid);
}

int pzpd_words_open(pzpd *a, const char *table, const char *column, const char *source, size_t source_len, unsigned flags, pzpd_words **out)
{
    pzpd_clear_error();
    if (out != NULL) { *out = NULL; }
    if ( (a == NULL) || (table == NULL) || (column == NULL) || (out == NULL) || (flags & ~PZPD_WORDS_CANONICAL) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    pzpd_words *w = (pzpd_words *) calloc(1, sizeof(pzpd_words));
    if (w == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    pthread_mutex_init(&w->lock, NULL);
    w->a = a;
    w->flags = flags;
    snprintf(w->table, sizeof(w->table), "%s", table);
    snprintf(w->column, sizeof(w->column), "%s", column);
    int ok = 1;
    if (source != NULL)
    {
        w->source = (char *) malloc(source_len ? source_len : 1);
        if (w->source == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
        else { memcpy(w->source, source, source_len); w->source_len = source_len; }
    }
    w->st = (struct pzpd_wstate *) calloc(a->shard_total ? a->shard_total : 1, sizeof(struct pzpd_wstate));
    if (w->st == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }

    // Surface vocabulary: the members' vocabularies, merged by word bytes (their records are disjoint: stats add up)
    struct pzpd_buf ents = {0};
    int any = 0;
    for (unsigned mi = 0; ok && (mi < a->member_count); mi++)
    {
        struct pzpd_wsec v;
        struct pzpd_wsub sub;
        int has = 0;
        int r = pzpd_words_member_vocab(a, mi, table, column, w->source, w->source_len, &v, &sub, &has);
        if (r < 0) { ok = 0; break; }
        if (has) { any = 1; w->covered += a->m[mi].count; }
        for (uint64_t k = 0; ok && (r == 1) && (k < sub.words); k++)
        {
            struct pzpd_went e;
            size_t l = 0;
            ok = pzpd_wsub_word(&sub, k, &e.w, &l);
            e.len = (uint32_t) l;
            e.records = sub.vocab ? sub.vocab[k].records : sub.mvocab[k].records;
            e.count   = sub.vocab ? sub.vocab[k].count   : sub.mvocab[k].count;
            if (ok && !pzpd_buf_append(&ents, &e, sizeof(e))) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
        }
    }
    if (ok && !any) { pzpd_set_error(PZPD_E_NOTFOUND, "no word index %s.%s", table, column); ok = 0; }
    size_t ns = ok ? pzpd_went_fold((struct pzpd_went *) ents.data, ents.len / sizeof(struct pzpd_went)) : 0;
    const struct pzpd_went *e = (const struct pzpd_went *) ents.data;
    if (ok && (ns > 0xFFFFFFFEull)) { pzpd_set_error(PZPD_E_ARG, "word index: more than 4 G words"); ok = 0; }
    if (ok)
    {
        w->ns = (uint32_t) ns;
        uint64_t hb = 0;
        for (size_t i = 0; i < ns; i++) { hb += e[i].len; }
        w->sheap = (char *) malloc(hb ? hb : 1);
        w->soff  = (uint64_t *) malloc(sizeof(uint64_t) * (ns + 1));
        if ( (w->sheap == NULL) || (w->soff == NULL) ) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
        uint64_t o = 0;
        for (size_t i = 0; ok && (i < ns); i++) { w->soff[i] = o; memcpy(w->sheap + o, e[i].w, e[i].len); o += e[i].len; }
        if (ok) { w->soff[ns] = o; }
    }

    // Handle vocabulary: the surface words, or their canonical forms grouped
    struct pzpd_sdict from;
    memset(&from, 0, sizeof(from));
    struct pzpd_buf toheap = {0}, tooff = {0}, canon = {0}, recs = {0};
    if (ok && (flags & PZPD_WORDS_CANONICAL)) { ok = pzpd_words_synonyms(a, &from, &toheap, &tooff); }
    if (ok)
    {
        for (uint32_t i = 0; ok && (i < w->ns); i++)
        {
            struct pzpd_wcanon c = { w->sheap + w->soff[i], (uint32_t)(w->soff[i + 1] - w->soff[i]), i };
            int64_t id = (flags & PZPD_WORDS_CANONICAL) ? pzpd_sdict_find(&from, c.c, c.clen) : -1;
            if (id >= 0) { c.c = (const char *) toheap.data + ((const uint64_t *) tooff.data)[2 * id]; c.clen = (uint32_t) ((const uint64_t *) tooff.data)[2 * id + 1]; }
            if (!pzpd_buf_append(&canon, &c, sizeof(c))) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
        }
    }
    if (ok)
    {
        struct pzpd_wcanon *c = (struct pzpd_wcanon *) canon.data;
        size_t nc = canon.len / sizeof(*c);
        if (flags & PZPD_WORDS_CANONICAL) { qsort(c, nc, sizeof(*c), pzpd_cmp_wcanon); }
        // Groups of equal canonical words (the surface view: every word its own group, already sorted)
        uint32_t n = 0;
        uint64_t hb = 0;
        for (size_t i = 0; i < nc; i++) { if ( (i == 0) || pzpd_word_cmp(c[i - 1].c, c[i - 1].clen, c[i].c, c[i].clen) ) { n++; hb += c[i].clen; } }
        w->n = n;
        w->heap    = (char *) malloc(hb ? hb : 1);
        w->offsets = (uint64_t *) malloc(sizeof(uint64_t) * ((size_t) n + 1));
        w->records = (uint64_t *) calloc((size_t) n + 1, sizeof(uint64_t));
        w->count   = (uint64_t *) calloc((size_t) n + 1, sizeof(uint64_t));
        w->sstart  = (uint32_t *) malloc(sizeof(uint32_t) * ((size_t) n + 1));
        // sidx: [0, ns) the surface words of each group in group order; canonical view: [ns, 2 ns) the handle word of each surface word
        w->sidx    = (uint32_t *) malloc(sizeof(uint32_t) * ((flags & PZPD_WORDS_CANONICAL) ? 2 * (size_t) nc + 1 : (size_t) nc + 1));
        if ( (w->heap == NULL) || (w->offsets == NULL) || (w->records == NULL) || (w->count == NULL) || (w->sstart == NULL) || (w->sidx == NULL) )
            { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
        uint64_t o = 0;
        uint32_t h = 0;
        for (size_t i = 0; ok && (i < nc); i++)
        {
            if ( (i == 0) || pzpd_word_cmp(c[i - 1].c, c[i - 1].clen, c[i].c, c[i].clen) )
            {
                if (i > 0) { h++; }
                w->offsets[h] = o;
                w->sstart[h]  = (uint32_t) i;
                memcpy(w->heap + o, c[i].c, c[i].clen);
                o += c[i].clen;
            }
            w->sidx[i] = c[i].sid;
            if (flags & PZPD_WORDS_CANONICAL) { w->sidx[nc + c[i].sid] = h; }
            w->count[h]   += e[c[i].sid].count;
            w->records[h] += e[c[i].sid].records;      // exact for single-word groups; merged groups are recounted below
        }
        if (ok) { w->offsets[n] = o; w->sstart[n] = (uint32_t) nc; }
        // Canonical groups of several surface words: records = |union of their postings| (opens the shards holding them)
        for (uint32_t g = 0; ok && (flags & PZPD_WORDS_CANONICAL) && (g < n); g++)
        {
            if (w->sstart[g + 1] - w->sstart[g] < 2) { continue; }
            ok = pzpd_words_records_buf(w, g, &recs);
            if (ok) { w->records[g] = recs.len / 8; }
        }
    }
    pzpd_sdict_free(&from);
    pzpd_buf_free(&toheap); pzpd_buf_free(&tooff); pzpd_buf_free(&canon); pzpd_buf_free(&recs); pzpd_buf_free(&ents);
    if (!ok)
    {
        struct pzpd_saved_error se;
        pzpd_error_save(&se);
        pzpd_words_close(w);
        pzpd_error_restore(&se);
        return 0;
    }
    *out = w;
    return 1;
}

uint32_t pzpd_words_count(const pzpd_words *w) { return (w == NULL) ? 0 : w->n; }

const char *pzpd_words_word(const pzpd_words *w, uint32_t id, size_t *len)
{
    if ( (w == NULL) || (id >= w->n) ) { return NULL; }
    if (len != NULL) { *len = (size_t)(w->offsets[id + 1] - w->offsets[id]); }
    return w->heap + w->offsets[id];
}

int pzpd_words_stats(const pzpd_words *w, uint32_t id, uint64_t *records, uint64_t *count)
{
    if ( (w == NULL) || (id >= w->n) ) { return 0; }
    if (records != NULL) { *records = w->records[id]; }
    if (count != NULL)   { *count = w->count[id]; }
    return 1;
}

uint32_t pzpd_words_arrays(const pzpd_words *w, const uint64_t **records, const uint64_t **count, const char **heap, const uint64_t **offsets)
{
    if (w == NULL) { return 0; }
    if (records != NULL) { *records = w->records; }
    if (count != NULL)   { *count = w->count; }
    if (heap != NULL)    { *heap = w->heap; }
    if (offsets != NULL) { *offsets = w->offsets; }
    return w->n;
}

int pzpd_words_info(const pzpd_words *w, unsigned *tokenizer, uint64_t *covered_records)
{
    if (w == NULL) { return 0; }
    if (tokenizer != NULL) { *tokenizer = PZPD_TOKENIZER_V1; }
    if (covered_records != NULL) { *covered_records = w->covered; }
    return 1;
}

size_t pzpd_words_records(pzpd_words *w, uint32_t id, uint64_t *ordinals, size_t max)
{
    pzpd_clear_error();
    if ( (w == NULL) || (id >= w->n) || ((ordinals == NULL) && (max > 0)) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    struct pzpd_buf r = {0};
    size_t n = 0;
    if (pzpd_words_records_buf(w, id, &r))
    {
        n = r.len / 8;
        if ( (n > 0) && (max > 0) ) { memcpy(ordinals, r.data, ((n < max) ? n : max) * 8); }
    }
    pzpd_buf_free(&r);
    return n;
}

size_t pzpd_words_of_record(pzpd_words *w, uint64_t ordinal, uint32_t *ids, size_t max)
{
    pzpd_clear_error();
    if ( (w == NULL) || ((ids == NULL) && (max > 0)) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    pzpd *a = w->a;
    unsigned mi; uint64_t local;
    struct pzpd_archive *ar = pzpd_route(a, ordinal, &mi, &local);
    if (ar == NULL) { return 0; }
    int si = pzpd_shard_of(ar, local);
    if (si < 0) { return 0; }
    struct pzpd_wstate *st = pzpd_words_state(w, a->m[mi].shard_base + (unsigned) si);
    if ( (st == NULL) || !st->has ) { return 0; }
    uint64_t r = ordinal - st->first;
    if (r >= st->sub.records) { pzpd_set_error(PZPD_E_FORMAT, "word index: record %llu missing from its shard's index", (unsigned long long) ordinal); return 0; }
    uint32_t b0 = st->sub.fwd_index[r], b1 = st->sub.fwd_index[r + 1];
    if ( (b0 > b1) || (b1 > st->sub.postings) ) { pzpd_set_error(PZPD_E_FORMAT, "word index: forward list of record %llu is damaged", (unsigned long long) ordinal); return 0; }
    size_t n = b1 - b0;
    uint32_t stackIds[256];
    uint32_t *t = (n <= 256) ? stackIds : (uint32_t *) malloc(n * sizeof(uint32_t));
    if (t == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    for (size_t i = 0; i < n; i++)
    {
        uint32_t k = st->sub.fwd[b0 + i];
        if (k >= st->sub.words) { if (t != stackIds) { free(t); } pzpd_set_error(PZPD_E_FORMAT, "word index: forward list of record %llu is damaged", (unsigned long long) ordinal); return 0; }
        t[i] = st->map[k];
    }
    if ( (w->flags & PZPD_WORDS_CANONICAL) && (n > 1) )
    {
        // Surface words of one group map to the same canonical id: sort and deduplicate
        qsort(t, n, sizeof(uint32_t), pzpd_cmp_u32);
        size_t o = 0;
        for (size_t i = 1; i < n; i++) { if (t[i] != t[o]) { t[++o] = t[i]; } }
        n = o + 1;
    }
    if ( (n > 0) && (max > 0) ) { memcpy(ids, t, ((n < max) ? n : max) * sizeof(uint32_t)); }
    if (t != stackIds) { free(t); }
    return n;
}

int pzpd_words_index(pzpd *a, unsigned i, const char **table, const char **column, const char **source_column)
{
    pzpd_clear_error();
    if (a == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL handle"); return 0; }
    const struct pzpd_disk_words *seen[PZPD_MAX_MEMBERS > 64 ? 64 : PZPD_MAX_MEMBERS];
    unsigned nseen = 0;
    for (unsigned mi = 0; mi < a->member_count; mi++)
    {
        struct pzpd_archive *ar = a->m[mi].arch;
        if (ar == NULL) { continue; }
        const struct pzpd_disk_words *dir;
        const unsigned char *map;
        uint64_t mlen;
        int manifest = !ar->standalone;
        if (manifest) { if (ar->mwords_bad) { continue; } dir = ar->mwords; map = ar->mmap_manifest; mlen = ar->manifest_len; }
        else
        {
            struct pzpd_rshard *s = pzpd_shard(ar, 0);
            if ( (s == NULL) || s->words_bad ) { pzpd_clear_error(); continue; }
            dir = s->sb.words; map = s->map; mlen = s->map_len;
        }
        for (unsigned j = 0; j < pzpd_words_used(dir); j++)
        {
            int dup = 0;
            for (unsigned k = 0; k < nseen; k++) { if ( !strncmp(seen[k]->table, dir[j].table, 24) && !strncmp(seen[k]->column, dir[j].column, 24) ) { dup = 1; } }
            if (dup) { continue; }
            if (nseen == i)
            {
                struct pzpd_wsec v;
                const char *src = "";
                if ( pzpd_check_section(map, mlen, dir[j].section_offset, manifest ? PZPD_SECT_MWORDS : PZPD_SECT_WORDS, dir[j].section_bytes) &&
                     pzpd_wsec_parse(map + dir[j].section_offset, dir[j].section_bytes, manifest, &v) )
                    { src = (const char *) (map + dir[j].section_offset + offsetof(struct pzpd_disk_words_head, source_column)); }
                pzpd_clear_error();
                if (table != NULL)         { *table = dir[j].table; }
                if (column != NULL)        { *column = dir[j].column; }
                if (source_column != NULL) { *source_column = src; }
                return 1;
            }
            if (nseen < sizeof(seen) / sizeof(seen[0])) { seen[nseen++] = &dir[j]; }
        }
    }
    return 0;
}

size_t pzpd_words_sources(pzpd *a, const char *table, const char *column, const char **names, size_t *lens, size_t max)
{
    pzpd_clear_error();
    if ( (a == NULL) || (table == NULL) || (column == NULL) || (((names == NULL) || (lens == NULL)) && (max > 0)) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    struct pzpd_buf all = {0};
    int ok = 1;
    for (unsigned mi = 0; ok && (mi < a->member_count); mi++)
    {
        struct pzpd_wsec v;
        struct pzpd_wsub sub;
        int has = 0;
        int r = pzpd_words_member_vocab(a, mi, table, column, NULL, 0, &v, &sub, &has);
        if (r < 0) { ok = 0; break; }
        if (r == 0) { continue; }
        for (unsigned k = 1; ok && (k < v.nsub); k++)
        {
            struct pzpd_wsub sk;
            ok = pzpd_wsec_sub(&v, k, v.manifest ? -1 : (int64_t) sub.records, &sk);
            struct pzpd_bstr b = { sk.source, sk.source_len };
            if (ok && !pzpd_buf_append(&all, &b, sizeof(b))) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
        }
    }
    size_t n = 0;
    if (ok)
    {
        struct pzpd_bstr *b = (struct pzpd_bstr *) all.data;
        n = all.len / sizeof(*b);
        if (n > 1)
        {
            qsort(b, n, sizeof(*b), pzpd_cmp_bstr);
            size_t o = 0;
            for (size_t i = 1; i < n; i++) { if (pzpd_cmp_bstr(&b[o], &b[i]) != 0) { b[++o] = b[i]; } }
            n = o + 1;
        }
        for (size_t i = 0; (i < n) && (i < max); i++) { names[i] = b[i].s; lens[i] = b[i].len; }
    }
    pzpd_buf_free(&all);
    return n;
}

//-----------------------------------------------------------------------------------------------
// Collection files
//-----------------------------------------------------------------------------------------------

/** @brief Directory part of a path, canonicalised (the file itself need not exist). Caller frees. */
static char *pzpd_canonical_dir_of(const char *path)
{
    char dir[4096];
    const char *slash = strrchr(path, '/');
    if (slash == NULL) { snprintf(dir, sizeof(dir), "."); }
    else if (slash == path) { snprintf(dir, sizeof(dir), "/"); }
    else { snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path); }
    return realpath(dir, NULL);
}

/** @brief Store a member path relative to the collection's directory when both live under the same
 *  top-level directory (e.g. both in /home), otherwise absolute (e.g. a member on /dev/shm).
 *  @return malloc'd path, NULL on failure (error set). */
static char *pzpd_member_store_path(const char *collDir, const char *member, int absolute)
{
    char *abs = realpath(member, NULL);
    if (abs == NULL) { pzpd_set_error(PZPD_E_IO, "cannot resolve %s: %s", member, strerror(errno)); return NULL; }
    if (absolute || (collDir == NULL)) { return abs; }
    // Same top-level component?
    const char *a1 = strchr(collDir + 1, '/'), *b1 = strchr(abs + 1, '/');
    size_t la = (a1 == NULL) ? strlen(collDir) : (size_t)(a1 - collDir);
    size_t lb = (b1 == NULL) ? strlen(abs) : (size_t)(b1 - abs);
    if ( (la != lb) || (strncmp(collDir, abs, la) != 0) || (strcmp(collDir, "/") == 0) ) { return abs; }
    // Longest common directory prefix, then "../" for each remaining component of collDir
    size_t common = 0, i = 0;
    while ( (collDir[i] != 0) && (abs[i] != 0) && (collDir[i] == abs[i]) )
    {
        i++;
        if ( (collDir[i] == 0 || collDir[i] == '/') && (abs[i] == '/') ) { common = i; }
    }
    size_t ups = 0;
    for (const char *p = collDir + common; *p != 0; p++) { if (*p == '/') { ups++; } }
    const char *rest = abs + common + 1;
    char *rel = (char *) malloc(ups * 3 + strlen(rest) + 1);
    if (rel == NULL) { free(abs); pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return NULL; }
    size_t o = 0;
    for (size_t k = 0; k < ups; k++) { memcpy(rel + o, "../", 3); o += 3; }
    strcpy(rel + o, rest);
    free(abs);
    return rel;
}

/** @brief Write a collection file. absolute[i] (may be NULL) forces an absolute path per member. */
static int pzpd_coll_write(const char *out, const char *const *paths, const char *const *aliases, unsigned n, const int *absolute)
{
    if ( (out == NULL) || (paths == NULL) || (n == 0) || (n > PZPD_MAX_MEMBERS) ) { pzpd_set_error(PZPD_E_ARG, "need an output path and 1..%d members", PZPD_MAX_MEMBERS); return 0; }
    pzpd *a = pzpd_open_many(paths, aliases, n, 0);           // every member must be present to be recorded
    if (a == NULL) { return 0; }
    char *collDir = pzpd_canonical_dir_of(out);
    struct pzpd_buf members = {0}, heap = {0}, remap = {0};
    int ok = (collDir != NULL);
    if (!ok) { pzpd_set_error(PZPD_E_IO, "cannot resolve the directory of %s: %s", out, strerror(errno)); }
    for (unsigned i = 0; ok && (i < n); i++)
    {
        struct pzpd_member *mb = &a->m[i];
        int absPath = (absolute != NULL) && absolute[i];
        char *stored = pzpd_member_store_path(collDir, paths[i], absPath);
        if (stored == NULL) { ok = 0; break; }
        struct pzpd_disk_member dm;
        memset(&dm, 0, sizeof(dm));
        dm.first_ordinal = mb->first;
        dm.record_count  = mb->count;
        memcpy(dm.archive_uuid, mb->arch->uuid, 16);
        dm.path_offset   = (uint32_t) heap.len;
        dm.path_len      = (uint32_t) strlen(stored);
        dm.flags         = (stored[0] == '/') ? 1u : 0u;
        ok = pzpd_buf_append(&heap, stored, strlen(stored));
        free(stored);
        dm.alias_offset  = (uint32_t) heap.len;
        dm.alias_len     = (uint32_t) strlen(mb->alias);
        ok = ok && pzpd_buf_append(&heap, mb->alias, strlen(mb->alias)) && pzpd_buf_append(&members, &dm, sizeof(dm));
        uint8_t rm[PZPD_MAX_STREAMS];
        for (unsigned u = 0; u < PZPD_MAX_STREAMS; u++) { rm[u] = (uint8_t)((mb->to_member[u] < 0) ? 0xFF : mb->to_member[u]); }
        ok = ok && pzpd_buf_append(&remap, rm, sizeof(rm));
    }

    char *tmp = NULL;
    int fd = -1;
    if (ok)
    {
        size_t tl = strlen(out) + 8;
        tmp = (char *) malloc(tl);
        if (tmp == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
        else
        {
            snprintf(tmp, tl, "%s.tmp", out);
            fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd < 0) { pzpd_set_error(PZPD_E_IO, "cannot create %s: %s", tmp, strerror(errno)); ok = 0; }
        }
    }
    if (ok)
    {
        struct pzpd_disk_collection h;
        memset(&h, 0, sizeof(h));
        memcpy(h.magic, PZPD_MAGIC_COLL, 8);
        h.version       = PZPD_FORMAT_VERSION;
        h.total_records = a->total;
        h.member_count  = n;
        h.stream_count  = a->S;
        pzpd_fill_streams(h.streams, a->S, a->streams);
        XXH64_state_t *idx = XXH64_createState();
        if (idx == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
        else
        {
            XXH64_reset(idx, 0);
            uint64_t off = PZPD_BLOCK;
            ok = pzpd_write_section(fd, &off, PZPD_SECT_CMEMBERS, members.data, members.len, &h.members_offset, idx) &&
                 pzpd_write_section(fd, &off, PZPD_SECT_CHEAP,    heap.data,    heap.len,    &h.heap_offset,    idx) &&
                 pzpd_write_section(fd, &off, PZPD_SECT_CREMAP,   remap.data,   remap.len,   &h.remap_offset,   idx);
            h.index_checksum = XXH64_digest(idx);
            XXH64_freeState(idx);
            h.heap_bytes  = heap.len;
            h.file_bytes  = off;
            h.sb_checksum = XXH64(&h, offsetof(struct pzpd_disk_collection, sb_checksum), 0);
            unsigned char block[PZPD_BLOCK];
            memset(block, 0, sizeof(block));
            memcpy(block, &h, sizeof(h));
            ok = ok && pzpd_pwrite_all(fd, block, PZPD_BLOCK, 0);
            if (ok && (fsync(fd) != 0)) { pzpd_set_error(PZPD_E_IO, "fsync %s: %s", tmp, strerror(errno)); ok = 0; }
        }
    }
    if (fd >= 0) { close(fd); }
    if (ok && (rename(tmp, out) != 0)) { pzpd_set_error(PZPD_E_IO, "rename %s -> %s: %s", tmp, out, strerror(errno)); ok = 0; }
    if (ok) { pzpd_fsync_dir_of(out); }
    else if (tmp != NULL) { unlink(tmp); }
    free(tmp);
    free(collDir);
    pzpd_buf_free(&members);
    pzpd_buf_free(&heap);
    pzpd_buf_free(&remap);
    struct pzpd_saved_error e;
    pzpd_error_save(&e);
    pzpd_close(a);
    if (!ok) { pzpd_error_restore(&e); }
    return ok;
}

int pzpd_collection_write(const char *out_path, const char *const *paths, const char *const *aliases, unsigned n, unsigned flags)
{
    pzpd_clear_error();
    if ( (n == 0) || (n > PZPD_MAX_MEMBERS) ) { pzpd_set_error(PZPD_E_ARG, "need 1..%d members", PZPD_MAX_MEMBERS); return 0; }
    int *absolute = (int *) calloc(n, sizeof(int));
    if (absolute == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    for (unsigned i = 0; i < n; i++) { absolute[i] = (flags & PZPD_COLL_ABSOLUTE) ? 1 : 0; }
    int ok = pzpd_coll_write(out_path, paths, aliases, n, absolute);
    free(absolute);
    return ok;
}

int pzpd_collection_refresh(const char *path)
{
    pzpd_clear_error();
    struct pzpd_coll_file c;
    if ( (path == NULL) || !pzpd_coll_parse(path, &c) ) { return 0; }
    unsigned n = c.h.member_count;
    char **paths   = (char **) calloc(n, sizeof(char *));
    char **aliases = (char **) calloc(n, sizeof(char *));
    int   *absolute = (int *) calloc(n, sizeof(int));
    int ok = (paths != NULL) && (aliases != NULL) && (absolute != NULL);
    if (!ok) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); }
    for (unsigned i = 0; ok && (i < n); i++)
    {
        char *stored = pzpd_heap_str(c.heap, c.members[i].path_offset, c.members[i].path_len);
        aliases[i]  = pzpd_heap_str(c.heap, c.members[i].alias_offset, c.members[i].alias_len);
        paths[i]    = (stored != NULL) ? pzpd_resolve_member_path(path, stored) : NULL;
        absolute[i] = (c.members[i].flags & 1u) ? 1 : 0;
        free(stored);
        if ( (paths[i] == NULL) || (aliases[i] == NULL) ) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
    }
    free(c.buf);
    if (ok) { ok = pzpd_coll_write(path, (const char *const *) paths, (const char *const *) aliases, n, absolute); }
    for (unsigned i = 0; i < n; i++) { if (paths != NULL) { free(paths[i]); } if (aliases != NULL) { free(aliases[i]); } }
    free(paths);
    free(aliases);
    free(absolute);
    return ok;
}

//-----------------------------------------------------------------------------------------------
// Storage kind
//-----------------------------------------------------------------------------------------------

int pzpd_storage_kind(pzpd *a, uint64_t ordinal)
{
    unsigned mi; uint64_t local, sl;
    pzpd_clear_error();
    struct pzpd_archive *ar = pzpd_route(a, ordinal, &mi, &local);
    if (ar == NULL) { return pzpd_errorCode; }
    struct pzpd_rshard *s = pzpd_locate(ar, local, &sl);
    return (s == NULL) ? pzpd_errorCode : s->storage;
}

//-----------------------------------------------------------------------------------------------
// Prefetcher (spec §6). Each shard gets one of three modes:
//  - MAP and PAGECACHE hand out mmap views: the I/O threads only make the records of the schedule
//    resident (and their page tables filled) ahead of the consumers.
//  - BUFFERS reads records with O_DIRECT (buffered pread where refused) into private, page-aligned
//    buffers counted against budget_bytes, so the page cache doesn't grow. A get moves the buffer
//    into the ticket; release frees it.
// A schedule entry has an I/O state (queued → in flight → ready) and a claim state (free →
// claimed by a get → done by release / discard). The schedule is split into PZPD_PF_LANES lanes
// by ordinal, each with its own lock, entries, ordinal hash and buffer slots. `outstanding`
// (entries whose I/O was started and whose claim isn't done: the window limits it) and `used`
// (bytes of prefetched buffers plus the tickets' buffers: the budget limits it) are atomic
// counters shared by the lanes; an I/O thread reserves both before it starts an entry.
//-----------------------------------------------------------------------------------------------

#define PZPD_PF_GAP  (256u * 1024u)   ///< Gaps between requested blobs up to this size are read over (one range)
#define PZPD_PF_NONE 0xFFFFFFFFu      ///< No schedule position / no buffer slot
#define PZPD_PF_DIO  4096u            ///< O_DIRECT granularity: file offsets, lengths and buffers aligned to this
#define PZPD_PF_BATCH 8u              ///< MAP / PAGECACHE entries an I/O thread starts per lock hold; also the free window an idle I/O thread is woken for
/** @brief Lanes of a prefetcher. The schedule is split by ordinal (a hash of it) into this many parts, each with its own
 *  lock, so consumers and I/O threads working on different records rarely wait for each other; only the window and the
 *  budget span the lanes (atomic counters). I/O threads serve the lanes round-robin: with fewer threads than lanes each
 *  serves several, with more several serve each lane. */
#define PZPD_PF_LANES 8u

enum { PZPD_PF_QUEUED = 0, PZPD_PF_INFLIGHT = 1, PZPD_PF_READY = 2 };   ///< I/O state of a schedule entry
enum { PZPD_PF_FREE = 0, PZPD_PF_CLAIMED = 1, PZPD_PF_DONE = 2 };       ///< Claim state of a schedule entry

/** @brief One submitted claim (40 bytes). */
struct pzpd_pf_entry
{
    uint64_t ordinal;  ///< Record
    uint64_t seq;      ///< Position in the whole schedule (over all lanes): it may start once seq < claims done + window
    uint64_t need;     ///< Buffer bytes when the record's shard is in BUFFERS mode, else 0 (computed at submit, outside the lock)
    uint32_t mask;     ///< Streams to prefetch
    uint32_t next;     ///< Next claim of the same ordinal, PZPD_PF_NONE if none
    uint32_t slot;     ///< Buffer slot (BUFFERS shards, while in flight or ready), PZPD_PF_NONE otherwise
    uint8_t  io;       ///< PZPD_PF_QUEUED / _INFLIGHT / _READY
    uint8_t  claim;    ///< PZPD_PF_FREE / _CLAIMED / _DONE
};

/** @brief Hash slot: the claims of one ordinal, as a list through pzpd_pf_entry::next. */
struct pzpd_pf_hslot
{
    uint64_t ordinal;  ///< Record
    uint32_t head;     ///< First claim that may still be free (PZPD_PF_NONE = empty slot)
    uint32_t tail;     ///< Last claim
};

/** @brief A prefetched record's private buffer (BUFFERS shards). */
struct pzpd_pf_buf
{
    unsigned char *data;  ///< Page-aligned buffer, NULL until read (or if the read failed)
    uint64_t       bytes; ///< Bytes counted against the budget
    uint32_t       mask;  ///< Streams it holds
};

/** @brief Where one blob of a record lives in its shard file. */
struct pzpd_pf_loc
{
    uint64_t off;      ///< File offset
    uint32_t size;     ///< Bytes
    uint32_t format;   ///< FourCC
    int      mstream;  ///< Member stream id
    int      present;  ///< 1 if the record has this blob
};

/** @brief A byte range [lo, hi) of a shard file. */
struct pzpd_pf_range
{
    uint64_t lo;  ///< First byte
    uint64_t hi;  ///< One past the last byte
};

/** @brief A mutex on a cache line of its own (64-byte aligned). A struct holding one is aligned the same way, so
 *  lanes and I/O threads next to each other in an array never share a line (no false sharing between their locks). */
typedef pthread_mutex_t pzpd_cacheline_mutex __attribute__((aligned(64)));

/** @brief One lane: the part of the schedule whose ordinals hash to it (pzpd_pf_lane_of()), with its own lock.
 *  Consumers and I/O threads working on records of different lanes never wait for each other. */
struct pzpd_pf_lane
{
    pzpd_cacheline_mutex lock;     ///< Guards every field below
    pthread_cond_t   ready;        ///< Gets and clears wait here for this lane's in-flight I/O
    struct pzpd_pf_entry *e;       ///< This lane's claims, in submission order
    uint64_t         n;            ///< Entries
    uint64_t         cap;          ///< Allocated entries
    struct pzpd_pf_hslot *slots;   ///< Ordinal hash (linear probing), hcap slots
    uint64_t         hcap;         ///< Slots, a power of two (0 before the first submit)
    uint64_t         hcount;       ///< Distinct ordinals in the hash
    struct pzpd_pf_buf *bufs;      ///< Buffer slots, grown on demand up to `window`
    uint32_t        *free_bufs;    ///< Free buffer slot ids
    unsigned         nbufs;        ///< Buffer slots allocated
    unsigned         nfree;        ///< Free buffer slots
    uint64_t         cursor;       ///< Next entry the I/O threads look at
    unsigned         inflight;     ///< Entries being prefetched right now
    uint64_t         gen;          ///< Schedule generation, bumped by pzpd_prefetch_clear()
    pzpd_prefetch_stats st;        ///< This lane's counters (pzpd_prefetch_stats_get() adds the lanes up)
};

_Static_assert(_Alignof(struct pzpd_pf_lane) == 64, "a lane starts a cache line");

/** @brief Parking states of an I/O thread (pzpd_pf_worker::parked). */
enum { PZPD_PF_AWAKE = 0, PZPD_PF_PARKED_EMPTY = 1, PZPD_PF_PARKED_STARVED = 2 };

/** @brief An I/O thread and its parking place: it sleeps here when none of its lanes has an entry it may start. */
struct pzpd_pf_worker
{
    struct pzpd_prefetcher *p;     ///< Prefetcher
    unsigned         id;           ///< Thread index
    pzpd_cacheline_mutex m;        ///< Guards the sleep
    pthread_cond_t   c;            ///< Signalled by pzpd_pf_wake_worker()
    int              wake;         ///< Atomic: set by a waker, reset by the thread before its last look at its lanes
    int              parked;       ///< Atomic: PZPD_PF_AWAKE, _PARKED_EMPTY (no queued entries) or _PARKED_STARVED (window / budget full)
};

_Static_assert(_Alignof(struct pzpd_pf_worker) == 64, "an I/O thread's parking place starts a cache line");

/** @brief Prefetcher state (see the section comment above). Allocated 64-byte aligned (the lanes are). */
struct pzpd_prefetcher
{
    struct pzpd_pf_lane lane[PZPD_PF_LANES]; ///< The lanes
    pzpd            *a;            ///< Handle
    uint32_t         mask;         ///< Default stream mask
    unsigned         window;       ///< Most outstanding entries, over all lanes
    uint64_t         budget;       ///< BUFFERS byte budget, over all lanes
    uint8_t         *shard_mode;   ///< Per shard: PZPD_PF_MAP, _PAGECACHE or _BUFFERS
    int             *dfd;          ///< Per shard: O_DIRECT descriptor in use, -1 = buffered pread (read atomically)
    int             *dfd_open;     ///< Per shard: O_DIRECT descriptor to close at destroy, -1 if none
    int              any_buffers;  ///< 1 if some shard is in BUFFERS mode
    pthread_mutex_t  ctl;          ///< Serialises submit and clear (taken before a lane lock, never while holding one)
    unsigned         nworkers;     ///< I/O threads planned (their lanes depend on it)
    unsigned         nthreads;     ///< I/O threads started
    struct pzpd_pf_worker *w;      ///< I/O threads' parking places (nworkers, 64-byte aligned)
    pthread_t       *threads;      ///< I/O threads
    int              stop;         ///< Atomic: set by pzpd_prefetcher_destroy()
    uint64_t         outstanding;  ///< Atomic: started entries whose claim isn't done (the window limits it)
    uint64_t         done;         ///< Atomic: claims done (released / discarded) since the last clear: the schedule's progress
    uint64_t         seq_next;     ///< Schedule position of the next submitted claim (under `ctl`)
    uint64_t         used;         ///< Atomic: buffer bytes in slots and tickets (the budget limits it)
    uint64_t         used_peak;    ///< Atomic: highest `used`
    uint64_t         stalls;       ///< Atomic: times an I/O thread parked with queued entries (window / budget full)
    uint64_t         fallbacks;    ///< Atomic: BUFFERS shards that fell back to buffered reads
    pzpd_prefetch_stats st;        ///< Counters fixed at create (shard modes)
    struct timespec  t0;           ///< Creation time
};

/** @brief Atomic load of a shared prefetcher counter. */
static inline uint64_t pzpd_atomic_get(const uint64_t *v) { return __atomic_load_n(v, __ATOMIC_SEQ_CST); }
/** @brief Atomic add to a shared prefetcher counter. */
static inline void pzpd_atomic_add(uint64_t *v, uint64_t d) { (void) __atomic_add_fetch(v, d, __ATOMIC_SEQ_CST); }
/** @brief Atomic subtract from a shared prefetcher counter. */
static inline void pzpd_atomic_sub(uint64_t *v, uint64_t d) { (void) __atomic_sub_fetch(v, d, __ATOMIC_SEQ_CST); }
/** @brief Atomic store to a shared prefetcher counter. */
static inline void pzpd_atomic_set(uint64_t *v, uint64_t x) { __atomic_store_n(v, x, __ATOMIC_SEQ_CST); }
/** @brief Atomic load of a shared flag. */
static inline int pzpd_atomic_geti(const int *v) { return __atomic_load_n(v, __ATOMIC_SEQ_CST); }
/** @brief Atomic store of a shared flag. */
static inline void pzpd_atomic_seti(int *v, int x) { __atomic_store_n(v, x, __ATOMIC_SEQ_CST); }

/** @brief Reserve `amount` of a limit shared by the lanes: *v += amount unless that passes `limit`.
 *  @param first When 1, a zero *v always takes the reservation (one record always fits the budget).
 *  @return 1 if reserved, 0 if the limit is reached. */
static int pzpd_atomic_reserve(uint64_t *v, uint64_t amount, uint64_t limit, int first)
{
    uint64_t cur = __atomic_load_n(v, __ATOMIC_SEQ_CST);
    for (;;)
    {
        if ( !((first && (cur == 0)) || (cur + amount <= limit)) ) { return 0; }
        if (__atomic_compare_exchange_n(v, &cur, cur + amount, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) { return 1; }
    }
}

/** @brief Raise a shared maximum to at least x. */
static void pzpd_atomic_max(uint64_t *v, uint64_t x)
{
    uint64_t cur = __atomic_load_n(v, __ATOMIC_SEQ_CST);
    while ( (cur < x) && !__atomic_compare_exchange_n(v, &cur, x, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ) { }
}

/** @brief Lane of an ordinal (high bits of a multiplicative hash: the lanes' ordinal hashes use the low ones). */
static inline unsigned pzpd_pf_lane_of(uint64_t ordinal)
{
    return (unsigned)(((ordinal * 0x9E3779B97F4A7C15ull) >> 40) % PZPD_PF_LANES);
}

/** @brief Seconds elapsed since t0. */
static double pzpd_seconds_since(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - t0->tv_sec) + (double)(t.tv_nsec - t0->tv_nsec) * 1e-9;
}

/** @brief Hash slot of an ordinal: the existing one, or the empty slot where it would go. hcap must be > 0. */
static struct pzpd_pf_hslot *pzpd_pf_slot_of(struct pzpd_pf_hslot *slots, uint64_t hcap, uint64_t ordinal)
{
    uint64_t i = (ordinal * 0x9E3779B97F4A7C15ull) & (hcap - 1);
    while ( (slots[i].head != PZPD_PF_NONE) && (slots[i].ordinal != ordinal) ) { i = (i + 1) & (hcap - 1); }
    return &slots[i];
}

/** @brief Make room for `more` distinct ordinals in a lane (load ≤ ½). Lane lock held. @return 1, or 0 if out of memory. */
static int pzpd_pf_hash_reserve(struct pzpd_pf_lane *ln, uint64_t more)
{
    if (2 * (ln->hcount + more) <= ln->hcap) { return 1; }
    uint64_t nc = (ln->hcap == 0) ? 1024 : ln->hcap;
    while (nc < 2 * (ln->hcount + more)) { nc *= 2; }
    struct pzpd_pf_hslot *ns = (struct pzpd_pf_hslot *) malloc(nc * sizeof(struct pzpd_pf_hslot));
    if (ns == NULL) { return 0; }
    memset(ns, 0xFF, nc * sizeof(struct pzpd_pf_hslot));      // head = PZPD_PF_NONE everywhere
    for (uint64_t i = 0; i < ln->hcap; i++)
    {
        if (ln->slots[i].head != PZPD_PF_NONE) { *pzpd_pf_slot_of(ns, nc, ln->slots[i].ordinal) = ln->slots[i]; }
    }
    free(ln->slots);
    ln->slots = ns;
    ln->hcap  = nc;
    return 1;
}

/** @brief First free claim of an ordinal in its lane. Lane lock held. @return Its position, or UINT64_MAX if none. */
static uint64_t pzpd_pf_claim(struct pzpd_pf_lane *ln, uint64_t ordinal)
{
    if (ln->hcap == 0) { return UINT64_MAX; }
    struct pzpd_pf_hslot *sl = pzpd_pf_slot_of(ln->slots, ln->hcap, ordinal);
    if (sl->head == PZPD_PF_NONE) { return UINT64_MAX; }
    uint32_t i = sl->head;
    while ( (i != PZPD_PF_NONE) && (ln->e[i].claim != PZPD_PF_FREE) ) { i = ln->e[i].next; }
    sl->head = (i != PZPD_PF_NONE) ? i : sl->tail;              // claims never become free again: skip them next time
    return (i != PZPD_PF_NONE) ? i : UINT64_MAX;
}

/** @brief Free the buffer slot of entry i, if it holds one. Lane lock held. @return The budget bytes given back. */
static uint64_t pzpd_pf_drop_slot(struct pzpd_prefetcher *p, struct pzpd_pf_lane *ln, uint64_t i)
{
    uint32_t k = ln->e[i].slot;
    if (k == PZPD_PF_NONE) { return 0; }
    uint64_t bytes = ln->bufs[k].bytes;
    free(ln->bufs[k].data);
    pzpd_atomic_sub(&p->used, bytes);
    ln->bufs[k].data = NULL;
    ln->bufs[k].bytes = 0;
    ln->free_bufs[ln->nfree++] = k;
    ln->e[i].slot = PZPD_PF_NONE;
    return bytes;
}

/** @brief Mark a claim done, giving back its window place (and buffer, once its read finished) if its I/O was started.
 *  Lane lock held. @return What it freed for pzpd_pf_wake(): bit 0 window, bit 1 budget. */
static int pzpd_pf_done(struct pzpd_prefetcher *p, struct pzpd_pf_lane *ln, uint64_t i)
{
    ln->e[i].claim = PZPD_PF_DONE;
    int freed = 0;
    if ( (ln->e[i].io == PZPD_PF_READY) && (pzpd_pf_drop_slot(p, ln, i) > 0) ) { freed |= 2; }   // in flight: the I/O thread frees it when done
    if (ln->e[i].io != PZPD_PF_QUEUED) { pzpd_atomic_sub(&p->outstanding, 1); freed |= 1; }
    // Progress lets later entries start: count it as window room once per batch of claims
    if ( (__atomic_add_fetch(&p->done, 1, __ATOMIC_SEQ_CST) % PZPD_PF_BATCH) == 0 ) { freed |= 1; }
    return freed;
}

/** @brief Grow a lane's buffer slots (doubling, at most `window`: no more entries than that are ever started).
 *  Lane lock held. @return 1 if a slot was added, 0 otherwise. */
static int pzpd_pf_grow_bufs(struct pzpd_prefetcher *p, struct pzpd_pf_lane *ln)
{
    if (ln->nbufs >= p->window) { return 0; }
    unsigned nb = (ln->nbufs == 0) ? 16 : 2 * ln->nbufs;
    if (nb > p->window) { nb = p->window; }
    struct pzpd_pf_buf *b = (struct pzpd_pf_buf *) realloc(ln->bufs, nb * sizeof(struct pzpd_pf_buf));
    if (b == NULL) { return 0; }
    ln->bufs = b;
    uint32_t *f = (uint32_t *) realloc(ln->free_bufs, nb * sizeof(uint32_t));
    if (f == NULL) { return 0; }
    ln->free_bufs = f;
    for (unsigned k = ln->nbufs; k < nb; k++) { memset(&ln->bufs[k], 0, sizeof(ln->bufs[k])); ln->free_bufs[ln->nfree++] = k; }
    ln->nbufs = nb;
    return 1;
}

/** @brief Wake one I/O thread (it rechecks its lanes). No lane lock may be held. */
static void pzpd_pf_wake_worker(struct pzpd_pf_worker *w)
{
    pzpd_atomic_seti(&w->wake, 1);
    pthread_mutex_lock(&w->m);
    pthread_cond_signal(&w->c);
    pthread_mutex_unlock(&w->m);
}

/** @brief Wake parked I/O threads after something changed. No lane lock may be held.
 *  @param freed Bits of pzpd_pf_done(): 1 window place (threads parked with queued entries are woken once a whole
 *               batch fits, so a release costs a wake-up per batch, not per record), 2 budget bytes.
 *  @param all   1 for new entries, clear and destroy: every parked thread is woken. */
static void pzpd_pf_wake(struct pzpd_prefetcher *p, int freed, int all)
{
    if ( !all && !(freed & 2) )
    {
        uint64_t room = (p->window < PZPD_PF_BATCH) ? p->window : PZPD_PF_BATCH;
        if ( !(freed & 1) || (pzpd_atomic_get(&p->outstanding) + room > p->window) ) { return; }
    }
    for (unsigned t = 0; t < p->nworkers; t++)
    {
        int parked = pzpd_atomic_geti(&p->w[t].parked);
        if ( (parked == PZPD_PF_PARKED_STARVED) || (all && (parked != PZPD_PF_AWAKE)) ) { pzpd_pf_wake_worker(&p->w[t]); }
    }
}

/** @brief File locations of a record's requested, present blobs (indexed by merged stream).
 *  @return The record's shard (its global index in *shard, record index in *local), or NULL on error (error set). */
static struct pzpd_rshard *pzpd_pf_locate(pzpd *a, uint64_t ordinal, uint32_t mask, struct pzpd_pf_loc *loc, unsigned *shard, uint64_t *local)
{
    unsigned mi; uint64_t ml, sl;
    struct pzpd_archive *ar = pzpd_route(a, ordinal, &mi, &ml);
    if (ar == NULL) { return NULL; }
    struct pzpd_rshard *s = pzpd_locate(ar, ml, &sl);
    if (s == NULL) { return NULL; }
    *shard = a->m[mi].shard_base + (unsigned)(s - ar->shards);
    *local = sl;
    memset(loc, 0, sizeof(struct pzpd_pf_loc) * a->S);
    for (unsigned u = 0; u < a->S; u++)
    {
        int ms = a->m[mi].to_member[u];
        if ( !(mask & (1u << u)) || (ms < 0) ) { continue; }
        const struct pzpd_disk_blob *b = pzpd_blob_entry(s, sl, (unsigned) ms);
        if (b == NULL) { return NULL; }
        if (b->rel_offset == PZPD_MISSING) { continue; }
        loc[u].off     = s->rtab[sl].offset + b->rel_offset;
        loc[u].size    = b->size;
        loc[u].format  = b->format;
        loc[u].mstream = ms;
        loc[u].present = 1;
    }
    return s;
}

/** @brief Non-empty blobs of loc as file ranges sorted by offset; ranges closer than PZPD_PF_GAP are merged.
 *  @return Number of ranges. */
static unsigned pzpd_pf_ranges(const struct pzpd_pf_loc *loc, unsigned S, struct pzpd_pf_range *r, uint64_t *overRead)
{
    unsigned n = 0;
    for (unsigned u = 0; u < S; u++)
    {
        if ( !loc[u].present || (loc[u].size == 0) ) { continue; }
        unsigned k = n++;                                          // insertion sort by offset (≤ 32 entries)
        while ( (k > 0) && (r[k - 1].lo > loc[u].off) ) { r[k] = r[k - 1]; k--; }
        r[k].lo = loc[u].off;
        r[k].hi = loc[u].off + loc[u].size;
    }
    unsigned m = 0;
    *overRead = 0;
    for (unsigned k = 0; k < n; k++)
    {
        if ( (m > 0) && (r[k].lo >= r[m - 1].hi) && (r[k].lo - r[m - 1].hi <= PZPD_PF_GAP) )
        {
            *overRead += r[k].lo - r[m - 1].hi;
            if (r[k].hi > r[m - 1].hi) { r[m - 1].hi = r[k].hi; }
        }
        else { r[m++] = r[k]; }
    }
    return m;
}

/** @brief Bytes of the page-aligned buffer holding ranges r (each range rounded out to PZPD_PF_DIO). */
static uint64_t pzpd_pf_buffer_bytes(const struct pzpd_pf_range *r, unsigned n)
{
    uint64_t total = 0;
    for (unsigned k = 0; k < n; k++)
    {
        total += ((r[k].hi + PZPD_PF_DIO - 1) & ~(uint64_t)(PZPD_PF_DIO - 1)) - (r[k].lo & ~(uint64_t)(PZPD_PF_DIO - 1));
    }
    return (total > 0) ? total : PZPD_PF_DIO;
}

/** @brief Read ranges r of shard `shard` into a new page-aligned buffer, with O_DIRECT where the shard has it.
 *  @return The buffer (pzpd_pf_buffer_bytes() bytes), or NULL on error (error set). */
static unsigned char *pzpd_pf_read(struct pzpd_prefetcher *p, unsigned shard, const struct pzpd_rshard *s, const struct pzpd_pf_range *r, unsigned n)
{
    void *mem = NULL;
    if (posix_memalign(&mem, PZPD_PF_DIO, (size_t) pzpd_pf_buffer_bytes(r, n)) != 0) { pzpd_set_error(PZPD_E_NOMEM, "out of memory for a record buffer"); return NULL; }
    unsigned char *buf = (unsigned char *) mem;
    uint64_t bo = 0;
    for (unsigned k = 0; k < n; k++)
    {
        uint64_t alo = r[k].lo & ~(uint64_t)(PZPD_PF_DIO - 1);
        uint64_t ahi = (r[k].hi + PZPD_PF_DIO - 1) & ~(uint64_t)(PZPD_PF_DIO - 1);
        uint64_t got = 0, need = r[k].hi - alo;
        while (got < need)
        {
            int fd = __atomic_load_n(&p->dfd[shard], __ATOMIC_RELAXED);
            int direct = (fd >= 0);
            if (!direct) { fd = s->fd; }
            // O_DIRECT reads whole aligned blocks (the last one may end at EOF); buffered reads only what's needed
            size_t len = direct ? (size_t)(ahi - alo - got) : (size_t)(need - got);
            ssize_t rd = pread(fd, buf + bo + got, len, (off_t)(alo + got));
            if ( (rd < 0) && (errno == EINTR) ) { continue; }
            if ( (rd < 0) && direct && (errno == EINVAL) )
            {
                // The file system refuses O_DIRECT for this read: buffered reads from now on (counted once)
                if (__atomic_exchange_n(&p->dfd[shard], -1, __ATOMIC_RELAXED) >= 0) { pzpd_atomic_add(&p->fallbacks, 1); }
                continue;
            }
            if (rd <= 0) { free(buf); pzpd_set_error(PZPD_E_IO, "%s: read failed at %llu: %s", s->path, (unsigned long long)(alo + got), (rd < 0) ? strerror(errno) : "unexpected end of file"); return NULL; }
            got += (uint64_t) rd;
        }
        bo += ahi - alo;
    }
    return buf;
}

/** @brief Point refs at the wanted blobs inside a buffer holding ranges r.
 *  @return 1 if every wanted present blob is inside the buffer, 0 if some aren't (refs then incomplete). */
static int pzpd_pf_buffer_refs(const struct pzpd_pf_loc *loc, unsigned S, uint32_t want, const struct pzpd_pf_range *r, unsigned n,
                               const unsigned char *buf, pzpd_blob_ref *refs)
{
    static const unsigned char empty[1] = { 0 };
    int complete = 1;
    for (unsigned u = 0; u < S; u++)
    {
        if ( !(want & (1u << u)) || !loc[u].present ) { continue; }
        if (loc[u].size == 0) { refs[u].data = empty; refs[u].size = 0; refs[u].format = loc[u].format; continue; }
        uint64_t bo = 0;
        unsigned k = 0;
        for (; k < n; k++)
        {
            uint64_t alo = r[k].lo & ~(uint64_t)(PZPD_PF_DIO - 1);
            if ( (loc[u].off >= r[k].lo) && (loc[u].off + loc[u].size <= r[k].hi) )
            {
                refs[u].data   = buf + bo + (loc[u].off - alo);
                refs[u].size   = loc[u].size;
                refs[u].format = loc[u].format;
                break;
            }
            bo += ((r[k].hi + PZPD_PF_DIO - 1) & ~(uint64_t)(PZPD_PF_DIO - 1)) - alo;
        }
        if (k == n) { complete = 0; }
    }
    return complete;
}

/** @brief Buffer bytes a record needs when its shard is in BUFFERS mode (pzpd_pf_entry::need). Called without
 *  the prefetcher lock: locating may open and validate a shard, and the index is read-only.
 *  @return The bytes, or 0 for MAP / PAGECACHE shards and records that can't be located (their get reports why). */
static uint64_t pzpd_pf_need(struct pzpd_prefetcher *p, uint64_t ordinal, uint32_t mask)
{
    struct pzpd_pf_loc loc[PZPD_MAX_STREAMS];
    struct pzpd_pf_range r[PZPD_MAX_STREAMS];
    unsigned shard = 0;
    uint64_t local, over;
    if ( (pzpd_pf_locate(p->a, ordinal, mask, loc, &shard, &local) == NULL) || (p->shard_mode[shard] != PZPD_PF_BUFFERS) ) { return 0; }
    return pzpd_pf_buffer_bytes(r, pzpd_pf_ranges(loc, p->a->S, r, &over));
}

/** @brief Take a lane's next queued entry, reserving its window place and budget bytes (shared by the lanes).
 *  Lane lock held. @param starved Set to 1 when an entry is queued but the window, budget or buffer slots are full.
 *  @return Its position, or UINT64_MAX if none may start now. */
static uint64_t pzpd_pf_next(struct pzpd_prefetcher *p, struct pzpd_pf_lane *ln, int *starved)
{
    while ( (ln->cursor < ln->n) && ((ln->e[ln->cursor].io != PZPD_PF_QUEUED) || (ln->e[ln->cursor].claim != PZPD_PF_FREE)) ) { ln->cursor++; }
    if (ln->cursor >= ln->n) { return UINT64_MAX; }
    // Global order: no lane runs more than `window` claims ahead of the schedule's progress
    if (ln->e[ln->cursor].seq >= pzpd_atomic_get(&p->done) + p->window) { *starved = 1; return UINT64_MAX; }
    uint64_t need = ln->e[ln->cursor].need;
    // slots of discarded in-flight entries come back when their read ends
    if ( (need > 0) && (ln->nfree == 0) && !pzpd_pf_grow_bufs(p, ln) ) { *starved = 1; return UINT64_MAX; }
    if (!pzpd_atomic_reserve(&p->outstanding, 1, p->window, 0)) { *starved = 1; return UINT64_MAX; }
    if (need > 0)
    {
        if (!pzpd_atomic_reserve(&p->used, need, p->budget, 1)) { pzpd_atomic_sub(&p->outstanding, 1); *starved = 1; return UINT64_MAX; }   // one record always fits
        pzpd_atomic_max(&p->used_peak, pzpd_atomic_get(&p->used));
    }
    return ln->cursor++;
}

/** @brief Start entry i (lane lock held; window and budget already reserved): its buffer slot for a BUFFERS shard, in-flight state. */
static void pzpd_pf_start(struct pzpd_pf_lane *ln, uint64_t i)
{
    uint32_t slot = PZPD_PF_NONE;
    if (ln->e[i].need > 0)
    {
        slot = ln->free_bufs[--ln->nfree];
        ln->bufs[slot].data  = NULL;
        ln->bufs[slot].bytes = ln->e[i].need;
        ln->bufs[slot].mask  = ln->e[i].mask;
    }
    ln->e[i].slot = slot;
    ln->e[i].io = PZPD_PF_INFLIGHT;
    ln->inflight++;
}

/** @brief Prefetch one batch of a lane: up to PZPD_PF_BATCH MAP / PAGECACHE entries, or one BUFFERS entry (its read
 *  takes long), taken under one lock hold, prefetched without the lock, completed under one lock hold.
 *  @param starved Set to 1 when the lane has queued entries that can't start yet. @return Entries prefetched. */
static unsigned pzpd_pf_lane_batch(struct pzpd_prefetcher *p, struct pzpd_pf_lane *ln, int *starved)
{
    struct { uint64_t i, ordinal, bytes, over; uint32_t mask, slot; int isBuf; unsigned char *data; double dt; } job[PZPD_PF_BATCH];
    unsigned nt = 0;
    pthread_mutex_lock(&ln->lock);
    uint64_t i;
    while ( (nt < PZPD_PF_BATCH) && ((i = pzpd_pf_next(p, ln, starved)) != UINT64_MAX) )
    {
        pzpd_pf_start(ln, i);
        job[nt].i = i; job[nt].ordinal = ln->e[i].ordinal; job[nt].mask = ln->e[i].mask; job[nt].slot = ln->e[i].slot;
        job[nt].isBuf = (ln->e[i].need > 0); job[nt].data = NULL; job[nt].bytes = 0; job[nt].over = 0;
        nt++;
        if (job[nt - 1].isBuf) { break; }
    }
    pthread_mutex_unlock(&ln->lock);
    if (nt == 0) { return 0; }

    for (unsigned k = 0; k < nt; k++)
    {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        struct pzpd_pf_loc loc[PZPD_MAX_STREAMS];
        struct pzpd_pf_range r[PZPD_MAX_STREAMS];
        unsigned shard = 0, nr = 0;
        uint64_t local = 0;
        struct pzpd_rshard *s = pzpd_pf_locate(p->a, job[k].ordinal, job[k].mask, loc, &shard, &local);
        if (s != NULL)                                            // on error the get reports it
        {
            nr = pzpd_pf_ranges(loc, p->a->S, r, &job[k].over);
            if (job[k].isBuf) { job[k].data = pzpd_pf_read(p, shard, s, r, nr); }  // NULL on error: the get reads again and reports it
            else
            {
                if (p->shard_mode[shard] == PZPD_PF_PAGECACHE)
                {
                    // Start every range's reads at once (queue depth), then wait for them while pre-faulting
                    uintptr_t page = (uintptr_t) sysconf(_SC_PAGESIZE);
                    for (unsigned q = 0; q < nr; q++)
                    {
                        uintptr_t lo = (uintptr_t)(s->map + r[q].lo) & ~(page - 1);
                        (void) madvise((void *) lo, (uintptr_t)(s->map + r[q].hi) - lo, MADV_WILLNEED);
                    }
                }
                for (unsigned q = 0; q < nr; q++) { pzpd_populate(s->map + r[q].lo, (size_t)(r[q].hi - r[q].lo)); }
            }
            for (unsigned q = 0; q < nr; q++) { job[k].bytes += r[q].hi - r[q].lo; }
        }
        job[k].dt = pzpd_seconds_since(&t0);
    }

    int freed = 0;
    pthread_mutex_lock(&ln->lock);
    for (unsigned k = 0; k < nt; k++)
    {
        uint64_t e = job[k].i;
        if (job[k].slot != PZPD_PF_NONE) { ln->bufs[job[k].slot].data = job[k].data; }
        ln->e[e].io = PZPD_PF_READY;
        if ( (ln->e[e].claim == PZPD_PF_DONE) && (pzpd_pf_drop_slot(p, ln, e) > 0) ) { freed |= 2; }   // discarded meanwhile
        ln->inflight--;
        ln->st.prefetched++;
        ln->st.bytes_prefetched += job[k].bytes;
        ln->st.bytes_over_read  += job[k].over;
        ln->st.io_seconds       += job[k].dt;
    }
    pthread_cond_broadcast(&ln->ready);
    pthread_mutex_unlock(&ln->lock);
    if (freed) { pzpd_pf_wake(p, freed, 0); }
    return nt;
}

/** @brief One pass of an I/O thread over its lanes (lane l is served by thread l mod T when there are T < lanes
 *  threads, else by the threads t with t mod lanes = l): a batch from each. @return Entries prefetched. */
static unsigned pzpd_pf_scan(struct pzpd_prefetcher *p, const struct pzpd_pf_worker *w, int *starved)
{
    unsigned T = p->nworkers, took = 0;
    if (T >= PZPD_PF_LANES) { return pzpd_pf_lane_batch(p, &p->lane[w->id % PZPD_PF_LANES], starved); }
    for (unsigned l = w->id; l < PZPD_PF_LANES; l += T) { took += pzpd_pf_lane_batch(p, &p->lane[l], starved); }
    return took;
}

/** @brief I/O thread: prefetch batches from its lanes until none can start, then park. Before sleeping it announces
 *  why (queued entries blocked by window / budget, or none) and looks once more, so a waker either sees the
 *  announcement or the thread sees the waker's change: no wake-up is lost. */
static void *pzpd_pf_thread(void *arg)
{
    struct pzpd_pf_worker *w = (struct pzpd_pf_worker *) arg;
    struct pzpd_prefetcher *p = w->p;
    while (!pzpd_atomic_geti(&p->stop))
    {
        int starved = 0;
        if (pzpd_pf_scan(p, w, &starved) > 0) { continue; }
        pzpd_atomic_seti(&w->wake, 0);
        int took = 0;
        for (;;)
        {
            int why = starved ? PZPD_PF_PARKED_STARVED : PZPD_PF_PARKED_EMPTY;
            pzpd_atomic_seti(&w->parked, why);
            starved = 0;
            if ( (took = (pzpd_pf_scan(p, w, &starved) > 0)) ) { break; }
            if ( (starved ? PZPD_PF_PARKED_STARVED : PZPD_PF_PARKED_EMPTY) == why ) { break; }   // announced the right reason
        }
        if (!took)
        {
            if (starved) { pzpd_atomic_add(&p->stalls, 1); }
            pthread_mutex_lock(&w->m);
            while ( !pzpd_atomic_geti(&w->wake) && !pzpd_atomic_geti(&p->stop) ) { pthread_cond_wait(&w->c, &w->m); }
            pthread_mutex_unlock(&w->m);
        }
        pzpd_atomic_seti(&w->parked, PZPD_PF_AWAKE);
    }
    return NULL;
}

/** @brief Lowest limit in `file` of a cgroup directory (base + rel) and of each of its parents up to base.
 *  @return The lower of that and `limit` ("max", i.e. no limit, and missing files leave `limit` as is). */
static uint64_t pzpd_cgroup_min(const char *base, const char *rel, const char *file, uint64_t limit)
{
    char dir[4096];
    int n = snprintf(dir, sizeof(dir), "%s%s", base, rel);
    if ( (n <= 0) || ((size_t) n >= sizeof(dir)) ) { return limit; }
    size_t bl = strlen(base);
    while ( (strlen(dir) > bl) && (dir[strlen(dir) - 1] == '/') ) { dir[strlen(dir) - 1] = 0; }
    for (;;)
    {
        char path[4200];
        snprintf(path, sizeof(path), "%s/%s", dir, file);
        FILE *f = fopen(path, "r");
        if (f != NULL)
        {
            unsigned long long v;
            if ( (fscanf(f, "%llu", &v) == 1) && (v < limit) ) { limit = v; }
            fclose(f);
        }
        char *slash = strrchr(dir, '/');
        if ( (slash == NULL) || ((size_t)(slash - dir) < bl) ) { break; }
        *slash = 0;
    }
    return limit;
}

/** @brief Memory this process may fill: physical RAM, or less when its cgroup or an ancestor sets a limit
 *  (v2 memory.max, v1 memory.limit_in_bytes), as systemd MemoryMax, Slurm and containers do. */
static uint64_t pzpd_memory_limit(void)
{
    uint64_t limit = (uint64_t) sysconf(_SC_PHYS_PAGES) * (uint64_t) sysconf(_SC_PAGESIZE);
    FILE *f = fopen("/proc/self/cgroup", "r");
    if (f == NULL) { return limit; }
    char line[4096];
    while (fgets(line, sizeof(line), f) != NULL)
    {
        // "0::/path" (v2) or "N:controller,controller:/path" (v1)
        line[strcspn(line, "\n")] = 0;
        char *c1 = strchr(line, ':'), *c2 = (c1 != NULL) ? strchr(c1 + 1, ':') : NULL;
        if (c2 == NULL) { continue; }
        *c2 = 0;
        char *controllers = c1 + 1, *save = NULL;
        const char *rel = c2 + 1;
        if (controllers[0] == 0) { limit = pzpd_cgroup_min("/sys/fs/cgroup", rel, "memory.max", limit); continue; }
        for (char *t = strtok_r(controllers, ",", &save); t != NULL; t = strtok_r(NULL, ",", &save))
        {
            if (strcmp(t, "memory") == 0) { limit = pzpd_cgroup_min("/sys/fs/cgroup/memory", rel, "memory.limit_in_bytes", limit); }
        }
    }
    fclose(f);
    return limit;
}

/** @brief Bytes of a member's shards: from its manifest's shard table (no shard is opened), or its standalone shard. */
static uint64_t pzpd_member_bytes(const struct pzpd_member *mb)
{
    const struct pzpd_archive *ar = mb->arch;
    if (ar == NULL) { return 0; }
    if (ar->standalone) { return ar->shards[0].sb.file_bytes; }   // loaded by arch_open()
    uint64_t bytes = 0;
    for (unsigned k = 0; k < ar->shard_count; k++) { bytes += ar->mshards[k].file_bytes; }
    return bytes;
}

/** @brief pzpd_prefetch_auto_mode() with the memory limit given (pzpd_prefetcher_create() reads it once). */
static int pzpd_auto_mode(pzpd *a, unsigned shard, uint64_t memLimit)
{
    pzpd_shard_info si;
    if (!pzpd_shard_info_get(a, shard, &si)) { return pzpd_errorCode; }
    if (si.storage == PZPD_STORAGE_RAM) { return PZPD_PF_MAP; }
    return (pzpd_member_bytes(&a->m[si.member]) < memLimit / 2) ? PZPD_PF_PAGECACHE : PZPD_PF_BUFFERS;
}

int pzpd_prefetch_auto_mode(pzpd *a, unsigned shard)
{
    return pzpd_auto_mode(a, shard, pzpd_memory_limit());
}

pzpd_prefetcher *pzpd_prefetcher_create(pzpd *a, const pzpd_prefetch_opts *o)
{
    pzpd_clear_error();
    pzpd_prefetch_opts d;
    memset(&d, 0, sizeof(d));
    if (o != NULL) { d = *o; }
    if (a == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL handle"); return NULL; }
    if (d.mode > PZPD_PF_BUFFERS) { pzpd_set_error(PZPD_E_ARG, "unknown prefetch mode %u", d.mode); return NULL; }
    if (d.io_threads > 256) { pzpd_set_error(PZPD_E_ARG, "at most 256 I/O threads"); return NULL; }
    if (d.window > (1u << 24)) { pzpd_set_error(PZPD_E_ARG, "window of %u records is too large", d.window); return NULL; }

    struct pzpd_prefetcher *p = (struct pzpd_prefetcher *) aligned_alloc(64, sizeof(struct pzpd_prefetcher));
    if (p == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return NULL; }
    memset(p, 0, sizeof(*p));
    p->a      = a;
    p->mask   = (d.stream_mask != 0) ? d.stream_mask : 0xFFFFFFFFu;
    p->window = (d.window != 0) ? d.window : 256;
    p->budget = (d.budget_bytes != 0) ? d.budget_bytes : (512ull << 20);
    clock_gettime(CLOCK_MONOTONIC, &p->t0);
    for (unsigned l = 0; l < PZPD_PF_LANES; l++) { pthread_mutex_init(&p->lane[l].lock, NULL); pthread_cond_init(&p->lane[l].ready, NULL); }
    pthread_mutex_init(&p->ctl, NULL);
    unsigned threads = (d.io_threads != 0) ? d.io_threads : 4;
    unsigned shards = a->shard_total ? a->shard_total : 1;
    p->shard_mode = (uint8_t *) malloc(shards);
    p->dfd        = (int *) malloc(shards * sizeof(int));
    p->dfd_open   = (int *) malloc(shards * sizeof(int));
    p->threads    = (pthread_t *) calloc(threads, sizeof(pthread_t));
    p->w          = (struct pzpd_pf_worker *) aligned_alloc(64, threads * sizeof(struct pzpd_pf_worker));
    if (p->w != NULL)
    {
        memset(p->w, 0, threads * sizeof(struct pzpd_pf_worker));
        for (unsigned t = 0; t < threads; t++) { p->w[t].p = p; p->w[t].id = t; pthread_mutex_init(&p->w[t].m, NULL); pthread_cond_init(&p->w[t].c, NULL); }
        p->nworkers = threads;
    }
    if ( (p->shard_mode == NULL) || (p->dfd == NULL) || (p->dfd_open == NULL) || (p->threads == NULL) || (p->w == NULL) )
    {
        if (p->dfd_open != NULL) { for (unsigned sh = 0; sh < shards; sh++) { p->dfd_open[sh] = -1; } }
        pzpd_prefetcher_destroy(p);
        pzpd_set_error(PZPD_E_NOMEM, "out of memory");
        return NULL;
    }

    // Mode per shard; O_DIRECT descriptors for BUFFERS shards
    for (unsigned sh = 0; sh < shards; sh++) { p->dfd[sh] = -1; p->dfd_open[sh] = -1; }
    uint64_t memLimit = (d.mode == PZPD_PF_AUTO) ? pzpd_memory_limit() : 0;
    for (unsigned sh = 0; sh < a->shard_total; sh++)
    {
        int m = (int) d.mode;
        if (m == PZPD_PF_AUTO) { m = pzpd_auto_mode(a, sh, memLimit); if (m < 0) { m = PZPD_PF_PAGECACHE; } }
        p->shard_mode[sh] = (uint8_t) m;
        if (m == PZPD_PF_MAP) { p->st.shards_map++; }
        else if (m == PZPD_PF_PAGECACHE) { p->st.shards_pagecache++; }
        else
        {
            p->st.shards_buffers++;
            p->any_buffers = 1;
            pzpd_shard_info si;
            if (pzpd_shard_info_get(a, sh, &si) && si.available)
            {
                p->dfd_open[sh] = open(si.path, O_RDONLY | O_DIRECT | O_CLOEXEC);
                p->dfd[sh] = p->dfd_open[sh];
            }
            if (p->dfd[sh] < 0) { p->fallbacks++; }
        }
    }
    pzpd_clear_error();

    for (unsigned t = 0; t < threads; t++)
    {
        int err = pthread_create(&p->threads[t], NULL, pzpd_pf_thread, &p->w[t]);
        if (err != 0) { pzpd_prefetcher_destroy(p); pzpd_set_error(PZPD_E_IO, "cannot start an I/O thread: %s", strerror(err)); return NULL; }
        p->nthreads++;
    }
    return p;
}

int pzpd_prefetch_submit(pzpd_prefetcher *p, const uint64_t *ordinals, const uint32_t *masks, size_t n)
{
    pzpd_clear_error();
    if ( (p == NULL) || ((ordinals == NULL) && (n > 0)) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    uint64_t total = pzpd_count(p->a);
    for (size_t k = 0; k < n; k++)
    {
        if (ordinals[k] >= total) { pzpd_set_error(PZPD_E_ARG, "ordinal %llu out of range (%llu records)", (unsigned long long) ordinals[k], (unsigned long long) total); return 0; }
    }
    // BUFFERS sizes are worked out here, before any lock: locating a record may open a shard. The claims are
    // sorted by lane (stable: each lane keeps the submission order), so each lane is locked once
    uint64_t *need = NULL;
    uint32_t *order = (n > 0) ? (uint32_t *) malloc(n * sizeof(uint32_t)) : NULL;
    if ( (n > 0) && (order == NULL) ) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    if (p->any_buffers && (n > 0))
    {
        need = (uint64_t *) malloc(n * sizeof(uint64_t));
        if (need == NULL) { free(order); pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
        for (size_t k = 0; k < n; k++) { need[k] = pzpd_pf_need(p, ordinals[k], (masks != NULL) ? masks[k] : p->mask); }
        pzpd_clear_error();                                        // records that can't be located fail in their get
    }
    uint64_t start[PZPD_PF_LANES + 1] = {0};
    for (size_t k = 0; k < n; k++) { start[pzpd_pf_lane_of(ordinals[k]) + 1]++; }
    for (unsigned l = 0; l < PZPD_PF_LANES; l++) { start[l + 1] += start[l]; }
    uint64_t fill[PZPD_PF_LANES];
    memcpy(fill, start, sizeof(fill));
    for (size_t k = 0; k < n; k++) { order[fill[pzpd_pf_lane_of(ordinals[k])]++] = (uint32_t) k; }
    if (n >= PZPD_PF_NONE) { free(order); free(need); pzpd_set_error(PZPD_E_ARG, "schedule longer than %u entries", PZPD_PF_NONE - 1); return 0; }

    pthread_mutex_lock(&p->ctl);
    // Room in every lane first, so a failed submit adds nothing
    int ok = 1;
    for (unsigned l = 0; ok && (l < PZPD_PF_LANES); l++)
    {
        uint64_t cnt = start[l + 1] - start[l];
        if (cnt == 0) { continue; }
        struct pzpd_pf_lane *ln = &p->lane[l];
        pthread_mutex_lock(&ln->lock);
        if (ln->n + cnt >= PZPD_PF_NONE) { pzpd_set_error(PZPD_E_ARG, "schedule longer than %u entries", PZPD_PF_NONE - 1); ok = 0; }
        if (ok && (ln->n + cnt > ln->cap))
        {
            uint64_t nc = (ln->cap == 0) ? 1024 : ln->cap;
            while (nc < ln->n + cnt) { nc *= 2; }
            struct pzpd_pf_entry *ne = (struct pzpd_pf_entry *) realloc(ln->e, nc * sizeof(struct pzpd_pf_entry));
            if (ne == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
            else { ln->e = ne; ln->cap = nc; }
        }
        if (ok && !pzpd_pf_hash_reserve(ln, cnt)) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
        pthread_mutex_unlock(&ln->lock);
    }
    for (unsigned l = 0; ok && (l < PZPD_PF_LANES); l++)
    {
        if (start[l + 1] == start[l]) { continue; }
        struct pzpd_pf_lane *ln = &p->lane[l];
        pthread_mutex_lock(&ln->lock);
        for (uint64_t q = start[l]; q < start[l + 1]; q++)
        {
            size_t k = order[q];
            uint32_t i = (uint32_t) ln->n++;
            ln->e[i].ordinal = ordinals[k];
            ln->e[i].seq     = p->seq_next + k;
            ln->e[i].mask    = (masks != NULL) ? masks[k] : p->mask;
            ln->e[i].need    = (need != NULL) ? need[k] : 0;
            ln->e[i].next    = PZPD_PF_NONE;
            ln->e[i].slot    = PZPD_PF_NONE;
            ln->e[i].io      = PZPD_PF_QUEUED;
            ln->e[i].claim   = PZPD_PF_FREE;
            struct pzpd_pf_hslot *sl = pzpd_pf_slot_of(ln->slots, ln->hcap, ordinals[k]);
            if (sl->head == PZPD_PF_NONE) { sl->ordinal = ordinals[k]; sl->head = i; sl->tail = i; ln->hcount++; }
            else { ln->e[sl->tail].next = i; sl->tail = i; }
        }
        ln->st.submitted += start[l + 1] - start[l];
        pthread_mutex_unlock(&ln->lock);
    }
    if (ok) { p->seq_next += n; }
    pthread_mutex_unlock(&p->ctl);
    free(need);
    free(order);
    if (ok && (n > 0)) { pzpd_pf_wake(p, 0, 1); }
    return ok;
}

void pzpd_prefetch_clear(pzpd_prefetcher *p)
{
    if (p == NULL) { return; }
    pthread_mutex_lock(&p->ctl);
    for (unsigned l = 0; l < PZPD_PF_LANES; l++)
    {
        struct pzpd_pf_lane *ln = &p->lane[l];
        pthread_mutex_lock(&ln->lock);
        ln->cursor = ln->n;                                        // I/O threads take nothing new from this lane
        while (ln->inflight > 0) { pthread_cond_wait(&ln->ready, &ln->lock); }
        uint64_t started = 0;
        for (uint64_t i = 0; i < ln->n; i++)
        {
            if ( (ln->e[i].io != PZPD_PF_QUEUED) && (ln->e[i].claim != PZPD_PF_DONE) ) { started++; }   // their window places
            pzpd_pf_drop_slot(p, ln, i);                           // prefetched, not yet got (tickets keep theirs)
        }
        pzpd_atomic_sub(&p->outstanding, started);
        ln->n = 0;
        ln->cursor = 0;
        ln->hcount = 0;
        if (ln->slots != NULL) { memset(ln->slots, 0xFF, ln->hcap * sizeof(struct pzpd_pf_hslot)); }
        ln->gen++;
        pthread_cond_broadcast(&ln->ready);                        // gets waiting on the old schedule
        pthread_mutex_unlock(&ln->lock);
    }
    p->seq_next = 0;                                               // every lane is empty: the new schedule starts at 0
    pzpd_atomic_set(&p->done, 0);
    pthread_mutex_unlock(&p->ctl);
    pzpd_pf_wake(p, 3, 1);
}

int pzpd_prefetch_get(pzpd_prefetcher *p, uint64_t ordinal, uint32_t mask, pzpd_blob_ref *refs, pzpd_ticket *t)
{
    pzpd_clear_error();
    if (t != NULL) { t->pos = UINT64_MAX; t->gen = 0; t->buf = NULL; t->bytes = 0; }
    if ( (p == NULL) || (refs == NULL) || (t == NULL) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return PZPD_E_ARG; }
    pzpd *a = p->a;
    memset(refs, 0, sizeof(pzpd_blob_ref) * a->S);

    uint32_t heldMask = 0;
    unsigned l = pzpd_pf_lane_of(ordinal);
    struct pzpd_pf_lane *ln = &p->lane[l];
    pthread_mutex_lock(&ln->lock);
    uint64_t i = pzpd_pf_claim(ln, ordinal);
    if (i == UINT64_MAX) { ln->st.unscheduled++; }
    else
    {
        ln->e[i].claim = PZPD_PF_CLAIMED;
        if (ln->e[i].io == PZPD_PF_READY) { ln->st.hits++; }
        else if (ln->e[i].io == PZPD_PF_QUEUED) { ln->st.sync_misses++; }
        else
        {
            ln->st.waits++;
            uint64_t gen = ln->gen;
            while ( (ln->gen == gen) && (ln->e[i].io == PZPD_PF_INFLIGHT) ) { pthread_cond_wait(&ln->ready, &ln->lock); }
            if (ln->gen != gen) { i = UINT64_MAX; }                // cleared meanwhile: nothing to release
        }
        if (i != UINT64_MAX)
        {
            t->pos = ((uint64_t) l << 32) | i;
            t->gen = ln->gen;
            uint32_t k = ln->e[i].slot;
            if (k != PZPD_PF_NONE)                                 // the buffer moves to the ticket (still counted in `used`)
            {
                t->buf = ln->bufs[k].data;
                t->bytes = ln->bufs[k].bytes;
                heldMask = ln->bufs[k].mask;
                ln->bufs[k].data = NULL;
                ln->bufs[k].bytes = 0;
                ln->free_bufs[ln->nfree++] = k;
                ln->e[i].slot = PZPD_PF_NONE;
            }
        }
    }
    pthread_mutex_unlock(&ln->lock);

    struct pzpd_pf_loc loc[PZPD_MAX_STREAMS];
    unsigned shard = 0;
    uint64_t local = 0;
    struct pzpd_rshard *s = pzpd_pf_locate(a, ordinal, mask, loc, &shard, &local);
    int present = 0;
    if ( (s != NULL) && (p->shard_mode[shard] == PZPD_PF_BUFFERS) )
    {
        //--- BUFFERS: blobs from the prefetched buffer, or a synchronous read into a new one ----------
        struct pzpd_pf_range r[PZPD_MAX_STREAMS];
        uint64_t over;
        int complete = 0;
        if (t->buf != NULL)
        {
            struct pzpd_pf_loc held[PZPD_MAX_STREAMS];
            unsigned hs; uint64_t hl;
            if (pzpd_pf_locate(a, ordinal, heldMask, held, &hs, &hl) != NULL)
            {
                complete = pzpd_pf_buffer_refs(loc, a->S, mask, r, pzpd_pf_ranges(held, a->S, r, &over), (const unsigned char *) t->buf, refs);
            }
        }
        if (!complete)                                             // not prefetched, the read failed, or streams beyond the submitted mask
        {
            memset(refs, 0, sizeof(pzpd_blob_ref) * a->S);
            unsigned nr = pzpd_pf_ranges(loc, a->S, r, &over);
            uint64_t bytes = pzpd_pf_buffer_bytes(r, nr);
            unsigned char *buf = pzpd_pf_read(p, shard, s, r, nr);
            int freed = (t->bytes > 0) ? 2 : 0;
            free(t->buf);
            pzpd_atomic_sub(&p->used, t->bytes);
            t->buf = buf;
            t->bytes = (buf != NULL) ? bytes : 0;
            pzpd_atomic_add(&p->used, t->bytes);
            pzpd_atomic_max(&p->used_peak, pzpd_atomic_get(&p->used));
            if (freed) { pzpd_pf_wake(p, freed, 0); }
            if (buf == NULL) { s = NULL; }
            else { pzpd_pf_buffer_refs(loc, a->S, mask, r, nr, buf, refs); }
        }
    }
    else if (s != NULL)
    {
        //--- MAP / PAGECACHE: views into the shard mapping, where pzpd_pf_locate() found each blob -----
        for (unsigned u = 0; u < a->S; u++)
        {
            if (!loc[u].present) { continue; }
            refs[u].data   = s->map + loc[u].off;
            refs[u].size   = loc[u].size;
            refs[u].format = loc[u].format;
        }
    }
    // Count the blobs; with PZPD_O_VERIFY, check each against the XXH32 in its record header
    for (unsigned u = 0; (s != NULL) && (u < a->S); u++)
    {
        if (refs[u].data == NULL) { continue; }
        present++;
        uint32_t want;
        if ( (a->flags & PZPD_O_VERIFY) && (!pzpd_header_blob_xxh(s, local, (unsigned) loc[u].mstream, &want) || (XXH32(refs[u].data, refs[u].size, 0) != want)) )
        {
            if (pzpd_errorCode == PZPD_OK) { pzpd_set_error(PZPD_E_CHECKSUM, "%s: checksum mismatch in record %llu stream %u", s->path, (unsigned long long) local, (unsigned) loc[u].mstream); }
            s = NULL;
        }
    }
    if (s == NULL)
    {
        if (pzpd_errorCode == PZPD_OK) { pzpd_errorCode = PZPD_E_FORMAT; }
        struct pzpd_saved_error e;
        pzpd_error_save(&e);
        memset(refs, 0, sizeof(pzpd_blob_ref) * a->S);
        pzpd_prefetch_release(p, t);
        pzpd_error_restore(&e);
        return e.code;
    }
    return present;
}

void pzpd_prefetch_release(pzpd_prefetcher *p, pzpd_ticket *t)
{
    if ( (p == NULL) || (t == NULL) ) { return; }
    int freed = 0;
    free(t->buf);
    if (t->bytes > 0) { pzpd_atomic_sub(&p->used, t->bytes); freed |= 2; }
    unsigned l = (unsigned)(t->pos >> 32);
    uint64_t i = t->pos & 0xFFFFFFFFull;
    if ( (t->pos != UINT64_MAX) && (l < PZPD_PF_LANES) )
    {
        struct pzpd_pf_lane *ln = &p->lane[l];
        pthread_mutex_lock(&ln->lock);
        if ( (t->gen == ln->gen) && (i < ln->n) && (ln->e[i].claim == PZPD_PF_CLAIMED) )
        {
            freed |= pzpd_pf_done(p, ln, i);
            ln->st.released++;
        }
        pthread_mutex_unlock(&ln->lock);
    }
    t->pos = UINT64_MAX;
    t->buf = NULL;
    t->bytes = 0;
    if (freed) { pzpd_pf_wake(p, freed, 0); }
}

void pzpd_prefetch_discard(pzpd_prefetcher *p, uint64_t ordinal)
{
    if (p == NULL) { return; }
    struct pzpd_pf_lane *ln = &p->lane[pzpd_pf_lane_of(ordinal)];
    int freed = 0;
    pthread_mutex_lock(&ln->lock);
    uint64_t i = pzpd_pf_claim(ln, ordinal);
    if (i != UINT64_MAX)
    {
        freed = pzpd_pf_done(p, ln, i);
        ln->st.discarded++;
    }
    pthread_mutex_unlock(&ln->lock);
    if (freed) { pzpd_pf_wake(p, freed, 0); }
}

void pzpd_prefetch_stats_get(const pzpd_prefetcher *p, pzpd_prefetch_stats *s)
{
    if (s == NULL) { return; }
    memset(s, 0, sizeof(*s));
    if (p == NULL) { return; }
    struct pzpd_prefetcher *q = (struct pzpd_prefetcher *) p;     // the locks are not part of the logical state
    *s = q->st;
    for (unsigned l = 0; l < PZPD_PF_LANES; l++)
    {
        struct pzpd_pf_lane *ln = &q->lane[l];
        pthread_mutex_lock(&ln->lock);
        s->submitted        += ln->st.submitted;
        s->prefetched       += ln->st.prefetched;
        s->hits             += ln->st.hits;
        s->waits            += ln->st.waits;
        s->sync_misses      += ln->st.sync_misses;
        s->unscheduled      += ln->st.unscheduled;
        s->discarded        += ln->st.discarded;
        s->released         += ln->st.released;
        s->bytes_prefetched += ln->st.bytes_prefetched;
        s->bytes_over_read  += ln->st.bytes_over_read;
        s->io_seconds       += ln->st.io_seconds;
        pthread_mutex_unlock(&ln->lock);
    }
    s->producer_stalls   = pzpd_atomic_get(&q->stalls);
    s->direct_fallbacks  = (unsigned) pzpd_atomic_get(&q->fallbacks);
    s->buffer_bytes      = pzpd_atomic_get(&q->used);
    s->buffer_bytes_peak = pzpd_atomic_get(&q->used_peak);
    s->elapsed_seconds   = pzpd_seconds_since(&p->t0);
}

void pzpd_prefetcher_destroy(pzpd_prefetcher *p)
{
    if (p == NULL) { return; }
    pzpd_atomic_seti(&p->stop, 1);
    for (unsigned t = 0; t < p->nthreads; t++) { pzpd_pf_wake_worker(&p->w[t]); }
    for (unsigned t = 0; t < p->nthreads; t++) { pthread_join(p->threads[t], NULL); }
    for (unsigned t = 0; (p->w != NULL) && (t < p->nworkers); t++) { pthread_mutex_destroy(&p->w[t].m); pthread_cond_destroy(&p->w[t].c); }
    for (unsigned l = 0; l < PZPD_PF_LANES; l++)
    {
        struct pzpd_pf_lane *ln = &p->lane[l];
        for (unsigned k = 0; k < ln->nbufs; k++) { free(ln->bufs[k].data); }
        free(ln->bufs);
        free(ln->free_bufs);
        free(ln->slots);
        free(ln->e);
        pthread_cond_destroy(&ln->ready);
        pthread_mutex_destroy(&ln->lock);
    }
    for (unsigned sh = 0; (p->dfd_open != NULL) && (sh < (p->a->shard_total ? p->a->shard_total : 1)); sh++) { if (p->dfd_open[sh] >= 0) { close(p->dfd_open[sh]); } }
    pthread_mutex_destroy(&p->ctl);
    free(p->w);
    free(p->threads);
    free(p->shard_mode);
    free(p->dfd);
    free(p->dfd_open);
    free(p);
}

//-----------------------------------------------------------------------------------------------
// Recovery (spec §4.1 / §4.7): rebuild a manifest from its shards, salvage a shard without its index
//-----------------------------------------------------------------------------------------------

/** @brief Directory part of a path (malloc'd, "." when there is none), resolved with realpath() when possible. */
static char *pzpd_dir_of(const char *path)
{
    const char *sl = strrchr(path, '/');
    char *d = (sl == NULL) ? strdup(".") : strndup(path, (sl == path) ? 1 : (size_t)(sl - path));
    if (d == NULL) { return NULL; }
    char *r = realpath(d, NULL);
    if (r != NULL) { free(d); return r; }
    return d;
}

int pzpd_manifest_rebuild(const char *manifest_path, const char *const *shard_paths, unsigned n)
{
    pzpd_clear_error();
    if ( (manifest_path == NULL) || (shard_paths == NULL) || (n == 0) || (n > 1000000) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    struct pzpd_archive **ar = (struct pzpd_archive **) calloc(n, sizeof(struct pzpd_archive *));
    struct pzpd_archive **byIndex = (struct pzpd_archive **) calloc(n, sizeof(struct pzpd_archive *));
    char *mdir = pzpd_dir_of(manifest_path);
    int ok = (ar != NULL) && (byIndex != NULL) && (mdir != NULL);
    if (!ok) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); }

    // Open every shard on its own (the ladder applies: backup superblock, section scan) and order them
    for (unsigned i = 0; ok && (i < n); i++)
    {
        ar[i] = arch_open(shard_paths[i], 0);
        if (ar[i] == NULL) { pzpd_error_wrap(PZPD_OK, "%s", shard_paths[i]); ok = 0; break; }
        if (!ar[i]->standalone) { pzpd_set_error(PZPD_E_ARG, "%s is not a shard", shard_paths[i]); ok = 0; break; }
        const struct pzpd_disk_superblock *sb = &ar[i]->shards[0].sb;
        if (sb->shard_index >= n) { pzpd_set_error(PZPD_E_ARG, "%s is shard %u, but only %u shards were given", shard_paths[i], sb->shard_index, n); ok = 0; break; }
        if ( (sb->shard_count != 0) && (sb->shard_count != n) ) { pzpd_set_error(PZPD_E_ARG, "%s belongs to an archive of %u shards, %u were given", shard_paths[i], sb->shard_count, n); ok = 0; break; }
        if (byIndex[sb->shard_index] != NULL) { pzpd_set_error(PZPD_E_ARG, "shard %u was given twice", sb->shard_index); ok = 0; break; }
        byIndex[sb->shard_index] = ar[i];
        char *sdir = pzpd_dir_of(shard_paths[i]);
        if ( (sdir == NULL) || (strcmp(sdir, mdir) != 0) ) { pzpd_set_error(PZPD_E_ARG, "%s is not in the manifest's directory", shard_paths[i]); ok = 0; }
        free(sdir);
    }

    // Consistency: one archive, contiguous ordinals, same streams and tables everywhere
    struct pzpd_buf shardTab = {0}, names = {0}, ghash = {0};
    uint64_t total = 0;
    const struct pzpd_archive *a0 = ok ? byIndex[0] : NULL;
    for (unsigned k = 0; ok && (k < n); k++)
    {
        const struct pzpd_archive *a = byIndex[k];
        const struct pzpd_rshard *s = &a->shards[0];
        const struct pzpd_disk_superblock *sb = &s->sb;
        if (memcmp(sb->archive_uuid, a0->shards[0].sb.archive_uuid, 16) != 0) { pzpd_set_error(PZPD_E_ARG, "%s belongs to another archive", s->path); ok = 0; break; }
        if (sb->first_ordinal != total) { pzpd_set_error(PZPD_E_FORMAT, "%s starts at ordinal %llu, expected %llu", s->path, (unsigned long long) sb->first_ordinal, (unsigned long long) total); ok = 0; break; }
        if ( (a->S != a0->S) || (a->T != a0->T) ) { pzpd_set_error(PZPD_E_FORMAT, "%s has other streams or tables than shard 0", s->path); ok = 0; break; }
        for (unsigned u = 0; ok && (u < a->S); u++) { if (strcmp(a->streams[u], a0->streams[u]) != 0) { pzpd_set_error(PZPD_E_FORMAT, "%s: stream %u differs from shard 0", s->path, u); ok = 0; } }
        for (unsigned t = 0; ok && (t < a->T); t++) { if (!pzpd_schema_equal(&a->tables[t], &a0->tables[t])) { pzpd_set_error(PZPD_E_FORMAT, "%s: table %s differs from shard 0", s->path, a->tables[t].name); ok = 0; } }
        if (!ok) { break; }

        const char *nm = strrchr(s->path, '/');
        nm = (nm == NULL) ? s->path : nm + 1;
        struct pzpd_disk_manifest_shard ms;
        memset(&ms, 0, sizeof(ms));
        ms.first_ordinal  = sb->first_ordinal;
        ms.record_count   = sb->record_count;
        ms.file_bytes     = sb->file_bytes;
        ms.generation     = sb->generation;
        ms.index_checksum = sb->index_checksum;
        ms.name_offset    = (uint32_t) names.len;
        ms.name_len       = (uint32_t) strlen(nm);
        ok = pzpd_buf_append(&shardTab, &ms, sizeof(ms)) && pzpd_buf_append(&names, nm, strlen(nm));
        for (uint64_t h = 0; ok && (h < sb->hash_count); h++)
        {
            struct pzpd_disk_hash he;
            memcpy(&he, &s->hash[h], sizeof(he));
            struct pzpd_disk_global_hash ge;
            memset(&ge, 0, sizeof(ge));
            ge.hash = he.hash; ge.ordinal = sb->first_ordinal + he.local_ordinal; ge.stream = he.stream; ge.kind = he.kind;
            ok = pzpd_buf_append(&ghash, &ge, sizeof(ge));
        }
        total += sb->record_count;
    }
    if (ok)
    {
        struct pzpd_mtable tabs[PZPD_MAX_TABLES];
        for (unsigned t = 0; t < a0->T; t++)
        {
            int global = (a0->tables[t].flags & PZPD_TABLE_GLOBAL) != 0;
            tabs[t].sc       = &a0->tables[t];
            tabs[t].rows     = global ? a0->gview[t].rowdata : NULL;
            tabs[t].nrows    = global ? a0->gview[t].rows : 0;
            tabs[t].heap     = global ? a0->gview[t].heap : NULL;
            tabs[t].heap_len = global ? a0->gview[t].heap_bytes : 0;
        }
        char streams[PZPD_MAX_STREAMS][24];
        memcpy(streams, a0->streams, sizeof(streams));
        ok = pzpd_write_manifest(manifest_path, a0->shards[0].sb.archive_uuid, total, a0->S, streams, n, &shardTab, &names, &ghash, a0->T, tabs,
                                 byIndex);
    }
    struct pzpd_saved_error e;
    pzpd_error_save(&e);
    pzpd_buf_free(&shardTab);
    pzpd_buf_free(&names);
    pzpd_buf_free(&ghash);
    for (unsigned i = 0; (ar != NULL) && (i < n); i++) { if (ar[i] != NULL) { arch_close(ar[i]); } }
    free(ar);
    free(byIndex);
    free(mdir);
    if (!ok) { pzpd_error_restore(&e); }
    return ok;
}

/** @brief Stream names of a shard whose index may be gone: a valid superblock (primary or backup), else the
 *  metadata JSON found by the section scan. @return Count (0 if none). */
static unsigned pzpd_salvage_names(const unsigned char *map, uint64_t file, char names[][24], int *from)
{
    struct pzpd_disk_superblock sb;
    for (int k = 0; k < 2; k++)
    {
        memcpy(&sb, map + ((k == 0) ? 0 : file - PZPD_BLOCK), sizeof(sb));
        if (pzpd_superblock_valid(&sb, file))
        {
            for (unsigned u = 0; u < sb.stream_count; u++) { pzpd_get_slot_name(names[u], sb.streams[u].name); }
            *from = 1;
            return sb.stream_count;
        }
    }
    if (pzpd_sb_from_sections(map, file, 0, &sb))
    {
        for (unsigned u = 0; u < sb.stream_count; u++) { pzpd_get_slot_name(names[u], sb.streams[u].name); }
        *from = 2;
        return sb.stream_count;
    }
    pzpd_clear_error();
    return 0;
}

int pzpd_salvage(const char *shard_path, pzpd *schemas, pzpd_salvage_fn fn, void *user, pzpd_salvage_info *info)
{
    pzpd_clear_error();
    pzpd_salvage_info mine;
    if (info == NULL) { info = &mine; }
    memset(info, 0, sizeof(*info));
    if (shard_path == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL path"); return 0; }
    int fd = open(shard_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { pzpd_set_error(PZPD_E_IO, "cannot open %s: %s", shard_path, strerror(errno)); return 0; }
    struct stat st;
    if ( (fstat(fd, &st) != 0) || (st.st_size < 2 * PZPD_BLOCK) ) { close(fd); pzpd_set_error(PZPD_E_FORMAT, "%s is too small to be a shard", shard_path); return 0; }
    uint64_t file = (uint64_t) st.st_size;
    const unsigned char *map = (const unsigned char *) mmap(NULL, (size_t) file, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) { pzpd_set_error(PZPD_E_IO, "cannot map %s: %s", shard_path, strerror(errno)); return 0; }
    (void) madvise((void *) map, (size_t) file, MADV_SEQUENTIAL);

    // Stream names and table schemas
    if (schemas != NULL)
    {
        info->stream_count = schemas->S;
        memcpy(info->streams, schemas->streams, sizeof(info->streams));
        info->names_from = 3;
        info->schemas_from = (schemas->T > 0) ? 2 : 0;
    }
    else { info->stream_count = pzpd_salvage_names(map, file, info->streams, &info->names_from); }

    pzpd_salvaged_blob blobs[256];
    pzpd_salvaged_rows rows[PZPD_MAX_TABLES];
    struct pzpd_buf csv[PZPD_MAX_TABLES];
    memset(csv, 0, sizeof(csv));
    int keepGoing = 1, nomem = 0;
    unsigned char *hbuf = NULL;
    uint32_t hcap = 0;
    uint64_t off = PZPD_BLOCK;
    while (keepGoing && (off + sizeof(struct pzpd_disk_record_header) <= file))
    {
        if (memcmp(map + off, PZPD_MAGIC_RECORD, 8) != 0) { off += 64; continue; }
        struct pzpd_disk_record_header rh;
        memcpy(&rh, map + off, sizeof(rh));
        uint64_t descEnd = sizeof(rh) + (uint64_t) rh.blob_count * sizeof(struct pzpd_disk_record_blob);
        int ok = (rh.header_bytes >= descEnd + rh.key_len) && (rh.header_bytes <= rh.record_bytes) && (rh.record_bytes >= 64) &&
                 (rh.record_bytes % 64 == 0) && (off + rh.record_bytes <= file) && (rh.key_len > 0);
        if (ok)
        {
            // XXH32 of the header bytes with the checksum field zeroed (hashed from a copy)
            if (rh.header_bytes > hcap)
            {
                unsigned char *nb2 = (unsigned char *) realloc(hbuf, rh.header_bytes);
                if (nb2 == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); nomem = 1; break; }
                hbuf = nb2;
                hcap = rh.header_bytes;
            }
            memcpy(hbuf, map + off, rh.header_bytes);
            memset(hbuf + offsetof(struct pzpd_disk_record_header, checksum), 0, sizeof(rh.checksum));
            ok = (XXH32(hbuf, rh.header_bytes, 0) == rh.checksum);
        }
        if (!ok) { info->damaged_headers++; off += 64; continue; }

        // Blobs: descriptors, then the key, then the names
        const unsigned char *rec = map + off;
        uint64_t namePos = descEnd + rh.key_len;
        unsigned nb = 0;
        for (unsigned i = 0; ok && (i < rh.blob_count); i++)
        {
            struct pzpd_disk_record_blob d;
            memcpy(&d, rec + sizeof(rh) + (uint64_t) i * sizeof(d), sizeof(d));
            if ( (namePos + d.name_len > rh.header_bytes) || ((uint64_t) d.rel_offset + d.size > rh.record_bytes) || (d.rel_offset < rh.header_bytes) ) { ok = 0; break; }
            pzpd_salvaged_blob *b = &blobs[nb++];
            memset(b, 0, sizeof(*b));
            b->stream      = d.stream;
            b->stream_name = (d.stream < info->stream_count) ? info->streams[d.stream] : NULL;
            b->name        = (const char *)(rec + namePos);
            b->name_len    = d.name_len;
            b->data        = rec + d.rel_offset;
            b->size        = d.size;
            b->meta.format = d.format; b->meta.width = d.width; b->meta.height = d.height; b->meta.channels = d.channels;
            b->meta.frames = d.frames; b->meta.bits = d.bits; b->meta.meta_flags = d.meta_flags;
            b->intact      = (XXH32(b->data, b->size, 0) == d.xxh32);
            namePos += d.name_len;
        }
        // Copied table rows, 8-aligned after the names
        uint64_t cp = (namePos + 7) & ~(uint64_t) 7, cend = cp + rh.table_bytes;
        unsigned nt = 0;
        if (ok && (rh.table_bytes > 0) && (cend > rh.header_bytes)) { ok = 0; }
        while (ok && (rh.table_bytes > 0) && (cp + sizeof(struct pzpd_disk_row_copy) <= cend) && (nt < PZPD_MAX_TABLES))
        {
            struct pzpd_disk_row_copy rc;
            memcpy(&rc, rec + cp, sizeof(rc));
            uint64_t blk = sizeof(rc) + (uint64_t) rc.rows_bytes + rc.str_bytes;
            if (cp + blk > cend) { ok = 0; break; }
            pzpd_salvaged_rows *r = &rows[nt];
            memset(r, 0, sizeof(*r));
            r->table       = rc.table;
            r->rows        = rc.rows;
            r->row_data    = rec + cp + sizeof(rc);
            r->row_bytes   = rc.rows_bytes;
            r->strings     = (const char *)(rec + cp + sizeof(rc) + rc.rows_bytes);
            r->strings_len = rc.str_bytes;
            const struct pzpd_tschema *sc = ( (schemas != NULL) && (rc.table < schemas->T) ) ? schemas->tables[rc.table] : NULL;
            if ( (sc != NULL) && ((uint64_t) rc.rows * sc->stride == rc.rows_bytes) )
            {
                r->table_name = sc->name;
                csv[nt].len = 0;
                int rok = 1;
                for (uint32_t k = 0; rok && (k < rc.rows); k++)
                {
                    rok = pzpd_csv_render(sc, (const unsigned char *) r->row_data + (size_t) k * sc->stride, r->strings, r->strings_len, &csv[nt]);   // ends the line itself
                }
                if (rok && pzpd_buf_append(&csv[nt], "", 1)) { r->csv = (const char *) csv[nt].data; r->csv_len = csv[nt].len - 1; }
            }
            nt++;
            cp += (blk + 7) & ~(uint64_t) 7;
        }
        if (!ok) { info->damaged_headers++; off += 64; continue; }

        for (unsigned i = 0; i < nb; i++) { if (blobs[i].intact) { info->blobs++; } else { info->damaged_blobs++; } }
        info->records++;
        pzpd_salvaged_record R;
        memset(&R, 0, sizeof(R));
        R.file_offset = off;
        R.key         = (const char *)(rec + descEnd);
        R.key_len     = rh.key_len;
        R.group       = rh.group;
        R.frame       = rh.frame;
        R.blob_count  = nb;
        R.blobs       = blobs;
        R.table_count = nt;
        R.tables      = rows;
        if ( (fn != NULL) && !fn(&R, user) ) { keepGoing = 0; }
        off += rh.record_bytes;                                  // skip the payloads: never scanned for magics
    }
    for (unsigned t = 0; t < PZPD_MAX_TABLES; t++) { pzpd_buf_free(&csv[t]); }
    free(hbuf);
    munmap((void *) map, (size_t) file);
    return !nomem;                                               // a callback that stopped the scan is not an error
}

//-----------------------------------------------------------------------------------------------
// Edits (spec §7): table edits and compaction append a new superblock generation to each shard;
// stream edits rewrite shards one at a time. Both finish by rewriting the manifest.
//-----------------------------------------------------------------------------------------------

/** @brief Test hook for the crash tests (tests/run_cli_tests.sh): with PZPDIR_TEST_CRASH=<point>:<n> in the
 *  environment, the n-th time this thread reaches <point> the process exits at once (as if killed). */
static void pzpd_test_crash(const char *point)
{
    static __thread int seen = 0;
    const char *e = getenv("PZPDIR_TEST_CRASH");
    size_t pl = strlen(point);
    if ( (e == NULL) || (strncmp(e, point, pl) != 0) || (e[pl] != ':') ) { return; }
    if (++seen == atoi(e + pl + 1)) { _exit(99); }
}

static int pzpd_write_sb_block(int fd, const struct pzpd_disk_superblock *sb, uint64_t off);   // defined below

/** @brief Finish an interrupted switch: when the shard was loaded from a newer backup superblock than
 *  its primary (an edit stopped between writing the backup and flipping), make the primary match. */
static int pzpd_finish_flip(struct pzpd_rshard *s)
{
    struct pzpd_disk_superblock p;
    memcpy(&p, s->map, sizeof(p));
    if ( (s->recovery != 1) || (memcmp(&p, &s->sb, sizeof(p)) == 0) ) { return 1; }
    int fd = open(s->path, O_RDWR | O_CLOEXEC);
    if (fd < 0) { pzpd_set_error(PZPD_E_IO, "cannot open %s for writing: %s", s->path, strerror(errno)); return 0; }
    int ok = pzpd_write_sb_block(fd, &s->sb, 0) && (fsync(fd) == 0);
    close(fd);
    return ok;
}

/** @brief One table section of a shard's new layout (pzpd_write_generation()). */
struct pzpd_tslot
{
    struct pzpd_disk_table slot;  ///< Directory slot (section_offset / _bytes filled when written)
    const unsigned char   *data;  ///< Section data to write, or NULL to keep the existing section at slot.section_offset
    uint64_t               bytes; ///< Its size
};

/** @brief One word index section of a shard's new layout (pzpd_write_generation()). */
struct pzpd_wslot
{
    struct pzpd_disk_words slot;  ///< Directory slot (section_offset / _bytes filled when written)
    const unsigned char   *data;  ///< Section data to write, or NULL to keep the existing section at slot.section_offset
    uint64_t               bytes; ///< Its size
};

/** @brief Write one full superblock block (4 KiB) at off. */
static int pzpd_write_sb_block(int fd, const struct pzpd_disk_superblock *sb, uint64_t off)
{
    unsigned char block[PZPD_BLOCK];
    memset(block, 0, sizeof(block));
    memcpy(block, sb, sizeof(*sb));
    return pzpd_pwrite_all(fd, block, PZPD_BLOCK, off);
}

/** @brief Give a shard a new generation with a new table directory, crash-safely:
 *  1. write the new table sections (those with data) from `at` on, then the new backup superblock after them;
 *  2. fsync; 3. rewrite the primary superblock; 4. fsync. `truncate_after_flip` = 0 cuts the file right after
 *  the new backup before the flip (the old generation never reaches past `at`); 1 cuts it only after the flip
 *  (compact, whose old generation lies behind the new one). The reader prefers the higher valid generation of
 *  the primary and the backup at EOF, so a crash anywhere leaves the old or the new generation, never a mix.
 *  @param map Mapping of the shard (for the unchanged index sections and kept table sections).
 *  @return 1 on success (new superblock in *out), 0 on failure (error set). */
static int pzpd_write_generation(int fd, const unsigned char *map, const struct pzpd_disk_superblock *base, uint64_t at,
                                 unsigned T, struct pzpd_tslot *ts, unsigned W, const struct pzpd_wslot *ws, int truncate_after_flip,
                                 struct pzpd_disk_superblock *out)
{
    struct pzpd_disk_superblock sb = *base;
    memset(sb.tables, 0, sizeof(sb.tables));
    memset(sb.words, 0, sizeof(sb.words));
    XXH64_state_t *idx = XXH64_createState();
    if (idx == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    XXH64_reset(idx, 0);
    uint64_t n = sb.record_count, S = sb.stream_count;
    XXH64_update(idx, map + sb.rtab_offset, (size_t)(n * sizeof(struct pzpd_disk_record)));
    XXH64_update(idx, map + sb.btab_offset, (size_t)(n * S * sizeof(struct pzpd_disk_blob)));
    XXH64_update(idx, map + sb.hash_offset, (size_t)(sb.hash_count * sizeof(struct pzpd_disk_hash)));
    XXH64_update(idx, map + sb.heap_offset, (size_t) sb.heap_bytes);
    XXH64_update(idx, map + sb.meta_offset, (size_t) sb.meta_bytes);
    if (sb.group_count > 0) { XXH64_update(idx, map + sb.groups_offset, (size_t)(sb.group_count * sizeof(struct pzpd_disk_group))); }
    uint64_t off = at;
    int ok = 1;
    for (unsigned t = 0; ok && (t < T); t++)
    {
        sb.tables[t] = ts[t].slot;
        if (ts[t].data != NULL)
        {
            uint64_t dataOff = 0;
            ok = pzpd_write_section(fd, &off, PZPD_SECT_TABLE, ts[t].data, ts[t].bytes, &dataOff, idx);
            sb.tables[t].section_offset = dataOff;
            sb.tables[t].section_bytes  = ts[t].bytes;
        }
        else { XXH64_update(idx, map + ts[t].slot.section_offset, (size_t) ts[t].slot.section_bytes); }
    }
    sb.index_checksum = XXH64_digest(idx);
    // Word indexes after the tables, with their own checksum
    XXH64_reset(idx, 0);
    for (unsigned j = 0; ok && (j < W); j++)
    {
        sb.words[j] = ws[j].slot;
        if (ws[j].data != NULL)
        {
            uint64_t dataOff = 0;
            ok = pzpd_write_section(fd, &off, PZPD_SECT_WORDS, ws[j].data, ws[j].bytes, &dataOff, idx);
            sb.words[j].section_offset = dataOff;
            sb.words[j].section_bytes  = ws[j].bytes;
        }
        else { XXH64_update(idx, map + ws[j].slot.section_offset, (size_t) ws[j].slot.section_bytes); }
    }
    sb.words_checksum = (W > 0) ? XXH64_digest(idx) : 0;
    XXH64_freeState(idx);
    if (!ok) { return 0; }
    sb.generation = base->generation + 1;
    sb.file_bytes = pzpd_align_up(off, PZPD_BLOCK) + PZPD_BLOCK;
    pzpd_seal_superblock(&sb);
    ok = pzpd_write_sb_block(fd, &sb, sb.file_bytes - PZPD_BLOCK);
    if (ok && !truncate_after_flip && (ftruncate(fd, (off_t) sb.file_bytes) != 0)) { pzpd_set_error(PZPD_E_IO, "ftruncate: %s", strerror(errno)); ok = 0; }
    if (ok && (fsync(fd) != 0)) { pzpd_set_error(PZPD_E_IO, "fsync: %s", strerror(errno)); ok = 0; }
    if (ok) { pzpd_test_crash("flip"); }                       // between the appended generation and the flip
    ok = ok && pzpd_write_sb_block(fd, &sb, 0);
    if (ok && (fsync(fd) != 0)) { pzpd_set_error(PZPD_E_IO, "fsync: %s", strerror(errno)); ok = 0; }
    if (ok && truncate_after_flip && ((ftruncate(fd, (off_t) sb.file_bytes) != 0) || (fsync(fd) != 0))) { pzpd_set_error(PZPD_E_IO, "ftruncate: %s", strerror(errno)); ok = 0; }
    if (ok) { *out = sb; }
    return ok;
}

/** @brief Input keys of an edit, sorted by hash for lookups by record key. */
struct pzpd_ekey
{
    uint64_t hash;  ///< XXH64 of the key
    size_t   idx;   ///< Input position
};

/** @brief qsort comparator: (hash, input position), so equal keys keep their input order. */
static int pzpd_cmp_ekey(const void *a, const void *b)
{
    const struct pzpd_ekey *x = (const struct pzpd_ekey *) a, *y = (const struct pzpd_ekey *) b;
    if (x->hash != y->hash) { return (x->hash < y->hash) ? -1 : 1; }
    return (x->idx < y->idx) ? -1 : (x->idx > y->idx);
}

/** @brief First entry of the sorted key array with this hash (or n). */
static size_t pzpd_ekey_find(const struct pzpd_ekey *k, size_t n, uint64_t h)
{
    size_t lo = 0, hi = n;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (k[mid].hash < h) { lo = mid + 1; } else { hi = mid; } }
    return lo;
}

/** @brief Open an archive for editing: its manifest (shards not loaded), which must not be a lone shard. */
static struct pzpd_archive *pzpd_edit_open(const char *manifest)
{
    struct pzpd_archive *m = arch_open(manifest, 0);
    if (m == NULL) { return NULL; }
    if (m->standalone) { arch_close(m); pzpd_set_error(PZPD_E_ARG, "%s is a shard; edits take the archive's manifest", manifest); return NULL; }
    return m;
}

/** @brief Rewrite the manifest after an edit, from the shards the manifest lists. */
static int pzpd_edit_finish(const char *manifest, struct pzpd_archive *m)
{
    const char **paths = (const char **) calloc(m->shard_count ? m->shard_count : 1, sizeof(char *));
    if (paths == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    for (unsigned k = 0; k < m->shard_count; k++) { paths[k] = m->shards[k].path; }
    int ok = pzpd_manifest_rebuild(manifest, paths, m->shard_count);
    free(paths);
    return ok;
}

/** @brief Build a shard's word index section from one of its table sections (edits and reindex).
 *  @return 1 on success, 0 on failure (error set, e.g. the column is gone from a replaced table). */
static int pzpd_words_build_view(struct pzpd_buf *out, const struct pzpd_tschema *sc, const struct pzpd_tview *tv, uint64_t records,
                                 const char *column, const char *source_column)
{
    int ct = -1, cs = -1;
    for (unsigned c = 0; c < sc->ncols; c++)
    {
        if (!strcmp(sc->colname[c], column)) { ct = (int) c; }
        if ( (source_column != NULL) && !strcmp(sc->colname[c], source_column) ) { cs = (int) c; }
    }
    if ( (ct < 0) || (sc->type[ct] != PZPD_TYPE_STR) || (sc->count[ct] != 1) || (sc->flags & PZPD_TABLE_GLOBAL) )
        { pzpd_set_error(PZPD_E_ARG, "word index %s.%s: the table has no such str column (drop the word index first: reindex --drop)", sc->name, column); return 0; }
    if ( (source_column != NULL) && ((cs < 0) || (sc->type[cs] != PZPD_TYPE_STR) || (sc->count[cs] != 1) || (cs == ct)) )
        { pzpd_set_error(PZPD_E_ARG, "word index %s.%s: source column %s is not another str column", sc->name, column, source_column); return 0; }
    if ( (tv->index == NULL) || (tv->records != records) ) { pzpd_set_error(PZPD_E_FORMAT, "word index %s.%s: the table's row index is damaged", sc->name, column); return 0; }
    struct pzpd_wsrc src = { records, tv->index, tv->rowdata, tv->rows, sc->stride, tv->heap, tv->heap_bytes, sc->offset[ct], (cs >= 0) ? (int64_t) sc->offset[cs] : -1 };
    return pzpd_words_build(out, &src, source_column);
}

int pzpd_edit_table(const char *manifest, unsigned op, const char *table, const char *schema, unsigned table_flags,
                    const pzpd_edit_rows *rows, size_t n, const char *global_csv, size_t global_len, unsigned flags, uint64_t *unmatched)
{
    pzpd_clear_error();
    if (unmatched != NULL) { *unmatched = 0; }
    if ( (manifest == NULL) || (table == NULL) || (op < PZPD_EDIT_ADD) || (op > PZPD_EDIT_DROP) || ((rows == NULL) && (n > 0)) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    if ( (op == PZPD_EDIT_ADD) && (schema == NULL) ) { pzpd_set_error(PZPD_E_ARG, "add-table needs a schema"); return 0; }
    struct pzpd_archive *m = pzpd_edit_open(manifest);
    if (m == NULL) { return 0; }

    // The new schema (checked once against the manifest's view of the tables)
    struct pzpd_tschema nsc;
    memset(&nsc, 0, sizeof(nsc));
    int old = -1;
    for (unsigned t = 0; t < m->T; t++) { if (!strcmp(m->tables[t].name, table)) { old = (int) t; } }
    int ok = 1;
    if ( (op == PZPD_EDIT_ADD) && (old >= 0) ) { pzpd_set_error(PZPD_E_DUPLICATE, "table %s already exists", table); ok = 0; }
    if ( (op != PZPD_EDIT_ADD) && (old < 0) ) { pzpd_set_error(PZPD_E_NOTFOUND, "no table %s", table); ok = 0; }
    if ( ok && (op == PZPD_EDIT_ADD) && (m->T >= PZPD_MAX_TABLES) ) { pzpd_set_error(PZPD_E_ARG, "more than %d tables", PZPD_MAX_TABLES); ok = 0; }
    for (unsigned u = 0; ok && (op == PZPD_EDIT_ADD) && (u < m->S); u++) { if (!strcmp(m->streams[u], table)) { pzpd_set_error(PZPD_E_ARG, "\"%s\" is a stream name", table); ok = 0; } }
    if (ok && (op != PZPD_EDIT_DROP))
    {
        if (schema != NULL) { ok = pzpd_schema_parse(table, schema, (op == PZPD_EDIT_ADD) ? table_flags : (table_flags | (m->tables[old].flags & PZPD_TABLE_GLOBAL)), &nsc); }
        else { nsc = m->tables[old]; }
        if (ok) { pzpd_schema_publish(&nsc); }
        if ( ok && (flags & PZPD_EDIT_KEEP_MISSING) && (old >= 0) && !pzpd_schema_equal(&nsc, &m->tables[old]) ) { pzpd_set_error(PZPD_E_ARG, "keeping old rows needs the same schema"); ok = 0; }
        if ( ok && (old >= 0) && ((nsc.flags & PZPD_TABLE_GLOBAL) != (m->tables[old].flags & PZPD_TABLE_GLOBAL)) ) { pzpd_set_error(PZPD_E_ARG, "a replacement can't change between record and global table"); ok = 0; }
        if ( ok && (nsc.flags & PZPD_TABLE_GLOBAL) && (n > 0) ) { pzpd_set_error(PZPD_E_ARG, "%s is a global table: give its rows as global CSV", table); ok = 0; }
    }

    // Input keys
    struct pzpd_ekey *keys = (n > 0) ? (struct pzpd_ekey *) malloc(n * sizeof(struct pzpd_ekey)) : NULL;
    unsigned char *matched = (n > 0) ? (unsigned char *) calloc(n, 1) : NULL;
    if ( (n > 0) && ((keys == NULL) || (matched == NULL)) ) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
    for (size_t i = 0; ok && (i < n); i++) { keys[i].hash = XXH64(rows[i].key, rows[i].key_len, 0); keys[i].idx = i; }
    if (ok && (n > 0)) { qsort(keys, n, sizeof(struct pzpd_ekey), pzpd_cmp_ekey); }

    // Every row must parse before any shard is touched
    struct pzpd_buf grows = {0}, gheap = {0};
    for (size_t i = 0; ok && (op != PZPD_EDIT_DROP) && (i < n); i++)
    {
        grows.len = gheap.len = 0;
        if (pzpd_csv_parse(&nsc, rows[i].csv, rows[i].csv_len, &grows, &gheap) < 0)
        {
            pzpd_error_wrap(PZPD_E_ARG, "rows of \"%.*s\" (entry %zu)", (int)(rows[i].key_len > 200 ? 200 : rows[i].key_len), rows[i].key, i + 1);
            ok = 0;
        }
    }
    grows.len = gheap.len = 0;

    // Global rows are parsed once
    int64_t gn = 0;
    if ( ok && (op != PZPD_EDIT_DROP) && (nsc.flags & PZPD_TABLE_GLOBAL) )
    {
        gn = pzpd_csv_parse(&nsc, (global_csv != NULL) ? global_csv : "", (global_csv != NULL) ? global_len : 0, &grows, &gheap);
        if (gn < 0) { ok = 0; }
    }
    // The word index's merge rules are checked before any shard is touched
    if ( ok && (op != PZPD_EDIT_DROP) && !strcmp(table, PZPD_SYNONYMS_TABLE) )
    {
        ok = pzpd_synonyms_check(&nsc, grows.data, (gn > 0) ? (uint64_t) gn : 0, (const char *) gheap.data, gheap.len);
    }

    struct pzpd_buf secbuf = {0}, trows = {0}, theap = {0}, tindex = {0};
    for (unsigned k = 0; ok && (k < m->shard_count); k++)
    {
        struct pzpd_archive *sa = arch_open(m->shards[k].path, 0);
        if (sa == NULL) { pzpd_error_wrap(PZPD_OK, "%s", m->shards[k].path); ok = 0; break; }
        struct pzpd_rshard *s = &sa->shards[0];
        int so = -1;
        for (unsigned t = 0; t < sa->T; t++) { if (!strcmp(sa->tables[t].name, table)) { so = (int) t; } }
        // Resume: a shard already in the target state is left alone (replacing is simply redone)
        int done = ( (op == PZPD_EDIT_ADD) && (so >= 0) && pzpd_schema_equal(&sa->tables[so], &nsc) ) || ( (op == PZPD_EDIT_DROP) && (so < 0) );
        if ( !done && (op != PZPD_EDIT_ADD) && (so < 0) ) { pzpd_set_error(PZPD_E_FORMAT, "%s lacks table %s", s->path, table); ok = 0; }
        if ( !done && (op == PZPD_EDIT_ADD) && (so >= 0) ) { pzpd_set_error(PZPD_E_DUPLICATE, "%s already has a different table %s", s->path, table); ok = 0; }
        if (ok && done) { ok = pzpd_finish_flip(s); }
        if (!ok || done) { arch_close(sa); continue; }

        // The new section (record tables: rows of every record from the input, or kept / empty)
        secbuf.len = trows.len = theap.len = tindex.len = 0;
        uint64_t nrows = 0;
        if ( (op != PZPD_EDIT_DROP) && !(nsc.flags & PZPD_TABLE_GLOBAL) )
        {
            for (uint64_t i = 0; ok && (i < s->sb.record_count); i++)
            {
                uint32_t start = (uint32_t) nrows;
                ok = pzpd_buf_append(&tindex, &start, 4);
                size_t kl = 0;
                const char *key = arch_record_key(sa, i, &kl);
                if (key == NULL) { ok = 0; break; }
                uint64_t h = XXH64(key, kl, 0);
                int any = 0;
                for (size_t e = pzpd_ekey_find(keys, n, h); ok && (e < n) && (keys[e].hash == h); e++)
                {
                    const pzpd_edit_rows *r = &rows[keys[e].idx];
                    if ( (r->key_len != kl) || memcmp(r->key, key, kl) ) { continue; }
                    matched[keys[e].idx] = 1;
                    any = 1;
                    int64_t got = pzpd_csv_parse(&nsc, r->csv, r->csv_len, &trows, &theap);
                    if (got < 0)
                    {
                        pzpd_error_wrap(PZPD_E_ARG, "rows of \"%.*s\"", (int)(kl > 200 ? 200 : kl), key);
                        ok = 0;
                    }
                    else { nrows += (uint64_t) got; }
                }
                if (ok && !any && (flags & PZPD_EDIT_KEEP_MISSING))
                {
                    const void *orows = NULL;
                    uint32_t on = arch_table_rows(sa, i, (unsigned) so, &orows);
                    if ( (on == 0) && (pzpd_errorCode != PZPD_OK) ) { ok = 0; }
                    else if (on > 0) { ok = pzpd_stage_rows(&nsc, orows, on, s->tv[so].heap, s->tv[so].heap_bytes, &trows, &theap); nrows += on; }
                }
                if (nrows > 0xFFFFFFFFull) { pzpd_set_error(PZPD_E_ARG, "table %s: more than 4 G rows in one shard", table); ok = 0; }
            }
            uint32_t last = (uint32_t) nrows;
            ok = ok && pzpd_buf_append(&tindex, &last, 4) &&
                 pzpd_table_section(&secbuf, &nsc, s->sb.record_count, (const uint32_t *) tindex.data, trows.data, nrows, theap.data, theap.len);
        }
        else if (op != PZPD_EDIT_DROP)
        {
            ok = pzpd_table_section(&secbuf, &nsc, 0, NULL, grows.data, (uint64_t) gn, gheap.data, gheap.len);
        }

        // New directory: kept sections stay where they are
        struct pzpd_tslot ts[PZPD_MAX_TABLES];
        unsigned T = 0;
        for (unsigned t = 0; ok && (t < sa->T); t++)
        {
            if ( (op == PZPD_EDIT_DROP) && ((int) t == so) ) { continue; }
            memset(&ts[T], 0, sizeof(ts[T]));
            ts[T].slot = s->sb.tables[t];
            if ( (op == PZPD_EDIT_REPLACE) && ((int) t == so) )
            {
                pzpd_put_slot_name(ts[T].slot.name, nsc.name);
                ts[T].slot.flags = (uint8_t) nsc.flags;
                ts[T].slot.row_stride = nsc.stride;
                ts[T].data = secbuf.data;
                ts[T].bytes = secbuf.len;
            }
            T++;
        }
        if (ok && (op == PZPD_EDIT_ADD))
        {
            memset(&ts[T], 0, sizeof(ts[T]));
            pzpd_put_slot_name(ts[T].slot.name, nsc.name);
            ts[T].slot.flags = (uint8_t) nsc.flags;
            ts[T].slot.row_stride = nsc.stride;
            ts[T].data = secbuf.data;
            ts[T].bytes = secbuf.len;
            T++;
        }
        // Word indexes: rebuilt with a replaced table, dropped with a dropped one, kept otherwise. A damaged word
        // directory is not carried over (the indexes are re-derivable with reindex).
        struct pzpd_wslot ws[PZPD_MAX_WORD_INDEXES];
        struct pzpd_buf wbufs[PZPD_MAX_WORD_INDEXES];
        memset(wbufs, 0, sizeof(wbufs));
        unsigned W = 0, nw = s->words_bad ? 0 : pzpd_words_used(s->sb.words);
        for (unsigned j = 0; ok && (j < nw); j++)
        {
            const struct pzpd_disk_words *d = &s->sb.words[j];
            int mine = !strncmp(d->table, table, sizeof(d->table));
            if (mine && (op == PZPD_EDIT_DROP)) { continue; }
            memset(&ws[W], 0, sizeof(ws[W]));
            ws[W].slot = *d;
            if (mine && (op == PZPD_EDIT_REPLACE))
            {
                struct pzpd_wsec v;
                struct pzpd_tschema tsc;
                struct pzpd_tview tv;
                ok = pzpd_shard_wsec(s, j, &v) &&
                     pzpd_table_parse(secbuf.data, secbuf.len, &tsc, &tv, (int64_t) s->sb.record_count) &&
                     pzpd_words_build_view(&wbufs[W], &tsc, &tv, s->sb.record_count, d->column, v.source_column[0] ? v.source_column : NULL);
                ws[W].data  = wbufs[W].data;
                ws[W].bytes = wbufs[W].len;
            }
            W++;
        }
        if (ok)
        {
            int fd = open(s->path, O_RDWR | O_CLOEXEC);
            struct pzpd_disk_superblock nsb;
            if (fd < 0) { pzpd_set_error(PZPD_E_IO, "cannot open %s for writing: %s", s->path, strerror(errno)); ok = 0; }
            else
            {
                ok = pzpd_write_generation(fd, s->map, &s->sb, s->sb.file_bytes, T, ts, W, ws, 0, &nsb);
                close(fd);
            }
        }
        for (unsigned j = 0; j < PZPD_MAX_WORD_INDEXES; j++) { pzpd_buf_free(&wbufs[j]); }
        arch_close(sa);
    }
    if (ok) { ok = pzpd_edit_finish(manifest, m); }
    uint64_t um = 0;
    for (size_t i = 0; i < n; i++) { um += (matched != NULL) && !matched[i]; }
    if (unmatched != NULL) { *unmatched = um; }
    struct pzpd_saved_error e;
    pzpd_error_save(&e);
    pzpd_buf_free(&secbuf); pzpd_buf_free(&trows); pzpd_buf_free(&theap); pzpd_buf_free(&tindex);
    pzpd_buf_free(&grows); pzpd_buf_free(&gheap);
    free(keys);
    free(matched);
    arch_close(m);
    if (!ok) { pzpd_error_restore(&e); }
    return ok;
}

int pzpd_edit_words(const char *manifest, unsigned op, const char *table, const char *column, const char *source_column)
{
    pzpd_clear_error();
    if ( (manifest == NULL) || (table == NULL) || (column == NULL) || (op < PZPD_EDIT_ADD) || (op > PZPD_EDIT_DROP) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    struct pzpd_archive *m = pzpd_edit_open(manifest);
    if (m == NULL) { return 0; }
    int ok = 1, t = -1;
    for (unsigned k = 0; k < m->T; k++) { if (!strcmp(m->tables[k].name, table)) { t = (int) k; } }
    if ( (op != PZPD_EDIT_DROP) && (t < 0) ) { pzpd_set_error(PZPD_E_NOTFOUND, "no table %s", table); ok = 0; }
    struct pzpd_buf wbuf = {0};
    for (unsigned k = 0; ok && (k < m->shard_count); k++)
    {
        struct pzpd_archive *sa = arch_open(m->shards[k].path, 0);
        if (sa == NULL) { pzpd_error_wrap(PZPD_OK, "%s", m->shards[k].path); ok = 0; break; }
        struct pzpd_rshard *s = &sa->shards[0];
        unsigned nw = s->words_bad ? 0 : pzpd_words_used(s->sb.words);   // a damaged directory: only the named index is rebuilt
        int j = s->words_bad ? -1 : pzpd_words_slot(s->sb.words, table, column);
        // Resume: a shard already in the target state is left alone (a rebuild is simply redone)
        int done = 0;
        if ( (op == PZPD_EDIT_DROP) && (j < 0) ) { done = 1; }
        if ( (op == PZPD_EDIT_ADD) && (j >= 0) )
        {
            struct pzpd_wsec v;
            ok = pzpd_shard_wsec(s, (unsigned) j, &v);
            if (ok && strcmp(v.source_column, (source_column != NULL) ? source_column : "")) { pzpd_set_error(PZPD_E_DUPLICATE, "%s already has word index %s.%s with another source column", s->path, table, column); ok = 0; }
            done = 1;
        }
        if ( ok && !done && (op != PZPD_EDIT_DROP) && (j < 0) && (nw >= PZPD_MAX_WORD_INDEXES) ) { pzpd_set_error(PZPD_E_ARG, "more than %d word indexes", PZPD_MAX_WORD_INDEXES); ok = 0; }
        if (ok && done) { ok = pzpd_finish_flip(s); }
        if (!ok || done) { arch_close(sa); continue; }

        // The new directory: kept sections stay where they are
        struct pzpd_wslot ws[PZPD_MAX_WORD_INDEXES];
        unsigned W = 0;
        int target = -1;                                   // the slot rebuilt (at most one per call)
        for (unsigned q = 0; q < nw; q++)
        {
            if ( ((int) q == j) && (op == PZPD_EDIT_DROP) ) { continue; }
            memset(&ws[W], 0, sizeof(ws[W]));
            ws[W].slot = s->sb.words[q];
            if ((int) q == j) { target = (int) W; }
            W++;
        }
        if ( (op != PZPD_EDIT_DROP) && (j < 0) )
        {
            memset(&ws[W], 0, sizeof(ws[W]));
            snprintf(ws[W].slot.table, sizeof(ws[W].slot.table), "%s", table);
            snprintf(ws[W].slot.column, sizeof(ws[W].slot.column), "%s", column);
            target = (int) W;
            W++;
        }
        if (target >= 0)
        {
            int st = -1;
            for (unsigned u = 0; u < sa->T; u++) { if (!strcmp(sa->tables[u].name, table)) { st = (int) u; } }
            if (st < 0) { pzpd_set_error(PZPD_E_FORMAT, "%s lacks table %s", s->path, table); ok = 0; }
            ok = ok && pzpd_words_build_view(&wbuf, &sa->tables[st], &s->tv[st], s->sb.record_count, column, source_column);
            ws[target].data  = wbuf.data;
            ws[target].bytes = wbuf.len;
        }
        // Tables stay as they are
        struct pzpd_tslot ts[PZPD_MAX_TABLES];
        for (unsigned u = 0; u < sa->T; u++) { memset(&ts[u], 0, sizeof(ts[u])); ts[u].slot = s->sb.tables[u]; }
        if (ok)
        {
            int fd = open(s->path, O_RDWR | O_CLOEXEC);
            struct pzpd_disk_superblock nsb;
            if (fd < 0) { pzpd_set_error(PZPD_E_IO, "cannot open %s for writing: %s", s->path, strerror(errno)); ok = 0; }
            else
            {
                ok = pzpd_write_generation(fd, s->map, &s->sb, s->sb.file_bytes, sa->T, ts, W, ws, 0, &nsb);
                close(fd);
            }
        }
        arch_close(sa);
    }
    if (ok) { ok = pzpd_edit_finish(manifest, m); }
    struct pzpd_saved_error e;
    pzpd_error_save(&e);
    pzpd_buf_free(&wbuf);
    arch_close(m);
    if (!ok) { pzpd_error_restore(&e); }
    return ok;
}

int pzpd_compact(const char *manifest, uint64_t *reclaimed)
{
    pzpd_clear_error();
    if (reclaimed != NULL) { *reclaimed = 0; }
    struct pzpd_archive *m = pzpd_edit_open(manifest);
    if (m == NULL) { return 0; }
    int ok = 1, changed = 0;
    for (unsigned k = 0; ok && (k < m->shard_count); k++)
    {
        struct pzpd_archive *sa = arch_open(m->shards[k].path, 0);
        if (sa == NULL) { pzpd_error_wrap(PZPD_OK, "%s", m->shards[k].path); ok = 0; break; }
        struct pzpd_rshard *s = &sa->shards[0];
        if (!pzpd_finish_flip(s)) { arch_close(sa); ok = 0; break; }
        struct pzpd_disk_superblock sb = s->sb;
        if (s->map_len > sb.file_bytes)
        {
            // An earlier compact stopped after its last flip: cut the stale tail
            int fd = open(s->path, O_RDWR | O_CLOEXEC);
            if ( (fd < 0) || (ftruncate(fd, (off_t) sb.file_bytes) != 0) || (fsync(fd) != 0) ) { pzpd_set_error(PZPD_E_IO, "cannot trim %s: %s", s->path, strerror(errno)); ok = 0; }
            if (fd >= 0) { close(fd); }
            if (ok && (reclaimed != NULL)) { *reclaimed += s->map_len - sb.file_bytes; }
            changed = 1;
            if (!ok) { arch_close(sa); break; }
        }
        // Where the tables should start: after the last non-table index section
        uint64_t idxEnd = sb.meta_offset + sb.meta_bytes;
        uint64_t n = sb.record_count;
        uint64_t ends[4] = { sb.rtab_offset + n * sizeof(struct pzpd_disk_record), sb.btab_offset + n * sb.stream_count * sizeof(struct pzpd_disk_blob),
                             sb.hash_offset + sb.hash_count * sizeof(struct pzpd_disk_hash), sb.heap_offset + sb.heap_bytes };
        for (int i = 0; i < 4; i++) { if (ends[i] > idxEnd) { idxEnd = ends[i]; } }
        if ( (sb.group_count > 0) && (sb.groups_offset + sb.group_count * sizeof(struct pzpd_disk_group) > idxEnd) ) { idxEnd = sb.groups_offset + sb.group_count * sizeof(struct pzpd_disk_group); }
        uint64_t start = pzpd_align_up(idxEnd, PZPD_BLOCK), live = 0;
        unsigned T = 0;
        struct pzpd_tslot ts[PZPD_MAX_TABLES];
        unsigned char *copies[PZPD_MAX_TABLES] = {0};
        while ( (T < PZPD_MAX_TABLES) && (sb.tables[T].name[0] != 0) )
        {
            memset(&ts[T], 0, sizeof(ts[T]));
            ts[T].slot  = sb.tables[T];
            ts[T].bytes = sb.tables[T].section_bytes;
            copies[T]   = (unsigned char *) malloc(ts[T].bytes ? ts[T].bytes : 1);
            if (copies[T] == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; T++; break; }
            memcpy(copies[T], s->map + sb.tables[T].section_offset, (size_t) ts[T].bytes);
            ts[T].data = copies[T];
            live += pzpd_align_up(sizeof(struct pzpd_disk_section) + ts[T].bytes, PZPD_BLOCK);
            T++;
        }
        // Word index sections move with the tables
        unsigned W = s->words_bad ? 0 : pzpd_words_used(sb.words);
        struct pzpd_wslot ws[PZPD_MAX_WORD_INDEXES];
        unsigned char *wcopies[PZPD_MAX_WORD_INDEXES] = {0};
        for (unsigned j = 0; ok && (j < W); j++)
        {
            memset(&ws[j], 0, sizeof(ws[j]));
            ws[j].slot  = sb.words[j];
            ws[j].bytes = sb.words[j].section_bytes;
            wcopies[j]  = (unsigned char *) malloc(ws[j].bytes ? ws[j].bytes : 1);
            if (wcopies[j] == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; break; }
            memcpy(wcopies[j], s->map + sb.words[j].section_offset, (size_t) ws[j].bytes);
            ws[j].data = wcopies[j];
            live += pzpd_align_up(sizeof(struct pzpd_disk_section) + ws[j].bytes, PZPD_BLOCK);
        }
        uint64_t compactEnd = start + live + PZPD_BLOCK;
        if (ok && (compactEnd < sb.file_bytes))
        {
            // 1. append a copy of the live sections as generation +1 and flip to it; 2. write them again right after
            //    the index as generation +2, flip, and only then cut the file
            int fd = open(s->path, O_RDWR | O_CLOEXEC);
            struct pzpd_disk_superblock g1, g2;
            if (fd < 0) { pzpd_set_error(PZPD_E_IO, "cannot open %s for writing: %s", s->path, strerror(errno)); ok = 0; }
            else
            {
                ok = pzpd_write_generation(fd, s->map, &sb, sb.file_bytes, T, ts, W, ws, 0, &g1);
                if (ok) { pzpd_test_crash("compact"); }       // between the two generations
                ok = ok && pzpd_write_generation(fd, s->map, &g1, start, T, ts, W, ws, 1, &g2);
                close(fd);
                if (ok && (reclaimed != NULL)) { *reclaimed += sb.file_bytes - g2.file_bytes; }
                changed = 1;
            }
        }
        for (unsigned t = 0; t < T; t++) { free(copies[t]); }
        for (unsigned j = 0; j < PZPD_MAX_WORD_INDEXES; j++) { free(wcopies[j]); }
        arch_close(sa);
    }
    if (ok && changed) { ok = pzpd_edit_finish(manifest, m); }
    struct pzpd_saved_error e;
    pzpd_error_save(&e);
    arch_close(m);
    if (!ok) { pzpd_error_restore(&e); }
    return ok;
}

/** @brief Add a blob copied from another shard, keeping its stored metadata exactly (no PZPD_META_USER). */
static int pzpd_writer_blob_copy(pzpd_writer *w, unsigned stream, const char *name, size_t name_len, const void *data, size_t size, const pzpd_blob_meta *meta)
{
    if (!pzpd_writer_blob_ex(w, stream, name, name_len, data, size, meta)) { return 0; }
    w->blobs[stream].meta = *meta;
    return 1;
}

/** @brief Declare a table on a writer from an existing schema (same layout, same id order). */
static int pzpd_writer_table_copy(pzpd_writer *w, const struct pzpd_tschema *sc)
{
    if (w->tables == NULL) { w->tables = (struct pzpd_wtable *) calloc(PZPD_MAX_TABLES, sizeof(struct pzpd_wtable)); }
    if ( (w->tables == NULL) || (w->T >= PZPD_MAX_TABLES) ) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    w->tables[w->T].sc = *sc;
    pzpd_schema_publish(&w->tables[w->T].sc);
    w->T++;
    return 1;
}

/** @brief Rewrite one shard with the edited stream (stream edits, spec §7): a new shard file with the
 *  same identity and generation + 1 is written next to it, verified, and renamed over it.
 *  @param newS / names  New stream list; map[u] = old stream of new stream u, -1 for the added one.
 *  @param target        New index of the edited stream (-1 for drop-stream). */
static int pzpd_rewrite_shard(struct pzpd_archive *sa, unsigned newS, char names[][24], const int *map, int target,
                              const pzpd_edit_blob *blobs, size_t n, const struct pzpd_ekey *keys, unsigned char *matched, unsigned flags)
{
    struct pzpd_rshard *s = &sa->shards[0];
    const struct pzpd_disk_superblock sb = s->sb;
    size_t pl = strlen(s->path);
    char *base = (char *) malloc(pl + 32);
    if (base == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    snprintf(base, pl + 32, "%s.rewrite.pzpd", s->path);
    const char *sp[PZPD_MAX_STREAMS];
    for (unsigned u = 0; u < newS; u++) { sp[u] = names[u]; }
    pzpd_writer_opts o = { sp, newS, 0xFFFFFFFFFFFFull, sb.align };
    pzpd_writer *w = pzpd_writer_create(base, &o);
    free(base);
    if (w == NULL) { return 0; }
    int ok = 1;
    for (unsigned t = 0; ok && (t < sa->T); t++) { ok = pzpd_writer_table_copy(w, &sa->tables[t]); }
    for (unsigned t = 0; ok && (t < sa->T); t++)
    {
        if (sa->tables[t].flags & PZPD_TABLE_GLOBAL) { ok = pzpd_writer_global_rows(w, t, sa->gview[t].rowdata, (uint32_t) sa->gview[t].rows, sa->gview[t].heap, sa->gview[t].heap_bytes); }
    }
    // The shard's word indexes, rebuilt from the copied rows when the new shard closes
    unsigned nw = s->words_bad ? 0 : pzpd_words_used(sb.words);
    for (unsigned j = 0; ok && (j < nw); j++)
    {
        struct pzpd_wsec v;
        ok = pzpd_shard_wsec(s, j, &v) && pzpd_writer_words(w, sb.words[j].table, sb.words[j].column, v.source_column[0] ? v.source_column : NULL);
    }
    // The shard's groups, with their ids and names
    for (uint64_t g = 0; ok && (s->groups != NULL) && (g < s->sb.group_count); g++)
    {
        ok = pzpd_writer_group_as(w, s->groups[g].group_id, s->heap + s->groups[g].name_offset, s->groups[g].name_len, 0);
    }
    // The shard's identity (after the global rows, which the writer only takes for shard 0)
    memcpy(w->uuid, sb.archive_uuid, 16);
    w->shard_index   = sb.shard_index;
    w->shard_first   = sb.first_ordinal;
    w->total_records = sb.first_ordinal;
    w->generation    = sb.generation + 1;

    for (uint64_t i = 0; ok && (i < sb.record_count); i++)
    {
        const struct pzpd_disk_record *r = &s->rtab[i];
        size_t kl = 0;
        const char *key = arch_record_key(sa, i, &kl);
        ok = (key != NULL) && pzpd_writer_begin(w, key, kl, r->group, r->frame);
        const pzpd_edit_blob *src = NULL;
        if (ok && (n > 0))
        {
            uint64_t h = XXH64(key, kl, 0);
            for (size_t e = pzpd_ekey_find(keys, n, h); (e < n) && (keys[e].hash == h); e++)
            {
                const pzpd_edit_blob *b = &blobs[keys[e].idx];
                if ( (b->key_len == kl) && !memcmp(b->key, key, kl) ) { src = b; matched[keys[e].idx] = 1; break; }
            }
        }
        for (unsigned u = 0; ok && (u < newS); u++)
        {
            if ( ((int) u == target) && (src != NULL) ) { ok = pzpd_writer_blob_file(w, u, src->name, src->name_len, src->path); continue; }
            if ( ((int) u == target) && ((map[u] < 0) || (flags & PZPD_EDIT_DROP_MISSING)) ) { continue; }
            const struct pzpd_disk_blob *b = pzpd_blob_entry(s, i, (unsigned) map[u]);
            if (b == NULL) { ok = 0; break; }
            if (b->rel_offset == PZPD_MISSING) { continue; }
            pzpd_blob_meta meta = { b->format, b->width, b->height, b->channels, b->frames, b->bits, b->meta_flags };
            ok = pzpd_writer_blob_copy(w, u, s->heap + b->name_offset, b->name_len, s->map + r->offset + b->rel_offset, b->size, &meta);
        }
        for (unsigned t = 0; ok && (t < sa->T); t++)
        {
            if (sa->tables[t].flags & PZPD_TABLE_GLOBAL) { continue; }
            const void *rows = NULL;
            uint32_t cnt = arch_table_rows(sa, i, t, &rows);
            if ( (cnt == 0) && (pzpd_errorCode != PZPD_OK) ) { ok = 0; break; }
            if (cnt > 0) { ok = pzpd_writer_rows(w, t, rows, cnt, s->tv[t].heap, s->tv[t].heap_bytes); }
        }
        ok = ok && pzpd_writer_end(w);
    }
    char *final = NULL;
    if (ok) { ok = pzpd_writer_close_shard(w); }
    if (ok) { final = w->shards[0].path; ok = pzpd_patch_shard(final, sb.shard_count, sb.total_records); }

    // Verify the new shard completely before it replaces the old one
    if (ok)
    {
        struct pzpd_archive *v = arch_open(final, 0);
        ok = (v != NULL) && (v->shards[0].sb.record_count == sb.record_count) && arch_verify_shard(v, 0);
        for (uint64_t i = 0; ok && (i < sb.record_count); i++) { ok = arch_verify_record(v, i, 1); }
        if ( (v != NULL) && !ok && (pzpd_errorCode == PZPD_OK) ) { pzpd_set_error(PZPD_E_FORMAT, "%s: the rewritten shard failed verification", final); }
        if (v != NULL) { arch_close(v); }
    }
    if (ok) { pzpd_test_crash("rewrite"); }                    // the new shard is written, the old one not replaced yet
    if (ok && (rename(final, s->path) != 0)) { pzpd_set_error(PZPD_E_IO, "rename %s -> %s: %s", final, s->path, strerror(errno)); ok = 0; }
    if (ok) { pzpd_fsync_dir_of(s->path); }
    else if (final != NULL) { unlink(final); }
    if (w->fd >= 0) { close(w->fd); w->fd = -1; }
    if (w->tmp_path != NULL) { unlink(w->tmp_path); }
    struct pzpd_saved_error e;
    pzpd_error_save(&e);
    pzpd_writer_free(w);
    if (!ok) { pzpd_error_restore(&e); }
    return ok;
}

int pzpd_edit_stream(const char *manifest, unsigned op, const char *stream, const pzpd_edit_blob *blobs, size_t n, unsigned flags, uint64_t *unmatched)
{
    pzpd_clear_error();
    if (unmatched != NULL) { *unmatched = 0; }
    if ( (manifest == NULL) || (stream == NULL) || (op < PZPD_EDIT_ADD) || (op > PZPD_EDIT_DROP) || ((blobs == NULL) && (n > 0)) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    if ( (op == PZPD_EDIT_DROP) && (n > 0) ) { pzpd_set_error(PZPD_E_ARG, "drop-stream takes no files"); return 0; }
    struct pzpd_archive *m = pzpd_edit_open(manifest);
    if (m == NULL) { return 0; }
    int ok = 1, old = -1;
    for (unsigned u = 0; u < m->S; u++) { if (!strcmp(m->streams[u], stream)) { old = (int) u; } }
    if ( (op == PZPD_EDIT_ADD) && (old >= 0) ) { pzpd_set_error(PZPD_E_DUPLICATE, "stream %s already exists", stream); ok = 0; }
    if ( (op != PZPD_EDIT_ADD) && (old < 0) ) { pzpd_set_error(PZPD_E_NOTFOUND, "no stream %s", stream); ok = 0; }
    if ( ok && (op == PZPD_EDIT_ADD) && (m->S >= PZPD_MAX_STREAMS) ) { pzpd_set_error(PZPD_E_ARG, "more than %d streams", PZPD_MAX_STREAMS); ok = 0; }
    if ( ok && (op == PZPD_EDIT_DROP) && (m->S == 1) ) { pzpd_set_error(PZPD_E_ARG, "can't drop the only stream"); ok = 0; }
    for (unsigned t = 0; ok && (op == PZPD_EDIT_ADD) && (t < m->T); t++) { if (!strcmp(m->tables[t].name, stream)) { pzpd_set_error(PZPD_E_ARG, "\"%s\" is a table name", stream); ok = 0; } }
    if ( ok && (op == PZPD_EDIT_ADD) && !pzpd_check_stream_name(stream) ) { ok = 0; }

    // New stream list and the old stream of each new one
    char names[PZPD_MAX_STREAMS][24];
    int map[PZPD_MAX_STREAMS];
    unsigned newS = 0;
    int target = -1;
    for (unsigned u = 0; ok && (u < m->S); u++)
    {
        if ( (op == PZPD_EDIT_DROP) && ((int) u == old) ) { continue; }
        if ( (op == PZPD_EDIT_REPLACE) && ((int) u == old) ) { target = (int) newS; }
        memcpy(names[newS], m->streams[u], 24);
        map[newS++] = (int) u;
    }
    if (ok && (op == PZPD_EDIT_ADD)) { snprintf(names[newS], 24, "%s", stream); map[newS] = -1; target = (int) newS; newS++; }

    // Input: sorted keys; duplicate keys or names are errors
    struct pzpd_ekey *keys = (n > 0) ? (struct pzpd_ekey *) malloc(n * sizeof(struct pzpd_ekey)) : NULL;
    struct pzpd_ekey *nk = (n > 0) ? (struct pzpd_ekey *) malloc(n * sizeof(struct pzpd_ekey)) : NULL;
    unsigned char *matched = (n > 0) ? (unsigned char *) calloc(n, 1) : NULL;
    if ( (n > 0) && ((keys == NULL) || (nk == NULL) || (matched == NULL)) ) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
    for (size_t i = 0; ok && (i < n); i++)
    {
        if ( (blobs[i].key_len == 0) || (blobs[i].path == NULL) || !pzpd_check_name("blob name", blobs[i].name, blobs[i].name_len) ) { if (pzpd_errorCode == PZPD_OK) { pzpd_set_error(PZPD_E_ARG, "entry %zu: empty key or no path", i); } ok = 0; break; }
        keys[i].hash = XXH64(blobs[i].key, blobs[i].key_len, 0); keys[i].idx = i;
        nk[i].hash = XXH64(blobs[i].name, blobs[i].name_len, 0); nk[i].idx = i;
    }
    if (ok && (n > 0))
    {
        qsort(keys, n, sizeof(struct pzpd_ekey), pzpd_cmp_ekey);
        qsort(nk, n, sizeof(struct pzpd_ekey), pzpd_cmp_ekey);
        for (size_t i = 0; ok && (i + 1 < n); i++)
        {
            for (size_t j = i + 1; ok && (j < n) && (keys[j].hash == keys[i].hash); j++)
            {
                const pzpd_edit_blob *x = &blobs[keys[i].idx], *y = &blobs[keys[j].idx];
                if ( (x->key_len == y->key_len) && !memcmp(x->key, y->key, x->key_len) ) { pzpd_set_error(PZPD_E_DUPLICATE, "key \"%.*s\" is given twice", (int)(x->key_len > 200 ? 200 : x->key_len), x->key); ok = 0; }
            }
            for (size_t j = i + 1; ok && (j < n) && (nk[j].hash == nk[i].hash); j++)
            {
                const pzpd_edit_blob *x = &blobs[nk[i].idx], *y = &blobs[nk[j].idx];
                if ( (x->name_len == y->name_len) && !memcmp(x->name, y->name, x->name_len) ) { pzpd_set_error(PZPD_E_DUPLICATE, "name \"%.*s\" is given twice", (int)(x->name_len > 200 ? 200 : x->name_len), x->name); ok = 0; }
            }
        }
    }

    // Which shards still need the edit (resume), and a dry run over those: name clashes, empty records
    unsigned char *todo = (unsigned char *) calloc(m->shard_count ? m->shard_count : 1, 1);
    if (todo == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
    for (unsigned k = 0; ok && (k < m->shard_count); k++)
    {
        struct pzpd_archive *sa = arch_open(m->shards[k].path, 0);
        if (sa == NULL) { pzpd_error_wrap(PZPD_OK, "%s", m->shards[k].path); ok = 0; break; }
        int has = 0;
        for (unsigned u = 0; u < sa->S; u++) { if (!strcmp(sa->streams[u], stream)) { has = 1; } }
        int done = (op == PZPD_EDIT_ADD) ? has : (op == PZPD_EDIT_DROP) ? !has : (sa->shards[0].sb.generation > m->mshards[k].generation);
        todo[k] = !done;
        // Names: a new name must not exist anywhere in the archive, except as the blob it replaces. The shard's
        // hash and the input names are both sorted by hash: one merge pass, not a lookup per name per shard
        struct pzpd_rshard *s0 = pzpd_shard(sa, 0);
        size_t e = 0;
        for (uint64_t h = 0; ok && (s0 != NULL) && (n > 0) && (h < s0->sb.hash_count); h++)
        {
            const struct pzpd_disk_hash *he = &s0->hash[h];
            if (he->kind != PZPD_KIND_NAME) { continue; }
            while ( (e < n) && (nk[e].hash < he->hash) ) { e++; }
            for (size_t j = e; ok && (j < n) && (nk[j].hash == he->hash); j++)
            {
                const pzpd_edit_blob *bl = &blobs[nk[j].idx];
                if (!pzpd_match(sa, he->local_ordinal, he->stream, PZPD_KIND_NAME, bl->name, bl->name_len)) { continue; }
                size_t kl = 0;
                const char *key = arch_record_key(sa, he->local_ordinal, &kl);
                // The name may already be the edited stream's blob of the same record (replace, or a resumed add)
                int same = (key != NULL) && (kl == bl->key_len) && !memcmp(key, bl->key, kl) && (he->stream < sa->S) && !strcmp(sa->streams[he->stream], stream);
                if (!same) { pzpd_set_error(PZPD_E_DUPLICATE, "name \"%.*s\" already exists in the archive", (int)(bl->name_len > 200 ? 200 : bl->name_len), bl->name); ok = 0; }
            }
        }
        if (ok) { pzpd_clear_error(); }                         // failed matches are not errors

        if (ok && (!done || (n > 0)))
        {
            // Every input key must be a record somewhere (counted below), and in a shard still to edit every
            // record must keep a blob (an edited shard's stream list no longer matches `map`)
            struct pzpd_rshard *s = &sa->shards[0];
            for (uint64_t i = 0; ok && (i < s->sb.record_count); i++)
            {
                size_t kl = 0;
                const char *key = arch_record_key(sa, i, &kl);
                int gets = 0;
                if ( (key != NULL) && (n > 0) )
                {
                    uint64_t h = XXH64(key, kl, 0);
                    for (size_t e = pzpd_ekey_find(keys, n, h); (e < n) && (keys[e].hash == h); e++)
                    {
                        const pzpd_edit_blob *b = &blobs[keys[e].idx];
                        if ( (b->key_len == kl) && !memcmp(b->key, key, kl) ) { gets = 1; matched[keys[e].idx] = 1; }
                    }
                }
                if (done) { continue; }
                unsigned left = gets;
                for (unsigned u = 0; u < newS; u++)
                {
                    if ((int) u == target) { if ( (op == PZPD_EDIT_REPLACE) && !gets && !(flags & PZPD_EDIT_DROP_MISSING) ) { const struct pzpd_disk_blob *b = pzpd_blob_entry(s, i, (unsigned) map[u]); left += (b != NULL) && (b->rel_offset != PZPD_MISSING); } continue; }
                    const struct pzpd_disk_blob *b = pzpd_blob_entry(s, i, (unsigned) map[u]);
                    left += (b != NULL) && (b->rel_offset != PZPD_MISSING);
                }
                if (left == 0) { pzpd_set_error(PZPD_E_ARG, "record \"%.*s\" would be left without blobs", (int)(kl > 200 ? 200 : kl), key ? key : ""); ok = 0; }
            }
        }
        arch_close(sa);
    }

    // The rewrite, shard by shard
    for (unsigned k = 0; ok && (k < m->shard_count); k++)
    {
        if (!todo[k]) { continue; }
        struct pzpd_archive *sa = arch_open(m->shards[k].path, 0);
        if (sa == NULL) { ok = 0; break; }
        ok = pzpd_rewrite_shard(sa, newS, names, map, target, blobs, n, keys, matched, flags);
        arch_close(sa);
        if (ok) { pzpd_test_crash("shard"); }                 // between shards
    }
    if (ok) { ok = pzpd_edit_finish(manifest, m); }
    uint64_t um = 0;
    for (size_t i = 0; i < n; i++) { um += (matched != NULL) && !matched[i]; }
    if (unmatched != NULL) { *unmatched = um; }
    struct pzpd_saved_error e;
    pzpd_error_save(&e);
    free(keys);
    free(nk);
    free(matched);
    free(todo);
    arch_close(m);
    if (!ok) { pzpd_error_restore(&e); }
    return ok;
}

//-----------------------------------------------------------------------------------------------
// Video groups (spec §3.3, §4.6): a group's frames are consecutive records in one shard
//-----------------------------------------------------------------------------------------------

int64_t pzpd_group_find(pzpd *a, const char *name, size_t len)
{
    pzpd_clear_error();
    if ( (a == NULL) || (name == NULL) || (len == 0) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return -1; }
    for (unsigned i = 0; i < a->member_count; i++)
    {
        struct pzpd_member *mb = &a->m[i];
        if (mb->arch == NULL) { continue; }
        int64_t r = arch_find(mb->arch, name, len, NULL, PZPD_KIND_GROUP);
        if (r >= 0) { return (int64_t) mb->first + r; }
    }
    pzpd_set_error(PZPD_E_NOTFOUND, "no group \"%.*s\"", (int)(len > 200 ? 200 : len), name);
    return -1;
}

int pzpd_group_info(pzpd *a, uint64_t ordinal, pzpd_group *out)
{
    unsigned mi; uint64_t local, sl;
    pzpd_clear_error();
    if (out == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL output"); return 0; }
    memset(out, 0, sizeof(*out));
    struct pzpd_archive *ar = pzpd_route(a, ordinal, &mi, &local);
    if (ar == NULL) { return 0; }
    struct pzpd_rshard *s = pzpd_locate(ar, local, &sl);
    if (s == NULL) { return 0; }
    const struct pzpd_disk_group *g = pzpd_group_at(s, sl);
    if (g == NULL) { return 0; }                                  // not in a group (no error)
    out->first_ordinal = a->m[mi].first + s->first_ordinal + g->first_local;
    out->frames        = g->frame_count;
    out->id            = g->group_id;
    out->index         = (uint32_t)(sl - g->first_local);
    out->name          = s->heap + g->name_offset;
    out->name_len      = g->name_len;
    return 1;
}

/** @brief Check a range of consecutive records (one member, one shard, one group or none) and find the
 *  file span covering their requested blobs. @return 1 with blobs, 0 with none, -1 on error (error set). */
static int pzpd_range(pzpd *a, uint64_t first, uint32_t count, uint32_t mask, struct pzpd_archive **arOut, unsigned *miOut,
                      struct pzpd_rshard **so, uint64_t *slFirst, uint64_t *lo, uint64_t *hi)
{
    unsigned mi, mi2; uint64_t local, local2, sl, sl2;
    if (count == 0) { pzpd_set_error(PZPD_E_ARG, "empty range"); return -1; }
    struct pzpd_archive *ar = pzpd_route(a, first, &mi, &local);
    if (ar == NULL) { return -1; }
    if ( (first + count - 1 < first) || (pzpd_route(a, first + count - 1, &mi2, &local2) == NULL) ) { return -1; }
    if (mi2 != mi) { pzpd_set_error(PZPD_E_ARG, "range %llu+%u spans two collection members", (unsigned long long) first, count); return -1; }
    struct pzpd_rshard *s = pzpd_locate(ar, local, &sl), *s2 = pzpd_locate(ar, local2, &sl2);
    if ( (s == NULL) || (s2 == NULL) ) { return -1; }
    if (s != s2) { pzpd_set_error(PZPD_E_ARG, "range %llu+%u spans two shards", (unsigned long long) first, count); return -1; }
    uint32_t mm = pzpd_member_mask(&a->m[mi], mask);
    uint32_t g = s->rtab[sl].group;
    uint64_t L = UINT64_MAX, H = 0;
    for (uint64_t k = 0; k < count; k++)
    {
        if (s->rtab[sl + k].group != g) { pzpd_set_error(PZPD_E_ARG, "range %llu+%u crosses a group boundary", (unsigned long long) first, count); return -1; }
        struct pzpd_rshard *sx;
        uint64_t lx, st, en;
        int r = pzpd_span(ar, s->first_ordinal + sl + k, mm, &sx, &lx, &st, &en);
        if (r < 0) { return -1; }
        if (r == 0) { continue; }
        uint64_t base = s->rtab[sl + k].offset;
        if (base + st < L) { L = base + st; }
        if (base + en > H) { H = base + en; }
    }
    *arOut = ar; *miOut = mi; *so = s; *slFirst = sl;
    if (L == UINT64_MAX) { return 0; }
    *lo = L; *hi = H;
    return 1;
}

size_t pzpd_range_span(pzpd *a, uint64_t first, uint32_t count, uint32_t stream_mask)
{
    struct pzpd_archive *ar; unsigned mi; struct pzpd_rshard *s; uint64_t sl, lo = 0, hi = 0;
    pzpd_clear_error();
    if (a == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL handle"); return 0; }
    return (pzpd_range(a, first, count, stream_mask, &ar, &mi, &s, &sl, &lo, &hi) == 1) ? (size_t)(hi - lo) : 0;
}

ssize_t pzpd_read_range(pzpd *a, uint64_t first, uint32_t count, uint32_t stream_mask, void *buf, size_t cap, pzpd_blob_ref *refs)
{
    struct pzpd_archive *ar; unsigned mi; struct pzpd_rshard *s; uint64_t sl, lo = 0, hi = 0;
    pzpd_clear_error();
    if (a == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL handle"); return PZPD_E_ARG; }
    if (refs != NULL) { memset(refs, 0, sizeof(pzpd_blob_ref) * (size_t) count * a->S); }
    int r = pzpd_range(a, first, count, stream_mask, &ar, &mi, &s, &sl, &lo, &hi);
    if (r < 0) { return (ssize_t) pzpd_errorCode; }
    if (r == 0) { return 0; }
    if ( (buf == NULL) || (cap < hi - lo) ) { pzpd_set_error(PZPD_E_ARG, "buffer of %zu bytes is too small for a %llu byte range", cap, (unsigned long long)(hi - lo)); return PZPD_E_ARG; }
    if (!pzpd_pread_all(s->fd, buf, (size_t)(hi - lo), lo)) { return (ssize_t) pzpd_errorCode; }
    struct pzpd_member *mb = &a->m[mi];
    for (uint64_t k = 0; (refs != NULL) && (k < count); k++)
    {
        for (unsigned u = 0; u < a->S; u++)
        {
            int ms = mb->to_member[u];
            if ( !(stream_mask & (1u << u)) || (ms < 0) ) { continue; }
            const struct pzpd_disk_blob *b = pzpd_blob_entry(s, sl + k, (unsigned) ms);
            if (b == NULL) { return (ssize_t) pzpd_errorCode; }
            if (b->rel_offset == PZPD_MISSING) { continue; }
            pzpd_blob_ref *rf = &refs[k * a->S + u];
            rf->data   = (const unsigned char *) buf + (s->rtab[sl + k].offset + b->rel_offset - lo);
            rf->size   = b->size;
            rf->format = b->format;
            if (ar->flags & PZPD_O_VERIFY)
            {
                uint32_t want;
                if ( !pzpd_header_blob_xxh(s, sl + k, (unsigned) ms, &want) || (XXH32(rf->data, rf->size, 0) != want) )
                {
                    if (pzpd_errorCode == PZPD_OK) { pzpd_set_error(PZPD_E_CHECKSUM, "%s: checksum mismatch in record %llu stream %u", s->path, (unsigned long long)(sl + k), (unsigned) ms); }
                    return (ssize_t) pzpd_errorCode;
                }
            }
        }
    }
    return (ssize_t)(hi - lo);
}
