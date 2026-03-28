#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pzp.h"
//sudo apt install libzstd-dev

#define PPMREADBUFLEN 256
#define PRINT_COMMENTS 0

static unsigned int simplePowPPM(unsigned int base,unsigned int exp)
{
    if (exp==0) return 1;
    unsigned int retres=base;
    unsigned int i=0;
    for (i=0; i<exp-1; i++)
    {
        retres*=base;
    }
    return retres;
}

static unsigned char * ReadPNM(unsigned char * buffer,const char * filename,unsigned int *width,unsigned int *height,unsigned long * timestamp, unsigned int * bytesPerPixel, unsigned int * channels)
{
    * bytesPerPixel = 0;
    * channels = 0;

    //See http://en.wikipedia.org/wiki/Portable_anymap#File_format_description for this simple and useful format
    unsigned char * pixels=buffer;
    FILE *pf=0;
    pf = fopen(filename,"rb");

    if (pf!=0 )
    {
        *width=0;
        *height=0;
        *timestamp=0;

        char buf[PPMREADBUFLEN]= {0};
        char *t;
        unsigned int w=0, h=0, d=0;
        int r=0, z=0;

        t = fgets(buf, PPMREADBUFLEN, pf);
        if (t == 0)
        {
            fclose(pf);
            return buffer;
        }

        if ( strncmp(buf,"P6\n", 3) == 0 )
        {
            *channels=3;
        }
        else if ( strncmp(buf,"P5\n", 3) == 0 )
        {
            *channels=1;
        }
        else
        {
            fprintf(stderr,"Could not understand/Not supported file format\n");
            fclose(pf);
            return buffer;
        }
        do
        {
            /* Px formats can have # comments after first line */
#if PRINT_COMMENTS
            memset(buf,0,PPMREADBUFLEN);
#endif
            t = fgets(buf, PPMREADBUFLEN, pf);
            if (strstr(buf,"TIMESTAMP")!=0)
            {
                char * timestampPayloadStr = buf + 10;
                *timestamp = atoi(timestampPayloadStr);
            }

            if ( t == 0 )
            {
                fclose(pf);
                return buffer;
            }
        }
        while ( strncmp(buf, "#", 1) == 0 );
        z = sscanf(buf, "%u %u", &w, &h);
        if ( z < 2 )
        {
            fclose(pf);
            fprintf(stderr,"Incoherent dimensions received %ux%u \n",w,h);
            return buffer;
        }
        // The program fails if the first byte of the image is equal to 32. because
        // the fscanf eats the space and the image is read with some bit less
        r = fscanf(pf, "%u\n", &d);
        if (r < 1)
        {
            fprintf(stderr,"Could not understand how many bytesPerPixel there are on this image\n");
            fclose(pf);
            return buffer;
        }
        if (d==255)
        {
            *bytesPerPixel=1;
        }
        else if (d==65535)
        {
            *bytesPerPixel=2;
        }
        else
        {
            fprintf(stderr,"Incoherent payload received %u bits per pixel \n",d);
            fclose(pf);
            return buffer;
        }

        //This is a super ninja hackish patch that fixes the case where fscanf eats one character more on the stream
        //It could be done better  ( no need to fseek ) but this will have to do for now
        //Scan for border case
        unsigned long startOfBinaryPart = ftell(pf);
        if ( fseek (pf, 0, SEEK_END)!=0 )
        {
            fprintf(stderr,"Could not find file size to cache client..!\nUnable to serve client\n");
            fclose(pf);
            return 0;
        }
        unsigned long totalFileSize = ftell (pf); //lSize now holds the size of the file..

        //fprintf(stderr,"totalFileSize-startOfBinaryPart = %u \n",totalFileSize-startOfBinaryPart);
        //fprintf(stderr,"bytesPerPixel*channels*w*h = %u \n",bytesPerPixel*channels*w*h);
        if (totalFileSize-startOfBinaryPart < *bytesPerPixel*(*channels)*w*h )
        {
            fprintf(stderr," Detected Border Case\n\n\n");
            startOfBinaryPart-=1;
        }
        if ( fseek (pf, startOfBinaryPart, SEEK_SET)!=0 )
        {
            fprintf(stderr,"Could not find file size to cache client..!\nUnable to serve client\n");
            fclose(pf);
            return 0;
        }
        //-----------------------
        //----------------------

        *width=w;
        *height=h;
        if (pixels==0)
        {
            pixels= (unsigned char*) malloc(w*h*(*bytesPerPixel)*(*channels)*sizeof(char));
        }

        if ( pixels != 0 )
        {
            size_t rd = fread(pixels,*bytesPerPixel*(*channels), w*h, pf);
            if (rd < w*h )
            {
                fprintf(stderr,"Note : Incomplete read while reading file %s (%u instead of %u)\n",filename,(unsigned int) rd, w*h);
                fprintf(stderr,"Dimensions ( %u x %u ) , Depth %u bytes , Channels %u \n",w,h,*bytesPerPixel,*channels);
            }

            fclose(pf);

#if PRINT_COMMENTS
            if ( (*channels==1) && (*bytesPerPixel==2) && (timestamp!=0) )
            {
                printf("DEPTH %lu\n",*timestamp);
            }
            else if ( (*channels==3) && (*bytesPerPixel==1) && (timestamp!=0) )
            {
                printf("COLOR %lu\n",*timestamp);
            }
#endif

            return pixels;
        }
        else
        {
            fprintf(stderr,"Could not Allocate enough memory for file %s \n",filename);
        }
        fclose(pf);
    }
    else
    {
        fprintf(stderr,"File %s does not exist \n",filename);
    }
    return buffer;
}

