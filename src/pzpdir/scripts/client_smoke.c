/** @file client_smoke.c
 *  @brief First-client smoke test (PLAN.md §5): use pzpdir the way the DataLoader adapter will.
 *
 *  1. Open an archive or a collection (several members), like the DataLoader's list of dataset sources.
 *  2. Build a PoseDatabase-like array from the tables only (no .db, no descriptor files):
 *     `joints` (global, per member), `image`, `persons` (u16 bbox + keypoints), `descriptions`,
 *     `descriptor_*` (bulk). Tables are bulk-loaded per shard with pzpd_table_shard_view(), as the
 *     adapter will at startup, then every record is cross-checked with pzpd_table_rows().
 *     Members without a table simply give samples without those annotations.
 *  3. Submit a shuffled epoch (fixed seed) to a prefetcher (AUTO mode) and consume it from several
 *     threads with strided positions (thread t takes positions t, t+T, ...), as the DataLoader's
 *     workers do: pzpd_prefetch_get() → use the blobs → pzpd_prefetch_release(). The same threads
 *     also query tables. Every blob is checked against its index entry, every 16th record against
 *     its payload checksums.
 *
 *  Build under ASan / UBSan (make client_smoke) and run:
 *      ./client_smoke <archive-or-collection...> [--threads N] [--mode auto|map|pagecache|buffers]
 *  Exit status 0 = everything consistent.
 *
 *  Repository : https://github.com/AmmarkoV/PZP
 *  Author     : Ammar Qammaz (AmmarkoV)
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../pzpdir.h"

#define MAX_JOINTS 64          ///< Joints kept per member (the DataLoader uses MAX_KEYPOINT_NUMBER + 1)
#define MAX_JOINT_NAME 64      ///< As the DataLoader's MAX_JOINT_NAME

/** @brief One person, as the DataLoader's struct Skeleton (coordinates are u16). */
struct Skeleton
{
    unsigned short id;                        ///< Person id
    unsigned short bbox[4];                   ///< x, y, w, h
    unsigned short coords[MAX_JOINTS * 3];    ///< Keypoint triplets
};

/** @brief One sample, as the DataLoader's struct PoseEntry (only the fields the tables fill). */
struct PoseEntry
{
    unsigned short   width, height;           ///< From `image`
    unsigned short   numberOfSkeletons;       ///< Rows of `persons`
    struct Skeleton *sk;                      ///< Copied rows of `persons`
    unsigned         numberOfDescriptions;    ///< Rows of `descriptions`
    const float     *descriptor;              ///< Zero-copy row of the first `descriptor_*` table, NULL if absent
};

/** @brief One joint, as the DataLoader's struct Joint. */
struct Joint
{
    char           name[MAX_JOINT_NAME + 1];  ///< Joint name
    unsigned short parent;                    ///< Parent joint
};

/** @brief The PoseDatabase-like result. */
struct PoseDatabase
{
    uint64_t          numberOfSamples;        ///< Records over all members
    struct PoseEntry *sample;                 ///< One per record
    struct Joint     *joint[PZPD_MAX_MEMBERS];///< Joints of each member (NULL if the member has none)
    unsigned          joints[PZPD_MAX_MEMBERS];///< Joint count of each member
    unsigned          descriptorD;            ///< Descriptor length (0 = no descriptor table)
};

/** @brief Column ids and offsets the adapter needs, resolved once from the schemas. */
struct Layout
{
    int      joints, image, persons, descriptions, descriptor;  ///< Table ids (-1 = absent)
    uint32_t jName, jParent, iW, iH, pId, pBbox, pKp, dText;   ///< Column offsets
    unsigned kpCount;                                           ///< Values in persons.kp (3 × joints)
};

static int failures = 0;  ///< Inconsistencies found

/** @brief Report an inconsistency. */
#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED); } while (0)

/** @brief Find a column by name with the expected type; returns its offset or (uint32_t)-1. */
static uint32_t column(const pzpd_schema *sc, const char *name, int type, unsigned *count)
{
    for (unsigned c = 0; c < sc->ncols; c++)
    {
        if ( (strcmp(sc->cols[c].name, name) == 0) && (sc->cols[c].type == type) )
        {
            if (count != NULL) { *count = sc->cols[c].count; }
            return sc->cols[c].offset;
        }
    }
    FAIL("table %s: no column %s of type %d", sc->name, name, type);
    return (uint32_t) -1;
}

