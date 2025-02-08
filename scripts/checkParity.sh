#!/bin/bash


#scripts/convertAllFilesInDirectoryToPZP.sh test/segment_val2017 test/segment_val2017PZP/
#scripts/convertAllDepthFilesInDirectoryToPZP.sh test/depth_val2017 test/depth_val2017PZP/

#scripts/checkParity.sh test/segment_val2017 test/segment_val2017PZP/
#scripts/checkParity.sh test/depth_val2017 test/depth_val2017PZP/

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"
cd ..

# Directories containing .pzp and .png files
PNG_DIR=$1
PZP_DIR=$2

# Extract basenames (without extensions) and sort them
PZP_FILES=$(find "$PZP_DIR" -type f -name "*.pzp" | sed 's|.*/||' | sed 's/\.pzp$//' | sort)
PNG_FILES=$(find "$PNG_DIR" -type f -name "*.png" | sed 's|.*/||' | sed 's/\.png$//' | sort)

# Save to temporary files for comparison
PZP_TMP=$(mktemp)
PNG_TMP=$(mktemp)
echo "$PZP_FILES" > "$PZP_TMP"
echo "$PNG_FILES" > "$PNG_TMP"

# Find missing files
echo "Files missing .png counterparts:"
comm -23 "$PZP_TMP" "$PNG_TMP" | awk '{print $1 ".pzp"}'

echo "Files missing .pzp counterparts:"
comm -13 "$PZP_TMP" "$PNG_TMP" | awk '{print $1 ".png"}'

# Clean up temporary files
rm -f "$PZP_TMP" "$PNG_TMP"

