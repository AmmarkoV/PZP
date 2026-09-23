/** @file pzpdir_prefetch.inc.c
 *  @brief pzpdir.c, part 10 of 13: prefetcher.
 *  Included by pzpdir.c in this order (one translation unit: everything stays static); not compiled on its own. */

//-----------------------------------------------------------------------------------------------
// Prefetcher (spec §6). Each shard gets one of three modes:
//  - MAP and PAGECACHE hand out mmap views: the I/O threads only make the records of the schedule
//    resident (and their page tables filled) ahead of the consumers.
//  - BUFFERS reads records with O_DIRECT (buffered pread where refused) into private, page-aligned
//    buffers counted against budget_bytes, so the page cache doesn't grow. A get moves the buffer
//    into the ticket; release frees it.
// A schedule entry has an I/O state (queued → in flight → ready) and a claim state (free →
// claimed by a get → done by release / discard). The schedule is split into PZPD_PF_LANES lanes
// by ordinal, each with its own lock, entries, ordinal hash and buffer slots. `outstanding`
// (entries whose I/O was started and whose claim isn't done: the window limits it) and `used`
// (bytes of prefetched buffers plus the tickets' buffers: the budget limits it) are atomic
// counters shared by the lanes; an I/O thread reserves both before it starts an entry.
//-----------------------------------------------------------------------------------------------

#define PZPD_PF_GAP  (256u * 1024u)   ///< Gaps between requested blobs up to this size are read over (one range)
#define PZPD_PF_NONE 0xFFFFFFFFu      ///< No schedule position / no buffer slot
#define PZPD_PF_DIO  4096u            ///< O_DIRECT granularity: file offsets, lengths and buffers aligned to this
#define PZPD_PF_BATCH 8u              ///< MAP / PAGECACHE entries an I/O thread starts per lock hold; also the free window an idle I/O thread is woken for
/** @brief Lanes of a prefetcher. The schedule is split by ordinal (a hash of it) into this many parts, each with its own
 *  lock, so consumers and I/O threads working on different records rarely wait for each other; only the window and the
 *  budget span the lanes (atomic counters). I/O threads serve the lanes round-robin: with fewer threads than lanes each
 *  serves several, with more several serve each lane. */
#define PZPD_PF_LANES 8u

enum { PZPD_PF_QUEUED = 0, PZPD_PF_INFLIGHT = 1, PZPD_PF_READY = 2 };   ///< I/O state of a schedule entry
enum { PZPD_PF_FREE = 0, PZPD_PF_CLAIMED = 1, PZPD_PF_DONE = 2 };       ///< Claim state of a schedule entry

/** @brief One submitted claim (40 bytes). */
struct pzpd_pf_entry
{
    uint64_t ordinal;  ///< Record
    uint64_t seq;      ///< Position in the whole schedule (over all lanes): it may start once seq < claims done + window
    uint64_t need;     ///< Buffer bytes when the record's shard is in BUFFERS mode, else 0 (computed at submit, outside the lock)
    uint32_t mask;     ///< Streams to prefetch
    uint32_t next;     ///< Next claim of the same ordinal, PZPD_PF_NONE if none
    uint32_t slot;     ///< Buffer slot (BUFFERS shards, while in flight or ready), PZPD_PF_NONE otherwise
    uint8_t  io;       ///< PZPD_PF_QUEUED / _INFLIGHT / _READY
    uint8_t  claim;    ///< PZPD_PF_FREE / _CLAIMED / _DONE
};

/** @brief Hash slot: the claims of one ordinal, as a list through pzpd_pf_entry::next. */
struct pzpd_pf_hslot
{
    uint64_t ordinal;  ///< Record
    uint32_t head;     ///< First claim that may still be free (PZPD_PF_NONE = empty slot)
    uint32_t tail;     ///< Last claim
};

/** @brief A prefetched record's private buffer (BUFFERS shards). */
struct pzpd_pf_buf
{
    unsigned char *data;  ///< Page-aligned buffer, NULL until read (or if the read failed)
    uint64_t       bytes; ///< Bytes counted against the budget
    uint32_t       mask;  ///< Streams it holds
};

/** @brief Where one blob of a record lives in its shard file. */
struct pzpd_pf_loc
{
    uint64_t off;      ///< File offset
    uint32_t size;     ///< Bytes
    uint32_t format;   ///< FourCC
    int      mstream;  ///< Member stream id
    int      present;  ///< 1 if the record has this blob
};

/** @brief A byte range [lo, hi) of a shard file. */
struct pzpd_pf_range
{
    uint64_t lo;  ///< First byte
    uint64_t hi;  ///< One past the last byte
};

/** @brief A mutex on a cache line of its own (64-byte aligned). A struct holding one is aligned the same way, so
 *  lanes and I/O threads next to each other in an array never share a line (no false sharing between their locks). */
typedef pthread_mutex_t pzpd_cacheline_mutex __attribute__((aligned(64)));

/** @brief One lane: the part of the schedule whose ordinals hash to it (pzpd_pf_lane_of()), with its own lock.
 *  Consumers and I/O threads working on records of different lanes never wait for each other. */
struct pzpd_pf_lane
{
    pzpd_cacheline_mutex lock;     ///< Guards every field below
    pthread_cond_t   ready;        ///< Gets and clears wait here for this lane's in-flight I/O
    struct pzpd_pf_entry *e;       ///< This lane's claims, in submission order
    uint64_t         n;            ///< Entries
    uint64_t         cap;          ///< Allocated entries
    struct pzpd_pf_hslot *slots;   ///< Ordinal hash (linear probing), hcap slots
    uint64_t         hcap;         ///< Slots, a power of two (0 before the first submit)
    uint64_t         hcount;       ///< Distinct ordinals in the hash
    struct pzpd_pf_buf *bufs;      ///< Buffer slots, grown on demand up to `window`
    uint32_t        *free_bufs;    ///< Free buffer slot ids
    unsigned         nbufs;        ///< Buffer slots allocated
    unsigned         nfree;        ///< Free buffer slots
    uint64_t         cursor;       ///< Next entry the I/O threads look at
    unsigned         inflight;     ///< Entries being prefetched right now
    uint64_t         gen;          ///< Schedule generation, bumped by pzpd_prefetch_clear()
    pzpd_prefetch_stats st;        ///< This lane's counters (pzpd_prefetch_stats_get() adds the lanes up)
};

_Static_assert(_Alignof(struct pzpd_pf_lane) == 64, "a lane starts a cache line");

/** @brief Parking states of an I/O thread (pzpd_pf_worker::parked). */
enum { PZPD_PF_AWAKE = 0, PZPD_PF_PARKED_EMPTY = 1, PZPD_PF_PARKED_STARVED = 2 };

/** @brief An I/O thread and its parking place: it sleeps here when none of its lanes has an entry it may start. */
struct pzpd_pf_worker
{
    struct pzpd_prefetcher *p;     ///< Prefetcher
    unsigned         id;           ///< Thread index
    pzpd_cacheline_mutex m;        ///< Guards the sleep
    pthread_cond_t   c;            ///< Signalled by pzpd_pf_wake_worker()
    int              wake;         ///< Atomic: set by a waker, reset by the thread before its last look at its lanes
    int              parked;       ///< Atomic: PZPD_PF_AWAKE, _PARKED_EMPTY (no queued entries) or _PARKED_STARVED (window / budget full)
};

_Static_assert(_Alignof(struct pzpd_pf_worker) == 64, "an I/O thread's parking place starts a cache line");

