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

/** @file pzpdir.c
 *  @brief  Implementation of pzpdir.h (PZPD archives, spec doc/pzpd-spec.md v0.4).
 *
 *  The public functions are documented in the header; this file documents the internal
 *  helpers and the data they keep:
 *  - the on-disk structures (superblock, record header, index sections, manifest), all
 *    packed and little-endian, sizes checked with _Static_assert,
 *  - the thread-local error (pzpd_last_error()),
 *  - format detection (header-only probes, never a full decode),
 *  - the writer, which streams records into `.tmp` shards and keeps an archive-wide
 *    duplicate check of record keys and blob names,
 *  - the reader, which maps the manifest, opens shards lazily under a mutex, and
 *    bounds-checks every offset read from disk so damaged files fail cleanly.
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
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/vfs.h>
#include <time.h>
#include <sys/random.h>
#include <sys/types.h>

#include <zstd.h>
#include <lz4.h>

/** @brief Make every xxHash function static inline: no symbols are exported, so there is no
 *  clash with a system libxxhash or another vendored copy in the same program. */
#define XXH_INLINE_ALL
#include "third_party/xxhash.h"
#include "pzpdir_unicode.h"

#if PZPDIR_WITH_PZP
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wunused-function"
 #pragma GCC diagnostic ignored "-Wunused-variable"
 #include "pzp.h"
 #pragma GCC diagnostic pop
#endif

#ifndef PZPDIR_DEBUG
/** @brief Compile-time switch (0/1) for internal debug messages on stderr. */
#define PZPDIR_DEBUG 0
#endif

#define PZPD_NORMAL "\033[0m"   ///< ANSI escape: reset terminal color
#define PZPD_RED    "\033[31m"  ///< ANSI escape: red text
#define PZPD_GREEN  "\033[32m"  ///< ANSI escape: green text
#define PZPD_YELLOW "\033[33m"  ///< ANSI escape: yellow text

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "PZPD structures are little-endian and read in place");

// The implementation, in parts (one translation unit, so every internal function stays static and
// clients still compile only this file). The order matters: each part uses the ones before it.
#include "pzpdir_format.inc.c"
#include "pzpdir_detect.inc.c"
#include "pzpdir_tables.inc.c"
#include "pzpdir_words_build.inc.c"
#include "pzpdir_writer.inc.c"
#include "pzpdir_reader.inc.c"
#include "pzpdir_handle.inc.c"
#include "pzpdir_words_read.inc.c"
#include "pzpdir_collections.inc.c"
#include "pzpdir_prefetch.inc.c"
#include "pzpdir_recovery.inc.c"
#include "pzpdir_edit.inc.c"
#include "pzpdir_groups.inc.c"
