/** @file pzpdir_reader.c
 *  @brief PZPD library: reader: opening shards, lookup, reading data.
 *  Shared types and internal declarations are in pzpdir_internal.h. */

#include "pzpdir_internal.h"

#if PZPDIR_WITH_PZP
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wunused-function"
 #pragma GCC diagnostic ignored "-Wunused-variable"
 #include "pzp.h"
 #pragma GCC diagnostic pop
#endif

//-----------------------------------------------------------------------------------------------
// Reader
//-----------------------------------------------------------------------------------------------

/** @brief Bytes of a superblock's revision-12 extension covered by ext_checksum. */
#define PZPD_SB_EXT_BYTES (offsetof(struct pzpd_disk_superblock, ext_checksum) - offsetof(struct pzpd_disk_superblock, words))
/** @brief Bytes of a manifest header's revision-12 extension covered by ext_checksum. */
#define PZPD_MH_EXT_BYTES (offsetof(struct pzpd_disk_manifest, ext_checksum) - offsetof(struct pzpd_disk_manifest, words))

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


/** @brief Check a section header in front of data_off, of the expected kind and size. */
PZPD_INTERNAL int pzpd_check_section(const unsigned char *map, uint64_t file, uint64_t data_off, uint32_t kind, uint64_t bytes)
{
    if ( (data_off < sizeof(struct pzpd_disk_section)) || !pzpd_in_file(data_off, bytes, file) ) { return 0; }
    struct pzpd_disk_section sh;
    memcpy(&sh, map + data_off - sizeof(sh), sizeof(sh));
    return (memcmp(sh.magic, PZPD_MAGIC_SECTION, 8) == 0) && (sh.kind == kind) && (sh.bytes == bytes);
}

/** @brief Validate a superblock candidate (magic, version, checksum, geometry).
 *  @return 1 if valid, 0 otherwise (error set). */
