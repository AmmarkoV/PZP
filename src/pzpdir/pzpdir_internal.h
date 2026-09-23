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

/** @file pzpdir_internal.h
 *  @brief  Internal declarations shared by the pzpdir_*.c files that implement pzpdir.h (PZPD archives,
 *  spec doc/pzpd-spec.md v0.4). Not installed, not for clients: they include pzpdir.h and link libpzpdir.
 *
 *  Everything declared here has hidden visibility, so libpzpdir.so exports the public API only.
 *  The public functions are documented in pzpdir.h; the files document the internal helpers and the
 *  data they keep:
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

#ifndef PZPDIR_INTERNAL_H_INCLUDED
#define PZPDIR_INTERNAL_H_INCLUDED ///< Include guard

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

#ifndef PZPDIR_DEBUG
/** @brief Compile-time switch (0/1) for internal debug messages on stderr. */
#define PZPDIR_DEBUG 0
#endif

#define PZPD_NORMAL "\033[0m"   ///< ANSI escape: reset terminal color
#define PZPD_RED    "\033[31m"  ///< ANSI escape: red text
#define PZPD_GREEN  "\033[32m"  ///< ANSI escape: green text
#define PZPD_YELLOW "\033[33m"  ///< ANSI escape: yellow text

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "PZPD structures are little-endian and read in place");

/** @brief Marks the definition of an internal function shared between the pzpdir_*.c files: hidden, so
 *  libpzpdir.so exports the public API only (doxygen reads it as `static`, as these were in one file). */
#define PZPD_INTERNAL __attribute__((visibility("hidden")))

// Internal symbols stay inside libpzpdir.so (and never clash with a client's own symbols)
#pragma GCC visibility push(hidden)

//-----------------------------------------------------------------------------------------------
// Constants of the on-disk format
//-----------------------------------------------------------------------------------------------

/** @brief Superblocks, manifest headers and index sections live in 4 KiB slots / boundaries, so
 *  every section can be mmapped and found by scanning 4 KiB boundaries during recovery. */
#define PZPD_BLOCK 4096u

/** @brief Blob payloads inside a record start on 64-byte boundaries (cache line). */
#define PZPD_BLOB_ALIGN 64u

static const char PZPD_MAGIC_SHARD[8] __attribute__((unused))    = {'P','Z','P','D','S','H','R','D'}; ///< Shard superblock (primary at 0, backup at EOF-4096)
static const char PZPD_MAGIC_MANIFEST[8] __attribute__((unused)) = {'P','Z','P','D','M','A','N','I'}; ///< Manifest header
static const char PZPD_MAGIC_COLL[8] __attribute__((unused))     = {'P','Z','P','D','C','O','L','L'}; ///< Collection file (phase 1b)
static const char PZPD_MAGIC_RECORD[8] __attribute__((unused))   = {'P','Z','P','D','R','E','C','D'}; ///< Start of every record header
static const char PZPD_MAGIC_SECTION[8] __attribute__((unused))  = {'P','Z','P','D','S','E','C','T'}; ///< Start of every index section

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
// Errors and small helpers (pzpdir_format.c)
//-----------------------------------------------------------------------------------------------

extern __thread char pzpd_errorText[512]; ///< Message of the last error on this thread, "" after success
extern __thread int  pzpd_errorCode;      ///< enum pzpd_error of the last error on this thread

/** @brief An error kept across cleanup calls that clear or overwrite it (pzpd_error_save() / pzpd_error_restore()). */
struct pzpd_saved_error
{
    int  code;       ///< enum pzpd_error
    char text[512];  ///< Message
};

/** @brief Growable byte buffer used by the writer for index sections and the heap. */
struct pzpd_buf
{
    unsigned char *data; ///< Contents
    size_t         len;  ///< Bytes used
    size_t         cap;  ///< Bytes allocated
};

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

//-----------------------------------------------------------------------------------------------
// Table schemas (pzpdir_tables.c)
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

//-----------------------------------------------------------------------------------------------
// Word index sections (pzpdir_words_build.c)
//-----------------------------------------------------------------------------------------------

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

//-----------------------------------------------------------------------------------------------
// Writer (pzpdir_writer.c)
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

/** @brief One table as a manifest holds it: the schema, plus the rows for global tables. */
struct pzpd_mtable
{
    const struct pzpd_tschema *sc;  ///< Schema
    const void *rows;               ///< Global rows (NULL for record tables: schema only)
    uint64_t    nrows;              ///< Global rows
    const void *heap;               ///< Their strings
    uint64_t    heap_len;           ///< Strings size
};

