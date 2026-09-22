/** @file test_pzpdir.c
 *  @brief  Unit tests for pzpdir (phase 1): round trips with adversarial names across shards,
 *          rejection rules, format probes, metadata override, version refusal, checksums,
 *          missing shards and open time; collections (phase 1b), tables (phase 1c) and the
 *          prefetcher (phases 6, 7); recovery (phase 2).
 *
 *  Build and run with `make test` (ASan + UBSan). Scratch files go to $PZPDIR_TEST_DIR
 *  (default /tmp/pzpdir_test).
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
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <zstd.h>
#include <lz4.h>

#define XXH_INLINE_ALL
#include "../third_party/xxhash.h"

static int failures = 0;   ///< Failed checks
static int checks   = 0;   ///< Checks run

/** @brief Count a check; print it when it fails. */
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "\033[31mFAIL\033[0m %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "  [%s]\n", pzpd_last_error()); } } while (0)

static char dir[1024];     ///< Scratch directory

/** @brief Monotonic time in seconds. */
static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec + (double) t.tv_nsec * 1e-9;
}

/** @brief File-backed pages mapped into this process (the "shared" field of /proc/self/statm), -1 if unknown. */
static long resident_file_pages(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    long size = 0, resident = 0, shared = -1;
    if (f == NULL) { return -1; }
    if (fscanf(f, "%ld %ld %ld", &size, &resident, &shared) != 3) { shared = -1; }
    fclose(f);
    return shared;
}

/** @brief Deterministic pseudo-random bytes for (record, stream). */
static void fill(unsigned char *p, size_t n, uint64_t seed)
{
    uint64_t x = seed * 0x9E3779B97F4A7C15ull + 1;
    for (size_t i = 0; i < n; i++)
    {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        p[i] = (unsigned char) x;
    }
}

/** @brief Size of the blob of (record, stream): 0..600 bytes, or -1 when the stream is missing. */
static long blob_size(uint64_t i, unsigned s)
{
    uint64_t h = XXH64(&i, sizeof(i), s + 1);
    if ( (h % 7) == 0 && (i % 3) != 0 ) { return -1; }   // some records lack some streams
    if ( (h % 11) == 0 ) { return 0; }                    // some blobs are empty
    return (long)(h % 601);
}

/** @brief Expected blob size of (record, stream) as written: blob_size(), except that a record
 *  whose three streams are all "missing" gets a 5-byte fallback blob in stream 0. */
static long expected_size(uint64_t i, unsigned s)
{
    if ( (blob_size(i, 0) < 0) && (blob_size(i, 1) < 0) && (blob_size(i, 2) < 0) ) { return (s == 0) ? 5 : -1; }
    return blob_size(i, s);
}

/** @brief Adversarial key for record i (unique). */
static size_t make_key(uint64_t i, char *out, size_t cap)
{
    switch (i % 8)
    {
        case 0: return (size_t) snprintf(out, cap, "%llu", (unsigned long long) i);
        case 1: return (size_t) snprintf(out, cap, "images with spaces/sample %llu.jpg", (unsigned long long) i);
        case 2: return (size_t) snprintf(out, cap, "δείγμα_%llu_✓/файл.jpg", (unsigned long long) i);
        case 3: return (size_t) snprintf(out, cap, "a.b.c.%llu.tar.gz.pzp", (unsigned long long) i);
        case 4:
        {
            size_t n = 0;
            for (int d = 0; d < 30; d++) { n += (size_t) snprintf(out + n, cap - n, "d%d/", d); }
            n += (size_t) snprintf(out + n, cap - n, "%llu", (unsigned long long) i);
            return n;
        }
        case 5: return (size_t) snprintf(out, cap, "tab\there\nnewline %llu\\back", (unsigned long long) i);
        case 6: return (size_t) snprintf(out, cap, "#hash @at %llu", (unsigned long long) i);
        default: return (size_t) snprintf(out, cap, "x%llu", (unsigned long long) i);
    }
}

/** @brief Blob name for (record, stream): the key's stem with a per-stream extension, so the
 *  same stem appears with different extensions. */
static size_t make_name(uint64_t i, unsigned s, char *out, size_t cap)
{
    static const char *ext[3] = { ".jpg", ".png", ".pzp" };
    size_t n = make_key(i, out, cap);
    n += (size_t) snprintf(out + n, cap - n, "%s", ext[s % 3]);
    return n;
}

//-----------------------------------------------------------------------------------------------

/** @brief Write the adversarial archive: N records × 3 streams, small shards, 64-byte alignment. */
static void test_roundtrip(uint64_t N)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/rt.pzpd", dir);
    const char *streams[3] = { "rgb", "depth", "seg" };
    pzpd_writer_opts o = { streams, 3, 16ull * 1024 * 1024, 64 };
    pzpd_writer *w = pzpd_writer_create(path, &o);
    CHECK(w != NULL, "writer_create");
    if (w == NULL) { return; }

    static char key[70000], name[70000];
    static unsigned char data[700];
    double t0 = now_s();
    for (uint64_t i = 0; i < N; i++)
    {
        size_t kl = make_key(i, key, sizeof(key));
        int ok = pzpd_writer_begin(w, key, kl, PZPD_NO_GROUP, 0);
        int any = 0;
        for (unsigned s = 0; ok && (s < 3); s++)
        {
            long sz = blob_size(i, s);
            if (sz < 0) { continue; }
            size_t nl = make_name(i, s, name, sizeof(name));
            fill(data, (size_t) sz, i * 3 + s);
            ok = pzpd_writer_blob(w, s, name, nl, data, (size_t) sz);
            any = 1;
        }
        if (ok && !any)
        {
            size_t nl = make_name(i, 0, name, sizeof(name));
            fill(data, 5, i * 3);
            ok = pzpd_writer_blob(w, 0, name, nl, data, 5);
        }
        ok = ok && pzpd_writer_end(w);
        CHECK(ok, "record %llu", (unsigned long long) i);
        if (!ok) { pzpd_writer_abort(w); return; }
    }
    // One record with a 65535-byte key and name, and one with a 1-byte key
    memset(key, 'k', PZPD_MAX_NAME);
    memset(name, 'n', PZPD_MAX_NAME);
    CHECK(pzpd_writer_begin(w, key, PZPD_MAX_NAME, PZPD_NO_GROUP, 0) && pzpd_writer_blob(w, 1, name, PZPD_MAX_NAME, "long", 4) && pzpd_writer_end(w), "65535-byte key/name");
    CHECK(pzpd_writer_begin(w, "\x01", 1, PZPD_NO_GROUP, 0) && pzpd_writer_blob(w, 2, "\x02", 1, "one", 3) && pzpd_writer_end(w), "1-byte key/name");
    CHECK(pzpd_writer_finish(w), "finish");
    printf("  wrote %llu records in %.2f s\n", (unsigned long long)(N + 2), now_s() - t0);

    //-------------------------------------------------------------------------
    pzpd *a = pzpd_open(path, 0);
    CHECK(a != NULL, "open");
    if (a == NULL) { return; }
    CHECK(pzpd_count(a) == N + 2, "count %llu", (unsigned long long) pzpd_count(a));
    CHECK(pzpd_stream_count(a) == 3, "stream count");
    CHECK(pzpd_stream_id(a, "depth") == 1, "stream id");
    CHECK(pzpd_shard_count(a) >= 3, "shards: %u", pzpd_shard_count(a));
    printf("  %u shards\n", pzpd_shard_count(a));

    static unsigned char buf[4096], want[700], rec[8192];
    t0 = now_s();
    for (uint64_t i = 0; i < N; i++)
    {
        size_t kl = make_key(i, key, sizeof(key)), gl = 0;
        const char *k = pzpd_record_key(a, i, &gl);
        CHECK( (k != NULL) && (gl == kl) && (memcmp(k, key, kl) == 0), "key of %llu", (unsigned long long) i);
        int so = 99;
        CHECK(pzpd_find(a, key, kl, &so) == (int64_t) i && (so == -1), "find key %llu", (unsigned long long) i);
        pzpd_blob_ref refs[3];
        ssize_t span = pzpd_read_record(a, i, 7u, rec, sizeof(rec), refs);
        CHECK(span >= 0, "read_record %llu", (unsigned long long) i);
        int any = 0;
        for (unsigned s = 0; s < 3; s++)
        {
            long sz = expected_size(i, s);
            if (sz >= 0) { any = 1; }
            pzpd_blob_info bi;
            CHECK(pzpd_blob_info_get(a, i, s, &bi), "blob_info");
            if (sz < 0) { CHECK(!bi.present && (refs[s].data == NULL), "stream %u of %llu should be missing", s, (unsigned long long) i); continue; }
            size_t nl = make_name(i, s, name, sizeof(name));
            fill(want, (size_t) sz, i * 3 + s);   // the fallback blob is also fill(.., i*3)
            CHECK(bi.present && (bi.size == (uint64_t) sz) && (bi.name_len == nl) && (memcmp(bi.name, name, nl) == 0), "blob_info %llu/%u", (unsigned long long) i, s);
            CHECK(pzpd_read_into(a, i, s, buf, sizeof(buf)) == sz && (memcmp(buf, want, (size_t) sz) == 0), "read_into %llu/%u", (unsigned long long) i, s);
            size_t vs = 0;
            const void *v = pzpd_view(a, i, s, &vs);
            CHECK( (vs == (size_t) sz) && ((sz == 0) || ((v != NULL) && (memcmp(v, want, (size_t) sz) == 0))), "view %llu/%u", (unsigned long long) i, s);
            CHECK( (refs[s].size == (size_t) sz) && ((sz == 0) || (memcmp(refs[s].data, want, (size_t) sz) == 0)), "read_record ref %llu/%u", (unsigned long long) i, s);
            CHECK(pzpd_find(a, name, nl, &so) == (int64_t) i && (so == (int) s), "find name %llu/%u", (unsigned long long) i, s);
        }
        if ( (i % 997) == 0 ) { CHECK(pzpd_verify_record(a, i, 1), "verify %llu", (unsigned long long) i); }
        (void) any;
    }
    printf("  verified %llu records in %.2f s\n", (unsigned long long) N, now_s() - t0);

    memset(key, 'k', PZPD_MAX_NAME);
    memset(name, 'n', PZPD_MAX_NAME);
    int so = 0;
    CHECK(pzpd_find(a, key, PZPD_MAX_NAME, &so) == (int64_t) N, "find 65535-byte key");
    CHECK(pzpd_find(a, name, PZPD_MAX_NAME, &so) == (int64_t) N && so == 1, "find 65535-byte name");
    CHECK(pzpd_find(a, "\x01", 1, &so) == (int64_t)(N + 1), "find 1-byte key");
    CHECK(pzpd_find(a, "no such key", 11, &so) == -1 && pzpd_last_error_code() == PZPD_E_NOTFOUND, "missing key");
    for (unsigned sh = 0; sh < pzpd_shard_count(a); sh++) { CHECK(pzpd_verify_shard(a, sh), "verify shard %u", sh); }

    // Every shard also opens standalone, with local ordinals
    pzpd_shard_info si;
    CHECK(pzpd_shard_info_get(a, 1, &si) && si.available, "shard info");
    pzpd *b = pzpd_open(si.path, 0);
    CHECK(b != NULL && pzpd_count(b) == si.record_count, "standalone shard");
    if (b != NULL)
    {
        uint64_t g = si.first_ordinal + 3;
        size_t kl = make_key(g, key, sizeof(key));
        CHECK(pzpd_find(b, key, kl, NULL) == 3, "standalone find (local ordinal)");
        pzpd_close(b);
    }
    pzpd_close(a);
}

/** @brief Writer rejection rules. */
static void test_rejections(void)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/rej.pzpd", dir);
    const char *streams[2] = { "a", "b" };
    pzpd_writer_opts o = { streams, 2, 0, 0 };
    pzpd_writer *w = pzpd_writer_create(path, &o);
    CHECK(w != NULL, "create");
    if (w == NULL) { return; }
    CHECK(pzpd_writer_begin(w, "k1", 2, PZPD_NO_GROUP, 0) && pzpd_writer_blob(w, 0, "n1", 2, "x", 1) && pzpd_writer_end(w), "first record");
    CHECK(pzpd_writer_begin(w, "k1", 2, PZPD_NO_GROUP, 0) && pzpd_writer_blob(w, 0, "n2", 2, "x", 1) && !pzpd_writer_end(w) && pzpd_last_error_code() == PZPD_E_DUPLICATE, "duplicate key rejected");
    CHECK(pzpd_writer_begin(w, "k2", 2, PZPD_NO_GROUP, 0) && pzpd_writer_blob(w, 1, "n1", 2, "x", 1) && !pzpd_writer_end(w) && pzpd_last_error_code() == PZPD_E_DUPLICATE, "duplicate name rejected (other stream)");
    CHECK(pzpd_writer_begin(w, "k3", 2, PZPD_NO_GROUP, 0) && pzpd_writer_blob(w, 0, "n3", 2, "x", 1) && !pzpd_writer_blob(w, 0, "n4", 2, "y", 1) && pzpd_last_error_code() == PZPD_E_DUPLICATE, "same stream twice rejected");
    pzpd_writer_end(w);
    CHECK(pzpd_writer_begin(w, "k5", 2, PZPD_NO_GROUP, 0) && pzpd_writer_blob(w, 0, "same", 4, "x", 1) && pzpd_writer_blob(w, 1, "same", 4, "y", 1) && !pzpd_writer_end(w), "same name twice in a record rejected");
    CHECK(!pzpd_writer_begin(w, "", 0, PZPD_NO_GROUP, 0) && pzpd_last_error_code() == PZPD_E_ARG, "empty key rejected");
    CHECK(!pzpd_writer_begin(w, "a\0b", 3, PZPD_NO_GROUP, 0), "key with NUL rejected");
    CHECK(pzpd_writer_begin(w, "k6", 2, PZPD_NO_GROUP, 0) && !pzpd_writer_end(w), "record without blobs rejected");
    CHECK(pzpd_writer_begin(w, "k7", 2, PZPD_NO_GROUP, 0) && !pzpd_writer_blob(w, 5, "z", 1, "x", 1) && pzpd_last_error_code() == PZPD_E_ARG, "bad stream rejected");
    pzpd_writer_end(w);
    CHECK(pzpd_writer_begin(w, "k8", 2, PZPD_NO_GROUP, 0) && pzpd_writer_blob(w, 1, "n8", 2, "ok", 2) && pzpd_writer_end(w), "writer usable after rejections");
    CHECK(pzpd_writer_finish(w), "finish");
    pzpd *a = pzpd_open(path, 0);
    // k1, k3 (its first blob was accepted before the rejected one) and k8
    CHECK(a != NULL && pzpd_count(a) == 3 && pzpd_find(a, "k3", 2, NULL) == 1 && pzpd_find(a, "k8", 2, NULL) == 2, "exactly the 3 valid records were written");
    pzpd_close(a);

    const char *bad[1] = { "this_stream_name_is_too_long" };
    pzpd_writer_opts ob = { bad, 1, 0, 0 };
    CHECK(pzpd_writer_create(path, &ob) == NULL, "stream name > 23 bytes rejected");
    pzpd_writer_opts oa = { streams, 2, 0, 100 };
    CHECK(pzpd_writer_create(path, &oa) == NULL, "alignment 100 rejected");
}

