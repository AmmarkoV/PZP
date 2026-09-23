#!/bin/bash
# CLI tests for pzpdir: directory and record-list round trips with hostile file names,
# escapes, unsafe-name refusal on unpack, record-list error reporting, collections and tables.
# Usage: tests/run_cli_tests.sh [path-to-pzpdir-binary]   (default ./dpzpdir)
# Scratch: $PZPDIR_TEST_DIR (default /tmp/pzpdir_test)

BIN=`realpath ${1:-./dpzpdir}`
T=${PZPDIR_TEST_DIR:-/tmp/pzpdir_test}/cli
rm -rf "$T"
mkdir -p "$T"
FAIL=0
ok()   { echo -e "\033[32mok\033[0m   $1"; }
bad()  { echo -e "\033[31mFAIL\033[0m $1"; FAIL=$((FAIL+1)); }

#-----------------------------------------------------------------------------------------
# 1. Hostile file names on disk, packed as a directory and unpacked again
#-----------------------------------------------------------------------------------------
python3 - "$T/tree" <<'EOF'
import os, sys, random
root = sys.argv[1]
random.seed(7)
names = ["plain.jpg", "with space.png", "δείγμα ✓.jpg", "a.b.c.tar.gz.pzp", "#hash", "@at", "back\\slash",
         "tab\there", "new\nline", "\u00e9\u0301 combining", "x" * 200]
deep = "/".join("d%d" % i for i in range(30))
for i, n in enumerate(names):
    for sub in ["", "sub dir/", deep + "/"]:
        p = os.path.join(root, sub + n)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(os.urandom(random.randint(0, 5000)))
open(os.path.join(root, "empty"), "wb").close()
EOF
if "$BIN" pack "$T/dir.pzpd" "$T/tree" --shard-size 16K >/dev/null 2>"$T/err" ; then ok "pack directory"; else bad "pack directory: `cat $T/err`"; fi
if "$BIN" verify "$T/dir.pzpd" --blobs 2>/dev/null ; then ok "verify --blobs"; else bad "verify"; fi
if "$BIN" unpack "$T/dir.pzpd" "$T/dir_out" 2>/dev/null && diff -r "$T/tree" "$T/dir_out" ; then ok "unpack == original tree (diff -r)"; else bad "unpack differs"; fi
N=`find "$T/tree" -type f -print0 | tr -dc "\\0" | wc -c`
M=`"$BIN" ls "$T/dir.pzpd" | wc -l`
if [ "$N" = "$M" ]; then ok "ls lists $M records"; else bad "ls lists $M records, expected $N"; fi
if "$BIN" cat "$T/dir.pzpd" "$(printf 'sub dir/new\nline')" | cmp - "$T/tree/sub dir/new
line" ; then ok "cat a key containing a newline"; else bad "cat newline key"; fi

#-----------------------------------------------------------------------------------------
# 2. A record list: two streams from two different trees, stored names, groups, escapes
#-----------------------------------------------------------------------------------------
python3 - "$T" <<'EOF'
import os, sys
T = sys.argv[1]
def esc(s, key=False):
    s = s.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")
    if key and s[:1] in "#@": s = "\\" + s
    return s
os.makedirs(T + "/A", exist_ok=True); os.makedirs(T + "/elsewhere/B", exist_ok=True)
lines, expect = ["# test list"], []
for i in range(40):
    key = ["k%d" % i, "#k%d" % i, "@k%d" % i, "tab\tk%d" % i, "nl\nk%d" % i][i % 5]
    a = T + "/A/%d.bin" % i
    b = T + "/elsewhere/B/%d.dat" % i
    open(a, "wb").write(os.urandom(100 + i))
    open(b, "wb").write(os.urandom(50 + i))
    if i == 20: lines.append("@group clip one")
    if i == 30: lines.append("@group")
    lines.append("\t".join([esc(key, True), "rgb", esc(a), esc("stored/rgb %d\t.bin" % i)]))
    expect.append((key, "rgb", "stored/rgb %d\t.bin" % i))
    if i % 4 != 0:
        lines.append("\t".join([esc(key, True), "depth", esc(b)]))
        expect.append((key, "depth", b))
open(T + "/list.tsv", "w").write("\n".join(lines) + "\n")
open(T + "/expect.tsv", "w").write("".join("\t".join([esc(k, True), s, esc(n)]) + "\n" for k, s, n in expect))
EOF
if "$BIN" pack "$T/list.pzpd" "$T/list.tsv" --align 64 >/dev/null 2>"$T/err"; then ok "pack record list"; else bad "pack list: `cat $T/err`"; fi
"$BIN" ls "$T/list.pzpd" --names > "$T/got.tsv"
if cmp -s "$T/got.tsv" "$T/expect.tsv"; then ok "ls --names round-trips keys, streams and stored names (escaped)"; else bad "ls --names differs"; diff "$T/got.tsv" "$T/expect.tsv" | head; fi
if "$BIN" cat "$T/list.pzpd" "$(printf 'stored/rgb 3\t.bin')" | cmp - "$T/A/3.bin"; then ok "cat by stored name with a tab"; else bad "cat stored name"; fi
if "$BIN" cat "$T/list.pzpd" "k5" --stream depth | cmp - "$T/elsewhere/B/5.dat"; then ok "cat by key + stream"; else bad "cat key+stream"; fi
if ! "$BIN" cat "$T/list.pzpd" "k5" >/dev/null 2>&1; then ok "cat of a 2-blob record without --stream is refused"; else bad "cat ambiguity"; fi

