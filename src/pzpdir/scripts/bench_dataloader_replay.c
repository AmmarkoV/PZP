/** @file bench_dataloader_replay.c
 *  @brief `dataloader-replay` (spec §10.6, PLAN.md phase 6): the DataLoader's exact access trace, replayed
 *         against the directories (today's code path) and against a PZPD archive.
 *
 *  The trace, as in RGBToPoseDetect2D/datasets/DataLoader (PrepareBatch.c workerThread, DataLoader.c):
 *  - the epoch order is a shuffled permutation (fixed seed);
 *  - batches of B positions [k·B, k·B+B); worker t of T takes the positions with position % T == t, and the
 *    main thread waits for the whole batch (barrier);
 *  - double buffer: while the main thread "consumes" batch k (Python's collect + copy: --consume-ms), the
 *    workers already fill batch k+1;
 *  - per sample (multiplexed COCO sample: rgb + all + geo), today's path is:
 *      signalPrefetchFile(rgb), signalPrefetchFile(all)   (open + posix_fadvise(WILLNEED), fd kept)
 *      open/fstat/read/close of rgb, all, geo            (cachedReadImage)
 *      freeFileDescriptor(rgb), freeFileDescriptor(all)  (posix_fadvise(DONTNEED) + close)
 *    then decode + augmentation, modelled by --work-us of busy CPU (0 = I/O only).
 *
 *  Modes: fs (today), pzpd-record (one pread per sample), pzpd-pf-<auto|map|pagecache|buffers>
 *  (the whole epoch submitted to a prefetcher, workers get / release).
 *
 *  Usage:
 *    bench_dataloader_replay <record-list.tsv> <archive.pzpd> [--threads 6,30] [--batch 40] [--work-us 0]
 *                            [--consume-ms 26] [--modes fs,pzpd-record,pzpd-pf-auto] [--seed 1] [--warm]
 *
 *  Repository : https://github.com/AmmarkoV/PZP
 *  @author Ammar Qammaz (AmmarkoV)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../pzpdir.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *STREAMS[3] = { "rgb", "all", "geo" };  ///< The multiplexed read set, in read order

static char   ***paths;      ///< paths[sample][k]: source file of STREAMS[k]
static size_t    nsamples;   ///< Samples
static size_t   *order;      ///< Shuffled epoch order
static pzpd     *archive;    ///< Open archive
static const char *archivePath; ///< Its path
static uint32_t  mask;       ///< rgb|all|geo in the archive
static double    workUs;     ///< Busy CPU per sample
static double    consumeMs;  ///< Main-thread time per batch
static unsigned  batch = 40; ///< Batch size

/** @brief Monotonic seconds. */
static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec + (double) t.tv_nsec * 1e-9;
}

/** @brief Busy-wait (CPU work) for us microseconds. */
static uint64_t spin(double us)
{
    uint64_t x = 0;
    double until = now_s() + us * 1e-6;
    while (now_s() < until) { x++; }
    return x;
}

/** @brief As the DataLoader's signalPrefetchFile(): open + fadvise(WILLNEED); the fd is kept until freed. */
static int signal_prefetch(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { return -1; }
    posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED | POSIX_FADV_SEQUENTIAL | POSIX_FADV_NOREUSE);
    return fd;
}

/** @brief As freeFileDescriptor(): fadvise(DONTNEED) + close. */
static void free_fd(int fd)
{
    if (fd > 0) { posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED); close(fd); }
}

/** @brief As cachedReadImage()'s file read: open / fstat / read / close into a growing buffer. */
static size_t read_file(const char *path, unsigned char **buf, size_t *cap)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    if ( (fd < 0) || (fstat(fd, &st) != 0) ) { if (fd >= 0) { close(fd); } return 0; }
    size_t n = (size_t) st.st_size, got = 0;
    if (n > *cap) { *cap = n; *buf = (unsigned char *) realloc(*buf, n); }
    while (got < n) { ssize_t r = read(fd, *buf + got, n - got); if (r <= 0) { break; } got += (size_t) r; }
    close(fd);
    return got;
}

/** @brief Shared state of one run. */
struct run
{
    int              mode;       ///< 0 fs, 1 pzpd-record, 2 prefetcher
    unsigned         T;          ///< Workers
    pzpd_prefetcher *pf;         ///< Prefetcher (mode 2)
    pthread_mutex_t  lock;       ///< Guards the batch hand-off
    pthread_cond_t   go, done;   ///< Batch start / batch finished
    long             batchNo;    ///< Batch the workers should fill (-1 = none yet)
    int              finished;   ///< Workers finished with batchNo
    int              quit;       ///< Stop the workers
    uint64_t         bytes;      ///< Bytes delivered (atomic)
    uint64_t         sink;       ///< Keeps reads and work from being optimised away (atomic)
};