/** @brief Prefetcher state (see the section comment above). Allocated 64-byte aligned (the lanes are). */
struct pzpd_prefetcher
{
    struct pzpd_pf_lane lane[PZPD_PF_LANES]; ///< The lanes
    pzpd            *a;            ///< Handle
    uint32_t         mask;         ///< Default stream mask
    unsigned         window;       ///< Most outstanding entries, over all lanes
    uint64_t         budget;       ///< BUFFERS byte budget, over all lanes
    uint8_t         *shard_mode;   ///< Per shard: PZPD_PF_MAP, _PAGECACHE or _BUFFERS
    int             *dfd;          ///< Per shard: O_DIRECT descriptor in use, -1 = buffered pread (read atomically)
    int             *dfd_open;     ///< Per shard: O_DIRECT descriptor to close at destroy, -1 if none
    int              any_buffers;  ///< 1 if some shard is in BUFFERS mode
    pthread_mutex_t  ctl;          ///< Serialises submit and clear (taken before a lane lock, never while holding one)
    unsigned         nworkers;     ///< I/O threads planned (their lanes depend on it)
    unsigned         nthreads;     ///< I/O threads started
    struct pzpd_pf_worker *w;      ///< I/O threads' parking places (nworkers, 64-byte aligned)
    pthread_t       *threads;      ///< I/O threads
    int              stop;         ///< Atomic: set by pzpd_prefetcher_destroy()
    uint64_t         outstanding;  ///< Atomic: started entries whose claim isn't done (the window limits it)
    uint64_t         done;         ///< Atomic: claims done (released / discarded) since the last clear: the schedule's progress
    uint64_t         seq_next;     ///< Schedule position of the next submitted claim (under `ctl`)
    uint64_t         used;         ///< Atomic: buffer bytes in slots and tickets (the budget limits it)
    uint64_t         used_peak;    ///< Atomic: highest `used`
    uint64_t         stalls;       ///< Atomic: times an I/O thread parked with queued entries (window / budget full)
    uint64_t         fallbacks;    ///< Atomic: BUFFERS shards that fell back to buffered reads
    pzpd_prefetch_stats st;        ///< Counters fixed at create (shard modes)
    struct timespec  t0;           ///< Creation time
};

/** @brief Atomic load of a shared prefetcher counter. */
static inline uint64_t pzpd_atomic_get(const uint64_t *v) { return __atomic_load_n(v, __ATOMIC_SEQ_CST); }
/** @brief Atomic add to a shared prefetcher counter. */
static inline void pzpd_atomic_add(uint64_t *v, uint64_t d) { (void) __atomic_add_fetch(v, d, __ATOMIC_SEQ_CST); }
/** @brief Atomic subtract from a shared prefetcher counter. */
static inline void pzpd_atomic_sub(uint64_t *v, uint64_t d) { (void) __atomic_sub_fetch(v, d, __ATOMIC_SEQ_CST); }
/** @brief Atomic store to a shared prefetcher counter. */
static inline void pzpd_atomic_set(uint64_t *v, uint64_t x) { __atomic_store_n(v, x, __ATOMIC_SEQ_CST); }
/** @brief Atomic load of a shared flag. */
static inline int pzpd_atomic_geti(const int *v) { return __atomic_load_n(v, __ATOMIC_SEQ_CST); }
/** @brief Atomic store of a shared flag. */
static inline void pzpd_atomic_seti(int *v, int x) { __atomic_store_n(v, x, __ATOMIC_SEQ_CST); }

/** @brief Reserve `amount` of a limit shared by the lanes: *v += amount unless that passes `limit`.
 *  @param first When 1, a zero *v always takes the reservation (one record always fits the budget).
 *  @return 1 if reserved, 0 if the limit is reached. */
