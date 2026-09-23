"""
pzp.pzpdir — Python bindings (ctypes) for PZPD archives: millions of files, grouped into records,
in a few self-contained shards. The format and the C API are in doc/pzpd-spec.md and
src/pzpdir/pzpdir.h; this module wraps libpzpdir.so.

Usage:
    import pzp.pzpdir as pzpdir

    with pzpdir.open("coco_val2017.pzpd") as a:            # manifest, single shard, or collection file
        len(a), a.streams                                   # 5000, ['rgb', 'all', 'geo', 'depth', 'seg']
        i = a.find("000000000139.jpg")                      # record key -> ordinal
        raw = a.read(i, "rgb")                              # bytes
        rec = a.read_record(i, ["rgb", "all", "geo"])       # dict of memoryviews, one pread
        a.info(i, "depth")                                  # format + metadata, no data I/O
        img = a.read_image(i, "rgb")                        # numpy (PZP natively, JPEG / PNG via PIL)
        a.table(i, "persons")                               # numpy structured array
        a.global_table("joints")
        idx, rows = a.table_all("persons")                  # CSR arrays over all records

    with pzpdir.open(["coco.pzpd", "imagenet.pzpd"]) as a:  # several archives as one
        a.members, a.member_of(12345)

    with a.prefetcher(streams=["rgb", "all", "geo"], mode="auto") as pf:
        pf.submit(order)
        with pf.get(order[0]) as rec:                        # dict-like of memoryviews
            decode(rec["rgb"])

    with pzpdir.Writer("x.pzpd", streams=["rgb", "depth"]) as w:
        persons = w.table("persons", "id:u16 bbox:u16[4]")
        with w.record("000000000009"):
            w.add_file("rgb", "val2017/000000000009.jpg", path)
            w.rows_csv(persons, "1,10,20,30,40")

    # word index (spec §3.7): words as strings, never token ids
    with a.words("descriptions.text", canonical=True) as w:  # merged sub-index; source="old" for one source
        w.records_per_word, w.find("dog"), w.records("dog"), w.of_record(i)

ctypes releases the GIL during every library call, so the library's I/O threads and several
Python threads reading the same archive run in parallel.

Library lookup: $PZPDIR_LIB, then libpzpdir.so next to this file, then src/pzpdir/ of this
repository, then the system library path.

Repository : https://github.com/AmmarkoV/PZP
Author     : Ammar Qammaz (AmmarkoV)
"""

import ctypes
import ctypes.util
import os
import sys
import weakref

try:
    import numpy as np
except ImportError:          # tables and images need numpy; everything else works without it
    np = None

#----------------------------------------------------------------------------------------------
# Library
#----------------------------------------------------------------------------------------------

def _find_lib():
    names = {"linux": "libpzpdir.so", "darwin": "libpzpdir.dylib", "win32": "pzpdir.dll"}.get(sys.platform, "libpzpdir.so")
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [os.environ.get("PZPDIR_LIB"), os.path.join(here, names), os.path.join(here, "..", "pzpdir", names)]
    for c in candidates:
        if c and os.path.isfile(c):
            return c
    found = ctypes.util.find_library("pzpdir")
    if found:
        return found
    raise OSError("pzp.pzpdir: %s not found (build it with `make -C src/pzpdir libpzpdir.so`, or set PZPDIR_LIB)" % names)


_lib = ctypes.CDLL(_find_lib())

c_u8, c_u16, c_u32, c_u64, c_i64 = ctypes.c_uint8, ctypes.c_uint16, ctypes.c_uint32, ctypes.c_uint64, ctypes.c_int64
c_size, c_ssize = ctypes.c_size_t, ctypes.c_ssize_t
c_char_p, c_void_p, c_int, c_uint = ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_uint

MAX_STREAMS = 32
NO_GROUP = 0xFFFFFFFF

O_VERIFY, O_ALLOW_MISSING, O_HUGEPAGE, O_POPULATE = 1, 4, 8, 16
TABLE_GLOBAL, TABLE_BULK = 1, 2
PF_AUTO, PF_MAP, PF_PAGECACHE, PF_BUFFERS = 0, 1, 2, 3
EDIT_ADD, EDIT_REPLACE, EDIT_DROP = 1, 2, 3
WORDS_CANONICAL = 1          # Archive.words(canonical=True): apply the `synonyms` table
EDIT_KEEP_MISSING, EDIT_DROP_MISSING = 1, 2
STORAGE_BLOCK, STORAGE_RAM = 0, 1

_TYPES = {1: "u1", 2: "i1", 3: "<u2", 4: "<i2", 5: "<u4", 6: "<i4", 7: "<u8", 8: "<i8", 9: "<f4", 10: "<f8"}
TYPE_STR = 11


class BlobMeta(ctypes.Structure):
    _fields_ = [("format", c_u32), ("width", c_u32), ("height", c_u32), ("channels", c_u16), ("frames", c_u16), ("bits", c_u8), ("meta_flags", c_u8)]


class BlobInfo(ctypes.Structure):
    _fields_ = [("present", c_int), ("size", c_u64), ("meta", BlobMeta), ("name", c_void_p), ("name_len", c_size),
                ("group", c_u32), ("frame", c_u32), ("member", c_uint), ("shard", c_uint)]


class BlobRef(ctypes.Structure):
    _fields_ = [("data", c_void_p), ("size", c_size), ("format", c_u32)]


class ShardInfo(ctypes.Structure):
    _fields_ = [("path", c_char_p), ("first_ordinal", c_u64), ("record_count", c_u64), ("file_bytes", c_u64),
                ("available", c_int), ("member", c_uint), ("storage", c_int), ("recovery", c_int)]


class Column(ctypes.Structure):
    _fields_ = [("name", c_char_p), ("type", c_u8), ("count", c_u16), ("offset", c_u32)]


class Schema(ctypes.Structure):
    _fields_ = [("name", c_char_p), ("flags", c_uint), ("row_stride", c_u32), ("ncols", c_uint), ("cols", ctypes.POINTER(Column))]


class Group(ctypes.Structure):
    _fields_ = [("first_ordinal", c_u64), ("frames", c_u32), ("id", c_u32), ("index", c_u32), ("name", c_void_p), ("name_len", c_size)]


class TableView(ctypes.Structure):
    _fields_ = [("first_ordinal", c_u64), ("records", c_u64), ("row_index", ctypes.POINTER(c_u32)), ("rows", c_void_p),
                ("total_rows", c_u64), ("strings", c_void_p), ("strings_len", c_u64)]


class PrefetchOpts(ctypes.Structure):
    _fields_ = [("stream_mask", c_u32), ("io_threads", c_uint), ("mode", c_uint), ("budget_bytes", c_u64), ("window", c_uint)]


class Ticket(ctypes.Structure):
    _fields_ = [("pos", c_u64), ("gen", c_u64), ("buf", c_void_p), ("bytes", c_u64)]


class PrefetchStats(ctypes.Structure):
    _fields_ = [(n, c_u64) for n in ("submitted", "prefetched", "hits", "waits", "sync_misses", "unscheduled", "discarded",
                                     "released", "producer_stalls", "bytes_prefetched", "bytes_over_read")] + \
               [("io_seconds", ctypes.c_double), ("elapsed_seconds", ctypes.c_double)] + \
               [(n, c_uint) for n in ("shards_map", "shards_pagecache", "shards_buffers", "direct_fallbacks")] + \
               [("buffer_bytes", c_u64), ("buffer_bytes_peak", c_u64)]


class WriterOpts(ctypes.Structure):
    _fields_ = [("streams", ctypes.POINTER(c_char_p)), ("stream_count", c_uint), ("shard_max_bytes", c_u64), ("align", c_u32)]


class EditRows(ctypes.Structure):
    _fields_ = [("key", c_char_p), ("key_len", c_size), ("csv", c_char_p), ("csv_len", c_size)]