/** @brief Worker argument. */
struct worker
{
    struct run *r;  ///< Run
    unsigned    t;  ///< Worker number
};

/** @brief Process one sample the way the chosen path does. */
static void sample(struct run *r, size_t pos, unsigned char **buf, size_t *cap)
{
    size_t i = order[pos];
    uint64_t bytes = 0, sum = 0;
    if (r->mode == 0)
    {
        int fdRgb = signal_prefetch(paths[i][0]), fdAll = signal_prefetch(paths[i][1]);
        for (int k = 0; k < 3; k++)
        {
            if (paths[i][k] == NULL) { continue; }
            size_t n = read_file(paths[i][k], buf, cap);
            bytes += n;
            if (n) { sum += (*buf)[n - 1]; }
        }
        free_fd(fdRgb);
        free_fd(fdAll);
    }
    else if (r->mode == 1)
    {
        size_t need = pzpd_record_span(archive, i, mask);
        if (need > *cap) { *cap = need; *buf = (unsigned char *) realloc(*buf, need); }
        pzpd_blob_ref refs[PZPD_MAX_STREAMS];
        ssize_t n = pzpd_read_record(archive, i, mask, *buf, *cap, refs);
        if (n > 0) { bytes += (uint64_t) n; sum += (*buf)[n - 1]; }
    }
    else
    {
        pzpd_blob_ref refs[PZPD_MAX_STREAMS];
        pzpd_ticket tk;
        if (pzpd_prefetch_get(r->pf, i, mask, refs, &tk) >= 0)
        {
            for (unsigned s = 0; s < pzpd_stream_count(archive); s++)
            {
                if (refs[s].data == NULL) { continue; }
                const unsigned char *p = (const unsigned char *) refs[s].data;
                for (size_t o = 0; o < refs[s].size; o += 4096) { sum += p[o]; }   // the decoder reads every page
                bytes += refs[s].size;
            }
        }
        pzpd_prefetch_release(r->pf, &tk);
    }
    if (workUs > 0) { sum += spin(workUs); }
    __atomic_add_fetch(&r->bytes, bytes, __ATOMIC_RELAXED);
    __atomic_add_fetch(&r->sink, sum, __ATOMIC_RELAXED);
}

/** @brief Worker: wait for a batch, take its positions ≡ t (mod T), report, repeat. */
static void *worker(void *arg)
{
    struct worker *wk = (struct worker *) arg;
    struct run *r = wk->r;
    unsigned char *buf = NULL;
    size_t cap = 0;
    long seen = -1;
    for (;;)
    {
        pthread_mutex_lock(&r->lock);
        while (!r->quit && (r->batchNo == seen)) { pthread_cond_wait(&r->go, &r->lock); }
        if (r->quit) { pthread_mutex_unlock(&r->lock); break; }
        long b = seen = r->batchNo;
        pthread_mutex_unlock(&r->lock);
        size_t start = (size_t) b * batch, end = start + batch;
        if (end > nsamples) { end = nsamples; }
        for (size_t pos = start; pos < end; pos++) { if (pos % r->T == wk->t) { sample(r, pos, &buf, &cap); } }
        pthread_mutex_lock(&r->lock);
        if (++r->finished == (int) r->T) { pthread_cond_signal(&r->done); }
        pthread_mutex_unlock(&r->lock);
    }
    free(buf);
    return NULL;
}

/** @brief Start batch b on the workers (like db_StartUpdate). */
static void start_batch(struct run *r, long b)
{
    pthread_mutex_lock(&r->lock);
    r->batchNo = b;
    r->finished = 0;
    pthread_cond_broadcast(&r->go);
    pthread_mutex_unlock(&r->lock);
}

/** @brief Wait until the workers finished the current batch (like the main thread's wait in db_update). */
static void wait_batch(struct run *r)
{
    pthread_mutex_lock(&r->lock);
    while (r->finished < (int) r->T) { pthread_cond_wait(&r->done, &r->lock); }
    pthread_mutex_unlock(&r->lock);
}

/** @brief Evict a file from the page cache. */
static void evict(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd >= 0) { posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED); close(fd); }
}

