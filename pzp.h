/*
PZP Portable Zipped PNM
Copyright (C) 2025 Ammar Qammaz

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef PZP_H_INCLUDED
#define PZP_H_INCLUDED

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <zstd.h>
//sudo apt install libzstd-dev

#if INTEL_OPTIMIZATIONS
#include <immintrin.h>  // AVX intrinsics
#include <stdint.h>
#warning "Intel Optimizations Enabled"
#endif // INTEL_OPTIMIZATIONS

#define PZP_VERBOSE 0

static const char pzp_version[]="v0.0";
static const char pzp_header[4]={"PZP0"};

static const int headerSize =  sizeof(unsigned int) * 10;
//header, width, height, bitsperpixel, channels, internalbitsperpixel, internalchannels, checksum, compression_mode, unused

// Define flags using bitwise shift for clarity
typedef enum
{
    USE_COMPRESSION = 1 << 0,  // 0001
    USE_RLE         = 1 << 1,  // 0010
    TEST_FLAG1      = 1 << 2,  // 0100
    TEST_FLAG2      = 1 << 3   // 1000
} PZPFlags;

static unsigned int convert_header(const char header[4])
{
    return ((unsigned int)header[0] << 24) |
           ((unsigned int)header[1] << 16) |
           ((unsigned int)header[2] << 8)  |
           ((unsigned int)header[3]);
}

static void fail(const char * message)
{
  fprintf(stderr,"PZP Fatal Error: %s\n",message);
  exit(EXIT_FAILURE);
}

static unsigned int hash_checksum(const void *data, size_t dataSize)
{
    const unsigned char *bytes = (const unsigned char *)data;
    unsigned int h1 = 0x12345678, h2 = 0x9ABCDEF0, h3 = 0xFEDCBA98, h4 = 0x87654321;

    while (dataSize >= 4)
    {
        h1 = (h1 ^ bytes[0]) * 31;
        h2 = (h2 ^ bytes[1]) * 37;
        h3 = (h3 ^ bytes[2]) * 41;
        h4 = (h4 ^ bytes[3]) * 43;
        bytes += 4;
        dataSize -= 4;
    }

    // Process remaining bytes
    if (dataSize > 0) h1 = (h1 ^ bytes[0]) * 31;
    if (dataSize > 1) h2 = (h2 ^ bytes[1]) * 37;
    if (dataSize > 2) h3 = (h3 ^ bytes[2]) * 41;

    // Final mix to spread entropy
    return (h1 ^ (h2 >> 3)) + (h3 ^ (h4 << 5));
}

static void pzp_split_channels(const unsigned char *image, unsigned char **buffers, int num_buffers, int WIDTH, int HEIGHT)
{
    int total_size = WIDTH * HEIGHT;

    // Split channels
    for (int i = 0; i < total_size; i++)
    {
        for (int ch = 0; ch < num_buffers; ch++)
        {
            buffers[ch][i] = image[i * num_buffers + ch];
        }
    }

}

static void pzp_RLE_filter(unsigned char **buffers, int num_buffers, int WIDTH, int HEIGHT)
{
    int total_size = WIDTH * HEIGHT;

    // Apply left-pixel delta filtering
    for (int i = total_size - 1; i > 0; i--)
    {
        for (int ch = 0; ch < num_buffers; ch++)
        {
            buffers[ch][i] -= buffers[ch][i - 1];
        }
    }
}
//-----------------------------------------------------------------------------------------------
//-----------------------------------------------------------------------------------------------
static void pzp_compress_combined(unsigned char **buffers,
                              unsigned int width,unsigned int height,
                              unsigned int bitsperpixelExternal, unsigned int channelsExternal,
                              unsigned int bitsperpixelInternal, unsigned int channelsInternal, unsigned int configuration,
                              const char *output_filename)
{
    FILE *output = fopen(output_filename, "wb");
    if (!output)
    {
        fail("File error");
    }

    unsigned int combined_buffer_size = (width * height * (bitsperpixelInternal/8)* channelsInternal) + headerSize;

    unsigned int dataSize = combined_buffer_size;       //width * height;
    fwrite(&dataSize, sizeof(unsigned int), 1, output); // Store size for decompression

    //printf("Write size: %d bytes\n", dataSize);

    size_t max_compressed_size = ZSTD_compressBound(combined_buffer_size);
    void *compressed_buffer = malloc(max_compressed_size);
    if (!compressed_buffer)
    {
        fail("Memory allocation failed");
    }

    unsigned char *combined_buffer_raw = (unsigned char *) malloc(combined_buffer_size);
    if (!combined_buffer_raw)
    {
        fail("Memory allocation failed");
    }

    // Store header information
    //---------------------------------------------------------------------------------------------------
    unsigned int *memStartAsUINT             = (unsigned int*) combined_buffer_raw;
    //---------------------------------------------------------------------------------------------------
    unsigned int *headerTarget               = memStartAsUINT + 0; // Move by 1, not sizeof(unsigned int)
    unsigned int *bitsperpixelTarget         = memStartAsUINT + 1; // Move by 1, not sizeof(unsigned int)
    unsigned int *channelsTarget             = memStartAsUINT + 2; // Move by 1, not sizeof(unsigned int)
    unsigned int *widthTarget                = memStartAsUINT + 3; // Move by 1, not sizeof(unsigned int)
    unsigned int *heightTarget               = memStartAsUINT + 4; // Move by 1, not sizeof(unsigned int)
    unsigned int *bitsperpixelInternalTarget = memStartAsUINT + 5; // Move by 1, not sizeof(unsigned int)
    unsigned int *channelsInternalTarget     = memStartAsUINT + 6; // Move by 1, not sizeof(unsigned int)
    unsigned int *checksumTarget             = memStartAsUINT + 7; // Move by 1, not sizeof(unsigned int)
    unsigned int *compressionModeTarget      = memStartAsUINT + 8; // Move by 1, not sizeof(unsigned int)
    unsigned int *unusedTarget               = memStartAsUINT + 9; // Move by 1, not sizeof(unsigned int)
    //---------------------------------------------------------------------------------------------------

    //Store data to their target location
    *headerTarget               = convert_header(pzp_header);
    *bitsperpixelTarget         = bitsperpixelExternal;
    *channelsTarget             = channelsExternal;
    *widthTarget                = width;
    *heightTarget               = height;
    *bitsperpixelInternalTarget = bitsperpixelInternal;
    *channelsInternalTarget     = channelsInternal;
    *compressionModeTarget      = configuration;
    *unusedTarget               = 0; //<- Just so that it is not random

    // Store separate image planes so that they get better compressed :P
    unsigned char *combined_buffer = combined_buffer_raw + headerSize;
    for (int i = 0; i < width*height; i++)
    {
        for (unsigned int ch = 0; ch < channelsInternal; ch++)
        {
            combined_buffer[i * channelsInternal + ch] = buffers[ch][i];
        }
    }

    //Calculate the checksum of the combined buffer
    *checksumTarget = hash_checksum(combined_buffer,width*height*channelsInternal);

    #if PZP_VERBOSE
    fprintf(stderr, "Storing %ux%ux%u@%ubit/",width,height,channelsExternal,bitsperpixelExternal);
    fprintf(stderr, "%u@%ubit",channelsInternal,bitsperpixelInternal);
    fprintf(stderr, " | mode %u | CRC:0x%X\n", configuration, *checksumTarget);
    #endif // PZP_VERBOSE


    size_t compressed_size = ZSTD_compress(compressed_buffer, max_compressed_size, combined_buffer_raw, combined_buffer_size, 1);
    if (ZSTD_isError(compressed_size))
    {
        fprintf(stderr, "Zstd compression error: %s\n", ZSTD_getErrorName(compressed_size));
        fail("Zstd compression error");
    }

    #if PZP_VERBOSE
    fprintf(stderr,"Compression Ratio : %0.2f\n", (float) dataSize/compressed_size);
    #endif // PZP_VERBOSE

    fwrite(compressed_buffer, 1, compressed_size, output);

    free(compressed_buffer);
    free(combined_buffer_raw);
    fclose(output);
}
//-----------------------------------------------------------------------------------------------
//-----------------------------------------------------------------------------------------------
static void pzp_extractAndReconstruct(unsigned char *decompressed_bytes, unsigned char *reconstructed, unsigned int width, unsigned int height, unsigned int channels, int restoreRLEChannels)
{
    unsigned int total_size = width * height;
    unsigned char *src = decompressed_bytes;
    unsigned char *r = reconstructed;

    if (restoreRLEChannels)
    {
        switch (channels)
        {
            case 1:
                r[0] = src[0];
                for (unsigned int i = 1; i < total_size; i++)
                {
                    r[i] = src[i] + r[i - 1];
                }
                break;
            case 2:
                r[0] = src[0];
                r[1] = src[1];
                for (unsigned int i = 1; i < total_size; i++)
                {
                    r += 2;
                    src += 2;
                    r[0] = src[0] + r[-2];
                    r[1] = src[1] + r[-1];
                }
                break;
            case 3:
                r[0] = src[0];
                r[1] = src[1];
                r[2] = src[2];
                for (unsigned int i = 1; i < total_size; i++)
                {
                    r += 3;
                    src += 3;
                    r[0] = src[0] + r[-3];
                    r[1] = src[1] + r[-2];
                    r[2] = src[2] + r[-1];
                }
                break;
            default:
                for (unsigned int ch = 0; ch < channels; ch++)
                {
                    r[ch] = src[ch];
                }
                for (unsigned int i = 1; i < total_size; i++)
                {
                    for (unsigned int ch = 0; ch < channels; ch++)
                    {
                        r[i * channels + ch] = src[i * channels + ch] + r[(i - 1) * channels + ch];
                    }
                }
                break;
        }
    }
    else // Non-RLE path
    {
        switch (channels)
        {
            case 1:
                memcpy(reconstructed, src, total_size);
                break;
            case 2:
                for (unsigned int i = 0; i < total_size; i++)
                {
                    reconstructed[2 * i] = src[2 * i];
                    reconstructed[2 * i + 1] = src[2 * i + 1];
                }
                break;
            case 3:
                for (unsigned int i = 0; i < total_size; i++)
                {
                    reconstructed[3 * i] = src[3 * i];
                    reconstructed[3 * i + 1] = src[3 * i + 1];
                    reconstructed[3 * i + 2] = src[3 * i + 2];
                }
                break;
            default:
                for (unsigned int i = 0; i < total_size; i++)
                {
                    for (unsigned int ch = 0; ch < channels; ch++)
                    {
                        reconstructed[i * channels + ch] = src[i * channels + ch];
                    }
                }
                break;
        }
    }
}
//-----------------------------------------------------------------------------------------------
//-----------------------------------------------------------------------------------------------
static unsigned char* pzp_decompress_combined(const char *input_filename,
                                unsigned int *widthOutput, unsigned int *heightOutput,
                                unsigned int *bitsperpixelExternalOutput, unsigned int *channelsExternalOutput,
                                unsigned int *bitsperpixelInternalOutput, unsigned int *channelsInternalOutput,
                                unsigned int *configuration)
{
    FILE *input = fopen(input_filename, "rb");
    if (!input)
    {
        fail("File error");
    }

    // Read stored size
    unsigned int dataSize;
    if (fread(&dataSize, sizeof(unsigned int), 1, input) != 1)
    {
        fclose(input);
        fail("Failed to read data size");
    }

    if (dataSize == 0 || dataSize > 100000000)   // Sanity check
    {
        fclose(input);
        fprintf(stderr, "Error: Invalid size read from file (%d)\n", dataSize);
        fail("Error: Invalid size read from file");
    }
    //printf("Read size: %d bytes\n", dataSize);

    // Read compressed data
    if (fseek(input, 0, SEEK_END) != 0)
    {
        fclose(input);
        fail("Failed to seek file end");
    }

    long fileSize = ftell(input);
    if (fileSize < 0)
    {
        fclose(input);
        fail("Failed to determine file size");
    }

    size_t compressed_size = fileSize - sizeof(unsigned int);

    if (fseek(input, sizeof(unsigned int), SEEK_SET) != 0)
    {
        fclose(input);
        fail("Failed to seek to compressed data");
    }

    void *compressed_buffer = malloc(compressed_size);
    if (!compressed_buffer)
    {
        fclose(input);
        fail("Memory allocation #1 failed");
    }

    if (fread(compressed_buffer, 1, compressed_size, input) != compressed_size)
    {
        free(compressed_buffer);
        fclose(input);
        fail("Failed to read compressed data");
    }

    fclose(input);

    size_t decompressed_size = (size_t)dataSize;
    void *decompressed_buffer = malloc(decompressed_size);
    if (!decompressed_buffer)
    {
        free(compressed_buffer);
        fail("Memory allocation #2 failed");
    }

    size_t actual_decompressed_size = ZSTD_decompress(decompressed_buffer, decompressed_size, compressed_buffer, compressed_size);
    if (ZSTD_isError(actual_decompressed_size))
    {
        free(compressed_buffer);
        free(decompressed_buffer);
        fprintf(stderr, "Zstd decompression error: %s\n", ZSTD_getErrorName(actual_decompressed_size));
        fail("Decompression Error");
    }

    free(compressed_buffer);

    if (actual_decompressed_size != decompressed_size)
    {
        free(decompressed_buffer);
        fprintf(stderr, "Actual Decompressed size %lu mismatch with Decompressed size %lu \n", actual_decompressed_size, decompressed_size);
        fail("Decompression Error");
    }

    // Read header information
    unsigned int *memStartAsUINT = (unsigned int *)decompressed_buffer;

    unsigned int *headerSource            = memStartAsUINT + 0;
    unsigned int *bitsperpixelExtSource   = memStartAsUINT + 1;
    unsigned int *channelsExtSource       = memStartAsUINT + 2;
    unsigned int *widthSource             = memStartAsUINT + 3;
    unsigned int *heightSource            = memStartAsUINT + 4;
    unsigned int *bitsperpixelInSource    = memStartAsUINT + 5;
    unsigned int *channelsInSource        = memStartAsUINT + 6;
    unsigned int *checksumSource          = memStartAsUINT + 7;
    unsigned int *compressionConfigSource = memStartAsUINT + 8;
    //unsigned int *unusedSource            = memStartAsUINT + 9;

    // Move from mapped header memory to our local variables
    unsigned int bitsperpixelExt = *bitsperpixelExtSource;
    unsigned int channelsExt     = *channelsExtSource;
    unsigned int width           = *widthSource;
    unsigned int height          = *heightSource;
    unsigned int bitsperpixelIn  = *bitsperpixelInSource;
    unsigned int channelsIn      = *channelsInSource;
    unsigned int compressionCfg  = *compressionConfigSource;

#if PZP_VERBOSE
    fprintf(stderr, "Detected %ux%ux%u@%ubit/", width, height, channelsExt, bitsperpixelExt);
    fprintf(stderr, "%u@%ubit", channelsIn, bitsperpixelIn);
    fprintf(stderr, " | mode %u | CRC:0x%X\n", compressionCfg, *checksumSource);
#endif

    unsigned int runtimeVersion = convert_header(pzp_header);
    if (runtimeVersion != *headerSource)
    {
        free(decompressed_buffer);
        fail("PZP version mismatch stopping to ensure consistency..");
    }

    // Move from our local variables to function output
    *bitsperpixelExternalOutput = bitsperpixelExt;
    *channelsExternalOutput     = channelsExt;
    *widthOutput                = width;
    *heightOutput               = height;
    *bitsperpixelInternalOutput = bitsperpixelIn;
    *channelsInternalOutput     = channelsIn;
    *configuration              = compressionCfg;

    // Copy decompressed data into the reconstructed buffers
    unsigned char *decompressed_bytes = (unsigned char *)decompressed_buffer + headerSize;

    unsigned char *reconstructed = malloc( width * height * (bitsperpixelIn/8)* channelsIn );
    if (reconstructed!=NULL)
         {
           unsigned int restoreRLEChannels = compressionCfg && USE_RLE;
           pzp_extractAndReconstruct(decompressed_bytes, reconstructed, width, height, channelsIn, restoreRLEChannels);
         }

    free(decompressed_buffer);
    return reconstructed;
}

#ifdef __cplusplus
}
#endif

#endif
