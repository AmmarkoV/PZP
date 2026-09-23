/*
PZPDIR PZP Directory Archives
Copyright (C) 2026 Ammar Qammaz

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/** @file pzpdir_cli.c
 *  @brief  The `pzpdir` command line tool: pack, collect, ls, cat, info, verify, unpack.
 *
 *  Record list format (`pzpdir pack out.pzpd list.tsv`, or `-` for stdin), one blob per line:
 *  @code
 *  # key <TAB> stream <TAB> source-path [<TAB> stored-name]
 *  000000000139.jpg	rgb	/data/coco/val2017/000000000139.jpg	val2017/000000000139.jpg
 *  000000000139.jpg	depth	/data/coco/depth_val2017/000000000139.png	depth_val2017/000000000139.png
 *  @group clip_0042
 *  clip_0042/0000	rgb	/video/a/0000.png
 *  @group
 *  @endcode
 *  - Consecutive lines with the same key form one record; record order = line order.
 *  - Lines starting with `#` are comments; `@group NAME` / `@group` start / end a video group.
 *  - Escapes `\t`, `\n`, `\\` allow any character; a leading `\#`, `\@` or `\\` escapes a key
 *    that starts with `#`, `@` or `\`.
 *  - The stored name defaults to the source path.
 *  - Tables: `@table NAME col:type[n] ... [bulk]` and `@global NAME col:type ...` declare them (before
 *    the first record); `@row NAME csv` adds a global row; a record line whose second field names a
 *    declared table carries one CSV row: `key<TAB>persons<TAB>1,388,69,...`.
 *
 *  `pzpdir ls` prints keys and names with the same escaping, so its output can be fed back.
 *
 *  Every read command accepts one archive, a collection file, or several archives (opened as one,
 *  like pzpd_open_many()). With more than one member, `ls` prefixes each line with the member alias
 *  and `unpack` writes each member into `<dir>/<alias>/`.
 *
 *  Repository : https://github.com/AmmarkoV/PZP
 *  @author Ammar Qammaz (AmmarkoV)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE ///< pread / pwrite / mmap / O_CLOEXEC and friends (the DataLoader passes -D_GNU_SOURCE itself)
#endif

#include "pzpdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <ftw.h>
#include <sys/stat.h>

#define CLI_RED    "\033[31m"  ///< ANSI escape: red text
#define CLI_GREEN  "\033[32m"  ///< ANSI escape: green text
#define CLI_NORMAL "\033[0m"   ///< ANSI escape: reset terminal color

/** @brief Print the usage text. */
static void usage(void)
{
    fprintf(stderr,
      "pzpdir %s - PZP directory archives (spec v0.4)\n\n"
      "  pzpdir pack    <out.pzpd> <record-list|-> [--streams a,b,...] [--align 64|4096] [--shard-size 4G]\n"
      "  pzpdir pack    <out.pzpd> <directory>      [--align 64|4096] [--shard-size 4G]\n"
      "  pzpdir collect <out.pzpd> [alias=]<a.pzpd> [alias=]<b.pzpd> ... [--absolute]\n"
      "  pzpdir collect --refresh <collection.pzpd>\n"
      "  pzpdir ls      <archive...> [--long] [--names]\n"
      "  pzpdir cat     <archive...> <record-key|blob-name> [--stream S | --table T] > out\n"
      "  pzpdir export-table <archive...> <table>             (CSV; record tables: key<TAB>row)\n"
      "  pzpdir info    <archive...> [--stream S] [--dups]\n"
      "  pzpdir verify  <archive...> [--blobs]\n"
      "  pzpdir groups  <archive...>                        (video groups: name, first record, frames, shard)\n"
      "  pzpdir unpack  <archive...> <directory>\n"
      "  pzpdir salvage <shard> <directory> [--schemas <archive>] [--list out.tsv]   (index lost: record-header scan)\n"
      "  pzpdir rebuild-manifest <name>.*.pzpd [--out <name>.pzpd]\n"
      "  pzpdir add-table | replace-table <archive> TABLE <list|-> [--missing empty|keep]   (no record data rewritten)\n"
      "  pzpdir drop-table <archive> TABLE\n"
      "  pzpdir add-stream | replace-stream <archive> STREAM <list|-> [--missing keep|drop]   (shard by shard)\n"
      "  pzpdir drop-stream <archive> STREAM\n"
      "  pzpdir compact <archive>                  (drop dead table sections left by table edits)\n"
      "  pzpdir words   <archive...> [TABLE.COLUMN] [--source S | --sources] [--canonical] [--top N | --word W]\n"
      "                 (word index: word<TAB>records<TAB>count by records; --word: keys of the records containing W)\n"
      "  pzpdir reindex <archive> [TABLE.COLUMN [SOURCE_COLUMN]] [--drop]   (no TABLE.COLUMN: rebuild every word index)\n\n"
      "<archive...> = one archive (manifest or shard), a collection file, or several archives opened as one.\n"
      "Record list: one blob per line, key<TAB>stream<TAB>source-path[<TAB>stored-name].\n"
      "Consecutive lines with the same key form one record. '#' comments, '@group NAME' / '@group'.\n"
      "Escapes: \\t \\n \\\\ ; a key starting with # @ or \\ is written with a leading \\.\n"
      "Tables: '@table NAME col:type[n] ... [bulk]', '@global NAME ...', '@row NAME csv', and lines key<TAB>table<TAB>csv.\n"
      "Word indexes: '@words TABLE.COLUMN [SOURCE_COLUMN]' (after the table); merge rules in '@global synonyms word:str canonical:str'.\n",
      pzpdirVersion);
}

/** @brief Parse a size like "4G", "256M", "64K" or "1000000".
 *  @return Bytes, 0 on a malformed value. */
static uint64_t parse_size(const char *s)
{
    char *end = NULL;
    double v = strtod(s, &end);
    if ( (end == s) || (v <= 0) ) { return 0; }
    uint64_t mul = 1;
    if (*end != 0)
    {
        switch (*end)
        {
            case 'k': case 'K': mul = 1024ull; break;
            case 'm': case 'M': mul = 1024ull * 1024ull; break;
            case 'g': case 'G': mul = 1024ull * 1024ull * 1024ull; break;
            case 't': case 'T': mul = 1024ull * 1024ull * 1024ull * 1024ull; break;
            default: return 0;
        }
        if (end[1] != 0) { return 0; }
    }
    return (uint64_t)(v * (double) mul);
}

/** @brief Write bytes to a stream with the record-list escaping (tab, newline, backslash;
 *  a leading #, @ or \ gets a backslash when at_line_start). */
static void put_escaped(FILE *f, const char *s, size_t len, int at_line_start)
{
    for (size_t i = 0; i < len; i++)
    {
        char c = s[i];
        if      (c == '\t') { fputs("\\t", f); }
        else if (c == '\n') { fputs("\\n", f); }
        else if (c == '\\') { fputs("\\\\", f); }
        else if ( at_line_start && (i == 0) && ( (c == '#') || (c == '@') ) ) { fputc('\\', f); fputc(c, f); }
        else { fputc(c, f); }
    }
}

static void put_schema(FILE *f, const pzpd_schema *sc);   // used by info before its definition

//-----------------------------------------------------------------------------------------------
// Record lists
//-----------------------------------------------------------------------------------------------

/** @brief One blob line of a record list. */
struct list_blob
{
    unsigned stream;  ///< Stream id
    char    *path;    ///< Source path (NUL-terminated, unescaped)
    char    *name;    ///< Stored name (unescaped; may contain NUL-free arbitrary bytes)
    size_t   name_len;///< Stored name length
    size_t   line;    ///< Line number, for messages
};

/** @brief One CSV row line of a record list (record or global table). */
struct list_row
{
    unsigned table;   ///< Table index (declaration order)
    char    *csv;     ///< CSV text (unescaped)
    size_t   len;     ///< Its length
    size_t   line;    ///< Line number
};

/** @brief One table declared in a record list. */
struct list_table
{
    char    *name;    ///< Table name
    char    *schema;  ///< Schema text
    unsigned flags;   ///< PZPD_TABLE_GLOBAL / PZPD_TABLE_BULK
    size_t   line;    ///< Line number
};

/** @brief One record of a record list. */
struct list_record
{
    char    *key;       ///< Record key (unescaped)
    size_t   key_len;   ///< Key length
    uint32_t group;     ///< Group id or PZPD_NO_GROUP
    uint32_t frame;     ///< Frame within the group
    struct list_blob *blobs; ///< Blobs
    unsigned blob_count;     ///< Blobs
    struct list_row *rows;   ///< Table rows
    unsigned row_count;      ///< Table rows
    size_t   line;      ///< Line of the first blob
};

/** @brief Unescape a record-list field in place.
 *  @return New length, or (size_t)-1 on a bad escape. */
static size_t unescape(char *s, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i++)
    {
        if (s[i] != '\\') { s[o++] = s[i]; continue; }
        if (i + 1 >= len) { return (size_t) -1; }
        char c = s[++i];
        switch (c)
        {
            case 't':  s[o++] = '\t'; break;
            case 'n':  s[o++] = '\n'; break;
            case '\\': s[o++] = '\\'; break;
            case '#':  s[o++] = '#';  break;
            case '@':  s[o++] = '@';  break;
            default:   return (size_t) -1;
        }
    }
    return o;
}

/** @brief Read a whole file (or stdin for "-") into memory.
 *  @return malloc'd buffer (NUL-terminated), NULL on failure. */
static char *read_all(const char *path, size_t *len)
{
    FILE *f = (strcmp(path, "-") == 0) ? stdin : fopen(path, "rb");
    if (f == NULL) { fprintf(stderr, CLI_RED "cannot open %s: %s" CLI_NORMAL "\n", path, strerror(errno)); return NULL; }
    size_t cap = 1 << 20, n = 0;
    char *buf = (char *) malloc(cap + 1);
    while (buf != NULL)
    {
        size_t r = fread(buf + n, 1, cap - n, f);
        n += r;
        if (r == 0) { break; }
        if (n == cap)
        {
            cap *= 2;
            char *nb = (char *) realloc(buf, cap + 1);
            if (nb == NULL) { free(buf); buf = NULL; break; }
            buf = nb;
        }
    }
    if (f != stdin) { fclose(f); }
    if (buf == NULL) { fprintf(stderr, CLI_RED "out of memory reading %s" CLI_NORMAL "\n", path); return NULL; }
    buf[n] = 0;
    *len = n;
    return buf;
}

/** @brief Split a record-list line at tabs (in place) and unescape the fields.
 *  @return Field count, or -1 on a bad escape. */
static int split_fields(char *line, size_t ll, char **f, size_t *fl, int max)
{
    int nf = 0;
    char *q = line;
    while (nf < max)
    {
        char *tab = memchr(q, '\t', (size_t)(line + ll - q));
        f[nf] = q;
        fl[nf] = (tab != NULL) ? (size_t)(tab - q) : (size_t)(line + ll - q);
        if (tab != NULL) { *tab = 0; }
        size_t u = unescape(f[nf], fl[nf]);
        if (u == (size_t) -1) { return -1; }
        fl[nf] = u;
        f[nf][u] = 0;
        nf++;
        if (tab == NULL) { break; }
        q = tab + 1;
    }
    return nf;
}

/** @brief Parsed record list plus the stream names it uses. */
struct record_list
{
    struct list_record *records; ///< Records in list order
    size_t count;                ///< Records
    char  *streams[PZPD_MAX_STREAMS]; ///< Stream names
    unsigned stream_count;       ///< Streams
    char  *text;                 ///< The list text (fields point into it)
    struct list_table tables[PZPD_MAX_TABLES]; ///< Declared tables
    unsigned table_count;        ///< Declared tables
    struct list_row *grows;      ///< Global rows
    unsigned grow_count;         ///< Global rows
    char   **gnames;             ///< Name of each list group (`@group NAME`), by list group id
    unsigned gname_count;        ///< Groups
    struct { char *table, *column, *source; size_t line; } words[PZPD_MAX_WORD_INDEXES]; ///< `@words` declarations
    unsigned words_count;        ///< Word indexes
};

