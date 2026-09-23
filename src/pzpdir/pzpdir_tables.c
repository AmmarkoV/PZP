/** @file pzpdir_tables.c
 *  @brief PZPD library: table schemas and CSV.
 *  Shared types and internal declarations are in pzpdir_internal.h. */

#include "pzpdir_internal.h"

//-----------------------------------------------------------------------------------------------
// Table schemas and CSV
//-----------------------------------------------------------------------------------------------

/** @brief Type table: name, size and alignment of every enum pzpd_type. */
static const struct { const char *name; uint8_t type; uint8_t size; uint8_t align; } pzpd_types[] =
{
    { "u8",  PZPD_TYPE_U8,  1, 1 }, { "i8",  PZPD_TYPE_I8,  1, 1 },
    { "u16", PZPD_TYPE_U16, 2, 2 }, { "i16", PZPD_TYPE_I16, 2, 2 },
    { "u32", PZPD_TYPE_U32, 4, 4 }, { "i32", PZPD_TYPE_I32, 4, 4 },
    { "u64", PZPD_TYPE_U64, 8, 8 }, { "i64", PZPD_TYPE_I64, 8, 8 },
    { "f32", PZPD_TYPE_F32, 4, 4 }, { "f64", PZPD_TYPE_F64, 8, 8 },
    { "str", PZPD_TYPE_STR, 8, 4 },
};

/** @brief Size in bytes of one element of a type, 0 if unknown. */
static unsigned pzpd_type_size(uint8_t t)
{
    for (size_t i = 0; i < sizeof(pzpd_types) / sizeof(pzpd_types[0]); i++) { if (pzpd_types[i].type == t) { return pzpd_types[i].size; } }
    return 0;
}

/** @brief Alignment of a type, 0 if unknown. */
static unsigned pzpd_type_align(uint8_t t)
{
    for (size_t i = 0; i < sizeof(pzpd_types) / sizeof(pzpd_types[0]); i++) { if (pzpd_types[i].type == t) { return pzpd_types[i].align; } }
    return 0;
}

/** @brief Name of a type ("u16", ...), "?" if unknown. */
static const char *pzpd_type_name(uint8_t t)
{
    for (size_t i = 0; i < sizeof(pzpd_types) / sizeof(pzpd_types[0]); i++) { if (pzpd_types[i].type == t) { return pzpd_types[i].name; } }
    return "?";
}

/** @brief Point the public views of a schema at its own arrays (after it reached its final address). */
PZPD_INTERNAL void pzpd_schema_publish(struct pzpd_tschema *sc)
{
    for (unsigned c = 0; c < sc->ncols; c++)
    {
        sc->cols[c].name   = sc->colname[c];
        sc->cols[c].type   = sc->type[c];
        sc->cols[c].count  = sc->count[c];
        sc->cols[c].offset = sc->offset[c];
    }
    sc->pub.name       = sc->name;
    sc->pub.flags      = sc->flags;
    sc->pub.row_stride = sc->stride;
    sc->pub.ncols      = sc->ncols;
    sc->pub.cols       = sc->cols;
}

/** @brief Compute offsets and stride with C struct rules; check sizes.
 *  @return 1 if the layout is valid, 0 otherwise (error set). */
static int pzpd_schema_layout(struct pzpd_tschema *sc)
{
    uint64_t off = 0;
    unsigned maxAlign = 1;
    sc->has_str = 0;
    for (unsigned c = 0; c < sc->ncols; c++)
    {
        unsigned al = pzpd_type_align(sc->type[c]), sz = pzpd_type_size(sc->type[c]);
        if ( (al == 0) || (sc->count[c] == 0) ) { pzpd_set_error(PZPD_E_ARG, "table %s: column %s has a bad type or count", sc->name, sc->colname[c]); return 0; }
        off = pzpd_align_up(off, al);
        sc->offset[c] = (uint32_t) off;
        off += (uint64_t) sz * sc->count[c];
        if (al > maxAlign) { maxAlign = al; }
        if (sc->type[c] == PZPD_TYPE_STR) { sc->has_str = 1; }
    }
    off = pzpd_align_up(off, maxAlign);
    if ( (off == 0) || (off > (1u << 20)) ) { pzpd_set_error(PZPD_E_ARG, "table %s: rows of %llu bytes (limit 1 MiB)", sc->name, (unsigned long long) off); return 0; }
    sc->stride = (uint32_t) off;
    return 1;
}