static int WritePNM(const char * filename, unsigned char * pixels, unsigned int width, unsigned int height, unsigned int bitsperpixel, unsigned int channels)
{
    if ((width == 0) || (height == 0) || (channels == 0) || (bitsperpixel == 0))
    {
        fprintf(stderr, "saveRawImageToFile(%s) called with zero dimensions ( %ux%u %u channels %u bpp\n", filename, width, height, channels, bitsperpixel);
        return 0;
    }
    if (pixels == 0)
    {
        fprintf(stderr, "saveRawImageToFile(%s) called for an unallocated (empty) frame, will not write any file output\n", filename);
        return 0;
    }
    if (bitsperpixel / channels > 16)
    {
        fprintf(stderr, "PNM does not support more than 2 bytes per pixel..!\n");
        return 0;
    }

    FILE *fd = fopen(filename, "wb");
    if (fd != 0)
    {
        if (channels == 3)
        {
            fprintf(fd, "P6\n");
        }
        else if (channels == 1)
        {
            fprintf(fd, "P5\n");
        }
        else
        {
            fprintf(stderr, "Invalid channels arg (%u) for SaveRawImageToFile\n", channels);
            fclose(fd);
            return 1;
        }

        unsigned int bitsperchannelpixel = bitsperpixel / channels;
        fprintf(fd, "%u %u\n%u\n", width, height, simplePowPPM(2,bitsperchannelpixel) - 1);

        unsigned int n = width * height * channels * (bitsperchannelpixel / 8);

        fwrite(pixels, 1, n, fd);
        fflush(fd);
        fclose(fd);
        return 1;
    }
    else
    {
        fprintf(stderr, "SaveRawImageToFile could not open output file %s\n", filename);
        return 0;
    }
    return 0;
}

/* ── Helpers for compress operations ──────────────────────────────────────── */

