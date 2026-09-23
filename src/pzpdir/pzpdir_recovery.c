/** @file pzpdir_recovery.c
 *  @brief PZPD library: recovery: rebuild-manifest, salvage.
 *  Shared types and internal declarations are in pzpdir_internal.h. */

#include "pzpdir_internal.h"

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
        ar[i] = pzpd_arch_open(shard_paths[i], 0);
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
    for (unsigned i = 0; (ar != NULL) && (i < n); i++) { if (ar[i] != NULL) { pzpd_arch_close(ar[i]); } }
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
