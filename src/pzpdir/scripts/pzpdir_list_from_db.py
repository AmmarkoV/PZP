#!/usr/bin/env python3
"""
Example "smart writer" for pzpdir: turn a Y-MAP-Net .db dataset into a pzpdir record list.

The .db file (DB1 format) defines the sample list and its order; each sample becomes one
record whose key is the DB's imagePath, so ordinal i == today's sample number i. For every
stream a template says where that sample's file lives, relative to the dataset root.
Several alternatives can be given with '|'; the first existing file wins, and a sample with
none of them simply has no blob in that stream.

Placeholders: {file} = imagePath (e.g. 000000000139.jpg), {stem} = imagePath up to the
first dot (000000000139).

Example (COCO val2017, the defaults):
    python3 pzpdir_list_from_db.py coco/cocoVal.db coco/cache/coco > val2017.tsv
    pzpdir pack coco_val2017.pzpd val2017.tsv

Annotation tables (phase 1c) replace the .db and descriptor files:
    joints            global  name:str parent:u16          (.db header)
    image             record  width:u16 height:u16         (.db "w,h,persons" line)
    persons           record  id:u16 bbox:u16[4] kp:u16[3J] (.db "SKn,..." lines, all coordinates u16)
    descriptions      record  source:str text:str          (--descriptions SOURCE=FILE.json, caption *text*;
                                                             the .db token IDs are not migrated)
    descriptor_MODEL  record  v:f32[D], bulk               (--descriptors MODEL=FILE, D read from the file)
Descriptions and descriptors are matched to samples by file name (basename of imagePath).

Repository : https://github.com/AmmarkoV/PZP
Author     : Ammar Qammaz (AmmarkoV)
"""

import argparse
import json
import os
import sys

# Stream order = on-disk order within a record: rgb+all+geo is the DataLoader's usual read
# set and forms a contiguous prefix; depth+seg (read only without a combined file) follow.
COCO_VAL_STREAMS = [
    ("rgb",   "val2017/{file}"),
    ("all",   "all_val2017/{stem}.pzp|all_val2017/{stem}.png"),
    ("geo",   "geo_val2017/{file}.pzp"),
    ("depth", "depth_val2017/{stem}.pzp|depth_val2017/{stem}.png|depth_val2017/{stem}_depth.png"),
    ("seg",   "segment_val2017/{stem}.pzp|segment_val2017/{stem}.png"),
]


def read_db(db_path):
    """
    Parse a DB1 file.

    Parameters
    ----------
    db_path : str
        Path of the .db file.

    Returns
    -------
    tuple
        (joint_names, joint_parents, samples) with samples a list of
        (imagePath, width, height, [(id, [bbox x4], [kp x3J]), ...]).
    """
    with open(db_path, "r") as f:
        lines = f.read().split("\n")
    if lines[0].strip() != "DB1":
        raise ValueError("%s: not a DB1 file (%r)" % (db_path, lines[0][:20]))
    n, J = int(lines[1]), int(lines[2])
    names = [lines[3 + j].rstrip("\n") for j in range(J)]
    parents = [int(lines[3 + J + j]) for j in range(J)]
    i = 3 + 2 * J
    samples = []
    for _ in range(n):
        image = lines[i].strip()
        w, h, persons = (int(x) for x in lines[i + 1].split(","))
        people = []
        for k in range(persons):
            f = lines[i + 3 + k].strip().split(",")
            if not f[0].startswith("SK"):
                raise ValueError("%s: expected SK line for %s, got %r" % (db_path, image, f[0]))
            people.append((int(f[0][2:]), [int(x) for x in f[1:5]], [int(x) for x in f[5:]]))
        samples.append((image, w, h, people))
        i += 3 + persons
    return names, parents, samples


def read_descriptors(path):
    """
    Read a DataLoader descriptor file (12-byte header with value_count, or the legacy 8-byte header
    whose value count is inferred so the entries end exactly at EOF, as descriptorConverter.h does).

    Parameters
    ----------
    path : str
        Descriptor file (e.g. cocoVal.db.dinov2).

    Returns
    -------
    tuple
        (D, {filename: raw little-endian float32 bytes})
    """
    import struct
    data = open(path, "rb").read()
    count, _maxname = struct.unpack_from("<ii", data, 0)
    value_count = struct.unpack_from("<i", data, 8)[0]
    starts = []
    if 0 < value_count <= 8192:
        starts.append((12, value_count))
    for cand in (768, 1024, 1280, 384, 512, 1536, 2048, 300):
        starts.append((8, cand))
    for start, D in starts:
        pos, out, ok = start, {}, True
        for _ in range(count):
            if pos >= len(data):
                ok = False
                break
            nl = data[pos]
            name = data[pos + 1:pos + 1 + nl].decode("utf-8", "surrogateescape")
            vals = data[pos + 1 + nl:pos + 1 + nl + 4 * D]
            if len(vals) != 4 * D:
                ok = False
                break
            out[name] = vals
            pos += 1 + nl + 4 * D
        if ok and pos == len(data):
            return D, out
    raise ValueError("%s: can't determine the descriptor length" % path)


