/** @file fuzz_pzpdir.c
 *  @brief  Truncation / bit-flip fuzzing of shards and manifests (phase 1 gate).
 *
 *  Builds a small multi-shard archive, a second archive and a collection of both, then
 *  repeatedly damages one file (the collection, the manifest or a shard: random
 *  truncation, or random bit flips biased toward the superblocks and index sections) and runs
 *  every read API over the result. Pass = no crash and no sanitizer report (build: `make fuzz`,
 *  ASan + UBSan). Errors returned by the API are expected and fine.
 *  Usage: fuzz_pzpdir [iterations] (default 3000). Scratch: $PZPDIR_TEST_DIR/fuzz.
 *
 *  Repository : https://github.com/AmmarkoV/PZP
 *  @author Ammar Qammaz (AmmarkoV)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../pzpdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static char dir[1024];   ///< Scratch directory

/** @brief Small deterministic PRNG. */
static uint64_t rng(void)
{
    static uint64_t x = 0x243F6A8885A308D3ull;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return x;
}

/** @brief Read a whole file. */
static unsigned char *slurp(const char *p, size_t *n)
{
    int fd = open(p, O_RDONLY);
    struct stat st;
    if ( (fd < 0) || (fstat(fd, &st) != 0) ) { return NULL; }
    unsigned char *b = (unsigned char *) malloc((size_t) st.st_size + 1);
    size_t got = 0;
    while (got < (size_t) st.st_size) { ssize_t r = read(fd, b + got, (size_t) st.st_size - got); if (r <= 0) { break; } got += (size_t) r; }
    close(fd);
    *n = got;
    return b;
}

/** @brief Write a whole file. */
static void spit(const char *p, const unsigned char *b, size_t n)
{
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { return; }
    size_t put = 0;
    while (put < n) { ssize_t w = write(fd, b + put, n - put); if (w <= 0) { break; } put += (size_t) w; }
    close(fd);
}

/** @brief Run every read API over an archive; results are ignored, only crashes matter. */
/** @brief pzpd_salvage() callback: touch every reported byte range. */
static int salvage_touch(const pzpd_salvaged_record *r, void *user)
{
    (void) user;
    volatile unsigned char c = 0;
    if (r->key_len > 0) { c ^= (unsigned char) r->key[r->key_len - 1]; }
    for (unsigned i = 0; i < r->blob_count; i++)
    {
        if (r->blobs[i].name_len > 0) { c ^= (unsigned char) r->blobs[i].name[r->blobs[i].name_len - 1]; }
        if (r->blobs[i].size > 0) { c ^= ((const unsigned char *) r->blobs[i].data)[r->blobs[i].size - 1]; }
    }
    for (unsigned t = 0; t < r->table_count; t++)
    {
        if (r->tables[t].row_bytes > 0) { c ^= ((const unsigned char *) r->tables[t].row_data)[r->tables[t].row_bytes - 1]; }
        if (r->tables[t].strings_len > 0) { c ^= (unsigned char) r->tables[t].strings[r->tables[t].strings_len - 1]; }
        if (r->tables[t].csv != NULL) { c ^= (unsigned char) r->tables[t].csv[r->tables[t].csv_len]; }
    }
    (void) c;
    return 1;
}

