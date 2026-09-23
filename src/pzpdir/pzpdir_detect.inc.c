/** @file pzpdir_detect.inc.c
 *  @brief pzpdir.c, part 2 of 13: format detection (header-only probes).
 *  Included by pzpdir.c in this order (one translation unit: everything stays static); not compiled on its own. */

//-----------------------------------------------------------------------------------------------
// Format detection (header-only probes)
//-----------------------------------------------------------------------------------------------

/** @brief Read a big-endian u16. */
static inline uint32_t pzpd_be16(const unsigned char *p) { return ((uint32_t)p[0] << 8) | p[1]; }
/** @brief Read a big-endian u32. */
static inline uint32_t pzpd_be32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
/** @brief Read a little-endian u32. */
static inline uint32_t pzpd_le32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/** @brief Clamp a value to u16 (saturating). */
static inline uint16_t pzpd_sat16(uint64_t v) { return (v > 65535) ? 65535 : (uint16_t) v; }

/** @brief JPEG: walk the marker segments until a start-of-frame marker. */
static int pzpd_probe_jpeg(const unsigned char *d, size_t n, pzpd_blob_meta *m)
{
    if ( (n < 4) || (d[0] != 0xFF) || (d[1] != 0xD8) || (d[2] != 0xFF) ) { return 0; }
    m->format = PZPD_FORMAT_JPEG;
    size_t i = 2;
    while (i + 4 <= n)
    {
        if (d[i] != 0xFF) { return 1; }                  // not a marker: format known, metadata not found
        unsigned int marker = d[i + 1];
        if (marker == 0xFF) { i++; continue; }           // fill byte
        if ( (marker == 0xD8) || (marker == 0x01) || ((marker >= 0xD0) && (marker <= 0xD7)) ) { i += 2; continue; } // standalone markers
        if ( (marker == 0xD9) || (marker == 0xDA) ) { return 1; } // end of image / start of scan before any frame header
        uint32_t len = pzpd_be16(d + i + 2);
        int isSOF = (marker >= 0xC0) && (marker <= 0xCF) && (marker != 0xC4) && (marker != 0xC8) && (marker != 0xCC);
        if (isSOF)
        {
            if (i + 10 > n) { return 1; }
            m->bits       = d[i + 4];
            m->height     = pzpd_be16(d + i + 5);
            m->width      = pzpd_be16(d + i + 7);
            m->channels   = d[i + 9];
            m->frames     = 1;
            m->meta_flags |= PZPD_META_VALID;
            return 1;
        }
        if (len < 2) { return 1; }
        i += 2 + len;
    }
    return 1;
}

/** @brief PNG: signature + IHDR chunk. */
static int pzpd_probe_png(const unsigned char *d, size_t n, pzpd_blob_meta *m)
{
    static const unsigned char sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    if ( (n < 8) || (memcmp(d, sig, 8) != 0) ) { return 0; }
    m->format = PZPD_FORMAT_PNG;
    if ( (n < 8 + 8 + 13) || (memcmp(d + 12, "IHDR", 4) != 0) ) { return 1; }
    m->width  = pzpd_be32(d + 16);
    m->height = pzpd_be32(d + 20);
    m->bits   = d[24];
    switch (d[25])  // colour type
    {
        case 0: m->channels = 1; break;
        case 2: m->channels = 3; break;
        case 3: m->channels = 1; m->meta_flags |= PZPD_META_INDEXED; break;
        case 4: m->channels = 2; break;
        case 6: m->channels = 4; break;
        default: return 1;
    }
    m->frames     = 1;
    m->meta_flags |= PZPD_META_VALID;
    return 1;
}

/** @brief Read the ASCII header tokens of a PNM / PFM file (skipping '#' comments).
 *  @param count Number of unsigned integers to read after the 2-byte magic (or signed float for PFM).
 *  @param vals  Output values as doubles.
 *  @return Offset after the last token, 0 on failure. */
