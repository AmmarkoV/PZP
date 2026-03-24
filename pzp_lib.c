#include <stdlib.h>
#include <string.h>
#include "pzp.h"

/*
 * Exported C API for ctypes / FFI consumers.
 * All functions here have stable, non-static linkage.
 */

/* ── Single-frame decompressor ─────────────────────────────────────────────── */

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

/* ── Single-frame compressor ───────────────────────────────────────────────── */

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

    pzp_compress_combined(buffers, width, height,
                          bpp, channels,
                          bpp_internal, channels_internal,
                          configuration, output_filename);

    for (unsigned int ch = 0; ch < channels_internal; ch++) free(buffers[ch]);
    free(buffers);
    return 1;
}

/* ── Container API ─────────────────────────────────────────────────────────── */

/*
 * pzp_container_frame_count — return the number of frames in a container.
 * Returns 0 if the file is not a container or on error.
 */
unsigned int pzp_container_frame_count(const char *filename)
{
    PZPContainerHeader hdr;
    PZPFrameEntry *entries = NULL;
    if (!pzp_container_get_info(filename, &hdr, &entries))
        return 0;
    unsigned int n = hdr.frame_count;
    free(entries);
    return n;
}

/*
 * pzp_container_get_frame — decompress frame frame_index from a container.
 * Same return convention as pzp_decompress_file; caller frees with pzp_free().
 */
unsigned char *pzp_container_get_frame(
        const char   *filename,
        unsigned int  frame_index,
        unsigned int *width,
        unsigned int *height,
        unsigned int *bpp_ext,
        unsigned int *channels_ext,
        unsigned int *bpp_int,
        unsigned int *channels_int,
        unsigned int *configuration)
{
    return pzp_container_read_frame(filename, frame_index,
                                    width, height,
                                    bpp_ext, channels_ext,
                                    bpp_int, channels_int,
                                    configuration);
}

/*
 * pzp_container_read_metadata — return the metadata blob from a container.
 * Sets *bytes_out. Caller frees with pzp_free(). Returns NULL if absent.
 */
unsigned char *pzp_container_read_metadata(
        const char   *filename,
        unsigned int *bytes_out)
{
    return pzp_container_get_metadata(filename, bytes_out);
}

/*
 * pzp_container_read_audio — return the audio blob from a container.
 * Sets *bytes_out and *format_out. Caller frees with pzp_free(). Returns NULL if absent.
 */
unsigned char *pzp_container_read_audio(
        const char   *filename,
        unsigned int *bytes_out,
        unsigned int *format_out)
{
    return pzp_container_get_audio(filename, bytes_out, format_out);
}

/*
 * pzp_write_frames — compress multiple frames and write a PZP container file.
 *
 * pixel_arrays  : array of frame_count pointers to interleaved pixel data.
 * widths/heights: per-frame image dimensions in pixels.
 * bpps          : per-frame bits per channel (8 or 16).
 * channels_arr  : per-frame channel count.
 * configurations: per-frame bitfield (USE_COMPRESSION | USE_RLE | USE_PALETTE).
 * delay_ms_arr  : per-frame display delay in ms; NULL = all zeros.
 * loop_count    : animation loop count; 0 = loop forever.
 * metadata/metadata_bytes : opaque blob to embed; NULL to omit.
 * audio/audio_bytes/audio_format : raw audio bytes and PZP_AUDIO_* tag; NULL to omit.
 *
 * Returns 1 on success, 0 on failure.
 */
int pzp_write_frames(
        const char          *output_filename,
        const unsigned char **pixel_arrays,
        unsigned int          frame_count,
        unsigned int         *widths,
        unsigned int         *heights,
        unsigned int         *bpps,
        unsigned int         *channels_arr,
        unsigned int         *configurations,
        unsigned int         *delay_ms_arr,
        unsigned int          loop_count,
        const unsigned char  *metadata,
        unsigned int          metadata_bytes,
        const unsigned char  *audio,
        unsigned int          audio_bytes,
        unsigned int          audio_format)
{
    if (!pixel_arrays || !output_filename || frame_count == 0)
        return 0;

    unsigned char ***all_buffers = (unsigned char ***)calloc(frame_count, sizeof(unsigned char **));
    unsigned int *bpp_ints  = (unsigned int *)malloc(frame_count * sizeof(unsigned int));
    unsigned int *ch_ints   = (unsigned int *)malloc(frame_count * sizeof(unsigned int));
    unsigned int *bpp_exts  = (unsigned int *)malloc(frame_count * sizeof(unsigned int));
    unsigned int *ch_exts   = (unsigned int *)malloc(frame_count * sizeof(unsigned int));
    unsigned int *delay_arr = (unsigned int *)calloc(frame_count, sizeof(unsigned int));

    if (!all_buffers || !bpp_ints || !ch_ints || !bpp_exts || !ch_exts || !delay_arr)
    {
        free(all_buffers); free(bpp_ints); free(ch_ints);
        free(bpp_exts); free(ch_exts); free(delay_arr);
        return 0;
    }

    if (delay_ms_arr)
        memcpy(delay_arr, delay_ms_arr, frame_count * sizeof(unsigned int));

    int ok = 1;
    unsigned int frames_ready = 0;

    for (unsigned int f = 0; f < frame_count && ok; f++)
    {
        unsigned int bpp = bpps[f], ch = channels_arr[f];
        bpp_exts[f] = bpp;
        ch_exts[f]  = ch;
        bpp_ints[f] = (bpp == 16) ? 8 : bpp;
        ch_ints[f]  = (bpp == 16) ? ch * 2 : ch;

        all_buffers[f] = (unsigned char **)calloc(ch_ints[f], sizeof(unsigned char *));
        if (!all_buffers[f]) { ok = 0; break; }
        frames_ready = f + 1;

        for (unsigned int c = 0; c < ch_ints[f]; c++)
        {
            all_buffers[f][c] = (unsigned char *)malloc(widths[f] * heights[f]);
            if (!all_buffers[f][c]) { ok = 0; break; }
        }

        if (ok)
            pzp_split_channels(pixel_arrays[f], all_buffers[f],
                               ch_ints[f], widths[f], heights[f]);
    }

    if (ok)
        pzp_container_write(output_filename, all_buffers,
                            frame_count, widths, heights,
                            bpp_exts, ch_exts, bpp_ints, ch_ints,
                            configurations, delay_arr, loop_count,
                            metadata, metadata_bytes,
                            audio, audio_bytes, audio_format);

    for (unsigned int f = 0; f < frames_ready; f++)
    {
        if (all_buffers[f])
        {
            for (unsigned int c = 0; c < ch_ints[f]; c++) free(all_buffers[f][c]);
            free(all_buffers[f]);
        }
    }
    free(all_buffers); free(bpp_ints); free(ch_ints);
    free(bpp_exts); free(ch_exts); free(delay_arr);
    return ok;
}
