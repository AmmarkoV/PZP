/** @file pzpdir_edit.c
 *  @brief PZPD library: edits: tables, streams, compaction.
 *  Shared types and internal declarations are in pzpdir_internal.h. */

#include "pzpdir_internal.h"

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
    struct pzpd_archive *m = pzpd_arch_open(manifest, 0);
    if (m == NULL) { return NULL; }
    if (m->standalone) { pzpd_arch_close(m); pzpd_set_error(PZPD_E_ARG, "%s is a shard; edits take the archive's manifest", manifest); return NULL; }
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

    // Every row must parse before any shard is touched. The parsed rows are kept (entry i's are rows
    // pfirst[i] .. pfirst[i + 1] of prows, strings in pheap), so the shard pass doesn't parse them again
    struct pzpd_buf grows = {0}, gheap = {0}, prows = {0}, pheap = {0};
    uint64_t *pfirst = (n > 0) ? (uint64_t *) malloc((n + 1) * sizeof(uint64_t)) : NULL;
    if ( (n > 0) && (pfirst == NULL) ) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
    uint64_t parsed = 0;
    for (size_t i = 0; ok && (op != PZPD_EDIT_DROP) && (i < n); i++)
    {
        pfirst[i] = parsed;
        int64_t got = pzpd_csv_parse(&nsc, rows[i].csv, rows[i].csv_len, &prows, &pheap);
        if (got < 0)
        {
            pzpd_error_wrap(PZPD_E_ARG, "rows of \"%.*s\" (entry %zu)", (int)(rows[i].key_len > 200 ? 200 : rows[i].key_len), rows[i].key, i + 1);
            ok = 0;
        }
        else { parsed += (uint64_t) got; }
    }
    if (pfirst != NULL) { pfirst[n] = parsed; }

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
        struct pzpd_archive *sa = pzpd_arch_open(m->shards[k].path, 0);
        if (sa == NULL) { pzpd_error_wrap(PZPD_OK, "%s", m->shards[k].path); ok = 0; break; }
        struct pzpd_rshard *s = &sa->shards[0];
        int so = -1;
        for (unsigned t = 0; t < sa->T; t++) { if (!strcmp(sa->tables[t].name, table)) { so = (int) t; } }
        // Resume: a shard already in the target state is left alone (replacing is simply redone)
        int done = ( (op == PZPD_EDIT_ADD) && (so >= 0) && pzpd_schema_equal(&sa->tables[so], &nsc) ) || ( (op == PZPD_EDIT_DROP) && (so < 0) );
        if ( !done && (op != PZPD_EDIT_ADD) && (so < 0) ) { pzpd_set_error(PZPD_E_FORMAT, "%s lacks table %s", s->path, table); ok = 0; }
        if ( !done && (op == PZPD_EDIT_ADD) && (so >= 0) ) { pzpd_set_error(PZPD_E_DUPLICATE, "%s already has a different table %s", s->path, table); ok = 0; }
        if (ok && done) { ok = pzpd_finish_flip(s); }
        if (!ok || done) { pzpd_arch_close(sa); continue; }

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
                const char *key = pzpd_arch_record_key(sa, i, &kl);
                if (key == NULL) { ok = 0; break; }
                uint64_t h = XXH64(key, kl, 0);
                int any = 0;
                for (size_t e = pzpd_ekey_find(keys, n, h); ok && (e < n) && (keys[e].hash == h); e++)
                {
                    const pzpd_edit_rows *r = &rows[keys[e].idx];
                    if ( (r->key_len != kl) || memcmp(r->key, key, kl) ) { continue; }
                    matched[keys[e].idx] = 1;
                    any = 1;
                    uint64_t got = pfirst[keys[e].idx + 1] - pfirst[keys[e].idx];
                    if (got > 0xFFFFFFFFull) { pzpd_set_error(PZPD_E_ARG, "table %s: more than 4 G rows in one shard", table); ok = 0; break; }
                    if (got > 0) { ok = pzpd_stage_rows(&nsc, prows.data + pfirst[keys[e].idx] * nsc.stride, (uint32_t) got, pheap.data, pheap.len, &trows, &theap); }
                    nrows += got;
                }
                if (ok && !any && (flags & PZPD_EDIT_KEEP_MISSING))
                {
                    const void *orows = NULL;
                    uint32_t on = pzpd_arch_table_rows(sa, i, (unsigned) so, &orows);
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
        pzpd_arch_close(sa);
    }
    if (ok) { ok = pzpd_edit_finish(manifest, m); }
    uint64_t um = 0;
    for (size_t i = 0; i < n; i++) { um += (matched != NULL) && !matched[i]; }
    if (unmatched != NULL) { *unmatched = um; }
    struct pzpd_saved_error e;
    pzpd_error_save(&e);
    pzpd_buf_free(&secbuf); pzpd_buf_free(&trows); pzpd_buf_free(&theap); pzpd_buf_free(&tindex);
    pzpd_buf_free(&grows); pzpd_buf_free(&gheap); pzpd_buf_free(&prows); pzpd_buf_free(&pheap);
    free(pfirst);
    free(keys);
    free(matched);
    pzpd_arch_close(m);
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
        struct pzpd_archive *sa = pzpd_arch_open(m->shards[k].path, 0);
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
        if (!ok || done) { pzpd_arch_close(sa); continue; }

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
        pzpd_arch_close(sa);
    }
    if (ok) { ok = pzpd_edit_finish(manifest, m); }
    struct pzpd_saved_error e;
    pzpd_error_save(&e);
    pzpd_buf_free(&wbuf);
    pzpd_arch_close(m);
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
        struct pzpd_archive *sa = pzpd_arch_open(m->shards[k].path, 0);
        if (sa == NULL) { pzpd_error_wrap(PZPD_OK, "%s", m->shards[k].path); ok = 0; break; }
        struct pzpd_rshard *s = &sa->shards[0];
        if (!pzpd_finish_flip(s)) { pzpd_arch_close(sa); ok = 0; break; }
        struct pzpd_disk_superblock sb = s->sb;
        if (s->map_len > sb.file_bytes)
        {
            // An earlier compact stopped after its last flip: cut the stale tail
            int fd = open(s->path, O_RDWR | O_CLOEXEC);
            if ( (fd < 0) || (ftruncate(fd, (off_t) sb.file_bytes) != 0) || (fsync(fd) != 0) ) { pzpd_set_error(PZPD_E_IO, "cannot trim %s: %s", s->path, strerror(errno)); ok = 0; }
            if (fd >= 0) { close(fd); }
            if (ok && (reclaimed != NULL)) { *reclaimed += s->map_len - sb.file_bytes; }
            changed = 1;
            if (!ok) { pzpd_arch_close(sa); break; }
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
        pzpd_arch_close(sa);
    }
    if (ok && changed) { ok = pzpd_edit_finish(manifest, m); }
    struct pzpd_saved_error e;
    pzpd_error_save(&e);
    pzpd_arch_close(m);
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
        const char *key = pzpd_arch_record_key(sa, i, &kl);
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
            uint32_t cnt = pzpd_arch_table_rows(sa, i, t, &rows);
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
        struct pzpd_archive *v = pzpd_arch_open(final, 0);
        ok = (v != NULL) && (v->shards[0].sb.record_count == sb.record_count) && pzpd_arch_verify_shard(v, 0);
        for (uint64_t i = 0; ok && (i < sb.record_count); i++) { ok = pzpd_arch_verify_record(v, i, 1); }
        if ( (v != NULL) && !ok && (pzpd_errorCode == PZPD_OK) ) { pzpd_set_error(PZPD_E_FORMAT, "%s: the rewritten shard failed verification", final); }
        if (v != NULL) { pzpd_arch_close(v); }
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

