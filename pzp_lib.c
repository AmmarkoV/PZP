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