/** @brief Cold cache: close the archive, evict every source file and shard, reopen. */
static void make_cold(void)
{
    char sp[64][4096];
    unsigned nsh = pzpd_shard_count(archive);
    for (unsigned s = 0; (s < nsh) && (s < 64); s++) { pzpd_shard_info si; pzpd_shard_info_get(archive, s, &si); snprintf(sp[s], sizeof(sp[s]), "%s", si.path); }
    pzpd_close(archive);
    for (size_t i = 0; i < nsamples; i++) { for (int k = 0; k < 3; k++) { if (paths[i][k]) { evict(paths[i][k]); } } }
    for (unsigned s = 0; (s < nsh) && (s < 64); s++) { evict(sp[s]); }
    archive = pzpd_open(archivePath, 0);
}

/** @brief One run: the whole epoch through the double-buffered batch pipeline. */
static void run(int mode, int pfMode, unsigned T, int cold)
{
    static const char *pfNames[4] = { "auto", "map", "pagecache", "buffers" };
    if (cold) { make_cold(); }
    struct run r;
    memset(&r, 0, sizeof(r));
    r.mode = mode;
    r.T = T;
    r.batchNo = -1;
    pthread_mutex_init(&r.lock, NULL);
    pthread_cond_init(&r.go, NULL);
    pthread_cond_init(&r.done, NULL);
    struct rusage u0, u1;
    getrusage(RUSAGE_SELF, &u0);
    double t0 = now_s();
    if (mode == 2)
    {
        pzpd_prefetch_opts o = { mask, 4, (unsigned) pfMode, 0, 4 * batch * 2 };
        r.pf = pzpd_prefetcher_create(archive, &o);
        uint64_t *ords = (uint64_t *) malloc(nsamples * sizeof(uint64_t));
        for (size_t p = 0; p < nsamples; p++) { ords[p] = order[p]; }
        pzpd_prefetch_submit(r.pf, ords, NULL, nsamples);      // at shuffle time: the whole epoch
        free(ords);
    }
    struct worker *wk = (struct worker *) calloc(T, sizeof(struct worker));
    pthread_t *th = (pthread_t *) calloc(T, sizeof(pthread_t));
    for (unsigned t = 0; t < T; t++) { wk[t].r = &r; wk[t].t = t; pthread_create(&th[t], NULL, worker, &wk[t]); }

    long nb = (long)((nsamples + batch - 1) / batch);
    double waited = 0;
    start_batch(&r, 0);
    double w0 = now_s();
    wait_batch(&r);
    waited += now_s() - w0;
    for (long k = 0; k < nb; k++)
    {
        if (k + 1 < nb) { start_batch(&r, k + 1); }          // workers fill k+1 ...
        if (consumeMs > 0) { r.sink += spin(consumeMs * 1e3); }   // ... while "Python" consumes k
        if (k + 1 < nb) { w0 = now_s(); wait_batch(&r); waited += now_s() - w0; }
    }
    double wall = now_s() - t0;
    pthread_mutex_lock(&r.lock);
    r.quit = 1;
    pthread_cond_broadcast(&r.go);
    pthread_mutex_unlock(&r.lock);
    for (unsigned t = 0; t < T; t++) { pthread_join(th[t], NULL); }
    getrusage(RUSAGE_SELF, &u1);
    double cpu = (double)(u1.ru_utime.tv_sec - u0.ru_utime.tv_sec + u1.ru_stime.tv_sec - u0.ru_stime.tv_sec) +
                 (double)(u1.ru_utime.tv_usec - u0.ru_utime.tv_usec + u1.ru_stime.tv_usec - u0.ru_stime.tv_usec) * 1e-6;
    char name[64];
    snprintf(name, sizeof(name), "%s%s", (mode == 0) ? "fs" : (mode == 1) ? "pzpd-record" : "pzpd-pf-", (mode == 2) ? pfNames[pfMode] : "");
    printf("%-18s %s T=%-2u B=%u  %7.0f samples/s  %7.1f MB/s  main waited %6.2f ms/batch  cpu %6.2f ms/sample  wall %6.2f s\n",
           name, cold ? "cold" : "warm", T, batch, (double) nsamples / wall, (double) r.bytes / wall / 1e6, waited / (double) nb * 1e3,
           cpu / (double) nsamples * 1e3, wall);
    if (r.pf != NULL)
    {
        pzpd_prefetch_stats st;
        pzpd_prefetch_stats_get(r.pf, &st);
        printf("                   prefetch: hits %llu, waits %llu, sync misses %llu, stalls %llu\n", (unsigned long long) st.hits,
               (unsigned long long) st.waits, (unsigned long long) st.sync_misses, (unsigned long long) st.producer_stalls);
        pzpd_prefetcher_destroy(r.pf);
    }
    fflush(stdout);
    free(wk);
    free(th);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s list.tsv archive.pzpd [--threads 6,30] [--batch 40] [--work-us 0] [--consume-ms 26] [--modes fs,pzpd-record,pzpd-pf-auto] [--seed 1] [--warm]\n", argv[0]); return 1; }
    const char *threads = "6,30", *modes = "fs,pzpd-record,pzpd-pf-auto";
    unsigned seed = 1;
    int cold = 1;
    consumeMs = 26;
    for (int i = 3; i < argc; i++)
    {
        if      (!strcmp(argv[i], "--threads") && i + 1 < argc)    { threads = argv[++i]; }
        else if (!strcmp(argv[i], "--batch") && i + 1 < argc)      { batch = (unsigned) atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--work-us") && i + 1 < argc)    { workUs = atof(argv[++i]); }
        else if (!strcmp(argv[i], "--consume-ms") && i + 1 < argc) { consumeMs = atof(argv[++i]); }
        else if (!strcmp(argv[i], "--modes") && i + 1 < argc)      { modes = argv[++i]; }
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)       { seed = (unsigned) atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--warm"))                       { cold = 0; }
    }
    archivePath = argv[2];
    archive = pzpd_open(archivePath, 0);
    if (archive == NULL) { fprintf(stderr, "%s\n", pzpd_last_error()); return 1; }
    for (int k = 0; k < 3; k++)
    {
        int s = pzpd_stream_id(archive, STREAMS[k]);
        if (s < 0) { fprintf(stderr, "archive has no stream %s\n", STREAMS[k]); return 1; }
        mask |= 1u << s;
    }
    // Source paths from the record list (key, stream, path), in archive order
    nsamples = (size_t) pzpd_count(archive);
    paths = (char ***) calloc(nsamples, sizeof(char **));
    for (size_t i = 0; i < nsamples; i++) { paths[i] = (char **) calloc(3, sizeof(char *)); }
    FILE *f = fopen(argv[1], "r");
    if (f == NULL) { perror(argv[1]); return 1; }
    char *line = NULL, *last = NULL;
    size_t lcap = 0, cur = (size_t) -1;
    while (getline(&line, &lcap, f) > 0)
    {
        if ( (line[0] == '#') || (line[0] == '@') || (line[0] == '\n') ) { continue; }
        line[strcspn(line, "\n")] = 0;
        char *k = strtok(line, "\t"), *s = strtok(NULL, "\t"), *p = strtok(NULL, "\t");
        if ( !k || !s || !p ) { continue; }
        if ( (last == NULL) || strcmp(last, k) ) { cur++; free(last); last = strdup(k); }
        for (int j = 0; (j < 3) && (cur < nsamples); j++) { if (!strcmp(STREAMS[j], s)) { paths[cur][j] = strdup(p); } }
    }
    fclose(f);
    free(line);
    free(last);
    if (cur + 1 != nsamples) { fprintf(stderr, "list has %zu records, the archive %zu\n", cur + 1, nsamples); return 1; }

    order = (size_t *) malloc(nsamples * sizeof(size_t));
    for (size_t i = 0; i < nsamples; i++) { order[i] = i; }
    srand(seed);
    for (size_t i = nsamples - 1; i > 0; i--) { size_t j = (size_t) rand() % (i + 1); size_t t = order[i]; order[i] = order[j]; order[j] = t; }

    printf("dataloader-replay: %zu samples, rgb+all+geo, batch %u, work %.0f us/sample, consume %.0f ms/batch, %s cache\n",
           nsamples, batch, workUs, consumeMs, cold ? "cold" : "warm");
    char *tl = strdup(threads), *ts = NULL;
    for (char *t = strtok_r(tl, ",", &ts); t != NULL; t = strtok_r(NULL, ",", &ts))
    {
        char *ml = strdup(modes), *ms = NULL;
        for (char *m = strtok_r(ml, ",", &ms); m != NULL; m = strtok_r(NULL, ",", &ms))
        {
            int mode = !strcmp(m, "fs") ? 0 : !strcmp(m, "pzpd-record") ? 1 : !strncmp(m, "pzpd-pf-", 8) ? 2 : -1;
            int pfMode = PZPD_PF_AUTO;
            if (mode == 2) { const char *x = m + 8; pfMode = !strcmp(x, "map") ? PZPD_PF_MAP : !strcmp(x, "pagecache") ? PZPD_PF_PAGECACHE : !strcmp(x, "buffers") ? PZPD_PF_BUFFERS : PZPD_PF_AUTO; }
            if (mode < 0) { fprintf(stderr, "unknown mode %s\n", m); continue; }
            run(mode, pfMode, (unsigned) atoi(t), cold);
        }
        free(ml);
    }
    free(tl);
    pzpd_close(archive);
    return 0;
}