/** @brief Index of a declared table, -1 if none. */
static int table_index(const struct record_list *L, const char *name)
{
    for (unsigned t = 0; t < L->table_count; t++) { if (!strcmp(L->tables[t].name, name)) { return (int) t; } }
    return -1;
}

/** @brief Find or add a stream name.
 *  @return Stream id, or -1 if the name is unknown and adding is not allowed / too many streams. */
static int stream_index(struct record_list *L, const char *name, int allow_add)
{
    for (unsigned s = 0; s < L->stream_count; s++) { if (strcmp(L->streams[s], name) == 0) { return (int) s; } }
    if (!allow_add || (L->stream_count >= PZPD_MAX_STREAMS) || (strlen(name) > PZPD_MAX_STREAM_NAME) || (name[0] == 0)) { return -1; }
    L->streams[L->stream_count] = strdup(name);
    return (int)(L->stream_count++);
}

/** @brief Parse a record list.
 *  @param fixed_streams Comma-separated stream names (--streams), or NULL to take them from the list.
 *  @return 1 on success, 0 on a syntax error (message printed). */
static int parse_list(const char *path, const char *fixed_streams, struct record_list *L)
{
    memset(L, 0, sizeof(*L));
    size_t len = 0;
    L->text = read_all(path, &len);
    if (L->text == NULL) { return 0; }
    if (fixed_streams != NULL)
    {
        char *tmp = strdup(fixed_streams), *save = NULL;
        for (char *t = strtok_r(tmp, ",", &save); t != NULL; t = strtok_r(NULL, ",", &save))
        {
            if (stream_index(L, t, 0) >= 0 || stream_index(L, t, 1) < 0)
            {
                fprintf(stderr, CLI_RED "--streams: bad or repeated stream name \"%s\" (1..%d bytes, at most %d streams)" CLI_NORMAL "\n", t, PZPD_MAX_STREAM_NAME, PZPD_MAX_STREAMS);
                free(tmp);
                return 0;
            }
        }
        free(tmp);
    }

    size_t cap = 1024;
    L->records = (struct list_record *) calloc(cap, sizeof(struct list_record));
    uint32_t group = PZPD_NO_GROUP, nextGroup = 0, frame = 0;
    size_t lineNo = 0;
    char *p = L->text, *end = L->text + len;
    while (p < end)
    {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char *le = (nl != NULL) ? nl : end;
        *le = 0;
        lineNo++;
        size_t ll = (size_t)(le - p);
        char *line = p;
        p = le + 1;

        if ( (ll == 0) || (line[0] == '#') ) { continue; }
        if (line[0] == '@')
        {
            if ( (strcmp(line, "@group") == 0) ) { group = PZPD_NO_GROUP; continue; }
            if (strncmp(line, "@group ", 7) == 0)
            {
                char *gn = line + 7;
                size_t u = unescape(gn, strlen(gn));
                if ( (u == (size_t) -1) || (u == 0) ) { fprintf(stderr, CLI_RED "%s:%zu: bad group name" CLI_NORMAL "\n", path, lineNo); return 0; }
                gn[u] = 0;
                char **ng = (char **) realloc(L->gnames, (L->gname_count + 1) * sizeof(char *));
                if (ng == NULL) { fprintf(stderr, CLI_RED "out of memory" CLI_NORMAL "\n"); return 0; }
                L->gnames = ng;
                L->gnames[L->gname_count++] = gn;
                group = nextGroup++;
                frame = 0;
                continue;
            }
            int isTable = (strncmp(line, "@table ", 7) == 0), isGlobal = (strncmp(line, "@global ", 8) == 0);
            if (isTable || isGlobal)
            {
                if (L->count > 0) { fprintf(stderr, CLI_RED "%s:%zu: tables must be declared before the first record" CLI_NORMAL "\n", path, lineNo); return 0; }
                if (L->table_count == PZPD_MAX_TABLES) { fprintf(stderr, CLI_RED "%s:%zu: more than %d tables" CLI_NORMAL "\n", path, lineNo, PZPD_MAX_TABLES); return 0; }
                char *nm = line + (isTable ? 7 : 8);
                while (*nm == ' ') { nm++; }
                char *sp = strchr(nm, ' ');
                if (sp == NULL) { fprintf(stderr, CLI_RED "%s:%zu: expected @table NAME col:type ..." CLI_NORMAL "\n", path, lineNo); return 0; }
                *sp = 0;
                char *schema = sp + 1;
                unsigned flags = isGlobal ? PZPD_TABLE_GLOBAL : 0;
                size_t sl = strlen(schema);
                while ( (sl > 0) && (schema[sl - 1] == ' ') ) { schema[--sl] = 0; }
                if ( (sl >= 5) && !strcmp(schema + sl - 4, "bulk") && (schema[sl - 5] == ' ') ) { flags |= PZPD_TABLE_BULK; schema[sl - 5] = 0; }
                if ( (table_index(L, nm) >= 0) || (stream_index(L, nm, 0) >= 0) ) { fprintf(stderr, CLI_RED "%s:%zu: \"%s\" is already declared" CLI_NORMAL "\n", path, lineNo, nm); return 0; }
                struct list_table *lt = &L->tables[L->table_count++];
                lt->name = nm; lt->schema = schema; lt->flags = flags; lt->line = lineNo;
                continue;
            }
            if (strncmp(line, "@row ", 5) == 0)
            {
                char *nm = line + 5;
                char *sp = strchr(nm, ' ');
                if (sp == NULL) { fprintf(stderr, CLI_RED "%s:%zu: expected @row NAME csv" CLI_NORMAL "\n", path, lineNo); return 0; }
                *sp = 0;
                int t = table_index(L, nm);
                if ( (t < 0) || !(L->tables[t].flags & PZPD_TABLE_GLOBAL) ) { fprintf(stderr, CLI_RED "%s:%zu: \"%s\" is not a declared @global table" CLI_NORMAL "\n", path, lineNo, nm); return 0; }
                char *csv = sp + 1;
                size_t u = unescape(csv, strlen(csv));
                if (u == (size_t) -1) { fprintf(stderr, CLI_RED "%s:%zu: bad escape" CLI_NORMAL "\n", path, lineNo); return 0; }
                struct list_row *nr = (struct list_row *) realloc(L->grows, (L->grow_count + 1) * sizeof(struct list_row));
                if (nr == NULL) { fprintf(stderr, CLI_RED "out of memory" CLI_NORMAL "\n"); return 0; }
                L->grows = nr;
                L->grows[L->grow_count].table = (unsigned) t;
                L->grows[L->grow_count].csv = csv;
                L->grows[L->grow_count].len = u;
                L->grows[L->grow_count].line = lineNo;
                L->grow_count++;
                continue;
            }
            if (strncmp(line, "@words ", 7) == 0)
            {
                // @words TABLE.COLUMN [SOURCE_COLUMN]
                if (L->count > 0) { fprintf(stderr, CLI_RED "%s:%zu: word indexes must be declared before the first record" CLI_NORMAL "\n", path, lineNo); return 0; }
                if (L->words_count == PZPD_MAX_WORD_INDEXES) { fprintf(stderr, CLI_RED "%s:%zu: more than %d word indexes" CLI_NORMAL "\n", path, lineNo, PZPD_MAX_WORD_INDEXES); return 0; }
                char *spec = line + 7, *save = NULL;
                char *tc = strtok_r(spec, " ", &save), *src = strtok_r(NULL, " ", &save);
                char *dot = (tc != NULL) ? strchr(tc, '.') : NULL;
                if ( (dot == NULL) || (dot == tc) || (dot[1] == 0) || (strtok_r(NULL, " ", &save) != NULL) )
                    { fprintf(stderr, CLI_RED "%s:%zu: expected @words TABLE.COLUMN [SOURCE_COLUMN]" CLI_NORMAL "\n", path, lineNo); return 0; }
                *dot = 0;
                L->words[L->words_count].table  = tc;
                L->words[L->words_count].column = dot + 1;
                L->words[L->words_count].source = src;
                L->words[L->words_count].line   = lineNo;
                L->words_count++;
                continue;
            }
            fprintf(stderr, CLI_RED "%s:%zu: unknown directive \"%.40s\"" CLI_NORMAL "\n", path, lineNo, line);
            return 0;
        }

        // key <TAB> stream <TAB> path [<TAB> name]
        char *f[5] = {0};
        size_t fl[5] = {0};
        int nf = split_fields(line, ll, f, fl, 5);
        if (nf < 0) { fprintf(stderr, CLI_RED "%s:%zu: bad escape (use \\t \\n \\\\ \\# \\@)" CLI_NORMAL "\n", path, lineNo); return 0; }
        if ( (nf < 3) || (nf > 4) )
        {
            fprintf(stderr, CLI_RED "%s:%zu: expected key<TAB>stream<TAB>path[<TAB>name], found %d field(s)" CLI_NORMAL "\n", path, lineNo, nf);
            return 0;
        }
        if ( (fl[0] == 0) || (fl[2] == 0) ) { fprintf(stderr, CLI_RED "%s:%zu: empty key or path" CLI_NORMAL "\n", path, lineNo); return 0; }
        int tix = table_index(L, f[1]);
        if (tix >= 0)
        {
            if ( (nf != 3) || (L->tables[tix].flags & PZPD_TABLE_GLOBAL) ) { fprintf(stderr, CLI_RED "%s:%zu: a row line is key<TAB>table<TAB>csv, for a record table" CLI_NORMAL "\n", path, lineNo); return 0; }
            struct list_record *rr = (L->count > 0) ? &L->records[L->count - 1] : NULL;
            if ( (rr == NULL) || (rr->key_len != fl[0]) || (memcmp(rr->key, f[0], fl[0]) != 0) )
            {
                if (L->count == cap)
                {
                    cap *= 2;
                    struct list_record *nrr = (struct list_record *) realloc(L->records, cap * sizeof(struct list_record));
                    if (nrr == NULL) { fprintf(stderr, CLI_RED "out of memory" CLI_NORMAL "\n"); return 0; }
                    L->records = nrr;
                }
                rr = &L->records[L->count++];
                memset(rr, 0, sizeof(*rr));
                rr->key = f[0]; rr->key_len = fl[0]; rr->group = group; rr->frame = (group == PZPD_NO_GROUP) ? 0 : frame++; rr->line = lineNo;
            }
            struct list_row *nr = (struct list_row *) realloc(rr->rows, (rr->row_count + 1) * sizeof(struct list_row));
            if (nr == NULL) { fprintf(stderr, CLI_RED "out of memory" CLI_NORMAL "\n"); return 0; }
            rr->rows = nr;
            rr->rows[rr->row_count].table = (unsigned) tix;
            rr->rows[rr->row_count].csv   = f[2];
            rr->rows[rr->row_count].len   = fl[2];
            rr->rows[rr->row_count].line  = lineNo;
            rr->row_count++;
            continue;
        }
        int s = stream_index(L, f[1], fixed_streams == NULL);
        if (s < 0) { fprintf(stderr, CLI_RED "%s:%zu: unknown or invalid stream \"%s\"" CLI_NORMAL "\n", path, lineNo, f[1]); return 0; }

        struct list_record *r = (L->count > 0) ? &L->records[L->count - 1] : NULL;
        if ( (r == NULL) || (r->key_len != fl[0]) || (memcmp(r->key, f[0], fl[0]) != 0) )
        {
            if (L->count == cap)
            {
                cap *= 2;
                struct list_record *nr = (struct list_record *) realloc(L->records, cap * sizeof(struct list_record));
                if (nr == NULL) { fprintf(stderr, CLI_RED "out of memory" CLI_NORMAL "\n"); return 0; }
                L->records = nr;
            }
            r = &L->records[L->count++];
            memset(r, 0, sizeof(*r));
            r->key     = f[0];
            r->key_len = fl[0];
            r->group   = group;
            r->frame   = (group == PZPD_NO_GROUP) ? 0 : frame++;
            r->line    = lineNo;
        }
        struct list_blob *nb = (struct list_blob *) realloc(r->blobs, (r->blob_count + 1) * sizeof(struct list_blob));
        if (nb == NULL) { fprintf(stderr, CLI_RED "out of memory" CLI_NORMAL "\n"); return 0; }
        r->blobs = nb;
        r->blobs[r->blob_count].stream   = (unsigned) s;
        r->blobs[r->blob_count].path     = f[2];
        r->blobs[r->blob_count].name     = (nf == 4) ? f[3] : f[2];
        r->blobs[r->blob_count].name_len = (nf == 4) ? fl[3] : fl[2];
        r->blobs[r->blob_count].line     = lineNo;
        r->blob_count++;
    }
    if (L->stream_count == 0) { fprintf(stderr, CLI_RED "%s: no blob lines (records need at least one blob)" CLI_NORMAL "\n", path); return 0; }
    return 1;
}