static int pzpd_atomic_reserve(uint64_t *v, uint64_t amount, uint64_t limit, int first)
{
    uint64_t cur = __atomic_load_n(v, __ATOMIC_SEQ_CST);
    for (;;)
    {
        if ( !((first && (cur == 0)) || (cur + amount <= limit)) ) { return 0; }
        if (__atomic_compare_exchange_n(v, &cur, cur + amount, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) { return 1; }
    }
}

/** @brief Raise a shared maximum to at least x. */
static void pzpd_atomic_max(uint64_t *v, uint64_t x)
{
    uint64_t cur = __atomic_load_n(v, __ATOMIC_SEQ_CST);
    while ( (cur < x) && !__atomic_compare_exchange_n(v, &cur, x, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ) { }
}

/** @brief Lane of an ordinal (high bits of a multiplicative hash: the lanes' ordinal hashes use the low ones). */
static inline unsigned pzpd_pf_lane_of(uint64_t ordinal)
{
    return (unsigned)(((ordinal * 0x9E3779B97F4A7C15ull) >> 40) % PZPD_PF_LANES);
}

/** @brief Seconds elapsed since t0. */
static double pzpd_seconds_since(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - t0->tv_sec) + (double)(t.tv_nsec - t0->tv_nsec) * 1e-9;
}

/** @brief Hash slot of an ordinal: the existing one, or the empty slot where it would go. hcap must be > 0. */
static struct pzpd_pf_hslot *pzpd_pf_slot_of(struct pzpd_pf_hslot *slots, uint64_t hcap, uint64_t ordinal)
{
    uint64_t i = (ordinal * 0x9E3779B97F4A7C15ull) & (hcap - 1);
    while ( (slots[i].head != PZPD_PF_NONE) && (slots[i].ordinal != ordinal) ) { i = (i + 1) & (hcap - 1); }
    return &slots[i];
}

/** @brief Make room for `more` distinct ordinals in a lane (load ≤ ½). Lane lock held. @return 1, or 0 if out of memory. */
static int pzpd_pf_hash_reserve(struct pzpd_pf_lane *ln, uint64_t more)
{
    if (2 * (ln->hcount + more) <= ln->hcap) { return 1; }
    uint64_t nc = (ln->hcap == 0) ? 1024 : ln->hcap;
    while (nc < 2 * (ln->hcount + more)) { nc *= 2; }
    struct pzpd_pf_hslot *ns = (struct pzpd_pf_hslot *) malloc(nc * sizeof(struct pzpd_pf_hslot));
    if (ns == NULL) { return 0; }
    memset(ns, 0xFF, nc * sizeof(struct pzpd_pf_hslot));      // head = PZPD_PF_NONE everywhere
    for (uint64_t i = 0; i < ln->hcap; i++)
    {
        if (ln->slots[i].head != PZPD_PF_NONE) { *pzpd_pf_slot_of(ns, nc, ln->slots[i].ordinal) = ln->slots[i]; }
    }
    free(ln->slots);
    ln->slots = ns;
    ln->hcap  = nc;
    return 1;
}

/** @brief First free claim of an ordinal in its lane. Lane lock held. @return Its position, or UINT64_MAX if none. */
static uint64_t pzpd_pf_claim(struct pzpd_pf_lane *ln, uint64_t ordinal)
{
    if (ln->hcap == 0) { return UINT64_MAX; }
    struct pzpd_pf_hslot *sl = pzpd_pf_slot_of(ln->slots, ln->hcap, ordinal);
    if (sl->head == PZPD_PF_NONE) { return UINT64_MAX; }
    uint32_t i = sl->head;
    while ( (i != PZPD_PF_NONE) && (ln->e[i].claim != PZPD_PF_FREE) ) { i = ln->e[i].next; }
    sl->head = (i != PZPD_PF_NONE) ? i : sl->tail;              // claims never become free again: skip them next time
    return (i != PZPD_PF_NONE) ? i : UINT64_MAX;
}

/** @brief Free the buffer slot of entry i, if it holds one. Lane lock held. @return The budget bytes given back. */
static uint64_t pzpd_pf_drop_slot(struct pzpd_prefetcher *p, struct pzpd_pf_lane *ln, uint64_t i)
{
    uint32_t k = ln->e[i].slot;
    if (k == PZPD_PF_NONE) { return 0; }
    uint64_t bytes = ln->bufs[k].bytes;
    free(ln->bufs[k].data);
    pzpd_atomic_sub(&p->used, bytes);
    ln->bufs[k].data = NULL;
    ln->bufs[k].bytes = 0;
    ln->free_bufs[ln->nfree++] = k;
    ln->e[i].slot = PZPD_PF_NONE;
    return bytes;
}

/** @brief Mark a claim done, giving back its window place (and buffer, once its read finished) if its I/O was started.
 *  Lane lock held. @return What it freed for pzpd_pf_wake(): bit 0 window, bit 1 budget. */
static int pzpd_pf_done(struct pzpd_prefetcher *p, struct pzpd_pf_lane *ln, uint64_t i)
{
    ln->e[i].claim = PZPD_PF_DONE;
    int freed = 0;
    if ( (ln->e[i].io == PZPD_PF_READY) && (pzpd_pf_drop_slot(p, ln, i) > 0) ) { freed |= 2; }   // in flight: the I/O thread frees it when done
    if (ln->e[i].io != PZPD_PF_QUEUED) { pzpd_atomic_sub(&p->outstanding, 1); freed |= 1; }
    // Progress lets later entries start: count it as window room once per batch of claims
    if ( (__atomic_add_fetch(&p->done, 1, __ATOMIC_SEQ_CST) % PZPD_PF_BATCH) == 0 ) { freed |= 1; }
    return freed;
}

/** @brief Grow a lane's buffer slots (doubling, at most `window`: no more entries than that are ever started).
 *  Lane lock held. @return 1 if a slot was added, 0 otherwise. */
static int pzpd_pf_grow_bufs(struct pzpd_prefetcher *p, struct pzpd_pf_lane *ln)
{
    if (ln->nbufs >= p->window) { return 0; }
    unsigned nb = (ln->nbufs == 0) ? 16 : 2 * ln->nbufs;
    if (nb > p->window) { nb = p->window; }
    struct pzpd_pf_buf *b = (struct pzpd_pf_buf *) realloc(ln->bufs, nb * sizeof(struct pzpd_pf_buf));
    if (b == NULL) { return 0; }
    ln->bufs = b;
    uint32_t *f = (uint32_t *) realloc(ln->free_bufs, nb * sizeof(uint32_t));
    if (f == NULL) { return 0; }
    ln->free_bufs = f;
    for (unsigned k = ln->nbufs; k < nb; k++) { memset(&ln->bufs[k], 0, sizeof(ln->bufs[k])); ln->free_bufs[ln->nfree++] = k; }
    ln->nbufs = nb;
    return 1;
}

/** @brief Wake one I/O thread (it rechecks its lanes). No lane lock may be held. */
static void pzpd_pf_wake_worker(struct pzpd_pf_worker *w)
{
    pzpd_atomic_seti(&w->wake, 1);
    pthread_mutex_lock(&w->m);
    pthread_cond_signal(&w->c);
    pthread_mutex_unlock(&w->m);
}

/** @brief Wake parked I/O threads after something changed. No lane lock may be held.
 *  @param freed Bits of pzpd_pf_done(): 1 window place (threads parked with queued entries are woken once a whole
 *               batch fits, so a release costs a wake-up per batch, not per record), 2 budget bytes.
 *  @param all   1 for new entries, clear and destroy: every parked thread is woken. */
static void pzpd_pf_wake(struct pzpd_prefetcher *p, int freed, int all)
{
    if ( !all && !(freed & 2) )
    {
        uint64_t room = (p->window < PZPD_PF_BATCH) ? p->window : PZPD_PF_BATCH;
        if ( !(freed & 1) || (pzpd_atomic_get(&p->outstanding) + room > p->window) ) { return; }
    }
    for (unsigned t = 0; t < p->nworkers; t++)
    {
        int parked = pzpd_atomic_geti(&p->w[t].parked);
        if ( (parked == PZPD_PF_PARKED_STARVED) || (all && (parked != PZPD_PF_AWAKE)) ) { pzpd_pf_wake_worker(&p->w[t]); }
    }
}

/** @brief File locations of a record's requested, present blobs (indexed by merged stream).
 *  @return The record's shard (its global index in *shard, record index in *local), or NULL on error (error set). */
static struct pzpd_rshard *pzpd_pf_locate(pzpd *a, uint64_t ordinal, uint32_t mask, struct pzpd_pf_loc *loc, unsigned *shard, uint64_t *local)
{
    unsigned mi; uint64_t ml, sl;
    struct pzpd_archive *ar = pzpd_route(a, ordinal, &mi, &ml);
    if (ar == NULL) { return NULL; }
    struct pzpd_rshard *s = pzpd_locate(ar, ml, &sl);
    if (s == NULL) { return NULL; }
    *shard = a->m[mi].shard_base + (unsigned)(s - ar->shards);
    *local = sl;
    memset(loc, 0, sizeof(struct pzpd_pf_loc) * a->S);
    for (unsigned u = 0; u < a->S; u++)
    {
        int ms = a->m[mi].to_member[u];
        if ( !(mask & (1u << u)) || (ms < 0) ) { continue; }
        const struct pzpd_disk_blob *b = pzpd_blob_entry(s, sl, (unsigned) ms);
        if (b == NULL) { return NULL; }
        if (b->rel_offset == PZPD_MISSING) { continue; }
        loc[u].off     = s->rtab[sl].offset + b->rel_offset;
        loc[u].size    = b->size;
        loc[u].format  = b->format;
        loc[u].mstream = ms;
        loc[u].present = 1;
    }
    return s;
}

/** @brief Non-empty blobs of loc as file ranges sorted by offset; ranges closer than PZPD_PF_GAP are merged.
 *  @return Number of ranges. */
static unsigned pzpd_pf_ranges(const struct pzpd_pf_loc *loc, unsigned S, struct pzpd_pf_range *r, uint64_t *overRead)
{
    unsigned n = 0;
    for (unsigned u = 0; u < S; u++)
    {
        if ( !loc[u].present || (loc[u].size == 0) ) { continue; }
        unsigned k = n++;                                          // insertion sort by offset (≤ 32 entries)
        while ( (k > 0) && (r[k - 1].lo > loc[u].off) ) { r[k] = r[k - 1]; k--; }
        r[k].lo = loc[u].off;
        r[k].hi = loc[u].off + loc[u].size;
    }
    unsigned m = 0;
    *overRead = 0;
    for (unsigned k = 0; k < n; k++)
    {
        if ( (m > 0) && (r[k].lo >= r[m - 1].hi) && (r[k].lo - r[m - 1].hi <= PZPD_PF_GAP) )
        {
            *overRead += r[k].lo - r[m - 1].hi;
            if (r[k].hi > r[m - 1].hi) { r[m - 1].hi = r[k].hi; }
        }
        else { r[m++] = r[k]; }
    }
    return m;
}

/** @brief Bytes of the page-aligned buffer holding ranges r (each range rounded out to PZPD_PF_DIO). */
static uint64_t pzpd_pf_buffer_bytes(const struct pzpd_pf_range *r, unsigned n)
{
    uint64_t total = 0;
    for (unsigned k = 0; k < n; k++)
    {
        total += ((r[k].hi + PZPD_PF_DIO - 1) & ~(uint64_t)(PZPD_PF_DIO - 1)) - (r[k].lo & ~(uint64_t)(PZPD_PF_DIO - 1));
    }
    return (total > 0) ? total : PZPD_PF_DIO;
}

/** @brief Read ranges r of shard `shard` into a new page-aligned buffer, with O_DIRECT where the shard has it.
 *  @return The buffer (pzpd_pf_buffer_bytes() bytes), or NULL on error (error set). */
static unsigned char *pzpd_pf_read(struct pzpd_prefetcher *p, unsigned shard, const struct pzpd_rshard *s, const struct pzpd_pf_range *r, unsigned n)
{
    void *mem = NULL;
    if (posix_memalign(&mem, PZPD_PF_DIO, (size_t) pzpd_pf_buffer_bytes(r, n)) != 0) { pzpd_set_error(PZPD_E_NOMEM, "out of memory for a record buffer"); return NULL; }
    unsigned char *buf = (unsigned char *) mem;
    uint64_t bo = 0;
    for (unsigned k = 0; k < n; k++)
    {
        uint64_t alo = r[k].lo & ~(uint64_t)(PZPD_PF_DIO - 1);
        uint64_t ahi = (r[k].hi + PZPD_PF_DIO - 1) & ~(uint64_t)(PZPD_PF_DIO - 1);
        uint64_t got = 0, need = r[k].hi - alo;
        while (got < need)
        {
            int fd = __atomic_load_n(&p->dfd[shard], __ATOMIC_RELAXED);
            int direct = (fd >= 0);
            if (!direct) { fd = s->fd; }
            // O_DIRECT reads whole aligned blocks (the last one may end at EOF); buffered reads only what's needed
            size_t len = direct ? (size_t)(ahi - alo - got) : (size_t)(need - got);
            ssize_t rd = pread(fd, buf + bo + got, len, (off_t)(alo + got));
            if ( (rd < 0) && (errno == EINTR) ) { continue; }
            if ( (rd < 0) && direct && (errno == EINVAL) )
            {
                // The file system refuses O_DIRECT for this read: buffered reads from now on (counted once)
                if (__atomic_exchange_n(&p->dfd[shard], -1, __ATOMIC_RELAXED) >= 0) { pzpd_atomic_add(&p->fallbacks, 1); }
                continue;
            }
            if (rd <= 0) { free(buf); pzpd_set_error(PZPD_E_IO, "%s: read failed at %llu: %s", s->path, (unsigned long long)(alo + got), (rd < 0) ? strerror(errno) : "unexpected end of file"); return NULL; }
            got += (uint64_t) rd;
        }
        bo += ahi - alo;
    }
    return buf;
}

/** @brief Point refs at the wanted blobs inside a buffer holding ranges r.
 *  @return 1 if every wanted present blob is inside the buffer, 0 if some aren't (refs then incomplete). */
static int pzpd_pf_buffer_refs(const struct pzpd_pf_loc *loc, unsigned S, uint32_t want, const struct pzpd_pf_range *r, unsigned n,
                               const unsigned char *buf, pzpd_blob_ref *refs)
{
    static const unsigned char empty[1] = { 0 };
    int complete = 1;
    for (unsigned u = 0; u < S; u++)
    {
        if ( !(want & (1u << u)) || !loc[u].present ) { continue; }
        if (loc[u].size == 0) { refs[u].data = empty; refs[u].size = 0; refs[u].format = loc[u].format; continue; }
        uint64_t bo = 0;
        unsigned k = 0;
        for (; k < n; k++)
        {
            uint64_t alo = r[k].lo & ~(uint64_t)(PZPD_PF_DIO - 1);
            if ( (loc[u].off >= r[k].lo) && (loc[u].off + loc[u].size <= r[k].hi) )
            {
                refs[u].data   = buf + bo + (loc[u].off - alo);
                refs[u].size   = loc[u].size;
                refs[u].format = loc[u].format;
                break;
            }
            bo += ((r[k].hi + PZPD_PF_DIO - 1) & ~(uint64_t)(PZPD_PF_DIO - 1)) - alo;
        }
        if (k == n) { complete = 0; }
    }
    return complete;
}

/** @brief Buffer bytes a record needs when its shard is in BUFFERS mode (pzpd_pf_entry::need). Called without
 *  the prefetcher lock: locating may open and validate a shard, and the index is read-only.
 *  @return The bytes, or 0 for MAP / PAGECACHE shards and records that can't be located (their get reports why). */
static uint64_t pzpd_pf_need(struct pzpd_prefetcher *p, uint64_t ordinal, uint32_t mask)
{
    struct pzpd_pf_loc loc[PZPD_MAX_STREAMS];
    struct pzpd_pf_range r[PZPD_MAX_STREAMS];
    unsigned shard = 0;
    uint64_t local, over;
    if ( (pzpd_pf_locate(p->a, ordinal, mask, loc, &shard, &local) == NULL) || (p->shard_mode[shard] != PZPD_PF_BUFFERS) ) { return 0; }
    return pzpd_pf_buffer_bytes(r, pzpd_pf_ranges(loc, p->a->S, r, &over));
}

/** @brief Take a lane's next queued entry, reserving its window place and budget bytes (shared by the lanes).
 *  Lane lock held. @param starved Set to 1 when an entry is queued but the window, budget or buffer slots are full.
 *  @return Its position, or UINT64_MAX if none may start now. */
static uint64_t pzpd_pf_next(struct pzpd_prefetcher *p, struct pzpd_pf_lane *ln, int *starved)
{
    while ( (ln->cursor < ln->n) && ((ln->e[ln->cursor].io != PZPD_PF_QUEUED) || (ln->e[ln->cursor].claim != PZPD_PF_FREE)) ) { ln->cursor++; }
    if (ln->cursor >= ln->n) { return UINT64_MAX; }
    // Global order: no lane runs more than `window` claims ahead of the schedule's progress
    if (ln->e[ln->cursor].seq >= pzpd_atomic_get(&p->done) + p->window) { *starved = 1; return UINT64_MAX; }
    uint64_t need = ln->e[ln->cursor].need;
    // slots of discarded in-flight entries come back when their read ends
    if ( (need > 0) && (ln->nfree == 0) && !pzpd_pf_grow_bufs(p, ln) ) { *starved = 1; return UINT64_MAX; }
    if (!pzpd_atomic_reserve(&p->outstanding, 1, p->window, 0)) { *starved = 1; return UINT64_MAX; }
    if (need > 0)
    {
        if (!pzpd_atomic_reserve(&p->used, need, p->budget, 1)) { pzpd_atomic_sub(&p->outstanding, 1); *starved = 1; return UINT64_MAX; }   // one record always fits
        pzpd_atomic_max(&p->used_peak, pzpd_atomic_get(&p->used));
    }
    return ln->cursor++;
}

/** @brief Start entry i (lane lock held; window and budget already reserved): its buffer slot for a BUFFERS shard, in-flight state. */
static void pzpd_pf_start(struct pzpd_pf_lane *ln, uint64_t i)
{
    uint32_t slot = PZPD_PF_NONE;
    if (ln->e[i].need > 0)
    {
        slot = ln->free_bufs[--ln->nfree];
        ln->bufs[slot].data  = NULL;
        ln->bufs[slot].bytes = ln->e[i].need;
        ln->bufs[slot].mask  = ln->e[i].mask;
    }
    ln->e[i].slot = slot;
    ln->e[i].io = PZPD_PF_INFLIGHT;
    ln->inflight++;
}

/** @brief Prefetch one batch of a lane: up to PZPD_PF_BATCH MAP / PAGECACHE entries, or one BUFFERS entry (its read
 *  takes long), taken under one lock hold, prefetched without the lock, completed under one lock hold.
 *  @param starved Set to 1 when the lane has queued entries that can't start yet. @return Entries prefetched. */
static unsigned pzpd_pf_lane_batch(struct pzpd_prefetcher *p, struct pzpd_pf_lane *ln, int *starved)
{
    struct { uint64_t i, ordinal, bytes, over; uint32_t mask, slot; int isBuf; unsigned char *data; double dt; } job[PZPD_PF_BATCH];
    unsigned nt = 0;
    pthread_mutex_lock(&ln->lock);
    uint64_t i;
    while ( (nt < PZPD_PF_BATCH) && ((i = pzpd_pf_next(p, ln, starved)) != UINT64_MAX) )
    {
        pzpd_pf_start(ln, i);
        job[nt].i = i; job[nt].ordinal = ln->e[i].ordinal; job[nt].mask = ln->e[i].mask; job[nt].slot = ln->e[i].slot;
        job[nt].isBuf = (ln->e[i].need > 0); job[nt].data = NULL; job[nt].bytes = 0; job[nt].over = 0;
        nt++;
        if (job[nt - 1].isBuf) { break; }
    }
    pthread_mutex_unlock(&ln->lock);
    if (nt == 0) { return 0; }

    for (unsigned k = 0; k < nt; k++)
    {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        struct pzpd_pf_loc loc[PZPD_MAX_STREAMS];
        struct pzpd_pf_range r[PZPD_MAX_STREAMS];
        unsigned shard = 0, nr = 0;
        uint64_t local = 0;
        struct pzpd_rshard *s = pzpd_pf_locate(p->a, job[k].ordinal, job[k].mask, loc, &shard, &local);
        if (s != NULL)                                            // on error the get reports it
        {
            nr = pzpd_pf_ranges(loc, p->a->S, r, &job[k].over);
            if (job[k].isBuf) { job[k].data = pzpd_pf_read(p, shard, s, r, nr); }  // NULL on error: the get reads again and reports it
            else
            {
                if (p->shard_mode[shard] == PZPD_PF_PAGECACHE)
                {
                    // Start every range's reads at once (queue depth), then wait for them while pre-faulting
                    uintptr_t page = (uintptr_t) sysconf(_SC_PAGESIZE);
                    for (unsigned q = 0; q < nr; q++)
                    {
                        uintptr_t lo = (uintptr_t)(s->map + r[q].lo) & ~(page - 1);
                        (void) madvise((void *) lo, (uintptr_t)(s->map + r[q].hi) - lo, MADV_WILLNEED);
                    }
                }
                for (unsigned q = 0; q < nr; q++) { pzpd_populate(s->map + r[q].lo, (size_t)(r[q].hi - r[q].lo)); }
            }
            for (unsigned q = 0; q < nr; q++) { job[k].bytes += r[q].hi - r[q].lo; }
        }
        job[k].dt = pzpd_seconds_since(&t0);
    }

    int freed = 0;
    pthread_mutex_lock(&ln->lock);
    for (unsigned k = 0; k < nt; k++)
    {
        uint64_t e = job[k].i;
        if (job[k].slot != PZPD_PF_NONE) { ln->bufs[job[k].slot].data = job[k].data; }
        ln->e[e].io = PZPD_PF_READY;
        if ( (ln->e[e].claim == PZPD_PF_DONE) && (pzpd_pf_drop_slot(p, ln, e) > 0) ) { freed |= 2; }   // discarded meanwhile
        ln->inflight--;
        ln->st.prefetched++;
        ln->st.bytes_prefetched += job[k].bytes;
        ln->st.bytes_over_read  += job[k].over;
        ln->st.io_seconds       += job[k].dt;
    }
    pthread_cond_broadcast(&ln->ready);
    pthread_mutex_unlock(&ln->lock);
    if (freed) { pzpd_pf_wake(p, freed, 0); }
    return nt;
}

/** @brief One pass of an I/O thread over its lanes (lane l is served by thread l mod T when there are T < lanes
 *  threads, else by the threads t with t mod lanes = l): a batch from each. @return Entries prefetched. */
static unsigned pzpd_pf_scan(struct pzpd_prefetcher *p, const struct pzpd_pf_worker *w, int *starved)
{
    unsigned T = p->nworkers, took = 0;
    if (T >= PZPD_PF_LANES) { return pzpd_pf_lane_batch(p, &p->lane[w->id % PZPD_PF_LANES], starved); }
    for (unsigned l = w->id; l < PZPD_PF_LANES; l += T) { took += pzpd_pf_lane_batch(p, &p->lane[l], starved); }
    return took;
}

/** @brief I/O thread: prefetch batches from its lanes until none can start, then park. Before sleeping it announces
 *  why (queued entries blocked by window / budget, or none) and looks once more, so a waker either sees the
 *  announcement or the thread sees the waker's change: no wake-up is lost. */
static void *pzpd_pf_thread(void *arg)
{
    struct pzpd_pf_worker *w = (struct pzpd_pf_worker *) arg;
    struct pzpd_prefetcher *p = w->p;
    while (!pzpd_atomic_geti(&p->stop))
    {
        int starved = 0;
        if (pzpd_pf_scan(p, w, &starved) > 0) { continue; }
        pzpd_atomic_seti(&w->wake, 0);
        int took = 0;
        for (;;)
        {
            int why = starved ? PZPD_PF_PARKED_STARVED : PZPD_PF_PARKED_EMPTY;
            pzpd_atomic_seti(&w->parked, why);
            starved = 0;
            if ( (took = (pzpd_pf_scan(p, w, &starved) > 0)) ) { break; }
            if ( (starved ? PZPD_PF_PARKED_STARVED : PZPD_PF_PARKED_EMPTY) == why ) { break; }   // announced the right reason
        }
        if (!took)
        {
            if (starved) { pzpd_atomic_add(&p->stalls, 1); }
            pthread_mutex_lock(&w->m);
            while ( !pzpd_atomic_geti(&w->wake) && !pzpd_atomic_geti(&p->stop) ) { pthread_cond_wait(&w->c, &w->m); }
            pthread_mutex_unlock(&w->m);
        }
        pzpd_atomic_seti(&w->parked, PZPD_PF_AWAKE);
    }
    return NULL;
}

/** @brief Lowest limit in `file` of a cgroup directory (base + rel) and of each of its parents up to base.
 *  @return The lower of that and `limit` ("max", i.e. no limit, and missing files leave `limit` as is). */
static uint64_t pzpd_cgroup_min(const char *base, const char *rel, const char *file, uint64_t limit)
{
    char dir[4096];
    int n = snprintf(dir, sizeof(dir), "%s%s", base, rel);
    if ( (n <= 0) || ((size_t) n >= sizeof(dir)) ) { return limit; }
    size_t bl = strlen(base);
    while ( (strlen(dir) > bl) && (dir[strlen(dir) - 1] == '/') ) { dir[strlen(dir) - 1] = 0; }
    for (;;)
    {
        char path[4200];
        snprintf(path, sizeof(path), "%s/%s", dir, file);
        FILE *f = fopen(path, "r");
        if (f != NULL)
        {
            unsigned long long v;
            if ( (fscanf(f, "%llu", &v) == 1) && (v < limit) ) { limit = v; }
            fclose(f);
        }
        char *slash = strrchr(dir, '/');
        if ( (slash == NULL) || ((size_t)(slash - dir) < bl) ) { break; }
        *slash = 0;
    }
    return limit;
}

/** @brief Memory this process may fill: physical RAM, or less when its cgroup or an ancestor sets a limit
 *  (v2 memory.max, v1 memory.limit_in_bytes), as systemd MemoryMax, Slurm and containers do. */
static uint64_t pzpd_memory_limit(void)
{
    uint64_t limit = (uint64_t) sysconf(_SC_PHYS_PAGES) * (uint64_t) sysconf(_SC_PAGESIZE);
    FILE *f = fopen("/proc/self/cgroup", "r");
    if (f == NULL) { return limit; }
    char line[4096];
    while (fgets(line, sizeof(line), f) != NULL)
    {
        // "0::/path" (v2) or "N:controller,controller:/path" (v1)
        line[strcspn(line, "\n")] = 0;
        char *c1 = strchr(line, ':'), *c2 = (c1 != NULL) ? strchr(c1 + 1, ':') : NULL;
        if (c2 == NULL) { continue; }
        *c2 = 0;
        char *controllers = c1 + 1, *save = NULL;
        const char *rel = c2 + 1;
        if (controllers[0] == 0) { limit = pzpd_cgroup_min("/sys/fs/cgroup", rel, "memory.max", limit); continue; }
        for (char *t = strtok_r(controllers, ",", &save); t != NULL; t = strtok_r(NULL, ",", &save))
        {
            if (strcmp(t, "memory") == 0) { limit = pzpd_cgroup_min("/sys/fs/cgroup/memory", rel, "memory.limit_in_bytes", limit); }
        }
    }
    fclose(f);
    return limit;
}

/** @brief Bytes of a member's shards: from its manifest's shard table (no shard is opened), or its standalone shard. */
static uint64_t pzpd_member_bytes(const struct pzpd_member *mb)
{
    const struct pzpd_archive *ar = mb->arch;
    if (ar == NULL) { return 0; }
    if (ar->standalone) { return ar->shards[0].sb.file_bytes; }   // loaded by arch_open()
    uint64_t bytes = 0;
    for (unsigned k = 0; k < ar->shard_count; k++) { bytes += ar->mshards[k].file_bytes; }
    return bytes;
}

/** @brief pzpd_prefetch_auto_mode() with the memory limit given (pzpd_prefetcher_create() reads it once). */
static int pzpd_auto_mode(pzpd *a, unsigned shard, uint64_t memLimit)
{
    pzpd_shard_info si;
    if (!pzpd_shard_info_get(a, shard, &si)) { return pzpd_errorCode; }
    if (si.storage == PZPD_STORAGE_RAM) { return PZPD_PF_MAP; }
    return (pzpd_member_bytes(&a->m[si.member]) < memLimit / 2) ? PZPD_PF_PAGECACHE : PZPD_PF_BUFFERS;
}

int pzpd_prefetch_auto_mode(pzpd *a, unsigned shard)
{
    return pzpd_auto_mode(a, shard, pzpd_memory_limit());
}

pzpd_prefetcher *pzpd_prefetcher_create(pzpd *a, const pzpd_prefetch_opts *o)
{
    pzpd_clear_error();
    pzpd_prefetch_opts d;
    memset(&d, 0, sizeof(d));
    if (o != NULL) { d = *o; }
    if (a == NULL) { pzpd_set_error(PZPD_E_ARG, "NULL handle"); return NULL; }
    if (d.mode > PZPD_PF_BUFFERS) { pzpd_set_error(PZPD_E_ARG, "unknown prefetch mode %u", d.mode); return NULL; }
    if (d.io_threads > 256) { pzpd_set_error(PZPD_E_ARG, "at most 256 I/O threads"); return NULL; }
    if (d.window > (1u << 24)) { pzpd_set_error(PZPD_E_ARG, "window of %u records is too large", d.window); return NULL; }

    struct pzpd_prefetcher *p = (struct pzpd_prefetcher *) aligned_alloc(64, sizeof(struct pzpd_prefetcher));
    if (p == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return NULL; }
    memset(p, 0, sizeof(*p));
    p->a      = a;
    p->mask   = (d.stream_mask != 0) ? d.stream_mask : 0xFFFFFFFFu;
    p->window = (d.window != 0) ? d.window : 256;
    p->budget = (d.budget_bytes != 0) ? d.budget_bytes : (512ull << 20);
    clock_gettime(CLOCK_MONOTONIC, &p->t0);
    for (unsigned l = 0; l < PZPD_PF_LANES; l++) { pthread_mutex_init(&p->lane[l].lock, NULL); pthread_cond_init(&p->lane[l].ready, NULL); }
    pthread_mutex_init(&p->ctl, NULL);
    unsigned threads = (d.io_threads != 0) ? d.io_threads : 4;
    unsigned shards = a->shard_total ? a->shard_total : 1;
    p->shard_mode = (uint8_t *) malloc(shards);
    p->dfd        = (int *) malloc(shards * sizeof(int));
    p->dfd_open   = (int *) malloc(shards * sizeof(int));
    p->threads    = (pthread_t *) calloc(threads, sizeof(pthread_t));
    p->w          = (struct pzpd_pf_worker *) aligned_alloc(64, threads * sizeof(struct pzpd_pf_worker));
    if (p->w != NULL)
    {
        memset(p->w, 0, threads * sizeof(struct pzpd_pf_worker));
        for (unsigned t = 0; t < threads; t++) { p->w[t].p = p; p->w[t].id = t; pthread_mutex_init(&p->w[t].m, NULL); pthread_cond_init(&p->w[t].c, NULL); }
        p->nworkers = threads;
    }
    if ( (p->shard_mode == NULL) || (p->dfd == NULL) || (p->dfd_open == NULL) || (p->threads == NULL) || (p->w == NULL) )
    {
        if (p->dfd_open != NULL) { for (unsigned sh = 0; sh < shards; sh++) { p->dfd_open[sh] = -1; } }
        pzpd_prefetcher_destroy(p);
        pzpd_set_error(PZPD_E_NOMEM, "out of memory");
        return NULL;
    }

    // Mode per shard; O_DIRECT descriptors for BUFFERS shards
    for (unsigned sh = 0; sh < shards; sh++) { p->dfd[sh] = -1; p->dfd_open[sh] = -1; }
    uint64_t memLimit = (d.mode == PZPD_PF_AUTO) ? pzpd_memory_limit() : 0;
    for (unsigned sh = 0; sh < a->shard_total; sh++)
    {
        int m = (int) d.mode;
        if (m == PZPD_PF_AUTO) { m = pzpd_auto_mode(a, sh, memLimit); if (m < 0) { m = PZPD_PF_PAGECACHE; } }
        p->shard_mode[sh] = (uint8_t) m;
        if (m == PZPD_PF_MAP) { p->st.shards_map++; }
        else if (m == PZPD_PF_PAGECACHE) { p->st.shards_pagecache++; }
        else
        {
            p->st.shards_buffers++;
            p->any_buffers = 1;
            pzpd_shard_info si;
            if (pzpd_shard_info_get(a, sh, &si) && si.available)
            {
                p->dfd_open[sh] = open(si.path, O_RDONLY | O_DIRECT | O_CLOEXEC);
                p->dfd[sh] = p->dfd_open[sh];
            }
            if (p->dfd[sh] < 0) { p->fallbacks++; }
        }
    }
    pzpd_clear_error();

    for (unsigned t = 0; t < threads; t++)
    {
        int err = pthread_create(&p->threads[t], NULL, pzpd_pf_thread, &p->w[t]);
        if (err != 0) { pzpd_prefetcher_destroy(p); pzpd_set_error(PZPD_E_IO, "cannot start an I/O thread: %s", strerror(err)); return NULL; }
        p->nthreads++;
    }
    return p;
}

int pzpd_prefetch_submit(pzpd_prefetcher *p, const uint64_t *ordinals, const uint32_t *masks, size_t n)
{
    pzpd_clear_error();
    if ( (p == NULL) || ((ordinals == NULL) && (n > 0)) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return 0; }
    uint64_t total = pzpd_count(p->a);
    for (size_t k = 0; k < n; k++)
    {
        if (ordinals[k] >= total) { pzpd_set_error(PZPD_E_ARG, "ordinal %llu out of range (%llu records)", (unsigned long long) ordinals[k], (unsigned long long) total); return 0; }
    }
    // BUFFERS sizes are worked out here, before any lock: locating a record may open a shard. The claims are
    // sorted by lane (stable: each lane keeps the submission order), so each lane is locked once
    uint64_t *need = NULL;
    uint32_t *order = (n > 0) ? (uint32_t *) malloc(n * sizeof(uint32_t)) : NULL;
    if ( (n > 0) && (order == NULL) ) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
    if (p->any_buffers && (n > 0))
    {
        need = (uint64_t *) malloc(n * sizeof(uint64_t));
        if (need == NULL) { free(order); pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return 0; }
        for (size_t k = 0; k < n; k++) { need[k] = pzpd_pf_need(p, ordinals[k], (masks != NULL) ? masks[k] : p->mask); }
        pzpd_clear_error();                                        // records that can't be located fail in their get
    }
    uint64_t start[PZPD_PF_LANES + 1] = {0};
    for (size_t k = 0; k < n; k++) { start[pzpd_pf_lane_of(ordinals[k]) + 1]++; }
    for (unsigned l = 0; l < PZPD_PF_LANES; l++) { start[l + 1] += start[l]; }
    uint64_t fill[PZPD_PF_LANES];
    memcpy(fill, start, sizeof(fill));
    for (size_t k = 0; k < n; k++) { order[fill[pzpd_pf_lane_of(ordinals[k])]++] = (uint32_t) k; }
    if (n >= PZPD_PF_NONE) { free(order); free(need); pzpd_set_error(PZPD_E_ARG, "schedule longer than %u entries", PZPD_PF_NONE - 1); return 0; }

    pthread_mutex_lock(&p->ctl);
    // Room in every lane first, so a failed submit adds nothing
    int ok = 1;
    for (unsigned l = 0; ok && (l < PZPD_PF_LANES); l++)
    {
        uint64_t cnt = start[l + 1] - start[l];
        if (cnt == 0) { continue; }
        struct pzpd_pf_lane *ln = &p->lane[l];
        pthread_mutex_lock(&ln->lock);
        if (ln->n + cnt >= PZPD_PF_NONE) { pzpd_set_error(PZPD_E_ARG, "schedule longer than %u entries", PZPD_PF_NONE - 1); ok = 0; }
        if (ok && (ln->n + cnt > ln->cap))
        {
            uint64_t nc = (ln->cap == 0) ? 1024 : ln->cap;
            while (nc < ln->n + cnt) { nc *= 2; }
            struct pzpd_pf_entry *ne = (struct pzpd_pf_entry *) realloc(ln->e, nc * sizeof(struct pzpd_pf_entry));
            if (ne == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
            else { ln->e = ne; ln->cap = nc; }
        }
        if (ok && !pzpd_pf_hash_reserve(ln, cnt)) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); ok = 0; }
        pthread_mutex_unlock(&ln->lock);
    }
    for (unsigned l = 0; ok && (l < PZPD_PF_LANES); l++)
    {
        if (start[l + 1] == start[l]) { continue; }
        struct pzpd_pf_lane *ln = &p->lane[l];
        pthread_mutex_lock(&ln->lock);
        for (uint64_t q = start[l]; q < start[l + 1]; q++)
        {
            size_t k = order[q];
            uint32_t i = (uint32_t) ln->n++;
            ln->e[i].ordinal = ordinals[k];
            ln->e[i].seq     = p->seq_next + k;
            ln->e[i].mask    = (masks != NULL) ? masks[k] : p->mask;
            ln->e[i].need    = (need != NULL) ? need[k] : 0;
            ln->e[i].next    = PZPD_PF_NONE;
            ln->e[i].slot    = PZPD_PF_NONE;
            ln->e[i].io      = PZPD_PF_QUEUED;
            ln->e[i].claim   = PZPD_PF_FREE;
            struct pzpd_pf_hslot *sl = pzpd_pf_slot_of(ln->slots, ln->hcap, ordinals[k]);
            if (sl->head == PZPD_PF_NONE) { sl->ordinal = ordinals[k]; sl->head = i; sl->tail = i; ln->hcount++; }
            else { ln->e[sl->tail].next = i; sl->tail = i; }
        }
        ln->st.submitted += start[l + 1] - start[l];
        pthread_mutex_unlock(&ln->lock);
    }
    if (ok) { p->seq_next += n; }
    pthread_mutex_unlock(&p->ctl);
    free(need);
    free(order);
    if (ok && (n > 0)) { pzpd_pf_wake(p, 0, 1); }
    return ok;
}

