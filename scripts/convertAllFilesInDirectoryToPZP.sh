#!/bin/bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"
cd ..


# Check if the correct number of arguments is provided
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <input_directory> <output_directory>"
    exit 1
fi

INPUT_DIR="$1"
OUTPUT_DIR="$2"

# Ensure output directory exists
mkdir -p "$OUTPUT_DIR"

# Scan for image files
for file in "$INPUT_DIR"/*.{jpg,jpeg,png,bmp,tiff,tif}; do
    # Check if file exists to avoid issues with wildcards
    [ -e "$file" ] || continue
    
    # Extract filename without path and extension
    FILENAME=$(basename -- "$file")
    FILENAME_NO_EXT="${FILENAME%.*}"
    
    echo "Converting $file"

    # Convert image to temporary PNM format
    convert "$file" temporary.ppm    
    if [ $? -ne 0 ]; then
        echo "Error: Failed to convert $file to PNM format"
        exit 3
    fi

    # Compress using pzp
    ./pzp compress-palette temporary.ppm "$OUTPUT_DIR/$FILENAME_NO_EXT.pzp"
    if [ $? -ne 0 ]; then
        echo "Error: pzp compression failed for $file"
        exit 4
    fi


    # Remove temporary file
    rm -f temporary.ppm

done

echo "Processing complete."

scripts/checkParity.sh $1 $2
exit 0