/** @brief Parse "name:type[count], ..." into a schema.
 *  @return 1 on success, 0 on failure (error set). */
PZPD_INTERNAL int pzpd_schema_parse(const char *name, const char *text, unsigned flags, struct pzpd_tschema *sc)
{
    memset(sc, 0, sizeof(*sc));
    if ( (name == NULL) || (name[0] == 0) || (strlen(name) > PZPD_MAX_TABLE_NAME) ) { pzpd_set_error(PZPD_E_ARG, "table names must be 1..%d bytes", PZPD_MAX_TABLE_NAME); return 0; }
    if (text == NULL) { pzpd_set_error(PZPD_E_ARG, "table %s: no schema", name); return 0; }
    if (flags & ~(PZPD_TABLE_GLOBAL | PZPD_TABLE_BULK)) { pzpd_set_error(PZPD_E_ARG, "table %s: unknown flags", name); return 0; }
    snprintf(sc->name, sizeof(sc->name), "%s", name);
    sc->flags = flags;
    const char *p = text;
    while (1)
    {
        while ( (*p == ' ') || (*p == ',') || (*p == '\t') ) { p++; }
        if (*p == 0) { break; }
        const char *tok = p;
        while ( (*p != 0) && (*p != ' ') && (*p != ',') && (*p != '\t') ) { p++; }
        size_t tl = (size_t)(p - tok);
        const char *colon = memchr(tok, ':', tl);
        if ( (colon == NULL) || (colon == tok) ) { pzpd_set_error(PZPD_E_ARG, "table %s: \"%.*s\" is not name:type[count]", name, (int) tl, tok); return 0; }
        if (sc->ncols == PZPD_MAX_COLUMNS) { pzpd_set_error(PZPD_E_ARG, "table %s: more than %d columns", name, PZPD_MAX_COLUMNS); return 0; }
        size_t nl = (size_t)(colon - tok);
        if (nl > PZPD_MAX_TABLE_NAME) { pzpd_set_error(PZPD_E_ARG, "table %s: column names must be 1..%d bytes", name, PZPD_MAX_TABLE_NAME); return 0; }
        unsigned c = sc->ncols;
        memcpy(sc->colname[c], tok, nl);
        sc->colname[c][nl] = 0;
        for (unsigned d = 0; d < c; d++) { if (strcmp(sc->colname[d], sc->colname[c]) == 0) { pzpd_set_error(PZPD_E_ARG, "table %s: column %s given twice", name, sc->colname[c]); return 0; } }
        const char *ty = colon + 1, *br = memchr(ty, '[', (size_t)(tok + tl - ty));
        size_t tyl = (br != NULL) ? (size_t)(br - ty) : (size_t)(tok + tl - ty);
        sc->type[c] = 0;
        for (size_t i = 0; i < sizeof(pzpd_types) / sizeof(pzpd_types[0]); i++)
        {
            if ( (strlen(pzpd_types[i].name) == tyl) && (memcmp(pzpd_types[i].name, ty, tyl) == 0) ) { sc->type[c] = pzpd_types[i].type; }
        }
        if (sc->type[c] == 0) { pzpd_set_error(PZPD_E_ARG, "table %s: unknown type \"%.*s\" (u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 str)", name, (int) tyl, ty); return 0; }
        sc->count[c] = 1;
        if (br != NULL)
        {
            char *e = NULL;
            unsigned long n = strtoul(br + 1, &e, 10);
            if ( (e == NULL) || (*e != ']') || (e + 1 != tok + tl) || (n == 0) || (n > 65535) ) { pzpd_set_error(PZPD_E_ARG, "table %s: bad array length in \"%.*s\"", name, (int) tl, tok); return 0; }
            sc->count[c] = (uint16_t) n;
        }
        sc->ncols++;
    }
    if (sc->ncols == 0) { pzpd_set_error(PZPD_E_ARG, "table %s: schema has no columns", name); return 0; }
    return pzpd_schema_layout(sc);
}

