/** @file bench_pzpdir_early.c
 *  @brief  Phase 1 early performance gate and phase 6 prefetcher benchmark: per-file reads vs one
 *          pread per record vs mmap views vs the prefetcher.
 *
 *  Reads the bytes of the chosen streams for every sample, in a shuffled order (fixed seed),
 *  with T threads taking every T-th position (like the DataLoader's workers). Before a cold
 *  run every source file and archive shard is evicted with posix_fadvise(DONTNEED), and the
 *  residency is measured with mincore() to prove it.
 *
 *  Usage:
 *    bench_pzpdir_early <record-list.tsv> <archive.pzpd> [--streams rgb,all,geo] [--threads 1,8]
 *                       [--modes fs-open,pzpd-record,pzpd-view] [--seed 1] [--limit N] [--warm] [--nocold]
 *                       [--work US] [--io-threads 4] [--window 256] [--budget MB]
 *
 *  Modes:
 *    fs-open      open + fstat + read + close per file (today's read_file_to_common_memory_of_cache)
 *    pzpd-record  pzpd_read_record(): one pread per sample covering the requested streams
 *    pzpd-view    pzpd_view() per stream, touching every page
 *    pzpd-pf-pagecache / pzpd-pf-map / pzpd-pf-buffers / pzpd-pf-auto
 *                 the whole shuffled order submitted to a prefetcher in that mode; workers
 *                 pzpd_prefetch_get(), touch every page, pzpd_prefetch_release()
 *
 *  --work US adds US microseconds of busy CPU per sample after its bytes are read (a stand-in for
 *  decode + augmentation), so I/O can overlap with work as it does in the DataLoader.
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
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>

#define MAXS 8   ///< Most streams a benchmark reads

static char   **keys;          ///< Record key per sample (list order)
static char  ***paths;         ///< paths[sample][k] = source file of the k-th selected stream, or NULL
static size_t   nsamples;      ///< Samples
static int      nsel;          ///< Selected streams
static char    *selName[MAXS]; ///< Selected stream names
static int      selId[MAXS];   ///< Their ids in the archive
static uint32_t mask;          ///< Stream mask for pzpd_read_record()
static size_t  *order;         ///< Shuffled sample order
static pzpd    *archive;       ///< Open archive
static const char *archivePath; ///< Its path (reopened for every cold run)
static double   workUs;        ///< Busy CPU per sample (microseconds)
static unsigned ioThreads = 4; ///< Prefetcher I/O threads
static unsigned pfWindow = 256;///< Prefetcher window
static uint64_t pfBudget = 0;  ///< Prefetcher BUFFERS budget (0 = default)
static pzpd_prefetcher *pf;    ///< Prefetcher of the current run (pf modes)

/** @brief Monotonic time in seconds. */
static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec + (double) t.tv_nsec * 1e-9;
}

/** @brief Per-thread work description and results. */
struct job
{
    int      mode;     ///< 0 fs-open, 1 pzpd-record, 2 pzpd-view, 3..6 prefetcher (pagecache, map, auto, buffers)
    int      t, T;     ///< Thread index, thread count
    size_t   limit;    ///< Samples to process
    double  *lat;      ///< Per-sample latency (seconds), indexed by position
    uint64_t bytes;    ///< Bytes delivered
    uint64_t sum;      ///< Checksum of touched bytes (keeps the compiler honest)
    int      errors;   ///< Failed reads
    long     minflt;   ///< Minor faults taken by this worker thread
};

