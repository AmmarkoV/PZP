#!/usr/bin/env python3
"""
Tests of the Python bindings (pzp.pzpdir, phase 5): a Writer round trip with tables, every read
path, the prefetcher (MAP / PAGECACHE / BUFFERS, several Python threads), collections, recovery
and edits. Runs under pytest, or directly:  python3 src/pzpdir/tests/test_pzpdir_py.py

Scratch files go to $PZPDIR_TEST_DIR (default /tmp/pzpdir_test)/py.

Repository : https://github.com/AmmarkoV/PZP
Author     : Ammar Qammaz (AmmarkoV)
"""

import ctypes
import io
import os
import random
import shutil
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))            # src/ (the pzp package)
import numpy as np                                             # noqa: E402
import pzp.pzpdir as pzpdir                                    # noqa: E402

DIR = os.path.join(os.environ.get("PZPDIR_TEST_DIR", "/tmp/pzpdir_test"), "py")
N = 200
STREAMS = ["rgb", "depth"]


def payload(i, s):
    r = random.Random(i * 7 + s)
    return bytes(r.getrandbits(8) for _ in range(50 + (i * 37 + s * 11) % 900))


def png_bytes(i):
    from PIL import Image
    a = np.full((8 + i % 5, 6, 3), i % 256, dtype=np.uint8)
    b = io.BytesIO()
    Image.fromarray(a).save(b, format="PNG")
    return b.getvalue(), a


def build(path, prefix="k", with_tables=True, shard_size="64K", align=64):
    """Write the test archive; returns the expected contents."""
    exp = {}
    with pzpdir.Writer(path, STREAMS, align=align, shard_size=shard_size) as w:
        if with_tables:
            j = w.table("joints", "name:str parent:u16", global_=True)
            p = w.table("persons", "id:u16 label:str kp:u16[6]")
            v = w.table("vec", "v:f32[3]", bulk=True)
            w.global_rows_csv(j, 'head,0\n"left, eye",0\nδ,1\n')
        for i in range(N):
            key = "%s%03d" % (prefix, i)
            with w.record(key):
                if i % 10 == 3:
                    data, arr = png_bytes(i)
                    w.add("rgb", "rgb/%s.png" % key, data)
                    exp[(i, "rgb")] = data
                else:
                    w.add("rgb", "rgb/%s.bin" % key, payload(i, 0))
                    exp[(i, "rgb")] = payload(i, 0)
                if i % 4 != 1:
                    w.add("depth", "depth/%s.raw" % key, payload(i, 1), meta={"format": "RAW ", "width": i, "height": 2, "channels": 1, "bits": 16})
                    exp[(i, "depth")] = payload(i, 1)
                if with_tables:
                    for q in range(i % 3):
                        w.rows_csv(p, '%d,"person %d, ""q""",%d,%d,2,3,4,%d' % (q, i, i, q, 65535 - i))
                    if i % 2 == 0:
                        w.rows(v, np.array([([np.float32(i) + 0.5, -1.0, 3e38],)], dtype=[("v", "<f4", (3,))]))
    return exp


