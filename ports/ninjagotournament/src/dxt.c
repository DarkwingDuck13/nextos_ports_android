/* dxt.c -- decode das texturas DXT/S3TC para formatos que o Mali aceita.
 *
 * Os .fib deste OBB trazem as texturas em DXT1/DXT5 (S3TC), que o Mali-450
 * rejeita (GL_INVALID_ENUM) -- dai o menu preto sem fundo/logos. Decodificamos
 * em CPU no upload, no mesmo molde do atc.c do legohp1: RGB -> RGB565,
 * RGBA -> RGBA8888.
 *
 * Bloco de cor DXT (8 bytes, 4x4 texels): color0/color1 u16 RGB565 LE +
 * 4 bytes de indices 2-bit (texel 0 = bits 0-1 do byte 4).
 *   c0 >  c1: 00=c0  01=c1  10=(2c0+c1)/3  11=(c0+2c1)/3
 *   c0 <= c1: 00=c0  01=c1  10=(c0+c1)/2   11=transparente/preto
 * DXT3: 8 bytes de alpha 4-bit explicito antes do bloco de cor.
 * DXT5: 8 bytes de alpha interpolado (a0,a1 + 16 indices de 3 bits) antes.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <stdlib.h>

#include <GLES2/gl2.h>

#include "util.h"
#include "dxt.h"

#define DXT1_RGB  0x83F0
#define DXT1_RGBA 0x83F1
#define DXT3_RGBA 0x83F2
#define DXT5_RGBA 0x83F3

/* decodifica um bloco de cor DXT em rgba[16][4] (alpha 0 so no modo 3-cores) */
static void dxt_color_block(const uint8_t *b, uint8_t rgba[16][4]) {
  const uint16_t c0 = (uint16_t)(b[0] | (b[1] << 8));
  const uint16_t c1 = (uint16_t)(b[2] | (b[3] << 8));
  const int r0 = (c0 >> 11) & 31, g0 = (c0 >> 5) & 63, b0 = c0 & 31;
  const int r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
  const int R0 = (r0 << 3) | (r0 >> 2), G0 = (g0 << 2) | (g0 >> 4), B0 = (b0 << 3) | (b0 >> 2);
  const int R1 = (r1 << 3) | (r1 >> 2), G1 = (g1 << 2) | (g1 >> 4), B1 = (b1 << 3) | (b1 >> 2);

  uint8_t pal[4][4];
  pal[0][0] = (uint8_t)R0; pal[0][1] = (uint8_t)G0; pal[0][2] = (uint8_t)B0; pal[0][3] = 255;
  pal[1][0] = (uint8_t)R1; pal[1][1] = (uint8_t)G1; pal[1][2] = (uint8_t)B1; pal[1][3] = 255;
  if (c0 > c1) {
    pal[2][0] = (uint8_t)((2 * R0 + R1) / 3);
    pal[2][1] = (uint8_t)((2 * G0 + G1) / 3);
    pal[2][2] = (uint8_t)((2 * B0 + B1) / 3);
    pal[2][3] = 255;
    pal[3][0] = (uint8_t)((R0 + 2 * R1) / 3);
    pal[3][1] = (uint8_t)((G0 + 2 * G1) / 3);
    pal[3][2] = (uint8_t)((B0 + 2 * B1) / 3);
    pal[3][3] = 255;
  } else {
    pal[2][0] = (uint8_t)((R0 + R1) / 2);
    pal[2][1] = (uint8_t)((G0 + G1) / 2);
    pal[2][2] = (uint8_t)((B0 + B1) / 2);
    pal[2][3] = 255;
    pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;   /* punch-through */
  }

  const uint32_t idx = (uint32_t)(b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24));
  for (int i = 0; i < 16; i++) {
    const uint8_t *p = pal[(idx >> (2 * i)) & 3];
    rgba[i][0] = p[0]; rgba[i][1] = p[1]; rgba[i][2] = p[2]; rgba[i][3] = p[3];
  }
}

/* alpha 4-bit explicito (DXT3): 8 bytes = 16 nibbles */
static void dxt_alpha_explicit(const uint8_t *a, uint8_t alpha[16]) {
  for (int i = 0; i < 8; i++) {
    uint8_t lo = (uint8_t)(a[i] & 0x0F), hi = (uint8_t)(a[i] >> 4);
    alpha[2 * i]     = (uint8_t)((lo << 4) | lo);
    alpha[2 * i + 1] = (uint8_t)((hi << 4) | hi);
  }
}