//-----------------------------------------------------------------------------------------------
// Reader: one archive (pzpdir_reader.c)
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

/** @brief One (word, stats) entry gathered from several vocabularies before they are merged. */
struct pzpd_went
{
    const char *w;       ///< Word bytes
    uint32_t    len;     ///< Length
    uint64_t    records; ///< Records containing it
    uint64_t    count;   ///< Occurrences
};

/** @brief A byte string pointer + length (source values). */
struct pzpd_bstr { const char *s; uint32_t len; };

//-----------------------------------------------------------------------------------------------
// Handles: collections of archives (pzpdir_handle.c)
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

//-----------------------------------------------------------------------------------------------
// Internal functions used by more than one file (documented where they are defined)
//-----------------------------------------------------------------------------------------------

// pzpdir_format.c
PZPD_INTERNAL void pzpd_set_error(int code, const char *fmt, ...);
PZPD_INTERNAL void pzpd_clear_error(void);
PZPD_INTERNAL void pzpd_error_save(struct pzpd_saved_error *e);
PZPD_INTERNAL void pzpd_error_restore(const struct pzpd_saved_error *e);
PZPD_INTERNAL void pzpd_error_wrap(int code, const char *fmt, ...);
PZPD_INTERNAL int pzpd_buf_append(struct pzpd_buf *b, const void *src, size_t n);
PZPD_INTERNAL void pzpd_buf_free(struct pzpd_buf *b);
PZPD_INTERNAL int pzpd_pwrite_all(int fd, const void *src, size_t n, uint64_t off);
PZPD_INTERNAL int pzpd_pread_all(int fd, void *dst, size_t n, uint64_t off);
PZPD_INTERNAL void pzpd_fsync_dir_of(const char *path);
PZPD_INTERNAL void pzpd_put_slot_name(char slot[23], const char *name);
PZPD_INTERNAL void pzpd_get_slot_name(char out[24], const char slot[23]);
PZPD_INTERNAL int pzpd_check_name(const char *what, const char *s, size_t len);
PZPD_INTERNAL int pzpd_check_stream_name(const char *nm);

// pzpdir_tables.c
PZPD_INTERNAL void pzpd_schema_publish(struct pzpd_tschema *sc);
PZPD_INTERNAL int pzpd_schema_parse(const char *name, const char *text, unsigned flags, struct pzpd_tschema *sc);
PZPD_INTERNAL int pzpd_schema_equal(const struct pzpd_tschema *a, const struct pzpd_tschema *b);
PZPD_INTERNAL int64_t pzpd_csv_parse(const struct pzpd_tschema *sc, const char *csv, size_t len, struct pzpd_buf *rows, struct pzpd_buf *heap);
PZPD_INTERNAL int pzpd_csv_render(const struct pzpd_tschema *sc, const unsigned char *row, const char *heap, uint64_t heapLen, struct pzpd_buf *out);
PZPD_INTERNAL int pzpd_table_section(struct pzpd_buf *out, const struct pzpd_tschema *sc, uint64_t records, const uint32_t *index,
                              const void *rows, uint64_t nrows, const void *heap, uint64_t heapLen);
PZPD_INTERNAL int pzpd_table_parse(const unsigned char *d, uint64_t bytes, struct pzpd_tschema *sc, struct pzpd_tview *v, int64_t expectRecords);