void pzpd_prefetch_clear(pzpd_prefetcher *p)
{
    if (p == NULL) { return; }
    pthread_mutex_lock(&p->ctl);
    for (unsigned l = 0; l < PZPD_PF_LANES; l++)
    {
        struct pzpd_pf_lane *ln = &p->lane[l];
        pthread_mutex_lock(&ln->lock);
        ln->cursor = ln->n;                                        // I/O threads take nothing new from this lane
        while (ln->inflight > 0) { pthread_cond_wait(&ln->ready, &ln->lock); }
        uint64_t started = 0;
        for (uint64_t i = 0; i < ln->n; i++)
        {
            if ( (ln->e[i].io != PZPD_PF_QUEUED) && (ln->e[i].claim != PZPD_PF_DONE) ) { started++; }   // their window places
            pzpd_pf_drop_slot(p, ln, i);                           // prefetched, not yet got (tickets keep theirs)
        }
        pzpd_atomic_sub(&p->outstanding, started);
        ln->n = 0;
        ln->cursor = 0;
        ln->hcount = 0;
        if (ln->slots != NULL) { memset(ln->slots, 0xFF, ln->hcap * sizeof(struct pzpd_pf_hslot)); }
        ln->gen++;
        pthread_cond_broadcast(&ln->ready);                        // gets waiting on the old schedule
        pthread_mutex_unlock(&ln->lock);
    }
    p->seq_next = 0;                                               // every lane is empty: the new schedule starts at 0
    pzpd_atomic_set(&p->done, 0);
    pthread_mutex_unlock(&p->ctl);
    pzpd_pf_wake(p, 3, 1);
}