static int compress_pnm_to_pzp(const char *input, const char *output, unsigned int configuration)
{
    fprintf(stderr, "Opening %s:", input);

    unsigned char *image = NULL;
    unsigned int width = 0, height = 0, bytesPerPixel = 0, channels = 0;
    unsigned int bitsperpixelInternal = 0, channelsInternal = 0;
    unsigned long timestamp = 0;

    image = ReadPNM(0, input, &width, &height, &timestamp, &bytesPerPixel, &channels);
    unsigned int bitsperpixel = bytesPerPixel * 8;
    fprintf(stderr, "%ux%ux%u@%ubit mode %u\n", width, height, channels, bitsperpixel, configuration);

    bitsperpixelInternal = bitsperpixel;
    channelsInternal     = channels;

    if (bitsperpixel == 16)
    {
        bitsperpixelInternal = 8;
        channelsInternal *= 2;
    }

    if (image == NULL)
        return 0;

    unsigned char **buffers = malloc(channelsInternal * sizeof(unsigned char *));
    if (!buffers) { free(image); return 0; }

    for (unsigned int ch = 0; ch < channelsInternal; ch++)
    {
        buffers[ch] = malloc(width * height * sizeof(unsigned char));
        if (buffers[ch] == NULL)
        {
            fprintf(stderr, "Failed to allocate channel buffer %u\n", ch);
            for (unsigned int j = 0; j < ch; j++) free(buffers[j]);
            free(buffers);
            free(image);
            return 0;
        }
    }

    pzp_split_channels(image, buffers, channelsInternal, width, height);
    pzp_compress_combined(buffers, width, height,
                          bitsperpixel, channels,
                          bitsperpixelInternal, channelsInternal,
                          configuration, output);

    for (unsigned int ch = 0; ch < channelsInternal; ch++) free(buffers[ch]);
    free(buffers);
    free(image);
    return 1;
}

/* Detect audio format from file extension. */
static unsigned int detect_audio_format(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (!dot) return PZP_AUDIO_WAVE;
    const char *ext = dot + 1;
    /* case-insensitive comparison via tolower */
    char low[8] = {0};
    for (int i = 0; i < 7 && ext[i]; i++)
        low[i] = (ext[i] >= 'A' && ext[i] <= 'Z') ? (ext[i] | 0x20) : ext[i];
    if (strcmp(low, "wav")  == 0 || strcmp(low, "wave") == 0) return PZP_AUDIO_WAVE;
    if (strcmp(low, "mp3")  == 0 || strcmp(low, "mpeg") == 0) return PZP_AUDIO_MPEG;
    if (strcmp(low, "ogg")  == 0) return PZP_AUDIO_OGG;
    if (strcmp(low, "flac") == 0) return PZP_AUDIO_FLAC;
    return PZP_AUDIO_WAVE;
}

