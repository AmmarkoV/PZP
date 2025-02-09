#include <stdio.h>
#include <stdlib.h>

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


int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        fprintf(stderr, "Usage: %s <compress|decompress> <input_file> <output_prefix>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char * operation                    = argv[1];
    const char * input_commandline_parameter  = argv[2];
    const char * output_commandline_parameter = argv[3];

    unsigned int configuration = 0;
    int performCompression     = 0;
    if (strcmp(operation, "compress") == 0) { performCompression=1; configuration = USE_COMPRESSION | USE_RLE; } else
    if (strcmp(operation, "pack") == 0)     { performCompression=1; configuration = USE_COMPRESSION; }

    if (performCompression)
    {
        fprintf(stderr, "Opening %s:", input_commandline_parameter);

        unsigned char *image = NULL;
        unsigned int width = 0, height = 0, bytesPerPixel = 0, channels = 0, bitsperpixelInternal = 0, channelsInternal=0;
        unsigned long timestamp = 0;

        image = ReadPNM(0, input_commandline_parameter, &width, &height, &timestamp, &bytesPerPixel, &channels);
        unsigned int bitsperpixel = bytesPerPixel * 8;
        fprintf(stderr, "%ux%ux%u@%ubit mode %u \n", width, height, channels, bitsperpixel,configuration);

        bitsperpixelInternal = bitsperpixel;
        channelsInternal     = channels;

        if (bitsperpixel==16)
        {
            //having one channel of 16bit is the same as having 2 channels of 8 bit
            bitsperpixelInternal = 8;
            channelsInternal*=2;
        }

        if (image!=NULL)
        {
         unsigned char **buffers = malloc(channelsInternal * sizeof(unsigned char *));

         if (buffers!=NULL)
         {
           for (unsigned int ch = 0; ch < channelsInternal; ch++)
           {
             buffers[ch] = malloc(width * height * sizeof(unsigned char));
             //if (buffers[ch]!=NULL)
             //{ memset(buffers[ch],0,width * height * sizeof(unsigned char)); }
           }

           pzp_split_channels(image, buffers, channelsInternal, width, height);

           if (configuration & USE_RLE)
           {
            fprintf(stderr,"Using RLE for compression (mode %u)\n",configuration);
            pzp_RLE_filter(buffers, channelsInternal, width, height);
           }

           pzp_compress_combined(buffers, width,height, bitsperpixel,channels, bitsperpixelInternal, channelsInternal, configuration, output_commandline_parameter);

         free(image);

         //Deallocate intermediate buffers..
         for (unsigned int ch = 0; ch < channels; ch++)
          {
            free(buffers[ch]);
          }
          free(buffers);
         }
        }//If we have an image
        else{ return EXIT_FAILURE; }
    }
    else
    if (strcmp(operation, "decompress") == 0)
    {
        fprintf(stderr, "Decompress %s \n", input_commandline_parameter);

        unsigned int width = 0, height = 0;
        unsigned int bitsperpixelExternal = 0, channelsExternal = 3;
        unsigned int bitsperpixelInternal = 24, channelsInternal = 3;
        unsigned int configuration = 0;

        unsigned char *reconstructed = pzp_decompress_combined(input_commandline_parameter, &width, &height,
                                                               &bitsperpixelExternal, &channelsExternal,
                                                               &bitsperpixelInternal, &channelsInternal, &configuration);

         if (reconstructed!=NULL)
         {
          bitsperpixelExternal *= channelsExternal; //This is needed because of what writePNM expects..
          WritePNM(output_commandline_parameter, reconstructed, width, height, bitsperpixelExternal, channelsExternal);
          free(reconstructed);
         }

    }
    else
    {
        fprintf(stderr, "Invalid mode: %s. Use 'compress' or 'decompress'.\n", operation);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
