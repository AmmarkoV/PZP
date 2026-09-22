#!/bin/bash
# Generates the pzpdir documentation: doc/html/, doc/latex/ and PZPDIR.pdf (next to pzpdir.h).
# Needs doxygen and pdflatex. Fails if doxygen reports any warning (the phase-1 documentation gate).

STARTDIR=`pwd`
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR/.."
ROOT=`pwd`

# Read by doc/doxyfile
export DOXYGEN_INPUT="$ROOT"
export DOXYGEN_OUTPUT="$ROOT/doc"

# Start clean, so documentation of removed items doesn't linger
rm -rf doc/html doc/latex doc/doxygen_warnings.txt

if ! doxygen doc/doxyfile; then
  echo "doxygen failed"
  cd "$STARTDIR"
  exit 1
fi

if [ -s doc/doxygen_warnings.txt ]; then
  echo "doxygen warnings (the gate is zero):"
  cat doc/doxygen_warnings.txt
  cd "$STARTDIR"
  exit 1
fi

if ! timeout 600 make -s -C doc/latex > doc/latex/make.log 2>&1; then
  echo "Building the PDF failed, see doc/latex/refman.log"
  cd "$STARTDIR"
  exit 1
fi

rm -f PZPDIR.pdf
cp doc/latex/refman.pdf PZPDIR.pdf
echo "Generated $ROOT/PZPDIR.pdf and $ROOT/doc/html/index.html (0 doxygen warnings)"

cd "$STARTDIR"
exit 0
