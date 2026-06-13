/* jpeg_enc.c — encoder JPEG baseline self-contained (sem libjpeg/turbojpeg).
 * Baseado no jo_jpeg.cpp de Jon Olick (domínio público / public domain),
 * adaptado para saída em MEMÓRIA (buffer growable). Entrada RGBA/RGB.
 * Necessário porque devices como o Amlogic-old só têm libpng/libz, sem JPEG.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* buffer de saída growable */
typedef struct { unsigned char *p; long len, cap; } jbuf;
static void jb_put(jbuf *b, int v) {
  if (b->len >= b->cap) { b->cap = b->cap ? b->cap * 2 : 65536; b->p = realloc(b->p, b->cap); }
  b->p[b->len++] = (unsigned char)v;
}

static const unsigned char s_jo_ZigZag[] = {0,1,5,6,14,15,27,28,2,4,7,13,16,26,29,42,3,8,12,17,25,30,41,43,9,11,18,24,31,40,44,53,10,19,23,32,39,45,52,54,20,22,33,38,46,51,55,60,21,34,37,47,50,56,59,61,35,36,48,49,57,58,62,63};

static void jo_writeBits(jbuf *b, int *bitBuf, int *bitCnt, const unsigned short bs[2]) {
  *bitCnt += bs[1];
  *bitBuf |= bs[0] << (24 - *bitCnt);
  while (*bitCnt >= 8) {
    unsigned char c = (*bitBuf >> 16) & 255;
    jb_put(b, c);
    if (c == 255) jb_put(b, 0);
    *bitBuf <<= 8;
    *bitCnt -= 8;
  }
}

static void jo_DCT(float *d0, float *d1, float *d2, float *d3, float *d4, float *d5, float *d6, float *d7) {
  float tmp0 = *d0 + *d7, tmp7 = *d0 - *d7;
  float tmp1 = *d1 + *d6, tmp6 = *d1 - *d6;
  float tmp2 = *d2 + *d5, tmp5 = *d2 - *d5;
  float tmp3 = *d3 + *d4, tmp4 = *d3 - *d4;
  float tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3;
  float tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
  *d0 = tmp10 + tmp11; *d4 = tmp10 - tmp11;
  float z1 = (tmp12 + tmp13) * 0.707106781f;
  *d2 = tmp13 + z1; *d6 = tmp13 - z1;
  tmp10 = tmp4 + tmp5; tmp11 = tmp5 + tmp6; tmp12 = tmp6 + tmp7;
  float z5 = (tmp10 - tmp12) * 0.382683433f;
  float z2 = tmp10 * 0.541196100f + z5;
  float z4 = tmp12 * 1.306562965f + z5;
  float z3 = tmp11 * 0.707106781f;
  float z11 = tmp7 + z3, z13 = tmp7 - z3;
  *d5 = z13 + z2; *d3 = z13 - z2;
  *d1 = z11 + z4; *d7 = z11 - z4;
}

/* calcula (bits, nbits) de um valor com sinal, MASCARADO (igual jo_jpeg). */
static void jo_calcBits(int val, unsigned short bits[2]) {
  int tmp1 = val < 0 ? -val : val;
  val = val < 0 ? val - 1 : val;
  bits[1] = 1;
  while (tmp1 >>= 1) ++bits[1];
  bits[0] = val & ((1 << bits[1]) - 1);
}