static size_t pzpd_pnm_tokens(const unsigned char *d, size_t n, int count, double *vals)
{
    size_t i = 2;
    for (int t = 0; t < count; t++)
    {
        while (i < n)
        {
            if (d[i] == '#') { while ( (i < n) && (d[i] != '\n') ) { i++; } }
            else if ( (d[i] == ' ') || (d[i] == '\t') || (d[i] == '\r') || (d[i] == '\n') ) { i++; }
            else { break; }
        }
        if (i >= n) { return 0; }
        char tmp[64];
        size_t k = 0;
        while ( (i < n) && (k < sizeof(tmp) - 1) && (d[i] != ' ') && (d[i] != '\t') && (d[i] != '\r') && (d[i] != '\n') && (d[i] != '#') )
            { tmp[k++] = (char) d[i++]; }
        tmp[k] = 0;
        char *end = NULL;
        vals[t] = strtod(tmp, &end);
        if ( (k == 0) || (end == NULL) || (*end != 0) ) { return 0; }
    }
    return i;
}

/** @brief PBM / PGM / PPM (P1..P6). */
static int pzpd_probe_pnm(const unsigned char *d, size_t n, pzpd_blob_meta *m)
{
    if ( (n < 3) || (d[0] != 'P') || (d[1] < '1') || (d[1] > '6') ) { return 0; }
    if ( (d[2] != ' ') && (d[2] != '\t') && (d[2] != '\r') && (d[2] != '\n') && (d[2] != '#') ) { return 0; }
    int bitmap = (d[1] == '1') || (d[1] == '4');
    double v[3] = {0,0,0};
    if (pzpd_pnm_tokens(d, n, bitmap ? 2 : 3, v) == 0) { return 0; }
    if ( !((v[0] > 0) && (v[0] <= 4294967295.0)) || !((v[1] > 0) && (v[1] <= 4294967295.0)) ) { return 0; }   // also refuses nan, inf
    m->format   = PZPD_FORMAT_PNM;
    m->width    = (uint32_t) v[0];
    m->height   = (uint32_t) v[1];
    m->channels = ( (d[1] == '3') || (d[1] == '6') ) ? 3 : 1;
    m->bits     = bitmap ? 1 : ( (v[2] > 255) ? 16 : 8 );
    m->frames   = 1;
    m->meta_flags |= PZPD_META_VALID;
    return 1;
}

/** @brief Portable float map (PF = 3 channels, Pf = 1 channel). */
static int pzpd_probe_pfm(const unsigned char *d, size_t n, pzpd_blob_meta *m)
{
    if ( (n < 3) || (d[0] != 'P') || ( (d[1] != 'F') && (d[1] != 'f') ) ) { return 0; }
    if ( (d[2] != ' ') && (d[2] != '\t') && (d[2] != '\r') && (d[2] != '\n') ) { return 0; }
    double v[3] = {0,0,0};
    if (pzpd_pnm_tokens(d, n, 3, v) == 0) { return 0; }
    if ( !((v[0] > 0) && (v[0] <= 4294967295.0)) || !((v[1] > 0) && (v[1] <= 4294967295.0)) || (v[2] == 0) ) { return 0; }   // also refuses nan, inf
    m->format   = PZPD_FORMAT_PFM;
    m->width    = (uint32_t) v[0];
    m->height   = (uint32_t) v[1];
    m->channels = (d[1] == 'F') ? 3 : 1;
    m->bits     = 32;
    m->frames   = 1;
    m->meta_flags |= PZPD_META_VALID | PZPD_META_FLOAT;
    if (v[2] > 0) { m->meta_flags |= PZPD_META_BIG_ENDIAN; }
    return 1;
}