/** @brief Free a parsed record list. */
static void free_list(struct record_list *L)
{
    for (size_t i = 0; i < L->count; i++) { free(L->records[i].blobs); free(L->records[i].rows); }
    free(L->grows);
    free(L->gnames);
    free(L->records);
    for (unsigned s = 0; s < L->stream_count; s++) { free(L->streams[s]); }
    free(L->text);
}

//-----------------------------------------------------------------------------------------------
// pack
//-----------------------------------------------------------------------------------------------

/** @brief Write a parsed record list to an archive.
 *  @return 0 on success, 1 on failure. */
static int pack_list(const char *out, struct record_list *L, uint64_t shard, uint32_t align, const char *src)
{
    pzpd_writer_opts o;
    memset(&o, 0, sizeof(o));
    o.streams         = (const char *const *) L->streams;
    o.stream_count    = L->stream_count;
    o.shard_max_bytes = shard;
    o.align           = align;
    pzpd_writer *w = pzpd_writer_create(out, &o);
    if (w == NULL) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); return 1; }
    for (unsigned t = 0; t < L->table_count; t++)
    {
        if (pzpd_writer_table(w, L->tables[t].name, L->tables[t].schema, L->tables[t].flags) < 0)
            { fprintf(stderr, CLI_RED "%s:%zu: %s" CLI_NORMAL "\n", src, L->tables[t].line, pzpd_last_error()); pzpd_writer_abort(w); return 1; }
    }
    for (unsigned g = 0; g < L->grow_count; g++)
    {
        if (!pzpd_writer_global_rows_csv(w, L->grows[g].table, L->grows[g].csv, L->grows[g].len))
            { fprintf(stderr, CLI_RED "%s:%zu: %s" CLI_NORMAL "\n", src, L->grows[g].line, pzpd_last_error()); pzpd_writer_abort(w); return 1; }
    }
    for (unsigned k = 0; k < L->words_count; k++)
    {
        if (!pzpd_writer_words(w, L->words[k].table, L->words[k].column, L->words[k].source))
            { fprintf(stderr, CLI_RED "%s:%zu: %s" CLI_NORMAL "\n", src, L->words[k].line, pzpd_last_error()); pzpd_writer_abort(w); return 1; }
    }
    uint32_t curGroupId = PZPD_NO_GROUP;
    for (size_t i = 0; i < L->count; i++)
    {
        struct list_record *r = &L->records[i];
        uint32_t wgroup = PZPD_NO_GROUP;
        if (r->group != PZPD_NO_GROUP)
        {
            // First record of a list group: register it, with the size of all its files as the hint
            if ( (i == 0) || (L->records[i - 1].group != r->group) )
            {
                uint64_t hint = 0;
                for (size_t k = i; (k < L->count) && (L->records[k].group == r->group); k++)
                {
                    for (unsigned b = 0; b < L->records[k].blob_count; b++) { struct stat st; if (stat(L->records[k].blobs[b].path, &st) == 0) { hint += (uint64_t) st.st_size + 64; } }
                }
                const char *gn = (r->group < L->gname_count) ? L->gnames[r->group] : "";
                int64_t id = pzpd_writer_group(w, gn, strlen(gn), hint);
                if (id < 0) { fprintf(stderr, CLI_RED "%s:%zu: %s" CLI_NORMAL "\n", src, r->line, pzpd_last_error()); pzpd_writer_abort(w); return 1; }
                curGroupId = (uint32_t) id;
            }
            wgroup = curGroupId;
        }
        int ok = pzpd_writer_begin(w, r->key, r->key_len, wgroup, r->frame);
        for (unsigned k = 0; ok && (k < r->row_count); k++)
        {
            ok = pzpd_writer_rows_csv(w, r->rows[k].table, r->rows[k].csv, r->rows[k].len);
            if (!ok) { fprintf(stderr, CLI_RED "%s:%zu: %s" CLI_NORMAL "\n", src, r->rows[k].line, pzpd_last_error()); }
        }
        for (unsigned b = 0; ok && (b < r->blob_count); b++)
        {
            ok = pzpd_writer_blob_file(w, r->blobs[b].stream, r->blobs[b].name, r->blobs[b].name_len, r->blobs[b].path);
            if (!ok) { fprintf(stderr, CLI_RED "%s:%zu: %s" CLI_NORMAL "\n", src, r->blobs[b].line, pzpd_last_error()); }
        }
        if (ok && !pzpd_writer_end(w)) { fprintf(stderr, CLI_RED "%s:%zu: %s" CLI_NORMAL "\n", src, r->line, pzpd_last_error()); ok = 0; }
        if (!ok) { pzpd_writer_abort(w); return 1; }
        if ( ((i + 1) % 1000 == 0) || (i + 1 == L->count) ) { fprintf(stderr, "\rpacked %zu / %zu records", i + 1, L->count); }
    }
    fprintf(stderr, "\n");
    if (!pzpd_writer_finish(w)) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); return 1; }
    fprintf(stderr, CLI_GREEN "wrote %s" CLI_NORMAL "\n", out);
    return 0;
}

static char **walkFiles;        ///< Files found by the directory walk (relative paths)
static size_t walkCount;        ///< Files found
static size_t walkCap;          ///< Allocated slots
static size_t walkPrefix;       ///< Length of the root prefix to strip

/** @brief nftw() callback collecting regular files. */
static int walk_cb(const char *fpath, const struct stat *sb, int type, struct FTW *ftw)
{
    (void) sb; (void) ftw;
    if (type != FTW_F) { return 0; }
    if (walkCount == walkCap)
    {
        walkCap = (walkCap == 0) ? 1024 : walkCap * 2;
        char **n = (char **) realloc(walkFiles, walkCap * sizeof(char *));
        if (n == NULL) { return 1; }
        walkFiles = n;
    }
    walkFiles[walkCount++] = strdup(fpath + walkPrefix);
    return 0;
}

/** @brief qsort comparator for C strings (byte order). */
static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *) a, *(char *const *) b);
}

/** @brief Pack a directory tree as a plain single-stream archive (key = relative path).
 *  @return 0 on success, 1 on failure. */
static int pack_dir(const char *out, const char *dir, uint64_t shard, uint32_t align)
{
    size_t dl = strlen(dir);
    while ( (dl > 1) && (dir[dl - 1] == '/') ) { dl--; }
    char root[4096];
    snprintf(root, sizeof(root), "%.*s", (int) dl, dir);
    walkPrefix = strlen(root) + 1;
    if (nftw(root, walk_cb, 64, FTW_PHYS) != 0) { fprintf(stderr, CLI_RED "cannot walk %s: %s" CLI_NORMAL "\n", root, strerror(errno)); return 1; }
    qsort(walkFiles, walkCount, sizeof(char *), cmp_str);

    const char *streams[1] = { "data" };
    pzpd_writer_opts o = { streams, 1, shard, align };
    pzpd_writer *w = pzpd_writer_create(out, &o);
    if (w == NULL) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); return 1; }
    int rc = 0;
    for (size_t i = 0; i < walkCount; i++)
    {
        char full[8192];
        snprintf(full, sizeof(full), "%s/%s", root, walkFiles[i]);
        size_t kl = strlen(walkFiles[i]);
        if ( !pzpd_writer_begin(w, walkFiles[i], kl, PZPD_NO_GROUP, 0) ||
             !pzpd_writer_blob_file(w, 0, walkFiles[i], kl, full) ||
             !pzpd_writer_end(w) )
        {
            fprintf(stderr, CLI_RED "%s: %s" CLI_NORMAL "\n", full, pzpd_last_error());
            rc = 1;
            break;
        }
        if ( ((i + 1) % 1000 == 0) || (i + 1 == walkCount) ) { fprintf(stderr, "\rpacked %zu / %zu files", i + 1, walkCount); }
    }
    fprintf(stderr, "\n");
    for (size_t i = 0; i < walkCount; i++) { free(walkFiles[i]); }
    free(walkFiles);
    if (rc != 0) { pzpd_writer_abort(w); return 1; }
    if (!pzpd_writer_finish(w)) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); return 1; }
    fprintf(stderr, CLI_GREEN "wrote %s" CLI_NORMAL "\n", out);
    return 0;
}

//-----------------------------------------------------------------------------------------------
// ls / cat / info / verify / unpack
//-----------------------------------------------------------------------------------------------

/** @brief Open one archive / collection, or several archives as one; print the error on failure. */
static pzpd *open_or_die(const char *const *paths, int n)
{
    pzpd *a = (n == 1) ? pzpd_open(paths[0], 0) : pzpd_open_many(paths, NULL, (unsigned) n, 0);
    if (a == NULL) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); }
    return a;
}

/** @brief Format metadata as "WxHxC@bits", "N lines", ... into out. */
static const char *meta_str(const pzpd_blob_meta *m, char *out, size_t cap)
{
    if ( !(m->meta_flags & PZPD_META_VALID) ) { snprintf(out, cap, "-"); return out; }
    if ( (m->format == PZPD_FORMAT_JSON) || (m->format == PZPD_FORMAT_TEXT) || (m->format == PZPD_FORMAT_CSV) || (m->format == PZPD_FORMAT_TSV) )
        { snprintf(out, cap, "%u lines", m->width); return out; }
    int n = snprintf(out, cap, "%ux%ux%u@%u%s", m->width, m->height, m->channels, m->bits, (m->meta_flags & PZPD_META_FLOAT) ? "f" : "");
    if ( (m->frames > 1) && (n > 0) && ((size_t) n < cap) ) { snprintf(out + n, cap - (size_t) n, " %u frames", m->frames); }
    return out;
}

/** @brief `pzpdir ls`. @return exit code. */
static int cmd_ls(const char *const *paths, int np, int lng, int names)
{
    pzpd *a = open_or_die(paths, np);
    if (a == NULL) { return 1; }
    uint64_t n = pzpd_count(a);
    unsigned S = pzpd_stream_count(a);
    int multi = (pzpd_member_count(a) > 1), rc = 0;
    for (uint64_t i = 0; i < n; i++)
    {
        size_t kl = 0;
        const char *k = pzpd_record_key(a, i, &kl);
        if (k == NULL) { fprintf(stderr, CLI_RED "record %llu: %s" CLI_NORMAL "\n", (unsigned long long) i, pzpd_last_error()); rc = 1; continue; }
        const char *alias = multi ? pzpd_member_alias(a, (unsigned) pzpd_member_of(a, i, NULL)) : NULL;
        if (!lng && !names) { if (alias) { fprintf(stdout, "%s\t", alias); } put_escaped(stdout, k, kl, !multi); fputc('\n', stdout); continue; }
        for (unsigned s = 0; s < S; s++)
        {
            pzpd_blob_info bi;
            if (!pzpd_blob_info_get(a, i, s, &bi)) { fprintf(stderr, CLI_RED "record %llu: %s" CLI_NORMAL "\n", (unsigned long long) i, pzpd_last_error()); rc = 1; continue; }
            if (!bi.present) { continue; }
            if (alias) { fprintf(stdout, "%s\t", alias); }
            put_escaped(stdout, k, kl, !multi);
            fprintf(stdout, "\t%s\t", pzpd_stream_name(a, s));
            if (lng)
            {
                char f[5], m[64];
                fprintf(stdout, "%s\t%s\t%llu\t", pzpd_format_name(bi.meta.format, f), meta_str(&bi.meta, m, sizeof(m)), (unsigned long long) bi.size);
            }
            put_escaped(stdout, bi.name, bi.name_len, 0);
            fputc('\n', stdout);
        }
    }
    pzpd_close(a);
    return rc;
}