static int jo_processDU(jbuf *b, int *bitBuf, int *bitCnt, float *CDU, float *fdtbl, int DC,
                        const unsigned short HTDC[256][2], const unsigned short HTAC[256][2]) {
  const unsigned short EOB[2] = {HTAC[0x00][0], HTAC[0x00][1]};
  const unsigned short M16zeroes[2] = {HTAC[0xF0][0], HTAC[0xF0][1]};
  int dataOff, i, diff, end0pos;
  int DU[64];
  for (dataOff = 0; dataOff < 64; dataOff += 8)
    jo_DCT(&CDU[dataOff], &CDU[dataOff+1], &CDU[dataOff+2], &CDU[dataOff+3], &CDU[dataOff+4], &CDU[dataOff+5], &CDU[dataOff+6], &CDU[dataOff+7]);
  for (dataOff = 0; dataOff < 8; ++dataOff)
    jo_DCT(&CDU[dataOff], &CDU[dataOff+8], &CDU[dataOff+16], &CDU[dataOff+24], &CDU[dataOff+32], &CDU[dataOff+40], &CDU[dataOff+48], &CDU[dataOff+56]);
  for (i = 0; i < 64; ++i) {
    float v = CDU[i] * fdtbl[i];
    /* reordena p/ ZIGZAG aqui (bug se faltar: DC/AC nas posicoes erradas) */
    DU[s_jo_ZigZag[i]] = (int)(v < 0 ? v - 0.5f : v + 0.5f);
  }
  diff = DU[0] - DC;
  if (diff == 0) jo_writeBits(b, bitBuf, bitCnt, HTDC[0]);
  else {
    unsigned short bits[2];
    jo_calcBits(diff, bits);
    jo_writeBits(b, bitBuf, bitCnt, HTDC[bits[1]]);
    jo_writeBits(b, bitBuf, bitCnt, bits);
  }
  end0pos = 63;
  for (; (end0pos > 0) && (DU[end0pos] == 0); --end0pos);
  if (end0pos == 0) { jo_writeBits(b, bitBuf, bitCnt, EOB); return DU[0]; }
  for (i = 1; i <= end0pos; ++i) {
    int startpos = i, nrzeroes;
    for (; DU[i] == 0 && i <= end0pos; ++i);
    nrzeroes = i - startpos;
    if (nrzeroes >= 16) {
      int lng = nrzeroes >> 4, nrmarker;
      for (nrmarker = 1; nrmarker <= lng; ++nrmarker) jo_writeBits(b, bitBuf, bitCnt, M16zeroes);
      nrzeroes &= 15;
    }
    {
      unsigned short bits[2];
      jo_calcBits(DU[i], bits);
      jo_writeBits(b, bitBuf, bitCnt, HTAC[(nrzeroes << 4) + bits[1]]);
      jo_writeBits(b, bitBuf, bitCnt, bits);
    }
  }
  if (end0pos != 63) jo_writeBits(b, bitBuf, bitCnt, EOB);
  return DU[0];
}