static void exercise(const char *path)
{
    pzpd *a = pzpd_open(path, (rng() & 1) ? PZPD_O_VERIFY : 0);
    if (a == NULL) { return; }
    uint64_t n = pzpd_count(a);
    unsigned S = pzpd_stream_count(a);
    static unsigned char buf[1 << 16];
    for (unsigned s = 0; s < pzpd_shard_count(a); s++)
    {
        pzpd_shard_info si;
        pzpd_shard_info_get(a, s, &si);
        pzpd_verify_shard(a, s);
    }
    uint64_t lim = (n > 400) ? 400 : n;
    for (uint64_t i = 0; i < lim; i++)
    {
        size_t kl = 0;
        const char *k = pzpd_record_key(a, i, &kl);
        if (k != NULL) { volatile char c = k[kl ? kl - 1 : 0]; (void) c; int so; pzpd_find(a, k, kl ? kl : 1, &so); }
        for (unsigned s = 0; s < S; s++)
        {
            pzpd_blob_info bi;
            if (pzpd_blob_info_get(a, i, s, &bi) && bi.present && (bi.name_len > 0))
            {
                volatile char c = bi.name[bi.name_len - 1]; (void) c;
                int so;
                pzpd_find(a, bi.name, bi.name_len, &so);
            }
            pzpd_read_into(a, i, s, buf, sizeof(buf));
            size_t vs = 0;
            const unsigned char *v = (const unsigned char *) pzpd_view(a, i, s, &vs);
            if ( (v != NULL) && (vs > 0) ) { volatile unsigned char c = v[0] ^ v[vs - 1]; (void) c; }
            void *al = pzpd_read_alloc(a, i, s, &vs);
            pzpd_free(al);
        }
        pzpd_blob_ref refs[PZPD_MAX_STREAMS];
        size_t span = pzpd_record_span(a, i, 0xFFFFFFFFu);
        if (span <= sizeof(buf)) { pzpd_read_record(a, i, 0xFFFFFFFFu, buf, sizeof(buf), refs); }
        pzpd_verify_record(a, i, 1);
    }
    // Groups and range reads
    pzpd_group_find(a, "clip seven", 10);
    for (uint64_t i = 0; i < lim; i += 3)
    {
        pzpd_group g;
        if (pzpd_group_info(a, i, &g) && (g.name_len > 0)) { volatile char c = g.name[g.name_len - 1]; (void) c; }
        uint32_t cnt = 1 + (uint32_t)(rng() % 12);
        size_t span = pzpd_range_span(a, i, cnt, 0xFFFFFFFFu);
        if ( (span > 0) && (span <= sizeof(buf)) )
        {
            pzpd_blob_ref rr[12 * PZPD_MAX_STREAMS];
            if (pzpd_read_range(a, i, cnt, 0xFFFFFFFFu, buf, sizeof(buf), (S * cnt <= 12 * PZPD_MAX_STREAMS) ? rr : NULL) > 0)
            {
                for (unsigned q = 0; (S * cnt <= 12 * PZPD_MAX_STREAMS) && (q < S * cnt); q++) { if (rr[q].data && rr[q].size) { volatile unsigned char c = ((const unsigned char *) rr[q].data)[rr[q].size - 1]; (void) c; } }
            }
        }
    }

    // Prefetcher over the (possibly damaged) archive: submit, strided-ish gets, discards, a clear
    pzpd_prefetch_opts po = { 0, 1 + (unsigned)(rng() % 3), (unsigned)(rng() % 4), 4096u * (1 + (unsigned)(rng() % 8)), 1 + (unsigned)(rng() % 16) };
    pzpd_prefetcher *pf = pzpd_prefetcher_create(a, &po);
    if ( (pf != NULL) && (lim > 0) )
    {
        uint64_t ord[64];
        uint32_t msk[64];
        for (int k = 0; k < 64; k++) { ord[k] = rng() % lim; msk[k] = (uint32_t) rng(); }
        pzpd_prefetch_submit(pf, ord, msk, 64);
        pzpd_storage_kind(a, ord[0]);
        for (int k = 0; k < 64; k++)
        {
            int kk = (k * 7) % 64;
            if (kk % 9 == 4) { pzpd_prefetch_discard(pf, ord[kk]); continue; }
            pzpd_blob_ref refs[PZPD_MAX_STREAMS];
            pzpd_ticket tk;
            if (pzpd_prefetch_get(pf, ord[kk], msk[kk], refs, &tk) > 0)
            {
                for (unsigned s = 0; s < S; s++) { if ( (refs[s].data != NULL) && (refs[s].size > 0) ) { volatile unsigned char c = ((const unsigned char *) refs[s].data)[refs[s].size - 1]; (void) c; } }
            }
            if (k == 40) { pzpd_prefetch_clear(pf); pzpd_prefetch_submit(pf, ord, NULL, 32); }
            pzpd_prefetch_release(pf, &tk);
        }
        pzpd_prefetch_stats st;
        pzpd_prefetch_stats_get(pf, &st);
    }
    for (unsigned sh = 0; sh < pzpd_shard_count(a); sh++) { pzpd_prefetch_auto_mode(a, sh); }
    pzpd_prefetcher_destroy(pf);                              // with queued and in-flight entries left

    // Tables: schemas, rows, strings, CSV, shard views, global rows
    static char csv[1 << 16];
    for (unsigned t = 0; t < pzpd_table_count(a); t++)
    {
        const pzpd_schema *sc = pzpd_table_schema(a, t);
        if (sc == NULL) { continue; }
        for (unsigned m = 0; m < pzpd_member_count(a); m++)
        {
            const void *gr = NULL;
            uint32_t gn = pzpd_global_rows(a, m, t, &gr);
            for (uint32_t r = 0; (gr != NULL) && (r < gn); r++)
            {
                const unsigned char *row = (const unsigned char *) gr + (size_t) r * sc->row_stride;
                volatile unsigned char c = row[sc->row_stride - 1]; (void) c;
                for (unsigned col = 0; col < sc->ncols; col++) { if (sc->cols[col].type == PZPD_TYPE_STR) { size_t l; const char *str = pzpd_global_str(a, m, t, row + sc->cols[col].offset, &l); if (str && l) { volatile char cc = str[l - 1]; (void) cc; } } }
            }
            pzpd_global_csv(a, m, t, csv, sizeof(csv));
        }
        for (unsigned sh = 0; sh < pzpd_shard_count(a); sh++)
        {
            pzpd_table_view v;
            if ( pzpd_table_shard_view(a, sh, t, &v) && (v.row_index != NULL) && (v.records > 0) )
            {
                uint32_t last = v.row_index[v.records];
                if (last > 0) { volatile unsigned char c = ((const unsigned char *) v.rows)[(size_t) last * sc->row_stride - 1]; (void) c; }
            }
        }
        for (uint64_t i = 0; i < lim; i++)
        {
            const void *r = NULL;
            uint32_t rn = pzpd_table_rows(a, i, t, &r);
            for (uint32_t q = 0; (r != NULL) && (q < rn); q++)
            {
                const unsigned char *row = (const unsigned char *) r + (size_t) q * sc->row_stride;
                volatile unsigned char c = row[sc->row_stride - 1]; (void) c;
                for (unsigned col = 0; col < sc->ncols; col++) { if (sc->cols[col].type == PZPD_TYPE_STR) { size_t l; const char *str = pzpd_table_str(a, i, t, row + sc->cols[col].offset, &l); if (str && l) { volatile char cc = str[l - 1]; (void) cc; } } }
            }
            pzpd_table_csv(a, i, t, csv, sizeof(csv));
        }
    }
    pzpd_find(a, "no-such-key", 11, NULL);
    pzpd_read_into(a, n + 5, 0, buf, sizeof(buf));
    pzpd_close(a);
}