/** @brief Build a single-frame PZP: 40-byte header (+ pixels), size prefix, zstd or lz4. */
static size_t make_pzp(unsigned char *out, size_t cap, uint32_t w, uint32_t h, uint32_t bpp, uint32_t ch, int lz4)
{
    uint32_t payload[10 + 64];
    memset(payload, 0, sizeof(payload));
    payload[0] = ((uint32_t)'P' << 24) | ((uint32_t)'Z' << 16) | ((uint32_t)'P' << 8) | (uint32_t)'0';
    payload[1] = bpp; payload[2] = ch; payload[3] = w; payload[4] = h; payload[5] = 8; payload[6] = ch;
    uint32_t usize = sizeof(payload);
    size_t c;
    if (lz4) { c = (size_t) LZ4_compress_default((const char *) payload, (char *) out + 4, (int) usize, (int)(cap - 4)); usize |= 0x80000000u; }
    else     { c = ZSTD_compress(out + 4, cap - 4, payload, sizeof(payload), 3); }
    memcpy(out, &usize, 4);
    return c + 4;
}

/** @brief Format detection and metadata probes. */
static void test_formats(void)
{
    pzpd_blob_meta m;
    char f[5];

    unsigned char jpg[64] = { 0xFF,0xD8, 0xFF,0xE0, 0x00,0x04, 0,0,   0xFF,0xC0, 0x00,0x11, 8, 0x01,0xE0, 0x02,0x80, 3 };
    CHECK(pzpd_detect_format(jpg, sizeof(jpg), "x.bin", 5, &m) == PZPD_FORMAT_JPEG && m.width == 640 && m.height == 480 && m.channels == 3 && m.bits == 8 && (m.meta_flags & PZPD_META_VALID), "JPEG SOF0 (%s)", pzpd_format_name(m.format, f));

    unsigned char png[33] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A, 0,0,0,13, 'I','H','D','R', 0,0,0x02,0x80, 0,0,0x01,0xE0, 16, 0 };
    CHECK(pzpd_detect_format(png, sizeof(png), NULL, 0, &m) == PZPD_FORMAT_PNG && m.width == 640 && m.height == 480 && m.channels == 1 && m.bits == 16, "PNG gray16");
    png[24] = 8; png[25] = 3;
    CHECK(pzpd_detect_format(png, sizeof(png), NULL, 0, &m) == PZPD_FORMAT_PNG && m.channels == 1 && (m.meta_flags & PZPD_META_INDEXED), "PNG palette");

    const char *p6 = "P6\n# comment\n64 32\n65535\n";
    CHECK(pzpd_detect_format(p6, strlen(p6), NULL, 0, &m) == PZPD_FORMAT_PNM && m.width == 64 && m.height == 32 && m.channels == 3 && m.bits == 16, "PPM 16-bit");
    const char *p4 = "P4 8 2\n\xff\x00";
    CHECK(pzpd_detect_format(p4, 9, NULL, 0, &m) == PZPD_FORMAT_PNM && m.bits == 1 && m.channels == 1, "PBM");
    const char *pf = "Pf\n10 20\n-1.0\n";
    CHECK(pzpd_detect_format(pf, strlen(pf), NULL, 0, &m) == PZPD_FORMAT_PFM && m.channels == 1 && m.bits == 32 && (m.meta_flags & PZPD_META_FLOAT) && !(m.meta_flags & PZPD_META_BIG_ENDIAN), "PFM little-endian");

    unsigned char npy[128];
    const char *hdr = "{'descr': '<f4', 'fortran_order': False, 'shape': (3, 4, 5), }";
    memcpy(npy, "\x93NUMPY\x01\x00", 8);
    npy[8] = (unsigned char) strlen(hdr); npy[9] = 0;
    memcpy(npy + 10, hdr, strlen(hdr));
    CHECK(pzpd_detect_format(npy, 10 + strlen(hdr), NULL, 0, &m) == PZPD_FORMAT_NPY && m.width == 3 && m.height == 4 && m.channels == 5 && m.bits == 32 && (m.meta_flags & PZPD_META_FLOAT), "NPY f4 (3,4,5)");

    const char *js = "  {\"a\": 1,\n \"b\": [1,2]}\n";
    CHECK(pzpd_detect_format(js, strlen(js), "x.json", 6, &m) == PZPD_FORMAT_JSON && m.width == 2, "JSON 2 lines");
    const char *tx = "line1\nline2\nline3";
    CHECK(pzpd_detect_format(tx, strlen(tx), "notes", 5, &m) == PZPD_FORMAT_TEXT && m.width == 3, "TEXT 3 lines (unterminated last)");
    CHECK(pzpd_detect_format(tx, strlen(tx), "t.CSV", 5, &m) == PZPD_FORMAT_CSV && m.width == 3, "CSV by extension");
    const char *utf = "καλημέρα κόσμε\n";
    CHECK(pzpd_detect_format(utf, strlen(utf), NULL, 0, &m) == PZPD_FORMAT_TEXT && m.width == 1, "UTF-8 text");
    unsigned char bad[4] = { 'a', 0xC3, 0x28, '\n' };
    CHECK(pzpd_detect_format(bad, 4, NULL, 0, &m) == PZPD_FORMAT_RAW && !(m.meta_flags & PZPD_META_VALID), "invalid UTF-8 is RAW");
    unsigned char rnd[64];
    fill(rnd, sizeof(rnd), 42);
    rnd[0] = 0x00;
    CHECK(pzpd_detect_format(rnd, sizeof(rnd), "a.png", 5, &m) == PZPD_FORMAT_PNG && !(m.meta_flags & PZPD_META_VALID), "extension fallback, metadata invalid");
    CHECK(pzpd_detect_format(rnd, sizeof(rnd), "a.xyz", 5, &m) == PZPD_FORMAT_RAW, "RAW");

    unsigned char pz[1024];
    size_t pn = make_pzp(pz, sizeof(pz), 320, 240, 16, 1, 0);
    CHECK(pzpd_detect_format(pz, pn, NULL, 0, &m) == PZPD_FORMAT_PZP && m.width == 320 && m.height == 240 && m.bits == 16 && m.channels == 1, "PZP zstd");
    pn = make_pzp(pz, sizeof(pz), 64, 48, 8, 3, 1);
    CHECK(pzpd_detect_format(pz, pn, NULL, 0, &m) == PZPD_FORMAT_PZP && m.width == 64 && m.height == 48 && m.bits == 8 && m.channels == 3, "PZP lz4");
    CHECK(strcmp(pzpd_format_name(PZPD_FORMAT_PNG, f), "PNG ") == 0, "format name");
}