/** @brief NumPy .npy: magic, header dict with 'descr' and 'shape'. */
static int pzpd_probe_npy(const unsigned char *d, size_t n, pzpd_blob_meta *m)
{
    if ( (n < 10) || (d[0] != 0x93) || (memcmp(d + 1, "NUMPY", 5) != 0) ) { return 0; }
    m->format = PZPD_FORMAT_NPY;
    size_t hlen, hstart;
    if (d[6] == 1) { hlen = (size_t)d[8] | ((size_t)d[9] << 8); hstart = 10; }   // v1: little-endian u16 header length
    else
    {
        if (n < 12) { return 1; }
        hlen = pzpd_le32(d + 8);
        hstart = 12;
    }
    if (hstart + hlen > n) { return 1; }
    char hdr[4096];
    size_t k = (hlen < sizeof(hdr) - 1) ? hlen : sizeof(hdr) - 1;
    memcpy(hdr, d + hstart, k);
    hdr[k] = 0;

    char *descr = strstr(hdr, "'descr'");
    char *shape = strstr(hdr, "'shape'");
    if ( (descr == NULL) || (shape == NULL) ) { return 1; }
    char *q = strchr(descr + 7, '\'');                 // opening quote of the dtype string
    if ( (q == NULL) || (q[1] == 0) || (q[2] == 0) ) { return 1; }   // a header cut inside the dtype string
    char order = q[1];                                  // '<', '>', '|' or '='
    char kind  = q[2];                                  // 'f', 'i', 'u', 'b', 'c', ...
    int itemsize = atoi(q + 3);
    if ( (itemsize <= 0) || (itemsize > 16) ) { return 1; }
    m->bits = (uint8_t)(itemsize * 8);
    if ( (kind == 'f') || (kind == 'c') ) { m->meta_flags |= PZPD_META_FLOAT; }
    if ( (order == '>') && (itemsize > 1) ) { m->meta_flags |= PZPD_META_BIG_ENDIAN; }

    char *p = strchr(shape, '(');
    if (p == NULL) { return 1; }
    p++;
    uint64_t dims[3] = {0,0,0};
    int nd = 0;
    while ( (*p != 0) && (*p != ')') )
    {
        while ( (*p == ' ') || (*p == ',') ) { p++; }
        if ( (*p == ')') || (*p == 0) ) { break; }
        char *end = NULL;
        unsigned long long v = strtoull(p, &end, 10);
        if (end == p) { return 1; }
        if (nd < 3) { dims[nd] = v; }
        nd++;
        p = end;
    }
    m->width    = (dims[0] > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t) dims[0];
    m->height   = (dims[1] > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t) dims[1];
    m->channels = pzpd_sat16(dims[2]);
    m->frames   = 0;
    m->meta_flags |= PZPD_META_VALID;
    return 1;
}

/** @brief Decompress the first 40 bytes (the inner PZP frame header) of a size-prefixed
 *  zstd / lz4 PZP frame. Uses zstd / lz4 directly, so it works without pzp.h.
 *  @param frame Start of the frame (the 4-byte size prefix).
 *  @param n     Bytes available from frame.
 *  @param hdr   Receives 10 × u32.
 *  @return 1 if 40 bytes were decoded and the magic matches, 0 otherwise. */
static int pzpd_pzp_inner_header(const unsigned char *frame, size_t n, uint32_t hdr[10])
{
    if (n < 8) { return 0; }
    uint32_t prefix = pzpd_le32(frame);
    uint32_t usize  = prefix & 0x7FFFFFFFu;
    if (usize < 40) { return 0; }
    unsigned char out[40];
    if (prefix & 0x80000000u)
    {
        int cs = (n - 4 > 0x7FFFFFFF) ? 0x7FFFFFFF : (int)(n - 4);
        int got = LZ4_decompress_safe_partial((const char *)(frame + 4), (char *) out, cs, 40, 40);
        if (got < 40) { return 0; }
    }
    else
    {
        ZSTD_DCtx *ctx = ZSTD_createDCtx();
        if (ctx == NULL) { return 0; }
        ZSTD_inBuffer  in  = { frame + 4, n - 4, 0 };
        ZSTD_outBuffer ob  = { out, 40, 0 };
        while ( (ob.pos < 40) && (in.pos < in.size) )
        {
            size_t r = ZSTD_decompressStream(ctx, &ob, &in);
            if (ZSTD_isError(r)) { break; }
            if ( (r == 0) && (ob.pos < 40) ) { break; }   // frame ended early
        }
        ZSTD_freeDCtx(ctx);
        if (ob.pos < 40) { return 0; }
    }
    memcpy(hdr, out, 40);
    // PZP writes convert_header("PZP0") natively: 'P'<<24 | 'Z'<<16 | 'P'<<8 | '0'; frames with a
    // channel group table ( PZP v0.03 ) use "PZP1", with the same 40-byte header
    const uint32_t magic  = ((uint32_t)'P' << 24) | ((uint32_t)'Z' << 16) | ((uint32_t)'P' << 8) | (uint32_t)'0';
    const uint32_t magic1 = ((uint32_t)'P' << 24) | ((uint32_t)'Z' << 16) | ((uint32_t)'P' << 8) | (uint32_t)'1';
    return (hdr[0] == magic) || (hdr[0] == magic1);
}

