/* eac_encode.h -- encoder do canal ALPHA do ETC2 (EAC 8-bit) + montagem do bloco
 * COMPRESSED_RGBA8_ETC2_EAC (0x9278). 16 bytes/bloco 4x4 = 8 (EAC alpha) + 8 (ETC2-RGB).
 * GLES3 only (G31/Wayland). O RGB reaproveita o etc1_encode (ETC1 = ETC2-RGB base valido).
 * Alpha com alpha => 8bpp (vs 32bpp RGBA8) sem perder resolucao. */
#ifndef EAC_ENCODE_H
#define EAC_ENCODE_H
#include <stdint.h>

/* Codifica o ALPHA de UM bloco 4x4 (input ROW-major RGBA, 16px*4bytes) em 8 bytes EAC. */
void eac_encode_block_alpha(const uint8_t *block_rgba, uint8_t out[8]);

/* Codifica imagem RGBA w x h (multiplos de 4) em ETC2-EAC RGBA.
 * out = (w/4)*(h/4)*16 bytes. channels = stride do pixel (4). */
void eac_encode_image_rgba(const uint8_t *rgba, int w, int h, int channels, uint8_t *out);

#endif