#-----------------------------------------------------------------------------------------
# 3. Unsafe stored names: packed fine, refused by unpack, nothing written outside
#-----------------------------------------------------------------------------------------
echo x > "$T/src"
printf 'a\tdata\t%s\t../escape\nb\tdata\t%s\t/abs/escape\nc\tdata\t%s\tdir//x\nd\tdata\t%s\t./dot\ne\tdata\t%s\tfine/name\n' "$T/src" "$T/src" "$T/src" "$T/src" "$T/src" > "$T/unsafe.tsv"
"$BIN" pack "$T/unsafe.pzpd" "$T/unsafe.tsv" >/dev/null 2>&1 || bad "pack unsafe names"
mkdir -p "$T/u/inner"
if ! "$BIN" unpack "$T/unsafe.pzpd" "$T/u/inner" >/dev/null 2>"$T/err"; then ok "unpack exits non-zero on unsafe names"; else bad "unpack accepted unsafe names"; fi
if [ ! -e "$T/u/escape" ] && [ ! -e /abs/escape ] && [ -f "$T/u/inner/fine/name" ] && [ `grep -c refusing "$T/err"` = 4 ]; then ok "4 unsafe names refused, safe one written, nothing outside"; else bad "unsafe name handling: `cat $T/err`"; fi

#-----------------------------------------------------------------------------------------
# 4. Record-list errors carry line numbers
#-----------------------------------------------------------------------------------------
expect_fail() { # $1 description, $2 list content, $3 expected message fragment, $4.. extra pack options
    printf "$2" > "$T/e.tsv"
    local desc="$1" frag="$3"; shift 3
    if "$BIN" pack "$T/e.pzpd" "$T/e.tsv" "$@" >/dev/null 2>"$T/err"; then bad "$desc: accepted"; return; fi
    set -- "$desc" "" "$frag"
    if grep -q "$3" "$T/err"; then ok "$1"; else bad "$1: `cat $T/err`"; fi
}
expect_fail "bad escape reported with its line"       "k\tdata\t$T/src\n#c\nk2\tdata\t$T/sr\\\\q\n" "e.tsv:3: bad escape"
expect_fail "too few fields"                           "k\tdata\n" "e.tsv:1: expected"
expect_fail "unknown directive"                        "@frobnicate\n" "unknown directive"
expect_fail "table declared after the first record"    "k\tdata\t$T/src\n@table p id:u16\n" "e.tsv:2: tables must be declared before"
expect_fail "bad schema reported with its line"        "#c\n@table p id:u17\nk\tdata\t$T/src\n" "e.tsv:2:"
expect_fail "duplicate table name"                     "@table p id:u16\n@global p n:str\n" "e.tsv:2: .p. is already declared"
expect_fail "@row for a record table"                  "@table p id:u16\n@row p 1\n" "e.tsv:2: .p. is not a declared @global"
expect_fail "row with too few fields, with its line"   "@table p a:u16 b:u16\nk\tdata\t$T/src\nk\tp\t1\n" "e.tsv:3: .*too few"
expect_fail "row out of range, with its line"          "@table p a:u8\nk\tp\t300\nk\tdata\t$T/src\n" "e.tsv:2: .*out of range"
expect_fail "row line with a stored-name field"        "@table p a:u8\nk\tp\t3\tx\n" "e.tsv:2: a row line is"
expect_fail "missing source file"                      "k\tdata\t$T/nope\n" "e.tsv:1: cannot open"
expect_fail "non-consecutive duplicate key"            "k\tdata\t$T/src\tn1\nj\tdata\t$T/src\tn2\nk\tdata\t$T/src\tn3\n" "already exists"
expect_fail "unknown stream with --streams"            "k\tother\t$T/src\n" "unknown or invalid stream" --streams data

#-----------------------------------------------------------------------------------------
# 5. Collections: collect, alias-prefixed ls, unpack into alias dirs, --dups, multi-archive reads, --refresh
#-----------------------------------------------------------------------------------------
mkdir -p "$T/c1" "$T/c2"
for i in 1 2 3 4 5; do head -c $((100+i)) /dev/urandom > "$T/c1/x$i"; done
for i in 3 4 5 6 7 8; do head -c $((200+i)) /dev/urandom > "$T/c2/x$i"; done
"$BIN" pack "$T/one.pzpd" "$T/c1" >/dev/null 2>&1 && "$BIN" pack "$T/two.pzpd" "$T/c2" >/dev/null 2>&1 || bad "pack collection members"
if "$BIN" collect "$T/set.pzpd" first="$T/one.pzpd" second="$T/two.pzpd" >/dev/null 2>"$T/err"; then ok "collect with aliases"; else bad "collect: `cat $T/err`"; fi
if [ "`"$BIN" ls "$T/set.pzpd" | wc -l`" = 11 ] && [ "`"$BIN" ls "$T/set.pzpd" | head -1`" = "`printf 'first\tx1'`" ] && [ "`"$BIN" ls "$T/set.pzpd" | tail -1`" = "`printf 'second\tx8'`" ]; then ok "ls of a collection: 11 records, alias-prefixed"; else bad "ls collection: `"$BIN" ls "$T/set.pzpd" | head -3`"; fi
if [ "`"$BIN" info "$T/set.pzpd" --dups 2>/dev/null | wc -l`" = 3 ] && "$BIN" info "$T/set.pzpd" --dups 2>/dev/null | grep -q "^x4	first:3,second:6$"; then ok "info --dups lists the 3 shared keys with their members"; else bad "dups: `"$BIN" info "$T/set.pzpd" --dups 2>&1`"; fi
rm -rf "$T/cu"
if "$BIN" unpack "$T/set.pzpd" "$T/cu" >/dev/null 2>&1 && diff -r "$T/c1" "$T/cu/first" && diff -r "$T/c2" "$T/cu/second"; then ok "unpack writes each member into <dir>/<alias>/"; else bad "unpack collection"; fi
if "$BIN" cat "$T/set.pzpd" x4 | cmp - "$T/c1/x4" && "$BIN" cat "$T/one.pzpd" "$T/two.pzpd" x7 | cmp - "$T/c2/x7"; then ok "cat: first member wins; several archives given directly"; else bad "cat collection"; fi
if [ "`"$BIN" ls "$T/one.pzpd" "$T/two.pzpd" | wc -l`" = 11 ] && "$BIN" verify "$T/one.pzpd" "$T/two.pzpd" --blobs 2>/dev/null; then ok "ls / verify on several archives given directly"; else bad "multi-archive ls/verify"; fi
if "$BIN" info "$T/set.pzpd" 2>/dev/null | grep -q "member 1  second  records 5..11"; then ok "info shows members and their ranges"; else bad "info members: `"$BIN" info "$T/set.pzpd" 2>&1 | head -8`"; fi
head -c 300 /dev/urandom > "$T/c2/x9"
"$BIN" pack "$T/two.pzpd" "$T/c2" >/dev/null 2>&1
if ! "$BIN" ls "$T/set.pzpd" >/dev/null 2>"$T/err" && grep -q "refresh" "$T/err"; then ok "rebuilt member makes the collection stale (message suggests --refresh)"; else bad "stale not detected: `cat $T/err`"; fi
if "$BIN" collect --refresh "$T/set.pzpd" >/dev/null 2>&1 && [ "`"$BIN" ls "$T/set.pzpd" | wc -l`" = 12 ]; then ok "collect --refresh"; else bad "refresh"; fi
mv "$T/one.pzpd" "$T/one.hidden"
if "$BIN" info "$T/set.pzpd" 2>/dev/null | grep -q "member 0  first  records 0..5  UNAVAILABLE" && "$BIN" cat "$T/set.pzpd" x8 | cmp - "$T/c2/x8"; then ok "missing member: range kept, reported UNAVAILABLE, other member readable"; else bad "missing member: `"$BIN" info "$T/set.pzpd" 2>&1 | head -8`"; fi
mv "$T/one.hidden" "$T/one.pzpd"