/** @brief Fill metadata from an inner PZP frame header (bpp_ext is bits per channel). */
static void pzpd_pzp_fill(const uint32_t hdr[10], pzpd_blob_meta *m)
{
    m->bits     = (uint8_t) hdr[1];
    m->channels = pzpd_sat16(hdr[2]);
    m->width    = hdr[3];
    m->height   = hdr[4];
    if ( (m->width > 0) && (m->height > 0) && (m->channels > 0) ) { m->meta_flags |= PZPD_META_VALID; }
}

/** @brief PZP: container ("PZP0" magic) or single frame (size prefix + compressed header). */
static int pzpd_probe_pzp(const unsigned char *d, size_t n, pzpd_blob_meta *m)
{
    if (n < 8) { return 0; }
    const uint32_t magic = ((uint32_t)'P' << 24) | ((uint32_t)'Z' << 16) | ((uint32_t)'P' << 8) | (uint32_t)'0';
    uint32_t hdr[10];
    if (pzpd_le32(d) == magic)
    {
        m->format = PZPD_FORMAT_PZPC;
        if (n < 48 + 16) { return 1; }
        uint32_t frames = pzpd_le32(d + 12);
        uint32_t off0   = pzpd_le32(d + 48);   // first PZPFrameEntry: frame_offset, compressed_size
        uint32_t size0  = pzpd_le32(d + 52);
        if ( (off0 < 48) || ((uint64_t)off0 + size0 > n) ) { return 1; }
        if (pzpd_pzp_inner_header(d + off0, size0, hdr))
        {
            pzpd_pzp_fill(hdr, m);
            m->frames = pzpd_sat16(frames);
        }
        return 1;
    }
    if (pzpd_pzp_inner_header(d, n, hdr))
    {
        m->format = PZPD_FORMAT_PZP;
        pzpd_pzp_fill(hdr, m);
        m->frames = 1;
        return 1;
    }
    return 0;
}

/** @brief Check that the bytes are valid UTF-8 without NUL; count lines.
 *  @return 1 if text, 0 otherwise. */
static int pzpd_probe_text(const unsigned char *d, size_t n, uint64_t *lines)
{
    if (n == 0) { return 0; }
    uint64_t nl = 0;
    size_t i = 0;
    while (i < n)
    {
        unsigned char c = d[i];
        if (c == 0) { return 0; }
        if (c < 0x80) { if (c == '\n') { nl++; } i++; continue; }
        int extra;
        if      ( (c & 0xE0) == 0xC0 ) { extra = 1; if (c < 0xC2) { return 0; } }
        else if ( (c & 0xF0) == 0xE0 ) { extra = 2; }
        else if ( (c & 0xF8) == 0xF0 ) { extra = 3; if (c > 0xF4) { return 0; } }
        else { return 0; }
        if (i + (size_t) extra >= n) { return 0; }                     // truncated sequence
        for (int k = 1; k <= extra; k++) { if ( (d[i + k] & 0xC0) != 0x80 ) { return 0; } }
        i += 1 + (size_t) extra;
    }
    if (d[n - 1] != '\n') { nl++; }
    *lines = nl;
    return 1;
}