// pzpdir_words_build.c
PZPD_INTERNAL int pzpd_word_cmp(const char *a, size_t al, const char *b, size_t bl);
PZPD_INTERNAL int pzpd_buf_pad8(struct pzpd_buf *b);
PZPD_INTERNAL void pzpd_sdict_free(struct pzpd_sdict *d);
PZPD_INTERNAL int64_t pzpd_sdict_find(const struct pzpd_sdict *d, const char *s, size_t n);
PZPD_INTERNAL int64_t pzpd_sdict_id(struct pzpd_sdict *d, const char *s, size_t n);
PZPD_INTERNAL int pzpd_cmp_u32(const void *a, const void *b);
PZPD_INTERNAL int pzpd_words_build(struct pzpd_buf *out, const struct pzpd_wsrc *in, const char *source_column);
PZPD_INTERNAL int pzpd_synonyms_check(const struct pzpd_tschema *sc, const unsigned char *rows, uint64_t n, const char *heap, uint64_t heap_bytes);
PZPD_INTERNAL int pzpd_wsec_parse(const unsigned char *d, uint64_t bytes, int manifest, struct pzpd_wsec *v);
PZPD_INTERNAL int pzpd_wsec_sub(const struct pzpd_wsec *v, unsigned k, int64_t expectRecords, struct pzpd_wsub *s);
PZPD_INTERNAL int pzpd_wsec_find(const struct pzpd_wsec *v, const char *source, size_t len, int64_t expectRecords);
PZPD_INTERNAL int pzpd_wsub_word(const struct pzpd_wsub *s, uint64_t k, const char **w, size_t *len);
PZPD_INTERNAL int64_t pzpd_wsub_find(const struct pzpd_wsub *s, const char *word, size_t len);
PZPD_INTERNAL int pzpd_wsub_check_full(const struct pzpd_wsub *s);

// pzpdir_writer.c
PZPD_INTERNAL int pzpd_write_section(int fd, uint64_t *off, uint32_t kind, const void *data, uint64_t bytes, uint64_t *data_off, XXH64_state_t *idx);
PZPD_INTERNAL void pzpd_fill_streams(struct pzpd_disk_stream *slots, unsigned S, char names[][24]);
PZPD_INTERNAL uint64_t pzpd_ext_checksum(const void *ext, size_t len);
PZPD_INTERNAL void pzpd_seal_superblock(struct pzpd_disk_superblock *sb);
PZPD_INTERNAL int pzpd_writer_close_shard(pzpd_writer *w);
PZPD_INTERNAL int pzpd_writer_group_as(pzpd_writer *w, uint32_t id, const char *name, size_t len, uint64_t hint);
PZPD_INTERNAL int pzpd_stage_rows(const struct pzpd_tschema *sc, const void *rows, uint32_t nrows, const void *strings, size_t strings_len,
                           struct pzpd_buf *outRows, struct pzpd_buf *outHeap);
PZPD_INTERNAL int pzpd_patch_shard(const char *path, uint32_t shard_count, uint64_t total_records);
PZPD_INTERNAL void pzpd_writer_free(pzpd_writer *w);
PZPD_INTERNAL int pzpd_write_manifest(const char *path, const uint8_t *uuid, uint64_t total, unsigned S, char streams[][24], unsigned shard_count,
                               const struct pzpd_buf *shardTab, const struct pzpd_buf *names, struct pzpd_buf *ghash, unsigned T, const struct pzpd_mtable *tabs,
                               struct pzpd_archive *const *wshards);

// pzpdir_reader.c
PZPD_INTERNAL int pzpd_check_section(const unsigned char *map, uint64_t file, uint64_t data_off, uint32_t kind, uint64_t bytes);
PZPD_INTERNAL int pzpd_superblock_valid(const struct pzpd_disk_superblock *sb, uint64_t file);
PZPD_INTERNAL void pzpd_populate(const void *p, size_t len);
PZPD_INTERNAL int pzpd_sb_from_sections(const unsigned char *map, uint64_t file, uint64_t hint_first, struct pzpd_disk_superblock *out);
PZPD_INTERNAL struct pzpd_rshard *pzpd_shard(struct pzpd_archive *a, unsigned i);
PZPD_INTERNAL int pzpd_shard_of(const struct pzpd_archive *a, uint64_t ordinal);
PZPD_INTERNAL struct pzpd_rshard *pzpd_locate(struct pzpd_archive *a, uint64_t ordinal, uint64_t *local);
PZPD_INTERNAL const struct pzpd_disk_blob *pzpd_blob_entry(const struct pzpd_rshard *s, uint64_t local, unsigned stream);
PZPD_INTERNAL struct pzpd_archive *pzpd_arch_open(const char *path, unsigned int flags);
PZPD_INTERNAL void pzpd_arch_close(struct pzpd_archive *a);
PZPD_INTERNAL int pzpd_shard_wsec(const struct pzpd_rshard *s, unsigned j, struct pzpd_wsec *v);
PZPD_INTERNAL int pzpd_words_slot(const struct pzpd_disk_words *dir, const char *table, const char *column);
PZPD_INTERNAL unsigned pzpd_words_used(const struct pzpd_disk_words *dir);
PZPD_INTERNAL int pzpd_cmp_bstr(const void *a, const void *b);
PZPD_INTERNAL size_t pzpd_went_fold(struct pzpd_went *e, size_t n);
PZPD_INTERNAL int pzpd_mwords_sections(struct pzpd_archive *const *shards, unsigned n, struct pzpd_buf secs[PZPD_MAX_WORD_INDEXES],
                                struct pzpd_disk_words dir[PZPD_MAX_WORD_INDEXES], unsigned *W);