/** @brief `pzpdir cat`. @return exit code. */
static int cmd_cat(const char *const *paths, int np, const char *what, const char *stream, const char *table)
{
    pzpd *a = open_or_die(paths, np);
    if (a == NULL) { return 1; }
    int s = -1;
    int64_t i = pzpd_find(a, what, strlen(what), &s);
    if (i < 0) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); pzpd_close(a); return 1; }
    if (table != NULL)
    {
        int t = pzpd_table_id(a, table);
        if (t < 0) { fprintf(stderr, CLI_RED "no table \"%s\"" CLI_NORMAL "\n", table); pzpd_close(a); return 1; }
        ssize_t n = pzpd_table_csv(a, (uint64_t) i, (unsigned) t, NULL, 0);
        char *buf = (n >= 0) ? (char *) malloc((size_t) n + 1) : NULL;
        int rc = 1;
        if ( (buf != NULL) && (pzpd_table_csv(a, (uint64_t) i, (unsigned) t, buf, (size_t) n + 1) == n) ) { fwrite(buf, 1, (size_t) n, stdout); rc = 0; }
        else { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); }
        free(buf);
        pzpd_close(a);
        return rc;
    }
    if (stream != NULL)
    {
        s = pzpd_stream_id(a, stream);
        if (s < 0) { fprintf(stderr, CLI_RED "no stream \"%s\"" CLI_NORMAL "\n", stream); pzpd_close(a); return 1; }
    }
    else if (s < 0)
    {
        // A record key without --stream: fine if the record has exactly one blob
        unsigned S = pzpd_stream_count(a), found = 0;
        for (unsigned t = 0; t < S; t++)
        {
            pzpd_blob_info bi;
            if (pzpd_blob_info_get(a, (uint64_t) i, t, &bi) && bi.present) { s = (int) t; found++; }
        }
        if (found != 1) { fprintf(stderr, CLI_RED "record has %u blobs, choose one with --stream" CLI_NORMAL "\n", found); pzpd_close(a); return 1; }
    }
    size_t size = 0;
    const void *data = pzpd_view(a, (uint64_t) i, (unsigned) s, &size);
    int rc = 0;
    if ( (data == NULL) && (size == 0) && (pzpd_last_error_code() != PZPD_OK) ) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); rc = 1; }
    else if (data == NULL) { fprintf(stderr, CLI_RED "the record has no blob in stream \"%s\"" CLI_NORMAL "\n", pzpd_stream_name(a, (unsigned) s)); rc = 1; }
    else if (fwrite(data, 1, size, stdout) != size) { fprintf(stderr, CLI_RED "write failed" CLI_NORMAL "\n"); rc = 1; }
    pzpd_close(a);
    return rc;
}

/** @brief Counter of one metadata combination for `info`. */
struct combo
{
    uint32_t format;  ///< FourCC
    pzpd_blob_meta m; ///< Metadata (format, dims, channels, bits, flags)
    uint64_t count;   ///< Blobs with this combination
};

/** @brief qsort comparator: most frequent combination first. */
static int cmp_combo(const void *a, const void *b)
{
    const struct combo *x = (const struct combo *) a, *y = (const struct combo *) b;
    return (x->count < y->count) ? 1 : (x->count > y->count) ? -1 : 0;
}

/** @brief `pzpdir info --dups`: record keys present in more than one member. @return exit code. */
static int cmd_dups(pzpd *a)
{
    uint64_t n = pzpd_count(a), dups = 0;
    int64_t ords[64];
    int streams[64];
    for (uint64_t i = 0; i < n; i++)
    {
        size_t kl = 0;
        const char *k = pzpd_record_key(a, i, &kl);
        if (k == NULL) { continue; }
        size_t t = pzpd_find_all(a, k, kl, ords, streams, 64);
        size_t shown = (t < 64) ? t : 64, keyMatches = 0;
        for (size_t j = 0; j < shown; j++) { if (streams[j] == -1) { keyMatches++; } }
        if ( (keyMatches < 2) || (ords[0] != (int64_t) i) ) { continue; }   // print each key once, at its first match
        put_escaped(stdout, k, kl, 1);
        int first = 1;
        for (size_t j = 0; j < shown; j++)
        {
            if (streams[j] != -1) { continue; }
            fprintf(stdout, "%s%s:%llu", first ? "\t" : ",", pzpd_member_alias(a, (unsigned) pzpd_member_of(a, (uint64_t) ords[j], NULL)), (unsigned long long) ords[j]);
            first = 0;
        }
        fputc('\n', stdout);
        dups++;
    }
    fprintf(stderr, "%llu record key(s) present in more than one member\n", (unsigned long long) dups);
    return 0;
}

/** @brief `pzpdir info`. @return exit code. */
static int cmd_info(const char *const *paths, int np, const char *only, int dups)
{
    pzpd *a = open_or_die(paths, np);
    if (a == NULL) { return 1; }
    if (dups) { int r = cmd_dups(a); pzpd_close(a); return r; }
    uint64_t n = pzpd_count(a);
    unsigned S = pzpd_stream_count(a), shards = pzpd_shard_count(a), M = pzpd_member_count(a);
    printf("archive  %s%s\nrecords  %llu\nstreams  %u\nshards   %u\nmembers  %u\n", paths[0], (np > 1) ? " ..." : "", (unsigned long long) n, S, shards, M);
    for (unsigned i = 0; (M > 1) && (i < M); i++)
    {
        uint64_t f = 0, c = 0;
        pzpd_member_range(a, i, &f, &c);
        size_t kl;
        int avail = (c == 0) || (pzpd_record_key(a, f, &kl) != NULL) || (pzpd_last_error_code() != PZPD_E_MEMBER_MISSING);
        printf("  member %u  %s  records %llu..%llu%s\n", i, pzpd_member_alias(a, i), (unsigned long long) f, (unsigned long long)(f + c), avail ? "" : "  UNAVAILABLE");
        if (!avail) { printf("           %s\n", pzpd_last_error()); }
    }
    uint64_t total = 0;
    int rc = 0;
    for (unsigned i = 0; i < shards; i++)
    {
        pzpd_shard_info si;
        pzpd_shard_info_get(a, i, &si);
        static const char *pfMode[] = { "AUTO", "MAP", "PAGECACHE", "BUFFERS" };
        int mode = si.available ? pzpd_prefetch_auto_mode(a, i) : -1;
        printf("  shard %u  %s  records %llu..%llu  %.1f MB", i, si.path, (unsigned long long) si.first_ordinal,
               (unsigned long long)(si.first_ordinal + si.record_count), (double) si.file_bytes / 1e6);
        if (si.available)
        {
            printf("  %s  prefetch %s%s\n", (si.storage == PZPD_STORAGE_RAM) ? "ram" : "block", (mode >= 0) && (mode <= 3) ? pfMode[mode] : "?",
                   (si.recovery == 1) ? "  RECOVERED (backup superblock)" : (si.recovery == 2) ? "  RECOVERED (superblock rebuilt from the index sections)" : "");
        }
        else              { printf("  UNAVAILABLE\n"); }
        total += si.file_bytes;
        if (!si.available) { rc = 1; }
    }
    printf("  total %.1f MB\n", (double) total / 1e6);
    for (unsigned t = 0; t < pzpd_table_count(a); t++)
    {
        const pzpd_schema *sc = pzpd_table_schema(a, t);
        uint64_t rows = 0;
        if (sc->flags & PZPD_TABLE_GLOBAL) { for (unsigned m = 0; m < M; m++) { rows += pzpd_global_rows(a, m, t, NULL); } }
        else
        {
            for (unsigned sh = 0; sh < shards; sh++) { pzpd_table_view v; if (pzpd_table_shard_view(a, sh, t, &v)) { rows += v.total_rows; } }
        }
        printf("table %s  %s%s  %llu rows  %u B/row  ", sc->name, (sc->flags & PZPD_TABLE_GLOBAL) ? "global" : "record", (sc->flags & PZPD_TABLE_BULK) ? " bulk" : "",
               (unsigned long long) rows, sc->row_stride);
        put_schema(stdout, sc);
        printf("\n");
    }
    const char *wt, *wc, *ws;
    for (unsigned k = 0; pzpd_words_index(a, k, &wt, &wc, &ws); k++)
    {
        pzpd_words *w = NULL;
        uint64_t cov = 0;
        if (!pzpd_words_open(a, wt, wc, NULL, 0, 0, &w)) { printf("words %s.%s  DAMAGED: %s\n", wt, wc, pzpd_last_error()); rc = 1; continue; }
        pzpd_words_info(w, NULL, &cov);
        printf("words %s.%s  %u words  records covered %llu", wt, wc, pzpd_words_count(w), (unsigned long long) cov);
        pzpd_words_close(w);
        if (ws[0] != 0)
        {
            const char *nm[PZPD_MAX_WORD_SOURCES]; size_t ln[PZPD_MAX_WORD_SOURCES];
            size_t n = pzpd_words_sources(a, wt, wc, nm, ln, PZPD_MAX_WORD_SOURCES);
            printf("  per %s:", ws);
            for (size_t i = 0; (i < n) && (i < PZPD_MAX_WORD_SOURCES); i++) { printf(" %.*s", (int) ln[i], nm[i]); }
        }
        printf("\n");
    }
    for (unsigned s = 0; s < S; s++)
    {
        if ( (only != NULL) && (strcmp(only, pzpd_stream_name(a, s)) != 0) ) { continue; }
        uint64_t present = 0, bytes = 0;
        size_t cc = 0, ccap = 64;
        struct combo *combos = (struct combo *) calloc(ccap, sizeof(struct combo));
        for (uint64_t i = 0; (combos != NULL) && (i < n); i++)
        {
            pzpd_blob_info bi;
            if (!pzpd_blob_info_get(a, i, s, &bi)) { rc = 1; continue; }
            if (!bi.present) { continue; }
            present++;
            bytes += bi.size;
            pzpd_blob_meta m = bi.meta;
            size_t j;
            for (j = 0; j < cc; j++) { if (memcmp(&combos[j].m, &m, sizeof(m)) == 0) { break; } }
            if (j == cc)
            {
                if (cc == ccap) { ccap *= 2; struct combo *nc = (struct combo *) realloc(combos, ccap * sizeof(struct combo)); if (nc == NULL) { break; } combos = nc; }
                memset(&combos[cc], 0, sizeof(combos[cc]));
                combos[cc].m = m;
                cc++;
            }
            combos[j].count++;
        }
        printf("stream %s  present %llu / %llu  %.1f MB  avg %.1f KB\n", pzpd_stream_name(a, s), (unsigned long long) present,
               (unsigned long long) n, (double) bytes / 1e6, present ? (double) bytes / (double) present / 1024.0 : 0.0);
        if (combos != NULL)
        {
            qsort(combos, cc, sizeof(struct combo), cmp_combo);
            for (size_t j = 0; (j < cc) && (j < 8); j++)
            {
                char f[5], m[64];
                printf("  %8llu  %s  %s\n", (unsigned long long) combos[j].count, pzpd_format_name(combos[j].m.format, f), meta_str(&combos[j].m, m, sizeof(m)));
            }
            if (cc > 8) { printf("  ... %zu more combinations\n", cc - 8); }
        }
        free(combos);
    }
    pzpd_close(a);
    return rc;
}