def fresh(name):
    d = os.path.join(DIR, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    return d


#----------------------------------------------------------------------------------------------

def test_abi():
    """ctypes structs have the C sizes (x86-64 / LP64 values from sizeof in pzpdir.h)."""
    want = {"Group": 40, "BlobMeta": 20, "BlobInfo": 72, "BlobRef": 24, "ShardInfo": 48, "Column": 16, "Schema": 32, "TableView": 56,
            "PrefetchOpts": 32, "Ticket": 32, "PrefetchStats": 136, "WriterOpts": 32, "EditRows": 32, "EditBlob": 40,
            "SalvagedBlob": 72, "SalvagedRows": 72, "SalvagedRecord": 64, "SalvageInfo": 816}
    for name, size in want.items():
        assert ctypes.sizeof(getattr(pzpdir, name)) == size, name


def test_roundtrip():
    d = fresh("rt")
    path = os.path.join(d, "a.pzpd")
    exp = build(path)
    with pzpdir.open(path) as a:
        assert len(a) == N and a.streams == STREAMS and a.members == ["a"]
        assert len(a.shards()) > 3
        for i in range(N):
            key = "k%03d" % i
            assert a.find(key) == i and a.key(i) == key
            for s in STREAMS:
                want = exp.get((i, s))
                assert a.read(i, s) == want
                inf = a.info(i, s)
                assert (inf is None) == (want is None)
                if inf is not None:
                    assert inf["size"] == len(want) and inf["name"].startswith(s + "/")
            rec = a.read_record(i, STREAMS)
            assert set(rec) == {s for s in STREAMS if (i, s) in exp}
            for s, mv in rec.items():
                assert bytes(mv) == exp[(i, s)]
        inf = a.info(4, "depth")
        assert inf["format"] == "RAW " and inf["width"] == 4 and inf["bits"] == 16
        assert a.find_name("depth/k007.raw") == (7, "depth")
        assert a.find_all("k007") == [(7, None)]
        try:
            a.find("depth/k007.raw")              # a name, not a key
            assert False
        except KeyError:
            pass
        assert a.verify()
        st = a.stream_stats("depth")
        assert st["present"] == N - N // 4 and st["formats"] == {"RAW ": st["present"]}
        img = a.read_image(3, "rgb")
        assert img.shape == png_bytes(3)[1].shape and (img == png_bytes(3)[1]).all()


def test_tables():
    d = fresh("tables")
    path = os.path.join(d, "a.pzpd")
    build(path)
    with pzpdir.open(path) as a:
        assert a.tables == ["joints", "persons", "vec"]
        sc = a.schema("persons")
        assert [c[:3] for c in sc["columns"]] == [("id", "u16", 1), ("label", "str", 1), ("kp", "u16", 6)] and not sc["global"]
        assert a.schema("vec")["bulk"]
        j = a.global_table("joints")
        assert list(j["name"]) == ["head", "left, eye", "δ"] and list(j["parent"]) == [0, 0, 1]
        t = a.table(5, "persons")
        assert len(t) == 2 and list(t["id"]) == [0, 1] and t["label"][1] == 'person 5, "q"' and list(t["kp"][1]) == [5, 1, 2, 3, 4, 65530]
        assert len(a.table(3, "persons")) == 0
        v = a.table(4, "vec")
        assert v["v"].dtype == np.float32 and list(v["v"][0]) == [np.float32(4.5), -1.0, np.float32(3e38)]
        idx, rows = a.table_all("persons")
        assert idx[0] == 0 and idx[-1] == len(rows) == sum(i % 3 for i in range(N))
        for i in (0, 5, 77, N - 1):
            ref = a.table(i, "persons")
            got = rows[idx[i]:idx[i + 1]]
            assert list(got["id"]) == list(ref["id"]) and list(got["label"]) == list(ref["label"])
        assert a.table_csv(5, "persons").startswith('0,"person 5, ""q""",5,0,')


def test_prefetcher_modes_and_threads():
    d = fresh("pf")
    path = os.path.join(d, "a.pzpd")
    exp = build(path, with_tables=False)
    order = list(range(N))
    random.Random(1).shuffle(order)
    with pzpdir.open(path) as a:
        for mode in ("map", "pagecache", "buffers", "auto"):
            with a.prefetcher(streams=STREAMS, io_threads=2, mode=mode, window=16, budget_mb=1) as pf:
                pf.submit(order)
                errors = []

                def worker(t, T=4):
                    for pos in range(t, N, T):
                        i = order[pos]
                        with pf.get(i) as rec:
                            for s in STREAMS:
                                want = exp.get((i, s))
                                if (want is None) != (s not in rec) or (want is not None and bytes(rec[s]) != want):
                                    errors.append((i, s))
                th = [threading.Thread(target=worker, args=(t,)) for t in range(4)]
                [x.start() for x in th]
                [x.join() for x in th]
                st = pf.stats()
                assert not errors, (mode, errors[:3])
                assert st["released"] == N and st["hits"] + st["waits"] + st["sync_misses"] == N and st["buffer_bytes"] == 0
                if mode == "buffers":
                    assert st["shards_buffers"] > 0 and st["buffer_bytes_peak"] > 0
        with a.prefetcher(mode="map") as pf:
            pf.submit([1, 2, 3], streams=[["rgb"], ["rgb", "depth"], "depth"])
            with pf.get(1) as r:
                assert set(r) == {"rgb"}
            with pf.get(2, streams=["depth"]) as r:
                assert set(r) == {"depth"} and bytes(r["depth"]) == exp[(2, "depth")]
            pf.discard(3)
            assert pf.stats()["discarded"] == 1


def test_collections():
    d = fresh("coll")
    pa, pb = os.path.join(d, "a.pzpd"), os.path.join(d, "b.pzpd")
    ea = build(pa, "k")
    eb = build(pb, "k")                                       # same keys: duplicates across members
    with pzpdir.open([pa, pb], aliases=["first", "second"]) as a:
        assert len(a) == 2 * N and a.members == ["first", "second"]
        assert a.find("k010") == 10 and a.find("k010", member="second") == N + 10
        assert a.member_of(N + 10) == ("second", 10)
        assert a.find_all("k010") == [(10, None), (N + 10, None)]
        assert a.read(N + 10, "rgb") == eb[(10, "rgb")] and a.read(10, "rgb") == ea[(10, "rgb")]
    coll = os.path.join(d, "set.pzpd")
    pzpdir.collection_write(coll, [pa, pb], aliases=["first", "second"])
    with pzpdir.open(coll) as a:
        assert len(a) == 2 * N and a.member_range("second") == (N, N)
        assert len(a.global_table("joints", member="second")) == 3


def test_recovery_and_edits():
    d = fresh("edit")
    path = os.path.join(d, "a.pzpd")
    exp = build(path)
    manifest = open(path, "rb").read()
    os.unlink(path)
    shards = sorted(os.path.join(d, f) for f in os.listdir(d) if f.startswith("a.0"))
    pzpdir.rebuild_manifest(path, shards)
    assert open(path, "rb").read() == manifest
    # salvage a shard whose index is zeroed
    with pzpdir.open(path) as a:
        sh = a.shards()[2]
    raw = bytearray(open(sh["path"], "rb").read())
    first = min(o for o in range(4096, len(raw), 4096) if raw[o:o + 8] == b"PZPDSECT")
    raw[0:4096] = bytes(4096)
    raw[first:] = bytes(len(raw) - first)
    open(sh["path"] + ".damaged", "wb").write(raw)
    with pzpdir.open(path) as a:
        recs, info = pzpdir.salvage(sh["path"] + ".damaged", schemas=a)
    assert info["records"] == sh["record_count"] == len(recs) and info["damaged_blobs"] == 0 and info["streams"] == STREAMS
    for r in recs:
        i = int(r["key"][1:])
        for b in r["blobs"]:
            assert b["intact"] and b["data"] == exp[(i, b["stream_name"])]
        persons = [t for t in r["tables"] if t["name"] == "persons"]
        assert len(persons) == (1 if i % 3 else 0)
    # edits
    rows = {"k%03d" % i: "%d.5,%d" % (i, -i) for i in range(0, N, 2)}
    rows["nosuchkey"] = "1,2"
    assert pzpdir.edit_table(path, "add", "score", rows, schema="a:f32 b:i32") == 1
    with pzpdir.open(path) as a:
        assert a.tables == ["joints", "persons", "vec", "score"]
        assert a.table(4, "score")["b"][0] == -4 and len(a.table(5, "score")) == 0
    freed_before = pzpdir.edit_table(path, "replace", "score", {"k001": "9.25,9"}, keep_missing=True)
    assert freed_before == 0
    assert pzpdir.compact(path) > 0
    newf = os.path.join(d, "n.bin")
    open(newf, "wb").write(b"new depth")
    assert pzpdir.edit_stream(path, "replace", "depth", [("k001", newf, "depth2/k001.raw")]) == 0
    with pzpdir.open(path) as a:
        assert a.read(1, "depth") == b"new depth" and a.info(1, "depth")["name"] == "depth2/k001.raw"
        assert a.read(2, "depth") == exp[(2, "depth")] and a.table(1, "score")["b"][0] == 9 and a.table(4, "score")["b"][0] == -4
        assert a.verify()
    pzpdir.edit_stream(path, "drop", "depth")
    with pzpdir.open(path) as a:
        assert a.streams == ["rgb"] and a.read(10, "rgb") == exp[(10, "rgb")]


def test_groups():
    d = fresh("groups")
    path = os.path.join(d, "g.pzpd")
    frames = {}
    with pzpdir.Writer(path, ["rgb", "depth"], align=64, shard_size="16K") as w:
        for i in range(5):
            with w.record("still%d" % i):
                w.add("rgb", "s%d" % i, payload(i, 0))
        for c, n in enumerate((12, 40)):
            gid = w.group("clip %d" % c, bytes_hint=n * 2000)
            for f in range(n):
                with w.record("c%d/%03d" % (c, f), group=gid, frame=f * 3):
                    w.add("rgb", "c%d/rgb/%03d" % (c, f), payload(1000 * c + f, 0))
                    if f % 5:
                        w.add("depth", "c%d/depth/%03d" % (c, f), payload(1000 * c + f, 1))
                frames[(c, f)] = (payload(1000 * c + f, 0), payload(1000 * c + f, 1) if f % 5 else None)
    with pzpdir.open(path) as a:
        first = a.group_find("clip 1")
        assert first == 17 and a.group(first + 7) == {"first": 17, "frames": 40, "id": 1, "index": 7, "name": "clip 1"}
        assert a.group(2) is None and a.info(first + 7, "rgb")["frame"] == 21
        fr = a.read_group("clip 1", ["rgb", "depth"])
        assert len(fr) == 40
        for f, rec in enumerate(fr):
            assert bytes(rec["rgb"]) == frames[(1, f)][0]
            assert (frames[(1, f)][1] is None and "depth" not in rec) or bytes(rec["depth"]) == frames[(1, f)][1]
        part = a.read_group(first + 3, ["rgb"], start=10, count=5)
        assert [bytes(r["rgb"]) for r in part] == [frames[(1, f)][0] for f in range(10, 15)]
        shards = {a.info(first + f, "rgb")["shard"] for f in range(40)}
        assert len(shards) == 1
        try:
            a.read_range(first - 1, 2)                          # crosses into the group
            assert False
        except pzpdir.PzpdError:
            pass
        try:
            a.group_find("nope")
            assert False
        except KeyError:
            pass


def test_errors():
    try:
        pzpdir.open("/nonexistent/x.pzpd")
        assert False
    except pzpdir.PzpdError as e:
        assert e.code == -1 and "cannot open" in str(e)
    d = fresh("err")
    path = os.path.join(d, "a.pzpd")
    try:
        with pzpdir.Writer(path, ["x"]) as w:
            with w.record("k"):
                w.add("x", "n", b"1")
            with w.record("k"):                               # duplicate key
                w.add("x", "n2", b"2")
        assert False
    except pzpdir.PzpdError as e:
        assert e.code == -7
    assert not os.path.exists(path)                           # aborted: no manifest
    with pzpdir.Writer(path, ["x"]) as w:
        with w.record("k"):
            w.add("x", "n", b"1")
    with pzpdir.open(path) as a:
        try:
            a.read(0, "nope")
            assert False
        except KeyError:
            pass
        assert pzpdir.detect_format(open(__file__, "rb").read(), "t.py")["format"] == "TEXT"


def test_pzp_images():
    """read_image of native PZP blobs (needs the pzp package's libpzp to write one)."""
    try:
        import pzp
    except Exception:
        return
    d = fresh("pzpimg")
    src = os.path.join(d, "img.pzp")
    arr = (np.arange(24 * 16 * 3) % 251).astype(np.uint8).reshape(24, 16, 3)
    try:
        pzp.write(src, arr)
    except Exception:
        return
    path = os.path.join(d, "a.pzpd")
    with pzpdir.Writer(path, ["all"]) as w:
        with w.record("k"):
            w.add_file("all", "img.pzp", src)
    with pzpdir.open(path) as a:
        assert a.info(0, "all")["format"] in ("PZP ", "PZPC")
        img = a.read_image(0, "all")
        assert img.shape == arr.shape and (img == arr).all()


def test_words():
    """Word index (spec §3.7): Writer.words, per-source + merged sub-indexes, canonical view, reindex, tokenize."""
    d = fresh("words")
    path = os.path.join(d, "w.pzpd")
    caps = [[("vlm", "A dog and a Dog."), ("old", "two dogs")], [("vlm", "a puppy!")], [("vlm", "Cat; caf\u00e9 dog_1")], []]
    with pzpdir.Writer(path, ["txt"], shard_size=1) as w:
        t = w.table("descriptions", "source:str text:str")
        syn = w.table("synonyms", "word:str canonical:str", global_=True)
        w.global_rows_csv(syn, "dogs,dog\npuppy,dog\n")
        w.words("descriptions", "text", "source")
        for i, rows in enumerate(caps):
            with w.record("r%d" % i):
                w.add("txt", "r%d.txt" % i, b"x")
                for src, text in rows:
                    w.rows_csv(t, '%s,"%s"' % (src, text))
    assert pzpdir.tokenize("A Dog, caf\u00e9 \u212a dog_1") == ["a", "dog", "k", "dog_1"]
    with pzpdir.open(path) as a:
        assert a.word_indexes() == [("descriptions", "text", "source")]
        assert a.word_sources("descriptions.text") == ["old", "vlm"]
        with a.words("descriptions.text") as w:
            assert w.words == ["a", "and", "cat", "dog", "dog_1", "dogs", "puppy", "two"]
            assert w.records("dog").tolist() == [0] and int(w.count_per_word[w.find("dog")]) == 2
            assert w.words_of_record(3) == [] and w.covered == 4 and w.find("nope") == -1
        with a.words("descriptions.text", canonical=True) as w:
            i = w.find("dog")
            assert w.records(i).tolist() == [0, 1] and int(w.records_per_word[i]) == 2 and int(w.count_per_word[i]) == 4
            assert w.words_of_record(0) == ["a", "and", "dog", "two"]
        with a.words("descriptions.text", source="old") as w:
            assert w.words == ["dogs", "two"] and w.records("two").tolist() == [0]
        w = a.words("descriptions.text")
    assert not w._h                                            # closed with its archive
    pzpdir.reindex(path, "descriptions.text", drop=True)
    with pzpdir.open(path) as a:
        assert a.word_indexes() == []
    pzpdir.reindex(path, "descriptions.text")                  # no source column now: merged only
    with pzpdir.open(path) as a:
        assert a.word_sources("descriptions.text") == [] and len(a.words("descriptions.text")) == 8
        assert a.verify()


def test_histograms():
    """scripts/pzpdir_histograms.py (spec §3.8) against tests/check_histograms.py's independent recomputation."""
    import subprocess
    from PIL import Image
    d = fresh("hist")
    path = os.path.join(d, "h.pzpd")
    r = np.random.default_rng(5)

    def png(arr):
        b = io.BytesIO()
        Image.fromarray(arr).save(b, format="PNG")
        return b.getvalue()
    with pzpdir.Writer(path, ["rgb", "lab", "dep"], align=64, shard_size="16K") as w:
        for i in range(24):
            with w.record("k%d" % i):
                hw = (9 + i, 13 + i)
                if i != 3:
                    w.add("rgb", "k%d.png" % i, png(r.integers(0, 256, hw + (3,), dtype=np.uint8)))
                w.add("lab", "k%d.l.png" % i, png(r.integers(0, 40, hw, dtype=np.uint8)))
                w.add("dep", "k%d.d.png" % i, png(r.integers(0, 65536, hw, dtype=np.uint16)))
    env = dict(os.environ, PYTHONPATH=os.path.join(HERE, "..", ".."))
    run = lambda *a: subprocess.run([sys.executable] + list(a), env=env, capture_output=True, text=True)
    t = run(os.path.join(HERE, "..", "scripts", "pzpdir_histograms.py"), path, "--rgb", "rgb", "--seg", "lab", "--depth", "dep", "--workers", "2")
    assert t.returncode == 0, t.stderr
    c = run(os.path.join(HERE, "check_histograms.py"), path, "--rgb", "rgb", "--seg", "lab", "--depth", "dep")
    assert c.returncode == 0, c.stdout + c.stderr
    with pzpdir.open(path) as a:
        idx, rows = a.table_all("hist_rgb")
        assert idx[4] - idx[3] == 0 and len(rows) == 23 and rows["h"].dtype == np.uint16    # record 3 has no rgb
        g = a.global_table("hist_depth_global")
        assert int(g["files"][0]) == 24 and abs(int(g["h"][0].astype(np.int64).sum()) - 65535) <= 128
        assert a.verify()


if __name__ == "__main__":
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith("test_") and callable(f)]
    failed = 0
    for name, fn in tests:
        try:
            fn()
            print("\033[32mok\033[0m   %s" % name)
        except Exception as e:                                 # report every test, not just the first failure
            import traceback
            traceback.print_exc()
            print("\033[31mFAIL\033[0m %s: %s" % (name, e))
            failed += 1
    print("%d Python tests, %d failed" % (len(tests), failed))
    sys.exit(1 if failed else 0)
