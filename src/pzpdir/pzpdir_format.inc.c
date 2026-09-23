/** @file pzpdir_format.inc.c
 *  @brief pzpdir.c, part 1 of 13: on-disk constants and structures, errors, small helpers.
 *  Included by pzpdir.c in this order (one translation unit: everything stays static); not compiled on its own. */

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