/** @brief Print a schema as `name:type[count] ...` (the form `@table` takes). */
static void put_schema(FILE *f, const pzpd_schema *sc)
{
    static const char *tn[] = { "?", "u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "f32", "f64", "str" };
    for (unsigned c = 0; c < sc->ncols; c++)
    {
        const pzpd_column *col = &sc->cols[c];
        if (col->count > 1) { fprintf(f, "%s%s:%s[%u]", c ? " " : "", col->name, (col->type <= 11) ? tn[col->type] : "?", col->count); }
        else                { fprintf(f, "%s%s:%s", c ? " " : "", col->name, (col->type <= 11) ? tn[col->type] : "?"); }
    }
}

/** @brief Print CSV text as record-list lines: each row (split at newlines outside quotes) is
 *  list-escaped and preceded by `prefix` (already escaped). */
static void put_rows(FILE *f, const char *prefix, const char *csv, size_t n)
{
    size_t start = 0;
    int inQuote = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (csv[i] == '"') { inQuote = !inQuote; }
        else if ( (csv[i] == '\n') && !inQuote )
        {
            fputs(prefix, f);
            put_escaped(f, csv + start, i - start, 0);
            fputc('\n', f);
            start = i + 1;
        }
    }
}

/** @brief `pzpdir export-table`: every row as record-list lines (`key<TAB>table<TAB>csv`, or `@row table csv`
 *  for global tables), so the output can be packed again; alias-prefixed with several members. @return exit code. */
static int cmd_export_table(const char *const *paths, int np, const char *table)
{
    pzpd *a = open_or_die(paths, np);
    if (a == NULL) { return 1; }
    int t = pzpd_table_id(a, table);
    if (t < 0) { fprintf(stderr, CLI_RED "no table \"%s\"" CLI_NORMAL "\n", table); pzpd_close(a); return 1; }
    const pzpd_schema *sc = pzpd_table_schema(a, (unsigned) t);
    int multi = (pzpd_member_count(a) > 1), rc = 0;
    size_t cap = 1 << 16;
    char *buf = (char *) malloc(cap);
    if (sc->flags & PZPD_TABLE_GLOBAL)
    {
        for (unsigned m = 0; m < pzpd_member_count(a); m++)
        {
            ssize_t n = pzpd_global_csv(a, m, (unsigned) t, NULL, 0);
            if (n < 0) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); rc = 1; continue; }
            if ((size_t) n + 1 > cap) { cap = (size_t) n + 1; buf = (char *) realloc(buf, cap); }
            pzpd_global_csv(a, m, (unsigned) t, buf, cap);
            char prefix[256];
            snprintf(prefix, sizeof(prefix), "%s%s@row %s ", multi ? pzpd_member_alias(a, m) : "", multi ? "\t" : "", sc->name);
            put_rows(stdout, prefix, buf, (size_t) n);
        }
    }
    else
    {
        uint64_t N = pzpd_count(a);
        for (uint64_t i = 0; i < N; i++)
        {
            ssize_t n = pzpd_table_csv(a, i, (unsigned) t, NULL, 0);
            if (n < 0) { fprintf(stderr, CLI_RED "record %llu: %s" CLI_NORMAL "\n", (unsigned long long) i, pzpd_last_error()); rc = 1; continue; }
            if (n == 0) { continue; }
            if ((size_t) n + 1 > cap) { cap = (size_t) n + 1; buf = (char *) realloc(buf, cap); }
            pzpd_table_csv(a, i, (unsigned) t, buf, cap);
            size_t kl = 0;
            const char *k = pzpd_record_key(a, i, &kl);
            // prefix = [alias<TAB>] escaped-key <TAB> table <TAB>
            char *pb = NULL;
            size_t pl = 0;
            FILE *pf = open_memstream(&pb, &pl);
            if (pf == NULL) { rc = 1; continue; }
            if (multi) { fprintf(pf, "%s\t", pzpd_member_alias(a, (unsigned) pzpd_member_of(a, i, NULL))); }
            put_escaped(pf, k, kl, !multi);
            fprintf(pf, "\t%s\t", sc->name);
            fclose(pf);
            put_rows(stdout, pb, buf, (size_t) n);
            free(pb);
        }
    }
    free(buf);
    pzpd_close(a);
    return rc;
}

/** @brief `pzpdir verify`. @return exit code. */
static int cmd_verify(const char *const *paths, int np, int blobs)
{
    pzpd *a = open_or_die(paths, np);
    if (a == NULL) { return 1; }
    uint64_t bad = 0, n = pzpd_count(a);
    for (unsigned i = 0; i < pzpd_shard_count(a); i++)
    {
        if (!pzpd_verify_shard(a, i)) { fprintf(stderr, CLI_RED "shard %u: %s" CLI_NORMAL "\n", i, pzpd_last_error()); bad++; }
    }
    for (uint64_t i = 0; i < n; i++)
    {
        if (!pzpd_verify_record(a, i, blobs)) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); bad++; }
        if ( ((i + 1) % 10000 == 0) || (i + 1 == n) ) { fprintf(stderr, "\rverified %llu / %llu records", (unsigned long long)(i + 1), (unsigned long long) n); }
    }
    fprintf(stderr, "\n");
    pzpd_close(a);
    if (bad == 0) { fprintf(stderr, CLI_GREEN "OK: %llu records%s" CLI_NORMAL "\n", (unsigned long long) n, blobs ? ", payload checksums included" : " (headers; add --blobs for payloads)"); return 0; }
    fprintf(stderr, CLI_RED "%llu problem(s)" CLI_NORMAL "\n", (unsigned long long) bad);
    return 1;
}

/** @brief Check that a stored name is safe to write under a directory: relative, no empty,
 *  "." or ".." components. */
static int safe_name(const char *s, size_t len)
{
    if ( (len == 0) || (s[0] == '/') ) { return 0; }
    size_t start = 0;
    for (size_t i = 0; i <= len; i++)
    {
        if ( (i == len) || (s[i] == '/') )
        {
            size_t cl = i - start;
            if (cl == 0) { return 0; }
            if ( (cl == 1) && (s[start] == '.') ) { return 0; }
            if ( (cl == 2) && (s[start] == '.') && (s[start + 1] == '.') ) { return 0; }
            start = i + 1;
        }
    }
    return 1;
}

/** @brief mkdir -p for the parent directories of path (path is modified temporarily). */
static int make_parents(char *path)
{
    for (char *p = strchr(path + 1, '/'); p != NULL; p = strchr(p + 1, '/'))
    {
        *p = 0;
        int r = mkdir(path, 0755);
        *p = '/';
        if ( (r != 0) && (errno != EEXIST) ) { return 0; }
    }
    return 1;
}

/** @brief Write a whole file (creating its parent directories). @return 1 on success. */
static int write_file(char *path, const void *data, size_t size)
{
    int fd = -1;
    if ( !make_parents(path) || ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0) ) { return 0; }
    const unsigned char *p = (const unsigned char *) data;
    size_t left = size;
    while (left > 0)
    {
        ssize_t w = write(fd, p, left);
        if (w < 0) { if (errno == EINTR) { continue; } break; }
        p += w;
        left -= (size_t) w;
    }
    return (close(fd) == 0) && (left == 0);
}

/** @brief `pzpdir unpack`. @return exit code. */
static int cmd_unpack(const char *const *paths, int np, const char *dir)
{
    pzpd *a = open_or_die(paths, np);
    if (a == NULL) { return 1; }
    if ( (mkdir(dir, 0755) != 0) && (errno != EEXIST) ) { fprintf(stderr, CLI_RED "cannot create %s: %s" CLI_NORMAL "\n", dir, strerror(errno)); pzpd_close(a); return 1; }
    uint64_t n = pzpd_count(a), files = 0, refused = 0, failed = 0;
    unsigned S = pzpd_stream_count(a);
    int multi = (pzpd_member_count(a) > 1);
    size_t dl = strlen(dir);
    char *full = (char *) malloc(dl + 2 + 4096 + PZPD_MAX_NAME + 1);   // dir / alias (≤ 4000) / name
    for (uint64_t i = 0; (full != NULL) && (i < n); i++)
    {
        for (unsigned s = 0; s < S; s++)
        {
            pzpd_blob_info bi;
            if (!pzpd_blob_info_get(a, i, s, &bi)) { fprintf(stderr, CLI_RED "record %llu: %s" CLI_NORMAL "\n", (unsigned long long) i, pzpd_last_error()); failed++; continue; }
            if (!bi.present) { continue; }
            if (!safe_name(bi.name, bi.name_len))
            {
                fprintf(stderr, CLI_RED "refusing unsafe name \"");
                put_escaped(stderr, bi.name, bi.name_len, 0);
                fprintf(stderr, "\" (record %llu)" CLI_NORMAL "\n", (unsigned long long) i);
                refused++;
                continue;
            }
            size_t o = dl;
            memcpy(full, dir, dl);
            full[o++] = '/';
            if (multi)
            {
                // Each member goes into <dir>/<alias>/ (the alias must itself be a safe single component)
                const char *alias = pzpd_member_alias(a, bi.member);
                size_t al = strlen(alias);
                if ( !safe_name(alias, al) || (memchr(alias, '/', al) != NULL) || (al > 4000) )
                    { fprintf(stderr, CLI_RED "refusing unsafe member alias \"%s\"" CLI_NORMAL "\n", alias); refused++; continue; }
                memcpy(full + o, alias, al);
                o += al;
                full[o++] = '/';
            }
            memcpy(full + o, bi.name, bi.name_len);
            full[o + bi.name_len] = 0;
            size_t size = 0;
            const void *data = pzpd_view(a, i, s, &size);
            if ( (data == NULL) || !write_file(full, data, size) )
            {
                fprintf(stderr, CLI_RED "cannot write %s: %s" CLI_NORMAL "\n", full, (data == NULL) ? pzpd_last_error() : strerror(errno));
                failed++;
                continue;
            }
            files++;
        }
        if ( ((i + 1) % 1000 == 0) || (i + 1 == n) ) { fprintf(stderr, "\runpacked %llu / %llu records", (unsigned long long)(i + 1), (unsigned long long) n); }
    }
    fprintf(stderr, "\n");
    free(full);
    pzpd_close(a);
    fprintf(stderr, "%llu files written, %llu unsafe names refused, %llu failures\n", (unsigned long long) files, (unsigned long long) refused, (unsigned long long) failed);
    return ( (refused == 0) && (failed == 0) ) ? 0 : 1;
}

/** @brief State of `pzpdir salvage` across records. */
struct salvage_ctx
{
    const char *dir;       ///< Output directory
    FILE       *list;      ///< Record list being written
    char       *full;      ///< Path buffer
    uint64_t    files;     ///< Blobs written
    uint64_t    refused;   ///< Unsafe names written under a sanitised path
    uint64_t    failed;    ///< Write failures
    uint64_t    damaged;   ///< Blobs failing their checksum (not written)
    uint64_t    empty;     ///< Records with no usable blob (left out of the list)
    uint64_t    rows;      ///< Row lines written
    uint64_t    rawrows;   ///< Row copies without a schema (not in the list)
    uint32_t    group;     ///< Group of the previous record
    pzpd       *sch;       ///< Schema / name source (may be NULL)
};