/** @brief Metadata override, version refusal, verify flag and a missing shard. */
static void test_misc(void)
{
    char path[1200], shard[1300];
    snprintf(path, sizeof(path), "%s/misc.pzpd", dir);
    const char *streams[1] = { "data" };
    pzpd_writer_opts o = { streams, 1, 64 * 1024, 64 };
    pzpd_writer *w = pzpd_writer_create(path, &o);
    CHECK(w != NULL, "create");
    if (w == NULL) { return; }
    pzpd_blob_meta um = { PZPD_FMT('M','I','N','E'), 7, 9, 2, 1, 16, 0 };
    unsigned char data[8192];
    for (int i = 0; i < 40; i++)
    {
        char k[32];
        int kl = snprintf(k, sizeof(k), "r%02d", i);
        fill(data, sizeof(data), (uint64_t) i);
        int ok = pzpd_writer_begin(w, k, (size_t) kl, PZPD_NO_GROUP, 0) &&
                 ( (i == 0) ? pzpd_writer_blob_ex(w, 0, k, (size_t) kl, data, sizeof(data), &um) : pzpd_writer_blob(w, 0, k, (size_t) kl, data, sizeof(data)) ) &&
                 pzpd_writer_end(w);
        CHECK(ok, "misc record %d", i);
    }
    CHECK(pzpd_writer_finish(w), "finish");

    pzpd *a = pzpd_open(path, 0);
    CHECK(a != NULL && pzpd_shard_count(a) >= 3, "misc shards");
    if (a == NULL) { return; }
    pzpd_blob_info bi;
    CHECK(pzpd_blob_info_get(a, 0, 0, &bi) && bi.meta.format == um.format && bi.meta.width == 7 && bi.meta.height == 9 && bi.meta.bits == 16 &&
          (bi.meta.meta_flags & (PZPD_META_USER | PZPD_META_VALID)) == (PZPD_META_USER | PZPD_META_VALID), "user metadata kept");
    unsigned n = pzpd_shard_count(a);
    pzpd_close(a);

    // Corrupt one payload byte of record 1: headers still verify, payload checksum does not
    snprintf(shard, sizeof(shard), "%s/misc.00000.pzpd", dir);
    a = pzpd_open(path, PZPD_O_VERIFY);
    size_t vs = 0;
    const unsigned char *v = (const unsigned char *) pzpd_view(a, 1, 0, &vs);
    long off = -1;
    {
        int fd = open(shard, O_RDONLY);
        struct stat st;
        fstat(fd, &st);
        unsigned char *all = (unsigned char *) malloc((size_t) st.st_size);
        if (read(fd, all, (size_t) st.st_size) == st.st_size) { unsigned char *hit = memmem(all, (size_t) st.st_size, v, 64); if (hit != NULL) { off = hit - all; } }
        free(all);
        close(fd);
    }
    pzpd_close(a);
    CHECK(off > 0, "found payload in shard");
    if (off > 0)
    {
        int fd = open(shard, O_RDWR);
        unsigned char c;
        CHECK(pread(fd, &c, 1, off + 10) == 1, "read byte");
        c ^= 0xFF;
        CHECK(pwrite(fd, &c, 1, off + 10) == 1, "flip byte");
        close(fd);
        a = pzpd_open(path, PZPD_O_VERIFY);
        CHECK(pzpd_read_into(a, 1, 0, data, sizeof(data)) == PZPD_E_CHECKSUM, "PZPD_O_VERIFY catches a flipped payload byte");
        CHECK(pzpd_verify_record(a, 1, 0) && !pzpd_verify_record(a, 1, 1), "verify_record: header ok, payload bad");
        CHECK(pzpd_read_into(a, 2, 0, data, sizeof(data)) == (ssize_t) sizeof(data), "other records still read");
        pzpd_close(a);
    }

    // A missing middle shard fails only its own ordinals
    snprintf(shard, sizeof(shard), "%s/misc.00001.pzpd", dir);
    char moved[1400];
    snprintf(moved, sizeof(moved), "%s.moved", shard);
    CHECK(rename(shard, moved) == 0, "hide shard 1");
    a = pzpd_open(path, 0);
    CHECK(a != NULL, "open with a missing shard");
    if (a != NULL)
    {
        pzpd_shard_info si;
        pzpd_shard_info_get(a, 1, &si);
        CHECK(!si.available, "shard 1 unavailable");
        CHECK(pzpd_read_into(a, si.first_ordinal, 0, data, sizeof(data)) == PZPD_E_SHARD_MISSING, "its records fail with PZPD_E_SHARD_MISSING");
        CHECK(pzpd_read_into(a, 0, 0, data, sizeof(data)) > 0 && pzpd_read_into(a, pzpd_count(a) - 1, 0, data, sizeof(data)) > 0, "other shards still read");
        pzpd_close(a);
    }
    rename(moved, shard);

    // A shard claiming a newer format version is refused: copy a shard, bump the version in both
    // superblocks and re-seal them (sb_checksum sits at byte 1728: 80 B of fixed fields,
    // 768 B streams, 768 B tables, 14 u64 fields; checked by a _Static_assert-equivalent below)
    snprintf(shard, sizeof(shard), "%s/misc.%05u.pzpd", dir, n - 1);
    char newer[1400];
    snprintf(newer, sizeof(newer), "%s/newer.pzpd", dir);
    {
        int in = open(shard, O_RDONLY), out = open(newer, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        struct stat st;
        fstat(in, &st);
        unsigned char *all = (unsigned char *) malloc((size_t) st.st_size);
        CHECK(read(in, all, (size_t) st.st_size) == st.st_size, "copy shard");
        const size_t csOff = 1728;
        uint64_t stored;
        memcpy(&stored, all + csOff, 8);
        CHECK(stored == XXH64(all, csOff, 0), "sb_checksum is at byte 1728");
        for (int copy = 0; copy < 2; copy++)
        {
            unsigned char *sb = all + ((copy == 0) ? 0 : (size_t) st.st_size - 4096);
            uint32_t ver = PZPD_FORMAT_VERSION + 1;
            memcpy(sb + 8, &ver, 4);
            uint64_t cs = XXH64(sb, csOff, 0);
            memcpy(sb + csOff, &cs, 8);
        }
        CHECK(write(out, all, (size_t) st.st_size) == st.st_size, "write copy");
        free(all);
        close(in);
        close(out);
    }
    pzpd *b = pzpd_open(newer, 0);
    CHECK( (b == NULL) && (pzpd_last_error_code() == PZPD_E_VERSION), "newer format version refused (code %d)", pzpd_last_error_code());
    pzpd_close(b);
    b = pzpd_open(shard, 0);
    CHECK(b != NULL, "unmodified shard still opens standalone");
    pzpd_close(b);

    // A manifest whose hash_count × 24 wraps around to the real section size (hash_count + 2^61), re-sealed:
    // must be refused at open, not read past the mapping by pzpd_find() (hash_count sits at byte 1616:
    // 48 B of fixed fields, 768 B streams, 768 B tables, 4 u64 offsets; sb_checksum 5 u64 later, at 1656)
    char crafted[1400];
    snprintf(crafted, sizeof(crafted), "%s/crafted.pzpd", dir);
    {
        int in = open(path, O_RDONLY), out = open(crafted, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        struct stat st;
        fstat(in, &st);
        unsigned char *all = (unsigned char *) malloc((size_t) st.st_size);
        CHECK(read(in, all, (size_t) st.st_size) == st.st_size, "copy manifest");
        const size_t hcOff = 1616, csOff = 1656;
        uint64_t stored, hc;
        memcpy(&stored, all + csOff, 8);
        CHECK(stored == XXH64(all, csOff, 0), "manifest sb_checksum is at byte 1656");
        memcpy(&hc, all + hcOff, 8);
        hc += 1ull << 61;
        memcpy(all + hcOff, &hc, 8);
        uint64_t cs = XXH64(all, csOff, 0);
        memcpy(all + csOff, &cs, 8);
        CHECK(write(out, all, (size_t) st.st_size) == st.st_size, "write crafted manifest");
        free(all);
        close(in);
        close(out);
    }
    b = pzpd_open(crafted, 0);
    CHECK( (b == NULL) && (pzpd_last_error_code() == PZPD_E_FORMAT), "manifest with a wrapping hash_count refused (code %d)", pzpd_last_error_code());
    if (b != NULL) { pzpd_find(b, "zz-not-there", 12, NULL); pzpd_close(b); }   // without the check: out-of-bounds read here
}

/** @brief Phase 1c: table schemas, CSV, binary rows, strings, global tables, collections. */
static void test_tables(void)
{
    char path[1200], path2[1200], path3[1200];
    snprintf(path, sizeof(path), "%s/tab.pzpd", dir);
    snprintf(path2, sizeof(path2), "%s/tab2.pzpd", dir);
    snprintf(path3, sizeof(path3), "%s/tab3.pzpd", dir);
    const char *streams[1] = { "rgb" };

    //--- schema layout and rejection ------------------------------------------------
    pzpd_writer_opts o = { streams, 1, 16 * 1024, 64 };
    pzpd_writer *w = pzpd_writer_create(path, &o);
    CHECK(w != NULL, "create");
    if (w == NULL) { return; }
    int tj = pzpd_writer_table(w, "joints", "name:str parent:u16", PZPD_TABLE_GLOBAL);
    int tp = pzpd_writer_table(w, "persons", "id:u16, bbox:u16[4], kp:u16[51]", 0);
    int tm = pzpd_writer_table(w, "mixed", "a:u8 b:f64 c:str d:u16[3] e:i8 f:f32", 0);
    int t7 = pzpd_writer_table(w, "d768", "v:f32[768]", PZPD_TABLE_BULK);
    int t1k = pzpd_writer_table(w, "d1024", "v:f32[1024]", PZPD_TABLE_BULK);
    int tf = pzpd_writer_table(w, "floats", "f:f32 d:f64", 0);
    CHECK(tj == 0 && tp == 1 && tm == 2 && t7 == 3 && t1k == 4 && tf == 5, "table ids in declaration order");
    CHECK(pzpd_writer_table(w, "bad1", "x", 0) < 0 && pzpd_writer_table(w, "bad2", "x:u17", 0) < 0 && pzpd_writer_table(w, "bad3", "x:u8[0]", 0) < 0 &&
          pzpd_writer_table(w, "bad4", "x:u8,x:u8", 0) < 0 && pzpd_writer_table(w, "persons", "x:u8", 0) < 0 && pzpd_writer_table(w, "rgb", "x:u8", 0) < 0 &&
          pzpd_writer_table(w, "this_table_name_is_too_long", "x:u8", 0) < 0 && pzpd_writer_table(w, "bad5", "", 0) < 0, "bad schemas / names rejected");

    //--- global rows ------------------------------------------------------------------
    CHECK(pzpd_writer_global_rows_csv(w, (unsigned) tj, "head,0\n\"left eye, l\",0\nnose\"\"x,1\n", 29) == 0, "unquoted field with a quote rejected");
    CHECK(pzpd_writer_global_rows_csv(w, (unsigned) tj, "head,0\n\"left eye, l\",0\n", 22), "global rows via CSV");
    struct { pzpd_str name; uint16_t parent; uint16_t pad; } jr = { { 0, 5 }, 1, 0 };
    CHECK(sizeof(jr) == 12 && pzpd_writer_global_rows(w, (unsigned) tj, &jr, 1, "rightX", 6), "global rows binary with strings");
    CHECK(!pzpd_writer_rows_csv(w, (unsigned) tp, "1,2,3,4,5", 9) && pzpd_last_error_code() == PZPD_E_STATE, "record rows outside a record rejected");
    CHECK(!pzpd_writer_global_rows_csv(w, (unsigned) tp, "1", 1), "a record table given to global_rows rejected");

    //--- records ------------------------------------------------------------------------
    uint32_t seed = 12345;
    static float f768[768], f1k[1024];
    int ok = 1;
    for (int i = 0; ok && (i < 300); i++)
    {
        char k[32];
        int kl = snprintf(k, sizeof(k), "r%03d", i);
        ok = pzpd_writer_begin(w, k, (size_t) kl, PZPD_NO_GROUP, 0) && pzpd_writer_blob(w, 0, k, (size_t) kl, "x", 1);
        // persons: i % 4 rows, binary
        struct { uint16_t id, bbox[4], kp[51]; } pr[3];
        for (int r = 0; r < i % 4 && r < 3; r++) { pr[r].id = (uint16_t)(r + 1); for (int q = 0; q < 4; q++) { pr[r].bbox[q] = (uint16_t)(i * 7 + q); } for (int q = 0; q < 51; q++) { pr[r].kp[q] = (uint16_t)(i + q * r); } }
        ok = ok && (sizeof(pr[0]) == 112) && pzpd_writer_rows(w, (unsigned) tp, pr, (uint32_t)((i % 4 > 3) ? 3 : i % 4), NULL, 0);
        // mixed via CSV, with a string containing comma / quote / newline
        char csv[256];
        int cl = snprintf(csv, sizeof(csv), "%d,%d.5,\"s%d, \"\"q\"\"\nline\",%d,%d,%d,-%d,0.%d\n", i % 256, i, i, i, i + 1, i + 2, i % 128, i + 1);
        ok = ok && pzpd_writer_rows_csv(w, (unsigned) tm, csv, (size_t) cl);
        // descriptors, only on some records
        for (int q = 0; q < 1024; q++) { seed = seed * 1103515245u + 12345u; if (q < 768) { f768[q] = (float)(int32_t) seed / 1e6f; } f1k[q] = (float)(int32_t)(seed ^ 0x5555) / 3e7f; }
        if (i % 3 != 0) { ok = ok && pzpd_writer_rows(w, (unsigned) t7, f768, 1, NULL, 0); }
        if (i % 5 != 0) { ok = ok && pzpd_writer_rows(w, (unsigned) t1k, f1k, 1, NULL, 0); }
        // random float bit patterns (finite), for the CSV round trip below
        struct { float f; uint32_t pad; double d; } fr;
        seed = seed * 1103515245u + 12345u;
        uint32_t fb = seed;
        if ( ((fb >> 23) & 0xFF) == 0xFF ) { fb &= ~(1u << 30); }        // no inf / nan
        uint64_t db = ((uint64_t) seed << 32) ^ ((uint64_t) seed * 2654435761u);
        if ( ((db >> 52) & 0x7FF) == 0x7FF ) { db &= ~(1ull << 62); }
        memcpy(&fr.f, &fb, 4); fr.pad = 0; memcpy(&fr.d, &db, 8);
        ok = ok && pzpd_writer_rows(w, (unsigned) tf, &fr, 1, NULL, 0);
        ok = ok && pzpd_writer_end(w);
    }
    CHECK(ok, "300 records with table rows");
    CHECK(pzpd_writer_begin(w, "late", 4, PZPD_NO_GROUP, 0) && !pzpd_writer_global_rows_csv(w, (unsigned) tj, "x,1", 3), "global rows after the first record rejected");
    pzpd_writer_blob(w, 0, "late", 4, "x", 1);
    CHECK(!pzpd_writer_rows_csv(w, (unsigned) tp, "1,2,3,4,70000,0", 15), "out-of-range u16 rejected");
    CHECK(strstr(pzpd_last_error(), "row 1") != NULL && strstr(pzpd_last_error(), "bbox") != NULL && strstr(pzpd_last_error(), "70000") != NULL, "error names row, column and value: %s", pzpd_last_error());
    CHECK(!pzpd_writer_rows_csv(w, (unsigned) tp, "1,2,3", 5) && strstr(pzpd_last_error(), "too few") != NULL, "too few fields");
    char many[512];
    int ml = snprintf(many, sizeof(many), "1,1,1,1,1");
    for (int q = 0; q < 52; q++) { ml += snprintf(many + ml, sizeof(many) - (size_t) ml, ",1"); }
    CHECK(!pzpd_writer_rows_csv(w, (unsigned) tp, many, (size_t) ml) && strstr(pzpd_last_error(), "too many") != NULL, "too many fields");
    CHECK(!pzpd_writer_rows_csv(w, (unsigned) tm, "1,x,\"s\",1,1,1,1,1", 17) && strstr(pzpd_last_error(), "column b") != NULL, "non-numeric f64 rejected");
    CHECK(!pzpd_writer_rows_csv(w, (unsigned) tm, "1,1,\"unterminated,1,1,1,1,1", 26), "unterminated quote rejected");
    CHECK(!pzpd_writer_rows_csv(w, (unsigned) tm, "1,1,s,1,1,1,-129,1", 18) && strstr(pzpd_last_error(), "out of range") != NULL, "i8 range");
    CHECK(pzpd_writer_end(w), "the record itself is still fine after rejected rows");
    CHECK(pzpd_writer_finish(w), "finish");

    // u64: a minus sign after leading whitespace must not wrap around (strtoull negates it silently)
    char upath[1100];
    snprintf(upath, sizeof(upath), "%s/u64.pzpd", dir);
    const char *ustreams[] = { "rgb" };
    pzpd_writer_opts uo = { ustreams, 1, 0, 0 };
    pzpd_writer *wu = pzpd_writer_create(upath, &uo);
    int tu = (wu != NULL) ? pzpd_writer_table(wu, "v", "v:u64", 0) : -1;
    CHECK( (tu >= 0) && pzpd_writer_begin(wu, "k", 1, PZPD_NO_GROUP, 0), "u64 table");
    CHECK(!pzpd_writer_rows_csv(wu, (unsigned) tu, " -1", 3) && strstr(pzpd_last_error(), "out of range") != NULL, "u64: \" -1\" rejected");
    CHECK(!pzpd_writer_rows_csv(wu, (unsigned) tu, "\t-5", 3) && strstr(pzpd_last_error(), "out of range") != NULL, "u64: tab then -5 rejected");
    CHECK(pzpd_writer_rows_csv(wu, (unsigned) tu, " 7", 2), "u64: \" 7\" still accepted");
    pzpd_writer_abort(wu);

    //--- read back ------------------------------------------------------------------------
    pzpd *a = pzpd_open(path, 0);
    CHECK(a != NULL && pzpd_table_count(a) == 6 && pzpd_shard_count(a) >= 2, "open (%u shards)", a ? pzpd_shard_count(a) : 0);
    if (a == NULL) { return; }
    const pzpd_schema *sp = pzpd_table_schema(a, (unsigned) pzpd_table_id(a, "persons"));
    CHECK(sp != NULL && sp->row_stride == 112 && sp->ncols == 3 && sp->cols[1].offset == 2 && sp->cols[2].offset == 10 && sp->cols[2].count == 51, "persons: 112 B, C-struct offsets");
    const pzpd_schema *sm = pzpd_table_schema(a, (unsigned) tm);
    CHECK(sm->cols[0].offset == 0 && sm->cols[1].offset == 8 && sm->cols[2].offset == 16 && sm->cols[3].offset == 24 && sm->cols[4].offset == 30 && sm->cols[5].offset == 32 && sm->row_stride == 40,
          "mixed: natural alignment and stride padding");
    CHECK(pzpd_table_schema(a, (unsigned) t7)->row_stride == 3072 && pzpd_table_schema(a, (unsigned) t1k)->row_stride == 4096 &&
          (pzpd_table_schema(a, (unsigned) t7)->flags & PZPD_TABLE_BULK), "D = 768 and D = 1024 tables coexist");
    const void *rows = NULL;
    CHECK(pzpd_global_rows(a, 0, (unsigned) tj, &rows) == 3, "3 joints");
    size_t sl = 0;
    const char *js = pzpd_global_str(a, 0, (unsigned) tj, (const char *) rows + 12, &sl);
    CHECK(js != NULL && sl == 11 && !memcmp(js, "left eye, l", 11), "global string");
    js = pzpd_global_str(a, 0, (unsigned) tj, (const char *) rows + 24, &sl);
    CHECK(js != NULL && sl == 5 && !memcmp(js, "right", 5), "binary global string");
    int errs = 0;
    for (uint64_t i = 0; i < 300; i++)
    {
        const void *r = NULL;
        uint32_t n = pzpd_table_rows(a, i, (unsigned) tp, &r);
        if (n != (uint32_t)(i % 4 > 3 ? 3 : i % 4)) { errs++; continue; }
        for (uint32_t q = 0; q < n; q++)
        {
            const uint16_t *row = (const uint16_t *)((const char *) r + q * 112);
            if ( (row[0] != q + 1) || (row[1] != (uint16_t)(i * 7)) || (row[5 + 50] != (uint16_t)(i + 50 * q)) ) { errs++; }
        }
        n = pzpd_table_rows(a, i, (unsigned) tm, &r);
        char want[64];
        int wl = snprintf(want, sizeof(want), "s%d, \"q\"\nline", (int) i);
        const char *sv = (n == 1) ? pzpd_table_str(a, i, (unsigned) tm, (const char *) r + 16, &sl) : NULL;
        if ( (sv == NULL) || (sl != (size_t) wl) || memcmp(sv, want, (size_t) wl) ) { errs++; }
        if ( (pzpd_table_rows(a, i, (unsigned) t7, NULL) != (i % 3 != 0)) || (pzpd_table_rows(a, i, (unsigned) t1k, NULL) != (i % 5 != 0)) ) { errs++; }
    }
    CHECK(errs == 0, "rows / strings / optional rows read back (%d errors)", errs);
    CHECK(pzpd_table_rows(a, 300, (unsigned) tp, NULL) == 0 && pzpd_last_error_code() == PZPD_OK, "a record without rows: 0, no error");
    char csvbuf[4096];
    ssize_t cl2 = pzpd_table_csv(a, 7, (unsigned) tm, csvbuf, sizeof(csvbuf));
    CHECK(cl2 > 0 && !strcmp(csvbuf, "7,7.5,\"s7, \"\"q\"\"\nline\",7,8,9,-7,0.8\n"), "CSV render: %s", csvbuf);
    CHECK(pzpd_table_csv(a, 7, (unsigned) tm, csvbuf, 5) == cl2 && strlen(csvbuf) == 4, "CSV truncation reports the full length");
    CHECK(pzpd_table_rows(a, 0, (unsigned) tj, NULL) == 0 && pzpd_last_error_code() == PZPD_E_ARG, "table_rows on a global table rejected");

    // Float round trip: binary -> CSV -> binary is bit-exact
    pzpd_writer_opts o2 = { streams, 1, 0, 64 };
    pzpd_writer *w2 = pzpd_writer_create(path2, &o2);
    int tf2 = pzpd_writer_table(w2, "floats", "f:f32 d:f64", 0);
    ok = (tf2 == 0);
    for (uint64_t i = 0; ok && (i < 300); i++)
    {
        ssize_t n = pzpd_table_csv(a, i, (unsigned) tf, csvbuf, sizeof(csvbuf));
        char k[16];
        int kl = snprintf(k, sizeof(k), "k%llu", (unsigned long long) i);
        ok = (n > 0) && pzpd_writer_begin(w2, k, (size_t) kl, PZPD_NO_GROUP, 0) && pzpd_writer_blob(w2, 0, k, (size_t) kl, "x", 1) &&
             pzpd_writer_rows_csv(w2, 0, csvbuf, (size_t) n) && pzpd_writer_end(w2);
    }
    CHECK(ok && pzpd_writer_finish(w2), "re-pack floats from CSV");
    pzpd *b = pzpd_open(path2, 0);
    errs = 0;
    for (uint64_t i = 0; (b != NULL) && (i < 300); i++)
    {
        const void *r1 = NULL, *r2 = NULL;
        if ( (pzpd_table_rows(a, i, (unsigned) tf, &r1) != 1) || (pzpd_table_rows(b, i, 0, &r2) != 1) || memcmp(r1, r2, 16) ) { errs++; }
    }
    CHECK(b != NULL && errs == 0, "f32 / f64 binary -> CSV -> binary is bit-exact (%d differ)", errs);
    pzpd_close(b);

    // Standalone shard: tables and global rows available without the manifest
    pzpd_shard_info si;
    pzpd_shard_info_get(a, 1, &si);
    b = pzpd_open(si.path, 0);
    CHECK(b != NULL && pzpd_table_count(b) == 6 && pzpd_global_rows(b, 0, 0, NULL) == 3 &&
          pzpd_table_rows(b, 0, (unsigned) tp, NULL) == (uint32_t)((si.first_ordinal % 4 > 3) ? 3 : si.first_ordinal % 4), "standalone shard has the tables and the global rows");
    pzpd_close(b);
    for (unsigned sh = 0; sh < pzpd_shard_count(a); sh++) { CHECK(pzpd_verify_shard(a, sh), "verify shard %u (with table sections)", sh); }
    CHECK(pzpd_verify_record(a, 5, 1), "record header with row copies verifies");
    pzpd_close(a);

    //--- collections: same schema merges, a different schema is rejected ------------------
    pzpd_writer_opts o3 = { streams, 1, 0, 64 };
    w = pzpd_writer_create(path3, &o3);
    CHECK(pzpd_writer_table(w, "persons", "id:u16, bbox:u16[4], kp:u16[51]", 0) == 0 && pzpd_writer_table(w, "extra", "x:i64", 0) == 1, "member 2 tables");
    CHECK(pzpd_writer_begin(w, "z", 1, PZPD_NO_GROUP, 0) && pzpd_writer_blob(w, 0, "z", 1, "x", 1) && pzpd_writer_rows_csv(w, 1, "-5\n6", 4) && pzpd_writer_end(w) && pzpd_writer_finish(w), "member 2");
    const char *both[2] = { path, path3 };
    a = pzpd_open_many(both, NULL, 2, 0);
    CHECK(a != NULL && pzpd_table_count(a) == 7 && pzpd_table_id(a, "extra") == 6, "tables merged by name");
    if (a != NULL)
    {
        uint64_t last = pzpd_count(a) - 1;
        CHECK(pzpd_table_rows(a, last, 6, NULL) == 2 && pzpd_table_rows(a, 0, 6, NULL) == 0 && pzpd_table_rows(a, last, (unsigned) tm, NULL) == 0, "a member without a table has no rows");
        CHECK(pzpd_global_rows(a, 1, 0, NULL) == 0 && pzpd_global_rows(a, 0, 0, NULL) == 3, "global rows are per member");
        pzpd_table_view v;
        CHECK(pzpd_table_shard_view(a, pzpd_shard_count(a) - 1, 6, &v) && v.row_index != NULL && v.total_rows == 2 && v.first_ordinal == last, "shard view of member 2");
        pzpd_close(a);
    }
    w = pzpd_writer_create(path3, &o3);
    pzpd_writer_table(w, "persons", "id:u32, bbox:u16[4]", 0);
    pzpd_writer_begin(w, "z", 1, PZPD_NO_GROUP, 0); pzpd_writer_blob(w, 0, "z", 1, "x", 1); pzpd_writer_end(w);
    CHECK(pzpd_writer_finish(w), "member with a different persons schema");
    a = pzpd_open_many(both, NULL, 2, 0);
    CHECK(a == NULL && pzpd_last_error_code() == PZPD_E_FORMAT && strstr(pzpd_last_error(), "different schema") != NULL, "schema mismatch across members rejected");
    pzpd_close(a);
}

/** @brief Open + first lookup + first read on a 1 M-record archive must take < 5 ms. */
static void test_open_time(void)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/big.pzpd", dir);
    const char *streams[1] = { "data" };
    pzpd_writer_opts o = { streams, 1, 0, 64 };
    pzpd_writer *w = pzpd_writer_create(path, &o);
    CHECK(w != NULL, "create");
    if (w == NULL) { return; }
    CHECK(pzpd_writer_table(w, "persons", "id:u16 bbox:u16[4] kp:u16[51]", 0) == 0, "persons table");
    struct { uint16_t id, bbox[4], kp[51]; } pr[2];
    memset(pr, 0, sizeof(pr));
    const uint64_t N = 1000000;
    double t0 = now_s();
    for (uint64_t i = 0; i < N; i++)
    {
        char k[32];
        int kl = snprintf(k, sizeof(k), "%012llu.jpg", (unsigned long long) i);
        pr[0].id = (uint16_t) i; pr[1].id = (uint16_t)(i + 1); pr[0].kp[50] = 7;
        if ( !pzpd_writer_begin(w, k, (size_t) kl, PZPD_NO_GROUP, 0) || !pzpd_writer_blob(w, 0, k, (size_t) kl, k, (size_t) kl) ||
             !pzpd_writer_rows(w, 0, pr, (uint32_t)(i % 3), NULL, 0) || !pzpd_writer_end(w) )
            { CHECK(0, "big record %llu", (unsigned long long) i); pzpd_writer_abort(w); return; }
    }
    CHECK(pzpd_writer_finish(w), "finish");
    printf("  wrote 1 M records in %.2f s\n", now_s() - t0);

    t0 = now_s();
    pzpd *a = pzpd_open(path, 0);
    int64_t i = (a != NULL) ? pzpd_find(a, "000000777777.jpg", 16, NULL) : -1;
    char buf[64];
    ssize_t r = (i >= 0) ? pzpd_read_into(a, (uint64_t) i, 0, buf, sizeof(buf)) : -1;
    double ms = (now_s() - t0) * 1000.0;
    CHECK( (i == 777777) && (r == 16) && (memcmp(buf, "000000777777.jpg", 16) == 0), "lookup in 1 M records");
    printf("  open + find + read on 1 M records: %.3f ms\n", ms);
    CHECK(ms < 5.0, "open time %.3f ms >= 5 ms", ms);

    // Bulk load: every persons row of 1 M records through the shard views (as a DataLoader would at startup)
    t0 = now_s();
    uint64_t total = 0, sum = 0, recs = 0;
    for (unsigned sh = 0; sh < pzpd_shard_count(a); sh++)
    {
        pzpd_table_view v;
        if (!pzpd_table_shard_view(a, sh, 0, &v)) { CHECK(0, "shard view %u", sh); break; }
        recs += v.records;
        for (uint64_t r = 0; r < v.records; r++)
        {
            uint32_t n = v.row_index[r + 1] - v.row_index[r];
            total += n;
            const uint16_t *row = (const uint16_t *)((const char *) v.rows + (size_t) v.row_index[r] * 112);
            for (uint32_t q = 0; q < n; q++) { sum += row[q * 56 + 5 + 50]; }
        }
    }
    double bl = now_s() - t0;
    printf("  bulk load of %llu persons rows over 1 M records: %.3f s\n", (unsigned long long) total, bl);
    CHECK(recs == N && total == 999999 && sum == 7ull * 666666, "bulk load saw every row (%llu rows, sum %llu)", (unsigned long long) total, (unsigned long long) sum);
    CHECK(bl < 1.0, "bulk load %.3f s >= 1 s", bl);
    pzpd_close(a);
}

/** @brief Payload of blob (prefix, key number, stream name): deterministic, 100..149 bytes. */
static size_t coll_payload(char prefix, int i, const char *stream, unsigned char *out)
{
    size_t n = 100 + (size_t)(i % 50);
    fill(out, n, (uint64_t) prefix * 1000003ull + (uint64_t) i * 131ull + (uint64_t)(unsigned char) stream[0]);
    return n;
}

/** @brief Write an archive with keys k<from>..k<to-1>, one blob per stream named "<prefix>/k<i>.<stream>". */
static int coll_archive(const char *path, const char **streams, unsigned S, int from, int to, char prefix)
{
    pzpd_writer_opts o = { streams, S, 8192, 64 };
    pzpd_writer *w = pzpd_writer_create(path, &o);
    if (w == NULL) { return 0; }
    unsigned char data[200];
    for (int i = from; i < to; i++)
    {
        char k[32], nm[64];
        int kl = snprintf(k, sizeof(k), "k%d", i);
        if (!pzpd_writer_begin(w, k, (size_t) kl, PZPD_NO_GROUP, 0)) { pzpd_writer_abort(w); return 0; }
        for (unsigned s = 0; s < S; s++)
        {
            int nl = snprintf(nm, sizeof(nm), "%c/k%d.%s", prefix, i, streams[s]);
            size_t n = coll_payload(prefix, i, streams[s], data);
            if (!pzpd_writer_blob(w, s, nm, (size_t) nl, data, n)) { pzpd_writer_abort(w); return 0; }
        }
        if (!pzpd_writer_end(w)) { pzpd_writer_abort(w); return 0; }
    }
    return pzpd_writer_finish(w);
}

/** @brief Check every ordinal of the A+B collection against the generated payloads. */
static void coll_check_all(pzpd *a, const char *what)
{
    int errs = 0;
    unsigned char want[200], buf[256], rec[4096];
    for (uint64_t o = 0; o < 200; o++)
    {
        int inA = (o < 100);
        int key = inA ? (int) o : 50 + (int)(o - 100);
        char prefix = inA ? 'a' : 'b';
        pzpd_blob_ref refs[PZPD_MAX_STREAMS];
        ssize_t r = pzpd_read_record(a, o, 0xFFFFFFFFu, rec, sizeof(rec), refs);
        if (r <= 0) { errs++; continue; }
        for (unsigned u = 0; u < pzpd_stream_count(a); u++)
        {
            const char *sn = pzpd_stream_name(a, u);
            int present = inA ? (!strcmp(sn, "rgb") || !strcmp(sn, "depth")) : (!strcmp(sn, "depth") || !strcmp(sn, "seg") || !strcmp(sn, "extra"));
            pzpd_blob_info bi;
            if (!pzpd_blob_info_get(a, o, u, &bi) || (bi.present != present) || (bi.member != (inA ? 0u : 1u))) { errs++; continue; }
            if (!present)
            {
                size_t vs = 1;
                if ( (pzpd_read_into(a, o, u, buf, sizeof(buf)) != 0) || (pzpd_view(a, o, u, &vs) != NULL) || (vs != 0) || (refs[u].data != NULL) ) { errs++; }
                continue;
            }
            size_t n = coll_payload(prefix, key, sn, want);
            size_t vs = 0;
            const void *v = pzpd_view(a, o, u, &vs);
            if ( (pzpd_read_into(a, o, u, buf, sizeof(buf)) != (ssize_t) n) || memcmp(buf, want, n) ||
                 (v == NULL) || (vs != n) || memcmp(v, want, n) ||
                 (refs[u].size != n) || memcmp(refs[u].data, want, n) ) { errs++; }
        }
    }
    CHECK(errs == 0, "%s: %d mismatches over 200 ordinals x %u streams", what, errs, pzpd_stream_count(a));
}

/** @brief Phase 1b: several archives as one, collection files, missing and stale members. */
static void test_collections(void)
{
    char pa[1200], pb[1200], set[1200], tmpA[1300];
    snprintf(pa, sizeof(pa), "%s/colA.pzpd", dir);
    snprintf(pb, sizeof(pb), "/dev/shm/pzpdir_test_colB.pzpd");
    snprintf(set, sizeof(set), "%s/set.pzpd", dir);
    const char *sa[2] = { "rgb", "depth" }, *sb[3] = { "depth", "seg", "extra" };
    CHECK(coll_archive(pa, sa, 2, 0, 100, 'a') && coll_archive(pb, sb, 3, 50, 150, 'b'), "write members");

    //--- open_many ---------------------------------------------------------
    const char *both[2] = { pa, pb };
    pzpd *a = pzpd_open_many(both, NULL, 2, 0);
    CHECK(a != NULL, "open_many");
    if (a == NULL) { return; }
    CHECK(pzpd_count(a) == 200 && pzpd_member_count(a) == 2, "count = sum of members");
    CHECK(pzpd_stream_count(a) == 4 && !strcmp(pzpd_stream_name(a, 0), "rgb") && !strcmp(pzpd_stream_name(a, 1), "depth") &&
          !strcmp(pzpd_stream_name(a, 2), "seg") && !strcmp(pzpd_stream_name(a, 3), "extra"), "streams merged by name, first-seen order");
    CHECK(!strcmp(pzpd_member_alias(a, 0), "colA") && !strcmp(pzpd_member_alias(a, 1), "pzpdir_test_colB") && pzpd_member_id(a, "colA") == 0, "default aliases");
    coll_check_all(a, "open_many");

    int so = 99;
    CHECK(pzpd_find(a, "k60", 3, &so) == 60 && so == -1, "find returns the first member's match");
    int64_t ords[4]; int sts[4];
    CHECK(pzpd_find_all(a, "k60", 3, ords, sts, 4) == 2 && ords[0] == 60 && ords[1] == 110, "find_all returns both");
    ords[1] = -7;
    CHECK(pzpd_find_all(a, "k60", 3, ords, sts, 1) == 2 && ords[0] == 60 && ords[1] == -7, "find_all with max 1 still reports 2, writes 1");
    CHECK(pzpd_find_in(a, 1, "k60", 3, &so) == 110, "find_in member 1");
    CHECK(pzpd_find(a, "b/k120.seg", 10, &so) == 170 && so == pzpd_stream_id(a, "seg"), "find a blob name, merged stream id");
    CHECK(pzpd_find_in(a, 0, "b/k120.seg", 10, &so) == -1, "find_in wrong member");
    CHECK(pzpd_find(a, "k10", 3, &so) == 10 && pzpd_find(a, "k140", 4, &so) == 190, "keys only in one member");
    uint64_t local = 0, f = 0, c = 0;
    CHECK(pzpd_member_of(a, 110, &local) == 1 && local == 10 && pzpd_member_range(a, 1, &f, &c) && f == 100 && c == 100, "member_of / member_range");
    unsigned shards = pzpd_shard_count(a), bShards = 0;
    for (unsigned sh = 0; sh < shards; sh++)
    {
        pzpd_shard_info si;
        CHECK(pzpd_shard_info_get(a, sh, &si) && si.available, "shard info %u", sh);
        if (si.member == 1) { bShards++; CHECK(si.first_ordinal >= 100 && strstr(si.path, "/dev/shm/") == si.path, "member-1 shards are B's"); }
        CHECK(pzpd_verify_shard(a, sh), "verify shard %u", sh);
    }
    CHECK(shards >= 4 && bShards >= 2, "both members have several shards (%u total, %u of B)", shards, bShards);
    pzpd_blob_info bi;
    CHECK(pzpd_blob_info_get(a, 150, (unsigned) pzpd_stream_id(a, "extra"), &bi) && bi.present && bi.member == 1 && bi.shard >= shards - bShards, "blob_info member and global shard");
    CHECK(pzpd_verify_record(a, 5, 1) && pzpd_verify_record(a, 150, 1), "verify records in both members");
    pzpd_close(a);

    //--- collection file -----------------------------------------------------
    CHECK(pzpd_collection_write(set, both, NULL, 2, 0), "collection_write");
    a = pzpd_open(set, 0);
    CHECK(a != NULL && pzpd_count(a) == 200, "open the collection file");
    if (a != NULL) { coll_check_all(a, "collection file"); pzpd_close(a); }
    {
        size_t len = 0;
        FILE *fp = fopen(set, "rb");
        static char buf[65536];
        len = fread(buf, 1, sizeof(buf), fp);
        fclose(fp);
        CHECK(memmem(buf, len, "colA.pzpd", 9) != NULL && memmem(buf, len, "/dev/shm/pzpdir_test_colB.pzpd", 30) != NULL &&
              memmem(buf, len, dir, strlen(dir)) == NULL, "A stored relative, the /dev/shm member absolute");
    }
    const char *al[2] = { "x", "y" };
    CHECK(pzpd_collection_write(set, both, al, 2, PZPD_COLL_ABSOLUTE), "collection_write with aliases, absolute");
    a = pzpd_open(set, 0);
    CHECK(a != NULL && pzpd_member_id(a, "y") == 1 && !strcmp(pzpd_member_alias(a, 0), "x"), "aliases kept");
    pzpd_close(a);
    CHECK(pzpd_collection_write(set, both, NULL, 2, 0), "collection_write again");

    //--- missing member ----------------------------------------------------------
    snprintf(tmpA, sizeof(tmpA), "%s.hidden", pa);
    CHECK(rename(pa, tmpA) == 0, "hide A's manifest");
    a = pzpd_open(set, 0);
    CHECK(a != NULL && pzpd_count(a) == 200, "collection opens with a missing member, its range reserved");
    if (a != NULL)
    {
        unsigned char buf[256];
        CHECK(pzpd_read_into(a, 5, 0, buf, sizeof(buf)) == PZPD_E_MEMBER_MISSING, "missing member's ordinals fail with PZPD_E_MEMBER_MISSING");
        CHECK(pzpd_read_into(a, 150, 1, buf, sizeof(buf)) > 0, "other member's ordinals unchanged");
        CHECK(pzpd_find(a, "k60", 3, NULL) == 110, "lookups skip the missing member");
        pzpd_close(a);
    }
    CHECK(pzpd_open_many(both, NULL, 2, 0) == NULL, "open_many fails on a missing member");
    a = pzpd_open_many(both, NULL, 2, PZPD_O_ALLOW_MISSING);
    CHECK(a != NULL && pzpd_count(a) == 100 && pzpd_find(a, "k60", 3, NULL) == 10, "PZPD_O_ALLOW_MISSING: missing member counts as 0 records");
    pzpd_close(a);
    CHECK(rename(tmpA, pa) == 0, "restore A");

    //--- stale member + refresh ------------------------------------------------
    CHECK(coll_archive(pb, sb, 3, 50, 160, 'b'), "rebuild B with 110 records");
    a = pzpd_open(set, 0);
    CHECK(a == NULL && pzpd_last_error_code() == PZPD_E_STALE_COLLECTION, "rebuilt member detected as stale");
    pzpd_close(a);
    CHECK(pzpd_collection_refresh(set), "collection_refresh");
    a = pzpd_open(set, 0);
    CHECK(a != NULL && pzpd_count(a) == 210 && pzpd_find(a, "k155", 4, NULL) == 205, "refreshed collection sees the new member");
    pzpd_close(a);

    //--- rejections --------------------------------------------------------------
    const char *twice[2] = { pa, pa };
    CHECK(pzpd_open_many(twice, NULL, 2, 0) == NULL && pzpd_last_error_code() == PZPD_E_ARG, "duplicate default aliases rejected");
    a = pzpd_open_many(twice, al, 2, 0);
    CHECK(a != NULL && pzpd_count(a) == 200 && pzpd_find_all(a, "k1", 2, NULL, NULL, 0) == 2, "same archive twice under two aliases");
    pzpd_close(a);
    const char *nested[2] = { set, pa };
    CHECK(pzpd_open_many(nested, NULL, 2, 0) == NULL, "collections can't be nested");
    CHECK(pzpd_collection_write(set, nested, NULL, 2, 0) == 0, "collection_write refuses a collection member");

    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "rm -f /dev/shm/pzpdir_test_colB*");
    CHECK(system(cmd) == 0, "clean /dev/shm");
}

