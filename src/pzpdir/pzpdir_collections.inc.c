/** @file pzpdir_collections.inc.c
 *  @brief pzpdir.c, part 9 of 13: collection files, storage kind.
 *  Included by pzpdir.c in this order (one translation unit: everything stays static); not compiled on its own. */

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
