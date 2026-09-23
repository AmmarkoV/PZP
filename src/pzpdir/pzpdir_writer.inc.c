/** @file pzpdir_writer.inc.c
 *  @brief pzpdir.c, part 5 of 13: writer.
 *  Included by pzpdir.c in this order (one translation unit: everything stays static); not compiled on its own. */

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