//-----------------------------------------------------------------------------------------------
// Phase 6: prefetcher
//-----------------------------------------------------------------------------------------------

/** @brief Shared state of the strided prefetch consumers. */
struct pf_job
{
    pzpd            *a;        ///< Handle
    pzpd_prefetcher *pf;       ///< Prefetcher
    const uint64_t  *ord;      ///< Schedule ordinals
    const uint32_t  *mask;     ///< Schedule masks
    size_t           n;        ///< Schedule length
    unsigned         t, T;     ///< This consumer, consumers
    int              errs;     ///< Mismatches found
    uint64_t         gets;     ///< Gets done
    uint64_t         discards; ///< Discards done
};

/** @brief Consumer: positions t, t+T, ...; every 37th is discarded, the others are got, compared with
 *  pread() copies (pzpd_read_into) and released. */
static void *pf_consumer(void *arg)
{
    struct pf_job *j = (struct pf_job *) arg;
    unsigned S = pzpd_stream_count(j->a);
    static __thread unsigned char buf[4096];
    for (size_t pos = j->t; pos < j->n; pos += j->T)
    {
        if (pos % 37 == 5) { pzpd_prefetch_discard(j->pf, j->ord[pos]); j->discards++; continue; }
        pzpd_blob_ref refs[PZPD_MAX_STREAMS];
        pzpd_ticket tk;
        int r = pzpd_prefetch_get(j->pf, j->ord[pos], j->mask[pos], refs, &tk);
        j->gets++;
        if (r < 0) { j->errs++; continue; }
        int present = 0;
        for (unsigned u = 0; u < S; u++)
        {
            ssize_t n = pzpd_read_into(j->a, j->ord[pos], u, buf, sizeof(buf));
            if ( !(j->mask[pos] & (1u << u)) || (n <= 0) )
            {
                if ( (refs[u].data != NULL) && ((j->mask[pos] & (1u << u)) == 0) ) { j->errs++; }
                if ( (n == 0) && (j->mask[pos] & (1u << u)) && (refs[u].size != 0) ) { j->errs++; }
                if ( (n == 0) && (refs[u].data != NULL) ) { present++; }   // present, empty blob
                continue;
            }
            pzpd_blob_info bi;
            present++;
            if ( (refs[u].data == NULL) || (refs[u].size != (size_t) n) || memcmp(refs[u].data, buf, (size_t) n) ||
                 !pzpd_blob_info_get(j->a, j->ord[pos], u, &bi) || (refs[u].format != bi.meta.format) ) { j->errs++; }
        }
        if (present != r) { j->errs++; }
        pzpd_prefetch_release(j->pf, &tk);
    }
    return NULL;
}