class EditBlob(ctypes.Structure):
    _fields_ = [("key", c_char_p), ("key_len", c_size), ("path", c_char_p), ("name", c_char_p), ("name_len", c_size)]


class SalvagedBlob(ctypes.Structure):
    _fields_ = [("stream", c_uint), ("stream_name", c_char_p), ("name", c_void_p), ("name_len", c_size), ("data", c_void_p),
                ("size", c_size), ("meta", BlobMeta), ("intact", c_int)]


class SalvagedRows(ctypes.Structure):
    _fields_ = [("table", c_uint), ("table_name", c_char_p), ("rows", c_u32), ("row_data", c_void_p), ("row_bytes", c_size),
                ("strings", c_void_p), ("strings_len", c_size), ("csv", c_void_p), ("csv_len", c_size)]


class SalvagedRecord(ctypes.Structure):
    _fields_ = [("file_offset", c_u64), ("key", c_void_p), ("key_len", c_size), ("group", c_u32), ("frame", c_u32),
                ("blob_count", c_uint), ("blobs", ctypes.POINTER(SalvagedBlob)), ("table_count", c_uint), ("tables", ctypes.POINTER(SalvagedRows))]


class SalvageInfo(ctypes.Structure):
    _fields_ = [("records", c_u64), ("blobs", c_u64), ("damaged_blobs", c_u64), ("damaged_headers", c_u64), ("stream_count", c_uint),
                ("streams", (ctypes.c_char * 24) * MAX_STREAMS), ("names_from", c_int), ("schemas_from", c_int)]


_SALVAGE_FN = ctypes.CFUNCTYPE(c_int, ctypes.POINTER(SalvagedRecord), c_void_p)

P = c_void_p   # opaque handles


def _sig(name, restype, *argtypes):
    f = getattr(_lib, name)
    f.restype = restype
    f.argtypes = list(argtypes)
    return f


_sig("pzpd_last_error", c_char_p)
_sig("pzpd_last_error_code", c_int)
_sig("pzpd_open", P, c_char_p, c_uint)
_sig("pzpd_open_many", P, ctypes.POINTER(c_char_p), ctypes.POINTER(c_char_p), c_uint, c_uint)
_sig("pzpd_close", None, P)
_sig("pzpd_count", c_u64, P)
_sig("pzpd_stream_count", c_uint, P)
_sig("pzpd_stream_id", c_int, P, c_char_p)
_sig("pzpd_stream_name", c_char_p, P, c_uint)
_sig("pzpd_member_count", c_uint, P)
_sig("pzpd_member_alias", c_char_p, P, c_uint)
_sig("pzpd_member_id", c_int, P, c_char_p)
_sig("pzpd_member_of", c_int, P, c_u64, ctypes.POINTER(c_u64))
_sig("pzpd_member_range", c_int, P, c_uint, ctypes.POINTER(c_u64), ctypes.POINTER(c_u64))
_sig("pzpd_shard_count", c_uint, P)
_sig("pzpd_shard_info_get", c_int, P, c_uint, ctypes.POINTER(ShardInfo))
_sig("pzpd_storage_kind", c_int, P, c_u64)
_sig("pzpd_find", c_i64, P, c_char_p, c_size, ctypes.POINTER(c_int))
_sig("pzpd_find_in", c_i64, P, c_uint, c_char_p, c_size, ctypes.POINTER(c_int))
_sig("pzpd_find_all", c_size, P, c_char_p, c_size, ctypes.POINTER(c_i64), ctypes.POINTER(c_int), c_size)
_sig("pzpd_record_key", c_void_p, P, c_u64, ctypes.POINTER(c_size))
_sig("pzpd_blob_info_get", c_int, P, c_u64, c_uint, ctypes.POINTER(BlobInfo))
_sig("pzpd_read_into", c_ssize, P, c_u64, c_uint, c_void_p, c_size)
_sig("pzpd_record_span", c_size, P, c_u64, c_u32)
_sig("pzpd_read_record", c_ssize, P, c_u64, c_u32, c_void_p, c_size, ctypes.POINTER(BlobRef))
_sig("pzpd_verify_record", c_int, P, c_u64, c_int)
_sig("pzpd_verify_shard", c_int, P, c_uint)
_sig("pzpd_free", None, c_void_p)
_sig("pzpd_table_count", c_uint, P)
_sig("pzpd_table_id", c_int, P, c_char_p)
_sig("pzpd_table_schema", ctypes.POINTER(Schema), P, c_uint)
_sig("pzpd_table_rows", c_u32, P, c_u64, c_uint, ctypes.POINTER(c_void_p))
_sig("pzpd_global_rows", c_u32, P, c_uint, c_uint, ctypes.POINTER(c_void_p))
_sig("pzpd_table_str", c_void_p, P, c_u64, c_uint, c_void_p, ctypes.POINTER(c_size))
_sig("pzpd_global_str", c_void_p, P, c_uint, c_uint, c_void_p, ctypes.POINTER(c_size))
_sig("pzpd_table_shard_view", c_int, P, c_uint, c_uint, ctypes.POINTER(TableView))
_sig("pzpd_table_csv", c_ssize, P, c_u64, c_uint, c_void_p, c_size)
_sig("pzpd_global_csv", c_ssize, P, c_uint, c_uint, c_void_p, c_size)
_sig("pzpd_detect_format", c_u32, c_void_p, c_size, c_char_p, c_size, ctypes.POINTER(BlobMeta))
_sig("pzpd_collection_write", c_int, c_char_p, ctypes.POINTER(c_char_p), ctypes.POINTER(c_char_p), c_uint, c_uint)
_sig("pzpd_collection_refresh", c_int, c_char_p)
_sig("pzpd_prefetcher_create", P, P, ctypes.POINTER(PrefetchOpts))
_sig("pzpd_prefetch_submit", c_int, P, ctypes.POINTER(c_u64), ctypes.POINTER(c_u32), c_size)
_sig("pzpd_prefetch_clear", None, P)
_sig("pzpd_prefetch_get", c_int, P, c_u64, c_u32, ctypes.POINTER(BlobRef), ctypes.POINTER(Ticket))
_sig("pzpd_prefetch_release", None, P, ctypes.POINTER(Ticket))
_sig("pzpd_prefetch_discard", None, P, c_u64)
_sig("pzpd_prefetch_stats_get", None, P, ctypes.POINTER(PrefetchStats))
_sig("pzpd_prefetch_auto_mode", c_int, P, c_uint)
_sig("pzpd_prefetcher_destroy", None, P)
_sig("pzpd_manifest_rebuild", c_int, c_char_p, ctypes.POINTER(c_char_p), c_uint)
_sig("pzpd_salvage", c_int, c_char_p, P, _SALVAGE_FN, c_void_p, ctypes.POINTER(SalvageInfo))
_sig("pzpd_edit_table", c_int, c_char_p, c_uint, c_char_p, c_char_p, c_uint, ctypes.POINTER(EditRows), c_size, c_char_p, c_size, c_uint, ctypes.POINTER(c_u64))
_sig("pzpd_edit_stream", c_int, c_char_p, c_uint, c_char_p, ctypes.POINTER(EditBlob), c_size, c_uint, ctypes.POINTER(c_u64))
_sig("pzpd_compact", c_int, c_char_p, ctypes.POINTER(c_u64))
_sig("pzpd_writer_create", P, c_char_p, ctypes.POINTER(WriterOpts))
_sig("pzpd_writer_begin", c_int, P, c_char_p, c_size, c_u32, c_u32)
_sig("pzpd_writer_blob_ex", c_int, P, c_uint, c_char_p, c_size, c_void_p, c_size, ctypes.POINTER(BlobMeta))
_sig("pzpd_writer_blob_file", c_int, P, c_uint, c_char_p, c_size, c_char_p)
_sig("pzpd_writer_table", c_int, P, c_char_p, c_char_p, c_uint)
_sig("pzpd_writer_rows", c_int, P, c_uint, c_void_p, c_u32, c_void_p, c_size)
_sig("pzpd_writer_rows_csv", c_int, P, c_uint, c_char_p, c_size)
_sig("pzpd_writer_global_rows", c_int, P, c_uint, c_void_p, c_u32, c_void_p, c_size)
_sig("pzpd_writer_global_rows_csv", c_int, P, c_uint, c_char_p, c_size)
_sig("pzpd_writer_end", c_int, P)
_sig("pzpd_writer_group", c_i64, P, c_char_p, c_size, c_u64)
_sig("pzpd_group_find", c_i64, P, c_char_p, c_size)
_sig("pzpd_group_info", c_int, P, c_u64, ctypes.POINTER(Group))
_sig("pzpd_range_span", c_size, P, c_u64, c_u32, c_u32)
_sig("pzpd_read_range", c_ssize, P, c_u64, c_u32, c_u32, c_void_p, c_size, ctypes.POINTER(BlobRef))
_sig("pzpd_writer_words", c_int, P, c_char_p, c_char_p, c_char_p)
_sig("pzpd_words_open", c_int, P, c_char_p, c_char_p, c_char_p, c_size, c_uint, ctypes.POINTER(P))
_sig("pzpd_words_close", None, P)
_sig("pzpd_words_index", c_int, P, c_uint, ctypes.POINTER(c_char_p), ctypes.POINTER(c_char_p), ctypes.POINTER(c_char_p))
_sig("pzpd_words_sources", c_size, P, c_char_p, c_char_p, ctypes.POINTER(c_void_p), ctypes.POINTER(c_size), c_size)
_sig("pzpd_words_find", c_i64, P, c_char_p, c_size)
_sig("pzpd_words_arrays", c_u32, P, ctypes.POINTER(ctypes.POINTER(c_u64)), ctypes.POINTER(ctypes.POINTER(c_u64)), ctypes.POINTER(c_void_p), ctypes.POINTER(ctypes.POINTER(c_u64)))
_sig("pzpd_words_records", c_size, P, c_u32, ctypes.POINTER(c_u64), c_size)
_sig("pzpd_words_of_record", c_size, P, c_u64, ctypes.POINTER(c_u32), c_size)
_sig("pzpd_words_info", c_int, P, ctypes.POINTER(c_uint), ctypes.POINTER(c_u64))
_sig("pzpd_edit_words", c_int, c_char_p, c_uint, c_char_p, c_char_p, c_char_p)
_TOKEN_FN = ctypes.CFUNCTYPE(c_int, c_void_p, c_size, c_void_p)
_sig("pzpd_tokenize", c_size, c_char_p, c_size, _TOKEN_FN, c_void_p)
_sig("pzpd_writer_finish", c_int, P)
_sig("pzpd_writer_abort", None, P)
try:
    _sig("pzpd_read_pzp", c_void_p, P, c_u64, c_uint, ctypes.POINTER(c_uint), ctypes.POINTER(c_uint), ctypes.POINTER(c_uint), ctypes.POINTER(c_uint))
    _HAVE_PZP = True