int pzpd_prefetch_get(pzpd_prefetcher *p, uint64_t ordinal, uint32_t mask, pzpd_blob_ref *refs, pzpd_ticket *t)
{
    pzpd_clear_error();
    if (t != NULL) { t->pos = UINT64_MAX; t->gen = 0; t->buf = NULL; t->bytes = 0; }
    if ( (p == NULL) || (refs == NULL) || (t == NULL) ) { pzpd_set_error(PZPD_E_ARG, "bad arguments"); return PZPD_E_ARG; }
    pzpd *a = p->a;
    memset(refs, 0, sizeof(pzpd_blob_ref) * a->S);

    uint32_t heldMask = 0;
    unsigned l = pzpd_pf_lane_of(ordinal);
    struct pzpd_pf_lane *ln = &p->lane[l];
    pthread_mutex_lock(&ln->lock);
    uint64_t i = pzpd_pf_claim(ln, ordinal);
    if (i == UINT64_MAX) { ln->st.unscheduled++; }
    else
    {
        ln->e[i].claim = PZPD_PF_CLAIMED;
        if (ln->e[i].io == PZPD_PF_READY) { ln->st.hits++; }
        else if (ln->e[i].io == PZPD_PF_QUEUED) { ln->st.sync_misses++; }
        else
        {
            ln->st.waits++;
            uint64_t gen = ln->gen;
            while ( (ln->gen == gen) && (ln->e[i].io == PZPD_PF_INFLIGHT) ) { pthread_cond_wait(&ln->ready, &ln->lock); }
            if (ln->gen != gen) { i = UINT64_MAX; }                // cleared meanwhile: nothing to release
        }
        if (i != UINT64_MAX)
        {
            t->pos = ((uint64_t) l << 32) | i;
            t->gen = ln->gen;
            uint32_t k = ln->e[i].slot;
            if (k != PZPD_PF_NONE)                                 // the buffer moves to the ticket (still counted in `used`)
            {
                t->buf = ln->bufs[k].data;
                t->bytes = ln->bufs[k].bytes;
                heldMask = ln->bufs[k].mask;
                ln->bufs[k].data = NULL;
                ln->bufs[k].bytes = 0;
                ln->free_bufs[ln->nfree++] = k;
                ln->e[i].slot = PZPD_PF_NONE;
            }
        }
    }
    pthread_mutex_unlock(&ln->lock);

    struct pzpd_pf_loc loc[PZPD_MAX_STREAMS];
    unsigned shard = 0;
    uint64_t local = 0;
    struct pzpd_rshard *s = pzpd_pf_locate(a, ordinal, mask, loc, &shard, &local);
    int present = 0;
    if ( (s != NULL) && (p->shard_mode[shard] == PZPD_PF_BUFFERS) )
    {
        //--- BUFFERS: blobs from the prefetched buffer, or a synchronous read into a new one ----------
        struct pzpd_pf_range r[PZPD_MAX_STREAMS];
        uint64_t over;
        int complete = 0;
        if (t->buf != NULL)
        {
            struct pzpd_pf_loc held[PZPD_MAX_STREAMS];
            unsigned hs; uint64_t hl;
            if (pzpd_pf_locate(a, ordinal, heldMask, held, &hs, &hl) != NULL)
            {
                complete = pzpd_pf_buffer_refs(loc, a->S, mask, r, pzpd_pf_ranges(held, a->S, r, &over), (const unsigned char *) t->buf, refs);
            }
        }
        if (!complete)                                             // not prefetched, the read failed, or streams beyond the submitted mask
        {
            memset(refs, 0, sizeof(pzpd_blob_ref) * a->S);
            unsigned nr = pzpd_pf_ranges(loc, a->S, r, &over);
            uint64_t bytes = pzpd_pf_buffer_bytes(r, nr);
            unsigned char *buf = pzpd_pf_read(p, shard, s, r, nr);
            int freed = (t->bytes > 0) ? 2 : 0;
            free(t->buf);
            pzpd_atomic_sub(&p->used, t->bytes);
            t->buf = buf;
            t->bytes = (buf != NULL) ? bytes : 0;
            pzpd_atomic_add(&p->used, t->bytes);
            pzpd_atomic_max(&p->used_peak, pzpd_atomic_get(&p->used));
            if (freed) { pzpd_pf_wake(p, freed, 0); }
            if (buf == NULL) { s = NULL; }
            else { pzpd_pf_buffer_refs(loc, a->S, mask, r, nr, buf, refs); }
        }
    }
    else if (s != NULL)
    {
        //--- MAP / PAGECACHE: views into the shard mapping, where pzpd_pf_locate() found each blob -----
        for (unsigned u = 0; u < a->S; u++)
        {
            if (!loc[u].present) { continue; }
            refs[u].data   = s->map + loc[u].off;
            refs[u].size   = loc[u].size;
            refs[u].format = loc[u].format;
        }
    }
    // Count the blobs; with PZPD_O_VERIFY, check each against the XXH32 in its record header
    for (unsigned u = 0; (s != NULL) && (u < a->S); u++)
    {
        if (refs[u].data == NULL) { continue; }
        present++;
        uint32_t want;
        if ( (a->flags & PZPD_O_VERIFY) && (!pzpd_header_blob_xxh(s, local, (unsigned) loc[u].mstream, &want) || (XXH32(refs[u].data, refs[u].size, 0) != want)) )
        {
            if (pzpd_errorCode == PZPD_OK) { pzpd_set_error(PZPD_E_CHECKSUM, "%s: checksum mismatch in record %llu stream %u", s->path, (unsigned long long) local, (unsigned) loc[u].mstream); }
            s = NULL;
        }
    }
    if (s == NULL)
    {
        if (pzpd_errorCode == PZPD_OK) { pzpd_errorCode = PZPD_E_FORMAT; }
        struct pzpd_saved_error e;
        pzpd_error_save(&e);
        memset(refs, 0, sizeof(pzpd_blob_ref) * a->S);
        pzpd_prefetch_release(p, t);
        pzpd_error_restore(&e);
        return e.code;
    }
    return present;
}