PZPD_INTERNAL int pzpd_superblock_valid(const struct pzpd_disk_superblock *sb, uint64_t file)
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
PZPD_INTERNAL void pzpd_populate(const void *p, size_t len)
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
PZPD_INTERNAL int pzpd_sb_from_sections(const unsigned char *map, uint64_t file, uint64_t hint_first, struct pzpd_disk_superblock *out)
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
    // Hex digits parsed by hand: sscanf() would first run strlen() over the mapping, which has no terminator
    const char *uu = pzpd_meta_value(meta, mlen, "archive_uuid");
    for (int i = 0; (uu != NULL) && (uu + 33 <= meta + mlen) && (*uu == '"') && (i < 16); i++)
    {
        int v = 0;
        for (int k = 1; k <= 2; k++)
        {
            char c = uu[2 * i + k];
            int d = ( (c >= '0') && (c <= '9') ) ? c - '0' : ( (c >= 'a') && (c <= 'f') ) ? c - 'a' + 10 : ( (c >= 'A') && (c <= 'F') ) ? c - 'A' + 10 : -1;
            v = ( (d < 0) || (v < 0) ) ? -1 : v * 16 + d;
        }
        if (v < 0) { break; }
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
         // no metadata section (0 / 0) only after a section-scan recovery that didn't find one
         ( ((sb.meta_offset != 0) || (sb.meta_bytes != 0)) && !pzpd_check_section(s->map, file, sb.meta_offset, PZPD_SECT_META, sb.meta_bytes) ) ||
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
PZPD_INTERNAL struct pzpd_rshard *pzpd_shard(struct pzpd_archive *a, unsigned i)
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
PZPD_INTERNAL int pzpd_shard_of(const struct pzpd_archive *a, uint64_t ordinal)
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
PZPD_INTERNAL struct pzpd_rshard *pzpd_locate(struct pzpd_archive *a, uint64_t ordinal, uint64_t *local)
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
PZPD_INTERNAL const struct pzpd_disk_blob *pzpd_blob_entry(const struct pzpd_rshard *s, uint64_t local, unsigned stream)
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

/** @brief The metadata of a blob table entry, as the public pzpd_blob_meta. */
PZPD_INTERNAL void pzpd_blob_meta_of(const struct pzpd_disk_blob *b, pzpd_blob_meta *m)
{
    m->format     = b->format;
    m->width      = b->width;
    m->height     = b->height;
    m->channels   = b->channels;
    m->frames     = b->frames;
    m->bits       = b->bits;
    m->meta_flags = b->meta_flags;
}

/** @brief Single-archive part of pzpd_open(): ordinals, shards and stream ids are local to the archive. */
PZPD_INTERNAL struct pzpd_archive *pzpd_arch_open(const char *path, unsigned int flags)
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
        if ( (a->shards == NULL) || ((a->shards[0].path = strdup(path)) == NULL) ) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); pzpd_arch_close(a); return NULL; }
        a->shards[0].fd = -1;
        a->shards[0].record_count = 0xFFFFFFFFFFFFFFFFull;
        a->total = 0xFFFFFFFFFFFFFFFFull;                      // lets pzpd_shard_of() pick shard 0 before it is loaded
        if (pzpd_shard(a, 0) == NULL)
        {
            char msg[512];
            snprintf(msg, sizeof(msg), "%s", a->shards[0].error);
            int code = a->shards[0].error_code;
            pzpd_arch_close(a);
            if (isShard) { pzpd_set_error(code, "%s", msg); }
            else         { pzpd_set_error(PZPD_E_FORMAT, "%s is not a PZPD file (bad magic)", path); }
            return NULL;
        }
        return a;
    }

    if (memcmp(magic, PZPD_MAGIC_COLL, 8) == 0)
    {
        close(fd);
        pzpd_arch_close(a);
        pzpd_set_error(PZPD_E_FORMAT, "%s is a collection file; collections can't be members of collections", path);
        return NULL;
    }

    //----------------------------------------------------------------------
    // Manifest
    //----------------------------------------------------------------------
    struct stat st;
    if ( (fstat(fd, &st) != 0) || ((uint64_t) st.st_size < PZPD_BLOCK) ) { close(fd); pzpd_arch_close(a); pzpd_set_error(PZPD_E_FORMAT, "%s: manifest is truncated", path); return NULL; }
    a->manifest_len  = (size_t) st.st_size;
    a->mmap_manifest = (unsigned char *) mmap(NULL, a->manifest_len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (a->mmap_manifest == MAP_FAILED) { a->mmap_manifest = NULL; pzpd_arch_close(a); pzpd_set_error(PZPD_E_IO, "cannot map %s: %s", path, strerror(errno)); return NULL; }

    struct pzpd_disk_manifest mh;
    memcpy(&mh, a->mmap_manifest, sizeof(mh));
    uint64_t file = a->manifest_len;
    if (mh.version > PZPD_FORMAT_VERSION) { pzpd_arch_close(a); pzpd_set_error(PZPD_E_VERSION, "%s: manifest format version %u is newer than this library (%d)", path, mh.version, PZPD_FORMAT_VERSION); return NULL; }
    if (mh.sb_checksum != XXH64(&mh, offsetof(struct pzpd_disk_manifest, sb_checksum), 0)) { pzpd_arch_close(a); pzpd_set_error(PZPD_E_CHECKSUM, "%s: manifest header checksum mismatch", path); return NULL; }
    if ( (mh.stream_count == 0) || (mh.stream_count > PZPD_MAX_STREAMS) || (mh.shard_count == 0) ||
         !pzpd_check_section(a->mmap_manifest, file, mh.shards_offset, PZPD_SECT_MSHARDS, (uint64_t) mh.shard_count * sizeof(struct pzpd_disk_manifest_shard)) ||
         !pzpd_check_section(a->mmap_manifest, file, mh.names_offset,  PZPD_SECT_MNAMES,  mh.names_bytes) ||
         (mh.hash_count > file / sizeof(struct pzpd_disk_global_hash)) ||      // so the size below can't wrap around
         !pzpd_check_section(a->mmap_manifest, file, mh.hash_offset,   PZPD_SECT_MHASH,   mh.hash_count * sizeof(struct pzpd_disk_global_hash)) )
    {
        pzpd_arch_close(a);
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
    if (a->shards == NULL) { pzpd_arch_close(a); pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return NULL; }
    uint64_t expect = 0;
    for (unsigned i = 0; i < a->shard_count; i++)
    {
        const struct pzpd_disk_manifest_shard *ms = &a->mshards[i];
        struct pzpd_rshard *s = &a->shards[i];
        s->fd = -1;
        if ( !pzpd_in_file(ms->name_offset, ms->name_len, mh.names_bytes) || (ms->first_ordinal != expect) )
        {
            pzpd_arch_close(a);
            pzpd_set_error(PZPD_E_FORMAT, "%s: shard table is damaged", path);
            return NULL;
        }
        expect += ms->record_count;
        s->first_ordinal = ms->first_ordinal;
        s->record_count  = ms->record_count;
        s->path = (char *) malloc(dirLen + ms->name_len + 1);
        if (s->path == NULL) { pzpd_arch_close(a); pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return NULL; }
        memcpy(s->path, path, dirLen);
        memcpy(s->path + dirLen, names + ms->name_offset, ms->name_len);
        s->path[dirLen + ms->name_len] = 0;
    }
    if (expect != a->total) { pzpd_arch_close(a); pzpd_set_error(PZPD_E_FORMAT, "%s: shard record counts don't add up", path); return NULL; }
    if (!pzpd_read_table_dir(a, mh.tables, a->mmap_manifest, file, NULL, -1, 1, path)) { struct pzpd_saved_error e; pzpd_error_save(&e); pzpd_arch_close(a); pzpd_error_restore(&e); return NULL; }
    if (flags & PZPD_O_POPULATE)
    {
        for (unsigned i = 0; i < a->shard_count; i++) { (void) pzpd_shard(a, i); }   // missing shards fail later, on use
        pzpd_clear_error();
    }
    return a;
}

/** @brief Single-archive part of pzpd_close(): ordinals, shards and stream ids are local to the archive. */
PZPD_INTERNAL void pzpd_arch_close(struct pzpd_archive *a)
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
PZPD_INTERNAL int pzpd_shard_wsec(const struct pzpd_rshard *s, unsigned j, struct pzpd_wsec *v)
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
PZPD_INTERNAL int pzpd_words_slot(const struct pzpd_disk_words *dir, const char *table, const char *column)
{
    for (unsigned j = 0; (j < PZPD_MAX_WORD_INDEXES) && (dir[j].table[0] != 0); j++)
    {
        if ( !strncmp(dir[j].table, table, sizeof(dir[j].table)) && !strncmp(dir[j].column, column, sizeof(dir[j].column)) ) { return (int) j; }
    }
    return -1;
}

/** @brief Word directory entries in use. */
PZPD_INTERNAL unsigned pzpd_words_used(const struct pzpd_disk_words *dir)
{
    unsigned n = 0;
    while ( (n < PZPD_MAX_WORD_INDEXES) && (dir[n].table[0] != 0) ) { n++; }
    return n;
}

static int pzpd_cmp_went(const void *a, const void *b)
{
    const struct pzpd_went *x = (const struct pzpd_went *) a, *y = (const struct pzpd_went *) b;
    return pzpd_word_cmp(x->w, x->len, y->w, y->len);
}

PZPD_INTERNAL int pzpd_cmp_bstr(const void *a, const void *b)
{
    const struct pzpd_bstr *x = (const struct pzpd_bstr *) a, *y = (const struct pzpd_bstr *) b;
    return pzpd_word_cmp(x->s, x->len, y->s, y->len);
}

/** @brief Sort and fold entries with equal words (summing their stats). @return The number left. */
PZPD_INTERNAL size_t pzpd_went_fold(struct pzpd_went *e, size_t n)
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
PZPD_INTERNAL int pzpd_mwords_sections(struct pzpd_archive *const *shards, unsigned n, struct pzpd_buf secs[PZPD_MAX_WORD_INDEXES],
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
PZPD_INTERNAL int pzpd_arch_shard_info_get(struct pzpd_archive *a, unsigned shard, pzpd_shard_info *out)
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
PZPD_INTERNAL const struct pzpd_disk_group *pzpd_group_at(const struct pzpd_rshard *s, uint64_t local)
{
    if (s->groups == NULL) { return NULL; }
    uint64_t lo = 0, hi = s->sb.group_count;          // first entry with first_local > local, then one back
    while (lo < hi) { uint64_t mid = lo + (hi - lo) / 2; if (s->groups[mid].first_local <= local) { lo = mid + 1; } else { hi = mid; } }
    if (lo == 0) { return NULL; }
    const struct pzpd_disk_group *g = &s->groups[lo - 1];
    return (local < (uint64_t) g->first_local + g->frame_count) ? g : NULL;
}

PZPD_INTERNAL int pzpd_match(struct pzpd_archive *a, uint64_t ordinal, uint8_t stream, uint8_t kind, const char *key, size_t len)
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
PZPD_INTERNAL int64_t pzpd_arch_find(struct pzpd_archive *a, const char *key, size_t len, int *stream_out, int only_kind)
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
PZPD_INTERNAL const char *pzpd_arch_record_key(struct pzpd_archive *a, uint64_t ordinal, size_t *len)
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
PZPD_INTERNAL int pzpd_arch_blob_info_get(struct pzpd_archive *a, uint64_t ordinal, unsigned stream, pzpd_blob_info *out)
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
    pzpd_blob_meta_of(b, &out->meta);
    out->name             = s->heap + b->name_offset;
    out->name_len         = b->name_len;
    return 1;
}

//-----------------------------------------------------------------------------------------------
// Reading data
//-----------------------------------------------------------------------------------------------

/** @brief Find a blob's descriptor in its record header (for checksums), bounds-checked.
 *  @return 1 and the payload XXH32 in *xxh, 0 if the header is damaged (error set). */
PZPD_INTERNAL int pzpd_header_blob_xxh(const struct pzpd_rshard *s, uint64_t local, unsigned stream, uint32_t *xxh)
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
PZPD_INTERNAL ssize_t pzpd_arch_read_into(struct pzpd_archive *a, uint64_t ordinal, unsigned stream, void *buf, size_t cap)
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
PZPD_INTERNAL void *pzpd_arch_read_alloc(struct pzpd_archive *a, uint64_t ordinal, unsigned stream, size_t *size)
{
    pzpd_clear_error();
    if (size != NULL) { *size = 0; }
    struct pzpd_rshard *s;
    uint64_t local;
    const struct pzpd_disk_blob *b = pzpd_resolve(a, ordinal, stream, &s, &local);
    if ( (b == NULL) || (b->rel_offset == PZPD_MISSING) ) { return NULL; }
    void *buf = malloc(b->size > 0 ? b->size : 1);
    if (buf == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory for a %u byte blob", b->size); return NULL; }
    ssize_t r = pzpd_arch_read_into(a, ordinal, stream, buf, b->size);
    if (r < 0) { free(buf); return NULL; }
    if (size != NULL) { *size = (size_t) r; }
    return buf;
}

void pzpd_free(void *ptr)
{
    free(ptr);
}

/** @brief Single-archive part of pzpd_view(): ordinals, shards and stream ids are local to the archive. */
PZPD_INTERNAL const void *pzpd_arch_view(struct pzpd_archive *a, uint64_t ordinal, unsigned stream, size_t *size)
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
PZPD_INTERNAL int pzpd_span(struct pzpd_archive *a, uint64_t ordinal, uint32_t mask, struct pzpd_rshard **so, uint64_t *localOut, uint64_t *start, uint64_t *end)
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
PZPD_INTERNAL size_t pzpd_arch_record_span(struct pzpd_archive *a, uint64_t ordinal, uint32_t stream_mask)
{
    pzpd_clear_error();
    struct pzpd_rshard *s;
    uint64_t local, start, end;
    if (pzpd_span(a, ordinal, stream_mask, &s, &local, &start, &end) != 1) { return 0; }
    return (size_t)(end - start);
}

/** @brief Single-archive part of pzpd_read_record(): ordinals, shards and stream ids are local to the archive. */
PZPD_INTERNAL ssize_t pzpd_arch_read_record(struct pzpd_archive *a, uint64_t ordinal, uint32_t stream_mask, void *buf, size_t cap, pzpd_blob_ref *refs)
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
            pzpd_blob_meta_of(b, &refs[st].meta);
        }
    }
    return (ssize_t)(end - start);
}