except AttributeError:       # library built with PZPDIR_WITH_PZP=0
    _HAVE_PZP = False

class PzpdError(OSError):
    """An error reported by libpzpdir: .code is the enum pzpd_error value, the message its text."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def _raise(what=None):
    msg = (_lib.pzpd_last_error() or b"").decode("utf-8", "replace")
    raise PzpdError(_lib.pzpd_last_error_code(), "%s: %s" % (what, msg) if what else msg)


def _b(s):
    """str / bytes / os.PathLike -> bytes (file names and keys are arbitrary bytes; str is UTF-8)."""
    if isinstance(s, bytes):
        return s
    if isinstance(s, os.PathLike):
        s = os.fspath(s)
        if isinstance(s, bytes):
            return s
    return s.encode("utf-8", "surrogateescape")


def _s(b):
    return b.decode("utf-8", "surrogateescape")


def fourcc(code):
    """FourCC integer -> 4-character string (e.g. 'JPEG', 'PNG ')."""
    return bytes((code >> (8 * i)) & 0xFF for i in range(4)).decode("latin-1")


def _meta_dict(m):
    return {"format": fourcc(m.format), "width": m.width, "height": m.height, "channels": m.channels,
            "frames": m.frames, "bits": m.bits, "meta_flags": m.meta_flags}


def detect_format(data, name=""):
    """Format and metadata of a file's bytes, as pack would store them (dict)."""
    m = BlobMeta()
    nb = _b(name)
    buf = ctypes.create_string_buffer(bytes(data), len(data))
    _lib.pzpd_detect_format(buf, len(data), nb, len(nb), ctypes.byref(m))
    return _meta_dict(m)

#----------------------------------------------------------------------------------------------
# Reading
#----------------------------------------------------------------------------------------------

def open(path, verify=False, aliases=None, allow_missing=False, hugepage=False, populate=False):  # noqa: A001 (mirrors builtins.open)
    """
    Open an archive: a manifest, a single shard, a collection file, or a list of archives opened as one.

    Parameters
    ----------
    path : str or list of str
    verify : bool
        Check record-header and payload checksums on every read.
    aliases : list of str, optional
        Member names when path is a list (default: file names).
    allow_missing : bool
        With a list: open even if some archives are missing (they count as 0 records).
    hugepage, populate : bool
        PZPD_O_HUGEPAGE / PZPD_O_POPULATE (RAM-disk archives).

    Returns
    -------
    Archive
    """
    return Archive(path, verify=verify, aliases=aliases, allow_missing=allow_missing, hugepage=hugepage, populate=populate)