#-----------------------------------------------------------------------------------------
# 6. Tables: pack a list with global / record / bulk tables, cat --table, info, and
#    export-table -> re-pack -> export-table round trip (hostile strings, float bits)
#-----------------------------------------------------------------------------------------
python3 - "$T" <<'EOF'
import os, sys, random, struct
T = sys.argv[1]
random.seed(11)
def esc(s, key=False):
    s = s.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")
    if key and s[:1] in "#@": s = "\\" + s
    return s
def q(s): return '"' + s.replace('"', '""') + '"'
os.makedirs(T + "/tb", exist_ok=True)
decl = ["@global joints name:str parent:u16",
        "@table persons id:u16 bbox:u16[4] score:f32",
        "@table text source:str text:str",
        "@table vec v:f32[3] bulk"]
lines = ["# tables"] + decl
for n, p in [("head", 0), ("left, eye", 0), ('quo"te', 1), ("new\nline\ttab", 2), ("", 3), ("δείγμα", 1)]:
    lines.append("@row joints " + esc(q(n) + ",%d" % p))
for i in range(60):
    key = ["r%d" % i, "#r%d" % i, "tab\tr%d" % i][i % 3]
    src = T + "/tb/%d.bin" % i
    open(src, "wb").write(os.urandom(10 + i))
    lines.append("%s\tdata\t%s" % (esc(key, True), esc(src)))
    for k in range(i % 4):
        f = struct.unpack("<f", struct.pack("<f", random.uniform(-1e6, 1e6)))[0]
        lines.append("%s\tpersons\t%d,%d,%d,%d,%d,%r" % (esc(key, True), k, i, i + 1, 65535, 0, f))
    if i % 5 != 4:
        lines.append("%s\ttext\t%s" % (esc(key, True), esc("src%d,%s" % (i % 2, q("caption %d, with \"quotes\"\nand\ttabs ✓" % i)))))
    if i % 2 == 0:
        lines.append("%s\tvec\t%s" % (esc(key, True), ",".join(repr(x) for x in (1e-38 * i, -0.0, 3.4e38))))
open(T + "/tables.tsv", "w").write("\n".join(lines) + "\n")
open(T + "/decl.tsv", "w").write("\n".join(decl) + "\n")
EOF
if "$BIN" pack "$T/tb.pzpd" "$T/tables.tsv" --shard-size 8K >/dev/null 2>"$T/err" && "$BIN" verify "$T/tb.pzpd" --blobs 2>/dev/null; then ok "pack + verify a list with global, record and bulk tables (small shards)"; else bad "pack tables: `cat $T/err`"; fi
if "$BIN" info "$T/tb.pzpd" 2>/dev/null | grep -q "^table joints  global  6 rows" && "$BIN" info "$T/tb.pzpd" 2>/dev/null | grep -q "^table persons  record  90 rows  16 B/row  id:u16 bbox:u16\[4\] score:f32$" && "$BIN" info "$T/tb.pzpd" 2>/dev/null | grep -q "^table vec  record bulk  30 rows"; then ok "info lists tables, row counts and schemas"; else bad "info tables: `"$BIN" info "$T/tb.pzpd" 2>&1 | grep ^table`"; fi
if [ "`"$BIN" cat "$T/tb.pzpd" r3 --table persons | wc -l`" = 3 ] && "$BIN" cat "$T/tb.pzpd" r3 --table persons | head -1 | grep -q "^0,3,4,65535,0," ; then ok "cat --table prints a record's rows as CSV"; else bad "cat --table: `"$BIN" cat "$T/tb.pzpd" r3 --table persons 2>&1`"; fi
if ! "$BIN" cat "$T/tb.pzpd" r3 --table nope >/dev/null 2>&1; then ok "cat --table of an unknown table fails"; else bad "cat unknown table accepted"; fi
for t in joints persons text vec; do "$BIN" export-table "$T/tb.pzpd" $t > "$T/exp1_$t" 2>"$T/err" || bad "export-table $t: `cat $T/err`"; done
( cat "$T/decl.tsv" "$T/exp1_joints"; grep -P '\tdata\t' "$T/tables.tsv"; cat "$T/exp1_persons" "$T/exp1_text" "$T/exp1_vec" ) > "$T/repack.tsv"
python3 - "$T" <<'EOF'
# The repack list must keep each record's lines consecutive: regroup by key in the original order.
import sys
T = sys.argv[1]
lines = open(T + "/repack.tsv").read().split("\n")
head = [l for l in lines if l.startswith("@") or l == ""]
order, by = [], {}
for l in lines:
    if l.startswith("@") or not l: continue
    k = l.split("\t", 1)[0]
    if k not in by: by[k] = []; order.append(k)
    by[k].append(l)