/** @brief Thread body: process positions t, t+T, t+2T, ... */
static void *worker(void *arg)
{
    struct job *j = (struct job *) arg;
    size_t cap = 1 << 22;
    unsigned char *buf = (unsigned char *) malloc(cap);
    pzpd_blob_ref refs[PZPD_MAX_STREAMS];
    struct rusage u0, u1;
    getrusage(RUSAGE_THREAD, &u0);
    for (size_t p = (size_t) j->t; p < j->limit; p += (size_t) j->T)
    {
        size_t i = order[p];
        double t0 = now_s();
        if (j->mode == 0)
        {
            for (int k = 0; k < nsel; k++)
            {
                if (paths[i][k] == NULL) { continue; }
                int fd = open(paths[i][k], O_RDONLY);
                struct stat st;
                if ( (fd < 0) || (fstat(fd, &st) != 0) ) { j->errors++; if (fd >= 0) { close(fd); } continue; }
                size_t n = (size_t) st.st_size;
                if (n > cap) { cap = n * 2; buf = (unsigned char *) realloc(buf, cap); }
                size_t got = 0;
                while (got < n) { ssize_t r = read(fd, buf + got, n - got); if (r <= 0) { break; } got += (size_t) r; }
                close(fd);
                if (got != n) { j->errors++; }
                j->bytes += got;
                j->sum += buf[0];
            }
        }
        else if (j->mode == 1)
        {
            size_t need = pzpd_record_span(archive, i, mask);
            if (need > cap) { cap = need * 2; buf = (unsigned char *) realloc(buf, cap); }
            ssize_t r = pzpd_read_record(archive, i, mask, buf, cap, refs);
            if (r < 0) { j->errors++; }
            for (int k = 0; k < nsel; k++) { j->bytes += refs[selId[k]].size; }
            j->sum += buf[0];
        }
        else if (j->mode == 2)
        {
            for (int k = 0; k < nsel; k++)
            {
                size_t n = 0;
                const unsigned char *v = (const unsigned char *) pzpd_view(archive, i, (unsigned) selId[k], &n);
                if (v == NULL) { continue; }
                for (size_t o = 0; o < n; o += 4096) { j->sum += v[o]; }
                j->sum += v[n - 1];
                j->bytes += n;
            }
        }
        else
        {
            pzpd_ticket tk;
            if (pzpd_prefetch_get(pf, i, mask, refs, &tk) < 0) { j->errors++; }
            for (int k = 0; k < nsel; k++)
            {
                const unsigned char *v = (const unsigned char *) refs[selId[k]].data;
                size_t n = refs[selId[k]].size;
                if ( (v == NULL) || (n == 0) ) { continue; }
                for (size_t o = 0; o < n; o += 4096) { j->sum += v[o]; }
                j->sum += v[n - 1];
                j->bytes += n;
            }
            pzpd_prefetch_release(pf, &tk);
        }
        j->lat[p] = now_s() - t0;
        if (workUs > 0) { double until = now_s() + workUs * 1e-6; while (now_s() < until) { j->sum++; } }
    }
    getrusage(RUSAGE_THREAD, &u1);
    j->minflt = u1.ru_minflt - u0.ru_minflt;
    free(buf);
    return NULL;
}

/** @brief Evict a file from the page cache. */
static void evict(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { return; }
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd);
}

/** @brief Fraction of a file's pages in the page cache (mincore). */
static double resident(const char *path, uint64_t *pages)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    if ( (fd < 0) || (fstat(fd, &st) != 0) || (st.st_size == 0) ) { if (fd >= 0) { close(fd); } return 0; }
    void *m = mmap(NULL, (size_t) st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { return 0; }
    size_t np = ((size_t) st.st_size + 4095) / 4096;
    unsigned char *vec = (unsigned char *) malloc(np);
    uint64_t in = 0;
    if (mincore(m, (size_t) st.st_size, vec) == 0) { for (size_t k = 0; k < np; k++) { in += vec[k] & 1; } }
    free(vec);
    munmap(m, (size_t) st.st_size);
    *pages += np;
    return (double) in;
}

/** @brief Evict every source file and shard, then report how much is still cached.
 *  The archive is closed first: pages mapped by a process (pzpd_view) can't be evicted. */
