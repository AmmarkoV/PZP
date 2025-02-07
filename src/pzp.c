#include <stdio.h>
#include <stdlib.h>

#include "pzp.h"
//sudo apt install libzstd-dev

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

    if (strcmp(operation, "compress") == 0)
    {
        fprintf(stderr, "Opening %s:", input_commandline_parameter);

        unsigned char *image = NULL;
        unsigned int width = 0, height = 0, bytesPerPixel = 0, channels = 0, bitsperpixelInternal = 0, channelsInternal=0;
        unsigned long timestamp = 0;

        image = ReadPNM(0, input_commandline_parameter, &width, &height, &timestamp, &bytesPerPixel, &channels);
        unsigned int bitsperpixel = bytesPerPixel * 8;
        fprintf(stderr, "%ux%ux%u@%ubit \n", width, height, channels, bitsperpixel);

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

           split_channels_and_filter(image, buffers, channelsInternal, width, height);

           compress_combined(buffers, width,height, bitsperpixel,channels, bitsperpixelInternal, channelsInternal, output_commandline_parameter );

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
    else if (strcmp(operation, "decompress") == 0)
    {
        fprintf(stderr, "Decompress %s \n", input_commandline_parameter);

        unsigned char **buffers = NULL;
        unsigned int width = 0, height = 0;
        unsigned int bitsperpixelExternal = 0, channelsExternal = 3;
        unsigned int bitsperpixelInternal = 24, channelsInternal = 3;

        decompress_combined(input_commandline_parameter, &buffers, &width, &height,
                            &bitsperpixelExternal, &channelsExternal,
                            &bitsperpixelInternal, &channelsInternal);
        if (buffers!=NULL)
        {

        restore_channels(buffers, channelsInternal, width, height);

        unsigned char *reconstructed = malloc( width * height * (bitsperpixelInternal/8)* channelsInternal );
        if (reconstructed!=NULL)
        {
          for (size_t i = 0; i < width * height; i++) //* (bitsperpixel/8)
          {
            for (unsigned int ch = 0; ch < channelsInternal; ch++)
            {
                reconstructed[i * channelsInternal + ch] = buffers[ch][i];
            }
          }

         bitsperpixelExternal *= channelsExternal; //This is needed because of what writePNM expects..
         WritePNM(output_commandline_parameter, reconstructed, width, height, bitsperpixelExternal, channelsExternal);
         free(reconstructed);
        }

        //Deallocate intermediate buffers..
        for (unsigned int ch = 0; ch < channelsInternal; ch++)
        {
            free(buffers[ch]);
        }
        free(buffers);

        }
    }
    else
    {
        fprintf(stderr, "Invalid mode: %s. Use 'compress' or 'decompress'.\n", operation);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
