/** @file pzpdir_words_build.c
 *  @brief PZPD library: word index: tokenizer v1, shard section builder, section views.
 *  Shared types and internal declarations are in pzpdir_internal.h. */

#include "pzpdir_internal.h"
#include "pzpdir_unicode.h"

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
    int  (*emit)(const char *, size_t, void *); ///< Called per word; non-zero stops the tokenizer
    void *user;             ///< Passed to emit
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
PZPD_INTERNAL int pzpd_word_cmp(const char *a, size_t al, const char *b, size_t bl)
{
    int c = memcmp(a, b, (al < bl) ? al : bl);
    if (c != 0) { return c; }
    return (al < bl) ? -1 : (al > bl);
}

PZPD_INTERNAL void pzpd_sdict_free(struct pzpd_sdict *d)
{
    free(d->slot); free(d->off); free(d->len); free(d->hash);
    pzpd_buf_free(&d->bytes);
    memset(d, 0, sizeof(*d));
}

/** @brief Id of a string without inserting it. @return The id, or -1 if absent. */
PZPD_INTERNAL int64_t pzpd_sdict_find(const struct pzpd_sdict *d, const char *s, size_t n)
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
PZPD_INTERNAL int64_t pzpd_sdict_id(struct pzpd_sdict *d, const char *s, size_t n)
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

/** @brief One word occurrence in a record: (word id, source id; 0 = no source). */
struct pzpd_wocc
{
    uint32_t tid;  ///< Word id (builder dictionary)
    uint32_t sid;  ///< Source id + 1, 0 = no source
};

/** @brief One (word, record, occurrences) pair of a sub-index, in record order. */
struct pzpd_wpair
{
    uint32_t tid;  ///< Word id (builder dictionary)
    uint32_t rec;  ///< Shard-local record
    uint32_t cnt;  ///< Occurrences of the word in the record
};

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

PZPD_INTERNAL int pzpd_cmp_u32(const void *a, const void *b)
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
PZPD_INTERNAL int pzpd_buf_pad8(struct pzpd_buf *b)
{
    static const unsigned char zeros[8] = {0};
    return pzpd_buf_append(b, zeros, (size_t)(pzpd_align_up(b->len, 8) - b->len));
}

/** @brief One sub-index serialized: pzpd_disk_subindex offsets relative to the start of `parts`. */
struct pzpd_wsub_out
{
    struct pzpd_disk_subindex head;  ///< Sub-index head, offsets relative to `parts`
    struct pzpd_buf parts;           ///< Vocabulary, postings, forward lists and heap, each 8-aligned
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
PZPD_INTERNAL int pzpd_words_build(struct pzpd_buf *out, const struct pzpd_wsrc *in, const char *source_column)
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

    // Sub-index order: the merged one, then the sources by bytes (after the too-many-sources error the
    // dictionary holds one more than `order` has room for)
    unsigned K = ok ? sources.n : 0;
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
struct pzpd_one_word
{
    const char *w;    ///< First word
    size_t      len;  ///< Its length
    unsigned    n;    ///< Words seen
};

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
PZPD_INTERNAL int pzpd_synonyms_check(const struct pzpd_tschema *sc, const unsigned char *rows, uint64_t n, const char *heap, uint64_t heap_bytes)
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
            if (!pzpd_in_file(f[c].offset, f[c].len, heap_bytes)) { pzpd_set_error(PZPD_E_FORMAT, "%s row %llu: a string lies outside the table's strings", PZPD_SYNONYMS_TABLE, (unsigned long long)(r + 1)); ok = 0; }
            else if (!pzpd_is_one_word(heap + f[c].offset, f[c].len))
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

/** @brief Validate a word index section's head (O(1) plus the sub-index heads).
 *  @return 1 if valid, 0 otherwise (error set). */
PZPD_INTERNAL int pzpd_wsec_parse(const unsigned char *d, uint64_t bytes, int manifest, struct pzpd_wsec *v)
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
PZPD_INTERNAL int pzpd_wsec_sub(const struct pzpd_wsec *v, unsigned k, int64_t expectRecords, struct pzpd_wsub *s)
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
PZPD_INTERNAL int pzpd_wsec_find(const struct pzpd_wsec *v, const char *source, size_t len, int64_t expectRecords)
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
PZPD_INTERNAL int pzpd_wsub_word(const struct pzpd_wsub *s, uint64_t k, const char **w, size_t *len)
{
    uint32_t off = s->vocab ? s->vocab[k].heap_offset : s->mvocab[k].heap_offset;
    uint16_t l   = s->vocab ? s->vocab[k].len         : s->mvocab[k].len;
    if (!pzpd_in_file(off, l, s->heap_bytes)) { pzpd_set_error(PZPD_E_FORMAT, "word index: word %llu is damaged", (unsigned long long) k); return 0; }
    *w = s->heap + off;
    *len = l;
    return 1;
}

/** @brief Binary search of a word in a sub-index vocabulary. @return Its id, -1 if absent, -2 if damaged (error set). */
PZPD_INTERNAL int64_t pzpd_wsub_find(const struct pzpd_wsub *s, const char *word, size_t len)
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
PZPD_INTERNAL int pzpd_wsub_check_full(const struct pzpd_wsub *s)
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