/** @brief Same schema? (name, flags, columns, stride) */
PZPD_INTERNAL int pzpd_schema_equal(const struct pzpd_tschema *a, const struct pzpd_tschema *b)
{
    if ( strcmp(a->name, b->name) || (a->flags != b->flags) || (a->ncols != b->ncols) || (a->stride != b->stride) ) { return 0; }
    for (unsigned c = 0; c < a->ncols; c++)
    {
        if ( strcmp(a->colname[c], b->colname[c]) || (a->type[c] != b->type[c]) || (a->count[c] != b->count[c]) || (a->offset[c] != b->offset[c]) ) { return 0; }
    }
    return 1;
}

/** @brief Parse one CSV field at *p (stops at ',', '\n' or end). Quoted fields follow RFC 4180.
 *  @param out  Receives the unquoted bytes.
 *  @return 1 on success, 0 on a malformed quote. *p is left on the separator. */
static int pzpd_csv_field(const char **p, const char *end, struct pzpd_buf *out)
{
    out->len = 0;
    const char *q = *p;
    if ( (q < end) && (*q == '"') )
    {
        q++;
        while (1)
        {
            if (q >= end) { return 0; }                               // unterminated quote
            if (*q == '"')
            {
                if ( (q + 1 < end) && (q[1] == '"') ) { if (!pzpd_buf_append(out, "\"", 1)) { return 0; } q += 2; continue; }
                q++;
                break;
            }
            if (!pzpd_buf_append(out, q, 1)) { return 0; }
            q++;
        }
        if ( (q < end) && (*q != ',') && (*q != '\n') ) { return 0; }     // text after the closing quote
    }
    else
    {
        const char *s0 = q;
        while ( (q < end) && (*q != ',') && (*q != '\n') ) { if (*q == '"') { return 0; } q++; }
        if (!pzpd_buf_append(out, s0, (size_t)(q - s0))) { return 0; }
    }
    *p = q;
    return 1;
}

/** @brief Store one parsed CSV value into a row.
 *  @return 1 on success, 0 on a bad or out-of-range value (error set). */