class Archive:
    """An open archive (or several as one). Thread-safe for reads; use as a context manager."""

    def __init__(self, path, verify=False, aliases=None, allow_missing=False, hugepage=False, populate=False):
        flags = (O_VERIFY if verify else 0) | (O_HUGEPAGE if hugepage else 0) | (O_POPULATE if populate else 0)
        if isinstance(path, (list, tuple)):
            paths = (c_char_p * len(path))(*[_b(p) for p in path])
            al = (c_char_p * len(path))(*[_b(x) for x in aliases]) if aliases else None
            self._h = _lib.pzpd_open_many(paths, al, len(path), flags | (O_ALLOW_MISSING if allow_missing else 0))
        else:
            self._h = _lib.pzpd_open(_b(path), flags)
        if not self._h:
            _raise(str(path))
        self._streams = [_s(_lib.pzpd_stream_name(self._h, u)) for u in range(_lib.pzpd_stream_count(self._h))]
        self._tables = {}
        self._words = weakref.WeakSet()      # open Words views: closed before the archive

    # --- lifetime ---------------------------------------------------------------------------
    def close(self):
        """Close the archive. Views, memoryviews and prefetchers of it must not be used afterwards."""
        if self._h:
            for w in list(self._words):
                w.close()
            _lib.pzpd_close(self._h)
            self._h = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def _handle(self):
        if not self._h:
            raise ValueError("archive is closed")
        return self._h

    # --- structure --------------------------------------------------------------------------
    def __len__(self):
        return int(_lib.pzpd_count(self._handle()))

    @property
    def streams(self):
        """Stream names, in on-disk order."""
        return list(self._streams)

    def _stream(self, s):
        if isinstance(s, int):
            if not 0 <= s < len(self._streams):
                raise KeyError(s)
            return s
        try:
            return self._streams.index(s)
        except ValueError:
            raise KeyError("no stream %r (streams: %s)" % (s, ", ".join(self._streams))) from None

    def _mask(self, streams):
        if streams is None:
            return 0xFFFFFFFF
        m = 0
        for s in ([streams] if isinstance(streams, (str, int)) else streams):
            m |= 1 << self._stream(s)
        return m

    @property
    def members(self):
        """Member aliases (one entry for a single archive)."""
        h = self._handle()
        return [_s(_lib.pzpd_member_alias(h, m)) for m in range(_lib.pzpd_member_count(h))]

    def member_of(self, ordinal):
        """(member alias, ordinal within that member) of a record."""
        local = c_u64()
        m = _lib.pzpd_member_of(self._handle(), ordinal, ctypes.byref(local))
        if m < 0:
            _raise()
        return self.members[m], local.value

    def member_range(self, member):
        """(first ordinal, record count) of a member (alias or index)."""
        mi = self._member(member)
        f, c = c_u64(), c_u64()
        if not _lib.pzpd_member_range(self._handle(), mi, ctypes.byref(f), ctypes.byref(c)):
            _raise()
        return f.value, c.value

    def _member(self, member):
        if isinstance(member, int):
            return member
        mi = _lib.pzpd_member_id(self._handle(), _b(member))
        if mi < 0:
            raise KeyError("no member %r" % member)
        return mi

    def shards(self):
        """List of dicts: path, first_ordinal, record_count, file_bytes, available, member, storage, recovery."""
        h = self._handle()
        out = []
        for k in range(_lib.pzpd_shard_count(h)):
            si = ShardInfo()
            if not _lib.pzpd_shard_info_get(h, k, ctypes.byref(si)):
                _raise()
            out.append({"path": _s(si.path), "first_ordinal": si.first_ordinal, "record_count": si.record_count, "file_bytes": si.file_bytes,
                        "available": bool(si.available), "member": si.member, "storage": "ram" if si.storage == STORAGE_RAM else "block",
                        "recovery": si.recovery, "auto_mode": ("auto", "map", "pagecache", "buffers")[max(0, _lib.pzpd_prefetch_auto_mode(h, k))]})
        return out

    # --- lookup -----------------------------------------------------------------------------
    def find(self, key, member=None):
        """Ordinal of the record with this key (in a member, by alias or index, if given). KeyError if none."""
        kb = _b(key)
        so = c_int()
        if member is None:
            i = _lib.pzpd_find(self._handle(), kb, len(kb), ctypes.byref(so))
        else:
            i = _lib.pzpd_find_in(self._handle(), self._member(member), kb, len(kb), ctypes.byref(so))
        if (i < 0) or (so.value != -1):         # keys are searched before names: a name match means no such key
            raise KeyError(key)
        return int(i)

    def find_name(self, name):
        """(ordinal, stream name) of the blob stored under this name. KeyError if none."""
        for o, s in self.find_all(name):
            if s is not None:
                return o, s
        raise KeyError(name)

    def find_all(self, key_or_name):
        """Every match: list of (ordinal, stream name or None for a record key), keys first."""
        kb = _b(key_or_name)
        h = self._handle()
        n = _lib.pzpd_find_all(h, kb, len(kb), None, None, 0)
        if n == 0:
            return []
        ords, sts = (c_i64 * n)(), (c_int * n)()
        n = _lib.pzpd_find_all(h, kb, len(kb), ords, sts, n)
        return [(int(ords[k]), None if sts[k] < 0 else self._streams[sts[k]]) for k in range(n)]

    def key(self, ordinal):
        """Record key (str; bytes that aren't UTF-8 are kept with surrogateescape)."""
        n = c_size()
        p = _lib.pzpd_record_key(self._handle(), ordinal, ctypes.byref(n))
        if not p:
            _raise()
        return _s(ctypes.string_at(p, n.value))

    # --- data -------------------------------------------------------------------------------
    def info(self, ordinal, stream):
        """Everything the index knows about a blob (no data I/O), or None if the record lacks the stream."""
        bi = BlobInfo()
        if not _lib.pzpd_blob_info_get(self._handle(), ordinal, self._stream(stream), ctypes.byref(bi)):
            _raise()
        if not bi.present:
            return None
        d = _meta_dict(bi.meta)
        d.update({"size": bi.size, "name": _s(ctypes.string_at(bi.name, bi.name_len)), "group": None if bi.group == NO_GROUP else bi.group,
                  "frame": bi.frame, "member": bi.member, "shard": bi.shard})
        return d

    def read(self, ordinal, stream):
        """A blob's bytes, or None if the record lacks the stream."""
        bi = BlobInfo()
        h = self._handle()
        u = self._stream(stream)
        if not _lib.pzpd_blob_info_get(h, ordinal, u, ctypes.byref(bi)):
            _raise()
        if not bi.present:
            return None
        buf = ctypes.create_string_buffer(max(1, bi.size))
        n = _lib.pzpd_read_into(h, ordinal, u, buf, bi.size)
        if n < 0:
            _raise()
        return buf.raw[:n]

    def read_record(self, ordinal, streams=None):
        """Several streams of a record with one pread: dict stream -> memoryview (absent streams left out)."""
        h = self._handle()
        mask = self._mask(streams)
        need = _lib.pzpd_record_span(h, ordinal, mask)
        buf = bytearray(max(1, need))
        cbuf = (ctypes.c_char * len(buf)).from_buffer(buf)
        refs = (BlobRef * max(1, len(self._streams)))()
        n = _lib.pzpd_read_record(h, ordinal, mask, cbuf, len(buf), refs)
        if n < 0:
            _raise()
        base = ctypes.addressof(cbuf)
        mv = memoryview(buf)
        out = {}
        for u, name in enumerate(self._streams):
            if refs[u].data:
                o = refs[u].data - base
                out[name] = mv[o:o + refs[u].size]
        return out

    def read_image(self, ordinal, stream):
        """A blob decoded to a numpy array: PZP natively, JPEG / PNG / other images through PIL."""
        if np is None:
            raise ImportError("read_image needs numpy")
        inf = self.info(ordinal, stream)
        if inf is None:
            return None
        if inf["format"] in ("PZP ", "PZPC") and _HAVE_PZP:
            w, hh, bpp, ch = c_uint(), c_uint(), c_uint(), c_uint()
            p = _lib.pzpd_read_pzp(self._handle(), ordinal, self._stream(stream), ctypes.byref(w), ctypes.byref(hh), ctypes.byref(bpp), ctypes.byref(ch))
            if not p:
                _raise()
            try:
                dt = np.uint16 if bpp.value == 16 else np.uint8
                n = w.value * hh.value * ch.value * dt().itemsize
                arr = np.frombuffer(ctypes.string_at(p, n), dtype=dt).reshape(hh.value, w.value, ch.value).copy()
            finally:
                _lib.pzpd_free(p)
            return arr
        import io
        try:
            from PIL import Image
        except ImportError:
            raise ImportError("read_image of %s blobs needs PIL (pip install pillow)" % inf["format"].strip()) from None
        return np.array(Image.open(io.BytesIO(self.read(ordinal, stream))))

    def verify(self, ordinal=None, blobs=True):
        """Check one record (or, without ordinal, every shard index and every record). Returns True / raises."""
        h = self._handle()
        if ordinal is not None:
            if not _lib.pzpd_verify_record(h, ordinal, 1 if blobs else 0):
                _raise("record %d" % ordinal)
            return True
        for k in range(_lib.pzpd_shard_count(h)):
            if not _lib.pzpd_verify_shard(h, k):
                _raise("shard %d" % k)
        for i in range(len(self)):
            if not _lib.pzpd_verify_record(h, i, 1 if blobs else 0):
                _raise("record %d" % i)
        return True

    def stream_stats(self, stream):
        """Summary of a stream over all records: present count, bytes, formats, and (format, WxHxC@bits) combinations."""
        h = self._handle()
        u = self._stream(stream)
        bi = BlobInfo()
        present = total = 0
        formats, combos = {}, {}
        for i in range(len(self)):
            if not _lib.pzpd_blob_info_get(h, i, u, ctypes.byref(bi)):
                _raise()
            if not bi.present:
                continue
            present += 1
            total += bi.size
            f = fourcc(bi.meta.format)
            formats[f] = formats.get(f, 0) + 1
            c = "%s %dx%dx%d@%d" % (f, bi.meta.width, bi.meta.height, bi.meta.channels, bi.meta.bits)
            combos[c] = combos.get(c, 0) + 1
        return {"present": present, "records": len(self), "bytes": total, "formats": formats,
                "combinations": dict(sorted(combos.items(), key=lambda kv: -kv[1]))}

    # --- video groups -----------------------------------------------------------------------
    def group_find(self, name):
        """Ordinal of a group's first record. KeyError if there is no such group."""
        nb = _b(name)
        o = _lib.pzpd_group_find(self._handle(), nb, len(nb))
        if o < 0:
            raise KeyError(name)
        return int(o)

    def group(self, ordinal):
        """The group of a record: dict(first, frames, id, index, name), or None if it's in no group."""
        g = Group()
        if not _lib.pzpd_group_info(self._handle(), ordinal, ctypes.byref(g)):
            if _lib.pzpd_last_error_code() != 0:
                _raise()
            return None
        return {"first": g.first_ordinal, "frames": g.frames, "id": g.id, "index": g.index,
                "name": _s(ctypes.string_at(g.name, g.name_len)) if g.name_len else ""}

    def read_range(self, first, count, streams=None):
        """Consecutive records with one pread (one shard, one group or none): list of dicts stream -> memoryview."""
        h = self._handle()
        mask = self._mask(streams)
        need = _lib.pzpd_range_span(h, first, count, mask)
        if need == 0 and _lib.pzpd_last_error_code() != 0:
            _raise()
        buf = bytearray(max(1, need))
        cbuf = (ctypes.c_char * len(buf)).from_buffer(buf)
        S = len(self._streams)
        refs = (BlobRef * (count * S))()
        n = _lib.pzpd_read_range(h, first, count, mask, cbuf, len(buf), refs)
        if n < 0:
            _raise()
        base = ctypes.addressof(cbuf)
        mv = memoryview(buf)
        out = []
        for k in range(count):
            d = {}
            for u, name in enumerate(self._streams):
                r = refs[k * S + u]
                if r.data:
                    o = r.data - base
                    d[name] = mv[o:o + r.size]
            out.append(d)
        return out

    def read_group(self, group, streams=None, start=0, count=None):
        """Frames of a group (by name or by any of its ordinals) with one pread: list of dicts stream -> memoryview."""
        first = self.group_find(group) if isinstance(group, (str, bytes)) else int(group)
        g = self.group(first)
        if g is None:
            raise KeyError("record %d is in no group" % first)
        n = g["frames"] - start if count is None else count
        return self.read_range(g["first"] + start, n, streams)

    # --- tables -----------------------------------------------------------------------------
    @property
    def tables(self):
        """Table names."""
        h = self._handle()
        return [_s(_lib.pzpd_table_schema(h, t).contents.name) for t in range(_lib.pzpd_table_count(h))]

    def schema(self, table):
        """A table's schema: dict with name, global, bulk, row_stride and columns [(name, type, count, offset)]."""
        t, sc = self._table(table)
        types = {v: k for k, v in {"u8": 1, "i8": 2, "u16": 3, "i16": 4, "u32": 5, "i32": 6, "u64": 7, "i64": 8, "f32": 9, "f64": 10, "str": 11}.items()}
        return {"name": _s(sc.name), "global": bool(sc.flags & TABLE_GLOBAL), "bulk": bool(sc.flags & TABLE_BULK), "row_stride": sc.row_stride,
                "columns": [(_s(sc.cols[c].name), types.get(sc.cols[c].type, "?"), sc.cols[c].count, sc.cols[c].offset) for c in range(sc.ncols)]}

    def _table(self, table):
        h = self._handle()
        t = table if isinstance(table, int) else _lib.pzpd_table_id(h, _b(table))
        if t < 0:
            raise KeyError("no table %r" % table)
        p = _lib.pzpd_table_schema(h, t)
        if not p:
            raise KeyError("no table %r" % table)
        return t, p.contents

    def _dtypes(self, sc):
        """(raw dtype over the row bytes, output dtype with str columns as Python objects, str column names)."""
        if np is None:
            raise ImportError("tables need numpy")
        names, formats, offsets, onames, oformats, strs = [], [], [], [], [], []
        for c in range(sc.ncols):
            col = sc.cols[c]
            n = _s(col.name)
            shape = () if col.count == 1 else (col.count,)
            if col.type == TYPE_STR:
                f = np.dtype([("offset", "<u4"), ("len", "<u4")])
                strs.append(n)
                of = np.dtype(("O", shape)) if shape else np.dtype("O")
            else:
                f = np.dtype((_TYPES[col.type], shape)) if shape else np.dtype(_TYPES[col.type])
                of = f
            names.append(n); formats.append(np.dtype((f, shape)) if (shape and col.type == TYPE_STR) else f); offsets.append(col.offset)
            onames.append(n); oformats.append(of)
        raw = np.dtype({"names": names, "formats": formats, "offsets": offsets, "itemsize": sc.row_stride})
        return raw, np.dtype({"names": onames, "formats": oformats}), strs

    def _rows_array(self, sc, ptr, n, strings_of):
        raw, out, strs = self._dtypes(sc)
        if n == 0 or not ptr:
            return np.zeros(0, dtype=out if strs else raw)
        a = np.frombuffer(ctypes.string_at(ptr, n * sc.row_stride), dtype=raw).copy()   # a copy: independent of the archive
        if not strs:
            return a
        o = np.empty(n, dtype=out)
        for name in out.names:
            if name not in strs:
                o[name] = a[name]
        for name in strs:
            col = a[name]
            # offsets / lengths as Python lists in one go: per-element numpy indexing dominates big tables
            if col.ndim == 1:
                o[name] = [strings_of(off, ln) for off, ln in zip(col["offset"].tolist(), col["len"].tolist())]
            else:
                flat = col.reshape(n, -1)
                vals = [[strings_of(off, ln) for off, ln in zip(offs, lens)]
                        for offs, lens in zip(flat["offset"].tolist(), flat["len"].tolist())]
                for r in range(n):
                    for k in range(flat.shape[1]):
                        o[name][r][k] = vals[r][k]
        return o

    def table(self, ordinal, table):
        """A record's rows of a record table: numpy structured array (str columns decoded to Python str)."""
        h = self._handle()
        t, sc = self._table(table)
        ptr = c_void_p()
        n = _lib.pzpd_table_rows(h, ordinal, t, ctypes.byref(ptr))
        if n == 0 and _lib.pzpd_last_error_code() != 0:
            _raise()

        def strings_of(off, ln, _t=t, _o=ordinal, _p=ptr):
            if ln == 0:
                return ""
            field = (c_u32 * 2)(off, ln)
            # resolve against the record's shard heap: pzpd_table_str takes the field bytes
            sz = c_size()
            s = _lib.pzpd_table_str(h, _o, _t, ctypes.addressof(field), ctypes.byref(sz))
            return _s(ctypes.string_at(s, sz.value)) if s else ""
        return self._rows_array(sc, ptr.value, n, strings_of)

    def global_table(self, table, member=0):
        """Rows of a global table (of one member): numpy structured array."""
        h = self._handle()
        t, sc = self._table(table)
        mi = self._member(member)
        ptr = c_void_p()
        n = _lib.pzpd_global_rows(h, mi, t, ctypes.byref(ptr))
        if n == 0 and _lib.pzpd_last_error_code() != 0:
            _raise()

        def strings_of(off, ln):
            if ln == 0:
                return ""
            field = (c_u32 * 2)(off, ln)
            sz = c_size()
            s = _lib.pzpd_global_str(h, mi, t, ctypes.addressof(field), ctypes.byref(sz))
            return _s(ctypes.string_at(s, sz.value)) if s else ""
        return self._rows_array(sc, ptr.value, n, strings_of)

    def table_all(self, table):
        """All rows of a record table, for bulk loading: (index, rows) with rows of record i = rows[index[i]:index[i+1]]."""
        h = self._handle()
        t, sc = self._table(table)
        if sc.flags & TABLE_GLOBAL:
            raise ValueError("%s is a global table; use global_table()" % table)
        N = len(self)
        index = np.zeros(N + 1, dtype=np.int64)
        parts = []
        total = 0
        for k in range(_lib.pzpd_shard_count(h)):
            v = TableView()
            if not _lib.pzpd_table_shard_view(h, k, t, ctypes.byref(v)):
                _raise("shard %d" % k)
            first = v.first_ordinal
            if v.row_index:
                ix = np.ctypeslib.as_array(v.row_index, shape=(v.records + 1,)).astype(np.int64)
                counts = np.diff(ix)
                heap = ctypes.string_at(v.strings, v.strings_len) if v.strings_len else b""   # one copy per shard

                def strings_of(off, ln, _h=heap):
                    return _s(_h[off:off + ln]) if (ln and off + ln <= len(_h)) else ""
                parts.append(self._rows_array(sc, v.rows, int(v.total_rows), strings_of))
            else:
                counts = np.zeros(v.records, dtype=np.int64)       # a member without this table
            index[first + 1:first + 1 + v.records] = counts
            total += int(counts.sum())
        index = np.cumsum(index)
        raw, out, strs = self._dtypes(sc)
        rows = np.concatenate(parts) if parts else np.zeros(0, dtype=out if strs else raw)
        return index, rows

    # --- word index (spec §3.7) ---------------------------------------------------------------
    def word_indexes(self):
        """The word indexes: list of (table, column, source_column or None)."""
        h = self._handle()
        out, k = [], 0
        t, c, sc = c_char_p(), c_char_p(), c_char_p()
        while _lib.pzpd_words_index(h, k, ctypes.byref(t), ctypes.byref(c), ctypes.byref(sc)):
            out.append((_s(t.value), _s(c.value), _s(sc.value) or None))
            k += 1
        return out

    def word_sources(self, spec):
        """Source values of word index "table.column" (per-source sub-indexes), sorted."""
        table, column = spec.split(".", 1)
        h = self._handle()
        n = _lib.pzpd_words_sources(h, _b(table), _b(column), None, None, 0)
        if n == 0:
            if _lib.pzpd_last_error_code() != 0:
                _raise(spec)
            return []
        names, lens = (c_void_p * n)(), (c_size * n)()
        _lib.pzpd_words_sources(h, _b(table), _b(column), names, lens, n)
        return [_s(ctypes.string_at(names[i], lens[i])) for i in range(n)]

    def words(self, spec, source=None, canonical=False):
        """
        Open word index "table.column": its merged sub-index, or one source's (source="old").
        canonical=True applies the `synonyms` table (a record with "dog" and "dogs" counts once for "dog").
        """
        table, column = spec.split(".", 1)
        out = P()
        src = _b(source) if source is not None else None
        if not _lib.pzpd_words_open(self._handle(), _b(table), _b(column), src, len(src) if src is not None else 0,
                                    WORDS_CANONICAL if canonical else 0, ctypes.byref(out)):
            _raise(spec)
        w = Words(self, out, spec, source, canonical)
        self._words.add(w)
        return w

    def table_csv(self, ordinal, table):
        """A record's rows as CSV text."""
        h = self._handle()
        t, _ = self._table(table)
        n = _lib.pzpd_table_csv(h, ordinal, t, None, 0)
        if n < 0:
            _raise()
        buf = ctypes.create_string_buffer(n + 1)
        _lib.pzpd_table_csv(h, ordinal, t, buf, n + 1)
        return _s(buf.raw[:n])

    # --- prefetching ------------------------------------------------------------------------
    def prefetcher(self, streams=None, io_threads=0, mode="auto", budget_mb=0, window=0):
        """A prefetcher on this archive (see Prefetcher). mode: auto | map | pagecache | buffers."""
        return Prefetcher(self, streams, io_threads, mode, budget_mb, window)