/** @brief pzpd_salvage() callback: write the record's intact blobs and its record-list lines. */
static int salvage_record(const pzpd_salvaged_record *r, void *user)
{
    struct salvage_ctx *c = (struct salvage_ctx *) user;
    size_t dl = strlen(c->dir);
    int wrote = 0;
    // Group directives: a new group, or leaving one (names from the schemas archive when it has this key)
    if (r->group != c->group)
    {
        if (r->group == PZPD_NO_GROUP) { fprintf(c->list, "@group\n"); }
        else
        {
            pzpd_group g;
            int64_t o = (c->sch != NULL) ? pzpd_find(c->sch, r->key, r->key_len, NULL) : -1;
            if ( (o >= 0) && pzpd_group_info(c->sch, (uint64_t) o, &g) && (g.name_len > 0) ) { fprintf(c->list, "@group "); put_escaped(c->list, g.name, g.name_len, 0); fputc('\n', c->list); }
            else { fprintf(c->list, "@group group%u\n", r->group); }
        }
        c->group = r->group;
    }
    for (unsigned i = 0; i < r->blob_count; i++)
    {
        const pzpd_salvaged_blob *b = &r->blobs[i];
        if (!b->intact)
        {
            fprintf(stderr, CLI_RED "damaged blob \"");
            put_escaped(stderr, b->name, b->name_len, 0);
            fprintf(stderr, "\" (checksum mismatch), not written" CLI_NORMAL "\n");
            c->damaged++;
            continue;
        }
        memcpy(c->full, c->dir, dl);
        c->full[dl] = '/';
        size_t o = dl + 1;
        if (safe_name(b->name, b->name_len)) { memcpy(c->full + o, b->name, b->name_len); o += b->name_len; }
        else
        {
            // Absolute paths, empty / . / .. components: written under a sanitised path inside dir; the
            // record list keeps the original name, so a re-pack restores it exactly
            size_t i = 0;
            while ( (i < b->name_len) && (b->name[i] == '/') ) { i++; }
            while (i <= b->name_len)
            {
                size_t j = i;
                while ( (j < b->name_len) && (b->name[j] != '/') ) { j++; }
                size_t cl = j - i;
                if (cl == 0) { c->full[o++] = '_'; }
                else if ( (cl <= 2) && (b->name[i] == '.') && ((cl == 1) || (b->name[i + 1] == '.')) ) { c->full[o++] = '_'; c->full[o++] = '_'; }
                else { memcpy(c->full + o, b->name + i, cl); o += cl; }
                if (j < b->name_len) { c->full[o++] = '/'; }
                i = j + 1;
            }
            c->refused++;
        }
        c->full[o] = 0;
        if (!write_file(c->full, b->data, b->size)) { fprintf(stderr, CLI_RED "cannot write %s: %s" CLI_NORMAL "\n", c->full, strerror(errno)); c->failed++; continue; }
        c->files++;
        wrote = 1;
        put_escaped(c->list, r->key, r->key_len, 1);
        if (b->stream_name != NULL) { fprintf(c->list, "\t%s\t", b->stream_name); } else { fprintf(c->list, "\tstream%u\t", b->stream); }
        put_escaped(c->list, c->full, strlen(c->full), 0);
        fputc('\t', c->list);
        put_escaped(c->list, b->name, b->name_len, 0);
        fputc('\n', c->list);
    }
    if (!wrote) { c->empty += 1; return 1; }                     // pack needs a blob per record: rows alone can't be listed
    for (unsigned t = 0; t < r->table_count; t++)
    {
        const pzpd_salvaged_rows *rw = &r->tables[t];
        if (rw->csv == NULL) { c->rawrows++; continue; }
        char *pb = NULL;
        size_t pl = 0;
        FILE *pf = open_memstream(&pb, &pl);
        if (pf == NULL) { continue; }
        put_escaped(pf, r->key, r->key_len, 1);
        fprintf(pf, "\t%s\t", rw->table_name);
        fclose(pf);
        put_rows(c->list, pb, rw->csv, rw->csv_len);
        free(pb);
        c->rows += rw->rows;
    }
    return 1;
}

/** @brief `pzpdir salvage`: recover a shard's records into a directory plus a record list that `pack` takes. @return exit code. */
static int cmd_salvage(const char *shardPath, const char *dir, const char *schemaPath, const char *listPath)
{
    // Schemas and stream names: the given archive, else the shard itself if its index is still usable
    pzpd *sch = (schemaPath != NULL) ? pzpd_open(schemaPath, 0) : pzpd_open(shardPath, 0);
    if ( (schemaPath != NULL) && (sch == NULL) ) { fprintf(stderr, CLI_RED "%s: %s" CLI_NORMAL "\n", schemaPath, pzpd_last_error()); return 1; }
    if ( (mkdir(dir, 0755) != 0) && (errno != EEXIST) ) { fprintf(stderr, CLI_RED "cannot create %s: %s" CLI_NORMAL "\n", dir, strerror(errno)); pzpd_close(sch); return 1; }
    char defList[4200];
    if (listPath == NULL) { snprintf(defList, sizeof(defList), "%s.salvage.tsv", dir); listPath = defList; }
    struct salvage_ctx c;
    memset(&c, 0, sizeof(c));
    c.dir  = dir;
    c.group = PZPD_NO_GROUP;
    c.sch  = sch;
    c.list = fopen(listPath, "w");
    c.full = (char *) malloc(strlen(dir) + 2 + 2 * PZPD_MAX_NAME + 1);   // sanitising can at most double a name
    if ( (c.list == NULL) || (c.full == NULL) ) { fprintf(stderr, CLI_RED "cannot write %s" CLI_NORMAL "\n", listPath); pzpd_close(sch); free(c.full); if (c.list) { fclose(c.list); } return 1; }

    fprintf(c.list, "# pzpdir salvage of %s\n", shardPath);
    for (unsigned t = 0; (sch != NULL) && (t < pzpd_table_count(sch)); t++)
    {
        const pzpd_schema *sc = pzpd_table_schema(sch, t);
        fprintf(c.list, "@%s %s ", (sc->flags & PZPD_TABLE_GLOBAL) ? "global" : "table", sc->name);
        put_schema(c.list, sc);
        fprintf(c.list, "%s\n", (sc->flags & PZPD_TABLE_BULK) ? " bulk" : "");
        if (sc->flags & PZPD_TABLE_BULK) { fprintf(stderr, "note: table %s is bulk: its rows are not in the record headers and can't be salvaged\n", sc->name); }
    }
    for (unsigned t = 0; (sch != NULL) && (t < pzpd_table_count(sch)); t++)
    {
        const pzpd_schema *sc = pzpd_table_schema(sch, t);
        if ( !(sc->flags & PZPD_TABLE_GLOBAL) ) { continue; }
        ssize_t n = pzpd_global_csv(sch, 0, t, NULL, 0);
        char *buf = (n > 0) ? (char *) malloc((size_t) n + 1) : NULL;
        if ( (buf != NULL) && (pzpd_global_csv(sch, 0, t, buf, (size_t) n + 1) == n) )
        {
            char prefix[64];
            snprintf(prefix, sizeof(prefix), "@row %s ", sc->name);
            put_rows(c.list, prefix, buf, (size_t) n);
        }
        free(buf);
    }

    pzpd_salvage_info info;
    int ok = pzpd_salvage(shardPath, sch, salvage_record, &c, &info);
    if (!ok) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); }
    int closed = (fclose(c.list) == 0);
    free(c.full);
    pzpd_close(sch);
    if (!ok) { return 1; }
    static const char *from[] = { "unknown (placeholders streamN)", "the superblock", "the metadata section", "the schemas archive" };
    fprintf(stderr, "salvaged %llu records: %llu blobs written, %llu damaged, %llu unsafe names sanitised, %llu write failures, %llu records without a usable blob; %llu table rows%s\n",
            (unsigned long long) info.records, (unsigned long long) c.files, (unsigned long long) c.damaged, (unsigned long long) c.refused,
            (unsigned long long) c.failed, (unsigned long long) c.empty, (unsigned long long) c.rows,
            c.rawrows ? " (some row copies had no schema: pass --schemas <archive>)" : "");
    fprintf(stderr, "%llu damaged record headers skipped; stream names from %s\n", (unsigned long long) info.damaged_headers, from[(info.names_from >= 0) && (info.names_from <= 3) ? info.names_from : 0]);
    if (info.stream_count > 0)
    {
        fprintf(stderr, "re-pack with: pzpdir pack <new.pzpd> %s --streams ", listPath);
        for (unsigned u = 0; u < info.stream_count; u++) { fprintf(stderr, "%s%s", u ? "," : "", info.streams[u]); }
        fprintf(stderr, "\n");
    }
    return ( closed && (c.failed == 0) && (c.damaged == 0) && (c.empty == 0) ) ? 0 : 1;
}

/** @brief `pzpdir rebuild-manifest`: regenerate a manifest from its shards. @return exit code. */
static int cmd_rebuild_manifest(const char *const *shards, int n, const char *out)
{
    char def[4200];
    if (out == NULL)
    {
        // <name>.NNNNN.pzpd -> <name>.pzpd
        size_t l = strlen(shards[0]);
        if ( (l < 12) || (strcmp(shards[0] + l - 5, ".pzpd") != 0) || (shards[0][l - 11] != '.') || (l + 1 > sizeof(def)) )
            { fprintf(stderr, CLI_RED "can't derive the manifest name from %s; use --out" CLI_NORMAL "\n", shards[0]); return 1; }
        for (size_t i = l - 10; i < l - 5; i++) { if ( (shards[0][i] < '0') || (shards[0][i] > '9') ) { fprintf(stderr, CLI_RED "can't derive the manifest name from %s; use --out" CLI_NORMAL "\n", shards[0]); return 1; } }
        memcpy(def, shards[0], l - 11);
        memcpy(def + l - 11, ".pzpd", 6);
        out = def;
    }
    if (!pzpd_manifest_rebuild(out, shards, (unsigned) n)) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); return 1; }
    fprintf(stderr, CLI_GREEN "wrote %s from %d shard(s)" CLI_NORMAL "\n", out, n);
    return 0;
}

/** @brief `pzpdir add-table | replace-table | drop-table <archive> TABLE [LIST|-]`. @return exit code. */
static int cmd_edit_table(unsigned op, const char *archive, const char *table, const char *listPath, int keep)
{
    char *text = NULL, *schema = NULL, *gcsv = NULL;
    size_t len = 0, gl = 0, n = 0, cap = 0;
    unsigned tflags = 0;
    pzpd_edit_rows *rows = NULL;
    int rc = 1;
    if (op != PZPD_EDIT_DROP)
    {
        if ( (listPath == NULL) || ((text = read_all(listPath, &len)) == NULL) ) { if (listPath == NULL) { fprintf(stderr, CLI_RED "a record list is needed" CLI_NORMAL "\n"); } return 1; }
        size_t lineNo = 0;
        char *p = text, *end = text + len;
        while (p < end)
        {
            char *nl = memchr(p, '\n', (size_t)(end - p));
            char *le = (nl != NULL) ? nl : end;
            *le = 0;
            lineNo++;
            char *line = p;
            size_t ll = (size_t)(le - p);
            p = le + 1;
            if ( (ll == 0) || (line[0] == '#') ) { continue; }
            int isT = !strncmp(line, "@table ", 7), isG = !strncmp(line, "@global ", 8);
            if (isT || isG)
            {
                char *nm = line + (isT ? 7 : 8), *sp = strchr(nm, ' ');
                if (sp == NULL) { fprintf(stderr, CLI_RED "%s:%zu: expected @table NAME schema" CLI_NORMAL "\n", listPath, lineNo); goto out; }
                *sp = 0;
                if (strcmp(nm, table) != 0) { fprintf(stderr, CLI_RED "%s:%zu: declares table %s, the edit is for %s" CLI_NORMAL "\n", listPath, lineNo, nm, table); goto out; }
                schema = sp + 1;
                size_t sl = strlen(schema);
                while ( (sl > 0) && (schema[sl - 1] == ' ') ) { schema[--sl] = 0; }
                tflags = isG ? PZPD_TABLE_GLOBAL : 0;
                if ( (sl >= 5) && !strcmp(schema + sl - 4, "bulk") && (schema[sl - 5] == ' ') ) { tflags |= PZPD_TABLE_BULK; schema[sl - 5] = 0; }
                continue;
            }
            if (!strncmp(line, "@row ", 5))
            {
                char *nm = line + 5, *sp = strchr(nm, ' ');
                if ( (sp == NULL) || (*sp = 0, strcmp(nm, table) != 0) ) { fprintf(stderr, CLI_RED "%s:%zu: @row for another table" CLI_NORMAL "\n", listPath, lineNo); goto out; }
                char *csv = sp + 1;
                size_t u = unescape(csv, strlen(csv));
                if (u == (size_t) -1) { fprintf(stderr, CLI_RED "%s:%zu: bad escape" CLI_NORMAL "\n", listPath, lineNo); goto out; }
                char *ng = (char *) realloc(gcsv, gl + u + 1);
                if (ng == NULL) { goto out; }
                gcsv = ng;
                memcpy(gcsv + gl, csv, u);
                gl += u;
                gcsv[gl++] = '\n';
                continue;
            }
            if (line[0] == '@') { fprintf(stderr, CLI_RED "%s:%zu: unexpected directive" CLI_NORMAL "\n", listPath, lineNo); goto out; }
            char *f[4];
            size_t fl[4];
            int nf = split_fields(line, ll, f, fl, 4);
            if ( (nf != 3) || strcmp(f[1], table) ) { fprintf(stderr, CLI_RED "%s:%zu: expected key<TAB>%s<TAB>csv" CLI_NORMAL "\n", listPath, lineNo, table); goto out; }
            if (n == cap)
            {
                cap = cap ? cap * 2 : 1024;
                pzpd_edit_rows *nr = (pzpd_edit_rows *) realloc(rows, cap * sizeof(pzpd_edit_rows));
                if (nr == NULL) { goto out; }
                rows = nr;
            }
            rows[n].key = f[0]; rows[n].key_len = fl[0]; rows[n].csv = f[2]; rows[n].csv_len = fl[2];
            n++;
        }
    }
    uint64_t um = 0;
    if (!pzpd_edit_table(archive, op, table, schema, tflags, rows, n, gcsv, gl, keep ? PZPD_EDIT_KEEP_MISSING : 0, &um))
        { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); goto out; }
    if (um > 0) { fprintf(stderr, "%llu row entries match no record key: not added\n", (unsigned long long) um); }
    fprintf(stderr, CLI_GREEN "%s table %s in %s" CLI_NORMAL "\n", (op == PZPD_EDIT_ADD) ? "added" : (op == PZPD_EDIT_REPLACE) ? "replaced" : "dropped", table, archive);
    rc = 0;