open(T + "/repack.tsv", "w").write("\n".join([h for h in head if h] + [l for k in order for l in by[k]]) + "\n")
EOF
if "$BIN" pack "$T/tb2.pzpd" "$T/repack.tsv" >/dev/null 2>"$T/err"; then ok "export-table output re-packs"; else bad "re-pack: `cat $T/err`"; fi
SAME=1
for t in joints persons text vec; do "$BIN" export-table "$T/tb2.pzpd" $t > "$T/exp2_$t" 2>/dev/null; cmp -s "$T/exp1_$t" "$T/exp2_$t" || SAME=0; done
if [ $SAME = 1 ]; then ok "export-table -> pack -> export-table is identical for all four tables"; else bad "table round trip differs"; fi
if [ "`grep -c . "$T/exp1_text"`" -ge 48 ] && grep -q '^tab\\tr2	text	src0,"caption 2, with ""quotes""\\nand\\ttabs ✓"$' "$T/exp1_text"; then ok "export-table escapes keys and CSV text"; else bad "export text: `head -3 $T/exp1_text`"; fi
if grep -q '^@row joints "new\\nline\\ttab",2$' "$T/exp1_joints" && grep -q '^@row joints "",3$' "$T/exp1_joints"; then ok "global rows exported as @row lines (quoted newline/tab, empty string)"; else bad "export joints: `cat $T/exp1_joints`"; fi
if grep -q '^r0	vec	0,-0,3.4e+38$' "$T/exp1_vec"; then ok "f32 shortest round-trip text (0, -0, 3.4e+38)"; else bad "vec text: `head -2 $T/exp1_vec`"; fi

#-----------------------------------------------------------------------------------------
# 7. Recovery: rebuild-manifest, superblock fallbacks in info, salvage -> pack round trip
#-----------------------------------------------------------------------------------------
rm -rf "$T/rec"; mkdir -p "$T/rec/src/sub dir"
for i in $(seq 0 39); do head -c $((500+i*37)) /dev/urandom > "$T/rec/src/sub dir/f$i.bin"; done
( cat "$T/decl.tsv"; for i in $(seq 0 39); do printf 'k%d\tdata\t%s\tsub dir/f%d.bin\n' $i "$T/rec/src/sub dir/f$i.bin" $i; for p in $(seq 1 $((i % 3))); do printf 'k%d\tpersons\t%d,1,2,3,4,%d.25\n' $i $p $i; done; printf 'k%d\ttext\ts,"line %d\\nwith, comma"\n' $i $i; done ) > "$T/rec/list.tsv"
"$BIN" pack "$T/rec/a.pzpd" "$T/rec/list.tsv" --shard-size 16K >/dev/null 2>"$T/err" || bad "pack recovery archive: `cat $T/err`"
cp "$T/rec/a.pzpd" "$T/rec/a.orig"; rm "$T/rec/a.pzpd"
if "$BIN" rebuild-manifest "$T"/rec/a.0*.pzpd >/dev/null 2>&1 && cmp -s "$T/rec/a.pzpd" "$T/rec/a.orig"; then ok "rebuild-manifest: byte-identical manifest"; else bad "rebuild-manifest"; fi
for t in persons text joints; do "$BIN" export-table "$T/rec/a.pzpd" $t > "$T/rec/before_$t" 2>/dev/null; done
"$BIN" unpack "$T/rec/a.pzpd" "$T/rec/before" >/dev/null 2>&1
python3 - "$T/rec/a.00001.pzpd" "$T/rec/a.00002.pzpd" <<'EOF'
import os, sys
with open(sys.argv[1], "r+b") as f: f.seek(40); f.write(b"damage")
sz = os.path.getsize(sys.argv[2])
with open(sys.argv[2], "r+b") as f:
    f.write(bytes(4096)); f.seek(sz - 4096); f.write(bytes(4096))