/** @brief Resolve the tables and columns the adapter uses. */
static void resolve(pzpd *a, struct Layout *L, struct PoseDatabase *db)
{
    memset(L, 0, sizeof(*L));
    L->joints       = pzpd_table_id(a, "joints");
    L->image        = pzpd_table_id(a, "image");
    L->persons      = pzpd_table_id(a, "persons");
    L->descriptions = pzpd_table_id(a, "descriptions");
    L->descriptor   = -1;
    for (unsigned t = 0; t < pzpd_table_count(a); t++)
    {
        if (strncmp(pzpd_table_schema(a, t)->name, "descriptor_", 11) == 0) { L->descriptor = (int) t; break; }
    }
    if (L->joints >= 0)
    {
        const pzpd_schema *sc = pzpd_table_schema(a, (unsigned) L->joints);
        L->jName   = column(sc, "name", PZPD_TYPE_STR, NULL);
        L->jParent = column(sc, "parent", PZPD_TYPE_U16, NULL);
    }
    if (L->image >= 0)
    {
        const pzpd_schema *sc = pzpd_table_schema(a, (unsigned) L->image);
        L->iW = column(sc, "width", PZPD_TYPE_U16, NULL);
        L->iH = column(sc, "height", PZPD_TYPE_U16, NULL);
    }
    if (L->persons >= 0)
    {
        const pzpd_schema *sc = pzpd_table_schema(a, (unsigned) L->persons);
        unsigned bb = 0;
        L->pId   = column(sc, "id", PZPD_TYPE_U16, NULL);
        L->pBbox = column(sc, "bbox", PZPD_TYPE_U16, &bb);
        L->pKp   = column(sc, "kp", PZPD_TYPE_U16, &L->kpCount);
        if ( (bb != 4) || (L->kpCount > MAX_JOINTS * 3) || (L->kpCount % 3 != 0) ) { FAIL("persons: bbox[%u] kp[%u] not supported", bb, L->kpCount); L->persons = -1; }
    }
    if (L->descriptions >= 0) { L->dText = column(pzpd_table_schema(a, (unsigned) L->descriptions), "text", PZPD_TYPE_STR, NULL); }
    if (L->descriptor >= 0)
    {
        const pzpd_schema *sc = pzpd_table_schema(a, (unsigned) L->descriptor);
        if ( (sc->ncols != 1) || (sc->cols[0].type != PZPD_TYPE_F32) ) { FAIL("%s: expected one f32 array column", sc->name); L->descriptor = -1; }
        else { db->descriptorD = sc->cols[0].count; }
    }
}

/** @brief Fill one sample from its rows (image row, persons rows, descriptions count, descriptor row). */
static void fill_sample(struct PoseEntry *e, const struct Layout *L,
                        const unsigned char *img, uint32_t nPersons, const unsigned char *persons, uint32_t personStride,
                        uint32_t nDesc, const void *descriptor)
{
    if (img != NULL) { memcpy(&e->width, img + L->iW, 2); memcpy(&e->height, img + L->iH, 2); }
    e->numberOfSkeletons = (unsigned short) nPersons;
    e->sk = (nPersons > 0) ? (struct Skeleton *) calloc(nPersons, sizeof(struct Skeleton)) : NULL;
    for (uint32_t k = 0; k < nPersons; k++)
    {
        const unsigned char *r = persons + (size_t) k * personStride;
        memcpy(&e->sk[k].id, r + L->pId, 2);
        memcpy(e->sk[k].bbox, r + L->pBbox, 8);
        memcpy(e->sk[k].coords, r + L->pKp, 2 * L->kpCount);
    }
    e->numberOfDescriptions = nDesc;
    e->descriptor = (const float *) descriptor;
}

/** @brief Build the database from per-shard table views (the startup bulk path). */
static struct PoseDatabase *build(pzpd *a, struct Layout *L)
{
    struct PoseDatabase *db = (struct PoseDatabase *) calloc(1, sizeof(*db));
    resolve(a, L, db);
    db->numberOfSamples = pzpd_count(a);
    db->sample = (struct PoseEntry *) calloc(db->numberOfSamples ? db->numberOfSamples : 1, sizeof(struct PoseEntry));