static int pzpd_csv_store(const struct pzpd_tschema *sc, unsigned c, unsigned k, const struct pzpd_buf *f, unsigned char *row, struct pzpd_buf *heap, uint64_t rowNo)
{
    unsigned char *dst = row + sc->offset[c] + (size_t) k * pzpd_type_size(sc->type[c]);
    uint8_t t = sc->type[c];
    if (t == PZPD_TYPE_STR)
    {
        if ( (f->len > 0xFFFFFFFFull) || (heap->len + f->len > 0xFFFFFFFFull) ) { pzpd_set_error(PZPD_E_ARG, "row %llu: string too long", (unsigned long long) rowNo); return 0; }
        pzpd_str sv = { (uint32_t) heap->len, (uint32_t) f->len };
        if ( (f->len > 0) && !pzpd_buf_append(heap, f->data, f->len) ) { return 0; }
        memcpy(dst, &sv, sizeof(sv));
        return 1;
    }
    char tmp[128];
    if ( (f->len == 0) || (f->len >= sizeof(tmp)) ) { pzpd_set_error(PZPD_E_ARG, "row %llu, column %s: %s value", (unsigned long long) rowNo, sc->colname[c], f->len ? "too long a" : "empty"); return 0; }
    // Fast path for the common integer field: digits only (a leading '-' for signed types), short enough that it
    // can't overflow 64 bits. Anything else (signs, spaces, long values) takes strtoull / strtoll below.
    if ( (t != PZPD_TYPE_F32) && (t != PZPD_TYPE_F64) && (f->len <= 18) )
    {
        const unsigned char *q = f->data;
        size_t n = f->len;
        int neg = (q[0] == '-') && (t != PZPD_TYPE_U8) && (t != PZPD_TYPE_U16) && (t != PZPD_TYPE_U32) && (t != PZPD_TYPE_U64);
        size_t i = neg ? 1 : 0;
        uint64_t v = 0;
        while ( (i < n) && (q[i] >= '0') && (q[i] <= '9') ) { v = v * 10 + (uint64_t)(q[i] - '0'); i++; }
        if ( (i == n) && (n > (size_t) neg) )
        {
            int64_t sv = neg ? -(int64_t) v : (int64_t) v;
            switch (t)
            {
                case PZPD_TYPE_U8:  if (v <= 0xFFull)       { uint8_t  x = (uint8_t)  v; memcpy(dst, &x, 1); return 1; } break;
                case PZPD_TYPE_U16: if (v <= 0xFFFFull)     { uint16_t x = (uint16_t) v; memcpy(dst, &x, 2); return 1; } break;
                case PZPD_TYPE_U32: if (v <= 0xFFFFFFFFull) { uint32_t x = (uint32_t) v; memcpy(dst, &x, 4); return 1; } break;
                case PZPD_TYPE_U64:                         { memcpy(dst, &v, 8); return 1; }
                case PZPD_TYPE_I8:  if ( (sv >= -128) && (sv <= 127) )                 { int8_t  x = (int8_t)  sv; memcpy(dst, &x, 1); return 1; } break;
                case PZPD_TYPE_I16: if ( (sv >= -32768) && (sv <= 32767) )             { int16_t x = (int16_t) sv; memcpy(dst, &x, 2); return 1; } break;
                case PZPD_TYPE_I32: if ( (sv >= -2147483648LL) && (sv <= 2147483647LL) ) { int32_t x = (int32_t) sv; memcpy(dst, &x, 4); return 1; } break;
                default:                                    { memcpy(dst, &sv, 8); return 1; }
            }
            // out of range for the column: the slow path below reports it
        }
    }
    memcpy(tmp, f->data, f->len);
    tmp[f->len] = 0;
    char *e = NULL;
    errno = 0;
    if ( (t == PZPD_TYPE_F32) || (t == PZPD_TYPE_F64) )
    {
        if (t == PZPD_TYPE_F32) { float v = strtof(tmp, &e); if ( (e == NULL) || (*e != 0) ) { goto bad; } memcpy(dst, &v, 4); }
        else                    { double v = strtod(tmp, &e); if ( (e == NULL) || (*e != 0) ) { goto bad; } memcpy(dst, &v, 8); }
        return 1;
    }
    if ( (t == PZPD_TYPE_U8) || (t == PZPD_TYPE_U16) || (t == PZPD_TYPE_U32) || (t == PZPD_TYPE_U64) )
    {
        if (tmp[strspn(tmp, " \t\n\v\f\r")] == '-') { goto range; }   // strtoull skips this whitespace, then negates without ERANGE
        unsigned long long v = strtoull(tmp, &e, 10);
        if ( (e == NULL) || (*e != 0) ) { goto bad; }
        if (errno == ERANGE) { goto range; }
        switch (t)
        {
            case PZPD_TYPE_U8:  if (v > 0xFFull) { goto range; }       { uint8_t  x = (uint8_t)  v; memcpy(dst, &x, 1); } break;
            case PZPD_TYPE_U16: if (v > 0xFFFFull) { goto range; }     { uint16_t x = (uint16_t) v; memcpy(dst, &x, 2); } break;
            case PZPD_TYPE_U32: if (v > 0xFFFFFFFFull) { goto range; } { uint32_t x = (uint32_t) v; memcpy(dst, &x, 4); } break;
            default:                                                   { uint64_t x = (uint64_t) v; memcpy(dst, &x, 8); } break;
        }
        return 1;
    }
    {
        long long v = strtoll(tmp, &e, 10);
        if ( (e == NULL) || (*e != 0) ) { goto bad; }
        if (errno == ERANGE) { goto range; }
        switch (t)
        {
            case PZPD_TYPE_I8:  if ( (v < -128) || (v > 127) ) { goto range; }                { int8_t  x = (int8_t)  v; memcpy(dst, &x, 1); } break;
            case PZPD_TYPE_I16: if ( (v < -32768) || (v > 32767) ) { goto range; }            { int16_t x = (int16_t) v; memcpy(dst, &x, 2); } break;
            case PZPD_TYPE_I32: if ( (v < -2147483648LL) || (v > 2147483647LL) ) { goto range; } { int32_t x = (int32_t) v; memcpy(dst, &x, 4); } break;
            default:                                                                          { int64_t x = (int64_t) v; memcpy(dst, &x, 8); } break;
        }
        return 1;
    }
bad:
    pzpd_set_error(PZPD_E_ARG, "row %llu, column %s: \"%s\" is not a valid %s", (unsigned long long) rowNo, sc->colname[c], tmp, pzpd_type_name(t));
    return 0;
range:
    pzpd_set_error(PZPD_E_ARG, "row %llu, column %s: %s is out of range for %s", (unsigned long long) rowNo, sc->colname[c], tmp, pzpd_type_name(t));
    return 0;
}