void pzpd_prefetch_release(pzpd_prefetcher *p, pzpd_ticket *t)
{
    if ( (p == NULL) || (t == NULL) ) { return; }
    int freed = 0;
    free(t->buf);
    if (t->bytes > 0) { pzpd_atomic_sub(&p->used, t->bytes); freed |= 2; }
    unsigned l = (unsigned)(t->pos >> 32);
    uint64_t i = t->pos & 0xFFFFFFFFull;
    if ( (t->pos != UINT64_MAX) && (l < PZPD_PF_LANES) )
    {
        struct pzpd_pf_lane *ln = &p->lane[l];
        pthread_mutex_lock(&ln->lock);
        if ( (t->gen == ln->gen) && (i < ln->n) && (ln->e[i].claim == PZPD_PF_CLAIMED) )
        {
            freed |= pzpd_pf_done(p, ln, i);
            ln->st.released++;
        }
        pthread_mutex_unlock(&ln->lock);
    }
    t->pos = UINT64_MAX;
    t->buf = NULL;
    t->bytes = 0;
    if (freed) { pzpd_pf_wake(p, freed, 0); }
}

void pzpd_prefetch_discard(pzpd_prefetcher *p, uint64_t ordinal)
{
    if (p == NULL) { return; }
    struct pzpd_pf_lane *ln = &p->lane[pzpd_pf_lane_of(ordinal)];
    int freed = 0;
    pthread_mutex_lock(&ln->lock);
    uint64_t i = pzpd_pf_claim(ln, ordinal);
    if (i != UINT64_MAX)
    {
        freed = pzpd_pf_done(p, ln, i);
        ln->st.discarded++;
    }
    pthread_mutex_unlock(&ln->lock);
    if (freed) { pzpd_pf_wake(p, freed, 0); }
}

