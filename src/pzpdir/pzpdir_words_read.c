/** @file pzpdir_words_read.c
 *  @brief PZPD library: word index handle.
 *  Shared types and internal declarations are in pzpdir_internal.h. */

#include "pzpdir_internal.h"

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
