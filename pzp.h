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
#include <emmintrin.h>  // SSE2
#include <stdint.h>
//#warning "Intel Optimizations Enabled"
#endif // INTEL_OPTIMIZATIONS

#define PZP_VERBOSE 0

static const char pzp_version[]="v0.01";
static const char pzp_header[4]={"PZP0"};

static const int headerSize =  sizeof(unsigned int) * 10;
//header, width, height, bitsperpixel, channels, internalbitsperpixel, internalchannels, checksum, compression_mode, unused


#define NORMAL   "\033[0m"
#define BLACK   "\033[30m"      /* Black */
#define RED     "\033[31m"      /* Red */
#define GREEN   "\033[32m"      /* Green */
#define YELLOW  "\033[33m"      /* Yellow */

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
  fprintf(stderr,RED "PZP Fatal Error: %s\n" NORMAL,message);
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


static void * pzp_read_file_to_memory(const char *filename, size_t *fileSize)
{
    FILE *fp = fopen(filename, "rb");
    if (!fp)
       {
        fprintf(stderr,"Failed to open file");
        return NULL;
       }

    if (fseek(fp, 0, SEEK_END) != 0)
       {
        fprintf(stderr,"Failed to seek file");
        fclose(fp);
        return NULL;
       }

    long file_size = ftell(fp);
    if (file_size < 0)
       {
        fprintf(stderr,"Failed to tell file size");
        fclose(fp);
        return NULL;
       }
    rewind(fp);

    void *buffer = malloc(file_size);
    if (!buffer)
      {
        fprintf(stderr,"Failed to allocate memory");
        fclose(fp);
        return NULL;
       }

    size_t read_size = fread(buffer, 1, file_size, fp);
    if (read_size != (size_t)file_size)
       {
        fprintf(stderr,"Failed to read file completely");
        free(buffer);
        fclose(fp);
        return NULL;
       }

    fclose(fp);
    if (fileSize) *fileSize = read_size;
    return buffer;
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
//----------------------------------------------------------------------------------------------
#if INTEL_OPTIMIZATIONS
//This function is ChatGPT generated and is crappy and incorrect
static void pzp_extractAndReconstruct_SSE2(unsigned char *decompressed_bytes, unsigned char *reconstructed, unsigned int width, unsigned int height, unsigned int channels, int restoreRLEChannels)
{
    fprintf(stderr,YELLOW "pzp_extractAndReconstruct_SSE2 is incorrect..\n" NORMAL);
    unsigned int total_size = width * height;
    unsigned char *src = decompressed_bytes;
    unsigned char *r   = reconstructed;

    if (restoreRLEChannels)
    {
        switch (channels)
        {
            case 1:
            {
                __m128i prev = _mm_setzero_si128();
                unsigned int i = 0;

                // Process 16 bytes at a time
                for (; i + 15 < total_size; i += 16)
                {
                    __m128i current = _mm_loadu_si128((__m128i*)(src + i));
                    __m128i shifted = _mm_slli_si128(current, 1);
                    __m128i result = _mm_add_epi8(current, _mm_add_epi8(shifted, prev));
                    _mm_storeu_si128((__m128i*)(r + i), result);
                    prev = _mm_srli_si128(result, 15);
                }

                // Process remaining bytes
                for (; i < total_size; i++)
                {
                    r[i] = src[i] + r[i - 1];
                }
                break;
            }
            case 2:
            {
                __m128i prev = _mm_setzero_si128();
                unsigned int i = 0;

                // Process 8 pixels (16 bytes) at a time
                for (; i + 7 < total_size; i += 8)
                {
                    __m128i current = _mm_loadu_si128((__m128i*)(src + i * 2));
                    __m128i shifted = _mm_slli_si128(current, 2);
                    __m128i result = _mm_add_epi16(current, _mm_add_epi16(shifted, prev));
                    _mm_storeu_si128((__m128i*)(r + i * 2), result);
                    prev = _mm_srli_si128(result, 14);
                }

                // Process remaining pixels
                for (; i < total_size; i++)
                {
                    r[i * 2]     = src[i * 2]     + r[(i - 1) * 2];
                    r[i * 2 + 1] = src[i * 2 + 1] + r[(i - 1) * 2 + 1];
                }
                break;
            }
            case 3:
            {
                // SSE2 is less efficient for 3-channel data due to alignment issues
                // Fall back to the naive implementation for 3 channels
                r[0] = src[0];
                r[1] = src[1];
                r[2] = src[2];
                for (unsigned int i = 1; i < total_size; i++)
                {
                    r   += 3;
                    src += 3;
                    r[0] = src[0] + r[-3];
                    r[1] = src[1] + r[-2];
                    r[2] = src[2] + r[-1];
                }
                break;
            }
            default:
            {
                // Fall back to the naive implementation for other channel counts
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
    }
    else // Non-RLE path
    {
        switch (channels)
        {
            case 1:
                memcpy(reconstructed, src, total_size);
                break;
            case 2:
            {
                unsigned int i = 0;

                // Process 16 bytes (8 pixels) at a time
                for (; i + 7 < total_size; i += 8)
                {
                    __m128i current = _mm_loadu_si128((__m128i*)(src + i * 2));
                    _mm_storeu_si128((__m128i*)(r + i * 2), current);
                }

                // Process remaining pixels
                for (; i < total_size; i++)
                {
                    r[i * 2]     = src[i * 2];
                    r[i * 2 + 1] = src[i * 2 + 1];
                }
                break;
            }
            case 3:
            {
                // Fall back to the naive implementation for 3 channels
                for (unsigned int i = 0; i < total_size; i++)
                {
                    *r = *src;
                    r++; src++;
                    *r = *src;
                    r++; src++;
                    *r = *src;
                    r++; src++;
                }
                break;
            }
            default:
            {
                // Fall back to the naive implementation for other channel counts
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
}


static void pzp_memcpy_avx2(unsigned char *dst, unsigned char *src, unsigned int size)
{
    unsigned int i = 0;
    __m256i v;

    // Process 32 bytes at a time
    for (; i + 31 < size; i += 32)
    {
        v = _mm256_loadu_si256((__m256i *)(src + i));
        _mm256_storeu_si256((__m256i *)(dst + i), v);
    }

    // Process remaining bytes
    for (; i < size; i++)
    {
        dst[i] = src[i];
    }
}

/**
 * @brief Computes the prefix sum of an array using AVX2 SIMD operations.
 *
 * This function processes an array of unsigned 8-bit integers (bytes) in chunks of 32 bytes at a time,
 * using AVX2 intrinsics to perform vectorized addition. The prefix sum means that each element in the
 * output is the sum of all previous elements including itself, i.e.,
 *
 *      dst[i] = src[i] + dst[i-1]
 *
 * The algorithm leverages SIMD parallelism for faster execution by processing 32 elements in a single iteration.
 *
 * @param src  Pointer to the source array of unsigned 8-bit integers.
 * @param dst  Pointer to the destination array where the computed prefix sum will be stored.
 * @param size Number of elements in the source array. Must be a multiple of 32 for best performance.
 * @note
 * - The function assumes `size` is a multiple of 32 for optimal performance. If not, a scalar fallback is needed.
 * - This implementation is efficient for **short sequences** but not ideal for very long sequences,
 *   as it does not fully exploit SIMD-friendly prefix sum techniques like **Hillis-Steele scan**.
 * - If processing very large arrays, consider a **two-pass approach** to propagate values properly across blocks.
 * - Works best when `src` and `dst` are **aligned** to 32-byte boundaries, though `_mm256_loadu_si256`
 *   handles unaligned memory safely but slightly slower than aligned `_mm256_load_si256`.
 */
static void pzp_prefix_sum_avx2(unsigned char *src, unsigned char *dst, unsigned int size)
{
 //This function is ChatGPT generated and is crappy and incorrect
   // Initialize previous sum to zero
    __m256i prev = _mm256_setzero_si256();  // Holds the last accumulated sum across iterations
    __m256i v, sum;  // Temporary variables for SIMD operations

    // Process the array in chunks of 32 bytes (AVX2 register width)
    for (unsigned int i = 0; i < size; i += 32)
    {
        // Load 32 bytes from the source array into an AVX2 register
        v = _mm256_loadu_si256((__m256i *)(src + i));

        // Compute the prefix sum for this block by adding the previous sum
        sum = _mm256_add_epi8(v, prev);

        // Store the result back into the destination array
        _mm256_storeu_si256((__m256i *)(dst + i), sum);

        // Update `prev` to propagate the last element for the next block
        // `_mm256_permute2x128_si256` extracts the high 128-bit half and moves it to low half
        prev = _mm256_permute2x128_si256(sum, sum, 0x11);  // Shuffle the last 16 bytes
    }
}

/**
 * @brief Computes the prefix sum for a 2-channel interleaved array using AVX2 SIMD operations.
 *
 * This function processes an array where two interleaved unsigned 8-bit integer channels are present.
 * It computes the prefix sum separately for each channel while preserving their interleaved layout.
 * The prefix sum ensures that:
 *
 *      dst[2*i]   = src[2*i]   + dst[2*i - 2]
 *      dst[2*i+1] = src[2*i+1] + dst[2*i - 1]
 *
 * The function uses AVX2 to process 32 elements (16 pairs of channels) per iteration, improving performance.
 *
 * @param src  Pointer to the source array of unsigned 8-bit integers (interleaved 2-channel format).
 * @param dst  Pointer to the destination array where the computed prefix sum will be stored.
 * @param size Number of interleaved channel pairs in the source array (not the byte size).
 * @note
 * - The function assumes `size` is a multiple of 16 for optimal performance. If not, a scalar fallback is needed.
 * - This implementation works efficiently for small to medium-sized sequences but does not fully optimize
 *   long sequences where more sophisticated prefix sum algorithms (such as Hillis-Steele scan) may be required.
 * - Works best when `src` and `dst` are **aligned** to 32-byte boundaries, although `_mm256_loadu_si256`
 *   allows for unaligned memory access at a slight performance cost.
 */
static void pzp_prefix_sum_avx2_2ch(unsigned char *src, unsigned char *dst, unsigned int size)
{
 //This function is ChatGPT generated and is crappy and incorrect
   // Initialize previous sum to zero for both channels
    __m256i prev0 = _mm256_setzero_si256();  // Holds the last accumulated sum for channel 0
    __m256i prev1 = _mm256_setzero_si256();  // Holds the last accumulated sum for channel 1
    __m256i v0, v1, sum0, sum1;

    // Process the array in chunks of 16 pairs (32 elements total, 16 per channel)
    for (unsigned int i = 0; i < size; i += 16)
    {
        // Load 16 pairs (32 bytes total) from the source array into AVX2 registers
        v0 = _mm256_loadu_si256((__m256i *)(src + 2 * i));       // Load channel 0 data
        v1 = _mm256_loadu_si256((__m256i *)(src + 2 * i + 32));  // Load channel 1 data

        // Compute prefix sum by adding the previous accumulated sum
        sum0 = _mm256_add_epi8(v0, prev0);
        sum1 = _mm256_add_epi8(v1, prev1);

        // Perform horizontal prefix sum within each register
        sum0 = _mm256_add_epi8(sum0, _mm256_slli_si256(sum0, 1));
        sum0 = _mm256_add_epi8(sum0, _mm256_slli_si256(sum0, 2));
        sum0 = _mm256_add_epi8(sum0, _mm256_slli_si256(sum0, 4));
        sum0 = _mm256_add_epi8(sum0, _mm256_slli_si256(sum0, 8));
        sum0 = _mm256_add_epi8(sum0, _mm256_slli_si256(sum0, 16));

        sum1 = _mm256_add_epi8(sum1, _mm256_slli_si256(sum1, 1));
        sum1 = _mm256_add_epi8(sum1, _mm256_slli_si256(sum1, 2));
        sum1 = _mm256_add_epi8(sum1, _mm256_slli_si256(sum1, 4));
        sum1 = _mm256_add_epi8(sum1, _mm256_slli_si256(sum1, 8));
        sum1 = _mm256_add_epi8(sum1, _mm256_slli_si256(sum1, 16));

        // Store the results back into the destination array
        _mm256_storeu_si256((__m256i *)(dst + 2 * i), sum0);
        _mm256_storeu_si256((__m256i *)(dst + 2 * i + 32), sum1);

        // Update prev0 and prev1 with the last elements for the next batch
        prev0 = _mm256_permute2x128_si256(sum0, sum0, 0x11);
        prev1 = _mm256_permute2x128_si256(sum1, sum1, 0x11);
    }
}


//This is buggy :
static void pzp_extractAndReconstruct_AVX2(unsigned char *decompressed_bytes, unsigned char *reconstructed, unsigned int width, unsigned int height, unsigned int channels, int restoreRLEChannels)
{
 //This function is ChatGPT generated and is crappy and incorrect
   fprintf(stderr,YELLOW "pzp_extractAndReconstruct_AVX2 is incorrect..\n" NORMAL);
    unsigned int total_size = width * height;
    unsigned char *src = decompressed_bytes;
    unsigned char *r = reconstructed;

    if (restoreRLEChannels)
    {
        switch (channels)
        {
            case 1: {
                // Handle RLE for 1 channel
                r[0] = src[0];
                unsigned int i = 1;
                // Process 32 elements at a time
                for (; i + 31 < total_size; i += 32)
                {
                    __m256i prev = _mm256_loadu_si256((__m256i*)(r + i - 1));
                    __m256i current = _mm256_loadu_si256((__m256i*)(src + i));
                    // Shift previous elements right by 1 byte and add
                    __m256i shifted_prev = _mm256_srli_si256(prev, 1);
                    __m256i result = _mm256_add_epi8(current, shifted_prev);
                    // Propagate carry through the vector
                    result = _mm256_add_epi8(result, _mm256_slli_si256(result, 1));
                    result = _mm256_add_epi8(result, _mm256_slli_si256(result, 2));
                    result = _mm256_add_epi8(result, _mm256_slli_si256(result, 4));
                    result = _mm256_add_epi8(result, _mm256_slli_si256(result, 8));
                    _mm256_storeu_si256((__m256i*)(r + i), result);
                }
                // Remaining elements
                for (; i < total_size; ++i) {
                    r[i] = src[i] + r[i - 1];
                }
                break;
            }
            case 2: {
                pzp_prefix_sum_avx2_2ch(src,r,total_size);
                break;
            }
            case 3: {
                // Handle RLE for 3 channels (scalar fallback)
                r[0] = src[0];
                r[1] = src[1];
                r[2] = src[2];
                for (unsigned int i = 1; i < total_size; ++i)
                {
                    r += 3;
                    src += 3;
                    r[0] = src[0] + r[-3];
                    r[1] = src[1] + r[-2];
                    r[2] = src[2] + r[-1];
                }
                break;
            }
            default: {
                // Generic case (scalar fallback)
                for (unsigned int ch = 0; ch < channels; ++ch)
                {
                    r[ch] = src[ch];
                }
                for (unsigned int i = 1; i < total_size; ++i)
                {
                    for (unsigned int ch = 0; ch < channels; ++ch)
                    {
                        r[i * channels + ch] = src[i * channels + ch] + r[(i - 1) * channels + ch];
                    }
                }
                break;
            }
        }
    } else
    {
        // Non-RLE path
        switch (channels)
        {
            case 1:
                memcpy(r, src, total_size);
                break;
            case 2:
            {
                // Copy 32 bytes at a time (16 pixels)
                unsigned int i = 0;
                for (; i + 15 < total_size; i += 16)
                {
                    __m256i data = _mm256_loadu_si256((__m256i*)(src + 2 * i));
                    _mm256_storeu_si256((__m256i*)(r + 2 * i), data);
                }
                // Remaining elements
                for (; i < total_size; ++i)
                {
                    r[2 * i] = src[2 * i];
                    r[2 * i + 1] = src[2 * i + 1];
                }
                break;
            }
            case 3:
            {
                // Copy 24 bytes at a time (8 pixels)
                unsigned int i = 0;
                for (; i + 7 < total_size; i += 8)
                {
                    __m256i data = _mm256_loadu_si256((__m256i*)(src + 3 * i));
                    _mm256_storeu_si256((__m256i*)(r + 3 * i), data);
                }
                // Remaining elements
                for (; i < total_size; ++i)
                {
                    r[3 * i]     = src[3 * i];
                    r[3 * i + 1] = src[3 * i + 1];
                    r[3 * i + 2] = src[3 * i + 2];
                }
                break;
            }
            default: {
                // Generic case (scalar fallback)
                for (unsigned int i = 0; i < total_size; ++i)
                {
                    for (unsigned int ch = 0; ch < channels; ++ch)
                    {
                        r[i * channels + ch] = src[i * channels + ch];
                    }
                }
                break;
            }
        }
    }
}
#endif // INTEL_OPTIMIZATIONS
static void pzp_extractAndReconstruct_Naive(unsigned char *decompressed_bytes, unsigned char *reconstructed, unsigned int width, unsigned int height, unsigned int channels, int restoreRLEChannels)
{
    unsigned int total_size = width * height;
    unsigned char *src = decompressed_bytes;
    unsigned char *r   = reconstructed;

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
                    r   += 2;
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
                    r   += 3;
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
                    *r = *src;
                    r++; src++;
                    *r = *src;
                    r++; src++;
                }
                break;
            case 3:
                for (unsigned int i = 0; i < total_size; i++)
                {
                    *r = *src;
                    r++; src++;
                    *r = *src;
                    r++; src++;
                    *r = *src;
                    r++; src++;
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
static void pzp_extractAndReconstruct(unsigned char *decompressed_bytes, unsigned char *reconstructed, unsigned int width, unsigned int height, unsigned int channels, int restoreRLEChannels)
{
   // Force Naive implementation since AVX2 does not produce accurate results (yet)
   pzp_extractAndReconstruct_Naive(decompressed_bytes,reconstructed,width,height,channels,restoreRLEChannels);
   return;

   #if INTEL_OPTIMIZATIONS
     //pzp_extractAndReconstruct_SSE2(decompressed_bytes,reconstructed,width,height,channels,restoreRLEChannels);
     pzp_extractAndReconstruct_AVX2(decompressed_bytes,reconstructed,width,height,channels,restoreRLEChannels);
   #else
     pzp_extractAndReconstruct_Naive(decompressed_bytes,reconstructed,width,height,channels,restoreRLEChannels);
   #endif // INTEL_OPTIMIZATIONS
}
//-----------------------------------------------------------------------------------------------
static unsigned char* pzp_decompress_combined_from_memory(
                                const void *file_data, size_t file_size,
                                unsigned int *widthOutput, unsigned int *heightOutput,
                                unsigned int *bitsperpixelExternalOutput, unsigned int *channelsExternalOutput,
                                unsigned int *bitsperpixelInternalOutput, unsigned int *channelsInternalOutput,
                                unsigned int *configuration)
{
    if (!file_data || file_size <= sizeof(unsigned int))
    {
        fprintf(stderr, "Invalid file data or size\n");
        return NULL;
    }

    const unsigned char *input_ptr = (const unsigned char *)file_data;

    // Read stored size
    unsigned int dataSize;
    memcpy(&dataSize, input_ptr, sizeof(unsigned int));

    if (dataSize == 0 || dataSize > 100000000)
    { // sanity check
        fprintf(stderr, "Error: Invalid size read from memory (%u)\n", dataSize);
        return NULL;
    }

    size_t compressed_size = file_size - sizeof(unsigned int);
    const void *compressed_buffer = input_ptr + sizeof(unsigned int);

    size_t decompressed_size = (size_t)dataSize;
    void *decompressed_buffer = malloc(decompressed_size);
    if (!decompressed_buffer)
    {
        //free(compressed_buffer);
        //fail("Memory allocation #2 failed");
        return 0;
    }



    size_t actual_decompressed_size = ZSTD_decompress(decompressed_buffer, decompressed_size, compressed_buffer, compressed_size);
    if (ZSTD_isError(actual_decompressed_size))
    {
        //free(compressed_buffer);
        free(decompressed_buffer);
        fprintf(stderr, "Zstd decompression error: %s\n", ZSTD_getErrorName(actual_decompressed_size));
        //fail("Decompression Error");
        return 0;
    }

    //free(compressed_buffer);

    if (actual_decompressed_size != decompressed_size)
    {
        free(decompressed_buffer);
        fprintf(stderr, "Actual Decompressed size %lu mismatch with Decompressed size %lu \n", actual_decompressed_size, decompressed_size);
        //fail("Decompression Error");
        return 0;
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
#if PZP_VERBOSE
    unsigned int *checksumSource          = memStartAsUINT + 7;
#endif
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
        //fail("PZP version mismatch stopping to ensure consistency..");
        return 0;
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
           unsigned int restoreRLEChannels = compressionCfg & USE_RLE;
           pzp_extractAndReconstruct(decompressed_bytes, reconstructed, width, height, channelsIn, restoreRLEChannels);
         }

    free(decompressed_buffer);
    return reconstructed;
}


static unsigned char* pzp_decompress_combined(const char *input_filename,
                                unsigned int *widthOutput, unsigned int *heightOutput,
                                unsigned int *bitsperpixelExternalOutput, unsigned int *channelsExternalOutput,
                                unsigned int *bitsperpixelInternalOutput, unsigned int *channelsInternalOutput,
                                unsigned int *configuration)
{
    size_t file_size = 0;
    void *file_data = pzp_read_file_to_memory(input_filename, &file_size);
    if (file_data!=NULL)
    {
      unsigned char *result = pzp_decompress_combined_from_memory(
                                                                    file_data, file_size,
                                                                    widthOutput, heightOutput,
                                                                    bitsperpixelExternalOutput, channelsExternalOutput,
                                                                    bitsperpixelInternalOutput, channelsInternalOutput,
                                                                    configuration
                                                                  );

      free(file_data);
      return result;
    }

    fprintf(stderr, "Failed to read file: %s\n", input_filename);
    return NULL;
}

#ifdef __cplusplus
}
#endif

#endif
