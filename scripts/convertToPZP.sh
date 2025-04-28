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

# Convert image to temporary PNM format
convert "$INPUT_FILE" temporary.ppm    
if [ $? -ne 0 ]; then
        echo "Error: Failed to convert $file to PNM format"
        exit 3
fi

# Compress using pzp
./pzp compress temporary.ppm "$OUTPUT_FILE"
if [ $? -ne 0 ]; then
        echo "Error: pzp compression failed for $file"
        exit 4
fi


# Remove temporary file
rm -f temporary.ppm

exit 0