/** @brief Parse CSV text (one row per line) into rows appended to `rows`, strings to `heap`.
 *  @return Rows parsed, or -1 on error (error set, naming the row and column). */
PZPD_INTERNAL int64_t pzpd_csv_parse(const struct pzpd_tschema *sc, const char *csv, size_t len, struct pzpd_buf *rows, struct pzpd_buf *heap)
{
    const char *p = csv, *end = csv + len;
    struct pzpd_buf f = {0};
    unsigned char *row = (unsigned char *) calloc(1, sc->stride);
    int64_t n = 0;
    if (row == NULL) { pzpd_set_error(PZPD_E_NOMEM, "out of memory"); return -1; }
    while (p < end)
    {
        if (*p == '\n') { p++; continue; }                                  // blank line
        memset(row, 0, sc->stride);
        for (unsigned c = 0; c < sc->ncols; c++)
        {
            for (unsigned k = 0; k < sc->count[c]; k++)
            {
                int first = (c == 0) && (k == 0);
                if (!first)
                {
                    if ( (p >= end) || (*p != ',') ) { pzpd_set_error(PZPD_E_ARG, "row %llu: too few fields (table %s needs one per column element)", (unsigned long long) n + 1, sc->name); goto fail; }
                    p++;
                }
                if (!pzpd_csv_field(&p, end, &f)) { pzpd_set_error(PZPD_E_ARG, "row %llu, column %s: malformed quoted field", (unsigned long long) n + 1, sc->colname[c]); goto fail; }
                if (!pzpd_csv_store(sc, c, k, &f, row, heap, (uint64_t) n + 1)) { goto fail; }
            }
        }
        if ( (p < end) && (*p != '\n') ) { pzpd_set_error(PZPD_E_ARG, "row %llu: too many fields for table %s", (unsigned long long) n + 1, sc->name); goto fail; }
        if (!pzpd_buf_append(rows, row, sc->stride)) { goto fail; }
        n++;
        if (p < end) { p++; }
    }
    free(row);
    pzpd_buf_free(&f);
    return n;
fail:
    free(row);
    pzpd_buf_free(&f);
    return -1;
}

/** @brief Append a printf-formatted string to a buffer. */
static int pzpd_buf_printf(struct pzpd_buf *b, const char *fmt, ...)
{
    char tmp[128];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if ( (n < 0) || ((size_t) n >= sizeof(tmp)) ) { return 0; }
    return pzpd_buf_append(b, tmp, (size_t) n);
}