PZPD_INTERNAL int pzpd_arch_shard_info_get(struct pzpd_archive *a, unsigned shard, pzpd_shard_info *out);
PZPD_INTERNAL const struct pzpd_disk_group *pzpd_group_at(const struct pzpd_rshard *s, uint64_t local);
PZPD_INTERNAL int pzpd_match(struct pzpd_archive *a, uint64_t ordinal, uint8_t stream, uint8_t kind, const char *key, size_t len);
PZPD_INTERNAL int64_t pzpd_arch_find(struct pzpd_archive *a, const char *key, size_t len, int *stream_out, int only_kind);
PZPD_INTERNAL const char *pzpd_arch_record_key(struct pzpd_archive *a, uint64_t ordinal, size_t *len);
PZPD_INTERNAL int pzpd_arch_blob_info_get(struct pzpd_archive *a, uint64_t ordinal, unsigned stream, pzpd_blob_info *out);
PZPD_INTERNAL int pzpd_header_blob_xxh(const struct pzpd_rshard *s, uint64_t local, unsigned stream, uint32_t *xxh);
PZPD_INTERNAL ssize_t pzpd_arch_read_into(struct pzpd_archive *a, uint64_t ordinal, unsigned stream, void *buf, size_t cap);
PZPD_INTERNAL void *pzpd_arch_read_alloc(struct pzpd_archive *a, uint64_t ordinal, unsigned stream, size_t *size);
PZPD_INTERNAL const void *pzpd_arch_view(struct pzpd_archive *a, uint64_t ordinal, unsigned stream, size_t *size);
PZPD_INTERNAL int pzpd_span(struct pzpd_archive *a, uint64_t ordinal, uint32_t mask, struct pzpd_rshard **so, uint64_t *localOut, uint64_t *start, uint64_t *end);
PZPD_INTERNAL size_t pzpd_arch_record_span(struct pzpd_archive *a, uint64_t ordinal, uint32_t stream_mask);
PZPD_INTERNAL ssize_t pzpd_arch_read_record(struct pzpd_archive *a, uint64_t ordinal, uint32_t stream_mask, void *buf, size_t cap, pzpd_blob_ref *refs);
PZPD_INTERNAL int pzpd_arch_verify_record(struct pzpd_archive *a, uint64_t ordinal, int check_blobs);
PZPD_INTERNAL int pzpd_arch_verify_shard(struct pzpd_archive *a, unsigned shard);
#if PZPDIR_WITH_PZP
PZPD_INTERNAL unsigned char *pzpd_arch_read_pzp(struct pzpd_archive *a, uint64_t ordinal, unsigned stream,
                             unsigned int *width, unsigned int *height,
                             unsigned int *bpp, unsigned int *channels);
#endif
PZPD_INTERNAL uint32_t pzpd_arch_table_rows(struct pzpd_archive *a, uint64_t ordinal, unsigned t, const void **rows_out);
PZPD_INTERNAL const char *pzpd_view_str(const struct pzpd_tview *v, const void *field, size_t *len);
PZPD_INTERNAL ssize_t pzpd_rows_csv(const struct pzpd_tschema *sc, const unsigned char *rows, uint32_t n, const struct pzpd_tview *v, char *out, size_t cap);

// pzpdir_handle.c
PZPD_INTERNAL int pzpd_coll_parse(const char *path, struct pzpd_coll_file *c);
PZPD_INTERNAL char *pzpd_heap_str(const char *heap, uint32_t off, uint32_t len);
PZPD_INTERNAL char *pzpd_resolve_member_path(const char *collPath, const char *stored);
PZPD_INTERNAL struct pzpd_archive *pzpd_route(pzpd *a, uint64_t ordinal, unsigned *member, uint64_t *local);
PZPD_INTERNAL uint32_t pzpd_member_mask(const struct pzpd_member *mb, uint32_t mask);

#pragma GCC visibility pop

#endif // PZPDIR_INTERNAL_H_INCLUDED