int main(int argc, char **argv)
{
    int iterations = (argc > 1) ? atoi(argv[1]) : 3000;
    const char *d = getenv("PZPDIR_TEST_DIR");
    snprintf(dir, sizeof(dir), "%s/fuzz", (d != NULL) ? d : "/tmp/pzpdir_test");
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
    if (system(cmd) != 0) { return 1; }

    // A small archive: 300 records, 3 streams, ~4 shards
    char path[1200];
    snprintf(path, sizeof(path), "%s/src.pzpd", dir);
    const char *streams[3] = { "a", "b", "c" };
    pzpd_writer_opts o = { streams, 3, 48 * 1024, 64 };
    pzpd_writer *w = pzpd_writer_create(path, &o);
    unsigned char data[600];
    pzpd_writer_table(w, "joints", "name:str parent:u16", PZPD_TABLE_GLOBAL);
    pzpd_writer_table(w, "persons", "id:u16 bbox:u16[4] kp:u16[6]", 0);
    pzpd_writer_table(w, "text", "source:str body:str", 0);
    pzpd_writer_table(w, "vec", "v:f32[16]", PZPD_TABLE_BULK);
    const char *jcsv = "head,0\n\"l eye\",0\nr eye,0\n";
    pzpd_writer_global_rows_csv(w, 0, jcsv, strlen(jcsv));
    int64_t clip = pzpd_writer_group(w, "clip seven", 10, 0);     // records 100..119: a named group; 200..209 an unnamed one (id 900)
    for (int i = 0; i < 300; i++)
    {
        char k[64], nm[64];
        int kl = snprintf(k, sizeof(k), "key %d ✓", i);
        pzpd_writer_begin(w, k, (size_t) kl, (i >= 100 && i < 120) ? (uint32_t) clip : (i >= 200 && i < 210) ? 900u : PZPD_NO_GROUP, (uint32_t)(i - 100));
        for (unsigned s = 0; s < 3; s++)
        {
            if ( ((i + s) % 5) == 0 ) { continue; }
            for (size_t j = 0; j < sizeof(data); j++) { data[j] = (unsigned char) rng(); }
            int nl = snprintf(nm, sizeof(nm), "dir/%d.%c", i, 'a' + s);
            pzpd_writer_blob(w, s, nm, (size_t) nl, data, 17 + (size_t)((i * 7 + s * 13) % 580));
        }
        char row[256];
        for (int q = 0; q < i % 3; q++) { int rl = snprintf(row, sizeof(row), "%d,1,2,3,4,5,6,7,8,9,%d", q, i); pzpd_writer_rows_csv(w, 1, row, (size_t) rl); }
        int rl = snprintf(row, sizeof(row), "src%d,\"caption, %d\"", i % 2, i);
        pzpd_writer_rows_csv(w, 2, row, (size_t) rl);
        float vv[16];
        for (int q = 0; q < 16; q++) { vv[q] = (float) i / (float)(q + 1); }
        if (i % 4) { pzpd_writer_rows(w, 3, vv, 1, NULL, 0); }
        if (!pzpd_writer_end(w)) { fprintf(stderr, "setup failed: %s\n", pzpd_last_error()); return 1; }
    }
    if (!pzpd_writer_finish(w)) { fprintf(stderr, "setup failed: %s\n", pzpd_last_error()); return 1; }
    pzpd *a = pzpd_open(path, 0);
    unsigned shards = pzpd_shard_count(a);
    pzpd_close(a);

    // A second member with overlapping keys and other streams, and a collection of both
    char path2[1200], coll[1200];
    snprintf(path2, sizeof(path2), "%s/second.pzpd", dir);
    snprintf(coll, sizeof(coll), "%s/set.pzpd", dir);
    const char *streams2[2] = { "c", "z" };
    pzpd_writer_opts o2 = { streams2, 2, 0, 64 };
    w = pzpd_writer_create(path2, &o2);
    for (int i = 250; i < 350; i++)
    {
        char k[64], nm[64];
        int kl = snprintf(k, sizeof(k), "key %d ✓", i);
        int nl = snprintf(nm, sizeof(nm), "other/%d", i);
        pzpd_writer_begin(w, k, (size_t) kl, PZPD_NO_GROUP, 0);
        pzpd_writer_blob(w, (unsigned)(i & 1), nm, (size_t) nl, data, 40 + (size_t)(i % 100));
        if (!pzpd_writer_end(w)) { fprintf(stderr, "setup failed: %s\n", pzpd_last_error()); return 1; }
    }
    if (!pzpd_writer_finish(w)) { fprintf(stderr, "setup failed: %s\n", pzpd_last_error()); return 1; }
    const char *members[2] = { path, path2 };
    if (!pzpd_collection_write(coll, members, NULL, 2, 0)) { fprintf(stderr, "setup failed: %s\n", pzpd_last_error()); return 1; }
    printf("fuzzing %d iterations over a %u-shard archive\n", iterations, shards);

    // Pristine copies of every file
    char files[16][1200];
    unsigned char *orig[16];
    size_t olen[16];
    int nf = 0;
    snprintf(files[nf], sizeof(files[nf]), "%s", coll);
    orig[nf] = slurp(files[nf], &olen[nf]); nf++;
    snprintf(files[nf], sizeof(files[nf]), "%s", path);
    orig[nf] = slurp(files[nf], &olen[nf]); nf++;
    for (unsigned s = 0; (s < shards) && (nf < 16); s++)
    {
        snprintf(files[nf], sizeof(files[nf]), "%s/src.%05u.pzpd", dir, s);
        orig[nf] = slurp(files[nf], &olen[nf]); nf++;
    }

    for (int it = 0; it < iterations; it++)
    {
        int f = (int)(rng() % (uint64_t) nf);
        unsigned char *m = (unsigned char *) malloc(olen[f]);
        memcpy(m, orig[f], olen[f]);
        size_t len = olen[f];
        int mode = (int)(rng() % 5);
        if (mode == 0)
        {
            len = (size_t)(rng() % (olen[f] + 1));                    // truncate anywhere
        }
        else if (mode == 4)
        {
            size_t from = (size_t)(rng() % (olen[f] + 1));           // zero a tail (index loss) and maybe the primary superblock
            memset(m + from, 0, olen[f] - from);
            if (rng() & 1) { memset(m, 0, olen[f] < 4096 ? olen[f] : 4096); }
        }
        else
        {
            int flips = 1 + (int)(rng() % 16);
            for (int k = 0; k < flips; k++)
            {
                size_t pos;
                uint64_t r = rng();
                if      (mode == 1) { pos = (size_t)(r % (olen[f] < 4096 ? olen[f] : 4096)); }                   // primary superblock / header
                else if (mode == 2) { size_t tail = olen[f] / 3; pos = olen[f] - 1 - (size_t)(r % (tail ? tail : 1)); } // index sections + backup
                else                { pos = (size_t)(r % olen[f]); }                                              // anywhere
                m[pos] ^= (unsigned char)(1u << (rng() % 8));
            }
        }
        if (getenv("FUZZ_TRACE")) { fprintf(stderr, "it %d file %d mode %d len %zu\n", it, f, mode, len); }
        spit(files[f], m, len);
        free(m);
        exercise(coll);                                  // through the collection (both members)
        exercise(path);                                  // through the manifest
        if (f > 1) { exercise(files[f]); }              // the damaged shard standalone
        if (f > 1)
        {
            // Recovery tools on the damaged shard: salvage (with / without a schema source), manifest rebuild
            pzpd *sch = (rng() & 1) ? pzpd_open(path, 0) : NULL;
            pzpd_salvage_info si;
            pzpd_salvage(files[f], sch, salvage_touch, NULL, &si);
            pzpd_close(sch);
            const char *sp[16];
            for (int k = 2; k < nf; k++) { sp[k - 2] = files[k]; }
            char rebuilt[1300];
            snprintf(rebuilt, sizeof(rebuilt), "%s/rebuilt.pzpd", dir);
            if (pzpd_manifest_rebuild(rebuilt, sp, (unsigned)(nf - 2))) { exercise(rebuilt); }
        }
        spit(files[f], orig[f], olen[f]);               // restore
        if ( (it + 1) % 250 == 0 ) { printf("  %d\n", it + 1); fflush(stdout); }
    }
    for (int i = 0; i < nf; i++) { free(orig[i]); }
    printf("\033[32mfuzzing finished without crashes\033[0m\n");
    return 0;
}