class Record:
    """Blobs of one record handed out by Prefetcher.get(): mapping stream -> memoryview.
    The views are valid until release() (BUFFERS mode) or the archive is closed (MAP / PAGECACHE)."""

    def __init__(self, pf, ordinal, views, ticket):
        self._pf, self.ordinal, self._views, self._ticket = pf, ordinal, views, ticket

    def __getitem__(self, stream):
        return self._views[stream]

    def __contains__(self, stream):
        return stream in self._views

    def __iter__(self):
        return iter(self._views)

    def __len__(self):
        return len(self._views)

    def keys(self):
        return self._views.keys()

    def items(self):
        return self._views.items()

    def get(self, stream, default=None):
        return self._views.get(stream, default)

    def release(self):
        """Give the record back to the prefetcher (frees its buffer in BUFFERS mode)."""
        if self._ticket is not None and self._pf._h:
            for v in self._views.values():
                try:
                    v.release()
                except BufferError:      # the caller still holds a slice of it; it must not be used after this
                    pass
            _lib.pzpd_prefetch_release(self._pf._h, ctypes.byref(self._ticket))
        self._ticket = None
        self._views = {}

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.release()

    def __del__(self):
        try:
            self.release()
        except Exception:
            pass


class Prefetcher:
    """I/O threads that make the records of a submitted schedule resident ahead of use (spec §6).
    Submit the whole epoch order, then get() records from any thread, in any order."""

    _MODES = {"auto": PF_AUTO, "map": PF_MAP, "pagecache": PF_PAGECACHE, "buffers": PF_BUFFERS}

    def __init__(self, archive, streams=None, io_threads=0, mode="auto", budget_mb=0, window=0):
        self._a = archive
        self._mask = archive._mask(streams)
        o = PrefetchOpts(self._mask if streams is not None else 0, io_threads, self._MODES[mode], int(budget_mb) << 20, window)
        self._h = _lib.pzpd_prefetcher_create(archive._handle(), ctypes.byref(o))
        if not self._h:
            _raise()

    def submit(self, ordinals, streams=None):
        """Append records to the schedule. streams: None (the prefetcher's), or one list per ordinal."""
        ords = list(ordinals) if np is None else np.ascontiguousarray(ordinals, dtype=np.uint64)
        n = len(ords)
        if np is not None:
            op = ords.ctypes.data_as(ctypes.POINTER(c_u64))
        else:
            arr = (c_u64 * n)(*ords)
            op = arr
        masks = None
        if streams is not None:
            masks = (c_u32 * n)(*[self._a._mask(s) for s in streams])
        if not _lib.pzpd_prefetch_submit(self._h, op, masks, n):
            _raise()

    def get(self, ordinal, streams=None):
        """A record's blobs (Record: stream -> memoryview). Waits for, or reads, the record if it isn't prefetched yet."""
        S = len(self._a._streams)
        refs = (BlobRef * max(1, S))()
        t = Ticket()
        mask = self._mask if streams is None else self._a._mask(streams)
        r = _lib.pzpd_prefetch_get(self._h, int(ordinal), mask, refs, ctypes.byref(t))
        if r < 0:
            _raise("record %d" % ordinal)
        views = {}
        for u in range(S):
            if refs[u].data:
                views[self._a._streams[u]] = memoryview((ctypes.c_char * refs[u].size).from_address(refs[u].data)).cast("B")
        return Record(self, int(ordinal), views, t)

    def discard(self, ordinal):
        """Drop the first pending claim of an ordinal without reading it."""
        _lib.pzpd_prefetch_discard(self._h, int(ordinal))

    def clear(self):
        """Drop the whole schedule (e.g. after a reshuffle)."""
        _lib.pzpd_prefetch_clear(self._h)

    def stats(self):
        """Counters (dict)."""
        s = PrefetchStats()
        _lib.pzpd_prefetch_stats_get(self._h, ctypes.byref(s))
        return {f: getattr(s, f) for f, _ in PrefetchStats._fields_}

    def close(self):
        """Stop the I/O threads. Release every Record first."""
        if self._h:
            _lib.pzpd_prefetcher_destroy(self._h)
            self._h = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

