#!/bin/bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"
cd ..

# Check if the correct number of arguments is provided
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <input_file> <output_file>"
    exit 1
fi

INPUT_FILE="$1"
OUTPUT_FILE="$2"

# Extract filename without path and extension
FILENAME=$(basename -- "$INPUT_FILE")
FILENAME_NO_EXT="${FILENAME%.*}"
    
echo "Converting $INPUT_FILE"


# Convert image to temporary 16-bit grayscale PGM format
convert "$INPUT_FILE" -depth 16 -colorspace Gray temporary.pnm
if [ $? -ne 0 ]; then
    echo "Error: Failed to convert $INPUT_FILE to 16-bit grayscale PNM"
    exit 3
fi

# Compress using pzp
./pzp compress temporary.pnm "$OUTPUT_FILE"
if [ $? -ne 0 ]; then
    echo "Error: pzp compression failed for $INPUT_FILE"
    exit 4
fi

# Remove temporary file
rm -f temporary.pnm


# Calculate compression ratio
INPUT_SIZE=$(stat -c%s "$INPUT_FILE")
OUTPUT_SIZE=$(stat -c%s "$OUTPUT_FILE")

if [ "$OUTPUT_SIZE" -gt 0 ]; then
    RATIO=$(echo "scale=2; $INPUT_SIZE / $OUTPUT_SIZE" | bc)
    echo "Compression ratio: $RATIO : 1"
else
    echo "Error: Output file size is zero."
    exit 5
fi

exit 0
