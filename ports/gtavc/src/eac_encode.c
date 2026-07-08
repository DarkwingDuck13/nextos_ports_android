/* eac_encode.c -- encoder do canal ALPHA do ETC2 (EAC 8-bit) + montagem do bloco
 * COMPRESSED_RGBA8_ETC2_EAC. Ver eac_encode.h. C puro (gcc, sem mangling).
 *
 * Bloco EAC alpha (8 bytes, big-endian):
 *   out[0] = base_codeword (0..255)
 *   out[1] = (multiplier<<4) | table_index
 *   out[2..7] = 48 bits: 16 indices de 3 bits, ordem de pixel COLUMN-major (p = x*4+y),
 *               pixel 0 nos bits mais significativos (47..45).
 * Decode (hardware): alpha[p] = clamp(base + tab[table][idx[p]] * multiplier, 0, 255).
 * Bloco ETC2 RGBA8: 16 bytes = [8 bytes EAC alpha][8 bytes ETC2-RGB (= ETC1)]. */
#include "eac_encode.h"
#include "etc1_encode.h"
#include <stdlib.h>
#include <string.h>

/* 16 tabelas modificadoras de intensidade do alpha ETC2 (8 valores cada). */
static const int kAlpha[16][8] = {
    {-3,-6,-9,-15, 2, 5, 8,14},
    {-3,-7,-10,-13,2, 6, 9,12},
    {-2,-5,-8,-13, 1, 4, 7,12},
    {-2,-4,-6,-13, 1, 3, 5,12},
    {-3,-6,-8,-12, 2, 5, 7,11},
    {-3,-7,-9,-11, 2, 6, 8,10},
    {-4,-7,-8,-11, 3, 6, 7,10},
    {-3,-5,-8,-11, 2, 4, 7,10},
    {-2,-6,-8,-10, 1, 5, 7, 9},
    {-2,-5,-8,-10, 1, 4, 7, 9},
    {-2,-4,-8,-10, 1, 3, 7, 9},
    {-2,-5,-7,-10, 1, 4, 6, 9},
    {-3,-4,-7,-10, 2, 3, 6, 9},
    {-1,-2,-3,-10, 0, 1, 2, 9},
    {-4,-6,-8,-9,  3, 5, 7, 8},
    {-3,-5,-7,-9,  2, 4, 6, 8},
};

static inline int clampi(int v, int lo, int hi){ return v < lo ? lo : (v > hi ? hi : v); }

/* Avalia (table,mult,base): retorna erro^2 total e preenche idx[16] (ordem column-major p). */
static long eval_combo(const int a[16], int base, int mult, int t, int idx_out[16]){
    long err = 0;
    for (int p = 0; p < 16; p++){
        int best_i = 0; long best_e = -1;
        for (int i = 0; i < 8; i++){
            int val = clampi(base + kAlpha[t][i]*mult, 0, 255);
            long d = (long)(a[p]-val); d *= d;
            if (best_e < 0 || d < best_e){ best_e = d; best_i = i; }
        }
        idx_out[p] = best_i; err += best_e;
    }
    return err;
}

/* escreve out[0..7] a partir de base/mult/table/indices. */
static void eac_pack(uint8_t out[8], int base, int mult, int t, const int idx[16]){
    out[0] = (uint8_t)base;
    out[1] = (uint8_t)(((mult & 0xF) << 4) | (t & 0xF));
    unsigned long long bits = 0;
    for (int p = 0; p < 16; p++)
        bits |= ((unsigned long long)(idx[p] & 7)) << ((15 - p) * 3);
    out[2]=(uint8_t)(bits>>40); out[3]=(uint8_t)(bits>>32); out[4]=(uint8_t)(bits>>24);
    out[5]=(uint8_t)(bits>>16); out[6]=(uint8_t)(bits>>8);  out[7]=(uint8_t)(bits);
}

void eac_encode_block_alpha(const uint8_t *block_rgba, uint8_t out[8]){
    /* alpha em ordem de bitstream: p = x*4 + y (input ROW-major: pixel (x,y) em (y*4+x)*4+3). */
    int a[16], sum = 0, amin = 255, amax = 0;
    for (int x = 0; x < 4; x++)
        for (int y = 0; y < 4; y++){
            int al = block_rgba[(y*4 + x)*4 + 3];
            a[x*4 + y] = al; sum += al;
            if (al < amin) amin = al; if (al > amax) amax = al;
        }

    /* ATALHO bloco CONSTANTE (cheio alpha=255 / vazio alpha=0 = a MAIORIA dos blocos de sprite):
     * exato e sem busca. base=valor, mult=1, tabela 13 (indice 4 = modifier 0) -> decode = valor.
     * Corta ~70-80% dos blocos -> o grande ganho de velocidade. */
    if (amin == amax){
        int idx[16]; for (int p=0;p<16;p++) idx[p]=4;   /* kAlpha[13][4]==0 */
        eac_pack(out, amin, 1, 13, idx);
        return;
    }

    /* nao-constante (so as bordas do sprite): base = media x 16 tabelas x mult 1..15. */
    int avg = (sum + 8) >> 4;
    int best_mult = 1, best_t = 0, best_idx[16];
    long best_err = -1;
    for (int t = 0; t < 16; t++)
        for (int mult = 1; mult <= 15; mult++){
            int idx[16];
            long err = eval_combo(a, avg, mult, t, idx);
            if (best_err < 0 || err < best_err){
                best_err = err; best_mult = mult; best_t = t;
                memcpy(best_idx, idx, sizeof idx);
            }
        }
    eac_pack(out, avg, best_mult, best_t, best_idx);
}

/* Imagem RGBA (w,h multiplos de 4) -> ETC2-EAC RGBA. out = (w/4)*(h/4)*16 bytes.
 * Cada bloco: 8 bytes EAC alpha + 8 bytes ETC2-RGB (reusa etc1_encode_block_rgba). */
void eac_encode_image_rgba(const uint8_t *rgba, int w, int h, int channels, uint8_t *out){
    int bw = w >> 2, bh = h >> 2;
    uint8_t block[16*4];
    for (int by = 0; by < bh; by++){
        for (int bx = 0; bx < bw; bx++){
            /* monta o bloco 4x4 em RGBA row-major (channels = stride do pixel de origem). */
            for (int y = 0; y < 4; y++){
                for (int x = 0; x < 4; x++){
                    const uint8_t *src = rgba + ((size_t)(by*4 + y)*w + (bx*4 + x))*channels;
                    uint8_t *dst = block + (y*4 + x)*4;
                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
                    dst[3] = channels >= 4 ? src[3] : 255;
                }
            }
            uint8_t *o = out + ((size_t)(by*bw + bx))*16;
            eac_encode_block_alpha(block, o);          /* 8 bytes alpha */
            etc1_encode_block_rgba(block, o + 8);      /* 8 bytes RGB (ETC1=ETC2-RGB) */
        }
    }
}