#----------------------------------------------------------------------------------------------
# Writing
#----------------------------------------------------------------------------------------------

def _size(s):
    if s is None or isinstance(s, int):
        return s or 0
    s = str(s).strip().upper()
    mul = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30, "T": 1 << 40}.get(s[-1:], 1)
    return int(float(s[:-1] if s[-1:] in "KMGT" else s) * mul)


class Writer:
    """Writes an archive (spec §7). Use as a context manager: finish() on success, abort() on an exception."""

    def __init__(self, path, streams, align=4096, shard_size="4G"):
        self._streams = list(streams)
        names = (c_char_p * len(self._streams))(*[_b(s) for s in self._streams])
        o = WriterOpts(names, len(self._streams), _size(shard_size), align)
        self._h = _lib.pzpd_writer_create(_b(path), ctypes.byref(o))
        if not self._h:
            _raise(str(path))
        self._tables = {}

    def _stream(self, s):
        return s if isinstance(s, int) else self._streams.index(s)

    def _t(self, table):
        return table if isinstance(table, int) else self._tables[table]

    def words(self, table, column, source_column=None):
        """Declare a word index (spec §3.7) after its table, before the first record."""
        if not _lib.pzpd_writer_words(self._h, _b(table), _b(column), _b(source_column) if source_column else None):
            _raise("words %s.%s" % (table, column))

    def table(self, name, schema, global_=False, bulk=False):
        """Declare a table (before the first record); returns its id. schema: 'name:type[n] ...'."""
        t = _lib.pzpd_writer_table(self._h, _b(name), _b(schema), (TABLE_GLOBAL if global_ else 0) | (TABLE_BULK if bulk else 0))
        if t < 0:
            _raise("table %s" % name)
        self._tables[name] = t
        return t

    def global_rows_csv(self, table, csv):
        """Rows of a global table, as CSV (before the first record)."""
        c = _b(csv)
        if not _lib.pzpd_writer_global_rows_csv(self._h, self._t(table), c, len(c)):
            _raise()

    def global_rows(self, table, rows, strings=b""):
        """Rows of a global table from a numpy array with the table's row layout (str fields index `strings`)."""
        self._rows(_lib.pzpd_writer_global_rows, table, rows, strings)

    def group(self, name="", bytes_hint=0):
        """Register a video group (name unique in the archive); returns its id for record(key, group=id, frame=n)."""
        nb = _b(name)
        gid = _lib.pzpd_writer_group(self._h, nb, len(nb), int(bytes_hint))
        if gid < 0:
            _raise("group %r" % name)
        return int(gid)

    def record(self, key, group=None, frame=0):
        """Context manager for one record: `with w.record(key): w.add(...)`."""
        return _RecordCtx(self, key, group, frame)

    def begin(self, key, group=None, frame=0):
        k = _b(key)
        if not _lib.pzpd_writer_begin(self._h, k, len(k), NO_GROUP if group is None else group, frame):
            _raise("record %r" % key)

    def end(self):
        if not _lib.pzpd_writer_end(self._h):
            _raise()

    def add(self, stream, name, data, meta=None):
        """Add a blob from bytes. meta (dict: format, width, height, channels, frames, bits) overrides detection."""
        nb = _b(name)
        data = bytes(data)
        m = None
        if meta is not None:
            fmt = meta.get("format", "RAW ")
            code = fmt if isinstance(fmt, int) else int.from_bytes(fmt.ljust(4).encode("latin-1")[:4], "little")
            m = BlobMeta(code, meta.get("width", 0), meta.get("height", 0), meta.get("channels", 0), meta.get("frames", 0), meta.get("bits", 0), 0)
        if not _lib.pzpd_writer_blob_ex(self._h, self._stream(stream), nb, len(nb), data, len(data), ctypes.byref(m) if m is not None else None):
            _raise("blob %r" % name)

    def add_file(self, stream, name, path):
        """Add a blob from a file (format and metadata detected)."""
        nb = _b(name)
        if not _lib.pzpd_writer_blob_file(self._h, self._stream(stream), nb, len(nb), _b(path)):
            _raise("blob %r" % name)

    def rows_csv(self, table, csv):
        """Rows of the current record, as CSV."""
        c = _b(csv)
        if not _lib.pzpd_writer_rows_csv(self._h, self._t(table), c, len(c)):
            _raise()

    def rows(self, table, rows, strings=b""):
        """Rows of the current record from a numpy array with the table's row layout."""
        self._rows(_lib.pzpd_writer_rows, table, rows, strings)

    def _rows(self, fn, table, rows, strings):
        a = np.ascontiguousarray(rows)
        sb = bytes(strings)
        if not fn(self._h, self._t(table), a.ctypes.data if a.size else None, len(a), sb if sb else None, len(sb)):
            _raise()

    def finish(self):
        """Complete the archive (writes the manifest)."""
        if self._h:
            h, self._h = self._h, None
            if not _lib.pzpd_writer_finish(h):
                _raise()

    def abort(self):
        """Abandon the archive (completed shards stay on disk)."""
        if self._h:
            _lib.pzpd_writer_abort(self._h)
            self._h = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, *exc):
        if exc_type is None:
            self.finish()
        else:
            self.abort()

    def __del__(self):
        try:
            self.abort()
        except Exception:
            pass