    for (unsigned m = 0; (L->joints >= 0) && (m < pzpd_member_count(a)); m++)
    {
        const void *rows = NULL;
        uint32_t n = pzpd_global_rows(a, m, (unsigned) L->joints, &rows);
        if (n == 0) { continue; }
        uint32_t stride = pzpd_table_schema(a, (unsigned) L->joints)->row_stride;
        db->joint[m] = (struct Joint *) calloc(n, sizeof(struct Joint));
        db->joints[m] = n;
        for (uint32_t j = 0; j < n; j++)
        {
            const unsigned char *r = (const unsigned char *) rows + (size_t) j * stride;
            size_t len = 0;
            const char *s = pzpd_global_str(a, m, (unsigned) L->joints, r + L->jName, &len);
            if (s == NULL) { FAIL("member %u joint %u: %s", m, j, pzpd_last_error()); continue; }
            if (len > MAX_JOINT_NAME) { len = MAX_JOINT_NAME; }
            memcpy(db->joint[m][j].name, s, len);
            memcpy(&db->joint[m][j].parent, r + L->jParent, 2);
            if (db->joint[m][j].parent >= n) { FAIL("member %u joint %u: parent %u out of range", m, j, db->joint[m][j].parent); }
        }
        if ( (L->persons >= 0) && (L->kpCount != 3 * n) ) { FAIL("member %u: %u joints but persons.kp has %u values", m, n, L->kpCount); }
    }

    int tabs[4] = { L->image, L->persons, L->descriptions, L->descriptor };
    for (unsigned sh = 0; sh < pzpd_shard_count(a); sh++)
    {
        pzpd_table_view v[4];
        memset(v, 0, sizeof(v));
        for (int k = 0; k < 4; k++)
        {
            if ( (tabs[k] >= 0) && !pzpd_table_shard_view(a, sh, (unsigned) tabs[k], &v[k]) ) { FAIL("shard %u table %d: %s", sh, tabs[k], pzpd_last_error()); }
        }
        pzpd_shard_info si;
        if (!pzpd_shard_info_get(a, sh, &si)) { FAIL("shard %u: %s", sh, pzpd_last_error()); continue; }
        for (uint64_t i = 0; i < si.record_count; i++)
        {
            const unsigned char *rowp[4] = { NULL, NULL, NULL, NULL };
            uint32_t n[4] = { 0, 0, 0, 0 };
            for (int k = 0; k < 4; k++)
            {
                if (v[k].row_index == NULL) { continue; }
                n[k] = v[k].row_index[i + 1] - v[k].row_index[i];
                rowp[k] = (const unsigned char *) v[k].rows + (size_t) v[k].row_index[i] * pzpd_table_schema(a, (unsigned) tabs[k])->row_stride;
            }
            if (n[0] > 1) { FAIL("record %llu: %u image rows", (unsigned long long)(si.first_ordinal + i), n[0]); }
            fill_sample(&db->sample[si.first_ordinal + i], L, n[0] ? rowp[0] : NULL, n[1], rowp[1],
                        (L->persons >= 0) ? pzpd_table_schema(a, (unsigned) L->persons)->row_stride : 0, n[2], n[3] ? rowp[3] : NULL);
        }
    }
    return db;
}

/** @brief Check one sample of the bulk-built database against the per-record table API. */
static void check_sample(pzpd *a, const struct Layout *L, const struct PoseDatabase *db, uint64_t i)
{
    const struct PoseEntry *e = &db->sample[i];
    const void *rows = NULL;
    if (L->image >= 0)
    {
        uint32_t n = pzpd_table_rows(a, i, (unsigned) L->image, &rows);
        unsigned short w = 0, h = 0;
        if (n == 1) { memcpy(&w, (const char *) rows + L->iW, 2); memcpy(&h, (const char *) rows + L->iH, 2); }
        if ( (w != e->width) || (h != e->height) ) { FAIL("record %llu: image %ux%u vs %ux%u", (unsigned long long) i, w, h, e->width, e->height); }
    }
    if (L->persons >= 0)
    {
        uint32_t n = pzpd_table_rows(a, i, (unsigned) L->persons, &rows);
        uint32_t stride = pzpd_table_schema(a, (unsigned) L->persons)->row_stride;
        if (n != e->numberOfSkeletons) { FAIL("record %llu: %u persons vs %u", (unsigned long long) i, n, e->numberOfSkeletons); return; }
        for (uint32_t k = 0; k < n; k++)
        {
            if (memcmp((const char *) rows + (size_t) k * stride + L->pKp, e->sk[k].coords, 2 * L->kpCount) != 0) { FAIL("record %llu person %u: keypoints differ", (unsigned long long) i, k); }
        }
    }
    if (L->descriptions >= 0)
    {
        uint32_t n = pzpd_table_rows(a, i, (unsigned) L->descriptions, &rows);
        uint32_t stride = pzpd_table_schema(a, (unsigned) L->descriptions)->row_stride;
        if (n != e->numberOfDescriptions) { FAIL("record %llu: %u descriptions vs %u", (unsigned long long) i, n, e->numberOfDescriptions); }
        for (uint32_t k = 0; k < n; k++)
        {
            size_t len = 0;
            const char *s = pzpd_table_str(a, i, (unsigned) L->descriptions, (const char *) rows + (size_t) k * stride + L->dText, &len);
            if ( (s == NULL) || (memchr(s, 0, len) != NULL) ) { FAIL("record %llu description %u: %s", (unsigned long long) i, k, s ? "contains NUL" : pzpd_last_error()); }
        }
    }
    if (L->descriptor >= 0)
    {
        uint32_t n = pzpd_table_rows(a, i, (unsigned) L->descriptor, &rows);
        if ( (n ? rows : NULL) != (const void *) e->descriptor ) { FAIL("record %llu: descriptor pointer differs", (unsigned long long) i); }
    }
}