/** @brief Shortest decimal text of a float that reads back bit-exactly. */
static int pzpd_fmt_f32(struct pzpd_buf *b, float v)
{
    char tmp[64];
    for (int p = 1; p <= 9; p++)
    {
        snprintf(tmp, sizeof(tmp), "%.*g", p, (double) v);
        float back = strtof(tmp, NULL);
        if ( (memcmp(&back, &v, 4) == 0) || ((v != v) && (back != back)) ) { break; }
    }
    return pzpd_buf_append(b, tmp, strlen(tmp));
}

/** @brief Shortest decimal text of a double that reads back bit-exactly. */
static int pzpd_fmt_f64(struct pzpd_buf *b, double v)
{
    char tmp[64];
    for (int p = 1; p <= 17; p++)
    {
        snprintf(tmp, sizeof(tmp), "%.*g", p, v);
        double back = strtod(tmp, NULL);
        if ( (memcmp(&back, &v, 8) == 0) || ((v != v) && (back != back)) ) { break; }
    }
    return pzpd_buf_append(b, tmp, strlen(tmp));
}

/** @brief Render one row as a CSV line (with '\n'). `str` values are resolved in heap; a value that
 *  points outside the heap is rendered empty (damaged input never reads out of bounds).
 *  @return 1 on success, 0 on allocation failure. */
PZPD_INTERNAL int pzpd_csv_render(const struct pzpd_tschema *sc, const unsigned char *row, const char *heap, uint64_t heapLen, struct pzpd_buf *out)
{
    int first = 1;
    for (unsigned c = 0; c < sc->ncols; c++)
    {
        unsigned sz = pzpd_type_size(sc->type[c]);
        for (unsigned k = 0; k < sc->count[c]; k++)
        {
            const unsigned char *v = row + sc->offset[c] + (size_t) k * sz;
            if (!first && !pzpd_buf_append(out, ",", 1)) { return 0; }
            first = 0;
            int ok = 1;
            switch (sc->type[c])
            {
                case PZPD_TYPE_U8:  { uint8_t  x; memcpy(&x, v, 1); ok = pzpd_buf_printf(out, "%u", (unsigned) x); } break;
                case PZPD_TYPE_I8:  { int8_t   x; memcpy(&x, v, 1); ok = pzpd_buf_printf(out, "%d", (int) x); } break;
                case PZPD_TYPE_U16: { uint16_t x; memcpy(&x, v, 2); ok = pzpd_buf_printf(out, "%u", (unsigned) x); } break;
                case PZPD_TYPE_I16: { int16_t  x; memcpy(&x, v, 2); ok = pzpd_buf_printf(out, "%d", (int) x); } break;
                case PZPD_TYPE_U32: { uint32_t x; memcpy(&x, v, 4); ok = pzpd_buf_printf(out, "%u", x); } break;
                case PZPD_TYPE_I32: { int32_t  x; memcpy(&x, v, 4); ok = pzpd_buf_printf(out, "%d", x); } break;
                case PZPD_TYPE_U64: { uint64_t x; memcpy(&x, v, 8); ok = pzpd_buf_printf(out, "%llu", (unsigned long long) x); } break;
                case PZPD_TYPE_I64: { int64_t  x; memcpy(&x, v, 8); ok = pzpd_buf_printf(out, "%lld", (long long) x); } break;
                case PZPD_TYPE_F32: { float    x; memcpy(&x, v, 4); ok = pzpd_fmt_f32(out, x); } break;
                case PZPD_TYPE_F64: { double   x; memcpy(&x, v, 8); ok = pzpd_fmt_f64(out, x); } break;
                case PZPD_TYPE_STR:
                {
                    pzpd_str sv;
                    memcpy(&sv, v, sizeof(sv));
                    const char *s = "";
                    size_t l = 0;
                    if ( (heap != NULL) && pzpd_in_file(sv.offset, sv.len, heapLen) ) { s = heap + sv.offset; l = sv.len; }
                    int quote = (l == 0);
                    for (size_t i = 0; i < l; i++) { if ( (s[i] == ',') || (s[i] == '"') || (s[i] == '\n') || (s[i] == '\r') ) { quote = 1; } }
                    if ( (l > 0) && ((s[0] == ' ') || (s[l - 1] == ' ')) ) { quote = 1; }
                    if (quote) { ok = pzpd_buf_append(out, "\"", 1); }
                    for (size_t i = 0; ok && (i < l); i++) { ok = (s[i] == '"') ? pzpd_buf_append(out, "\"\"", 2) : pzpd_buf_append(out, s + i, 1); }
                    if (ok && quote) { ok = pzpd_buf_append(out, "\"", 1); }
                }
                break;
                default: ok = 0;
            }
            if (!ok) { return 0; }
        }
    }
    return pzpd_buf_append(out, "\n", 1);
}