EOF
if "$BIN" info "$T/rec/a.pzpd" 2>/dev/null | grep -q "a.00001.pzpd.*RECOVERED (backup superblock)" && "$BIN" info "$T/rec/a.pzpd" 2>/dev/null | grep -q "a.00002.pzpd.*RECOVERED (superblock rebuilt from the index sections)" && "$BIN" verify "$T/rec/a.pzpd" --blobs 2>/dev/null; then ok "info reports both superblock fallbacks; verify --blobs passes"; else bad "fallbacks: `"$BIN" info "$T/rec/a.pzpd" 2>&1 | grep 'shard [12] '`"; fi
python3 - "$T/rec/a.00003.pzpd" <<'EOF'
import sys
p = sys.argv[1]; d = bytearray(open(p, "rb").read())
first = min(o for o in range(4096, len(d), 4096) if d[o:o+8] == b"PZPDSECT")
d[0:4096] = bytes(4096); d[first:] = bytes(len(d) - first)
open(p, "wb").write(d)
EOF
if ! "$BIN" verify "$T/rec/a.pzpd" >/dev/null 2>&1; then ok "a shard with a zeroed index fails verify"; else bad "zeroed index not detected"; fi
if "$BIN" salvage "$T/rec/a.00003.pzpd" "$T/rec/salv" --schemas "$T/rec/a.pzpd" --list "$T/rec/salv.tsv" >/dev/null 2>"$T/err"; then ok "salvage with --schemas"; else bad "salvage: `cat $T/err`"; fi
"$BIN" pack "$T/rec/s.pzpd" "$T/rec/salv.tsv" --streams data >/dev/null 2>"$T/err" || bad "re-pack salvage: `cat $T/err`"
"$BIN" unpack "$T/rec/s.pzpd" "$T/rec/after" >/dev/null 2>&1
SAME=1; N=0
for k in $("$BIN" ls "$T/rec/s.pzpd" 2>/dev/null); do
  N=$((N+1)); i=${k#k}
  cmp -s "$T/rec/before/sub dir/f$i.bin" "$T/rec/after/sub dir/f$i.bin" || SAME=0
  for t in persons text; do [ "`grep -P "^$k\t" "$T/rec/before_$t"`" = "`"$BIN" export-table "$T/rec/s.pzpd" $t 2>/dev/null | grep -P "^$k\t"`" ] || SAME=0; done
done
"$BIN" export-table "$T/rec/s.pzpd" joints > "$T/rec/after_joints" 2>/dev/null
if [ $SAME = 1 ] && [ $N -gt 0 ] && cmp -s "$T/rec/before_joints" "$T/rec/after_joints"; then ok "salvage -> pack restores the shard's $N records: bytes, names, persons / text rows, joints"; else bad "salvage round trip ($N records)"; fi
if "$BIN" salvage "$T/rec/a.00003.pzpd" "$T/rec/salv2" 2>&1 >/dev/null | grep -q "stream names from unknown"; then ok "salvage without --schemas: placeholder stream names reported"; else bad "salvage without schemas"; fi

#-----------------------------------------------------------------------------------------
# 8. Edits: table edits (no record data rewritten), stream edits (shard by shard), compact;
#    each interrupted by a simulated crash (PZPDIR_TEST_CRASH), then rerun -> same result as
#    the uninterrupted edit on a reference copy
#-----------------------------------------------------------------------------------------
E="$T/ed"; rm -rf "$E"; mkdir -p "$E/arc" "$E/ref" "$E/src"
"$BIN" pack "$E/arc/a.pzpd" "$T/rec/list.tsv" --shard-size 16K >/dev/null 2>&1 || bad "pack edit archive"
cp "$E"/arc/a.* "$E/ref/"
NSH=`ls "$E"/arc/a.0*.pzpd | wc -l`
( echo "@table text source:str text:str"; for i in $(seq 0 39); do printf 'k%d\ttext\tv2,"new caption %d, ""quoted"""\n' $i $i; done ) > "$E/text.tsv"
( echo "@table score v:f32[2] bulk"; for i in $(seq 0 39); do printf 'k%d\tscore\t%d.25,-%d\n' $i $i $i; done ) > "$E/score.tsv"
for i in $(seq 0 39); do head -c $((200+i*3)) /dev/urandom > "$E/src/d$i.png"; printf 'k%d\tdepth\t%s\tdepth/d%d.png\n' $i "$E/src/d$i.png" $i; done > "$E/depth.tsv"
python3 - "$E/arc" > "$E/data_before" <<'EOF'
import sys, glob, hashlib
# hash of every shard's record area (from 4 KiB to the first index section): table edits must not change it
for p in sorted(glob.glob(sys.argv[1] + "/a.0*.pzpd")):
    d = open(p, "rb").read()
    first = min(o for o in range(4096, len(d), 4096) if d[o:o+8] == b"PZPDSECT")
    print(p.split("/")[-1], hashlib.sha256(d[4096:first]).hexdigest())
EOF
shard_ok() { for f in "$1"/a.0*.pzpd; do "$BIN" verify "$f" --blobs >/dev/null 2>&1 || return 1; done; return 0; }
same_as_ref() { for t in $2; do cmp -s <("$BIN" export-table "$E/arc/a.pzpd" $t 2>&1) <("$BIN" export-table "$E/ref/a.pzpd" $t 2>&1) || return 1; done; cmp -s <("$BIN" ls "$E/arc/a.pzpd" --long 2>&1) <("$BIN" ls "$E/ref/a.pzpd" --long 2>&1); }

"$BIN" replace-table "$E/ref/a.pzpd" text "$E/text.tsv" >/dev/null 2>&1 && "$BIN" add-table "$E/ref/a.pzpd" score "$E/score.tsv" >/dev/null 2>&1 || bad "reference table edits"
PZPDIR_TEST_CRASH=flip:2 "$BIN" replace-table "$E/arc/a.pzpd" text "$E/text.tsv" >/dev/null 2>&1; RC=$?
if [ $RC = 99 ] && shard_ok "$E/arc"; then ok "replace-table killed between append and flip: every shard still verifies on its own"; else bad "crash in replace-table (rc $RC)"; fi
"$BIN" replace-table "$E/arc/a.pzpd" text "$E/text.tsv" >/dev/null 2>"$T/err" || bad "rerun replace-table: `cat $T/err`"
PZPDIR_TEST_CRASH=flip:3 "$BIN" add-table "$E/arc/a.pzpd" score "$E/score.tsv" >/dev/null 2>&1
"$BIN" add-table "$E/arc/a.pzpd" score "$E/score.tsv" >/dev/null 2>"$T/err" || bad "rerun add-table: `cat $T/err`"
if same_as_ref x "text score persons" && "$BIN" verify "$E/arc/a.pzpd" --blobs >/dev/null 2>&1; then ok "interrupted + rerun table edits = uninterrupted ones"; else bad "table edits after crash differ from the reference"; fi
python3 - "$E/arc" > "$E/data_after" <<'EOF'
import sys, glob, hashlib
for p in sorted(glob.glob(sys.argv[1] + "/a.0*.pzpd")):
    d = open(p, "rb").read()
    first = min(o for o in range(4096, len(d), 4096) if d[o:o+8] == b"PZPDSECT")
    print(p.split("/")[-1], hashlib.sha256(d[4096:first]).hexdigest())
EOF
if cmp -s "$E/data_before" "$E/data_after"; then ok "table edits rewrote no record data ($NSH shards: record areas byte-identical)"; else bad "record data changed by a table edit"; fi
if ! "$BIN" add-table "$E/arc/a.pzpd" text "$E/text.tsv" >/dev/null 2>"$T/err" && grep -q "already exists" "$T/err"; then ok "add-table of an existing table refused"; else bad "add existing table"; fi

S1=`du -sb "$E/arc" | cut -f1`
"$BIN" compact "$E/ref/a.pzpd" >/dev/null 2>&1 || bad "reference compact"
PZPDIR_TEST_CRASH=compact:2 "$BIN" compact "$E/arc/a.pzpd" >/dev/null 2>&1; RC=$?
if [ $RC = 99 ] && shard_ok "$E/arc" && same_as_ref x "text score persons"; then ok "compact killed between its two generations: archive intact"; else bad "crash in compact (rc $RC)"; fi
"$BIN" compact "$E/arc/a.pzpd" >/dev/null 2>"$T/err" || bad "rerun compact: `cat $T/err`"
S2=`du -sb "$E/arc" | cut -f1`; S3=`du -sb "$E/ref" | cut -f1`
if [ $S2 -lt $S1 ] && [ $S2 = $S3 ] && same_as_ref x "text score persons" && "$BIN" verify "$E/arc/a.pzpd" --blobs >/dev/null 2>&1; then ok "compact reclaims the dead sections ($S1 -> $S2 bytes, same as uninterrupted)"; else bad "compact: $S1 -> $S2 (reference $S3)"; fi

"$BIN" add-stream "$E/ref/a.pzpd" depth "$E/depth.tsv" >/dev/null 2>&1 || bad "reference add-stream"
PZPDIR_TEST_CRASH=shard:2 "$BIN" add-stream "$E/arc/a.pzpd" depth "$E/depth.tsv" >/dev/null 2>&1; RC=$?
if [ $RC = 99 ] && shard_ok "$E/arc"; then ok "add-stream killed after 2 of $NSH shards: every shard still verifies"; else bad "crash in add-stream (rc $RC)"; fi
"$BIN" add-stream "$E/arc/a.pzpd" depth "$E/depth.tsv" >/dev/null 2>"$T/err" || bad "rerun add-stream: `cat $T/err`"
if grep -q "match no record key" "$T/err"; then bad "rerun add-stream counted the done shards' files as unmatched: `cat $T/err`"; else ok "rerun add-stream still matches every file (done shards included)"; fi
rm -rf "$E/u1" "$E/u2"; "$BIN" unpack "$E/arc/a.pzpd" "$E/u1" >/dev/null 2>&1; "$BIN" unpack "$E/ref/a.pzpd" "$E/u2" >/dev/null 2>&1
if diff -r "$E/u1" "$E/u2" >/dev/null && same_as_ref x "text score persons" && "$BIN" verify "$E/arc/a.pzpd" --blobs >/dev/null 2>&1 && cmp -s "$E/u1/depth/d7.png" "$E/src/d7.png"; then ok "interrupted + rerun add-stream = uninterrupted (files, names, rows)"; else bad "add-stream after crash differs"; fi
for i in $(seq 0 19); do printf 'k%d\tdepth\t%s\tdepth2/e%d.png\n' $i "$E/src/d$((39-i)).png" $i; done > "$E/depth2.tsv"
"$BIN" replace-stream "$E/ref/a.pzpd" depth "$E/depth2.tsv" --missing drop >/dev/null 2>&1 || bad "reference replace-stream"
PZPDIR_TEST_CRASH=rewrite:1 "$BIN" replace-stream "$E/arc/a.pzpd" depth "$E/depth2.tsv" --missing drop >/dev/null 2>&1; RC=$?
EXTRA=`ls "$E/arc" | grep -c rewrite`
if [ $RC = 99 ] && [ $EXTRA = 1 ] && "$BIN" verify "$E/arc/a.pzpd" --blobs >/dev/null 2>&1; then ok "replace-stream killed before its first rename: archive unchanged, one extra shard on disk"; else bad "crash in replace-stream (rc $RC, $EXTRA extra files)"; fi
"$BIN" replace-stream "$E/arc/a.pzpd" depth "$E/depth2.tsv" --missing drop >/dev/null 2>"$T/err" || bad "rerun replace-stream: `cat $T/err`"
rm -rf "$E/u1" "$E/u2"; "$BIN" unpack "$E/arc/a.pzpd" "$E/u1" >/dev/null 2>&1; "$BIN" unpack "$E/ref/a.pzpd" "$E/u2" >/dev/null 2>&1
if diff -r "$E/u1" "$E/u2" >/dev/null && [ "`ls "$E/u1/depth2" | wc -l`" = 20 ] && [ ! -d "$E/u1/depth" ]; then ok "replace-stream --missing drop: 20 new depth files, the rest dropped; = uninterrupted"; else bad "replace-stream result differs"; fi
if ! "$BIN" add-stream "$E/arc/a.pzpd" rgb "$E/depth.tsv" >/dev/null 2>"$T/err" && grep -q "expected key" "$T/err"; then ok "a list for another stream is refused with its line"; else bad "stream list check: `cat $T/err`"; fi
printf 'k1\tdepth\t%s\tsub dir/f2.bin\n' "$E/src/d1.png" > "$E/clash.tsv"
if ! "$BIN" replace-stream "$E/arc/a.pzpd" depth "$E/clash.tsv" >/dev/null 2>"$T/err" && grep -q "already exists" "$T/err"; then ok "a new name that exists elsewhere in the archive is refused before anything is written"; else bad "name clash: `cat $T/err`"; fi
"$BIN" drop-stream "$E/arc/a.pzpd" depth >/dev/null 2>&1 && "$BIN" drop-stream "$E/ref/a.pzpd" depth >/dev/null 2>&1
rm -rf "$E/u1" "$E/u2"; "$BIN" unpack "$E/arc/a.pzpd" "$E/u1" >/dev/null 2>&1; "$BIN" unpack "$E/ref/a.pzpd" "$E/u2" >/dev/null 2>&1
if diff -r "$E/u1" "$E/u2" >/dev/null && diff -r "$E/u1/sub dir" "$T/rec/src/sub dir" >/dev/null && [ "`"$BIN" info "$E/arc/a.pzpd" | grep '^streams'`" = "streams  1" ]; then ok "drop-stream: back to the original data stream, byte-identical to the sources"; else bad "drop-stream"; fi

#-----------------------------------------------------------------------------------------
# 9. Video groups: @group NAME in lists, early shard cut, oversize clip, `groups`, salvage keeps groups
#-----------------------------------------------------------------------------------------
G="$T/grp"; rm -rf "$G"; mkdir -p "$G/v"
python3 - "$G" <<'EOF'
import os, sys
G = sys.argv[1]
lines = []
def rec(key, n):
    p = "%s/v/%s.bin" % (G, key.replace("/", "_"))
    open(p, "wb").write(os.urandom(n))
    lines.append("%s\tframe\t%s\t%s" % (key, p, key + ".bin"))
for i in range(12): rec("still%02d" % i, 1500)
for c, (name, frames, size) in enumerate([("walk", 10, 1200), ("run fast", 60, 1500), ("tab\\tname", 8, 900)]):
    lines.append("@group " + name)
    for f in range(frames): rec("clip%d/%03d" % (c, f), size)
    lines.append("@group")
for i in range(12, 20): rec("still%02d" % i, 1500)
open(G + "/list.tsv", "w").write("\n".join(lines) + "\n")
EOF
"$BIN" pack "$G/g.pzpd" "$G/list.tsv" --shard-size 32K >/dev/null 2>"$T/err" || bad "pack grouped list: `cat $T/err`"
"$BIN" groups "$G/g.pzpd" > "$G/groups" 2>/dev/null
if [ "$(cut -f1-3 "$G/groups")" = "$(printf 'walk\t12\t10\nrun fast\t22\t60\ntab\\tname\t82\t8')" ]; then ok "groups lists names (escaped), first records and frame counts"; else bad "groups: `cat $G/groups`"; fi
python3 - "$BIN" "$G/g.pzpd" > "$G/place" <<'EOF'
import subprocess, sys
info = subprocess.run([sys.argv[1], "info", sys.argv[2]], capture_output=True, text=True).stdout
shards = []
for l in info.splitlines():
    if l.strip().startswith("shard "):
        r = l.split("records ")[1].split()[0].split("..")
        shards.append((int(r[0]), int(r[1])))
def shard_of(o): return [k for k, (a, b) in enumerate(shards) if a <= o < b][0]
ok = all(shard_of(a) == shard_of(a + n - 1) for a, n in [(12, 10), (22, 60), (82, 8)])
own = shards[shard_of(22)] == (22, 82)
print("ok" if ok else "span", "own" if own else "shared", len(shards))
EOF
if grep -q "^ok own" "$G/place"; then ok "no group spans a shard; the oversize clip (60 x 1.5 KB > 32 KB) has a shard of its own ($(cut -d' ' -f3 "$G/place") shards)"; else bad "group placement: `cat $G/place`"; fi
F=`printf '%s/g.%05d.pzpd' "$G" "$(grep '^run fast' "$G/groups" | sed 's/.*shard //')"`
"$BIN" salvage "$F" "$G/s" --schemas "$G/g.pzpd" --list "$G/s.tsv" >/dev/null 2>&1
if grep -q "^@group run fast$" "$G/s.tsv" && "$BIN" pack "$G/s.pzpd" "$G/s.tsv" >/dev/null 2>&1 && [ "`"$BIN" groups "$G/s.pzpd" 2>/dev/null | cut -f1,3`" = "`printf 'run fast\t60'`" ]; then ok "salvage writes @group lines; the re-packed shard has the group back"; else bad "salvage groups: `grep '^@' $G/s.tsv`"; fi

#-----------------------------------------------------------------------------------------
# 11. Word indexes (spec §3.7): @words, synonyms, the words command, collections, reindex,
#     and a word index rebuilt by an interrupted replace-table / reindex (PZPDIR_TEST_CRASH)
#-----------------------------------------------------------------------------------------
W="$T/words"; rm -rf "$W"; mkdir -p "$W"
printf 'x' > "$W/blob"
wlist() { # $1 file, $2 key prefix, $3.. "source|caption" rows per record separated by "^"
    local f="$1" k="$2"; shift 2
    { echo "@table descriptions source:str text:str"; echo "@global synonyms word:str canonical:str"
      echo "@row synonyms dogs,dog"; echo "@row synonyms puppy,dog"; echo "@words descriptions.text source"
      local i=0
      for rec in "$@"; do
          printf '%s%d\tdata\t%s\t%s%d.bin\n' "$k" $i "$W/blob" "$k" $i
          IFS='^' read -ra rows <<< "$rec"
          for r in "${rows[@]}"; do [ -n "$r" ] && printf '%s%d\tdescriptions\t%s,"%s"\n' "$k" $i "${r%%|*}" "${r#*|}"; done
          i=$((i+1))
      done; } > "$f"
}
wlist "$W/a.tsv" a "vlm|A dog and a Dog.^old|two dogs" "vlm|a puppy!^old|no animals here" "vlm|Cat; café dog_1" "old|a cat"
if "$BIN" pack "$W/a.pzpd" "$W/a.tsv" --shard-size 1K --align 64 >/dev/null 2>"$T/err"; then ok "pack with @words and synonyms"; else bad "pack @words: `cat $T/err`"; fi
if "$BIN" verify "$W/a.pzpd" --blobs >/dev/null 2>"$T/err"; then ok "verify --blobs checks the word index"; else bad "verify words: `cat $T/err`"; fi
if [ "`"$BIN" words "$W/a.pzpd" --canonical --word dog 2>&1 | tr '\n' ' '`" = "a0 a1 " ]; then ok "words --canonical --word dog: dog / dogs / puppy, dog_1 is another word"; else bad "words --word: `"$BIN" words "$W/a.pzpd" --canonical --word dog 2>&1`"; fi
if [ "`"$BIN" words "$W/a.pzpd" --canonical --top 3 2>&1 | tr '\t\n' ',;'`" = "a,3,4;cat,2,2;dog,2,4;" ]; then ok "words --top: by records, ties by word (union semantics for dog: 2 records, 4 occurrences)"; else bad "words --top: `"$BIN" words "$W/a.pzpd" --canonical --top 3 2>&1`"; fi
if [ "`"$BIN" words "$W/a.pzpd" --source old 2>&1 | cut -f1 | tr '\n' ' '`" = "a animals cat dogs here no two " ]; then ok "words --source old: that source's rows only"; else bad "words --source: `"$BIN" words "$W/a.pzpd" --source old 2>&1`"; fi
if [ "`"$BIN" words "$W/a.pzpd" --sources 2>&1 | tr '\n' ' '`" = "old vlm " ]; then ok "words --sources"; else bad "words --sources"; fi
if "$BIN" info "$W/a.pzpd" 2>/dev/null | grep -q "^words descriptions.text  11 words  records covered 4  per source: old vlm"; then ok "info lists the word index"; else bad "info words: `"$BIN" info "$W/a.pzpd" 2>&1 | grep words`"; fi
wlist "$W/b.tsv" b "vlm|dogs everywhere" "old|a giraffe"
"$BIN" pack "$W/b.pzpd" "$W/b.tsv" >/dev/null 2>&1 || bad "pack b"
if [ "`"$BIN" words "$W/a.pzpd" "$W/b.pzpd" --canonical --word dog 2>&1 | tr '\n' ' '`" = "a0 a1 b0 " ] && \
   "$BIN" words "$W/a.pzpd" "$W/b.pzpd" 2>/dev/null | grep -q "^giraffe	1	1$"; then ok "two archives as one: vocabularies merged by word bytes, collection ordinals"; else bad "words over two archives"; fi
expect_fail "@words for an unknown table"      "@words nope.text\nk\tdata\t$T/src\n" "e.tsv:1: word index: no table nope"
expect_fail "@words on a non-str column"       "@table d n:u16\n@words d.n\nk\tdata\t$T/src\n" "e.tsv:2: .*not a str column"
expect_fail "synonyms chain rejected"          "@global synonyms word:str canonical:str\n@row synonyms dogs,dog\n@row synonyms dog,hound\nk\tdata\t$T/src\n" "row 1: \"dog\" is both"
expect_fail "synonyms: upper case rejected"    "@global synonyms word:str canonical:str\n@row synonyms Dogs,dog\nk\tdata\t$T/src\n" "not a single lower-case word"
expect_fail "synonyms: wrong schema rejected"  "@global synonyms from:str to:str\nk\tdata\t$T/src\n" "reserved for the word index"

# Interrupted rebuilds: replace-table (index rebuilt with the table) and reindex, then rerun
cp "$W"/a.* "$W/ref/" 2>/dev/null || { mkdir -p "$W/ref"; cp "$W"/a.* "$W/ref/"; }
wlist "$W/a2.tsv" a "vlm|three puppies" "old|dogs" "vlm|" "vlm|dog dog dog"
grep -v '^@global\|^@row\|^@words\|	data	' "$W/a2.tsv" > "$W/desc2.tsv"
"$BIN" replace-table "$W/ref/a.pzpd" descriptions "$W/desc2.tsv" >/dev/null 2>&1 || bad "reference replace-table"
PZPDIR_TEST_CRASH=flip:2 "$BIN" replace-table "$W/a.pzpd" descriptions "$W/desc2.tsv" >/dev/null 2>&1; RC=$?
"$BIN" replace-table "$W/a.pzpd" descriptions "$W/desc2.tsv" >/dev/null 2>"$T/err" || bad "rerun replace-table: `cat $T/err`"
if [ $RC = 99 ] && cmp -s <("$BIN" words "$W/a.pzpd" --canonical 2>&1) <("$BIN" words "$W/ref/a.pzpd" --canonical 2>&1) && \
   "$BIN" verify "$W/a.pzpd" --blobs >/dev/null 2>&1 && [ "`"$BIN" words "$W/a.pzpd" --canonical --word dog | tr '\n' ' '`" = "a1 a3 " ]; then
    ok "replace-table killed before its flip, rerun: the word index equals an uninterrupted rebuild"; else bad "interrupted replace-table with a word index (rc $RC)"; fi
"$BIN" reindex "$W/ref/a.pzpd" descriptions.text >/dev/null 2>&1 || bad "reference reindex"
PZPDIR_TEST_CRASH=flip:2 "$BIN" reindex "$W/a.pzpd" descriptions.text >/dev/null 2>&1; RC=$?
"$BIN" reindex "$W/a.pzpd" descriptions.text >/dev/null 2>"$T/err" || bad "rerun reindex: `cat $T/err`"
if [ $RC = 99 ] && cmp -s <("$BIN" words "$W/a.pzpd" 2>&1) <("$BIN" words "$W/ref/a.pzpd" 2>&1) && "$BIN" verify "$W/a.pzpd" --blobs >/dev/null 2>&1 && \
   [ -z "`"$BIN" words "$W/a.pzpd" --sources 2>&1`" ]; then ok "reindex (no source column now) killed before its flip, rerun: merged sub-index only"; else bad "interrupted reindex (rc $RC)"; fi
if "$BIN" reindex "$W/a.pzpd" descriptions.text --drop >/dev/null 2>&1 && ! "$BIN" words "$W/a.pzpd" >/dev/null 2>&1 && "$BIN" verify "$W/a.pzpd" --blobs >/dev/null 2>&1 && \
   "$BIN" reindex "$W/a.pzpd" descriptions.text source >/dev/null 2>&1 && [ "`"$BIN" words "$W/a.pzpd" --sources | tr '\n' ' '`" = "old vlm " ]; then ok "reindex --drop, then reindex with a source column"; else bad "reindex drop / add"; fi

echo
if [ $FAIL = 0 ]; then echo -e "\033[32mall CLI tests passed\033[0m"; exit 0; fi
echo -e "\033[31m$FAIL CLI test(s) failed\033[0m"
exit 1