/* alpha interpolado (DXT5): a0,a1 + 16 indices de 3 bits */
static void dxt_alpha_interp(const uint8_t *a, uint8_t alpha[16]) {
  const int a0 = a[0], a1 = a[1];
  uint8_t pal[8];
  pal[0] = (uint8_t)a0; pal[1] = (uint8_t)a1;
  if (a0 > a1) {
    for (int i = 1; i < 7; i++) pal[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
  } else {
    for (int i = 1; i < 5; i++) pal[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
    pal[6] = 0; pal[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; i++) bits |= (uint64_t)a[2 + i] << (8 * i);
  for (int i = 0; i < 16; i++) alpha[i] = pal[(bits >> (3 * i)) & 7];
}

/* decodifica e sobe uma imagem DXT; retorna 1 se o formato era DXT */
int dxt_upload(GLenum target, GLint level, GLenum ifmt,
               GLsizei w, GLsizei h, GLsizei size, const void *data) {
  int alpha_mode, block_bytes;   /* 0=sem bloco alpha, 1=DXT1a, 2=DXT3, 3=DXT5 */
  switch (ifmt) {
    case DXT1_RGB:  alpha_mode = 0; block_bytes = 8;  break;
    case DXT1_RGBA: alpha_mode = 1; block_bytes = 8;  break;
    case DXT3_RGBA: alpha_mode = 2; block_bytes = 16; break;
    case DXT5_RGBA: alpha_mode = 3; block_bytes = 16; break;
    default: return 0;
  }
  (void)size;
  if (!data || w <= 0 || h <= 0) return 1;

  const int bw = (w + 3) / 4, bh = (h + 3) / 4;
  const uint8_t *src = (const uint8_t *)data;

  if (alpha_mode == 0) {
    uint16_t *out = (uint16_t *)malloc((size_t)w * h * 2);
    if (!out) return 1;
    for (int by = 0; by < bh; by++) {
      for (int bx = 0; bx < bw; bx++) {
        uint8_t rgba[16][4];
        dxt_color_block(src + ((size_t)by * bw + bx) * block_bytes, rgba);
        for (int py = 0; py < 4; py++) {
          const int y = by * 4 + py;
          if (y >= h) break;
          for (int px = 0; px < 4; px++) {
            const int x = bx * 4 + px;
            if (x >= w) break;
            const uint8_t *p = rgba[py * 4 + px];
            out[(size_t)y * w + x] = (uint16_t)(((p[0] >> 3) << 11) |
                                                ((p[1] >> 2) << 5) |
                                                (p[2] >> 3));
          }
        }
      }
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexImage2D(target, level, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, out);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    free(out);
  } else {
    uint32_t *out = (uint32_t *)malloc((size_t)w * h * 4);
    if (!out) return 1;
    for (int by = 0; by < bh; by++) {
      for (int bx = 0; bx < bw; bx++) {
        const uint8_t *blk = src + ((size_t)by * bw + bx) * block_bytes;
        uint8_t alpha[16], rgba[16][4];
        dxt_color_block(blk + (block_bytes == 16 ? 8 : 0), rgba);
        if (alpha_mode == 2)      dxt_alpha_explicit(blk, alpha);
        else if (alpha_mode == 3) dxt_alpha_interp(blk, alpha);
        else                      for (int i = 0; i < 16; i++) alpha[i] = rgba[i][3];
        for (int py = 0; py < 4; py++) {
          const int y = by * 4 + py;
          if (y >= h) break;
          for (int px = 0; px < 4; px++) {
            const int x = bx * 4 + px;
            if (x >= w) break;
            const int i = py * 4 + px;
            out[(size_t)y * w + x] = (uint32_t)rgba[i][0] |
                                     ((uint32_t)rgba[i][1] << 8) |
                                     ((uint32_t)rgba[i][2] << 16) |
                                     ((uint32_t)alpha[i] << 24);
          }
        }
      }
    }
    glTexImage2D(target, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
    free(out);
  }

  {
    static int n = 0;
    GLenum e = glGetError();
    if (e || n < 12) {
      debugPrintf("DXT: fmt=0x%x %dx%d lvl=%d -> %s err=0x%x\n", ifmt, (int)w, (int)h,
                  (int)level, alpha_mode ? "RGBA8888" : "RGB565", e);
      n++;
    }
  }
  return 1;
}
