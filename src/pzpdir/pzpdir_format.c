/** @file pzpdir_format.c
 *  @brief PZPD library: on-disk constants and structures, errors, small helpers.
 *  Shared types and internal declarations are in pzpdir_internal.h. */

#include "pzpdir_internal.h"

//-----------------------------------------------------------------------------------------------
// Errors (thread-local)
//-----------------------------------------------------------------------------------------------

__thread char pzpd_errorText[512]; ///< Message of the last error on this thread, "" after success
__thread int  pzpd_errorCode;      ///< enum pzpd_error of the last error on this thread

/** @brief Record an error for pzpd_last_error() on this thread.
 *  @param code enum pzpd_error value.
 *  @param fmt  printf-style message. */
PZPD_INTERNAL void pzpd_set_error(int code, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(pzpd_errorText, sizeof(pzpd_errorText), fmt, args);
    va_end(args);
    pzpd_errorCode = code;
    #if PZPDIR_DEBUG
     fprintf(stderr, PZPD_RED "pzpdir: %s" PZPD_NORMAL "\n", pzpd_errorText);
    #endif
}

/** @brief Clear the error state of this thread (called at the start of public functions). */
PZPD_INTERNAL void pzpd_clear_error(void)
{
    pzpd_errorText[0] = 0;
    pzpd_errorCode    = PZPD_OK;
}

/** @brief Save this thread's error. */
PZPD_INTERNAL void pzpd_error_save(struct pzpd_saved_error *e)
{
    e->code = pzpd_errorCode;
    memcpy(e->text, pzpd_errorText, sizeof(e->text));
}

/** @brief Make a saved error this thread's error again. */
PZPD_INTERNAL void pzpd_error_restore(const struct pzpd_saved_error *e)
{
    memcpy(pzpd_errorText, e->text, sizeof(pzpd_errorText));
    pzpd_errorCode = e->code;
}

/** @brief Put printf-formatted context in front of this thread's error ("context: message").
 *  @param code New enum pzpd_error, or PZPD_OK to keep the current one. */
PZPD_INTERNAL void pzpd_error_wrap(int code, const char *fmt, ...)
{
    struct pzpd_saved_error e;
    pzpd_error_save(&e);
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(pzpd_errorText, sizeof(pzpd_errorText), fmt, args);
    va_end(args);
    size_t o = (n < 0) ? 0 : ((size_t) n < sizeof(pzpd_errorText)) ? (size_t) n : sizeof(pzpd_errorText) - 1;
    const char *sep = ": ";
    for (const char *q = sep; (*q != 0) && (o + 1 < sizeof(pzpd_errorText)); q++) { pzpd_errorText[o++] = *q; }
    for (const char *q = e.text; (*q != 0) && (o + 1 < sizeof(pzpd_errorText)); q++) { pzpd_errorText[o++] = *q; }   // cut to fit, like snprintf
    pzpd_errorText[o] = 0;
    pzpd_errorCode = (code != PZPD_OK) ? code : e.code;
    #if PZPDIR_DEBUG
     fprintf(stderr, PZPD_RED "pzpdir: %s" PZPD_NORMAL "\n", pzpd_errorText);
    #endif
}

const char *pzpd_last_error(void)
{
    return pzpd_errorText;
}

int pzpd_last_error_code(void)
{
    return pzpd_errorCode;
}

//-----------------------------------------------------------------------------------------------
// Small helpers
//-----------------------------------------------------------------------------------------------

/** @brief Append bytes to a buffer.
 *  @return 1 on success, 0 on allocation failure. */