/** @brief Run T strided consumers over a schedule. @return Mismatches; gets / discards added to the out counters. */
static int pf_run(pzpd *a, pzpd_prefetcher *pf, const uint64_t *ord, const uint32_t *mask, size_t n, unsigned T, uint64_t *gets, uint64_t *discards)
{
    struct pf_job j[16];
    pthread_t th[16];
    int errs = 0;
    for (unsigned t = 0; t < T; t++)
    {
        j[t] = (struct pf_job) { a, pf, ord, mask, n, t, T, 0, 0, 0 };
        pthread_create(&th[t], NULL, pf_consumer, &j[t]);
    }
    for (unsigned t = 0; t < T; t++) { pthread_join(th[t], NULL); errs += j[t].errs; *gets += j[t].gets; *discards += j[t].discards; }
    return errs;
}

/** @brief Phase 6: prefetcher schedule semantics, window, clear, errors, AUTO per member, open flags. */
static void test_prefetch(void)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/rt.pzpd", dir);
    pzpd *a = pzpd_open(path, 0);
    CHECK(a != NULL, "open rt.pzpd");
    if (a == NULL) { return; }
    uint64_t N = pzpd_count(a);
    unsigned S = pzpd_stream_count(a);

    //--- options ---------------------------------------------------------------
    pzpd_prefetch_opts bo = { 0, 0, 9, 0, 0 };
    CHECK(pzpd_prefetcher_create(a, &bo) == NULL, "unknown mode rejected");
    CHECK(pzpd_prefetcher_create(NULL, NULL) == NULL, "NULL handle rejected");

    //--- a shuffled epoch with duplicates and per-ordinal masks, 8 strided consumers, both modes ---
    size_t n = 20000;
    uint64_t *ord = (uint64_t *) malloc(n * sizeof(uint64_t));
    uint32_t *mask = (uint32_t *) malloc(n * sizeof(uint32_t));
    uint64_t x = 12345;
    for (size_t i = 0; i < n; i++)
    {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        ord[i]  = (i % 40 == 3) ? ord[i / 2] : x % N;           // ~2.5 % repeats of earlier ordinals
        mask[i] = (uint32_t)(1 + (x >> 20) % 7);                // any non-empty subset of the 3 streams
    }
    for (int mode = PZPD_PF_MAP; mode <= PZPD_PF_BUFFERS; mode++)
    {
        pzpd_prefetch_opts o = { 0, 3, (unsigned) mode, 0, 64 };
        pzpd_prefetcher *pf = pzpd_prefetcher_create(a, &o);
        CHECK(pf != NULL, "create mode %d", mode);
        if (pf == NULL) { continue; }
        CHECK(pzpd_prefetch_submit(pf, ord, mask, n), "submit %zu", n);
        uint64_t gets = 0, discards = 0;
        int errs = pf_run(a, pf, ord, mask, n, 8, &gets, &discards);
        pzpd_prefetch_stats st;
        pzpd_prefetch_stats_get(pf, &st);
        CHECK(errs == 0, "mode %d: %d mismatches between prefetched views and pread copies", mode, errs);
        CHECK(st.submitted == n && st.hits + st.waits + st.sync_misses + st.unscheduled == gets && st.released == gets &&
              st.discarded == discards && st.unscheduled == 0, "mode %d: counters add up (hits %llu waits %llu sync %llu unsched %llu released %llu discarded %llu)",
              mode, (unsigned long long) st.hits, (unsigned long long) st.waits, (unsigned long long) st.sync_misses,
              (unsigned long long) st.unscheduled, (unsigned long long) st.released, (unsigned long long) st.discarded);
        CHECK(st.prefetched > 0 && st.bytes_prefetched > 0, "mode %d: I/O threads prefetched %llu records", mode, (unsigned long long) st.prefetched);
        CHECK( ((mode == PZPD_PF_MAP) ? st.shards_map : (mode == PZPD_PF_PAGECACHE) ? st.shards_pagecache : st.shards_buffers) == pzpd_shard_count(a), "mode %d applied to every shard", mode);
        CHECK(st.buffer_bytes == 0 && ((mode == PZPD_PF_BUFFERS) ? (st.buffer_bytes_peak > 0) : (st.buffer_bytes_peak == 0)), "mode %d: no buffer memory left (peak %llu)", mode, (unsigned long long) st.buffer_bytes_peak);
        pzpd_prefetcher_destroy(pf);
    }

    //--- the window bounds prefetching while claims are held -------------------
    {
        pzpd_prefetch_opts o = { 0, 2, 0, 0, 8 };
        pzpd_prefetcher *pf = pzpd_prefetcher_create(a, &o);
        CHECK(pf != NULL && pzpd_prefetch_submit(pf, ord, NULL, 100), "submit 100 with window 8");
        pzpd_ticket tk[8];
        pzpd_blob_ref refs[PZPD_MAX_STREAMS];
        usleep(100000);
        pzpd_prefetch_stats st;
        pzpd_prefetch_stats_get(pf, &st);
        CHECK(st.prefetched == 8, "window 8 filled: %llu prefetched", (unsigned long long) st.prefetched);
        int ok = 1;
        for (int k = 0; k < 8; k++) { ok = ok && (pzpd_prefetch_get(pf, ord[k], 0xFFFFFFFFu, refs, &tk[k]) >= 0); }
        usleep(100000);
        pzpd_prefetch_stats_get(pf, &st);
        CHECK(ok && st.hits == 8 && st.prefetched == 8 && st.producer_stalls > 0, "8 held claims keep the window full (prefetched %llu, stalls %llu)",
              (unsigned long long) st.prefetched, (unsigned long long) st.producer_stalls);
        for (int k = 0; k < 8; k++) { pzpd_prefetch_release(pf, &tk[k]); }
        pzpd_prefetch_release(pf, &tk[0]);                        // second release: no effect
        for (int k = 8; k < 100; k++)
        {
            pzpd_ticket t;
            ok = ok && (pzpd_prefetch_get(pf, ord[k], 0xFFFFFFFFu, refs, &t) >= 0);
            pzpd_prefetch_release(pf, &t);
        }
        pzpd_prefetch_stats_get(pf, &st);
        CHECK(ok && st.released == 100 && st.hits + st.waits + st.sync_misses == 100, "all 100 got and released once (released %llu)", (unsigned long long) st.released);

        //--- clear mid-schedule: old tickets are ignored, a new schedule works --
        CHECK(pzpd_prefetch_submit(pf, ord, mask, 1000), "submit 1000");
        pzpd_ticket old;
        CHECK(pzpd_prefetch_get(pf, ord[0], mask[0], refs, &old) >= 0, "get before clear");
        pzpd_prefetch_clear(pf);
        pzpd_prefetch_stats_get(pf, &st);
        uint64_t rel = st.released;
        pzpd_prefetch_release(pf, &old);
        pzpd_prefetch_stats_get(pf, &st);
        CHECK(st.released == rel, "a ticket from before clear is ignored");
        pzpd_prefetch_discard(pf, ord[5]);
        pzpd_prefetch_stats_get(pf, &st);
        CHECK(st.discarded == 0, "discard after clear: nothing pending");
        CHECK(pzpd_prefetch_submit(pf, ord + 1000, mask + 1000, 500), "resubmit after clear");
        uint64_t gets = 0, discards = 0;
        CHECK(pf_run(a, pf, ord + 1000, mask + 1000, 500, 4, &gets, &discards) == 0, "schedule after clear reads correctly");

        //--- unscheduled gets, masks beyond the submitted one, errors ----------
        uint64_t unscheduled = N - 1;
        pzpd_ticket t;
        int r = pzpd_prefetch_get(pf, unscheduled, 0xFFFFFFFFu, refs, &t);
        pzpd_prefetch_stats_get(pf, &st);
        CHECK(r >= 0 && st.unscheduled >= 1 && t.pos == UINT64_MAX, "get of an unsubmitted ordinal reads on demand");
        pzpd_prefetch_release(pf, &t);
        uint64_t one = 7;
        uint32_t m1 = 1;
        CHECK(pzpd_prefetch_submit(pf, &one, &m1, 1), "submit rgb only");
        r = pzpd_prefetch_get(pf, 7, 0xFFFFFFFFu, refs, &t);
        int all = 1;
        for (unsigned u = 0; u < S; u++) { ssize_t k = pzpd_read_into(a, 7, u, NULL, 0); if ( (expected_size(7, u) > 0) && (refs[u].data == NULL) ) { all = 0; } (void) k; }
        CHECK(r >= 1 && all, "a get may ask for more streams than were submitted");
        pzpd_prefetch_release(pf, &t);
        uint64_t bad[2] = { 1, N };
        pzpd_prefetch_stats_get(pf, &st);
        uint64_t sub = st.submitted;
        CHECK(!pzpd_prefetch_submit(pf, bad, NULL, 2) && pzpd_last_error_code() == PZPD_E_ARG, "out-of-range ordinal rejected");
        pzpd_prefetch_stats_get(pf, &st);
        CHECK(st.submitted == sub, "a rejected submit appends nothing");
        CHECK(pzpd_prefetch_get(pf, N, 1, refs, &t) < 0 && t.pos == UINT64_MAX, "get of an out-of-range ordinal fails");
        CHECK(pzpd_prefetch_submit(pf, ord, mask, 5000), "submit, then destroy with queued, in-flight and held claims");
        CHECK(pzpd_prefetch_get(pf, ord[0], mask[0], refs, &t) >= 0, "hold one claim");
        pzpd_prefetcher_destroy(pf);                              // ASan: no leak, no use after free
    }
    //--- BUFFERS: the budget bounds prefetching; held buffers survive clear; wider masks re-read ---
    {
        pzpd_prefetch_opts o = { 0, 2, PZPD_PF_BUFFERS, 16384, 64 };
        pzpd_prefetcher *pf = pzpd_prefetcher_create(a, &o);
        CHECK(pf != NULL && pzpd_prefetch_submit(pf, ord, NULL, 200), "BUFFERS: submit 200 with a 16 KiB budget, window 64");
        usleep(100000);
        pzpd_prefetch_stats st;
        pzpd_prefetch_stats_get(pf, &st);
        CHECK(st.prefetched < 64 && st.prefetched >= 1 && st.buffer_bytes <= 16384 && st.buffer_bytes_peak <= 16384 && st.producer_stalls > 0,
              "the budget, not the window, stops the I/O threads (%llu prefetched, %llu bytes)", (unsigned long long) st.prefetched, (unsigned long long) st.buffer_bytes);
        pzpd_blob_ref refs[PZPD_MAX_STREAMS];
        pzpd_ticket held;
        unsigned char buf[700];
        int r = pzpd_prefetch_get(pf, ord[0], 0xFFFFFFFFu, refs, &held);
        pzpd_prefetch_clear(pf);
        int same = (r >= 0);
        for (unsigned u = 0; same && (u < S); u++)
        {
            ssize_t k = pzpd_read_into(a, ord[0], u, buf, sizeof(buf));
            if ( (k > 0) && ((refs[u].size != (size_t) k) || memcmp(refs[u].data, buf, (size_t) k)) ) { same = 0; }
        }
        CHECK(same && held.buf != NULL, "a BUFFERS ticket stays readable after clear");
        pzpd_prefetch_release(pf, &held);
        pzpd_prefetch_stats_get(pf, &st);
        CHECK(st.buffer_bytes == 0, "releasing it after clear frees its memory");
        uint64_t one = 11;
        uint32_t m1 = 1;
        pzpd_ticket t;
        CHECK(pzpd_prefetch_submit(pf, &one, &m1, 1), "BUFFERS: submit rgb only");
        usleep(20000);
        r = pzpd_prefetch_get(pf, 11, 0xFFFFFFFFu, refs, &t);
        same = (r >= 0);
        for (unsigned u = 0; same && (u < S); u++)
        {
            ssize_t k = pzpd_read_into(a, 11, u, buf, sizeof(buf));
            if ( (k > 0) && ((refs[u].data == NULL) || (refs[u].size != (size_t) k) || memcmp(refs[u].data, buf, (size_t) k)) ) { same = 0; }
            if ( (expected_size(11, u) == 0) && (refs[u].data == NULL) ) { same = 0; }
        }
        CHECK(same, "BUFFERS: a get wider than the submitted mask reads the rest");
        pzpd_prefetch_release(pf, &t);
        pzpd_prefetch_discard(pf, 11);
        CHECK(pzpd_prefetch_submit(pf, ord, NULL, 300), "BUFFERS: submit, then destroy with prefetched buffers left");
        usleep(20000);
        pzpd_prefetcher_destroy(pf);                              // ASan: prefetched buffers freed
    }
    free(ord);
    free(mask);

    //--- open flags ---------------------------------------------------------------
    long residentBefore = resident_file_pages();
    pzpd *h = pzpd_open(path, PZPD_O_POPULATE | PZPD_O_HUGEPAGE);
    long residentAfter = resident_file_pages();
    CHECK(h != NULL, "open with PZPD_O_POPULATE | PZPD_O_HUGEPAGE");
    if (h != NULL)
    {
        // POPULATE pre-faults every shard at open, before any read: at least half the archive is mapped in by now
        uint64_t archiveBytes = 0;
        for (unsigned k = 0; k < pzpd_shard_count(h); k++) { pzpd_shard_info si; if (pzpd_shard_info_get(h, k, &si)) { archiveBytes += si.file_bytes; } }
        long pageBytes = sysconf(_SC_PAGESIZE);
        CHECK( (residentBefore >= 0) && ((uint64_t)(residentAfter - residentBefore) * (uint64_t) pageBytes >= archiveBytes / 2),
               "PZPD_O_POPULATE reaches the archive: its shards are resident right after pzpd_open()");
        int errs = 0;
        unsigned char buf[700];
        for (uint64_t i = 0; i < N; i += 997)
        {
            for (unsigned u = 0; u < S; u++)
            {
                size_t vs = 0;
                const void *v = pzpd_view(h, i, u, &vs);
                ssize_t k = pzpd_read_into(a, i, u, buf, sizeof(buf));
                if ( (k != (ssize_t) vs) || ((k > 0) && memcmp(v, buf, (size_t) k)) ) { errs++; }
            }
        }
        CHECK(errs == 0, "reads through a populated mapping");
        pzpd_close(h);
    }
    pzpd_close(a);

    //--- AUTO per member: a block-device member and a /dev/shm member -----------------
    char pa[1200], pb[1200];
    snprintf(pa, sizeof(pa), "%s/pfA.pzpd", dir);
    snprintf(pb, sizeof(pb), "/dev/shm/pzpdir_test_pfB.pzpd");
    const char *sa[2] = { "rgb", "depth" }, *sb[3] = { "depth", "seg", "extra" };
    CHECK(coll_archive(pa, sa, 2, 0, 100, 'a') && coll_archive(pb, sb, 3, 50, 150, 'b'), "write members");
    const char *both[2] = { pa, pb };
    pzpd *c = pzpd_open_many(both, NULL, 2, 0);
    CHECK(c != NULL, "open_many");
    if (c != NULL)
    {
        CHECK(pzpd_storage_kind(c, 150) == PZPD_STORAGE_RAM, "a /dev/shm shard is RAM storage");
        CHECK(pzpd_storage_kind(c, 200) == PZPD_E_ARG, "storage_kind of an out-of-range ordinal fails");
        unsigned wantMap = 0, wantPc = 0, okModes = 1;
        for (unsigned sh = 0; sh < pzpd_shard_count(c); sh++)
        {
            pzpd_shard_info si;
            pzpd_shard_info_get(c, sh, &si);
            int m = pzpd_prefetch_auto_mode(c, sh);
            if (si.storage == PZPD_STORAGE_RAM) { wantMap++; okModes = okModes && (m == PZPD_PF_MAP) && (si.member == 1); }
            else { wantPc++; okModes = okModes && (m == PZPD_PF_PAGECACHE); }
        }
        CHECK(okModes && wantMap > 0, "AUTO: MAP for the /dev/shm member's shards, PAGECACHE for small block-device members");
        pzpd_prefetcher *pf = pzpd_prefetcher_create(c, NULL);
        pzpd_prefetch_stats st;
        pzpd_prefetch_stats_get(pf, &st);
        CHECK(pf != NULL && st.shards_map == wantMap && st.shards_pagecache == wantPc && st.shards_buffers == 0, "AUTO applied per shard (map %u, pagecache %u)", st.shards_map, st.shards_pagecache);
        uint64_t all[200];
        for (int i = 0; i < 200; i++) { all[i] = (uint64_t)((i * 7) % 200); }
        CHECK(pzpd_prefetch_submit(pf, all, NULL, 200), "submit a permutation of the collection");
        int errs = 0;
        unsigned char want[200];
        for (int i = 0; i < 200; i++)
        {
            pzpd_blob_ref refs[PZPD_MAX_STREAMS];
            pzpd_ticket t;
            uint64_t o = all[i];
            int r = pzpd_prefetch_get(pf, o, 0xFFFFFFFFu, refs, &t);
            int inA = (o < 100), key = inA ? (int) o : 50 + (int)(o - 100);
            if (r != (inA ? 2 : 3)) { errs++; }
            for (unsigned u = 0; u < pzpd_stream_count(c); u++)
            {
                if (refs[u].data == NULL) { continue; }
                size_t k = coll_payload(inA ? 'a' : 'b', key, pzpd_stream_name(c, u), want);
                if ( (refs[u].size != k) || memcmp(refs[u].data, want, k) ) { errs++; }
            }
            pzpd_prefetch_release(pf, &t);
        }
        CHECK(errs == 0, "prefetched views of both members match their payloads");
        pzpd_prefetcher_destroy(pf);

        // BUFFERS on both members (tmpfs may refuse O_DIRECT: then buffered reads, counted)
        pzpd_prefetch_opts bo2 = { 0, 2, PZPD_PF_BUFFERS, 0, 16 };
        pf = pzpd_prefetcher_create(c, &bo2);
        CHECK(pf != NULL && pzpd_prefetch_submit(pf, all, NULL, 200), "BUFFERS over a disk + /dev/shm collection");
        errs = 0;
        for (int i = 0; (pf != NULL) && (i < 200); i++)
        {
            pzpd_blob_ref refs[PZPD_MAX_STREAMS];
            pzpd_ticket t;
            uint64_t o = all[i];
            int r = pzpd_prefetch_get(pf, o, 0xFFFFFFFFu, refs, &t);
            int inA = (o < 100), key = inA ? (int) o : 50 + (int)(o - 100);
            if (r != (inA ? 2 : 3)) { errs++; }
            for (unsigned u = 0; u < pzpd_stream_count(c); u++)
            {
                if (refs[u].data == NULL) { continue; }
                size_t k = coll_payload(inA ? 'a' : 'b', key, pzpd_stream_name(c, u), want);
                if ( (refs[u].size != k) || memcmp(refs[u].data, want, k) ) { errs++; }
            }
            pzpd_prefetch_release(pf, &t);
        }
        pzpd_prefetch_stats_get(pf, &st);
        printf("  BUFFERS on disk + /dev/shm: O_DIRECT refused for %u of %u shards\n", st.direct_fallbacks, st.shards_buffers);
        CHECK(errs == 0 && st.buffer_bytes == 0, "BUFFERS reads of both members match their payloads");
        pzpd_prefetcher_destroy(pf);
        pzpd_close(c);
    }
    unlink(pb);
    for (int k = 0; k < 100000; k++)
    {
        char shard[1300];
        snprintf(shard, sizeof(shard), "/dev/shm/pzpdir_test_pfB.%05d.pzpd", k);
        if (unlink(shard) != 0) { break; }
    }
}

