#include "astcenc.h"
#include <string.h>
#include <stdlib.h>
// decode ASTC -> RGBA8. retorna 0 ok. outRGBA precisa w*h*4 bytes.
extern "C" int sor4_astc_decode(const unsigned char* data, unsigned long len,
                                int w, int h, int bx, int by, unsigned char* outRGBA) {
    astcenc_config config;
    astcenc_error st = astcenc_config_init(ASTCENC_PRF_LDR, bx, by, 1,
                          ASTCENC_PRE_FAST, ASTCENC_FLG_DECOMPRESS_ONLY, &config);
    if (st != ASTCENC_SUCCESS) return -1;
    astcenc_context* ctx = 0;
    st = astcenc_context_alloc(&config, 1, &ctx);
    if (st != ASTCENC_SUCCESS) return -2;
    astcenc_image image;
    image.dim_x = w; image.dim_y = h; image.dim_z = 1;
    image.data_type = ASTCENC_TYPE_U8;
    void* slice = outRGBA;
    image.data = &slice;
    astcenc_swizzle swz = { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
    st = astcenc_decompress_image(ctx, data, len, &image, &swz, 0);
    astcenc_context_free(ctx);
    return st == ASTCENC_SUCCESS ? 0 : -3;
}
