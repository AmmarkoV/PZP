#include <stdlib.h>
#include "pzp.h"

/*
 * Exported C API for ctypes / FFI consumers.
 * All functions here have stable, non-static linkage.
 */

unsigned char *pzp_decompress_file(
        const char   *filename,
        unsigned int *width,
        unsigned int *height,
        unsigned int *bpp_ext,
        unsigned int *channels_ext,
        unsigned int *bpp_int,
        unsigned int *channels_int,
        unsigned int *configuration)
{
    return pzp_decompress_combined(filename,
                                   width, height,
                                   bpp_ext, channels_ext,
                                   bpp_int, channels_int,
                                   configuration);
}

void pzp_free(void *ptr)
{
    free(ptr);
}

/*
 * pzp_compress_file — compress raw pixel data to a .pzp file.
 *
 * pixels      : interleaved pixel bytes.
 *               8-bit  → [ch0, ch1, ch2, …] per pixel, 1 byte per channel.
 *               16-bit → [ch0_hi, ch0_lo, ch1_hi, ch1_lo, …] per pixel
 *                        (big-endian, matching PNM byte order).
 * width/height: image dimensions in pixels.
 * bpp         : bits per channel (8 or 16).
 * channels    : number of colour channels (e.g. 1 = grey, 3 = RGB).
 * configuration: bitfield — USE_COMPRESSION (1) | USE_RLE (2).
 * output_filename: path of the .pzp file to write.
 *
 * Returns 1 on success, 0 on failure.
 */
int pzp_compress_file(
        const unsigned char *pixels,
        unsigned int width,
        unsigned int height,
        unsigned int bpp,
        unsigned int channels,
        unsigned int configuration,
        const char   *output_filename)
{
    if (!pixels || !output_filename || width == 0 || height == 0
            || (bpp != 8 && bpp != 16) || channels == 0)
        return 0;

    // 16-bit images are split into two 8-bit internal channels per original channel.
    unsigned int bpp_internal      = (bpp == 16) ? 8  : bpp;
    unsigned int channels_internal = (bpp == 16) ? channels * 2 : channels;

    unsigned char **buffers = malloc(channels_internal * sizeof(unsigned char *));
    if (!buffers)
        return 0;

    for (unsigned int ch = 0; ch < channels_internal; ch++)
    {
        buffers[ch] = malloc(width * height * sizeof(unsigned char));
        if (!buffers[ch])
        {
            for (unsigned int j = 0; j < ch; j++) free(buffers[j]);
            free(buffers);
            return 0;
        }
    }

    pzp_split_channels(pixels, buffers, channels_internal, width, height);

    if (configuration & USE_RLE)
        pzp_RLE_filter(buffers, channels_internal, width, height);

    pzp_compress_combined(buffers, width, height,
                          bpp, channels,
                          bpp_internal, channels_internal,
                          configuration, output_filename);

    for (unsigned int ch = 0; ch < channels_internal; ch++) free(buffers[ch]);
    free(buffers);
    return 1;
}