//-----------------------------------------------------------------------------------------------
// Phase 2: recovery
//-----------------------------------------------------------------------------------------------

/** @brief Copy a file. @return 1 on success. */
static int copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
    int ok = (a != NULL) && (b != NULL);
    char buf[65536];
    size_t n;
    while (ok && ((n = fread(buf, 1, sizeof(buf), a)) > 0)) { ok = (fwrite(buf, 1, n, b) == n); }
    if (a) { fclose(a); }
    if (b && (fclose(b) != 0)) { ok = 0; }
    return ok;
}

/** @brief Overwrite [off, off+len) of a file with a byte value (len 0 = to the end). */
static void damage(const char *path, uint64_t off, uint64_t len, int byte)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) { return; }
    struct stat st;
    fstat(fd, &st);
    if ( (len == 0) || (off + len > (uint64_t) st.st_size) ) { len = (uint64_t) st.st_size - off; }
    unsigned char *z = (unsigned char *) malloc(len ? len : 1);
    memset(z, byte, len);
    if (pwrite(fd, z, len, (off_t) off) < 0) { perror("pwrite"); }
    free(z);
    close(fd);
}

/** @brief Start of the index area of a shard: the first 4 KiB block holding a section magic. */
static uint64_t index_start(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) { return 0; }
    char m[8];
    uint64_t off = 4096, found = 0;
    while (!found && (fseek(f, (long) off, SEEK_SET) == 0) && (fread(m, 1, 8, f) == 8)) { if (memcmp(m, "PZPDSECT", 8) == 0) { found = off; } off += 4096; }
    fclose(f);
    return found;
}

/** @brief Write the recovery test archive: 300 records × 2 streams (relative names), a global joints table,
 *  persons (with a str column) and a bulk vec table; 16 KiB shards. */
static int rec_archive(const char *path, const char *prefix)
{
    const char *streams[2] = { "rgb", "depth" };
    pzpd_writer_opts o = { streams, 2, 16384, 64 };
    pzpd_writer *w = pzpd_writer_create(path, &o);
    if (w == NULL) { return 0; }
    int ok = (pzpd_writer_table(w, "joints", "name:str parent:u16", PZPD_TABLE_GLOBAL) == 0) &&
             (pzpd_writer_table(w, "persons", "id:u16 label:str kp:u16[6]", 0) == 1) &&
             (pzpd_writer_table(w, "vec", "v:f32[4]", PZPD_TABLE_BULK) == 2) &&
             pzpd_writer_global_rows_csv(w, 0, "head,0\n\"l, eye\",0\n", 18);
    unsigned char data[900];
    for (int i = 0; ok && (i < 300); i++)
    {
        char k[64], nm[96], csv[256];
        int kl = snprintf(k, sizeof(k), "%s/k%03d", prefix, i);
        ok = pzpd_writer_begin(w, k, (size_t) kl, PZPD_NO_GROUP, 0);
        for (unsigned s2 = 0; ok && (s2 < 2); s2++)
        {
            if ( (s2 == 1) && (i % 4 == 3) ) { continue; }          // some records lack depth
            int nl = snprintf(nm, sizeof(nm), "%s/%s/k%03d.%s", prefix, streams[s2], i, s2 ? "png" : "jpg");
            size_t n = 100 + (size_t)((i * 37 + (int) s2 * 11) % 700);
            fill(data, n, (uint64_t) i * 2 + s2 + 7);
            ok = pzpd_writer_blob(w, s2, nm, (size_t) nl, data, n);
        }
        for (int p = 0; ok && (p < i % 3); p++)
        {
            int cl = snprintf(csv, sizeof(csv), "%d,\"person %d, \"\"x\"\"\",%d,%d,2,%d,%d,1", p, i, i, p, i + 1, 65535 - i);
            ok = pzpd_writer_rows_csv(w, 1, csv, (size_t) cl);
        }
        if (ok && (i % 2 == 0)) { int cl = snprintf(csv, sizeof(csv), "%d.5,-1,0,3e+38", i); ok = pzpd_writer_rows_csv(w, 2, csv, (size_t) cl); }
        ok = ok && pzpd_writer_end(w);
    }
    if (!ok) { pzpd_writer_abort(w); return 0; }
    return pzpd_writer_finish(w);
}

/** @brief State for the salvage comparison callback. */
struct rec_check
{
    pzpd    *orig;      ///< The undamaged archive
    int      errs;      ///< Mismatches
    uint64_t records;   ///< Records seen
    int      csv;       ///< 1 if row CSV is expected (schemas given)
    uint64_t seen[400]; ///< Ordinals seen (count)
};

/** @brief pzpd_salvage() callback: compare a recovered record with the original archive. */
static int rec_cb(const pzpd_salvaged_record *r, void *user)
{
    struct rec_check *c = (struct rec_check *) user;
    c->records++;
    int so = 0;
    int64_t o = pzpd_find(c->orig, r->key, r->key_len, &so);
    if ( (o < 0) || (so != -1) ) { c->errs++; return 1; }
    c->seen[o]++;
    unsigned present = 0;
    for (unsigned u = 0; u < pzpd_stream_count(c->orig); u++)
    {
        pzpd_blob_info bi;
        if (pzpd_blob_info_get(c->orig, (uint64_t) o, u, &bi) && bi.present) { present++; }
    }
    if (r->blob_count != present) { c->errs++; }
    for (unsigned i = 0; i < r->blob_count; i++)
    {
        const pzpd_salvaged_blob *b = &r->blobs[i];
        pzpd_blob_info bi;
        size_t vs = 0;
        const void *v = pzpd_view(c->orig, (uint64_t) o, b->stream, &vs);
        if ( !b->intact || !pzpd_blob_info_get(c->orig, (uint64_t) o, b->stream, &bi) || !bi.present || (bi.name_len != b->name_len) ||
             memcmp(bi.name, b->name, b->name_len) || (vs != b->size) || memcmp(v, b->data, vs) || memcmp(&bi.meta, &b->meta, sizeof(bi.meta)) ) { c->errs++; }
        if ( c->csv && ((b->stream_name == NULL) || strcmp(b->stream_name, pzpd_stream_name(c->orig, b->stream))) ) { c->errs++; }
        if ( !c->csv && (b->stream_name != NULL) ) { c->errs++; }
    }
    // Non-bulk rows: persons (table 1) only; vec (bulk) and joints (global) are never copied
    char want[4096];
    ssize_t wl = pzpd_table_csv(c->orig, (uint64_t) o, 1, want, sizeof(want));
    int found = 0;
    for (unsigned t = 0; t < r->table_count; t++)
    {
        if (r->tables[t].table != 1) { c->errs++; continue; }
        found = 1;
        if (c->csv && ( (r->tables[t].csv == NULL) || ((ssize_t) r->tables[t].csv_len != wl) || memcmp(r->tables[t].csv, want, (size_t) wl) || strcmp(r->tables[t].table_name, "persons") )) { c->errs++; }
        if (!c->csv && (r->tables[t].csv != NULL)) { c->errs++; }
    }
    if (found != (wl > 0)) { c->errs++; }
    return 1;
}