/** @brief Serialise a table section: header, columns, [row index], rows, string heap.
 *  @param index records + 1 row starts, or NULL for global / schema-only tables. */
PZPD_INTERNAL int pzpd_table_section(struct pzpd_buf *out, const struct pzpd_tschema *sc, uint64_t records, const uint32_t *index,
                              const void *rows, uint64_t nrows, const void *heap, uint64_t heapLen)
{
    out->len = 0;
    struct pzpd_disk_table_head h;
    memset(&h, 0, sizeof(h));
    snprintf(h.name, sizeof(h.name), "%s", sc->name);
    h.flags      = sc->flags;
    h.ncols      = sc->ncols;
    h.row_stride = sc->stride;
    h.records    = (index != NULL) ? records : 0;
    h.rows       = nrows;
    uint64_t off = sizeof(h) + (uint64_t) sc->ncols * sizeof(struct pzpd_disk_column);
    off = pzpd_align_up(off, 8);
    h.index_offset = (index != NULL) ? off : 0;
    if (index != NULL) { off = pzpd_align_up(off + (records + 1) * 4, 8); }
    h.rows_offset = off;
    off = pzpd_align_up(off + nrows * sc->stride, 8);
    h.heap_offset = off;
    h.heap_bytes  = heapLen;
    int ok = pzpd_buf_append(out, &h, sizeof(h));
    for (unsigned c = 0; ok && (c < sc->ncols); c++)
    {
        struct pzpd_disk_column dc;
        memset(&dc, 0, sizeof(dc));
        snprintf(dc.name, sizeof(dc.name), "%s", sc->colname[c]);
        dc.type   = sc->type[c];
        dc.count  = sc->count[c];
        dc.offset = sc->offset[c];
        ok = pzpd_buf_append(out, &dc, sizeof(dc));
    }
    static const unsigned char zeros[8] = {0};
    if (ok && (index != NULL)) { ok = pzpd_buf_append(out, zeros, h.index_offset - out->len) && pzpd_buf_append(out, index, (records + 1) * 4); }
    ok = ok && pzpd_buf_append(out, zeros, h.rows_offset - out->len) && pzpd_buf_append(out, rows, nrows * sc->stride);
    ok = ok && pzpd_buf_append(out, zeros, h.heap_offset - out->len) && pzpd_buf_append(out, heap, heapLen);
    return ok;
}

/** @brief Validate a table section and extract its schema (sc may be NULL) and view.
 *  @param expectRecords For record tables in shards: the shard's record count (the index must cover it).
 *  @return 1 if valid, 0 otherwise (error set). */