/* Read an entire binary file to a malloc'd buffer. Sets *size. */
static unsigned char *read_binary_file(const char *filename, size_t *size)
{
    *size = 0;
    FILE *f = fopen(filename, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", filename); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz)
    { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)sz;
    return buf;
}

/* ── Usage ─────────────────────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s compress        <input.pnm>  <output.pzp>  [--lz4]\n"
        "  %s compress-palette <input.pnm> <output.pzp>  [--lz4]\n"
        "  %s pack            <input.pnm>  <output.pzp>  [--lz4]\n"
        "  %s decompress      <input.pzp>  <output.pnm>\n"
        "  %s info            <file.pzp>\n"
        "  %s extract-frame   <file.pzp>  <output.pnm> <frame_index>\n"
        "  %s pack-frames     <output.pzp> <loop_count> <delay_ms> [--delta] [--lz4] <frame1.pnm> [frame2.pnm ...]\n"
        "  %s attach-audio    <input.pzp>  <audio_file> <output.pzp>\n"
        "  %s attach-meta     <input.pzp>  <metadata>   <output.pzp>\n"
        "\n"
        "Codec flags:\n"
        "  --lz4    Use LZ4 instead of ZSTD (faster decompress, larger output)\n"
        "  --delta  Inter-frame delta encoding (better ratio for slow-motion content)\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

/* ── main ──────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *operation = argv[1];

    /* ── compress / pack ───────────────────────────────────────────────────── */

    if (strcmp(operation, "compress")         == 0 ||
        strcmp(operation, "compress-palette") == 0 ||
        strcmp(operation, "pack")             == 0)
    {
        /* Optional --lz4 flag: pzp compress <in> <out> [--lz4] */
        int use_lz4 = (argc == 5 && strcmp(argv[4], "--lz4") == 0);
        if (argc != 4 && !use_lz4) { print_usage(argv[0]); return EXIT_FAILURE; }

        unsigned int configuration = USE_COMPRESSION | USE_RLE;
        if (strcmp(operation, "compress-palette") == 0) configuration |= USE_PALETTE;
        if (strcmp(operation, "pack")             == 0) configuration  = USE_COMPRESSION;
        if (use_lz4)                                    configuration |= USE_LZ4;

        if (!compress_pnm_to_pzp(argv[2], argv[3], configuration))
            return EXIT_FAILURE;
    }

    /* ── decompress ────────────────────────────────────────────────────────── */

    else if (strcmp(operation, "decompress") == 0 ||
             strcmp(operation, "uncompress") == 0)
    {
        if (argc != 4) { print_usage(argv[0]); return EXIT_FAILURE; }

        fprintf(stderr, "Decompress %s\n", argv[2]);

        unsigned int width = 0, height = 0;
        unsigned int bitsperpixelExternal = 0, channelsExternal = 3;
        unsigned int bitsperpixelInternal = 24, channelsInternal = 3;
        unsigned int configuration = 0;

        unsigned char *reconstructed = pzp_decompress_combined(
                argv[2], &width, &height,
                &bitsperpixelExternal, &channelsExternal,
                &bitsperpixelInternal, &channelsInternal, &configuration);

        if (reconstructed != NULL)
        {
            bitsperpixelExternal *= channelsExternal;
            WritePNM(argv[3], reconstructed, width, height,
                     bitsperpixelExternal, channelsExternal);
            free(reconstructed);
        }
        else
        {
            return EXIT_FAILURE;
        }
    }

    /* ── info ──────────────────────────────────────────────────────────────── */

    else if (strcmp(operation, "info") == 0)
    {
        if (argc != 3) { print_usage(argv[0]); return EXIT_FAILURE; }

        PZPContainerHeader hdr;
        PZPFrameEntry *entries = NULL;

        if (!pzp_container_get_info(argv[2], &hdr, &entries))
        {
            fprintf(stderr, "info: failed to parse %s\n", argv[2]);
            return EXIT_FAILURE;
        }

        unsigned int has_meta  = (hdr.container_flags & PZP_CONTAINER_HAS_METADATA) != 0;
        unsigned int has_audio = (hdr.container_flags & PZP_CONTAINER_HAS_AUDIO)    != 0;

        printf("File          : %s\n", argv[2]);
        printf("Format        : PZP Container v%u\n", hdr.version);
        printf("Frames        : %u\n", hdr.frame_count);
        printf("Loop count    : %u%s\n", hdr.loop_count,
               hdr.loop_count == 0 ? " (forever)" : "");
        printf("Metadata      : %s", has_meta ? "yes" : "no");
        if (has_meta) printf(" (%u bytes at offset %u)", hdr.metadata_bytes, hdr.metadata_offset);
        printf("\n");
        printf("Audio         : %s", has_audio ? "yes" : "no");
        if (has_audio)
        {
            char fmt[5] = {0};
            fmt[0] = (char)((hdr.audio_format >> 24) & 0xFF);
            fmt[1] = (char)((hdr.audio_format >> 16) & 0xFF);
            fmt[2] = (char)((hdr.audio_format >>  8) & 0xFF);
            fmt[3] = (char)( hdr.audio_format        & 0xFF);
            printf(" (%u bytes, format=%s, offset=%u)", hdr.audio_bytes, fmt, hdr.audio_offset);
        }
        printf("\n");

        for (unsigned int f = 0; f < hdr.frame_count; f++)
        {
            printf("  Frame %-4u  offset=%-10u  size=%-10u  delay=%ums\n",
                   f, entries[f].frame_offset, entries[f].compressed_size,
                   entries[f].delay_ms);
        }

        free(entries);
    }

    /* ── extract-frame ─────────────────────────────────────────────────────── */

    else if (strcmp(operation, "extract-frame") == 0)
    {
        if (argc != 5) { print_usage(argv[0]); return EXIT_FAILURE; }

        unsigned int frame_index = (unsigned int)atoi(argv[4]);

        unsigned int width = 0, height = 0;
        unsigned int bpp_ext = 0, ch_ext = 0;
        unsigned int bpp_int = 0, ch_int = 0;
        unsigned int configuration = 0;

        unsigned char *pixels = pzp_container_read_frame(
                argv[2], frame_index,
                &width, &height,
                &bpp_ext, &ch_ext,
                &bpp_int, &ch_int,
                &configuration);

        if (!pixels)
        {
            fprintf(stderr, "extract-frame: failed to read frame %u from %s\n",
                    frame_index, argv[2]);
            return EXIT_FAILURE;
        }

        bpp_ext *= ch_ext;
        WritePNM(argv[3], pixels, width, height, bpp_ext, ch_ext);
        free(pixels);
    }

    /* ── pack-frames ───────────────────────────────────────────────────────── */

    else if (strcmp(operation, "pack-frames") == 0)
    {
        /* pzp pack-frames <output.pzp> <loop_count> <delay_ms> [--delta] <frame1> [frame2 ...] */
        if (argc < 6) { print_usage(argv[0]); return EXIT_FAILURE; }

        const char   *output_path  = argv[2];
        unsigned int  loop_count   = (unsigned int)atoi(argv[3]);
        unsigned int  global_delay = (unsigned int)atoi(argv[4]);

        /* Optional --delta / --lz4 flags before the frame list (any order). */
        int use_delta = 0;
        int use_lz4   = 0;
        int frames_argv_start = 5;
        while (frames_argv_start < argc &&
               (strcmp(argv[frames_argv_start], "--delta") == 0 ||
                strcmp(argv[frames_argv_start], "--lz4")   == 0))
        {
            if (strcmp(argv[frames_argv_start], "--delta") == 0) use_delta = 1;
            if (strcmp(argv[frames_argv_start], "--lz4")   == 0) use_lz4   = 1;
            frames_argv_start++;
        }
        if (argc <= frames_argv_start) { print_usage(argv[0]); return EXIT_FAILURE; }

        unsigned int  frame_count  = (unsigned int)(argc - frames_argv_start);
        const char  **frame_paths  = (const char **)(argv + frames_argv_start);

        /* Per-frame arrays */
        unsigned char    ***all_buffers   = (unsigned char ***)calloc(frame_count, sizeof(unsigned char **));
        unsigned int      *widths         = (unsigned int *)malloc(frame_count * sizeof(unsigned int));
        unsigned int      *heights        = (unsigned int *)malloc(frame_count * sizeof(unsigned int));
        unsigned int      *bpp_exts       = (unsigned int *)malloc(frame_count * sizeof(unsigned int));
        unsigned int      *ch_exts        = (unsigned int *)malloc(frame_count * sizeof(unsigned int));
        unsigned int      *bpp_ints       = (unsigned int *)malloc(frame_count * sizeof(unsigned int));
        unsigned int      *ch_ints        = (unsigned int *)malloc(frame_count * sizeof(unsigned int));
        unsigned int      *cfgs           = (unsigned int *)malloc(frame_count * sizeof(unsigned int));
        unsigned int      *delays         = (unsigned int *)malloc(frame_count * sizeof(unsigned int));
        unsigned char    **raw_images     = (unsigned char **)calloc(frame_count, sizeof(unsigned char *));

        if (!all_buffers || !widths || !heights || !bpp_exts || !ch_exts ||
            !bpp_ints || !ch_ints || !cfgs || !delays || !raw_images)
        {
            fprintf(stderr, "pack-frames: allocation failed\n");
            free(all_buffers); free(widths); free(heights);
            free(bpp_exts); free(ch_exts); free(bpp_ints); free(ch_ints);
            free(cfgs); free(delays); free(raw_images);
            return EXIT_FAILURE;
        }

        int ok = 1;
        unsigned int frames_ready = 0;

        for (unsigned int f = 0; f < frame_count && ok; f++)
        {
            unsigned long ts = 0;
            unsigned int bpp_bytes = 0, ch = 0;
            raw_images[f] = ReadPNM(0, frame_paths[f], &widths[f], &heights[f],
                                    &ts, &bpp_bytes, &ch);
            if (!raw_images[f] || widths[f] == 0)
            {
                fprintf(stderr, "pack-frames: failed to read %s\n", frame_paths[f]);
                ok = 0; break;
            }

            unsigned int bpp = bpp_bytes * 8;
            bpp_exts[f] = bpp;
            ch_exts[f]  = ch;
            bpp_ints[f] = (bpp == 16) ? 8 : bpp;
            ch_ints[f]  = (bpp == 16) ? ch * 2 : ch;
            cfgs[f]     = USE_COMPRESSION | USE_RLE
                        | (use_delta ? USE_INTER_DELTA : 0)
                        | (use_lz4   ? USE_LZ4         : 0);
            delays[f]   = global_delay;

            all_buffers[f] = (unsigned char **)calloc(ch_ints[f], sizeof(unsigned char *));
            if (!all_buffers[f]) { ok = 0; break; }
            frames_ready = f + 1;

            for (unsigned int c = 0; c < ch_ints[f]; c++)
            {
                all_buffers[f][c] = (unsigned char *)malloc(widths[f] * heights[f]);
                if (!all_buffers[f][c]) { ok = 0; break; }
            }

            if (ok)
                pzp_split_channels(raw_images[f], all_buffers[f],
                                   ch_ints[f], widths[f], heights[f]);
        }

        if (ok)
        {
            fprintf(stderr, "pack-frames: writing %u frames to %s\n",
                    frame_count, output_path);
            pzp_container_write(output_path, all_buffers,
                                frame_count, widths, heights,
                                bpp_exts, ch_exts, bpp_ints, ch_ints,
                                cfgs, delays, loop_count,
                                NULL, 0, NULL, 0, 0);
        }

        for (unsigned int f = 0; f < frames_ready; f++)
        {
            if (all_buffers[f])
            {
                for (unsigned int c = 0; c < ch_ints[f]; c++) free(all_buffers[f][c]);
                free(all_buffers[f]);
            }
            free(raw_images[f]);
        }
        free(all_buffers); free(widths); free(heights);
        free(bpp_exts); free(ch_exts); free(bpp_ints); free(ch_ints);
        free(cfgs); free(delays); free(raw_images);

        if (!ok) return EXIT_FAILURE;
    }

    /* ── attach-audio ──────────────────────────────────────────────────────── */

    else if (strcmp(operation, "attach-audio") == 0)
    {
        /* pzp attach-audio <input.pzp> <audio_file> <output.pzp> */
        if (argc != 5) { print_usage(argv[0]); return EXIT_FAILURE; }

        size_t audio_size = 0;
        unsigned char *audio_data = read_binary_file(argv[3], &audio_size);
        if (!audio_data)
        {
            fprintf(stderr, "attach-audio: cannot read %s\n", argv[3]);
            return EXIT_FAILURE;
        }

        unsigned int fmt = detect_audio_format(argv[3]);
        int rc = pzp_container_attach(argv[2], argv[4],
                                      NULL, 0,
                                      audio_data, (unsigned int)audio_size, fmt);
        free(audio_data);
        if (!rc)
        {
            fprintf(stderr, "attach-audio: failed\n");
            return EXIT_FAILURE;
        }
        fprintf(stderr, "attach-audio: wrote %s\n", argv[4]);
    }

    /* ── attach-meta ───────────────────────────────────────────────────────── */

    else if (strcmp(operation, "attach-meta") == 0)
    {
        /* pzp attach-meta <input.pzp> <metadata_string_or_file> <output.pzp> */
        if (argc != 5) { print_usage(argv[0]); return EXIT_FAILURE; }

        const unsigned char *meta_data  = (const unsigned char *)argv[3];
        unsigned int         meta_bytes = (unsigned int)strlen(argv[3]);

        int rc = pzp_container_attach(argv[2], argv[4],
                                      meta_data, meta_bytes,
                                      NULL, 0, 0);
        if (!rc)
        {
            fprintf(stderr, "attach-meta: failed\n");
            return EXIT_FAILURE;
        }
        fprintf(stderr, "attach-meta: wrote %s\n", argv[4]);
    }

    /* ── unknown operation ─────────────────────────────────────────────────── */

    else
    {
        fprintf(stderr, "Unknown operation: %s\n", operation);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