/** @brief Resume check of replace-stream for a shard whose generation is ahead of the manifest. Its generation was
 *  raised either by an interrupted run of this edit (the shard holds the new files) or by another interrupted edit
 *  (a table edit or compact: the shard still holds the old files), so the content decides: every input file of the
 *  shard's records is the stream's blob (same name, size and XXH32), and with PZPD_EDIT_DROP_MISSING no other record
 *  has a blob in the stream.
 *  @return 1 if the shard is already edited, 0 if it still needs the rewrite (also when a file can't be read: the
 *          rewrite then reports it). */
static int pzpd_stream_replaced(struct pzpd_archive *sa, const char *stream, const pzpd_edit_blob *blobs, size_t n, const struct pzpd_ekey *keys, unsigned flags)
{
    struct pzpd_rshard *s = &sa->shards[0];
    int st = -1;
    for (unsigned u = 0; u < sa->S; u++) { if (!strcmp(sa->streams[u], stream)) { st = (int) u; } }
    unsigned char *buf = (unsigned char *) malloc(1u << 20);
    XXH32_state_t *xs = XXH32_createState();
    int same = (st >= 0) && (buf != NULL) && (xs != NULL);
    for (uint64_t i = 0; same && (i < s->sb.record_count); i++)
    {
        size_t kl = 0;
        const char *key = pzpd_arch_record_key(sa, i, &kl);
        const struct pzpd_disk_blob *b = (key != NULL) ? pzpd_blob_entry(s, i, (unsigned) st) : NULL;
        if (b == NULL) { same = 0; break; }
        const pzpd_edit_blob *src = NULL;
        uint64_t h = XXH64(key, kl, 0);
        for (size_t e = pzpd_ekey_find(keys, n, h); (e < n) && (keys[e].hash == h); e++)
        {
            const pzpd_edit_blob *c = &blobs[keys[e].idx];
            if ( (c->key_len == kl) && !memcmp(c->key, key, kl) ) { src = c; break; }
        }
        if (src == NULL) { same = !( (flags & PZPD_EDIT_DROP_MISSING) && (b->rel_offset != PZPD_MISSING) ); continue; }
        uint32_t want = 0;
        same = (b->rel_offset != PZPD_MISSING) && (b->name_len == src->name_len) && !memcmp(s->heap + b->name_offset, src->name, src->name_len) &&
               pzpd_header_blob_xxh(s, i, (unsigned) st, &want);
        // Same name: the bytes decide (a replacement may keep the names)
        int fd = same ? open(src->path, O_RDONLY | O_CLOEXEC) : -1;
        struct stat fs;
        same = same && (fd >= 0) && (fstat(fd, &fs) == 0) && ((uint64_t) fs.st_size == b->size);
        if (same)
        {
            ssize_t r;
            XXH32_reset(xs, 0);
            while ( (r = read(fd, buf, 1u << 20)) > 0 ) { XXH32_update(xs, buf, (size_t) r); }
            same = (r == 0) && (XXH32_digest(xs) == want);
        }
        if (fd >= 0) { close(fd); }
    }
    XXH32_freeState(xs);
    free(buf);
    pzpd_clear_error();                                           // a failed check only means "not done"
    return same;
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
        struct pzpd_archive *sa = pzpd_arch_open(m->shards[k].path, 0);
        if (sa == NULL) { pzpd_error_wrap(PZPD_OK, "%s", m->shards[k].path); ok = 0; break; }
        int has = 0;
        for (unsigned u = 0; u < sa->S; u++) { if (!strcmp(sa->streams[u], stream)) { has = 1; } }
        int done = (op == PZPD_EDIT_ADD) ? has : (op == PZPD_EDIT_DROP) ? !has :
                   ( (sa->shards[0].sb.generation > m->mshards[k].generation) && pzpd_stream_replaced(sa, stream, blobs, n, keys, flags) );
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
                const char *key = pzpd_arch_record_key(sa, he->local_ordinal, &kl);
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
                const char *key = pzpd_arch_record_key(sa, i, &kl);
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
        pzpd_arch_close(sa);
    }

    // The rewrite, shard by shard
    for (unsigned k = 0; ok && (k < m->shard_count); k++)
    {
        if (!todo[k]) { continue; }
        struct pzpd_archive *sa = pzpd_arch_open(m->shards[k].path, 0);
        if (sa == NULL) { ok = 0; break; }
        ok = pzpd_rewrite_shard(sa, newS, names, map, target, blobs, n, keys, matched, flags);
        pzpd_arch_close(sa);
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
    pzpd_arch_close(m);
    if (!ok) { pzpd_error_restore(&e); }
    return ok;
}