/** @brief FourCC implied by a file name's extension (case-insensitive), 0 if unknown. */
static uint32_t pzpd_format_from_extension(const char *name, size_t len)
{
    if (name == NULL) { return 0; }
    size_t dot = len;
    for (size_t i = len; i > 0; i--)
    {
        if (name[i - 1] == '.') { dot = i - 1; break; }
        if (name[i - 1] == '/') { break; }
    }
    if (dot == len) { return 0; }
    char ext[8];
    size_t el = len - dot - 1;
    if ( (el == 0) || (el >= sizeof(ext)) ) { return 0; }
    for (size_t i = 0; i < el; i++)
    {
        char c = name[dot + 1 + i];
        ext[i] = ( (c >= 'A') && (c <= 'Z') ) ? (char)(c - 'A' + 'a') : c;
    }
    ext[el] = 0;
    if (!strcmp(ext,"jpg") || !strcmp(ext,"jpeg")) { return PZPD_FORMAT_JPEG; }
    if (!strcmp(ext,"png"))  { return PZPD_FORMAT_PNG; }
    if (!strcmp(ext,"pzp"))  { return PZPD_FORMAT_PZP; }
    if (!strcmp(ext,"pnm") || !strcmp(ext,"ppm") || !strcmp(ext,"pgm") || !strcmp(ext,"pbm")) { return PZPD_FORMAT_PNM; }
    if (!strcmp(ext,"pfm"))  { return PZPD_FORMAT_PFM; }
    if (!strcmp(ext,"npy"))  { return PZPD_FORMAT_NPY; }
    if (!strcmp(ext,"json")) { return PZPD_FORMAT_JSON; }
    if (!strcmp(ext,"csv"))  { return PZPD_FORMAT_CSV; }
    if (!strcmp(ext,"tsv"))  { return PZPD_FORMAT_TSV; }
    if (!strcmp(ext,"txt"))  { return PZPD_FORMAT_TEXT; }
    return 0;
}

uint32_t pzpd_detect_format(const void *data, size_t size, const char *name, size_t name_len, pzpd_blob_meta *meta_out)
{
    pzpd_blob_meta m;
    memset(&m, 0, sizeof(m));
    const unsigned char *d = (const unsigned char *) data;
    uint32_t byExt = pzpd_format_from_extension(name, name_len);

    if ( (d != NULL) && (size > 0) )
    {
        if ( pzpd_probe_jpeg(d, size, &m) || pzpd_probe_png(d, size, &m) || pzpd_probe_pzp(d, size, &m) ||
             pzpd_probe_npy(d, size, &m)  || pzpd_probe_pfm(d, size, &m) || pzpd_probe_pnm(d, size, &m) )
        {
            if (meta_out != NULL) { *meta_out = m; }
            return m.format;
        }
        uint64_t lines = 0;
        if (pzpd_probe_text(d, size, &lines))
        {
            size_t i = 0;
            if ( (size >= 3) && (d[0] == 0xEF) && (d[1] == 0xBB) && (d[2] == 0xBF) ) { i = 3; } // UTF-8 BOM
            while ( (i < size) && ( (d[i] == ' ') || (d[i] == '\t') || (d[i] == '\r') || (d[i] == '\n') ) ) { i++; }
            if      ( (byExt == PZPD_FORMAT_CSV) || (byExt == PZPD_FORMAT_TSV) ) { m.format = byExt; }
            else if ( (i < size) && ( (d[i] == '{') || (d[i] == '[') ) )    { m.format = PZPD_FORMAT_JSON; }
            else                                                            { m.format = PZPD_FORMAT_TEXT; }
            m.width = (lines > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t) lines;
            m.meta_flags |= PZPD_META_VALID;
            if (meta_out != NULL) { *meta_out = m; }
            return m.format;
        }
    }

    // Content not recognised: the extension names the format, but the metadata stays invalid
    m.format = (byExt != 0) ? byExt : PZPD_FORMAT_RAW;
    if (meta_out != NULL) { *meta_out = m; }
    return m.format;
}