def f32_text(raw):
    """Shortest decimal text of a little-endian float32 that reads back bit-exactly (as pzpdir writes it)."""
    import struct
    v = struct.unpack("<f", raw)[0]
    for p in range(1, 10):
        t = "%.*g" % (p, v)
        if struct.pack("<f", float(t)) == raw:
            return t
    return repr(v)


def csv_field(s):
    """RFC 4180 quoting for a string field."""
    if s == "" or any(c in s for c in ',"\n\r') or s[0] == " " or s[-1] == " ":
        return '"' + s.replace('"', '""') + '"'
    return s



def escape(s, key=False):
    """
    Escape a field for a pzpdir record list (tab, newline, backslash; a leading # or @ in a key).

    Parameters
    ----------
    s : str
        Field text.
    key : bool
        True for the first field of a line.

    Returns
    -------
    str
    """
    s = s.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")
    if key and s[:1] in ("#", "@"):
        s = "\\" + s
    return s


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("db", help="DB1 .db file (sample list and order)")
    ap.add_argument("root", help="dataset root the templates are relative to")
    ap.add_argument("--stream", action="append", default=[], metavar="NAME=TEMPLATE",
                    help="stream name and file template(s); repeat in on-disk order (default: COCO val2017 layout)")
    ap.add_argument("--absolute", action="store_true", help="store absolute paths as blob names instead of root-relative ones")
    ap.add_argument("--no-tables", action="store_true", help="emit the image streams only")
    ap.add_argument("--descriptions", action="append", default=[], metavar="SOURCE=FILE.json",
                    help="caption texts (JSON {path-or-filename: text}); repeat for several sources")
    ap.add_argument("--descriptors", action="append", default=[], metavar="MODEL=FILE",
                    help="descriptor file (e.g. dinov2=cocoVal.db.dinov2); one bulk table per model")
    args = ap.parse_args()

    streams = [tuple(s.split("=", 1)) for s in args.stream] if args.stream else COCO_VAL_STREAMS
    root = os.path.abspath(args.root)
    names, parents, samples = read_db(args.db)
    paths = [smp[0] for smp in samples]
    coverage = {name: 0 for name, _ in streams}
    out = sys.stdout
    out.write("# pzpdir record list from %s (%d samples)\n" % (args.db, len(paths)))

    tables = not args.no_tables
    captions, descs = [], []
    if tables:
        J = len(names)
        out.write("@global joints name:str parent:u16\n")
        for n_, p_ in zip(names, parents):
            out.write("@row joints %s,%d\n" % (escape(csv_field(n_)), p_))
        out.write("@table image width:u16 height:u16\n")
        out.write("@table persons id:u16 bbox:u16[4] kp:u16[%d]\n" % (3 * J))
        if args.descriptions:
            out.write("@table descriptions source:str text:str\n")
        for spec in args.descriptions:
            src, fn = spec.split("=", 1)
            d = json.load(open(fn), strict=False)
            captions.append((src, {os.path.basename(k): v for k, v in d.items()}))
        for spec in args.descriptors:
            model, fn = spec.split("=", 1)
            D, vecs = read_descriptors(fn)
            out.write("@table descriptor_%s v:f32[%d] bulk\n" % (model, D))
            descs.append((model, vecs))
            sys.stderr.write("descriptor_%s: D = %d, %d entries\n" % (model, D, len(vecs)))

    for image, w, h, people in samples:
        stem = os.path.basename(image).split(".", 1)[0]
        wrote = False
        for name, template in streams:
            for alt in template.split("|"):
                rel = alt.format(file=image, stem=stem)
                src = os.path.join(root, rel)
                if os.path.isfile(src):
                    stored = src if args.absolute else rel
                    out.write("%s\t%s\t%s\t%s\n" % (escape(image, True), name, escape(src), escape(stored)))
                    coverage[name] += 1
                    wrote = True
                    break
        if not wrote:
            sys.stderr.write("warning: sample %s has no file in any stream, skipped\n" % image)
            continue
        if tables:
            key = escape(image, True)
            out.write("%s\timage\t%d,%d\n" % (key, w, h))
            for pid, bbox, kp in people:
                if len(kp) != 3 * len(names):
                    raise ValueError("%s: person %d has %d keypoint values, expected %d" % (image, pid, len(kp), 3 * len(names)))
                out.write("%s\tpersons\t%s\n" % (key, ",".join(str(x) for x in [pid] + bbox + kp)))
            base = os.path.basename(image)
            for src, d in captions:
                if base in d:
                    out.write("%s\tdescriptions\t%s\n" % (key, escape(csv_field(src) + "," + csv_field(d[base]))))
            for model, vecs in descs:
                raw = vecs.get(base)
                if raw is not None:
                    out.write("%s\tdescriptor_%s\t%s\n" % (key, model, ",".join(f32_text(raw[4 * j:4 * j + 4]) for j in range(len(raw) // 4))))
    for name, _ in streams:
        sys.stderr.write("%-6s %d / %d\n" % (name, coverage[name], len(paths)))


if __name__ == "__main__":
    main()