PZPD_INTERNAL int pzpd_table_parse(const unsigned char *d, uint64_t bytes, struct pzpd_tschema *sc, struct pzpd_tview *v, int64_t expectRecords)
{
    struct pzpd_disk_table_head h;
    if (bytes < sizeof(h)) { pzpd_set_error(PZPD_E_FORMAT, "table section too small"); return 0; }
    memcpy(&h, d, sizeof(h));
    if ( (memchr(h.name, 0, sizeof(h.name)) == NULL) || (h.name[0] == 0) || (h.ncols == 0) || (h.ncols > PZPD_MAX_COLUMNS) || (h.row_stride == 0) ||
         (h.row_stride > (1u << 20)) || (h.flags & ~(PZPD_TABLE_GLOBAL | PZPD_TABLE_BULK)) ||
         ((uint64_t) sizeof(h) + (uint64_t) h.ncols * sizeof(struct pzpd_disk_column) > bytes) )
        { pzpd_set_error(PZPD_E_FORMAT, "table section header is damaged"); return 0; }
    struct pzpd_tschema tmp;
    struct pzpd_tschema *s = (sc != NULL) ? sc : &tmp;
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", h.name);
    s->flags = h.flags;
    s->ncols = h.ncols;
    for (unsigned c = 0; c < h.ncols; c++)
    {
        struct pzpd_disk_column dc;
        memcpy(&dc, d + sizeof(h) + c * sizeof(dc), sizeof(dc));
        if ( (memchr(dc.name, 0, sizeof(dc.name)) == NULL) || (dc.name[0] == 0) || (pzpd_type_size(dc.type) == 0) || (dc.count == 0) )
            { pzpd_set_error(PZPD_E_FORMAT, "table %s: column %u is damaged", s->name, c); return 0; }
        snprintf(s->colname[c], sizeof(s->colname[c]), "%s", dc.name);
        s->type[c]  = dc.type;
        s->count[c] = dc.count;
    }
    // The stored layout must equal the one the schema implies
    if (!pzpd_schema_layout(s) || (s->stride != h.row_stride)) { pzpd_set_error(PZPD_E_FORMAT, "table %s: row layout is damaged", s->name); return 0; }
    for (unsigned c = 0; c < h.ncols; c++)
    {
        struct pzpd_disk_column dc;
        memcpy(&dc, d + sizeof(h) + c * sizeof(dc), sizeof(dc));
        if (dc.offset != s->offset[c]) { pzpd_set_error(PZPD_E_FORMAT, "table %s: column offsets are damaged", s->name); return 0; }
    }
    int global = (h.flags & PZPD_TABLE_GLOBAL) != 0;
    if (v != NULL)
    {
        memset(v, 0, sizeof(*v));
        if ( (h.rows > 0xFFFFFFFFull) || !pzpd_in_file(h.rows_offset, h.rows * h.row_stride, bytes) || !pzpd_in_file(h.heap_offset, h.heap_bytes, bytes) ||
             ((h.rows_offset & 7) != 0) )
            { pzpd_set_error(PZPD_E_FORMAT, "table %s: rows or strings are damaged", s->name); return 0; }
        if (!global && (expectRecords >= 0))
        {
            if ( (h.records != (uint64_t) expectRecords) || (h.index_offset == 0) || ((h.index_offset & 3) != 0) || !pzpd_in_file(h.index_offset, (h.records + 1) * 4, bytes) )
                { pzpd_set_error(PZPD_E_FORMAT, "table %s: row index is damaged", s->name); return 0; }
            const uint32_t *ix = (const uint32_t *) (d + h.index_offset);
            // O(1) here, so opening a shard stays cheap: the ends of the CSR index. Single lookups check
            // their own two entries; pzpd_table_shard_view() checks the whole index once before handing it out.
            if ( (ix[0] != 0) || (ix[h.records] != h.rows) ) { pzpd_set_error(PZPD_E_FORMAT, "table %s: row index is damaged", s->name); return 0; }
            v->index = ix;
        }
        v->present    = 1;
        v->records    = h.records;
        v->rows       = h.rows;
        v->rowdata    = d + h.rows_offset;
        v->heap       = (const char *) (d + h.heap_offset);
        v->heap_bytes = h.heap_bytes;
    }
    return 1;
}