/** @brief Phase 2: manifest rebuild, superblock fallbacks, missing shards, salvage. */
static void test_recovery(void)
{
    char src[1200], rd[1200], path[1300], man[1300], orig[1300];
    snprintf(src, sizeof(src), "%s/rec", dir);
    snprintf(rd, sizeof(rd), "%s/rec_damaged", dir);
    mkdir(src, 0755);
    mkdir(rd, 0755);
    snprintf(orig, sizeof(orig), "%s/r.pzpd", src);
    CHECK(rec_archive(orig, "img"), "write the recovery archive");
    pzpd *a = pzpd_open(orig, 0);
    CHECK(a != NULL, "open it");
    if (a == NULL) { return; }
    unsigned nsh = pzpd_shard_count(a);
    CHECK(nsh >= 4, "%u shards", nsh);
    char **shards = (char **) calloc(nsh, sizeof(char *));
    for (unsigned k = 0; k < nsh; k++)
    {
        shards[k] = (char *) malloc(1400);
        snprintf(shards[k], 1400, "%s/r.%05u.pzpd", rd, k);
        snprintf(path, sizeof(path), "%s/r.%05u.pzpd", src, k);
        CHECK(copy_file(path, shards[k]), "copy shard %u", k);
    }
    snprintf(man, sizeof(man), "%s/r.pzpd", rd);

    //--- rebuild-manifest: byte-identical; errors -------------------------------------
    const char **sp = (const char **) calloc(nsh, sizeof(char *));
    for (unsigned k = 0; k < nsh; k++) { sp[(k * 3) % nsh == k ? k : k] = shards[nsh - 1 - k]; }   // any order
    CHECK(pzpd_manifest_rebuild(man, sp, nsh), "rebuild the manifest from shards given in reverse order");
    {
        FILE *f1 = fopen(orig, "rb"), *f2 = fopen(man, "rb");
        int same = (f1 != NULL) && (f2 != NULL);
        int c1 = 0, c2 = 0;
        while (same && ((c1 = fgetc(f1)) != EOF)) { c2 = fgetc(f2); if (c1 != c2) { same = 0; } }
        if (same && (fgetc(f2) != EOF)) { same = 0; }
        if (f1) { fclose(f1); }
        if (f2) { fclose(f2); }
        CHECK(same, "the rebuilt manifest is byte-identical to the original");
    }
    CHECK(!pzpd_manifest_rebuild(man, sp, nsh - 1), "a missing shard is reported");
    char other[1300], wrongDir[1300];
    snprintf(other, sizeof(other), "%s/o.pzpd", rd);
    CHECK(rec_archive(other, "img"), "write a second archive");
    snprintf(path, sizeof(path), "%s/o.00000.pzpd", rd);
    const char *mixed[2] = { shards[0], path };
    CHECK(!pzpd_manifest_rebuild(other, mixed, 2), "shards of two archives are rejected");
    snprintf(wrongDir, sizeof(wrongDir), "%s/r_elsewhere.pzpd", dir);
    CHECK(!pzpd_manifest_rebuild(wrongDir, sp, nsh), "a manifest outside the shards' directory is rejected");
    CHECK(!pzpd_manifest_rebuild(man, NULL, 0), "no shards: rejected");

    //--- superblock ladder: primary → backup → index sections --------------------------
    struct stat st;
    damage(shards[1], 0, 64, 0x5A);                                  // primary superblock
    stat(shards[2], &st);
    damage(shards[2], 0, 4096, 0);                                   // both superblocks
    damage(shards[2], (uint64_t) st.st_size - 4096, 4096, 0);
    pzpd *d = pzpd_open(man, 0);
    CHECK(d != NULL, "open with damaged superblocks");
    if (d != NULL)
    {
        pzpd_shard_info si[3];
        for (unsigned k = 0; k < 3; k++) { pzpd_shard_info_get(d, k, &si[k]); }
        CHECK(si[0].recovery == 0 && si[1].recovery == 1 && si[2].recovery == 2, "recovery 0 / 1 / 2 reported (%d %d %d)", si[0].recovery, si[1].recovery, si[2].recovery);
        int errs = 0;
        char c1[4096], c2[4096];
        for (uint64_t i = 0; i < pzpd_count(a); i++)
        {
            size_t kl1, kl2;
            const char *k1 = pzpd_record_key(a, i, &kl1), *k2 = pzpd_record_key(d, i, &kl2);
            if ( (k2 == NULL) || (kl1 != kl2) || memcmp(k1, k2, kl1) || (pzpd_find(d, k1, kl1, NULL) != (int64_t) i) ) { errs++; continue; }
            for (unsigned u = 0; u < 2; u++)
            {
                size_t v1 = 0, v2 = 0;
                const void *p1 = pzpd_view(a, i, u, &v1), *p2 = pzpd_view(d, i, u, &v2);
                if ( (v1 != v2) || ((p1 == NULL) != (p2 == NULL)) || (v1 && memcmp(p1, p2, v1)) ) { errs++; }
            }
            ssize_t l1 = pzpd_table_csv(a, i, 1, c1, sizeof(c1)), l2 = pzpd_table_csv(d, i, 1, c2, sizeof(c2));
            if ( (l1 != l2) || (l1 > 0 && memcmp(c1, c2, (size_t) l1)) ) { errs++; }
        }
        CHECK(errs == 0, "every record, lookup and table row reads the same through the fallbacks (%d mismatches)", errs);
        CHECK(pzpd_verify_shard(d, 1) && pzpd_verify_shard(d, 2), "index checksums of recovered shards verify");
        pzpd_close(d);
    }
    pzpd *one = pzpd_open(shards[2], 0);
    pzpd_shard_info s2;
    pzpd_shard_info o2;
    pzpd_shard_info_get(a, 2, &o2);
    CHECK(one != NULL && pzpd_shard_info_get(one, 0, &s2) && s2.recovery == 2 && pzpd_count(one) == o2.record_count &&
          !strcmp(pzpd_stream_name(one, 1), "depth") && pzpd_table_count(one) == 3, "a shard without superblocks opens standalone (streams, tables from its sections)");
    pzpd_close(one);

    //--- a deleted shard fails only its own ordinals ------------------------------------
    snprintf(path, sizeof(path), "%s.hidden", shards[3]);
    rename(shards[3], path);
    d = pzpd_open(man, 0);
    if (d != NULL)
    {
        pzpd_shard_info s3;
        pzpd_shard_info_get(d, 3, &s3);
        int bad = 0, missing = 0;
        unsigned char buf[1000];
        for (uint64_t i = 0; i < pzpd_count(d); i++)
        {
            ssize_t r = pzpd_read_into(d, i, 0, buf, sizeof(buf));
            int inK = (i >= s3.first_ordinal) && (i < s3.first_ordinal + s3.record_count);
            if (inK) { missing += (r == PZPD_E_SHARD_MISSING); } else if (r <= 0) { bad++; }
        }
        CHECK(bad == 0 && missing == (int) s3.record_count && s3.record_count > 0, "deleted shard: %d others failed, %d of %llu of its ordinals report PZPD_E_SHARD_MISSING",
              bad, missing, (unsigned long long) s3.record_count);
        pzpd_close(d);
    }
    rename(path, shards[3]);

    //--- zero a shard's index and both superblocks: salvage -------------------------------
    uint64_t is = index_start(shards[4]);
    CHECK(is > 0, "found shard 4's index");
    damage(shards[4], is, 0, 0);
    damage(shards[4], 0, 4096, 0);
    d = pzpd_open(man, 0);
    pzpd_shard_info s4;
    pzpd_shard_info_get(a, 4, &s4);
    CHECK(d != NULL && pzpd_read_into(d, s4.first_ordinal, 0, NULL, 0) < 0, "the zeroed shard can't be read normally");
    struct rec_check rc;
    memset(&rc, 0, sizeof(rc));
    rc.orig = a;
    rc.csv = 1;
    pzpd_salvage_info info;
    CHECK(pzpd_salvage(shards[4], d, rec_cb, &rc, &info), "salvage with the manifest as schema source");
    int all = 1;
    for (uint64_t i = s4.first_ordinal; i < s4.first_ordinal + s4.record_count; i++) { if (rc.seen[i] != 1) { all = 0; } }
    CHECK(rc.errs == 0 && all && info.records == s4.record_count && info.damaged_blobs == 0 && info.names_from == 3 && info.schemas_from == 2,
          "salvage recovers every record of the shard: key, names, bytes, format, metadata, persons rows (%d mismatches, %llu of %llu records)",
          rc.errs, (unsigned long long) info.records, (unsigned long long) s4.record_count);
    memset(&rc, 0, sizeof(rc));
    rc.orig = a;
    CHECK(pzpd_salvage(shards[4], NULL, rec_cb, &rc, &info) && rc.errs == 0 && info.records == s4.record_count && info.names_from == 0 && info.stream_count == 0,
          "salvage without a schema source: raw rows, no stream names (%d mismatches)", rc.errs);
    pzpd_close(d);

    //--- a damaged payload is reported, not hidden ------------------------------------------
    struct rec_check rc2;
    memset(&rc2, 0, sizeof(rc2));
    rc2.orig = a;
    rc2.csv = 1;
    pzpd_blob_info bi;
    pzpd_blob_info_get(a, s4.first_ordinal + 1, 0, &bi);
    {
        // Find the payload in the damaged copy by its bytes and flip one of them
        size_t vs = 0;
        const unsigned char *v = (const unsigned char *) pzpd_view(a, s4.first_ordinal + 1, 0, &vs);
        FILE *f = fopen(shards[4], "r+b");
        static unsigned char whole[1 << 20];
        size_t n = fread(whole, 1, sizeof(whole), f);
        unsigned char *hit = (unsigned char *) memmem(whole, n, v, vs);
        if (hit != NULL) { fseek(f, (long)(hit - whole) + 3, SEEK_SET); fputc(hit[3] ^ 0xFF, f); }
        fclose(f);
        CHECK(hit != NULL, "found the payload to damage");
    }
    pzpd *m2 = pzpd_open(man, 0);
    CHECK(pzpd_salvage(shards[4], m2, rec_cb, &rc2, &info) && info.damaged_blobs == 1 && info.records == s4.record_count && rc2.errs == 1,
          "a damaged payload is reported with intact = 0 (damaged %llu, mismatches %d)", (unsigned long long) info.damaged_blobs, rc2.errs);
    pzpd_close(m2);

    //--- an archive stored as a blob isn't mistaken for records --------------------------------
    {
        char nest[1300];
        snprintf(nest, sizeof(nest), "%s/nest.pzpd", src);
        const char *st1[1] = { "data" };
        pzpd_writer_opts o = { st1, 1, 0, 64 };
        pzpd_writer *w = pzpd_writer_create(nest, &o);
        snprintf(path, sizeof(path), "%s/r.00000.pzpd", src);
        int ok = (w != NULL) && pzpd_writer_begin(w, "outer0", 6, PZPD_NO_GROUP, 0) && pzpd_writer_blob_file(w, 0, "inner.pzpd", 10, path) && pzpd_writer_end(w) &&
                 pzpd_writer_begin(w, "outer1", 6, PZPD_NO_GROUP, 0) && pzpd_writer_blob(w, 0, "x", 1, "PZPDRECD", 8) && pzpd_writer_end(w) && pzpd_writer_finish(w);
        snprintf(path, sizeof(path), "%s/nest.00000.pzpd", src);
        struct rec_check rc3;
        memset(&rc3, 0, sizeof(rc3));
        pzpd_salvage_info ni;
        CHECK(ok && pzpd_salvage(path, NULL, NULL, &rc3, &ni) && ni.records == 2 && ni.blobs == 2 && ni.names_from == 1 && ni.stream_count == 1,
              "a shard holding another shard as a blob salvages as its own 2 records (found %llu)", (unsigned long long) ni.records);
    }
    CHECK(!pzpd_salvage("/nonexistent/x.pzpd", NULL, NULL, NULL, NULL), "salvage of a missing file fails");

    pzpd_close(a);
    for (unsigned k = 0; k < nsh; k++) { free(shards[k]); }
    free(shards);
    free(sp);

    //--- Phase 3: edit API (the CLI tests cover crashes and resumes) ---------------------------
    char ed[1300];
    snprintf(ed, sizeof(ed), "%s/edit.pzpd", src);
    CHECK(rec_archive(ed, "img"), "write the edit archive");
    pzpd_edit_rows er[3] = { { "img/k001", 8, "7,\"a\",1,2,3,4,5,6", 0 }, { "img/k001", 8, "8,b,1,2,3,4,5,6", 0 }, { "nope", 4, "1,c,1,1,1,1,1,1", 0 } };
    for (int q = 0; q < 3; q++) { er[q].csv_len = strlen(er[q].csv); }
    uint64_t um = 0;
    CHECK(!pzpd_edit_table(ed, PZPD_EDIT_ADD, "extra", NULL, 0, NULL, 0, NULL, 0, 0, NULL), "add-table without a schema refused");
    CHECK(!pzpd_edit_table(ed, PZPD_EDIT_REPLACE, "nosuch", NULL, 0, er, 1, NULL, 0, 0, NULL) && pzpd_last_error_code() == PZPD_E_NOTFOUND, "replacing an unknown table refused");
    pzpd_edit_rows badRow = { "img/k002", 8, "1,x,70000,0,0,0,0,0", 0 };
    badRow.csv_len = strlen(badRow.csv);
    pzpd_edit_rows two[2] = { er[0], badRow };
    CHECK(!pzpd_edit_table(ed, PZPD_EDIT_REPLACE, "persons", NULL, 0, two, 2, NULL, 0, 0, NULL) && strstr(pzpd_last_error(), "img/k002") != NULL, "a bad row is refused, naming its key");
    pzpd *e = pzpd_open(ed, 0);
    CHECK(e != NULL && pzpd_table_rows(e, 2, 1, NULL) == 2, "...before any shard changed");
    pzpd_close(e);
    CHECK(pzpd_edit_table(ed, PZPD_EDIT_REPLACE, "persons", NULL, 0, er, 3, NULL, 0, PZPD_EDIT_KEEP_MISSING, &um) && um == 1, "replace persons, keeping the others (1 unmatched key)");
    e = pzpd_open(ed, 0);
    char c1[512];
    CHECK(e != NULL && pzpd_table_rows(e, 1, 1, NULL) == 2 && pzpd_table_csv(e, 1, 1, c1, sizeof(c1)) > 0 && !strncmp(c1, "7,a,1,2,3,4,5,6\n8,b,", 20) &&
          pzpd_table_rows(e, 2, 1, NULL) == 2 && pzpd_table_rows(e, 0, 1, NULL) == 0, "record 1 has the new rows, record 2 kept its 2 rows, record 0 still none");
    pzpd_close(e);
    CHECK(pzpd_edit_table(ed, PZPD_EDIT_REPLACE, "joints", "name:str parent:u16", 0, NULL, 0, "hip,0\nknee,0\nankle,1\n", strlen("hip,0\nknee,0\nankle,1\n"), 0, NULL), "replace a global table");
    e = pzpd_open(ed, 0);
    CHECK(e != NULL && pzpd_global_rows(e, 0, 0, NULL) == 3, "the global table has 3 rows in the manifest copy");
    pzpd_close(e);
    uint64_t freed0 = 0;
    CHECK(pzpd_compact(ed, &freed0) && freed0 > 0, "compact reclaims the replaced tables' sections (%llu bytes)", (unsigned long long) freed0);
    e = pzpd_open(ed, 0);
    CHECK(e != NULL && pzpd_table_rows(e, 1, 1, NULL) == 2 && pzpd_global_rows(e, 0, 0, NULL) == 3 && pzpd_verify_shard(e, 0), "tables unchanged by compact");
    pzpd_close(e);
    pzpd_edit_blob eb[2] = { { "img/k003", 8, orig, "new/a.bin", 9 }, { "img/k003", 8, orig, "new/b.bin", 9 } };
    CHECK(!pzpd_edit_stream(ed, PZPD_EDIT_ADD, "extra", eb, 2, 0, NULL) && pzpd_last_error_code() == PZPD_E_DUPLICATE, "a key given twice is refused");
    CHECK(!pzpd_edit_stream(ed, PZPD_EDIT_DROP, "rgb", eb, 1, 0, NULL), "drop-stream with files refused");
    CHECK(pzpd_edit_stream(ed, PZPD_EDIT_DROP, "depth", NULL, 0, 0, NULL), "drop depth");
    CHECK(!pzpd_edit_stream(ed, PZPD_EDIT_REPLACE, "rgb", NULL, 0, PZPD_EDIT_DROP_MISSING, NULL) && strstr(pzpd_last_error(), "without blobs") != NULL, "dropping the last blob of records is refused up front");
    CHECK(pzpd_edit_stream(ed, PZPD_EDIT_ADD, "extra", eb, 1, 0, &um) && um == 0, "add a stream with one file");
    e = pzpd_open(ed, 0);
    size_t vs = 0;
    int xs = (e != NULL) ? pzpd_stream_id(e, "extra") : -1;
    CHECK(e != NULL && pzpd_stream_count(e) == 2 && xs == 1 && pzpd_view(e, 3, 1, &vs) != NULL && vs > 0 && pzpd_view(e, 4, 1, &vs) == NULL &&
          pzpd_table_rows(e, 1, 1, NULL) == 2 && pzpd_global_rows(e, 0, 0, NULL) == 3, "the new stream, and every table row, survived the rewrites");
    pzpd_close(e);
    uint64_t freed = 1;
    CHECK(pzpd_compact(ed, &freed) && freed == 0, "after stream rewrites there is nothing to compact");
    snprintf(path, sizeof(path), "%s/edit.00000.pzpd", src);
    CHECK(!pzpd_edit_table(path, PZPD_EDIT_DROP, "vec", NULL, 0, NULL, 0, NULL, 0, 0, NULL), "edits take the manifest, not a shard");
}

