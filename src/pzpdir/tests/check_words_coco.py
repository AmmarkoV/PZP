#!/usr/bin/env python3
"""
Phase 8 gate: the word index of a packed COCO val2017 archive equals a Python reference.

The reference is computed independently of the library: every caption row of every record, tokenized
with `re.findall(r'\\w+', text.lower())` keeping all-ASCII tokens only (tokenizer v1, spec §3.7).
For the merged sub-index and for each source's sub-index, in the surface and the canonical view:
  - the vocabulary, every word's records / count, and every word's postings are equal;
  - every record's word list is equal;
  - the canonical view uses union semantics (a record with "dog" and "dogs" counts once for "dog").
Then, after each edit, the views are checked again and the archive is verified with --blobs:
  - replace-table synonyms: the shards' word index sections stay byte-identical;
  - replace-table descriptions (the "old" captions removed): the index is rebuilt;
  - reindex --drop, then reindex: gone, then back;
  - compact, replace-stream, rebuild-manifest (manifest byte-identical).

Usage:
    check_words_coco.py PZPDIR RECORD_LIST WORKDIR

RECORD_LIST is a pzpdir record list with a `descriptions` table (source:str,text:str), e.g. the one
pzpdir_list_from_db.py writes for COCO val2017; only its descriptions rows are used.

Repository : https://github.com/AmmarkoV/PZP
Author     : Ammar Qammaz (AmmarkoV)
"""

import os
import re
import struct
import subprocess
import sys
import csv
import io

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
import pzp.pzpdir as pzpdir   # noqa: E402

SYNONYMS = [("dogs", "dog"), ("puppy", "dog"), ("cats", "cat"), ("men", "man"), ("women", "woman"), ("people", "person")]
FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("\033[31mFAIL\033[0m", what)
    return cond


def tokens(text):
    """Tokenizer v1, computed with Python's re (the reference)."""
    return [w for w in re.findall(r"\w+", text.lower()) if w.isascii()]