void pzpd_prefetch_stats_get(const pzpd_prefetcher *p, pzpd_prefetch_stats *s)
{
    if (s == NULL) { return; }
    memset(s, 0, sizeof(*s));
    if (p == NULL) { return; }
    struct pzpd_prefetcher *q = (struct pzpd_prefetcher *) p;     // the locks are not part of the logical state
    *s = q->st;
    for (unsigned l = 0; l < PZPD_PF_LANES; l++)
    {
        struct pzpd_pf_lane *ln = &q->lane[l];
        pthread_mutex_lock(&ln->lock);
        s->submitted        += ln->st.submitted;
        s->prefetched       += ln->st.prefetched;
        s->hits             += ln->st.hits;
        s->waits            += ln->st.waits;
        s->sync_misses      += ln->st.sync_misses;
        s->unscheduled      += ln->st.unscheduled;
        s->discarded        += ln->st.discarded;
        s->released         += ln->st.released;
        s->bytes_prefetched += ln->st.bytes_prefetched;
        s->bytes_over_read  += ln->st.bytes_over_read;
        s->io_seconds       += ln->st.io_seconds;
        pthread_mutex_unlock(&ln->lock);
    }
    s->producer_stalls   = pzpd_atomic_get(&q->stalls);
    s->direct_fallbacks  = (unsigned) pzpd_atomic_get(&q->fallbacks);
    s->buffer_bytes      = pzpd_atomic_get(&q->used);
    s->buffer_bytes_peak = pzpd_atomic_get(&q->used_peak);
    s->elapsed_seconds   = pzpd_seconds_since(&p->t0);
}

