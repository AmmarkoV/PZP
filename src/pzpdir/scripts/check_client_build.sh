#!/bin/bash
# First-client build check (PLAN.md §5): pzpdir must build inside the DataLoader
# (RGBToPoseDetect2D/datasets/DataLoader) exactly the way the DataLoader builds itself.
#
#  1. Vendor pzpdir.h, pzpdir.c and third_party/xxhash.h into a scratch copy (as DataLoader/pzpdir/
#     would), with PZPDIR_WITH_PZP=0 and no pzp.h on the include path.
#  2. Compile under the DataLoader's release, AVX2, ASan and -pg flag sets (makeLibrary.sh / Makefile),
#     plus -Wall -Wextra -Werror: zero warnings allowed.
#  3. Link pzpdir.c together with the DataLoader's own sources into one shared library with only the
#     DataLoader's libraries and -Wl,--no-undefined: no missing symbols, no symbol clashes.
#     The DataLoader repository is only read; all output goes to the scratch directory.
#
# Usage: scripts/check_client_build.sh [path-to-DataLoader]

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
DL=${1:-/home/ammar/Documents/Programming/RGBToPoseDetect2D/datasets/DataLoader}
T=${PZPDIR_TEST_DIR:-/tmp/pzpdir_test}/client
rm -rf "$T"; mkdir -p "$T/pzpdir/third_party"
cp "$HERE/pzpdir.h" "$HERE/pzpdir.c" "$T/pzpdir/"
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
  if gcc -c ${FLAGS[$k]} $STRICT "$T/pzpdir/pzpdir.c" -o "$T/pzpdir_$k.o" 2>"$T/$k.log"; then
    echo -e "\033[32mok\033[0m   compile ($k flags, PZPDIR_WITH_PZP=0, -Werror)"
  else
    echo -e "\033[31mFAIL\033[0m compile ($k flags):"; cat "$T/$k.log"; FAIL=1
  fi
done

if [ -d "$DL" ]; then
  SRC="$DL/codecs/codecs.c $DL/codecs/asciiInput.c $DL/codecs/bmpInput.c $DL/codecs/jpgInput.c $DL/codecs/pfmInput.c $DL/codecs/pngInput.c $DL/codecs/ppmInput.c $DL/codecs/pzpInput.c $DL/PrepareBatch.c $DL/DataLoader.c $DL/HeatmapGenerator.c $DL/DBLoader.c $DL/cache.c"
  if [ -f "$DL/PZPDLoader.c" ]; then SRC="$SRC $DL/PZPDLoader.c"; fi   # the archive adapter (DataLoader branch pzpdir)
  if gcc -shared -o "$T/libDataLoader_with_pzpdir.so" ${FLAGS[avx2]} -DPZPDIR_WITH_PZP=0 -I"$DL" $SRC "$T/pzpdir/pzpdir.c" -Wl,--no-undefined $LIBS 2>"$T/link.log"; then
    echo -e "\033[32mok\033[0m   link with the DataLoader's own sources into one .so (no clashes, no undefined symbols)"
  else
    echo -e "\033[31mFAIL\033[0m link with the DataLoader sources:"; grep -i "error\|multiple definition\|undefined" "$T/link.log" | head -20; FAIL=1
  fi
  EXTRA=`nm -D --defined-only "$T/libDataLoader_with_pzpdir.so" 2>/dev/null | awk '{print $3}' | grep -i "xxh\|zstd\|lz4" | head -5`
  if [ -z "$EXTRA" ]; then echo -e "\033[32mok\033[0m   no xxHash / zstd / lz4 symbols exported (xxHash is static inline)"; else echo -e "\033[31mFAIL\033[0m exported: $EXTRA"; FAIL=1; fi
else
  echo "skip: DataLoader not found at $DL"
fi

if [ $FAIL = 0 ]; then echo -e "\033[32mclient build check passed\033[0m"; exit 0; fi
echo -e "\033[31mclient build check failed\033[0m"; exit 1