out:
    free(rows);
    free(gcsv);
    free(text);
    return rc;
}

/** @brief `pzpdir add-stream | replace-stream | drop-stream <archive> STREAM [LIST|-]`. @return exit code. */
static int cmd_edit_stream(unsigned op, const char *archive, const char *stream, const char *listPath, int drop)
{
    char *text = NULL;
    size_t len = 0, n = 0, cap = 0;
    pzpd_edit_blob *blobs = NULL;
    int rc = 1;
    if (op != PZPD_EDIT_DROP)
    {
        if ( (listPath == NULL) || ((text = read_all(listPath, &len)) == NULL) ) { if (listPath == NULL) { fprintf(stderr, CLI_RED "a record list is needed" CLI_NORMAL "\n"); } return 1; }
        size_t lineNo = 0;
        char *p = text, *end = text + len;
        while (p < end)
        {
            char *nl = memchr(p, '\n', (size_t)(end - p));
            char *le = (nl != NULL) ? nl : end;
            *le = 0;
            lineNo++;
            char *line = p;
            size_t ll = (size_t)(le - p);
            p = le + 1;
            if ( (ll == 0) || (line[0] == '#') ) { continue; }
            char *f[5];
            size_t fl[5];
            int nf = split_fields(line, ll, f, fl, 5);
            if ( (nf < 3) || (nf > 4) || strcmp(f[1], stream) || (fl[0] == 0) || (fl[2] == 0) )
                { fprintf(stderr, CLI_RED "%s:%zu: expected key<TAB>%s<TAB>path[<TAB>name]" CLI_NORMAL "\n", listPath, lineNo, stream); goto out; }
            if (n == cap)
            {
                cap = cap ? cap * 2 : 1024;
                pzpd_edit_blob *nb = (pzpd_edit_blob *) realloc(blobs, cap * sizeof(pzpd_edit_blob));
                if (nb == NULL) { goto out; }
                blobs = nb;
            }
            blobs[n].key = f[0]; blobs[n].key_len = fl[0]; blobs[n].path = f[2];
            blobs[n].name = (nf == 4) ? f[3] : f[2]; blobs[n].name_len = (nf == 4) ? fl[3] : fl[2];
            n++;
        }
    }
    uint64_t um = 0;
    if (!pzpd_edit_stream(archive, op, stream, blobs, n, drop ? PZPD_EDIT_DROP_MISSING : 0, &um))
        { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); goto out; }
    if (um > 0) { fprintf(stderr, "%llu files match no record key: not added\n", (unsigned long long) um); }
    fprintf(stderr, CLI_GREEN "%s stream %s in %s" CLI_NORMAL "\n", (op == PZPD_EDIT_ADD) ? "added" : (op == PZPD_EDIT_REPLACE) ? "replaced" : "dropped", stream, archive);
    rc = 0;
out:
    free(blobs);
    free(text);
    return rc;
}

/** @brief `pzpdir groups`: list the video groups (name, first ordinal, frames, shard). @return exit code. */
static int cmd_groups(const char *const *paths, int np)
{
    pzpd *a = open_or_die(paths, np);
    if (a == NULL) { return 1; }
    uint64_t n = pzpd_count(a), groups = 0;
    for (uint64_t i = 0; i < n; )
    {
        pzpd_group g;
        if (!pzpd_group_info(a, i, &g))
        {
            if (pzpd_last_error_code() != PZPD_OK) { fprintf(stderr, CLI_RED "record %llu: %s" CLI_NORMAL "\n", (unsigned long long) i, pzpd_last_error()); pzpd_close(a); return 1; }
            i++;
            continue;
        }
        pzpd_blob_info bi;
        pzpd_blob_info_get(a, i, 0, &bi);
        if (g.name_len > 0) { put_escaped(stdout, g.name, g.name_len, 1); } else { printf("(group %u)", g.id); }
        printf("\t%llu\t%u\tshard %u\n", (unsigned long long) g.first_ordinal, g.frames, bi.shard);
        groups++;
        i = g.first_ordinal + g.frames;
    }
    fprintf(stderr, "%llu groups\n", (unsigned long long) groups);
    pzpd_close(a);
    return 0;
}

/** @brief `pzpdir collect`: write or refresh a collection file. @return exit code. */
static int cmd_collect(const char *out, const char *const *args, int n, int absolute, int refresh)
{
    if (refresh)
    {
        if (!pzpd_collection_refresh(out)) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); return 1; }
        fprintf(stderr, CLI_GREEN "refreshed %s" CLI_NORMAL "\n", out);
        return 0;
    }
    if (n < 1) { usage(); return 1; }
    const char **paths   = (const char **) calloc((size_t) n, sizeof(char *));
    char       **aliases = (char **) calloc((size_t) n, sizeof(char *));
    int anyAlias = 0;
    for (int i = 0; i < n; i++)
    {
        // "alias=path": the part before '=' must not contain '/', so paths with '=' in a directory still work
        const char *eq = strchr(args[i], '=');
        const char *sl = strchr(args[i], '/');
        if ( (eq != NULL) && (eq != args[i]) && ((sl == NULL) || (sl > eq)) )
        {
            aliases[i] = strndup(args[i], (size_t)(eq - args[i]));
            paths[i] = eq + 1;
            anyAlias = 1;
        }
        else { paths[i] = args[i]; }
    }
    // Members without an explicit alias get the default one (file name without ".pzpd")
    for (int i = 0; anyAlias && (i < n); i++)
    {
        if (aliases[i] != NULL) { continue; }
        const char *b = strrchr(paths[i], '/');
        b = (b == NULL) ? paths[i] : b + 1;
        size_t bl = strlen(b);
        if ( (bl > 5) && !strcmp(b + bl - 5, ".pzpd") ) { bl -= 5; }
        aliases[i] = strndup(b, bl);
    }
    int ok = pzpd_collection_write(out, paths, anyAlias ? (const char *const *) aliases : NULL, (unsigned) n, absolute ? PZPD_COLL_ABSOLUTE : 0);
    if (!ok) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); }
    else     { fprintf(stderr, CLI_GREEN "wrote %s (%d members)" CLI_NORMAL "\n", out, n); }
    for (int i = 0; i < n; i++) { free(aliases[i]); }
    free(aliases);
    free(paths);
    return ok ? 0 : 1;
}

//-----------------------------------------------------------------------------------------------
// Word index (spec §3.7)
//-----------------------------------------------------------------------------------------------

/** @brief Resolve TABLE.COLUMN (or, when spec is NULL, the handle's only word index) into buffers.
 *  @return 1 on success, 0 on failure (message printed). */
static int words_spec(pzpd *a, const char *spec, char table[64], char column[64], char source[64])
{
    const char *t = NULL, *c = NULL, *sc = "";
    source[0] = 0;
    if (spec == NULL)
    {
        if (!pzpd_words_index(a, 0, &t, &c, &sc)) { fprintf(stderr, CLI_RED "the archive has no word index" CLI_NORMAL "\n"); return 0; }
        const char *t2, *c2, *s2;
        if (pzpd_words_index(a, 1, &t2, &c2, &s2)) { fprintf(stderr, CLI_RED "the archive has several word indexes: name one as TABLE.COLUMN" CLI_NORMAL "\n"); return 0; }
        snprintf(table, 64, "%s", t); snprintf(column, 64, "%s", c); snprintf(source, 64, "%s", sc);
        return 1;
    }
    const char *dot = strchr(spec, '.');
    if ( (dot == NULL) || (dot == spec) || (dot[1] == 0) || ((size_t)(dot - spec) >= 64) || (strlen(dot + 1) >= 64) ) { fprintf(stderr, CLI_RED "expected TABLE.COLUMN, got \"%s\"" CLI_NORMAL "\n", spec); return 0; }
    snprintf(table, 64, "%.*s", (int)(dot - spec), spec);
    snprintf(column, 64, "%s", dot + 1);
    for (unsigned k = 0; pzpd_words_index(a, k, &t, &c, &sc); k++) { if (!strcmp(t, table) && !strcmp(c, column)) { snprintf(source, 64, "%s", sc); } }
    return 1;
}

/** @brief Sort context for cmd_words: records per word, descending, then word id. */
static const uint64_t *g_wrec;

static int cmp_word_rank(const void *x, const void *y)
{
    uint32_t a = *(const uint32_t *) x, b = *(const uint32_t *) y;
    if (g_wrec[a] != g_wrec[b]) { return (g_wrec[a] > g_wrec[b]) ? -1 : 1; }
    return (a < b) ? -1 : (a > b);
}

/** @brief `words`: a word index's vocabulary by records (--top N), the keys of the records containing a word
 *  (--word W), or its source values (--sources). @return 0 on success, 1 on failure. */
static int cmd_words(const char *const *paths, int np, const char *spec, const char *source, int sources, int canonical, long top, const char *word)
{
    pzpd *a = open_or_die(paths, np);
    if (a == NULL) { return 1; }
    char table[64], column[64], srccol[64];
    if (!words_spec(a, spec, table, column, srccol)) { pzpd_close(a); return 1; }
    int rc = 0;
    if (sources)
    {
        const char *nm[PZPD_MAX_WORD_SOURCES]; size_t ln[PZPD_MAX_WORD_SOURCES];
        size_t n = pzpd_words_sources(a, table, column, nm, ln, PZPD_MAX_WORD_SOURCES);
        if ( (n == 0) && (pzpd_last_error_code() != PZPD_OK) ) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); rc = 1; }
        for (size_t i = 0; i < n; i++) { printf("%.*s\n", (int) ln[i], nm[i]); }
        pzpd_close(a);
        return rc;
    }
    pzpd_words *w = NULL;
    if (!pzpd_words_open(a, table, column, source, (source != NULL) ? strlen(source) : 0, canonical ? PZPD_WORDS_CANONICAL : 0, &w))
    {
        fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error());
        pzpd_close(a);
        return 1;
    }
    if (word != NULL)
    {
        int64_t id = pzpd_words_find(w, word, strlen(word));
        if (id < 0) { fprintf(stderr, CLI_RED "\"%s\" is not in the vocabulary" CLI_NORMAL "\n", word); rc = 1; }
        else
        {
            size_t n = pzpd_words_records(w, (uint32_t) id, NULL, 0);
            uint64_t *ord = (uint64_t *) malloc((n ? n : 1) * sizeof(uint64_t));
            if ( (ord == NULL) || ((n > 0) && (pzpd_words_records(w, (uint32_t) id, ord, n) != n)) || (pzpd_last_error_code() != PZPD_OK) )
                { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", (ord == NULL) ? "out of memory" : pzpd_last_error()); rc = 1; n = 0; }
            for (size_t i = 0; i < n; i++)
            {
                size_t kl = 0;
                const char *k = pzpd_record_key(a, ord[i], &kl);
                if (k == NULL) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); rc = 1; break; }
                put_escaped(stdout, k, kl, 1);
                fputc('\n', stdout);
            }
            free(ord);
        }
    }
    else
    {
        const uint64_t *rec = NULL, *cnt = NULL;
        uint32_t n = pzpd_words_arrays(w, &rec, &cnt, NULL, NULL);
        uint32_t *ids = (uint32_t *) malloc(((size_t) n + 1) * sizeof(uint32_t));
        if (ids == NULL) { fprintf(stderr, CLI_RED "out of memory" CLI_NORMAL "\n"); rc = 1; n = 0; }
        for (uint32_t i = 0; i < n; i++) { ids[i] = i; }
        g_wrec = rec;
        qsort(ids, n, sizeof(uint32_t), cmp_word_rank);
        uint32_t shown = ((top > 0) && ((uint64_t) top < n)) ? (uint32_t) top : n;
        for (uint32_t i = 0; i < shown; i++)
        {
            size_t l;
            const char *s = pzpd_words_word(w, ids[i], &l);
            printf("%.*s\t%llu\t%llu\n", (int) l, s, (unsigned long long) rec[ids[i]], (unsigned long long) cnt[ids[i]]);
        }
        free(ids);
    }
    pzpd_words_close(w);
    pzpd_close(a);
    return rc;
}