class _RecordCtx:
    def __init__(self, w, key, group, frame):
        self.w, self.key, self.group, self.frame = w, key, group, frame

    def __enter__(self):
        self.w.begin(self.key, self.group, self.frame)
        return self.w

    def __exit__(self, exc_type, *exc):
        if exc_type is None:
            self.w.end()

#----------------------------------------------------------------------------------------------
# Collections, recovery, edits
#----------------------------------------------------------------------------------------------

def collection_write(out, paths, aliases=None, absolute=False):
    """Write a collection file naming several archives (opened as one by open(out))."""
    ps = (c_char_p * len(paths))(*[_b(p) for p in paths])
    al = (c_char_p * len(paths))(*[_b(a) for a in aliases]) if aliases else None
    if not _lib.pzpd_collection_write(_b(out), ps, al, len(paths), 1 if absolute else 0):
        _raise(str(out))


def collection_refresh(path):
    """Rewrite a collection file after its members changed."""
    if not _lib.pzpd_collection_refresh(_b(path)):
        _raise(str(path))


def rebuild_manifest(manifest, shards):
    """Regenerate a manifest from its shards (byte-identical to the original)."""
    ps = (c_char_p * len(shards))(*[_b(p) for p in shards])
    if not _lib.pzpd_manifest_rebuild(_b(manifest), ps, len(shards)):
        _raise(str(manifest))


def salvage(shard, schemas=None):
    """
    Recover the records of a shard whose index is gone (record-header scan).

    Returns
    -------
    (records, info) : list of dicts {offset, key, group, frame, blobs: [{stream, stream_name, name, data (bytes), meta, intact}],
                      tables: [{table, name, rows, csv}]}, and a dict of counts / stream names.
    """
    out = []

    def cb(rp, user):
        r = rp.contents
        blobs = []
        for i in range(r.blob_count):
            b = r.blobs[i]
            blobs.append({"stream": b.stream, "stream_name": _s(b.stream_name) if b.stream_name else None,
                          "name": _s(ctypes.string_at(b.name, b.name_len)), "data": ctypes.string_at(b.data, b.size) if b.size else b"",
                          "meta": _meta_dict(b.meta), "intact": bool(b.intact)})
        tables = []
        for i in range(r.table_count):
            t = r.tables[i]
            tables.append({"table": t.table, "name": _s(t.table_name) if t.table_name else None, "rows": t.rows,
                           "csv": _s(ctypes.string_at(t.csv, t.csv_len)) if t.csv else None})
        out.append({"offset": r.file_offset, "key": _s(ctypes.string_at(r.key, r.key_len)), "group": None if r.group == NO_GROUP else r.group,
                    "frame": r.frame, "blobs": blobs, "tables": tables})
        return 1
    fn = _SALVAGE_FN(cb)
    info = SalvageInfo()
    if not _lib.pzpd_salvage(_b(shard), schemas._handle() if schemas is not None else None, fn, None, ctypes.byref(info)):
        _raise(str(shard))
    names = [_s(info.streams[u].value) for u in range(info.stream_count)]
    return out, {"records": info.records, "blobs": info.blobs, "damaged_blobs": info.damaged_blobs, "damaged_headers": info.damaged_headers,
                 "streams": names, "names_from": info.names_from, "schemas_from": info.schemas_from}