static void make_cold(void)
{
    char shardPaths[64][4096];
    unsigned nsh = pzpd_shard_count(archive);
    for (unsigned s = 0; (s < nsh) && (s < 64); s++)
    {
        pzpd_shard_info si;
        pzpd_shard_info_get(archive, s, &si);
        snprintf(shardPaths[s], sizeof(shardPaths[s]), "%s", si.path);
    }
    pzpd_close(archive);
    for (size_t i = 0; i < nsamples; i++) { for (int k = 0; k < nsel; k++) { if (paths[i][k] != NULL) { evict(paths[i][k]); } } }
    for (unsigned s = 0; (s < nsh) && (s < 64); s++) { evict(shardPaths[s]); }
    // Residency check: every shard + every 25th source file
    uint64_t pages = 0;
    double in = 0;
    for (unsigned s = 0; (s < nsh) && (s < 64); s++) { in += resident(shardPaths[s], &pages); }
    for (size_t i = 0; i < nsamples; i += 25) { for (int k = 0; k < nsel; k++) { if (paths[i][k] != NULL) { in += resident(paths[i][k], &pages); } } }
    archive = pzpd_open(archivePath, 0);
    printf("  cold: %.2f%% of sampled pages still cached\n", pages ? 100.0 * in / (double) pages : 0.0);
}

/** @brief qsort comparator for doubles. */
static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *) a, y = *(const double *) b;
    return (x < y) ? -1 : (x > y);
}