/** @brief Single-archive part of pzpd_verify_record(): ordinals, shards and stream ids are local to the archive. */
PZPD_INTERNAL int pzpd_arch_verify_record(struct pzpd_archive *a, uint64_t ordinal, int check_blobs)
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
PZPD_INTERNAL int pzpd_arch_verify_shard(struct pzpd_archive *a, unsigned shard)
{
    pzpd_clear_error();
    if ( (a == NULL) || (shard >= a->shard_count) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    struct pzpd_rshard *s = pzpd_shard(a, shard);
    if (s == NULL) { return 0; }
    uint64_t offs[6 + PZPD_MAX_TABLES]  = { s->sb.rtab_offset, s->sb.btab_offset, s->sb.hash_offset, s->sb.heap_offset };
    uint32_t kinds[6 + PZPD_MAX_TABLES] = { PZPD_SECT_RECORDS, PZPD_SECT_BLOBS, PZPD_SECT_HASH, PZPD_SECT_HEAP };
    int nsec = 4;
    // no metadata section (0 / 0) only after a section-scan recovery that didn't find one (as pzpd_shard_load() accepts)
    if ( (s->sb.meta_offset != 0) || (s->sb.meta_bytes != 0) ) { offs[nsec] = s->sb.meta_offset; kinds[nsec] = PZPD_SECT_META; nsec++; }
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
PZPD_INTERNAL unsigned char *pzpd_arch_read_pzp(struct pzpd_archive *a, uint64_t ordinal, unsigned stream,
                             unsigned int *width, unsigned int *height,
                             unsigned int *bpp, unsigned int *channels)
{
    size_t size = 0;
    const void *data = pzpd_arch_view(a, ordinal, stream, &size);
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
PZPD_INTERNAL uint32_t pzpd_arch_table_rows(struct pzpd_archive *a, uint64_t ordinal, unsigned t, const void **rows_out)
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
PZPD_INTERNAL const char *pzpd_view_str(const struct pzpd_tview *v, const void *field, size_t *len)
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
PZPD_INTERNAL ssize_t pzpd_rows_csv(const struct pzpd_tschema *sc, const unsigned char *rows, uint32_t n, const struct pzpd_tview *v, char *out, size_t cap)
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