_OPS = {"add": EDIT_ADD, "replace": EDIT_REPLACE, "drop": EDIT_DROP}


def edit_table(manifest, op, table, rows=None, schema=None, global_=False, bulk=False, global_csv=None, keep_missing=False):
    """
    Add / replace / drop a table without rewriting record data. rows: iterable of (key, csv) or dict key -> csv.
    Returns the number of keys that matched no record.
    """
    items = list(rows.items()) if isinstance(rows, dict) else list(rows or [])
    arr = (EditRows * max(1, len(items)))()
    keep = []
    for k, (key, csv) in enumerate(items):
        kb, cb = _b(key), _b(csv)
        keep += [kb, cb]
        arr[k] = EditRows(kb, len(kb), cb, len(cb))
    g = _b(global_csv) if global_csv is not None else None
    um = c_u64()
    if not _lib.pzpd_edit_table(_b(manifest), _OPS[op], _b(table), _b(schema) if schema else None,
                                (TABLE_GLOBAL if global_ else 0) | (TABLE_BULK if bulk else 0), arr, len(items), g, len(g) if g else 0,
                                EDIT_KEEP_MISSING if keep_missing else 0, ctypes.byref(um)):
        _raise("%s-table %s" % (op, table))
    return um.value


def edit_stream(manifest, op, stream, files=None, drop_missing=False):
    """
    Add / replace / drop a stream (shards rewritten one at a time). files: iterable of (key, path[, name]).
    Returns the number of keys that matched no record.
    """
    items = list(files or [])
    arr = (EditBlob * max(1, len(items)))()
    keep = []
    for k, it in enumerate(items):
        key, path = it[0], it[1]
        name = it[2] if len(it) > 2 else path
        kb, pb, nb = _b(key), _b(path), _b(name)
        keep += [kb, pb, nb]
        arr[k] = EditBlob(kb, len(kb), pb, nb, len(nb))
    um = c_u64()
    if not _lib.pzpd_edit_stream(_b(manifest), _OPS[op], _b(stream), arr, len(items), EDIT_DROP_MISSING if drop_missing else 0, ctypes.byref(um)):
        _raise("%s-stream %s" % (op, stream))
    return um.value


def reindex(manifest, spec=None, source_column=None, drop=False):
    """
    Add / rebuild (spec "table.column") or drop (drop=True) a word index of an existing archive; with no spec,
    rebuild every word index it has.
    """
    if spec is None:
        with Archive(manifest) as a:
            todo = a.word_indexes()
        for t, c, sc in todo:
            if not _lib.pzpd_edit_words(_b(manifest), EDIT_REPLACE, _b(t), _b(c), _b(sc) if sc else None):
                _raise("%s.%s" % (t, c))
        return
    table, column = spec.split(".", 1)
    if not _lib.pzpd_edit_words(_b(manifest), EDIT_DROP if drop else EDIT_REPLACE, _b(table), _b(column),
                                _b(source_column) if source_column else None):
        _raise(spec)


def tokenize(text):
    """The words of text under tokenizer v1, the word index's rule (spec §3.7)."""
    out = []

    def cb(p, n, _u):
        out.append(ctypes.string_at(p, n).decode("ascii"))
        return 0
    b = _b(text)
    _lib.pzpd_tokenize(b, len(b), _TOKEN_FN(cb), None)
    return out


class Words:
    """
    One view (surface or canonical) of one sub-index (merged, or one source) of a word index; Archive.words().
    Word ids are valid only for this object and are never token ids.

        len(w); w.words (list[str]); w.records_per_word / w.count_per_word (numpy, by word id)
        w.find("dog") -> id or -1;  w.word(id);  w.records("dog" or id) -> numpy u64 ordinals
        w.of_record(ordinal) -> numpy u32 word ids;  w.words_of_record(ordinal) -> list[str]
    """

    def __init__(self, archive, handle, spec, source, canonical):
        self._archive, self._h = archive, handle      # the archive stays referenced while this is open
        self.spec, self.source, self.canonical = spec, source, canonical
        rec, cnt, off = ctypes.POINTER(c_u64)(), ctypes.POINTER(c_u64)(), ctypes.POINTER(c_u64)()
        heap = c_void_p()
        n = _lib.pzpd_words_arrays(handle, ctypes.byref(rec), ctypes.byref(cnt), ctypes.byref(heap), ctypes.byref(off))
        if np is None:
            raise ImportError("word indexes need numpy")
        self.records_per_word = np.ctypeslib.as_array(rec, shape=(n,)).copy() if n else np.zeros(0, np.uint64)
        self.count_per_word = np.ctypeslib.as_array(cnt, shape=(n,)).copy() if n else np.zeros(0, np.uint64)
        offsets = np.ctypeslib.as_array(off, shape=(n + 1,)).copy()
        text = ctypes.string_at(heap, int(offsets[-1])).decode("ascii") if n else ""
        o = offsets.tolist()
        self.words = [text[o[i]:o[i + 1]] for i in range(n)]
        tok, cov = c_uint(), c_u64()
        _lib.pzpd_words_info(handle, ctypes.byref(tok), ctypes.byref(cov))
        self.tokenizer, self.covered = tok.value, cov.value

    def close(self):
        if self._h:
            _lib.pzpd_words_close(self._h)
            self._h = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def _handle(self):
        if not self._h:
            raise ValueError("word index is closed")
        return self._h

    def __len__(self):
        return len(self.words)

    def word(self, id_):
        return self.words[id_]

    def find(self, word):
        """Id of a word, or -1."""
        b = _b(word)
        return int(_lib.pzpd_words_find(self._handle(), b, len(b)))

    def _id(self, word_or_id):
        if isinstance(word_or_id, str):
            i = self.find(word_or_id)
            if i < 0:
                raise KeyError(word_or_id)
            return i
        return int(word_or_id)

    def records(self, word_or_id):
        """Ordinals of the records containing the word (numpy u64, ascending)."""
        h, i = self._handle(), self._id(word_or_id)
        n = _lib.pzpd_words_records(h, i, None, 0)
        if n == 0 and _lib.pzpd_last_error_code() != 0:
            _raise(str(word_or_id))
        out = np.zeros(n, dtype=np.uint64)
        if n:
            _lib.pzpd_words_records(h, i, out.ctypes.data_as(ctypes.POINTER(c_u64)), n)
        return out

    def of_record(self, ordinal):
        """Word ids of one record (numpy u32, ascending)."""
        h = self._handle()
        buf = np.zeros(256, dtype=np.uint32)
        n = _lib.pzpd_words_of_record(h, ordinal, buf.ctypes.data_as(ctypes.POINTER(c_u32)), 256)
        if n == 0 and _lib.pzpd_last_error_code() != 0:
            _raise("record %d" % ordinal)
        if n > 256:
            buf = np.zeros(n, dtype=np.uint32)
            _lib.pzpd_words_of_record(h, ordinal, buf.ctypes.data_as(ctypes.POINTER(c_u32)), n)
        return buf[:n].copy()

    def words_of_record(self, ordinal):
        """The words of one record (list of str)."""
        return [self.words[i] for i in self.of_record(ordinal)]


def compact(manifest):
    """Remove dead table sections left by table edits; returns the bytes reclaimed."""
    freed = c_u64()
    if not _lib.pzpd_compact(_b(manifest), ctypes.byref(freed)):
        _raise(str(manifest))
    return freed.value