void pzpd_prefetcher_destroy(pzpd_prefetcher *p)
{
    if (p == NULL) { return; }
    pzpd_atomic_seti(&p->stop, 1);
    for (unsigned t = 0; t < p->nthreads; t++) { pzpd_pf_wake_worker(&p->w[t]); }
    for (unsigned t = 0; t < p->nthreads; t++) { pthread_join(p->threads[t], NULL); }
    for (unsigned t = 0; (p->w != NULL) && (t < p->nworkers); t++) { pthread_mutex_destroy(&p->w[t].m); pthread_cond_destroy(&p->w[t].c); }
    for (unsigned l = 0; l < PZPD_PF_LANES; l++)
    {
        struct pzpd_pf_lane *ln = &p->lane[l];
        for (unsigned k = 0; k < ln->nbufs; k++) { free(ln->bufs[k].data); }
        free(ln->bufs);
        free(ln->free_bufs);
        free(ln->slots);
        free(ln->e);
        pthread_cond_destroy(&ln->ready);
        pthread_mutex_destroy(&ln->lock);
    }
    for (unsigned sh = 0; (p->dfd_open != NULL) && (sh < (p->a->shard_total ? p->a->shard_total : 1)); sh++) { if (p->dfd_open[sh] >= 0) { close(p->dfd_open[sh]); } }
    pthread_mutex_destroy(&p->ctl);
    free(p->w);
    free(p->threads);
    free(p->shard_mode);
    free(p->dfd);
    free(p->dfd_open);
    free(p);
}
