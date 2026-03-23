#!/bin/bash
# encodeDirectoryToPZPStandalone.sh — Clone/update PZP, build it, create a
# Python venv, and batch-encode a directory of images to PZP format.
#
# Usage:
#   ./encodeDirectoryToPZPStandalone.sh <source_directory> [options]
#
# The encoded .pzp files are written to <source_directory>PZP/
#
# Options (passed through to encode_directory.py):
#   --rle           Enable delta pre-filter
#   --palette       Enable per-channel palette indexing
#   --workers N     Parallel encoding workers (default: CPU count)
#   --ext EXT       Source extension to scan for (default: png)
#
# Examples:
#   ./encodeDirectoryToPZPStandalone.sh /data/coco/val2017
#   ./encodeDirectoryToPZPStandalone.sh /data/coco/val2017 --rle --palette
#   ./encodeDirectoryToPZPStandalone.sh /data/depth_maps --ext ppm --workers 8
#
# The PZP toolchain is installed to ~/.local/share/pzp (no sudo required).
# Re-running the script updates the existing installation.

set -euo pipefail

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <source_directory> [--rle] [--palette] [--workers N] [--ext EXT]"
    exit 1
fi

SOURCE_DIR="$1"
shift
ENCODE_ARGS=("$@")   # remaining args forwarded to encode_directory.py

if [ ! -d "$SOURCE_DIR" ]; then
    echo "Error: source directory does not exist: $SOURCE_DIR"
    exit 1
fi

# Resolve to absolute path so it remains valid after any cd
SOURCE_DIR="$(cd "$SOURCE_DIR" && pwd)"
TARGET_DIR="${SOURCE_DIR}PZP"

# ---------------------------------------------------------------------------
# System dependency check — installs any missing packages via apt
# ---------------------------------------------------------------------------

SYSTEM_DEPENDENCIES="python3-pip python3-venv build-essential git libzstd-dev"

for REQUIRED_PKG in $SYSTEM_DEPENDENCIES; do
    PKG_OK=$(dpkg-query -W --showformat='${Status}\n' "$REQUIRED_PKG" 2>/dev/null | grep "install ok installed" || true)
    echo "Checking for $REQUIRED_PKG: ${PKG_OK:-NOT FOUND}"
    if [ "" = "$PKG_OK" ]; then
        echo "Missing: $REQUIRED_PKG — installing all required system packages."
        sudo apt-get install $SYSTEM_DEPENDENCIES
        break
    fi
done

# ---------------------------------------------------------------------------
# Clone or update the PZP repository
# ---------------------------------------------------------------------------

INSTALL_DIR="$HOME/.local/share/pzp"
REPO_URL="https://github.com/AmmarkoV/PZP"

if [ -d "$INSTALL_DIR/.git" ]; then
    echo "==> Updating existing PZP installation at $INSTALL_DIR"
    git -C "$INSTALL_DIR" pull --ff-only
else
    echo "==> Cloning PZP into $INSTALL_DIR"
    mkdir -p "$(dirname "$INSTALL_DIR")"
    git clone "$REPO_URL" "$INSTALL_DIR"
fi

# ---------------------------------------------------------------------------
# Build libpzp.so
# ---------------------------------------------------------------------------

echo "==> Building libpzp.so"
make -C "$INSTALL_DIR" libpzp.so

# ---------------------------------------------------------------------------
# Create / update the Python venv
# ---------------------------------------------------------------------------

VENV_DIR="$INSTALL_DIR/venv"

if [ ! -d "$VENV_DIR" ]; then
    echo "==> Creating Python venv at $VENV_DIR"
    python3 -m venv "$VENV_DIR"
fi

echo "==> Installing / updating Python dependencies"
"$VENV_DIR/bin/pip" install --quiet --upgrade pip
"$VENV_DIR/bin/pip" install --quiet --upgrade opencv-python-headless numpy

# ---------------------------------------------------------------------------
# Encode
# ---------------------------------------------------------------------------

echo "==> Encoding"
echo "    Source : $SOURCE_DIR"
echo "    Target : $TARGET_DIR"
echo "    Options: ${ENCODE_ARGS[*]:-none}"
echo ""

"$VENV_DIR/bin/python3" "$INSTALL_DIR/scripts/encode_directory.py" \
    "$SOURCE_DIR" "$TARGET_DIR" \
    "${ENCODE_ARGS[@]}"