/** @brief Per-thread work. */
struct Worker
{
    pthread_t                  th;        ///< Thread
    pzpd                      *a;         ///< Shared handle
    pzpd_prefetcher           *pf;        ///< Shared prefetcher
    const uint64_t            *order;     ///< The epoch order submitted to pf
    const struct Layout       *L;         ///< Resolved layout
    const struct PoseDatabase *db;        ///< Built database
    unsigned                   t, T;      ///< This thread, thread count
    uint64_t                   bytes;     ///< Payload bytes read
    uint64_t                   blobs;     ///< Blobs read
};

/** @brief Thread body: strided positions of the epoch through the prefetcher, plus concurrent table checks. */
static void *worker(void *arg)
{
    struct Worker *W = (struct Worker *) arg;
    unsigned S = pzpd_stream_count(W->a);
    uint32_t mask = (S >= 32) ? 0xFFFFFFFFu : ((1u << S) - 1);
    pzpd_blob_ref refs[PZPD_MAX_STREAMS];
    for (uint64_t pos = W->t; pos < W->db->numberOfSamples; pos += W->T)
    {
        uint64_t i = W->order[pos];
        pzpd_ticket tk;
        int n = pzpd_prefetch_get(W->pf, i, mask, refs, &tk);
        if (n < 0) { FAIL("record %llu: %s", (unsigned long long) i, pzpd_last_error()); continue; }
        for (unsigned s = 0; s < S; s++)
        {
            pzpd_blob_info bi;
            if (!pzpd_blob_info_get(W->a, i, s, &bi)) { FAIL("record %llu stream %u: %s", (unsigned long long) i, s, pzpd_last_error()); continue; }
            if ( (bi.present != (refs[s].data != NULL)) || (bi.present && ((refs[s].size != bi.size) || (refs[s].format != bi.meta.format))) )
                { FAIL("record %llu stream %u: prefetch_get disagrees with blob_info", (unsigned long long) i, s); }
            if (bi.present) { W->bytes += bi.size; W->blobs++; }
            if ( bi.present && (i % 16 == 0) )                     // the view holds the same bytes as a pread() copy
            {
                size_t sz = 0;
                void *copy = pzpd_read_alloc(W->a, i, s, &sz);
                if ( (sz != refs[s].size) || ((sz > 0) && ((copy == NULL) || memcmp(copy, refs[s].data, sz))) ) { FAIL("record %llu stream %u: view differs from pread copy", (unsigned long long) i, s); }
                pzpd_free(copy);
            }
        }
        pzpd_prefetch_release(W->pf, &tk);
        if ( (i % 16 == 0) && !pzpd_verify_record(W->a, i, 1) ) { FAIL("record %llu: %s", (unsigned long long) i, pzpd_last_error()); }
        check_sample(W->a, W->L, W->db, i);
    }
    return NULL;
}

