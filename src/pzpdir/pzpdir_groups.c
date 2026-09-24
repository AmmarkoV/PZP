/** @file pzpdir_groups.c
 *  @brief PZPD library: video groups.
 *  Shared types and internal declarations are in pzpdir_internal.h. */

#include "pzpdir_internal.h"

//-----------------------------------------------------------------------------------------------
// Video groups (spec §3.3, §4.6): a group's frames are consecutive records in one shard
//-----------------------------------------------------------------------------------------------

int64_t pzpd_group_find(pzpd *a, const char *name, size_t len)
{
    pzpd_clear_error();
    if ( (a == NULL) || (name == NULL) || (len == 0) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return -1; }
    for (unsigned i = 0; i < a->member_count; i++)
    {
        struct pzpd_member *mb = &a->m[i];
        if (mb->arch == NULL) { continue; }
        int64_t r = pzpd_arch_find(mb->arch, name, len, NULL, PZPD_KIND_GROUP);
        if (r >= 0) { return (int64_t) mb->first + r; }
    }
    pzpd_set_error(PZPD_E_NOTFOUND, "no group \"%.*s\"", (int)(len > 200 ? 200 : len), name);
    return -1;
}

int pzpd_group_info(pzpd *a, uint64_t ordinal, pzpd_group *out)
{
    unsigned mi; uint64_t local, sl;
    pzpd_clear_error();
    if (out == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL output"); return 0; }
    memset(out, 0, sizeof(*out));
    struct pzpd_archive *ar = pzpd_route(a, ordinal, &mi, &local);
    if (ar == NULL) { return 0; }
    struct pzpd_rshard *s = pzpd_locate(ar, local, &sl);
    if (s == NULL) { return 0; }
    const struct pzpd_disk_group *g = pzpd_group_at(s, sl);
    if (g == NULL) { return 0; }                                  // not in a group (no error)
    out->first_ordinal = a->m[mi].first + s->first_ordinal + g->first_local;
    out->frames        = g->frame_count;
    out->id            = g->group_id;
    out->index         = (uint32_t)(sl - g->first_local);
    out->name          = s->heap + g->name_offset;
    out->name_len      = g->name_len;
    return 1;
}

/** @brief Check a range of consecutive records (one member, one shard, one group or none) and find the
 *  file span covering their requested blobs. @return 1 with blobs, 0 with none, -1 on error (error set). */
static int pzpd_range(pzpd *a, uint64_t first, uint32_t count, uint32_t mask, struct pzpd_archive **arOut, unsigned *miOut,
                      struct pzpd_rshard **so, uint64_t *slFirst, uint64_t *lo, uint64_t *hi)
{
    unsigned mi, mi2; uint64_t local, local2, sl, sl2;
    if (count == 0) { pzpd_set_error(PZPD_E_ARG, "empty range"); return -1; }
    struct pzpd_archive *ar = pzpd_route(a, first, &mi, &local);
    if (ar == NULL) { return -1; }
    if ( (first + count - 1 < first) || (pzpd_route(a, first + count - 1, &mi2, &local2) == NULL) ) { return -1; }
    if (mi2 != mi) { pzpd_set_error(PZPD_E_ARG, "range %llu+%u spans two collection members", (unsigned long long) first, count); return -1; }
    struct pzpd_rshard *s = pzpd_locate(ar, local, &sl), *s2 = pzpd_locate(ar, local2, &sl2);
    if ( (s == NULL) || (s2 == NULL) ) { return -1; }
    if (s != s2) { pzpd_set_error(PZPD_E_ARG, "range %llu+%u spans two shards", (unsigned long long) first, count); return -1; }
    uint32_t mm = pzpd_member_mask(&a->m[mi], mask);
    uint32_t g = s->rtab[sl].group;
    uint64_t L = UINT64_MAX, H = 0;
    for (uint64_t k = 0; k < count; k++)
    {
        if (s->rtab[sl + k].group != g) { pzpd_set_error(PZPD_E_ARG, "range %llu+%u crosses a group boundary", (unsigned long long) first, count); return -1; }
        struct pzpd_rshard *sx;
        uint64_t lx, st, en;
        int r = pzpd_span(ar, s->first_ordinal + sl + k, mm, &sx, &lx, &st, &en);
        if (r < 0) { return -1; }
        if (r == 0) { continue; }
        uint64_t base = s->rtab[sl + k].offset;
        if (base + st < L) { L = base + st; }
        if (base + en > H) { H = base + en; }
    }
    *arOut = ar; *miOut = mi; *so = s; *slFirst = sl;
    if (L == UINT64_MAX) { return 0; }
    *lo = L; *hi = H;
    return 1;
}

size_t pzpd_range_span(pzpd *a, uint64_t first, uint32_t count, uint32_t stream_mask)
{
    struct pzpd_archive *ar; unsigned mi; struct pzpd_rshard *s; uint64_t sl, lo = 0, hi = 0;
    pzpd_clear_error();
    if (a == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL handle"); return 0; }
    return (pzpd_range(a, first, count, stream_mask, &ar, &mi, &s, &sl, &lo, &hi) == 1) ? (size_t)(hi - lo) : 0;
}

ssize_t pzpd_read_range(pzpd *a, uint64_t first, uint32_t count, uint32_t stream_mask, void *buf, size_t cap, pzpd_blob_ref *refs)
{
    struct pzpd_archive *ar; unsigned mi; struct pzpd_rshard *s; uint64_t sl, lo = 0, hi = 0;
    pzpd_clear_error();
    if (a == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL handle"); return PZPD_E_ARG; }
    if (refs != NULL) { memset(refs, 0, sizeof(pzpd_blob_ref) * (size_t) count * a->S); }
    int r = pzpd_range(a, first, count, stream_mask, &ar, &mi, &s, &sl, &lo, &hi);
    if (r < 0) { return (ssize_t) pzpd_errorCode; }
    if (r == 0) { return 0; }
    if ( (buf == NULL) || (cap < hi - lo) ) { pzpd_set_error(PZPD_E_ARG, "buffer of %zu bytes is too small for a %llu byte range", cap, (unsigned long long)(hi - lo)); return PZPD_E_ARG; }
    if (!pzpd_pread_all(s->fd, buf, (size_t)(hi - lo), lo)) { return (ssize_t) pzpd_errorCode; }
    struct pzpd_member *mb = &a->m[mi];
    for (uint64_t k = 0; (refs != NULL) && (k < count); k++)
    {
        for (unsigned u = 0; u < a->S; u++)
        {
            int ms = mb->to_member[u];
            if ( !(stream_mask & (1u << u)) || (ms < 0) ) { continue; }
            const struct pzpd_disk_blob *b = pzpd_blob_entry(s, sl + k, (unsigned) ms);
            if (b == NULL) { return (ssize_t) pzpd_errorCode; }
            if (b->rel_offset == PZPD_MISSING) { continue; }
            pzpd_blob_ref *rf = &refs[k * a->S + u];
            rf->data   = (const unsigned char *) buf + (s->rtab[sl + k].offset + b->rel_offset - lo);
            rf->size   = b->size;
            rf->format = b->format;
            pzpd_blob_meta_of(b, &rf->meta);
            if (ar->flags & PZPD_O_VERIFY)
            {
                uint32_t want;
                if ( !pzpd_header_blob_xxh(s, sl + k, (unsigned) ms, &want) || (XXH32(rf->data, rf->size, 0) != want) )
                {
                    if (pzpd_errorCode == PZPD_OK) { pzpd_set_error(PZPD_E_CHECKSUM, "%s: checksum mismatch in record %llu stream %u", s->path, (unsigned long long)(sl + k), (unsigned) ms); }
                    return (ssize_t) pzpd_errorCode;
                }
            }
        }
    }
    return (ssize_t)(hi - lo);
}