//-----------------------------------------------------------------------------------------------
// Phase 4: video groups
//-----------------------------------------------------------------------------------------------

/** @brief Size of frame f of clip c (stream s). */
static size_t gsize(int c, int f, int s) { return 600 + (size_t)((c * 131 + f * 17 + s * 7) % 900); }

/** @brief Phase 4: named groups, early shard cut, oversize clips, read_range, and groups through edits / recovery. */
static void test_groups(void)
{
    char gd[1200], path[1300];
    snprintf(gd, sizeof(gd), "%s/groups", dir);
    mkdir(gd, 0755);
    snprintf(path, sizeof(path), "%s/g.pzpd", gd);
    const char *streams[2] = { "rgb", "depth" };
    pzpd_writer_opts o = { streams, 2, 64 * 1024, 64 };
    pzpd_writer *w = pzpd_writer_create(path, &o);
    CHECK(w != NULL, "writer");
    if (w == NULL) { return; }
    static unsigned char data[2000];
    // plain 0..19, clip A (30 frames, hint), clip B (100 frames, oversize hint), clip C (unnamed, no hint, 20 frames), plain 20..29
    struct { const char *name; int frames; int hint; } clips[3] = { { "clip A", 30, 1 }, { "videos/clip_B", 100, 1 }, { "", 20, 0 } };
    int ok = 1, ord = 0;
    int64_t ids[3];
    uint64_t firstOf[3] = {0};
    for (int i = 0; ok && (i < 20); i++, ord++)
    {
        char k[32]; int kl = snprintf(k, sizeof(k), "p%d", i);
        fill(data, 1500, (uint64_t) i + 99);
        ok = pzpd_writer_begin(w, k, (size_t) kl, PZPD_NO_GROUP, 0) && pzpd_writer_blob(w, 0, k, (size_t) kl, data, 1500) && pzpd_writer_end(w);
    }
    for (int c = 0; ok && (c < 3); c++)
    {
        uint64_t hint = 0;
        for (int f = 0; f < clips[c].frames; f++) { hint += gsize(c, f, 0) + gsize(c, f, 1) + 256; }
        ids[c] = pzpd_writer_group(w, clips[c].name, strlen(clips[c].name), clips[c].hint ? hint : 0);
        ok = (ids[c] >= 0);
        firstOf[c] = (uint64_t) ord;
        for (int f = 0; ok && (f < clips[c].frames); f++, ord++)
        {
            char k[64], n0[64], n1[64];
            int kl = snprintf(k, sizeof(k), "c%d/%04d", c, f), l0 = snprintf(n0, sizeof(n0), "c%d/rgb/%04d", c, f), l1 = snprintf(n1, sizeof(n1), "c%d/depth/%04d", c, f);
            ok = pzpd_writer_begin(w, k, (size_t) kl, (uint32_t) ids[c], (uint32_t)(f * 2));
            fill(data, gsize(c, f, 0), (uint64_t)(c * 1000 + f));
            ok = ok && pzpd_writer_blob(w, 0, n0, (size_t) l0, data, gsize(c, f, 0));
            fill(data, gsize(c, f, 1), (uint64_t)(c * 1000 + f + 500));
            ok = ok && (f % 7 == 3 || pzpd_writer_blob(w, 1, n1, (size_t) l1, data, gsize(c, f, 1))) && pzpd_writer_end(w);
        }
    }
    CHECK(ok, "write plain records and 3 clips");
    // misuse
    CHECK(pzpd_writer_group(w, "clip A", 6, 0) < 0 && pzpd_last_error_code() == PZPD_E_DUPLICATE, "a group name is unique");
    CHECK(pzpd_writer_begin(w, "late", 4, (uint32_t) ids[0], 99) && pzpd_writer_blob(w, 0, "late", 4, "x", 1) && !pzpd_writer_end(w) &&
          strstr(pzpd_last_error(), "consecutive") != NULL, "a closed group can't get more records");
    int64_t d = pzpd_writer_group(w, "clip D", 6, 0);
    CHECK(d >= 0 && pzpd_writer_begin(w, "d0", 2, (uint32_t) d, 5) && pzpd_writer_blob(w, 0, "d0", 2, "x", 1) && pzpd_writer_end(w) &&
          pzpd_writer_begin(w, "d1", 2, (uint32_t) d, 5) && pzpd_writer_blob(w, 0, "d1", 2, "x", 1) && !pzpd_writer_end(w) && strstr(pzpd_last_error(), "increase") != NULL,
          "frames of a group must increase");
    ord++;                                                       // d0 was written
    for (int i = 20; ok && (i < 30); i++, ord++)
    {
        char k[32]; int kl = snprintf(k, sizeof(k), "p%d", i);
        fill(data, 1500, (uint64_t) i + 99);
        ok = pzpd_writer_begin(w, k, (size_t) kl, PZPD_NO_GROUP, 0) && pzpd_writer_blob(w, 0, k, (size_t) kl, data, 1500) && pzpd_writer_end(w);
    }
    CHECK(ok && pzpd_writer_finish(w), "finish");

    pzpd *a = pzpd_open(path, 0);
    CHECK(a != NULL && pzpd_count(a) == (uint64_t) ord, "open, %d records", ord);
    if (a == NULL) { return; }
    // names, info, shard placement
    for (int c = 0; c < 3; c++)
    {
        pzpd_group g;
        int64_t f = (clips[c].name[0] != 0) ? pzpd_group_find(a, clips[c].name, strlen(clips[c].name)) : (int64_t) firstOf[c];
        CHECK(f == (int64_t) firstOf[c], "group_find(%s) = %lld", clips[c].name, (long long) f);
        int infoOk = 1, sameShard = 1;
        pzpd_blob_info b0, bi;
        pzpd_blob_info_get(a, firstOf[c], 0, &b0);
        for (int k = 0; k < clips[c].frames; k++)
        {
            if ( !pzpd_group_info(a, firstOf[c] + k, &g) || (g.first_ordinal != firstOf[c]) || (g.frames != (uint32_t) clips[c].frames) || (g.index != (uint32_t) k) ||
                 (g.id != (uint32_t) ids[c]) || (g.name_len != strlen(clips[c].name)) || memcmp(g.name, clips[c].name, g.name_len) ) { infoOk = 0; }
            pzpd_blob_info_get(a, firstOf[c] + k, 0, &bi);
            if ( (bi.shard != b0.shard) || (bi.group != (uint32_t) ids[c]) || (bi.frame != (uint32_t)(2 * k)) ) { sameShard = 0; }
        }
        CHECK(infoOk, "group_info of every frame of clip %d", c);
        CHECK(sameShard, "clip %d lies in one shard", c);
    }
    pzpd_group g;
    CHECK(!pzpd_group_info(a, 3, &g) && pzpd_last_error_code() == PZPD_OK, "a plain record is in no group");
    CHECK(pzpd_group_find(a, "nope", 4) < 0 && pzpd_last_error_code() == PZPD_E_NOTFOUND, "unknown group name");
    CHECK(pzpd_find(a, "clip A", 6, NULL) < 0, "group names are not record keys");
    pzpd_blob_info bB;
    pzpd_blob_info_get(a, firstOf[1], 0, &bB);
    pzpd_shard_info sB;
    pzpd_shard_info_get(a, bB.shard, &sB);
    CHECK(sB.first_ordinal == firstOf[1] && sB.record_count == 100 && sB.file_bytes > 64 * 1024, "the oversize clip has a shard of its own (%llu records, %llu bytes)",
          (unsigned long long) sB.record_count, (unsigned long long) sB.file_bytes);
    pzpd_blob_info bA;
    pzpd_blob_info_get(a, firstOf[0], 0, &bA);
    pzpd_shard_info sA;
    pzpd_shard_info_get(a, bA.shard, &sA);
    CHECK(sA.first_ordinal == firstOf[0], "clip A (its hint didn't fit after 20 plain records) starts a new shard early");

    // read_range = concatenated single reads
    for (int c = 0; c < 3; c++)
    {
        uint32_t n = (uint32_t) clips[c].frames;
        size_t span = pzpd_range_span(a, firstOf[c], n, 3);
        unsigned char *buf = (unsigned char *) malloc(span);
        pzpd_blob_ref *refs = (pzpd_blob_ref *) calloc(n * 2, sizeof(pzpd_blob_ref));
        ssize_t r = pzpd_read_range(a, firstOf[c], n, 3, buf, span, refs);
        int same = (r == (ssize_t) span) && (span > 0);
        static unsigned char one[2000];
        for (uint32_t k = 0; same && (k < n); k++)
        {
            for (unsigned s2 = 0; s2 < 2; s2++)
            {
                ssize_t m = pzpd_read_into(a, firstOf[c] + k, s2, one, sizeof(one));
                if ( (m == 0) != (refs[k * 2 + s2].data == NULL) ) { same = 0; }
                if ( (m > 0) && ((refs[k * 2 + s2].size != (size_t) m) || memcmp(refs[k * 2 + s2].data, one, (size_t) m)) ) { same = 0; }
            }
        }
        CHECK(same, "read_range of clip %d (%u frames, one pread of %zu bytes) = the single reads", c, n, span);
        free(buf);
        free(refs);
    }
    unsigned char tiny[16];
    CHECK(pzpd_read_range(a, firstOf[2] + 19, 2, 1, tiny, sizeof(tiny), NULL) == PZPD_E_ARG && strstr(pzpd_last_error(), "group") != NULL, "a range across a group boundary (clip C -> clip D, one shard) is refused");
    CHECK(pzpd_read_range(a, 0, (uint32_t) pzpd_count(a), 1, NULL, 0, NULL) < 0 && strstr(pzpd_last_error(), "shard") != NULL, "a range across shards is refused");
    CHECK(pzpd_read_range(a, 2, 3, 1, tiny, sizeof(tiny), NULL) == PZPD_E_ARG && strstr(pzpd_last_error(), "too small") != NULL, "a too-small buffer is refused");
    CHECK(pzpd_verify_shard(a, bB.shard), "a shard with a group table verifies");
    unsigned nsh = pzpd_shard_count(a);
    pzpd_close(a);

    // Groups survive: manifest rebuild (byte-identical), a stream edit, the section-scan fallback
    char man2[1300];
    snprintf(man2, sizeof(man2), "%s/g.rebuilt.pzpd", gd);
    const char **sp = (const char **) calloc(nsh, sizeof(char *));
    char (*sps)[1300] = calloc(nsh, 1300);
    for (unsigned k = 0; k < nsh; k++) { snprintf(sps[k], 1300, "%s/g.%05u.pzpd", gd, k); sp[k] = sps[k]; }
    CHECK(pzpd_manifest_rebuild(man2, sp, nsh), "rebuild the manifest of a grouped archive");
    FILE *f1 = fopen(path, "rb"), *f2 = fopen(man2, "rb");
    int same = (f1 != NULL) && (f2 != NULL), c1;
    while (same && ((c1 = fgetc(f1)) != EOF)) { if (c1 != fgetc(f2)) { same = 0; } }
    if (same && (fgetc(f2) != EOF)) { same = 0; }
    if (f1) { fclose(f1); }
    if (f2) { fclose(f2); }
    CHECK(same, "...byte-identical (group names are in the global hash)");
    unlink(man2);
    pzpd_edit_blob eb = { "c1/0050", 7, path, "c1/extra/0050", 13 };
    CHECK(pzpd_edit_stream(path, PZPD_EDIT_ADD, "extra", &eb, 1, 0, NULL), "add a stream to the grouped archive");
    a = pzpd_open(path, 0);
    CHECK(a != NULL && pzpd_group_find(a, "videos/clip_B", 13) == (int64_t) firstOf[1] && pzpd_group_info(a, firstOf[1] + 50, &g) && g.frames == 100 && g.index == 50 &&
          pzpd_group_info(a, firstOf[2] + 3, &g) && g.name_len == 0 && g.frames == 20, "groups, names and unnamed groups survive the shard rewrites");
    pzpd_close(a);
    damage(sps[bB.shard], 0, 4096, 0);
    struct stat st;
    stat(sps[bB.shard], &st);
    damage(sps[bB.shard], (uint64_t) st.st_size - 4096, 4096, 0);
    a = pzpd_open(path, 0);
    pzpd_shard_info si;
    CHECK(a != NULL && pzpd_shard_info_get(a, bB.shard, &si) && si.recovery == 2 && pzpd_group_find(a, "videos/clip_B", 13) == (int64_t) firstOf[1] &&
          pzpd_group_info(a, firstOf[1] + 99, &g) && g.frames == 100 && pzpd_verify_shard(a, bB.shard), "the group table is recovered by the section scan");
    pzpd_close(a);
    free(sp);
    free(sps);
}

int main(void)
{
    const char *d = getenv("PZPDIR_TEST_DIR");
    snprintf(dir, sizeof(dir), "%s", (d != NULL) ? d : "/tmp/pzpdir_test");
    mkdir(dir, 0755);
    printf("pzpdir %s tests in %s\n", pzpdirVersion, dir);

    printf("formats\n");        test_formats();
    printf("rejections\n");     test_rejections();
    printf("round trip\n");     test_roundtrip(100000);
    printf("misc\n");           test_misc();
    printf("collections\n");    test_collections();
    printf("tables\n");         test_tables();
    printf("prefetcher\n");     test_prefetch();
    printf("recovery\n");       test_recovery();
    printf("groups\n");         test_groups();
    printf("open time\n");      test_open_time();

    printf("%d checks, %s%d failures\033[0m\n", checks, failures ? "\033[31m" : "\033[32m", failures);
    return failures ? 1 : 0;
}