/** @brief Seconds of a monotonic clock. */
static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    const char *paths[64];
    unsigned np = 0, T = 8, mode = PZPD_PF_AUTO;
    for (int i = 1; i < argc; i++)
    {
        if ( (strcmp(argv[i], "--threads") == 0) && (i + 1 < argc) ) { T = (unsigned) atoi(argv[++i]); }
        else if ( (strcmp(argv[i], "--mode") == 0) && (i + 1 < argc) )
        {
            const char *m = argv[++i];
            mode = !strcmp(m, "map") ? PZPD_PF_MAP : !strcmp(m, "pagecache") ? PZPD_PF_PAGECACHE : !strcmp(m, "buffers") ? PZPD_PF_BUFFERS : PZPD_PF_AUTO;
        }
        else if (np < 64) { paths[np++] = argv[i]; }
    }
    if ( (np == 0) || (T == 0) || (T > 256) ) { fprintf(stderr, "usage: %s <archive-or-collection...> [--threads N]\n", argv[0]); return 2; }

    double t0 = now();
    pzpd *a = (np == 1) ? pzpd_open(paths[0], 0) : pzpd_open_many(paths, NULL, np, 0);
    if (a == NULL) { fprintf(stderr, "open: %s\n", pzpd_last_error()); return 1; }
    double t1 = now();
    struct Layout L;
    struct PoseDatabase *db = build(a, &L);
    double t2 = now();

    uint64_t persons = 0, described = 0, withDescriptor = 0, withImage = 0;
    for (uint64_t i = 0; i < db->numberOfSamples; i++)
    {
        persons += db->sample[i].numberOfSkeletons;
        described += (db->sample[i].numberOfDescriptions > 0);
        withDescriptor += (db->sample[i].descriptor != NULL);
        withImage += (db->sample[i].width > 0);
    }
    printf("open %.2f ms, PoseDatabase from tables %.1f ms\n", (t1 - t0) * 1e3, (t2 - t1) * 1e3);
    printf("%u member(s), %llu samples: %llu with image size, %llu persons, %llu described, %llu with a %u-D descriptor\n",
           pzpd_member_count(a), (unsigned long long) db->numberOfSamples, (unsigned long long) withImage,
           (unsigned long long) persons, (unsigned long long) described, (unsigned long long) withDescriptor, db->descriptorD);
    for (unsigned m = 0; m < pzpd_member_count(a); m++)
    {
        printf("  member %u %-12s %u joints%s%s\n", m, pzpd_member_alias(a, m), db->joints[m],
               db->joints[m] ? ", first: " : "", db->joints[m] ? db->joint[m][0].name : "");
    }

    // A shuffled epoch (Fisher-Yates, fixed seed), submitted whole
    uint64_t *order = (uint64_t *) malloc((db->numberOfSamples ? db->numberOfSamples : 1) * sizeof(uint64_t));
    for (uint64_t i = 0; i < db->numberOfSamples; i++) { order[i] = i; }
    uint64_t x = 88172645463325252ull;
    for (uint64_t i = db->numberOfSamples; i > 1; i--)
    {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        uint64_t j = x % i, tmp = order[i - 1];
        order[i - 1] = order[j];
        order[j] = tmp;
    }
    pzpd_prefetch_opts po = { 0, 0, mode, 0, 0 };
    pzpd_prefetcher *pf = pzpd_prefetcher_create(a, &po);
    if ( (pf == NULL) || !pzpd_prefetch_submit(pf, order, NULL, db->numberOfSamples) ) { fprintf(stderr, "prefetcher: %s\n", pzpd_last_error()); return 1; }

    struct Worker *W = (struct Worker *) calloc(T, sizeof(struct Worker));
    for (unsigned t = 0; t < T; t++)
    {
        W[t].a = a; W[t].pf = pf; W[t].order = order; W[t].L = &L; W[t].db = db; W[t].t = t; W[t].T = T;
        pthread_create(&W[t].th, NULL, worker, &W[t]);
    }
    uint64_t bytes = 0, blobs = 0;
    for (unsigned t = 0; t < T; t++) { pthread_join(W[t].th, NULL); bytes += W[t].bytes; blobs += W[t].blobs; }
    double t3 = now();
    pzpd_prefetch_stats st;
    pzpd_prefetch_stats_get(pf, &st);
    printf("%u threads got %llu blobs, %.1f MB, via the prefetcher and re-checked every sample's tables in %.2f s\n",
           T, (unsigned long long) blobs, (double) bytes / 1e6, t3 - t2);
    printf("prefetcher: shards map %u / pagecache %u / buffers %u; %llu prefetched, %llu hits, %llu waits, %llu sync misses, %llu released, %llu stalls\n",
           st.shards_map, st.shards_pagecache, st.shards_buffers, (unsigned long long) st.prefetched, (unsigned long long) st.hits, (unsigned long long) st.waits,
           (unsigned long long) st.sync_misses, (unsigned long long) st.released, (unsigned long long) st.producer_stalls);
    if ( (st.released != db->numberOfSamples) || (st.hits + st.waits + st.sync_misses != db->numberOfSamples) || (st.unscheduled != 0) || (st.buffer_bytes != 0) ) { FAIL("prefetcher counters don't add up"); }
    pzpd_prefetcher_destroy(pf);
    free(order);

    for (uint64_t i = 0; i < db->numberOfSamples; i++) { free(db->sample[i].sk); }
    for (unsigned m = 0; m < PZPD_MAX_MEMBERS; m++) { free(db->joint[m]); }
    free(db->sample);
    free(db);
    free(W);
    pzpd_close(a);
    if (failures) { printf("client smoke: %d problem(s)\n", failures); return 1; }
    printf("client smoke: OK\n");
    return 0;
}
