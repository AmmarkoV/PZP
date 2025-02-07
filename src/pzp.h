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

#define PPMREADBUFLEN 256

#define PZP_VERBOSE 1
#define PRINT_COMMENTS 0


static const char pzp_version[]="v0.0";
static const char pzp_header[4]={"PZP0"};

static const int headerSize =  sizeof(unsigned int) * 8; //header, width,height,bitsperpixel,channels, internalbitsperpixel, internalchannels, checksum

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

/*
static int swapDepthEndianness(unsigned char * pixels,unsigned int width,unsigned int height,unsigned int bitsperpixel, unsigned int channels)
{
    if (pixels==0)
    {
        return 0;
    }
    if (bitsperpixel!=16)
    {
        fprintf(stderr,"Only 16bit PNM files need swapping ( we have a %u bit x %u channels file )..\n",bitsperpixel, channels);
        return 0;
    }

    unsigned char * traverser=(unsigned char * ) pixels;
    unsigned char * traverserSwap1;//=(unsigned char * ) pixels;
    unsigned char * traverserSwap2;//=(unsigned char * ) pixels;

    unsigned int bytesperpixel = (bitsperpixel/8);
    unsigned char * endOfMem = traverser + width * height * channels * bytesperpixel;

    while ( ( traverser < endOfMem)  )
    {
        traverserSwap1 = traverser;
        traverserSwap2 = traverser+1;

        unsigned char tmp = *traverserSwap1;
        *traverserSwap1 = *traverserSwap2;
        *traverserSwap2 = tmp;

        traverser += bytesperpixel;
    }

    return 1;
}

static int swap_endianness(int value)
{
    return ((value >> 24) & 0xFF) | ((value >> 8) & 0xFF00) | ((value << 8) & 0xFF0000) | ((value << 24) & 0xFF000000);
}
*/

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


static unsigned int hash_checksum(const void *data, size_t dataSize)
{
    const unsigned char *bytes = (const unsigned char *)data;
    unsigned int h1 = 0x12345678, h2 = 0x9ABCDEF0, h3 = 0xFEDCBA98, h4 = 0x87654321;

    while (dataSize >= 4) {
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


static void split_channels_and_filter(const unsigned char *image, unsigned char **buffers, int num_buffers, int WIDTH, int HEIGHT)
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

    // Apply left-pixel delta filtering
    for (int i = total_size - 1; i > 0; i--)
    {
        for (int ch = 0; ch < num_buffers; ch++)
        {
            buffers[ch][i] -= buffers[ch][i - 1];
        }
    }
}

static void restore_channels(unsigned char **buffers, int num_buffers, int WIDTH, int HEIGHT)
{
    int total_size = WIDTH * HEIGHT;
    for (int i = 1; i < total_size; i++)
    {
        for (int ch = 0; ch < num_buffers; ch++)
        {
            buffers[ch][i] += buffers[ch][i - 1];
        }
    }
}

static void compress_combined(unsigned char **buffers,
                              unsigned int width,unsigned int height,
                              unsigned int bitsperpixelExternal, unsigned int channelsExternal,
                              unsigned int bitsperpixelInternal, unsigned int channelsInternal,
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
    //---------------------------------------------------------------------------------------------------

    //Store data to their target location
    *headerTarget       = convert_header(pzp_header);
    *bitsperpixelTarget = bitsperpixelExternal;
    *channelsTarget     = channelsExternal;
    *widthTarget        = width;
    *heightTarget       = height;
    *bitsperpixelInternalTarget = bitsperpixelInternal;
    *channelsInternalTarget     = channelsInternal;

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
    fprintf(stderr, " | CRC:0x%X\n",*checksumTarget);
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

static void decompress_combined(const char *input_filename, unsigned char ***buffers,
                                unsigned int *widthOutput, unsigned int *heightOutput,
                                unsigned int *bitsperpixelExternalOutput, unsigned int *channelsExternalOutput,
                                unsigned int *bitsperpixelInternalOutput, unsigned int *channelsInternalOutput)
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

    unsigned int *headerSource          = memStartAsUINT + 0;
    unsigned int *bitsperpixelExtSource = memStartAsUINT + 1;
    unsigned int *channelsExtSource     = memStartAsUINT + 2;
    unsigned int *widthSource           = memStartAsUINT + 3;
    unsigned int *heightSource          = memStartAsUINT + 4;
    unsigned int *bitsperpixelInSource  = memStartAsUINT + 5;
    unsigned int *channelsInSource      = memStartAsUINT + 6;
    unsigned int *checksumSource        = memStartAsUINT + 7;

    // Move from mapped header memory to our local variables
    unsigned int bitsperpixelExt = *bitsperpixelExtSource;
    unsigned int channelsExt     = *channelsExtSource;
    unsigned int width           = *widthSource;
    unsigned int height          = *heightSource;
    unsigned int bitsperpixelIn  = *bitsperpixelInSource;
    unsigned int channelsIn      = *channelsInSource;

#if PZP_VERBOSE
    fprintf(stderr, "Detected %ux%ux%u@%ubit/", width, height, channelsExt, bitsperpixelExt);
    fprintf(stderr, "%u@%ubit", channelsIn, bitsperpixelIn);
    fprintf(stderr, " | CRC:0x%X\n", *checksumSource);
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

    // Allocate memory for all channels
    *buffers = (unsigned char **)malloc(channelsIn * sizeof(unsigned char *));
    if (!*buffers)
    {
        free(decompressed_buffer);
        fail("Memory allocation failed");
    }

    for (unsigned int ch = 0; ch < channelsIn; ch++)
    {
        (*buffers)[ch] = (unsigned char *)malloc(dataSize);
        if (!(*buffers)[ch])
        {
            for (unsigned int i = 0; i < ch; i++)
            {
                free((*buffers)[i]);  // Free previously allocated channels
            }
            free(*buffers);
            free(decompressed_buffer);
            fail("Memory allocation failed");
        }
    }

    // Copy decompressed data into the channel buffers
    unsigned char *decompressed_bytes = (unsigned char *)decompressed_buffer + headerSize;
    for (int i = 0; i < width * height; i++)
    {
        for (unsigned int ch = 0; ch < channelsIn; ch++)
        {
            (*buffers)[ch][i] = decompressed_bytes[i * channelsIn + ch];
        }
    }

    free(decompressed_buffer);
}


#ifdef __cplusplus
}
#endif

#endif