/** @brief `reindex`: rebuild every word index (no spec), add / rebuild one, or drop one. @return 0 on success, 1 on failure. */
static int cmd_reindex(const char *archive, const char *spec, const char *source_column, int drop)
{
    if (spec == NULL)
    {
        if (drop) { fprintf(stderr, CLI_RED "--drop needs TABLE.COLUMN" CLI_NORMAL "\n"); return 1; }
        pzpd *a = pzpd_open(archive, 0);
        if (a == NULL) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); return 1; }
        char tabs[PZPD_MAX_WORD_INDEXES][3][64];
        unsigned n = 0;
        const char *t, *c, *sc;
        for (unsigned k = 0; (n < PZPD_MAX_WORD_INDEXES) && pzpd_words_index(a, k, &t, &c, &sc); k++)
        {
            snprintf(tabs[n][0], 64, "%s", t); snprintf(tabs[n][1], 64, "%s", c); snprintf(tabs[n][2], 64, "%s", sc); n++;
        }
        pzpd_close(a);
        if (n == 0) { fprintf(stderr, CLI_RED "%s has no word index to rebuild (name one: reindex <archive> TABLE.COLUMN [SOURCE_COLUMN])" CLI_NORMAL "\n", archive); return 1; }
        for (unsigned k = 0; k < n; k++)
        {
            if (!pzpd_edit_words(archive, PZPD_EDIT_REPLACE, tabs[k][0], tabs[k][1], tabs[k][2][0] ? tabs[k][2] : NULL)) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); return 1; }
            fprintf(stderr, CLI_GREEN "rebuilt word index %s.%s in %s" CLI_NORMAL "\n", tabs[k][0], tabs[k][1], archive);
        }
        return 0;
    }
    const char *dot = strchr(spec, '.');
    if ( (dot == NULL) || (dot == spec) || (dot[1] == 0) || ((size_t)(dot - spec) >= 64) ) { fprintf(stderr, CLI_RED "expected TABLE.COLUMN, got \"%s\"" CLI_NORMAL "\n", spec); return 1; }
    char table[64];
    snprintf(table, sizeof(table), "%.*s", (int)(dot - spec), spec);
    if (!pzpd_edit_words(archive, drop ? PZPD_EDIT_DROP : PZPD_EDIT_REPLACE, table, dot + 1, source_column)) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); return 1; }
    fprintf(stderr, CLI_GREEN "%s word index %s in %s" CLI_NORMAL "\n", drop ? "dropped" : "built", spec, archive);
    return 0;
}

//-----------------------------------------------------------------------------------------------

/** @brief Entry point: dispatch the subcommand.
 *  @param argc Argument count.
 *  @param argv `pzpdir <command> <arguments...> [options]`.
 *  @return 0 on success, 1 on any failure. */
int main(int argc, char **argv)
{
    if (argc < 3) { usage(); return 1; }
    const char *cmd = argv[1];

    // Options shared by the subcommands
    const char *streams = NULL, *stream = NULL, *table = NULL, *schemas = NULL, *listOut = NULL, *out = NULL, *missing = NULL;
    uint64_t shard = 0;
    uint32_t align = 0;
    int lng = 0, names = 0, blobs = 0, absolute = 0, refresh = 0, dups = 0, sources = 0, canonical = 0, drop = 0;
    const char *source = NULL, *word = NULL;
    long top = 0;
    const char *pos[256] = {0};
    int npos = 0;
    for (int i = 2; i < argc; i++)
    {
        if      ( (strcmp(argv[i], "--streams") == 0) && (i + 1 < argc) )    { streams = argv[++i]; }
        else if ( (strcmp(argv[i], "--stream") == 0) && (i + 1 < argc) )     { stream = argv[++i]; }
        else if ( (strcmp(argv[i], "--table") == 0) && (i + 1 < argc) )      { table = argv[++i]; }
        else if ( (strcmp(argv[i], "--shard-size") == 0) && (i + 1 < argc) ) { shard = parse_size(argv[++i]); if (shard == 0) { fprintf(stderr, "bad --shard-size\n"); return 1; } }
        else if ( (strcmp(argv[i], "--align") == 0) && (i + 1 < argc) )      { align = (uint32_t) atoi(argv[++i]); }
        else if ( (strcmp(argv[i], "--schemas") == 0) && (i + 1 < argc) )    { schemas = argv[++i]; }
        else if ( (strcmp(argv[i], "--list") == 0) && (i + 1 < argc) )       { listOut = argv[++i]; }
        else if ( (strcmp(argv[i], "--out") == 0) && (i + 1 < argc) )        { out = argv[++i]; }
        else if ( (strcmp(argv[i], "--missing") == 0) && (i + 1 < argc) )    { missing = argv[++i]; }
        else if (strcmp(argv[i], "--long") == 0)  { lng = 1; }
        else if (strcmp(argv[i], "--names") == 0) { names = 1; }
        else if (strcmp(argv[i], "--blobs") == 0) { blobs = 1; }
        else if (strcmp(argv[i], "--absolute") == 0) { absolute = 1; }
        else if (strcmp(argv[i], "--refresh") == 0)  { refresh = 1; }
        else if (strcmp(argv[i], "--dups") == 0)     { dups = 1; }
        else if ( (strcmp(argv[i], "--source") == 0) && (i + 1 < argc) ) { source = argv[++i]; }
        else if ( (strcmp(argv[i], "--word") == 0) && (i + 1 < argc) )   { word = argv[++i]; }
        else if ( (strcmp(argv[i], "--top") == 0) && (i + 1 < argc) )    { top = atol(argv[++i]); }
        else if (strcmp(argv[i], "--sources") == 0)   { sources = 1; }
        else if (strcmp(argv[i], "--canonical") == 0) { canonical = 1; }
        else if (strcmp(argv[i], "--drop") == 0)      { drop = 1; }
        else if ( (argv[i][0] == '-') && (argv[i][1] == '-') ) { fprintf(stderr, "unknown option %s\n", argv[i]); usage(); return 1; }
        else if (npos < 256) { pos[npos++] = argv[i]; }
        else { fprintf(stderr, "too many arguments\n"); return 1; }
    }

    if (strcmp(cmd, "pack") == 0)
    {
        if (npos != 2) { usage(); return 1; }
        struct stat st;
        if ( (strcmp(pos[1], "-") != 0) && (stat(pos[1], &st) == 0) && S_ISDIR(st.st_mode) ) { return pack_dir(pos[0], pos[1], shard, align); }
        struct record_list L;
        int rc = 1;
        if (parse_list(pos[1], streams, &L)) { rc = pack_list(pos[0], &L, shard, align, pos[1]); }
        free_list(&L);
        return rc;
    }
    if ( (strcmp(cmd, "collect") == 0) && (npos >= 1) ) { return cmd_collect(pos[0], pos + 1, npos - 1, absolute, refresh); }
    if ( (strcmp(cmd, "ls") == 0) && (npos >= 1) )      { return cmd_ls(pos, npos, lng, names); }
    if ( (strcmp(cmd, "cat") == 0) && (npos >= 2) )     { return cmd_cat(pos, npos - 1, pos[npos - 1], stream, table); }
    if ( (strcmp(cmd, "export-table") == 0) && (npos >= 2) ) { return cmd_export_table(pos, npos - 1, pos[npos - 1]); }
    if ( (strcmp(cmd, "info") == 0) && (npos >= 1) )    { return cmd_info(pos, npos, stream, dups); }
    if ( (strcmp(cmd, "groups") == 0) && (npos >= 1) )  { return cmd_groups(pos, npos); }
    if ( (strcmp(cmd, "verify") == 0) && (npos >= 1) )  { return cmd_verify(pos, npos, blobs); }
    if ( (strcmp(cmd, "unpack") == 0) && (npos >= 2) )  { return cmd_unpack(pos, npos - 1, pos[npos - 1]); }
    if ( (strcmp(cmd, "salvage") == 0) && (npos == 2) ) { return cmd_salvage(pos[0], pos[1], schemas, listOut); }
    if ( (strcmp(cmd, "rebuild-manifest") == 0) && (npos >= 1) ) { return cmd_rebuild_manifest(pos, npos, out); }
    if ( (strcmp(cmd, "words") == 0) && (npos >= 1) )
    {
        // A last argument that isn't a file is the TABLE.COLUMN of the index
        struct stat st;
        int hasSpec = (npos >= 2) && (stat(pos[npos - 1], &st) != 0) && (strchr(pos[npos - 1], '.') != NULL);
        return cmd_words(pos, hasSpec ? npos - 1 : npos, hasSpec ? pos[npos - 1] : NULL, source, sources, canonical, top, word);
    }
    if ( (strcmp(cmd, "reindex") == 0) && (npos >= 1) && (npos <= 3) ) { return cmd_reindex(pos[0], (npos >= 2) ? pos[1] : NULL, (npos == 3) ? pos[2] : NULL, drop); }
    unsigned op = !strncmp(cmd, "add-", 4) ? PZPD_EDIT_ADD : !strncmp(cmd, "replace-", 8) ? PZPD_EDIT_REPLACE : !strncmp(cmd, "drop-", 5) ? PZPD_EDIT_DROP : 0;
    const char *what = (op == PZPD_EDIT_ADD) ? cmd + 4 : (op == PZPD_EDIT_REPLACE) ? cmd + 8 : (op == PZPD_EDIT_DROP) ? cmd + 5 : "";
    if ( (op != 0) && (!strcmp(what, "table") || !strcmp(what, "stream")) && (npos == ((op == PZPD_EDIT_DROP) ? 2 : 3)) )
    {
        int isTable = !strcmp(what, "table");
        if ( (missing != NULL) && strcmp(missing, isTable ? "keep" : "drop") && strcmp(missing, isTable ? "empty" : "keep") )
            { fprintf(stderr, "--missing takes %s\n", isTable ? "empty or keep" : "keep or drop"); return 1; }
        int flag = (missing != NULL) && !strcmp(missing, isTable ? "keep" : "drop");
        return isTable ? cmd_edit_table(op, pos[0], pos[1], (npos == 3) ? pos[2] : NULL, flag) : cmd_edit_stream(op, pos[0], pos[1], (npos == 3) ? pos[2] : NULL, flag);
    }
    if ( (strcmp(cmd, "compact") == 0) && (npos == 1) )
    {
        uint64_t freed = 0;
        if (!pzpd_compact(pos[0], &freed)) { fprintf(stderr, CLI_RED "%s" CLI_NORMAL "\n", pzpd_last_error()); return 1; }
        fprintf(stderr, CLI_GREEN "compacted %s: %.1f KB reclaimed" CLI_NORMAL "\n", pos[0], (double) freed / 1e3);
        return 0;
    }
    usage();
    return 1;
}