/** @brief Run one mode with T threads and print a result line. */
static void run(int mode, int T, size_t limit, int cold)
{
    static const char *names[7] = { "fs-open", "pzpd-record", "pzpd-view", "pf-pagecache", "pf-map", "pf-auto", "pf-buffers" };
    if (cold) { make_cold(); }
    uint64_t *ords = NULL;
    if (mode >= 3)
    {
        pzpd_prefetch_opts o = { mask, ioThreads, (mode == 3) ? PZPD_PF_PAGECACHE : (mode == 4) ? PZPD_PF_MAP : (mode == 6) ? PZPD_PF_BUFFERS : PZPD_PF_AUTO, pfBudget, pfWindow };
        pf = pzpd_prefetcher_create(archive, &o);
        if (pf == NULL) { fprintf(stderr, "prefetcher: %s\n", pzpd_last_error()); return; }
        ords = (uint64_t *) malloc(limit * sizeof(uint64_t));
        for (size_t p = 0; p < limit; p++) { ords[p] = order[p]; }
    }
    double *lat = (double *) calloc(limit, sizeof(double));
    struct job *jobs = (struct job *) calloc((size_t) T, sizeof(struct job));
    pthread_t *th = (pthread_t *) calloc((size_t) T, sizeof(pthread_t));
    struct rusage r0, r1;
    getrusage(RUSAGE_SELF, &r0);
    double t0 = now_s();
    if (pf != NULL) { pzpd_prefetch_submit(pf, ords, NULL, limit); }       // epoch start: part of the timed run
    for (int t = 0; t < T; t++)
    {
        jobs[t].mode = mode; jobs[t].t = t; jobs[t].T = T; jobs[t].limit = limit; jobs[t].lat = lat;
        pthread_create(&th[t], NULL, worker, &jobs[t]);
    }
    uint64_t bytes = 0;
    int errors = 0;
    long wflt = 0;
    for (int t = 0; t < T; t++) { pthread_join(th[t], NULL); bytes += jobs[t].bytes; errors += jobs[t].errors; wflt += jobs[t].minflt; }
    double wall = now_s() - t0;
    getrusage(RUSAGE_SELF, &r1);
    double cpu = (double)(r1.ru_utime.tv_sec - r0.ru_utime.tv_sec + r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) +
                 (double)(r1.ru_utime.tv_usec - r0.ru_utime.tv_usec + r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) * 1e-6;
    qsort(lat, limit, sizeof(double), cmp_d);
    printf("%-12s %s T=%-2d  %8.0f samples/s  %7.1f MB/s  p50 %6.2f ms  p99 %7.2f ms  cpu %6.1f us/sample  minflt %5.1f/sample (workers %5.2f)  wall %6.2f s%s\n",
           names[mode], cold ? "cold" : "warm", T, (double) limit / wall, (double) bytes / wall / 1e6,
           lat[limit / 2] * 1e3, lat[(size_t)((double) limit * 0.99)] * 1e3, cpu / (double) limit * 1e6,
           (double)(r1.ru_minflt - r0.ru_minflt) / (double) limit, (double) wflt / (double) limit, wall, errors ? "  ERRORS" : "");
    if (pf != NULL)
    {
        pzpd_prefetch_stats st;
        pzpd_prefetch_stats_get(pf, &st);
        printf("             prefetch: shards map %u / pagecache %u / buffers %u (O_DIRECT refused %u), %llu prefetched, hits %llu, waits %llu, sync misses %llu, stalls %llu, io %.2f s, buffers peak %.1f MB\n",
               st.shards_map, st.shards_pagecache, st.shards_buffers, st.direct_fallbacks, (unsigned long long) st.prefetched, (unsigned long long) st.hits, (unsigned long long) st.waits,
               (unsigned long long) st.sync_misses, (unsigned long long) st.producer_stalls, st.io_seconds, (double) st.buffer_bytes_peak / 1e6);
        pzpd_prefetcher_destroy(pf);
        pf = NULL;
        free(ords);
    }
    {
        uint64_t pages = 0;
        double in = 0;
        for (unsigned sh = 0; sh < pzpd_shard_count(archive); sh++) { pzpd_shard_info si; if (pzpd_shard_info_get(archive, sh, &si)) { in += resident(si.path, &pages); } }
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        printf("             archive pages in the page cache after the run: %.1f%%, peak RSS of the process %.1f MB\n", pages ? 100.0 * in / (double) pages : 0.0, (double) ru.ru_maxrss / 1e3);
    }
    fflush(stdout);
    free(lat); free(jobs); free(th);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s list.tsv archive.pzpd [--streams rgb,all,geo] [--threads 1,8] [--modes fs-open,pzpd-record,pzpd-view] [--seed 1] [--limit N] [--warm]\n", argv[0]); return 1; }
    const char *streams = "rgb,all,geo", *threads = "1,8", *modes = "fs-open,pzpd-record,pzpd-view";
    unsigned seed = 1;
    size_t limit = 0;
    int warm = 0, cold = 1;
    for (int i = 3; i < argc; i++)
    {
        if      (!strcmp(argv[i], "--streams") && i + 1 < argc) { streams = argv[++i]; }
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) { threads = argv[++i]; }
        else if (!strcmp(argv[i], "--modes") && i + 1 < argc)   { modes = argv[++i]; }
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)    { seed = (unsigned) atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc)   { limit = (size_t) atol(argv[++i]); }
        else if (!strcmp(argv[i], "--warm"))                    { warm = 1; }
        else if (!strcmp(argv[i], "--nocold"))                  { cold = 0; }   // skip eviction (e.g. for strace -c counts)
        else if (!strcmp(argv[i], "--work") && i + 1 < argc)    { workUs = atof(argv[++i]); }
        else if (!strcmp(argv[i], "--io-threads") && i + 1 < argc) { ioThreads = (unsigned) atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--window") && i + 1 < argc)  { pfWindow = (unsigned) atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--budget") && i + 1 < argc)  { pfBudget = (uint64_t) atol(argv[++i]) << 20; }
    }

    archivePath = argv[2];
    archive = pzpd_open(argv[2], 0);
    if (archive == NULL) { fprintf(stderr, "%s\n", pzpd_last_error()); return 1; }
    char *sl = strdup(streams), *save = NULL;
    for (char *t = strtok_r(sl, ",", &save); (t != NULL) && (nsel < MAXS); t = strtok_r(NULL, ",", &save))
    {
        selName[nsel] = t;
        selId[nsel] = pzpd_stream_id(archive, t);
        if (selId[nsel] < 0) { fprintf(stderr, "no stream %s in the archive\n", t); return 1; }
        mask |= 1u << selId[nsel];
        nsel++;
    }

    // Record list (no escapes needed for this dataset): key, stream, source path
    nsamples = (size_t) pzpd_count(archive);
    keys  = (char **)  calloc(nsamples, sizeof(char *));
    paths = (char ***) calloc(nsamples, sizeof(char **));
    for (size_t i = 0; i < nsamples; i++) { paths[i] = (char **) calloc((size_t) nsel, sizeof(char *)); }
    FILE *f = fopen(argv[1], "r");
    if (f == NULL) { perror(argv[1]); return 1; }
    char *line = NULL;
    size_t lcap = 0, cur = (size_t) -1;
    char *lastKey = NULL;
    while (getline(&line, &lcap, f) > 0)
    {
        if ( (line[0] == '#') || (line[0] == '@') || (line[0] == '\n') ) { continue; }
        line[strcspn(line, "\n")] = 0;
        char *k = strtok(line, "\t"), *s = strtok(NULL, "\t"), *p = strtok(NULL, "\t");
        if ( (k == NULL) || (s == NULL) || (p == NULL) ) { continue; }
        if ( (lastKey == NULL) || strcmp(lastKey, k) ) { cur++; free(lastKey); lastKey = strdup(k); if (cur < nsamples) { keys[cur] = strdup(k); } }
        for (int j = 0; (j < nsel) && (cur < nsamples); j++) { if (!strcmp(selName[j], s)) { paths[cur][j] = strdup(p); } }
    }
    fclose(f);
    free(line);
    free(lastKey);
    if (cur + 1 != nsamples) { fprintf(stderr, "list has %zu records, archive %zu\n", cur + 1, nsamples); return 1; }
    for (size_t i = 0; i < nsamples; i += 997)
    {
        size_t kl = 0;
        const char *k = pzpd_record_key(archive, i, &kl);
        if ( (k == NULL) || (kl != strlen(keys[i])) || memcmp(k, keys[i], kl) ) { fprintf(stderr, "list and archive order differ at %zu\n", i); return 1; }
    }

    // Shuffled order (fixed seed)
    order = (size_t *) malloc(nsamples * sizeof(size_t));
    for (size_t i = 0; i < nsamples; i++) { order[i] = i; }
    srand(seed);
    for (size_t i = nsamples - 1; i > 0; i--) { size_t j = (size_t) rand() % (i + 1); size_t t = order[i]; order[i] = order[j]; order[j] = t; }
    if ( (limit == 0) || (limit > nsamples) ) { limit = nsamples; }

    printf("pzpdir %s benchmark: %zu of %zu samples, streams %s, shuffled (seed %u), work %.0f us/sample, prefetch io-threads %u window %u\n",
           pzpdirVersion, limit, nsamples, streams, seed, workUs, ioThreads, pfWindow);
    char *tl = strdup(threads), *ts = NULL;
    for (char *t = strtok_r(tl, ",", &ts); t != NULL; t = strtok_r(NULL, ",", &ts))
    {
        int T = atoi(t);
        char *ml = strdup(modes), *ms = NULL;
        for (char *m = strtok_r(ml, ",", &ms); m != NULL; m = strtok_r(NULL, ",", &ms))
        {
            int mode = !strcmp(m, "fs-open") ? 0 : !strcmp(m, "pzpd-record") ? 1 : !strcmp(m, "pzpd-view") ? 2 :
                       !strcmp(m, "pzpd-pf-pagecache") ? 3 : !strcmp(m, "pzpd-pf-map") ? 4 : !strcmp(m, "pzpd-pf-auto") ? 5 : !strcmp(m, "pzpd-pf-buffers") ? 6 : -1;
            if (mode < 0) { fprintf(stderr, "unknown mode %s\n", m); continue; }
            if (cold) { run(mode, T, limit, 1); }
            if (warm || !cold) { run(mode, T, limit, 0); }
        }
        free(ml);
    }
    free(tl);
    pzpd_close(archive);
    return 0;
}