unsigned char *jpeg_encode_rgba(const unsigned char *data, int width, int height,
                                int comp, int quality, long *out_len) {
  static const unsigned char std_dc_luminance_nrcodes[] = {0,0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
  static const unsigned char std_dc_luminance_values[] = {0,1,2,3,4,5,6,7,8,9,10,11};
  static const unsigned char std_ac_luminance_nrcodes[] = {0,0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
  static const unsigned char std_ac_luminance_values[] = {0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa};
  static const unsigned char std_dc_chrominance_nrcodes[] = {0,0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
  static const unsigned char std_dc_chrominance_values[] = {0,1,2,3,4,5,6,7,8,9,10,11};
  static const unsigned char std_ac_chrominance_nrcodes[] = {0,0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
  static const unsigned char std_ac_chrominance_values[] = {0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa};
  static const int YQT[] = {16,11,10,16,24,40,51,61,12,12,14,19,26,58,60,55,14,13,16,24,40,57,69,56,14,17,22,29,51,87,80,62,18,22,37,56,68,109,103,77,24,35,55,64,81,104,113,92,49,64,78,87,103,121,120,101,72,92,95,98,112,100,103,99};
  static const int UVQT[] = {17,18,24,47,99,99,99,99,18,21,26,66,99,99,99,99,24,26,56,99,99,99,99,99,47,66,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99};
  static const float aasf[] = {2.5570806f,3.5454697f,3.3389568f,2.5570806f,1.6629870f,0.8526249f,0.4337662f,0.2147325f};
  int i, row, col, k;
  unsigned char YTable[64], UVTable[64];
  float fdtbl_Y[64], fdtbl_UV[64];
  jbuf b = {0};
  unsigned short YDC_HT[256][2], UVDC_HT[256][2], YAC_HT[256][2], UVAC_HT[256][2];

  if (!data || width <= 0 || height <= 0) return NULL;
  if (quality < 1) quality = 1; if (quality > 100) quality = 100;
  quality = quality < 50 ? 5000 / quality : 200 - quality * 2;

  for (i = 0; i < 64; ++i) {
    int yti = (YQT[i] * quality + 50) / 100;
    YTable[s_jo_ZigZag[i]] = yti < 1 ? 1 : (yti > 255 ? 255 : yti);
    int uvti = (UVQT[i] * quality + 50) / 100;
    UVTable[s_jo_ZigZag[i]] = uvti < 1 ? 1 : (uvti > 255 ? 255 : uvti);
  }
  for (row = 0, k = 0; row < 8; ++row)
    for (col = 0; col < 8; ++col, ++k) {
      fdtbl_Y[k]  = 1 / (YTable[s_jo_ZigZag[k]] * aasf[row] * aasf[col]);
      fdtbl_UV[k] = 1 / (UVTable[s_jo_ZigZag[k]] * aasf[row] * aasf[col]);
    }

  /* monta tabelas Huffman */
  {
    int pos;
    #define COMPUTE(HT, nrcodes, values) do { \
      int code = 0, kk = 0, K; unsigned char j2; \
      for (K = 1; K <= 16; ++K) { for (j2 = 1; j2 <= nrcodes[K]; ++j2) { \
        HT[values[kk]][0] = code; HT[values[kk]][1] = K; ++kk; ++code; } code <<= 1; } } while(0)
    COMPUTE(YDC_HT, std_dc_luminance_nrcodes, std_dc_luminance_values);
    COMPUTE(UVDC_HT, std_dc_chrominance_nrcodes, std_dc_chrominance_values);
    COMPUTE(YAC_HT, std_ac_luminance_nrcodes, std_ac_luminance_values);
    COMPUTE(UVAC_HT, std_ac_chrominance_nrcodes, std_ac_chrominance_values);
    #undef COMPUTE
    (void)pos;
  }

  /* headers */
  { static const unsigned char head0[] = {0xFF,0xD8,0xFF,0xE0,0,0x10,'J','F','I','F',0,1,1,0,0,1,0,1,0,0,0xFF,0xDB,0,0x84,0};
    for (i = 0; i < (int)sizeof(head0); ++i) jb_put(&b, head0[i]); }
  for (i = 0; i < 64; ++i) jb_put(&b, YTable[i]);
  jb_put(&b, 1);
  for (i = 0; i < 64; ++i) jb_put(&b, UVTable[i]);
  { unsigned char head1[] = {0xFF,0xC0,0,0x11,8,(unsigned char)(height>>8),(unsigned char)(height&0xFF),(unsigned char)(width>>8),(unsigned char)(width&0xFF),3,1,0x11,0,2,0x11,1,3,0x11,1,0xFF,0xC4,0x01,0xA2,0};
    for (i = 0; i < (int)sizeof(head1); ++i) jb_put(&b, head1[i]); }
  /* DHT */
  for (i = 0; i < 16; ++i) jb_put(&b, std_dc_luminance_nrcodes[i+1]);
  for (i = 0; i <= 11; ++i) jb_put(&b, std_dc_luminance_values[i]);
  jb_put(&b, 0x10);
  for (i = 0; i < 16; ++i) jb_put(&b, std_ac_luminance_nrcodes[i+1]);
  for (i = 0; i <= 161; ++i) jb_put(&b, std_ac_luminance_values[i]);
  jb_put(&b, 1);
  for (i = 0; i < 16; ++i) jb_put(&b, std_dc_chrominance_nrcodes[i+1]);
  for (i = 0; i <= 11; ++i) jb_put(&b, std_dc_chrominance_values[i]);
  jb_put(&b, 0x11);
  for (i = 0; i < 16; ++i) jb_put(&b, std_ac_chrominance_nrcodes[i+1]);
  for (i = 0; i <= 161; ++i) jb_put(&b, std_ac_chrominance_values[i]);
  { static const unsigned char sos[] = {0xFF,0xDA,0,0xC,3,1,0,2,0x11,3,0x11,0,0x3F,0};
    for (i = 0; i < (int)sizeof(sos); ++i) jb_put(&b, sos[i]); }

  /* scan */
  {
    int bitBuf = 0, bitCnt = 0;
    int DCY = 0, DCU = 0, DCV = 0;
    int x, y;
    float YDU[64], UDU[64], VDU[64];
    for (y = 0; y < height; y += 8) {
      for (x = 0; x < width; x += 8) {
        for (row = 0; row < 8; ++row) {
          int yy = y + row; if (yy >= height) yy = height - 1;
          for (col = 0; col < 8; ++col) {
            int xx = x + col; if (xx >= width) xx = width - 1;
            const unsigned char *px = data + ((long)yy * width + xx) * comp;
            float r = px[0], g = comp > 1 ? px[1] : px[0], bl = comp > 2 ? px[2] : px[0];
            int p = row * 8 + col;
            YDU[p] = 0.29900f*r + 0.58700f*g + 0.11400f*bl - 128;
            UDU[p] = -0.16874f*r - 0.33126f*g + 0.50000f*bl;
            VDU[p] = 0.50000f*r - 0.41869f*g - 0.08131f*bl;
          }
        }
        DCY = jo_processDU(&b, &bitBuf, &bitCnt, YDU, fdtbl_Y, DCY, YDC_HT, YAC_HT);
        DCU = jo_processDU(&b, &bitBuf, &bitCnt, UDU, fdtbl_UV, DCU, UVDC_HT, UVAC_HT);
        DCV = jo_processDU(&b, &bitBuf, &bitCnt, VDU, fdtbl_UV, DCV, UVDC_HT, UVAC_HT);
      }
    }
    { static const unsigned short fill[2] = {0x7F, 7}; jo_writeBits(&b, &bitBuf, &bitCnt, fill); }
  }
  jb_put(&b, 0xFF); jb_put(&b, 0xD9);
  *out_len = b.len;
  return b.p;
}