def unescape(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            out.append({"t": "\t", "n": "\n", "\\": "\\", "#": "#", "@": "@"}[s[i + 1]])
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def read_captions(path):
    """{key: [(source, text), ...]} in list order, from the list's descriptions rows."""
    caps = {}
    for line in open(path, encoding="utf-8", errors="surrogateescape"):
        line = line.rstrip("\n")
        if not line or line[0] in "#@":
            continue
        f = line.split("\t")
        if len(f) < 3:
            continue
        key = unescape(f[0])
        caps.setdefault(key, [])
        if f[1] == "descriptions":
            for row in csv.reader(io.StringIO(unescape(f[2]))):
                caps[key].append((row[0], row[1]))
    return caps


def write_list(path, keys, caps, blob, synonyms):
    with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write("@table descriptions source:str text:str\n")
        f.write("@global synonyms word:str canonical:str\n")
        for w, c in synonyms:
            f.write("@row synonyms %s,%s\n" % (w, c))
        f.write("@words descriptions.text source\n")
        for i, k in enumerate(keys):
            ek = k.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")
            if ek[0] in "#@":
                ek = "\\" + ek
            f.write("%s\ttxt\t%s\tb%d.txt\n" % (ek, blob, i))
            for src, text in caps[k]:
                row = io.StringIO()
                csv.writer(row, lineterminator="").writerow([src, text])
                f.write("%s\tdescriptions\t%s\n" % (ek, row.getvalue().replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")))


def reference(keys, caps, source, synonyms):
    """{word: (records set, count)} and per-record word sets, for one sub-index and view."""
    syn = dict(synonyms) if synonyms is not None else {}
    words, per = {}, []
    for i, k in enumerate(keys):
        mine = set()
        for src, text in caps[k]:
            if source is not None and src != source:
                continue
            for t in tokens(text):
                t = syn.get(t, t)
                r, c = words.setdefault(t, [set(), 0])
                r.add(i)
                words[t][1] = c + 1
                mine.add(t)
        per.append(mine)
    return words, per


def compare(a, keys, caps, label, synonyms):
    sources = sorted({s for k in keys for s, _ in caps[k] if s})
    check(a.word_sources("descriptions.text") == sources, "%s: sources %s" % (label, a.word_sources("descriptions.text")))
    for source in [None] + sources:
        for canonical in (False, True):
            ref, per = reference(keys, caps, source, synonyms if canonical else None)
            name = "%s [%s, %s]" % (label, source or "merged", "canonical" if canonical else "surface")
            with a.words("descriptions.text", source=source, canonical=canonical) as w:
                ok = check(w.words == sorted(ref), name + ": vocabulary")
                bad = 0
                for i, word in enumerate(w.words if ok else []):
                    r, c = ref[word]
                    if int(w.records_per_word[i]) != len(r) or int(w.count_per_word[i]) != c or w.records(i).tolist() != sorted(r):
                        bad += 1
                check(bad == 0, name + ": %d words with other records / count / postings" % bad)
                bad = sum(1 for o in range(len(keys)) if set(w.words_of_record(o)) != per[o] or len(w.of_record(o)) != len(per[o]))
                check(bad == 0, name + ": %d records with other words" % bad)
                check(w.covered == len(keys), name + ": covered %d" % w.covered)
    print("ok  ", label)


def word_sections(base):
    """{shard file: [bytes of each word index section]} (directory in the superblock extension)."""
    out = {}
    d = os.path.dirname(base) or "."
    stem = os.path.basename(base)[:-5]
    for fn in sorted(os.listdir(d)):
        if not (fn.startswith(stem + ".") and fn.endswith(".pzpd") and fn != os.path.basename(base)):
            continue
        data = open(os.path.join(d, fn), "rb").read()
        secs = []
        for j in range(4):
            slot = data[1736 + 64 * j: 1736 + 64 * (j + 1)]
            if slot[0] == 0:
                break
            off, n = struct.unpack("<QQ", slot[48:64])
            secs.append(data[off:off + n])
        out[fn] = secs
    return out


def run(*args):
    r = subprocess.run(list(args), capture_output=True, text=True)
    check(r.returncode == 0, " ".join(args[:3]) + ": " + (r.stderr.strip()[-300:]))
    return r


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    tool, lst, work = sys.argv[1:]
    os.makedirs(work, exist_ok=True)
    caps = read_captions(lst)
    keys = list(caps)
    blob = os.path.join(work, "blob.txt")
    open(blob, "w").write("x\n")
    arch = os.path.join(work, "wcoco.pzpd")
    for fn in os.listdir(work):
        if fn.startswith("wcoco."):
            os.remove(os.path.join(work, fn))
    wl = os.path.join(work, "wcoco.tsv")
    write_list(wl, keys, caps, blob, SYNONYMS)
    run(tool, "pack", arch, wl, "--shard-size", "256K", "--align", "64")
    shards = len([f for f in os.listdir(work) if f.startswith("wcoco.0")])
    print("packed %d records into %d shards" % (len(keys), shards))
    check(shards > 5, "several shards")

    def full(label, syn, c=caps):
        with pzpdir.open(arch) as a:
            compare(a, keys, c, label, syn)
        run(tool, "verify", arch, "--blobs")

    full("packed", SYNONYMS)

    # replace-table synonyms: shard word sections byte-identical, canonical view follows the new rules
    before = word_sections(arch)
    syn2 = SYNONYMS[:3] + [("kids", "child"), ("children", "child")]
    sl = os.path.join(work, "syn.tsv")
    with open(sl, "w") as f:
        f.write("@global synonyms word:str canonical:str\n")
        for w, c in syn2:
            f.write("@row synonyms %s,%s\n" % (w, c))
    run(tool, "replace-table", arch, "synonyms", sl)
    check(word_sections(arch) == before, "replace-table synonyms left the word index sections byte-identical")
    full("synonyms replaced", syn2)

    # a chained rule is rejected with its row, and nothing changes
    with open(sl, "w") as f:
        f.write("@global synonyms word:str canonical:str\n@row synonyms dogs,dog\n@row synonyms dog,hound\n")
    r = subprocess.run([tool, "replace-table", arch, "synonyms", sl], capture_output=True, text=True)
    check(r.returncode != 0 and "row 1" in r.stderr and "one step" in r.stderr, "chained synonyms rejected with its row: " + r.stderr.strip())
    check(word_sections(arch) == before, "rejected synonyms changed nothing")

    # replace-table descriptions without the "old" captions: the index is rebuilt
    caps2 = {k: [(s, t) for s, t in v if s != "old"] for k, v in caps.items()}
    dl = os.path.join(work, "desc.tsv")
    with open(dl, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write("@table descriptions source:str text:str\n")
        for k in keys:
            for s, t in caps2[k]:
                row = io.StringIO()
                csv.writer(row, lineterminator="").writerow([s, t])
                f.write("%s\tdescriptions\t%s\n" % (k, row.getvalue().replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")))
    run(tool, "replace-table", arch, "descriptions", dl)
    full("descriptions replaced", syn2, caps2)
    # back to the full captions for the remaining checks
    with open(dl, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write("@table descriptions source:str text:str\n")
        for k in keys:
            for s, t in caps[k]:
                row = io.StringIO()
                csv.writer(row, lineterminator="").writerow([s, t])
                f.write("%s\tdescriptions\t%s\n" % (k, row.getvalue().replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")))
    run(tool, "replace-table", arch, "descriptions", dl)

    # reindex --drop, then reindex
    run(tool, "reindex", arch, "descriptions.text", "--drop")
    with pzpdir.open(arch) as a:
        check(a.word_indexes() == [], "word index dropped")
    run(tool, "verify", arch, "--blobs")
    run(tool, "reindex", arch, "descriptions.text", "source")
    full("reindexed", syn2)

    # compact, replace-stream, rebuild-manifest
    run(tool, "compact", arch)
    full("compacted", syn2)
    stl = os.path.join(work, "stream.tsv")
    with open(stl, "w") as f:
        for i, k in enumerate(keys[:50]):
            f.write("%s\ttxt\t%s\tc%d.txt\n" % (k, blob, i))
    run(tool, "replace-stream", arch, "txt", stl)
    full("stream replaced", syn2)
    man = open(arch, "rb").read()
    shard_files = sorted(os.path.join(work, f) for f in os.listdir(work) if f.startswith("wcoco.0"))
    os.remove(arch)
    run(tool, "rebuild-manifest", *shard_files, "--out", arch)
    check(open(arch, "rb").read() == man, "rebuild-manifest is byte-identical")
    full("manifest rebuilt", syn2)

    print(("\033[32mall word index checks passed\033[0m" if not FAILS else "\033[31m%d failed\033[0m" % len(FAILS)))
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
