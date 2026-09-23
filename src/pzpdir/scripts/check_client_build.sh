#!/bin/bash
# First-client build check (PLAN.md §5): pzpdir must build inside the DataLoader
# (RGBToPoseDetect2D/datasets/DataLoader) exactly the way the DataLoader builds it.
#
#  1. Vendor pzpdir.h, pzpdir_internal.h, the pzpdir_*.c library files, pzpdir_unicode.h and
#     third_party/xxhash.h into a scratch copy (as DataLoader/pzpdir/ holds them), with PZPDIR_WITH_PZP=0
#     and no pzp.h on the include path.
#  2. Compile every file under the DataLoader's release, AVX2, ASan and -pg flag sets (makeLibrary.sh /
#     Makefile), plus -Wall -Wextra -Werror: zero warnings allowed.
#  3. Build pzpdir/libpzpdir.so as the DataLoader does; it must export the public API (pzpd_*) only.
#  4. Link the DataLoader's own sources into libDataLoader.so against it (-lpzpdir, DT_RPATH $ORIGIN/pzpdir:
#     searched before LD_LIBRARY_PATH, so no other libpzpdir.so, e.g. one in the current directory, is taken)
#     with only the DataLoader's libraries and -Wl,--no-undefined: no missing symbols, no clashes, and
#     the loader finds libpzpdir.so next to libDataLoader.so.
#     The DataLoader repository is only read; all output goes to the scratch directory.
#
# Usage: scripts/check_client_build.sh [path-to-DataLoader]

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
DL=${1:-/home/ammar/Documents/Programming/RGBToPoseDetect2D/datasets/DataLoader}
T=${PZPDIR_TEST_DIR:-/tmp/pzpdir_test}/client
rm -rf "$T"; mkdir -p "$T/pzpdir/third_party"
cp "$HERE/pzpdir.h" "$HERE/pzpdir_internal.h" "$HERE/pzpdir_unicode.h" "$T/pzpdir/"
for f in "$HERE"/pzpdir_*.c; do [ "${f##*/}" = pzpdir_cli.c ] || cp "$f" "$T/pzpdir/"; done
cp "$HERE/third_party/xxhash.h" "$T/pzpdir/third_party/"

FAIL=0
LIBS="-pthread -lm -lpng -ljpeg -lzstd -llz4"
STRICT="-Wall -Wextra -Werror -DPZPDIR_WITH_PZP=0"
declare -A FLAGS
FLAGS[release]="-D_GNU_SOURCE -O3 -fPIC -Wno-unused-function -march=native -mtune=native"
FLAGS[avx2]="-DINTEL_OPTIMIZATIONS -mavx2 -D_GNU_SOURCE -O3 -fPIC -Wno-unused-function -march=native -mtune=native"
FLAGS[asan]="-DINTEL_OPTIMIZATIONS -mavx2 -D_GNU_SOURCE -O0 -g3 -fno-omit-frame-pointer -pg -Wstrict-overflow -fsanitize=address -fPIE -fPIC -Wno-unused-function -march=native -mtune=native"
FLAGS[profile]="-DINTEL_OPTIMIZATIONS -mavx2 -D_GNU_SOURCE -O0 -g3 -fno-omit-frame-pointer -pg -Wstrict-overflow -fPIE -fPIC -Wno-unused-function -march=native -mtune=native"

for k in release avx2 asan profile; do
  ok=1
  for f in "$T"/pzpdir/pzpdir_*.c; do
    b=${f##*/}
    gcc -c ${FLAGS[$k]} $STRICT "$f" -o "$T/${b%.c}_$k.o" 2>>"$T/$k.log" || ok=0
  done
  if [ $ok = 1 ]; then echo -e "\033[32mok\033[0m   compile ($k flags, PZPDIR_WITH_PZP=0, -Werror)"
  else echo -e "\033[31mFAIL\033[0m compile ($k flags):"; cat "$T/$k.log"; FAIL=1; fi
done

# libpzpdir.so as the DataLoader's makeLibrary.sh / Makefile build it
if gcc -shared -o "$T/pzpdir/libpzpdir.so" ${FLAGS[avx2]} -DPZPDIR_WITH_PZP=0 "$T"/pzpdir/pzpdir_*.c -pthread -lzstd -llz4 -Wl,--no-undefined 2>"$T/so.log"; then
  echo -e "\033[32mok\033[0m   libpzpdir.so builds (DataLoader flags, --no-undefined)"
else
  echo -e "\033[31mFAIL\033[0m libpzpdir.so:"; cat "$T/so.log"; FAIL=1
fi
EXTRA=`nm -D --defined-only "$T/pzpdir/libpzpdir.so" 2>/dev/null | awk '{print $3}' | grep -v "^pzpd_" | head -5`
if [ -z "$EXTRA" ]; then echo -e "\033[32mok\033[0m   libpzpdir.so exports pzpd_* only (internals hidden, xxHash static inline)"; else echo -e "\033[31mFAIL\033[0m also exported: $EXTRA"; FAIL=1; fi

if [ -d "$DL" ]; then
  SRC="$DL/codecs/codecs.c $DL/codecs/asciiInput.c $DL/codecs/bmpInput.c $DL/codecs/jpgInput.c $DL/codecs/pfmInput.c $DL/codecs/pngInput.c $DL/codecs/ppmInput.c $DL/codecs/pzpInput.c $DL/PrepareBatch.c $DL/DataLoader.c $DL/HeatmapGenerator.c $DL/DBLoader.c $DL/cache.c"
  if [ -f "$DL/PZPDLoader.c" ]; then SRC="$SRC $DL/PZPDLoader.c"; fi   # the archive adapter (DataLoader branch pzpdir)
  # -I$T first: the DataLoader's `#include "pzpdir/pzpdir.h"` takes the header just built against
  if gcc -shared -o "$T/libDataLoader.so" ${FLAGS[avx2]} -DPZPDIR_WITH_PZP=0 -I"$T" -I"$DL" $SRC -L"$T/pzpdir" -lpzpdir -Wl,-rpath,'$ORIGIN/pzpdir' -Wl,--disable-new-dtags -Wl,--no-undefined $LIBS 2>"$T/link.log"; then
    echo -e "\033[32mok\033[0m   link the DataLoader's own sources against libpzpdir.so (no clashes, no undefined symbols)"
  else
    echo -e "\033[31mFAIL\033[0m link with the DataLoader sources:"; grep -i "error\|multiple definition\|undefined" "$T/link.log" | head -20; FAIL=1
  fi
  # Load it as DataLoader.py does (ctypes.CDLL) and see which libpzpdir.so the dynamic loader picked
  GOT=`python3 -c "import ctypes,sys; ctypes.CDLL(sys.argv[1]); print([l.split()[-1] for l in open('/proc/self/maps') if l.rstrip().endswith('/libpzpdir.so')][0])" "$T/libDataLoader.so" 2>&1`
  if [ "$GOT" = "$T/pzpdir/libpzpdir.so" ]; then
    echo -e "\033[32mok\033[0m   libDataLoader.so loads libpzpdir.so from next to it (rpath \$ORIGIN/pzpdir)"
  else
    echo -e "\033[31mFAIL\033[0m loading libDataLoader.so: $GOT"; FAIL=1
  fi
else
  echo "skip: DataLoader not found at $DL"
fi

if [ $FAIL = 0 ]; then echo -e "\033[32mclient build check passed\033[0m"; exit 0; fi
echo -e "\033[31mclient build check failed\033[0m"; exit 1
