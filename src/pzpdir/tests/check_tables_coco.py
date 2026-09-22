#!/usr/bin/env python3
"""
Phase 1c gate: the annotation tables of a packed COCO val2017 archive reproduce their sources.

Uses only `pzpdir export-table` output (record-list lines: key<TAB>table<TAB>csv, or @row table csv).
  1. joints / image / persons regenerate the .db (DB1) text byte-for-byte, except the token lines,
     which are deliberately not migrated (descriptions are stored as text instead).
  2. every caption text is byte-identical to its descriptions*.json source.
  3. buildVocabulary.py's tokenisation of the archived texts, mapped through index_to_word.json,
     reproduces vocabulary.json's token IDs. vocabulary.json files built before buildVocabulary.py
     started dropping non-ASCII tokens need --legacy-tokenizer; without it, differences that are
     explained only by non-ASCII words are reported but not counted as failures.
  4. every descriptor vector is bit-identical to the descriptor file.

Usage:
    check_tables_coco.py PZPDIR ARCHIVE DB [--descriptions SRC=FILE.json ...] [--descriptors MODEL=FILE ...]
                         [--vocabulary vocabulary.json --index-to-word index_to_word.json --vocab-source SRC]

Repository : https://github.com/AmmarkoV/PZP
Author     : Ammar Qammaz (AmmarkoV)
"""

import argparse
import csv
import io
import json
import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
from pzpdir_list_from_db import read_descriptors   # noqa: E402


def unescape(s):
    """Undo the record-list escaping."""
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            out.append({"t": "\t", "n": "\n", "\\": "\\", "#": "#", "@": "@"}[s[i + 1]])
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def export(pzpdir, archive, table):
    """
    Rows of a table via `pzpdir export-table`.

    Returns
    -------
    list
        [(key, [csv fields]), ...] for record tables, [(None, [fields]), ...] for global ones.
    """
    out = subprocess.run([pzpdir, "export-table", archive, table], check=True, capture_output=True).stdout.decode("utf-8", "surrogateescape")
    rows = []
    for line in out.split("\n"):
        if not line:
            continue
        if line.startswith("@row "):
            key, text = None, line.split(" ", 2)[2]
        else:
            k, t, text = line.split("\t", 2)
            key = unescape(k)
        fields = next(csv.reader(io.StringIO(unescape(text)), strict=True))
        rows.append((key, fields))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pzpdir")
    ap.add_argument("archive")
    ap.add_argument("db")
    ap.add_argument("--descriptions", action="append", default=[])
    ap.add_argument("--descriptors", action="append", default=[])
    ap.add_argument("--vocabulary")
    ap.add_argument("--index-to-word")
    ap.add_argument("--vocab-source", default="deepseekvl2")
    ap.add_argument("--legacy-tokenizer", action="store_true", help="keep non-ASCII tokens (buildVocabulary.py before the ASCII filter)")
    a = ap.parse_args()
    failures = 0

    #--- 1. DB1 regeneration -----------------------------------------------------
    db = open(a.db).read().split("\n")
    joints = export(a.pzpdir, a.archive, "joints")
    image = {k: f for k, f in export(a.pzpdir, a.archive, "image")}
    persons = {}
    for k, f in export(a.pzpdir, a.archive, "persons"):
        persons.setdefault(k, []).append(f)
    J = len(joints)
    regen = ["DB1", db[1], str(J)] + [f[0] for _, f in joints] + [f[1] for _, f in joints]
    i = 3 + 2 * J
    token_lines = []
    order = []
    while i < len(db) and db[i]:
        key = db[i]
        order.append(key)
        w, h = image[key]
        people = persons.get(key, [])
        regen += [key, "%s,%s,%d" % (w, h, len(people)), None]          # None: token line not migrated
        token_lines.append(len(regen) - 1)
        for f in people:
            regen.append("SK%s,%s" % (f[0], ",".join(f[1:])))
        i += 3 + int(db[i + 1].split(",")[2])
    compared = mism = 0
    for n, line in enumerate(regen):
        if line is None:
            continue
        compared += 1
        if n >= len(db) or db[n] != line:
            mism += 1
            if mism <= 5:
                print("DB1 line %d differs:\n  db:    %r\n  regen: %r" % (n + 1, db[n] if n < len(db) else None, line))
    print("1. DB1 regeneration: %d lines compared (%d token lines skipped), %d differ" % (compared, len(token_lines), mism))
    failures += mism

    #--- 2. captions ---------------------------------------------------------------
    rows = export(a.pzpdir, a.archive, "descriptions")
    by = {}
    for k, (src, text) in rows:
        by.setdefault(src, {})[k] = text
    for spec in a.descriptions:
        src, fn = spec.split("=", 1)
        d = {os.path.basename(k): v for k, v in json.load(open(fn), strict=False).items()}
        got = by.get(src, {})
        bad = sum(1 for k, v in d.items() if got.get(k) != v)
        print("2. captions %-12s %d texts, %d differ" % (src, len(d), bad))
        failures += bad

    #--- 3. token IDs ----------------------------------------------------------------
    if a.vocabulary and a.index_to_word:
        itw = json.load(open(a.index_to_word))
        w2i = {w: int(i) for i, w in itw.items()}
        voc = {os.path.basename(k): v for k, v in json.load(open(a.vocabulary)).items()}
        bad = checked = asciiOnly = 0
        for k, text in by.get(a.vocab_source, {}).items():
            allw = re.findall(r'\w+|[.,()]', text.lower())
            words = allw if a.legacy_tokenizer else [w for w in allw if w.isascii()]
            ids = [w2i[w] for w in words]
            checked += 1
            if voc.get(k) != ids:
                # Explained only by non-ASCII words (vocabulary built by the older tokenizer)?
                if not a.legacy_tokenizer and voc.get(k) == [w2i[w] for w in allw if w in w2i]:
                    asciiOnly += 1
                    continue
                bad += 1
                if bad <= 3:
                    print("   tokens differ for %s: %s vs %s" % (k, voc.get(k), ids))
        print("3. token IDs from archived texts (%s tokenizer): %d samples, %d differ%s" %
              ("legacy" if a.legacy_tokenizer else "current", checked, bad,
               (", %d more differ only by non-ASCII words" % asciiOnly) if asciiOnly else ""))
        failures += bad

    #--- 4. descriptors ----------------------------------------------------------------
    for spec in a.descriptors:
        model, fn = spec.split("=", 1)
        D, vecs = read_descriptors(fn)
        rows = export(a.pzpdir, a.archive, "descriptor_" + model)
        bad = 0
        for k, f in rows:
            raw = b"".join(struct.pack("<f", float(x)) for x in f)
            if raw != vecs.get(os.path.basename(k)):
                bad += 1
        missing = len(vecs) - len(rows)
        print("4. descriptor_%s: D = %d, %d vectors, %d differ, %d missing" % (model, D, len(rows), bad, missing))
        failures += bad + max(missing, 0)

    print("PASS" if failures == 0 else "FAIL (%d problems)" % failures)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