PZPD_INTERNAL int pzpd_buf_append(struct pzpd_buf *b, const void *src, size_t n)
{
    if (b->len + n > b->cap)
    {
        size_t ncap = (b->cap == 0) ? 4096 : b->cap;
        while (ncap < b->len + n) { ncap *= 2; }
        unsigned char *p = (unsigned char *) realloc(b->data, ncap);
        if (p == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory growing a buffer to %zu bytes", ncap); return 0; }
        b->data = p;
        b->cap  = ncap;
    }
    if (n > 0) { memcpy(b->data + b->len, src, n); }
    b->len += n;
    return 1;
}

/** @brief Free a buffer's memory and reset it. */
PZPD_INTERNAL void pzpd_buf_free(struct pzpd_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

/** @brief pwrite() the whole range, retrying on short writes and EINTR.
 *  @return 1 on success, 0 on failure (error set). */
PZPD_INTERNAL int pzpd_pwrite_all(int fd, const void *src, size_t n, uint64_t off)
{
    const unsigned char *p = (const unsigned char *) src;
    while (n > 0)
    {
        ssize_t w = pwrite(fd, p, n, (off_t) off);
        if (w < 0)
        {
            if (errno == EINTR) { continue; }
            pzpd_set_error(PZPD_E_IO, "write failed at offset %llu: %s", (unsigned long long) off, strerror(errno));
            return 0;
        }
        p   += w;
        n   -= (size_t) w;
        off += (uint64_t) w;
    }
    return 1;
}

/** @brief pread() the whole range, retrying on short reads and EINTR.
 *  @return 1 on success, 0 on failure or premature end of file (error set). */
PZPD_INTERNAL int pzpd_pread_all(int fd, void *dst, size_t n, uint64_t off)
{
    unsigned char *p = (unsigned char *) dst;
    while (n > 0)
    {
        ssize_t r = pread(fd, p, n, (off_t) off);
        if (r < 0)
        {
            if (errno == EINTR) { continue; }
            pzpd_set_error(PZPD_E_IO, "read failed at offset %llu: %s", (unsigned long long) off, strerror(errno));
            return 0;
        }
        if (r == 0)
        {
            pzpd_set_error(PZPD_E_FORMAT, "unexpected end of file at offset %llu", (unsigned long long) off);
            return 0;
        }
        p   += r;
        n   -= (size_t) r;
        off += (uint64_t) r;
    }
    return 1;
}

/** @brief fsync() the directory containing path, so renames inside it are durable. */
PZPD_INTERNAL void pzpd_fsync_dir_of(const char *path)
{
    char dir[4096];
    const char *slash = strrchr(path, '/');
    if (slash == NULL) { snprintf(dir, sizeof(dir), "."); }
    else if (slash == path) { snprintf(dir, sizeof(dir), "/"); }
    else { snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path); }
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) { fsync(fd); close(fd); }
}

/** @brief Copy a stream name into a 23-byte on-disk slot (NUL-padded). */
PZPD_INTERNAL void pzpd_put_slot_name(char slot[23], const char *name)
{
    memset(slot, 0, 23);
    size_t n = strlen(name);
    if (n > 23) { n = 23; }
    memcpy(slot, name, n);
}

/** @brief Copy a 23-byte on-disk slot into a NUL-terminated 24-byte buffer. */
PZPD_INTERNAL void pzpd_get_slot_name(char out[24], const char slot[23])
{
    memcpy(out, slot, 23);
    out[23] = 0;
}

/** @brief Validate a key or name: 1..PZPD_MAX_NAME bytes without NUL.
 *  @return 1 if valid, 0 otherwise (error set). */
PZPD_INTERNAL int pzpd_check_name(const char *what, const char *s, size_t len)
{
    if ( (s == NULL) || (len == 0) ) { pzpd_set_error(PZPD_E_ARG, "%s is empty", what); return 0; }
    if (len > PZPD_MAX_NAME) { pzpd_set_error(PZPD_E_ARG, "%s is %zu bytes, the limit is %u", what, len, PZPD_MAX_NAME); return 0; }
    if (memchr(s, 0, len) != NULL) { pzpd_set_error(PZPD_E_ARG, "%s contains a NUL byte", what); return 0; }
    return 1;
}

/** @brief Validate a stream name: 1..PZPD_MAX_STREAM_NAME bytes without `"`, `\` or control characters (the shard
 *  metadata JSON lists stream names unescaped, and recovery reads them back from it).
 *  @return 1 if valid, 0 otherwise (error set). */
PZPD_INTERNAL int pzpd_check_stream_name(const char *nm)
{
    size_t n = (nm != NULL) ? strlen(nm) : 0;
    if ( (n == 0) || (n > PZPD_MAX_STREAM_NAME) ) { pzpd_set_error(PZPD_E_ARG, "stream names must be 1..%d bytes", PZPD_MAX_STREAM_NAME); return 0; }
    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char) nm[i];
        if ( (c < 0x20) || (c == 0x7F) || (c == '"') || (c == '\\') ) { pzpd_set_error(PZPD_E_ARG, "stream name \"%s\": no quotes, backslashes or control characters", nm); return 0; }
    }
    return 1;
}

const char *pzpd_format_name(uint32_t fourcc, char out[5])
{
    for (int i = 0; i < 4; i++)
    {
        unsigned char c = (unsigned char)((fourcc >> (8 * i)) & 0xFF);
        out[i] = ( (c >= 32) && (c < 127) ) ? (char) c : '?';
    }
    out[4] = 0;
    return out;
}
