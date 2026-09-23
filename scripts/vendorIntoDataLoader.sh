#!/bin/bash
# Re-vendor PZP and pzpdir into the DataLoader (RGBToPoseDetect2D/datasets/DataLoader):
#
#   pzp.h                          -> codecs/pzp.h                  (decoder used by codecs/pzpInput.c)
#   pzp.c                          -> codecs/pzp.c                  (the pzp CLI makeLibrary.sh builds)
#   src/pzpdir/pzpdir.h            -> pzpdir/pzpdir.h
#   src/pzpdir/pzpdir.c            -> pzpdir/pzpdir.c              (the one file clients compile)
#   src/pzpdir/pzpdir_*.inc.c      -> pzpdir/pzpdir_*.inc.c        (its parts, #included by pzpdir.c)
#   src/pzpdir/pzpdir_unicode.h    -> pzpdir/pzpdir_unicode.h      (word index tokenizer tables, generated)
#   src/pzpdir/third_party/xxhash.h -> pzpdir/third_party/xxhash.h
#
# A destination with uncommitted changes in the DataLoader repository may hold local edits, so it is
# only overwritten with --force (commit the previous vendoring first and no --force is needed).
# After copying, the DataLoader build is checked in a scratch directory (check_client_build.sh + the
# pzp CLI). Nothing is committed and the DataLoader's own libDataLoader.so is not rebuilt.
#
# Usage: scripts/vendorIntoDataLoader.sh [--check] [--force] [path-to-DataLoader]
#   --check  only report which vendored files differ (exit 1 if any do); writes nothing

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
CHECK=0; FORCE=0; DL=~/Documents/Programming/RGBToPoseDetect2D/datasets/DataLoader
for a in "$@"; do
  case "$a" in
    --check) CHECK=1 ;;
    --force) FORCE=1 ;;
    -*) echo "unknown option $a"; exit 2 ;;
    *) DL="$a" ;;
  esac
done
if [ ! -d "$DL" ]; then echo "DataLoader not found at $DL"; exit 2; fi
INGIT=0; git -C "$DL" rev-parse --is-inside-work-tree >/dev/null 2>&1 && INGIT=1

FILES="pzp.h:codecs/pzp.h
pzp.c:codecs/pzp.c
src/pzpdir/pzpdir.h:pzpdir/pzpdir.h
src/pzpdir/pzpdir.c:pzpdir/pzpdir.c
src/pzpdir/pzpdir_unicode.h:pzpdir/pzpdir_unicode.h
$(cd "$HERE" && for p in src/pzpdir/pzpdir_*.inc.c; do echo "$p:pzpdir/${p##*/}"; done)
src/pzpdir/third_party/xxhash.h:pzpdir/third_party/xxhash.h"

CHANGED=""; LOCAL=""
for m in $FILES; do
  SRC="$HERE/${m%%:*}"; DST="$DL/${m#*:}"
  if cmp -s "$SRC" "$DST"; then echo "same     ${m#*:}"; continue; fi
  echo "differs  ${m#*:}"
  CHANGED="$CHANGED $m"
  # not in git: local edits cannot be told apart, so treat every existing destination as possibly edited
  if [ -f "$DST" ] && { [ $INGIT = 0 ] || ! git -C "$DL" diff --quiet HEAD -- "$DST"; }; then LOCAL="$LOCAL ${m#*:}"; fi
done

if [ -z "$CHANGED" ]; then echo -e "\033[32mDataLoader is up to date\033[0m"; exit 0; fi
if [ $CHECK = 1 ]; then exit 1; fi
if [ -n "$LOCAL" ] && [ $FORCE = 0 ]; then
  if [ $INGIT = 0 ]; then
    echo -e "\033[31mrefusing:\033[0m $DL is not a git checkout, so local edits in$LOCAL cannot be detected"
  else
    echo -e "\033[31mrefusing:\033[0m uncommitted changes in the DataLoader for:$LOCAL"
    echo "  (local edits, or an earlier vendoring not committed yet) - review with: git -C $DL diff --$LOCAL"
  fi
  echo "  commit them, or rerun with --force to overwrite"
  exit 1
fi

for m in $CHANGED; do
  SRC="$HERE/${m%%:*}"; DST="$DL/${m#*:}"
  mkdir -p "$(dirname "$DST")" && cp "$SRC" "$DST" && cmp -s "$SRC" "$DST" || { echo -e "\033[31mFAIL\033[0m copying ${m#*:}"; exit 1; }
  echo -e "\033[32mvendored\033[0m ${m#*:}"
done

# Build check: pzpdir under the DataLoader's flag sets, linked with its sources ( incl. codecs/pzpInput.c
# and the new codecs/pzp.h ) into one .so, then the pzp CLI exactly as makeLibrary.sh builds it.
FAIL=0
"$HERE/src/pzpdir/scripts/check_client_build.sh" "$DL" || FAIL=1
T=${PZPDIR_TEST_DIR:-/tmp/pzpdir_test}/client
mkdir -p "$T"
if ( cd "$DL" && gcc -o "$T/pzp_cli" codecs/pzp.c -pthread -lm -lpng -ljpeg -lzstd -llz4 ) 2>"$T/pzp_cli.log"; then
  echo -e "\033[32mok\033[0m   pzp CLI builds from codecs/pzp.c (makeLibrary.sh command)"
else
  echo -e "\033[31mFAIL\033[0m pzp CLI build:"; cat "$T/pzp_cli.log"; FAIL=1
fi

echo "pzpdir $(grep -o 'pzpdirVersion\[\]="[^"]*"' "$DL/pzpdir/pzpdir.h" | cut -d'"' -f2), pzp $(grep -o 'pzp_version\[\]="[^"]*"' "$DL/codecs/pzp.h" | cut -d'"' -f2) now in $DL"
[ $INGIT = 1 ] && git -C "$DL" status --short -- codecs/pzp.h codecs/pzp.c pzpdir/
if [ $FAIL = 0 ]; then echo -e "\033[32mvendoring done\033[0m (not committed; rebuild libDataLoader.so with make / makeLibrary.sh)"; exit 0; fi
echo -e "\033[31mvendoring done but the build check failed\033[0m"; exit 1
