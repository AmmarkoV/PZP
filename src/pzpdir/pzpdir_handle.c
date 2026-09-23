/** @file pzpdir_handle.c
 *  @brief PZPD library: collections behind one handle: routing, tables through the handle.
 *  Shared types and internal declarations are in pzpdir_internal.h. */

#include "pzpdir_internal.h"

//-----------------------------------------------------------------------------------------------
// Collections: one or several archives behind one handle. Every public read call goes through
// here: an ordinal is routed to its member archive, stream ids are mapped between the merged
// ("union") stream list and the member's own, and the pzpd_arch_* functions above do the work.
//-----------------------------------------------------------------------------------------------

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
        pzpd_arch_close(a->m[i].arch);
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
        mb->arch = pzpd_arch_open(paths[i], flags & ~PZPD_O_ALLOW_MISSING);   // VERIFY, HUGEPAGE, POPULATE act per archive
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

/** @brief Read and validate a collection file.
 *  @return 1 on success, 0 on failure (error set; c->buf freed). */
PZPD_INTERNAL int pzpd_coll_parse(const char *path, struct pzpd_coll_file *c)
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
PZPD_INTERNAL char *pzpd_heap_str(const char *heap, uint32_t off, uint32_t len)
{
    char *r = (char *) malloc((size_t) len + 1);
    if (r != NULL) { memcpy(r, heap + off, len); r[len] = 0; }
    return r;
}

/** @brief Resolve a stored member path: absolute as is, relative to the collection file's directory. */
PZPD_INTERNAL char *pzpd_resolve_member_path(const char *collPath, const char *stored)
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
        mb->arch = pzpd_arch_open(mb->path, flags & ~PZPD_O_ALLOW_MISSING);
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
PZPD_INTERNAL struct pzpd_archive *pzpd_route(pzpd *a, uint64_t ordinal, unsigned *member, uint64_t *local)
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
PZPD_INTERNAL uint32_t pzpd_member_mask(const struct pzpd_member *mb, uint32_t mask)
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
        if (!pzpd_arch_shard_info_get(mb->arch, shard - mb->shard_base, out)) { return 0; }
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
            int64_t r = pzpd_arch_find(mb->arch, key, len, &ms, kind);
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
    int64_t r = pzpd_arch_find(mb->arch, key, len, &ms, -1);
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
            int64_t r = pzpd_arch_find(mb->arch, key, len, &ms, kind);   // keys and names are unique within an archive
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
    return (ar == NULL) ? NULL : pzpd_arch_record_key(ar, local, len);
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
    else if (!pzpd_arch_blob_info_get(ar, local, (unsigned) ms, out)) { return 0; }
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
    return (ms < 0) ? 0 : pzpd_arch_read_into(ar, local, (unsigned) ms, buf, cap);
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
    return (ms < 0) ? NULL : pzpd_arch_read_alloc(ar, local, (unsigned) ms, size);
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
    return (ms < 0) ? NULL : pzpd_arch_view(ar, local, (unsigned) ms, size);
}

size_t pzpd_record_span(pzpd *a, uint64_t ordinal, uint32_t stream_mask)
{
    unsigned mi; uint64_t local;
    pzpd_clear_error();
    struct pzpd_archive *ar = pzpd_route(a, ordinal, &mi, &local);
    if (ar == NULL) { return 0; }
    return pzpd_arch_record_span(ar, local, pzpd_member_mask(&a->m[mi], stream_mask));
}

ssize_t pzpd_read_record(pzpd *a, uint64_t ordinal, uint32_t stream_mask, void *buf, size_t cap, pzpd_blob_ref *refs)
{
    unsigned mi; uint64_t local;
    pzpd_clear_error();
    struct pzpd_archive *ar = pzpd_route(a, ordinal, &mi, &local);
    if (ar == NULL) { return (ssize_t) pzpd_errorCode; }
    struct pzpd_member *mb = &a->m[mi];
    pzpd_blob_ref mine[PZPD_MAX_STREAMS];
    ssize_t r = pzpd_arch_read_record(ar, local, pzpd_member_mask(mb, stream_mask), buf, cap, mine);
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
    return (ar == NULL) ? 0 : pzpd_arch_verify_record(ar, local, check_blobs);
}

int pzpd_verify_shard(pzpd *a, unsigned shard)
{
    pzpd_clear_error();
    if ( (a == NULL) || (shard >= a->shard_total) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    for (unsigned i = 0; i < a->member_count; i++)
    {
        struct pzpd_member *mb = &a->m[i];
        if ( (mb->arch != NULL) && (shard >= mb->shard_base) && (shard < mb->shard_base + mb->shards) ) { return pzpd_arch_verify_shard(mb->arch, shard - mb->shard_base); }
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
    return pzpd_arch_read_pzp(ar, local, (unsigned) ms, width, height, bpp, channels);
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
    return pzpd_arch_table_rows(ar, local, (unsigned) mt, rows_out);
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
    uint32_t n = pzpd_arch_table_rows(ar, local, (unsigned) mt, &rows);
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
