/* texbake.c — converte TODAS as texturas do pak do DYSMANTLE pro formato GPU FINAL,
 * OFFLINE (1x na instalação / no progressor). Substitui o fixpak. Princípio: o jogo
 * NUNCA decodifica/transcoda textura em runtime — recebe o bloco pronto.
 *
 *   opaca  -> ETC1 (GL_ETC1_RGB8_OES 0x8D64, 4 bpp) — amostrada por TODA GPU (Utgard
 *             ES2 e qualquer ES3, pois ETC2 é superset). UM formato pra todos.
 *   alpha  -> RGBA4444 (0x8033, 16 bpp) — universal, metade do RGBA8.
 *
 * Para CADA textura (.jpg/.png) gera o irmão "<nome>.ktx" (KTX 11, zlib) que a engine
 * carrega via KtxImageLoader quando DYSMANTLE_KTX_REDIRECT está ligado. O runtime
 * (imports.c my_glCompressedTexImage2D) passa ETC1 DIRETO; RGBA4444 via glTexImage2D.
 *
 * Fonte dos pixels (nesta ordem):
 *   1) o "<nome>.ktx" ETC2 já existente (0x9274 RGB / 0x9278 RGBA) -> etc2_decode
 *   2) senão, o próprio "<nome>.jpg/.png" com dados reais        -> stb_image
 *
 * Uso: texbake <pak> [--scale N] [--no-alpha-scan]
 *   --scale N : downscale por fator N (1=qualidade total; 2,3 = menos RAM/nitidez)
 * Idempotente-ish: regrava sempre. Pak intacto se nada converter.
 *
 * Libs: zlib (dlopen, igual fixpak) + etc2_decode.c + etc1_encode.c + stb_image (header).
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

extern unsigned char *etc2_decode_rgba(unsigned fmt, int w, int h, const void *data, int size);
extern void etc1_encode_image(const unsigned char *rgba, int w, int h, int channels, uint8_t *out);

/* ---- libz via dlopen (universal: device só tem libz garantido) ---- */
static int (*z_uncompress)(unsigned char *, unsigned long *, const unsigned char *, unsigned long);
static int (*z_compress2)(unsigned char *, unsigned long *, const unsigned char *, unsigned long, int);
static unsigned long (*z_compressBound)(unsigned long);
static unsigned long (*z_crc32)(unsigned long, const unsigned char *, unsigned);
extern unsigned char *jpeg_encode_rgba(const unsigned char *, int, int, int, int, long *);
static int load_libs(void) {
  void *z = dlopen("libz.so.1", RTLD_NOW); if (!z) z = dlopen("libz.so", RTLD_NOW);
  if (!z) { fprintf(stderr, "texbake: dlopen libz falhou\n"); return -1; }
  z_uncompress = dlsym(z, "uncompress");
  z_compress2 = dlsym(z, "compress2");
  z_compressBound = dlsym(z, "compressBound");
  z_crc32 = dlsym(z, "crc32");
  if (!z_uncompress || !z_compress2 || !z_compressBound) { fprintf(stderr, "texbake: dlsym libz\n"); return -1; }
  return 0;
}

/* RGBA -> PNG (heap; *plen). Encoder mínimo IHDR+IDAT(zlib)+IEND (copiado do fixpak). */
static unsigned char *rgba_to_png(const unsigned char *rgba, int w, int h, long *plen) {
  long raw = (long)h * (w * 4 + 1);
  unsigned char *filt = malloc(raw); if (!filt) return NULL;
  for (int y = 0; y < h; y++) { filt[y*(w*4+1)] = 0; memcpy(filt + y*(w*4+1) + 1, rgba + (long)y*w*4, w*4); }
  unsigned long cb = z_compressBound ? z_compressBound(raw) : raw + raw/2 + 64;
  unsigned char *idat = malloc(cb); if (!idat) { free(filt); return NULL; }
  if (z_compress2(idat, &cb, filt, raw, 6) != 0) { free(filt); free(idat); return NULL; }
  free(filt);
  unsigned char *out = malloc(8 + 25 + (12 + (long)cb) + 12); if (!out) { free(idat); return NULL; }
  long o = 0; memcpy(out, "\x89PNG\r\n\x1a\n", 8); o = 8;
#define PUT32(v) do{out[o++]=(v)>>24;out[o++]=(v)>>16;out[o++]=(v)>>8;out[o++]=(v);}while(0)
  PUT32(13); long c0=o; memcpy(out+o,"IHDR",4); o+=4; PUT32((unsigned)w); PUT32((unsigned)h);
  out[o++]=8; out[o++]=6; out[o++]=0; out[o++]=0; out[o++]=0;
  { unsigned long c=z_crc32(0,out+c0,(unsigned)(o-c0)); PUT32((unsigned)c); }
  PUT32((unsigned)cb); long c1=o; memcpy(out+o,"IDAT",4); o+=4; memcpy(out+o,idat,cb); o+=cb;
  { unsigned long c=z_crc32(0,out+c1,(unsigned)(4+cb)); PUT32((unsigned)c); }
  free(idat);
  PUT32(0); long c2=o; memcpy(out+o,"IEND",4); o+=4; { unsigned long c=z_crc32(0,out+c2,4); PUT32((unsigned)c); }
#undef PUT32
  *plen = o; return out;
}

/* ---------- KTX 11 ---------- */
static const unsigned char KTX_ID[12] = {0xAB,'K','T','X',' ','1','1',0xBB,'\r','\n',0x1A,'\n'};

/* descomprime entrada zlib se for; senão devolve cópia. *olen = tamanho. */
static unsigned char *maybe_inflate(const unsigned char *in, long ilen, long *olen) {
  if (ilen >= 2 && in[0] == 0x78) {
    unsigned long cap = (unsigned long)ilen * 12 + 4096;
    unsigned char *out = malloc(cap);
    while (out && z_uncompress(out, &cap, in, ilen) == -5 /*Z_BUF_ERROR*/) {
      cap *= 2; unsigned char *n = realloc(out, cap); if (!n) { free(out); out = NULL; break; } out = n;
    }
    if (out) { *olen = (long)cap; return out; }
  }
  unsigned char *c = malloc(ilen); if (c) { memcpy(c, in, ilen); *olen = ilen; } return c;
}

/* KTX (já inflado) -> RGBA. preenche w/h/has_alpha. */
static unsigned char *ktx_to_rgba(const unsigned char *k, long klen, int *ow, int *oh, int *alpha) {
  if (klen < 64 || memcmp(k, KTX_ID, 12) != 0) return NULL;
  uint32_t gif = *(const uint32_t *)(k + 28);
  uint32_t w = *(const uint32_t *)(k + 36), h = *(const uint32_t *)(k + 40);
  uint32_t kv = *(const uint32_t *)(k + 60);
  long q = 64 + kv; if (q + 4 > klen) return NULL;
  uint32_t isz = *(const uint32_t *)(k + q); q += 4;
  if (q + isz > klen) return NULL;
  *ow = w; *oh = h;
  *alpha = (gif == 0x9278 || gif == 0x9279 || gif == 0x9276 || gif == 0x9277);
  return etc2_decode_rgba(gif, w, h, k + q, isz);   /* RGBA (free do caller) */
}

/* bilinear-downscale RGBA por fator FRACIONÁRIO f (>1.0; ex 1.2 = ~83% das dims —
 * MESMA semântica do DYSMANTLE_TEXSCALE runtime). devolve novo buffer + nw/nh. */
static unsigned char *downscale(const unsigned char *rgba, int w, int h, double f, int *nw, int *nh) {
  if (f <= 1.001) return NULL;
  int W = (int)(w / f), H = (int)(h / f);
  W = (W + 2) & ~3; H = (H + 2) & ~3;       /* 🔑 arredonda p/ MÚLTIPLO DE 4 (ETC1 exige) */
  if (W < 4) W = 4; if (H < 4) H = 4;
  if (W >= w && H >= h) return NULL;
  unsigned char *o = malloc((long)W * H * 4); if (!o) return NULL;
  for (int y = 0; y < H; y++) {
    float fy = ((float)y + 0.5f) * h / H - 0.5f;
    int y0 = (int)fy; if (y0 < 0) y0 = 0;
    int y1 = y0 + 1 < h ? y0 + 1 : h - 1; float wy = fy - y0;
    for (int x = 0; x < W; x++) {
      float fx = ((float)x + 0.5f) * w / W - 0.5f;
      int x0 = (int)fx; if (x0 < 0) x0 = 0;
      int x1 = x0 + 1 < w ? x0 + 1 : w - 1; float wx = fx - x0;
      const unsigned char *p00 = rgba + ((long)y0*w+x0)*4, *p01 = rgba + ((long)y0*w+x1)*4;
      const unsigned char *p10 = rgba + ((long)y1*w+x0)*4, *p11 = rgba + ((long)y1*w+x1)*4;
      unsigned char *d = o + ((long)y*W+x)*4;
      for (int c = 0; c < 4; c++) {
        float t = p00[c]*(1-wx)*(1-wy) + p01[c]*wx*(1-wy) + p10[c]*(1-wx)*wy + p11[c]*wx*wy;
        d[c] = (unsigned char)(t + 0.5f);
      }
    }
  }
  *nw = W; *nh = H; return o;
}

/* tem algum pixel com alpha < 250? (classifica opaco vs alpha de verdade) */
static int has_real_alpha(const unsigned char *rgba, int w, int h) {
  long n = (long)w * h;
  for (long i = 0; i < n; i++) if (rgba[i * 4 + 3] < 250) return 1;
  return 0;
}

/* empacota RGBA8 -> RGBA4444 (GL_UNSIGNED_SHORT_4_4_4_4). out = w*h*2 bytes. */
static void pack_4444(const unsigned char *rgba, int w, int h, unsigned char *out) {
  long n = (long)w * h;
  for (long i = 0; i < n; i++) {
    int r = rgba[i*4]>>4, g = rgba[i*4+1]>>4, b = rgba[i*4+2]>>4, a = rgba[i*4+3]>>4;
    uint16_t v = (r<<12)|(g<<8)|(b<<4)|a;   /* RGBA */
    out[i*2] = v & 0xff; out[i*2+1] = v >> 8;
  }
}

/* empacota RGBA8 -> RGBA5551 (GL_UNSIGNED_SHORT_5_5_5_1): cor 5-5-5 (melhor que 4444)
 * + 1 bit de alpha. Ideal p/ alpha de RECORTE (folhagem/decals borda-dura). */
static void pack_5551(const unsigned char *rgba, int w, int h, unsigned char *out) {
  long n = (long)w * h;
  for (long i = 0; i < n; i++) {
    int r = rgba[i*4]>>3, g = rgba[i*4+1]>>3, b = rgba[i*4+2]>>3, a = rgba[i*4+3] >= 128;
    uint16_t v = (r<<11)|(g<<6)|(b<<1)|a;
    out[i*2] = v & 0xff; out[i*2+1] = v >> 8;
  }
}

/* alpha é de RECORTE (binário ~0/255)? -> menos de ~5% dos pixels com alpha parcial.
 * Cutout -> 5551 (cor melhor); suave/gradiente -> 4444 (4 bits de alpha). */
static int is_cutout_alpha(const unsigned char *rgba, int w, int h) {
  long n = (long)w * h, partial = 0;
  for (long i = 0; i < n; i++) { int a = rgba[i*4+3]; if (a > 16 && a < 240) partial++; }
  return partial * 20 < n;   /* <5% parcial = recorte */
}

/* modos de saída */
enum { M_ETC1 = 0, M_RGBA4444 = 1, M_RGBA8 = 2, M_RGBA5551 = 3 };

/* monta um KTX 11 (1 nível, sem KV). mode = ETC1 / RGBA4444 / RGBA8. */
static unsigned char *build_ktx(const unsigned char *img, uint32_t isz, int w, int h,
                                int mode, long *olen) {
  long cap = 64 + 4 + isz;
  unsigned char *k = calloc(1, cap); if (!k) return NULL;
  memcpy(k, KTX_ID, 12);
  uint32_t *u = (uint32_t *)k;
  u[3] = 0x04030201;                        /* endianness */
  if (mode == M_ETC1) {
    /* 🔑 ROTULA como ETC2 RGB (0x9274), NÃO ETC1 (0x8D64): o KtxImageLoader da engine
     * só aceita os formatos do jogo (ETC2). ETC1 é SUBCONJUNTO do ETC2 RGB (mesmos 8
     * bytes/bloco, mesmo decode nos modos individual/differential) → o bloco ETC1 é um
     * bloco ETC2-RGB VÁLIDO. A engine aceita; o hook (imports.c) decodifica certo OU
     * sobe como ETC1 (Mali amostra). Sem isso, a engine rejeita (CTEX=0 = bug do rosa). */
    u[4] = 0;          /* glType */          u[5] = 1;  /* glTypeSize */
    u[6] = 0;          /* glFormat */        u[7] = 0x9274; /* glInternalFormat ETC2 RGB (conteúdo ETC1) */
    u[8] = 0x1907;     /* glBaseInternalFormat RGB */
  } else if (mode == M_RGBA4444) {
    u[4] = 0x8033;     /* glType UNSIGNED_SHORT_4_4_4_4 */ u[5] = 2;
    u[6] = 0x1908;     /* glFormat RGBA */   u[7] = 0x1908; /* glInternalFormat RGBA */
    u[8] = 0x1908;     /* glBaseInternalFormat RGBA */
  } else if (mode == M_RGBA5551) {
    u[4] = 0x8034;     /* glType UNSIGNED_SHORT_5_5_5_1 */ u[5] = 2;
    u[6] = 0x1908;     /* glFormat RGBA */   u[7] = 0x1908; /* glInternalFormat RGBA */
    u[8] = 0x1908;     /* glBaseInternalFormat RGBA */
  } else {                                  /* RGBA8 (32 bpp, igual SOR4 sem compressão) */
    u[4] = 0x1401;     /* glType UNSIGNED_BYTE */ u[5] = 1;
    u[6] = 0x1908;     /* glFormat RGBA */   u[7] = 0x1908; /* glInternalFormat RGBA */
    u[8] = 0x1908;     /* glBaseInternalFormat RGBA */
  }
  u[9] = w; u[10] = h; u[11] = 0;           /* pixelW/H/Depth */
  u[12] = 0; u[13] = 1; u[14] = 1;          /* array/faces/levels */
  u[15] = 0;                                /* bytesOfKeyValueData */
  *(uint32_t *)(k + 64) = isz;
  memcpy(k + 68, img, isz);
  *olen = 64 + 4 + isz;
  return k;
}

/* 🔑 TRANSCODE NATIVO (sem cache, sem JPEG): um KTX ETC2-RGB (0x9274/0x9275) vira
 * ETC1 PRESERVANDO a estrutura EXATA (mesmos mipLevels, imageSizes e padding) — só o
 * conteúdo dos blocos 8B muda (ETC2-RGB -> ETC1). Assim o KtxImageLoader fechado da
 * engine carrega IGUAL ao original (mesma cadeia de mips = SEM crash) e chama
 * glCompressedTexImage2D por nível; o hook (imports.c) sobe os blocos como ETC1
 * (0x8D64), que o Mali-450 amostra nativo (4bpp). ETC1 é subconjunto do ETC2-RGB:
 * só usamos os modos individual/differential (etc1_encode), nunca T/H/planar.
 * Devolve novo buffer (MESMO tamanho); NULL se não for ETC2-RGB transcodável. */
static unsigned char *transcode_etc2rgb_to_etc1(const unsigned char *k, long klen, long *olen) {
  if (klen < 64 || memcmp(k, KTX_ID, 12) != 0) return NULL;
  uint32_t gif = *(const uint32_t *)(k + 28);
  if (gif != 0x9274 && gif != 0x9275) return NULL;   /* só ETC2 RGB (opaco) */
  uint32_t w = *(const uint32_t *)(k + 36), h = *(const uint32_t *)(k + 40);
  uint32_t levels = *(const uint32_t *)(k + 56); if (levels == 0) levels = 1;  /* mipLevels @56 */
  uint32_t kv = *(const uint32_t *)(k + 60);
  unsigned char *out = malloc(klen); if (!out) return NULL;
  memcpy(out, k, klen);                               /* preserva tudo; sobrescreve blocos */
  long q = 64 + kv;
  for (uint32_t lvl = 0; lvl < levels; lvl++) {
    if (q + 4 > klen) { free(out); return NULL; }
    uint32_t isz = *(const uint32_t *)(k + q); long dq = q + 4;
    if (dq + isz > klen) { free(out); return NULL; }
    int wl = (int)(w >> lvl); if (wl < 1) wl = 1;
    int hl = (int)(h >> lvl); if (hl < 1) hl = 1;
    int bw = (wl + 3) / 4, bh = (hl + 3) / 4;         /* nº de blocos 4x4 */
    int pw = bw * 4, ph = bh * 4;                     /* dims arredondadas p/ bloco */
    if ((long)bw * bh * 8 == (long)isz) {             /* confere: ETC2-RGB = 8B/bloco */
      unsigned char *rgba = etc2_decode_rgba(0x9274, pw, ph, k + dq, isz);
      if (rgba) { etc1_encode_image(rgba, pw, ph, 4, out + dq); free(rgba); }
      /* se decode falhar, mantém o bloco ETC2 original (out já é cópia) */
    }
    long pad = (4 - ((long)isz & 3)) & 3;             /* mipPadding (0 p/ blocos 8B) */
    q = dq + isz + pad;
  }
  *olen = klen;
  return out;
}

/* deflata buf -> heap (*olen). nível 6. */
static unsigned char *deflate_buf(const unsigned char *in, long ilen, long *olen) {
  unsigned long cap = z_compressBound(ilen);
  unsigned char *out = malloc(cap); if (!out) return NULL;
  if (z_compress2(out, &cap, in, ilen, 6) != 0) { free(out); return NULL; }
  *olen = (long)cap; return out;
}

/* ---------- pak ---------- */
/* meta por entrada no índice: off, size, m0(mtime/hash), m1(FLAG ZLIB: 1=comprimido). */
typedef struct { char name[256]; long fpos; uint32_t off, size, m0, m1; } Ent;

/* comparador bytewise (unsigned) p/ ordenar IGUAL à engine (busca binária). */
static int ent_cmp_names(const char *a, const char *b) {
  const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
  while (*x && *x == *y) { x++; y++; }
  return (int)*x - (int)*y;
}
static int ent_cmp(const void *a, const void *b) {
  return ent_cmp_names(((const Ent *)a)->name, ((const Ent *)b)->name);
}

static double g_scale = 1.0;
static int g_alpha_scan = 1;
static int g_mode = M_ETC1;    /* default: opaca->ETC1(4bpp); alpha->5551(recorte)/4444(suave) */

/* box-downscale RGBA por 2 (cada dim /2, min 1). devolve buf novo + *ow,*oh. */
static unsigned char *halve_rgba(const unsigned char *s, int w, int h, int *ow, int *oh) {
  int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
  unsigned char *o = malloc((long)nw * nh * 4); if (!o) return NULL;
  for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) {
    int x0 = x * 2, y0 = y * 2, x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
    const unsigned char *a = s+((long)y0*w+x0)*4, *b = s+((long)y0*w+x1)*4,
                        *c = s+((long)y1*w+x0)*4, *d = s+((long)y1*w+x1)*4;
    unsigned char *p = o + ((long)y*nw+x)*4;
    for (int k = 0; k < 4; k++) p[k] = (a[k]+b[k]+c[k]+d[k]) >> 2;
  }
  *ow = nw; *oh = nh; return o;
}

/* encoda 1 nível RGBA -> formato `mode`. ETC1 pad p/ múltiplo de 4. devolve buf + *sz. */
static unsigned char *encode_level(const unsigned char *rgba, int w, int h, int mode, uint32_t *sz) {
  if (mode == M_ETC1) {
    int pw = (w + 3) & ~3, ph = (h + 3) & ~3;
    const unsigned char *src = rgba; unsigned char *pad = NULL;
    if (pw != w || ph != h) {                /* replica borda p/ completar o bloco 4x4 */
      pad = malloc((long)pw * ph * 4); if (!pad) return NULL;
      for (int y = 0; y < ph; y++) for (int x = 0; x < pw; x++) {
        int sx = x < w ? x : w-1, sy = y < h ? y : h-1;
        memcpy(pad+((long)y*pw+x)*4, rgba+((long)sy*w+sx)*4, 4);
      }
      src = pad;
    }
    *sz = (uint32_t)(pw/4) * (ph/4) * 8;
    unsigned char *o = malloc(*sz); if (o) etc1_encode_image(src, pw, ph, 4, o);
    free(pad); return o;
  } else if (mode == M_RGBA4444 || mode == M_RGBA5551) {
    *sz = (uint32_t)w * h * 2; unsigned char *o = malloc(*sz);
    if (o) { if (mode == M_RGBA4444) pack_4444(rgba, w, h, o); else pack_5551(rgba, w, h, o); }
    return o;
  }
  *sz = (uint32_t)w * h * 4; unsigned char *o = malloc(*sz);
  if (o) memcpy(o, rgba, *sz); return o;
}

/* 🔑 KTX com CADEIA DE MIPS COMPLETA (igual os .ktx originais do jogo). O KtxImageLoader
 * FECHADO espera numberOfMipmapLevels = log2(max(w,h))+1 e faz NULL-deref se vier só 1
 * nível -> SIGSEGV. Gera todos os níveis (box-filter) no formato `mode`. */
static unsigned char *build_ktx_mips(const unsigned char *rgba0, int w, int h, int mode, long *olen) {
  int levels = 1; { int m = w > h ? w : h; while (m > 1) { m >>= 1; levels++; } }
  if (levels > 20) levels = 20;
  unsigned char *ld[20]; uint32_t lsz[20];
  unsigned char *cur = (unsigned char *)rgba0; int cw = w, ch = h; unsigned char *tofree = NULL;
  int ok = 1;
  for (int l = 0; l < levels; l++) {
    ld[l] = encode_level(cur, cw, ch, mode, &lsz[l]);
    if (!ld[l]) { ok = 0; levels = l; break; }
    if (l + 1 < levels) { int nw, nh; unsigned char *nx = halve_rgba(cur, cw, ch, &nw, &nh);
      free(tofree); tofree = nx; cur = nx; cw = nw; ch = nh; if (!nx) { levels = l + 1; break; } }
  }
  free(tofree);
  if (!ok && levels == 0) return NULL;
  long total = 64; for (int l = 0; l < levels; l++) total += 4 + lsz[l] + ((4 - (lsz[l] & 3)) & 3);
  unsigned char *k = calloc(1, total); if (!k) { for (int l=0;l<levels;l++) free(ld[l]); return NULL; }
  memcpy(k, KTX_ID, 12); uint32_t *u = (uint32_t *)k; u[3] = 0x04030201;
  if (mode == M_ETC1)        { u[4]=0; u[5]=1; u[6]=0; u[7]=0x9274; u[8]=0x1907; }
  else if (mode == M_RGBA4444){ u[4]=0x8033; u[5]=2; u[6]=0x1908; u[7]=0x1908; u[8]=0x1908; }
  else if (mode == M_RGBA5551){ u[4]=0x8034; u[5]=2; u[6]=0x1908; u[7]=0x1908; u[8]=0x1908; }
  else                       { u[4]=0x1401; u[5]=1; u[6]=0x1908; u[7]=0x1908; u[8]=0x1908; }
  u[9]=w; u[10]=h; u[11]=0; u[12]=0; u[13]=1; u[14]=levels; u[15]=0;
  long q = 64;
  for (int l = 0; l < levels; l++) {
    *(uint32_t *)(k + q) = lsz[l]; q += 4;
    memcpy(k + q, ld[l], lsz[l]); q += lsz[l];
    q += (4 - (lsz[l] & 3)) & 3;            /* mipPadding */
    free(ld[l]);
  }
  *olen = q; return k;
}

/* converte UM RGBA -> blob KTX(zlib) com MIPS. devolve blob + *blen, e *out_mode. */
static unsigned char *rgba_to_blob(unsigned char *rgba, int w, int h, int hint_alpha,
                                   long *blen, int *out_mode) {
  int nw = w, nh = h; unsigned char *ds = downscale(rgba, w, h, g_scale, &nw, &nh);
  const unsigned char *src = ds ? ds : rgba; w = nw; h = nh;

  int mode = g_mode;
  if (g_mode != M_RGBA8) {                               /* opaca->ETC1, alpha->5551/4444 */
    int alpha = hint_alpha; if (g_alpha_scan) alpha = has_real_alpha(src, w, h);
    if (!alpha) mode = M_ETC1;
    else if (is_cutout_alpha(src, w, h)) mode = M_RGBA5551;
    else mode = M_RGBA4444;
  }
  long klen; unsigned char *ktx = build_ktx_mips(src, w, h, mode, &klen);
  free(ds); if (!ktx) return NULL;
  unsigned char *blob = deflate_buf(ktx, klen, blen);
  free(ktx);
  *out_mode = mode;
  return blob;
}

static int pread_at(FILE *f, long off, void *buf, long n) {
  if (fseek(f, off, SEEK_SET)) return -1;
  return fread(buf, 1, n, f) == (size_t)n ? 0 : -1;
}

static int bake_one(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "texbake: abrir %s\n", path); return -1; }
  fseek(f, 0, SEEK_END); long flen = ftell(f);
  unsigned char hdr[12];
  if (pread_at(f, 0, hdr, 12) || memcmp(hdr, "PAK\0V11\0", 8)) { fprintf(stderr, "texbake: %s nao e pak\n", path); fclose(f); return -1; }
  uint32_t idx = *(uint32_t *)(hdr + 8);
  long idxlen = flen - idx;
  if (idxlen <= 0 || idxlen > 64L*1024*1024) { fprintf(stderr, "texbake: indice %s\n", path); fclose(f); return -1; }
  unsigned char *idxbuf = malloc(idxlen);
  if (!idxbuf || pread_at(f, idx, idxbuf, idxlen)) { fprintf(stderr, "texbake: ler indice\n"); fclose(f); free(idxbuf); return -1; }

  /* 🔑 o índice começa com u32 COUNT (nº de entradas); as entradas vêm ORDENADAS por
   * nome (a engine faz BUSCA BINÁRIA). Pular o count e, no fim, REORDENAR + reescrever
   * o count — senão as entradas novas (fora de ordem) não são achadas. */
  uint32_t orig_count = *(uint32_t *)idxbuf;
  (void)orig_count;
  long cap = 8192; Ent *ent = malloc(cap * sizeof(Ent)); int ne = 0; long p = 4;
  while (p < idxlen - 16) {
    long e = p; while (e < idxlen && idxbuf[e]) e++;
    if (e >= idxlen || e == p || e - p > 250) break;
    if (ne >= cap) { cap *= 2; ent = realloc(ent, cap * sizeof(Ent)); }
    int n = (int)(e - p);
    memcpy(ent[ne].name, idxbuf + p, n); ent[ne].name[n] = 0;
    ent[ne].fpos = e + 1;
    ent[ne].off = *(uint32_t *)(idxbuf + e + 1);
    ent[ne].size = *(uint32_t *)(idxbuf + e + 5);
    ent[ne].m0 = *(uint32_t *)(idxbuf + e + 9);    /* mtime/hash */
    ent[ne].m1 = *(uint32_t *)(idxbuf + e + 13);   /* flag zlib (1=comprimido) */
    ne++; p = e + 1 + 16;
  }

  /* índice de nome -> entrada (p/ achar o .ktx irmão) */
  /* (linear é ok offline; pak tem ~25k entradas) */
  char tmp[600]; snprintf(tmp, sizeof tmp, "%s.bake", path);
  FILE *out = fopen(tmp, "wb");
  if (!out) { fprintf(stderr, "texbake: criar %s\n", tmp); fclose(f); free(idxbuf); free(ent); return -1; }

  /* copia [0..idx) verbatim em blocos de 1MB */
  { unsigned char *cp = malloc(1<<20); long left = idx, o = 0;
    while (left > 0) { long c = left < (1<<20) ? left : (1<<20);
      if (pread_at(f, o, cp, c)) { c = 0; } if (c <= 0) break;
      if (fwrite(cp, 1, c, out) != (size_t)c) { fprintf(stderr, "texbake: write curto (disco cheio?)\n"); fclose(out); fclose(f); free(cp); free(idxbuf); free(ent); remove(tmp); return -1; }
      o += c; left -= c; }
    free(cp);
  }
  long running = 0; long baked = 0, etc1n = 0, rgban = 0;

  /* p/ cada .jpg/.png, gera/atualiza o irmão .ktx */
  for (int i = 0; i < ne; i++) {
    const char *nm = ent[i].name; int L = (int)strlen(nm);
    if (!(L > 4 && (!strcmp(nm + L - 4, ".jpg") || !strcmp(nm + L - 4, ".png")))) continue;

    /* 1) tenta os pixels do .ktx ETC2 existente */
    char kn[270]; snprintf(kn, sizeof kn, "%s.ktx", nm);
    int ki = -1; for (int j = 0; j < ne; j++) if (!strcmp(ent[j].name, kn)) { ki = j; break; }
    unsigned char *rgba = NULL; int w = 0, h = 0, alpha = 0;
    if (ki >= 0 && ent[ki].size > 0) {
      unsigned char *kc = malloc(ent[ki].size);
      if (kc && !pread_at(f, ent[ki].off, kc, ent[ki].size)) {
        long klen; unsigned char *ktx = maybe_inflate(kc, ent[ki].size, &klen);
        if (ktx) { rgba = ktx_to_rgba(ktx, klen, &w, &h, &alpha); free(ktx); }
      }
      free(kc);
    }
    /* 2) senão, o .jpg/.png real */
    if (!rgba && ent[i].size > 0) {
      unsigned char *jc = malloc(ent[i].size);
      if (jc && !pread_at(f, ent[i].off, jc, ent[i].size)) {
        int comp; rgba = stbi_load_from_memory(jc, ent[i].size, &w, &h, &comp, 4);
        alpha = (nm[L-3] == 'p');  /* .png = provável alpha (refina com scan) */
      }
      free(jc);
    }
    if (!rgba) continue;

    long blen; int used_mode;
    unsigned char *blob = rgba_to_blob(rgba, w, h, alpha, &blen, &used_mode);
    free(rgba);
    if (!blob || blen <= 0) { free(blob); continue; }

    /* 🔑 ETC1 DIRETO: só escreve o .ktx pras OPACAS (ETC1 = glType=0 comprimido, que o
     * KtxImageLoader FECHADO aceita — estrutura idêntica ao original). Texturas com ALPHA
     * (4444/5551 = uncompressed, glType!=0) fazem o loader dar NULL-deref/SIGSEGV; então
     * deixamos a alpha INTACTA (PNG/JPEG original ou .ktx ETC2 original). */
    if (used_mode != M_ETC1) { free(blob); rgban++; continue; }

    fwrite(blob, 1, blen, out);
    free(blob);
    /* a entrada do .ktx irmão passa a apontar pro blob novo (cria se não existia).
     * 🔑 m1=1 (FLAG ZLIB): o blob é deflatado -> a engine PRECISA inflar antes do
     * KtxImageLoader. Sem isso (m1=0) ela passa zlib cru -> falha (o bug do F1). */
    if (ki >= 0) { ent[ki].off = (uint32_t)(idx + running); ent[ki].size = (uint32_t)blen; ent[ki].m1 = 1; }
    else {
      /* adiciona nova entrada .ktx (vai pro índice depois); herda mtime do source */
      if (ne >= cap) { cap *= 2; ent = realloc(ent, cap * sizeof(Ent)); }
      strcpy(ent[ne].name, kn); ent[ne].fpos = -1;
      ent[ne].off = (uint32_t)(idx + running); ent[ne].size = (uint32_t)blen;
      ent[ne].m0 = ent[i].m0; ent[ne].m1 = 1; ne++;
    }
    running += blen;
    baked++; if (used_mode == M_ETC1) etc1n++; else rgban++;
    if (baked % 500 == 0) fprintf(stderr, "  ... %ld convertidas\n", baked);
  }

  /* 🔑 ORDENA por nome (bytewise asc) — a engine faz busca binária; sem isso as
   * entradas novas não são achadas (o bug do "rosa"/source doesn't exist). */
  qsort(ent, ne, sizeof(Ent), ent_cmp);

  /* reescreve o índice: u32 COUNT + entradas [nome\0 + off + size + m0 + m1]. */
  long newidx = idx + running;
  uint32_t cnt = (uint32_t)ne;
  fwrite(&cnt, 4, 1, out);                 /* 🔑 header de contagem (faltava!) */
  for (int i = 0; i < ne; i++) {
    fwrite(ent[i].name, 1, strlen(ent[i].name) + 1, out);
    fwrite(&ent[i].off, 4, 1, out);
    fwrite(&ent[i].size, 4, 1, out);
    fwrite(&ent[i].m0, 4, 1, out);
    fwrite(&ent[i].m1, 4, 1, out);
  }
  long newlen = ftell(out);
  fseek(out, 8, SEEK_SET);
  uint32_t ni = (uint32_t)newidx, nl = (uint32_t)newlen;
  fwrite(&ni, 4, 1, out); fwrite(&nl, 4, 1, out);
  fclose(out); fclose(f); free(idxbuf); free(ent);

  if (baked > 0) {
    if (rename(tmp, path)) { fprintf(stderr, "texbake: rename falhou\n"); return -1; }
    fprintf(stderr, "texbake: %s -> %ld texturas (ETC1=%ld outras[RGBA8/4444]=%ld) idx@%ld size=%ld\n",
            path, baked, etc1n, rgban, newidx, newlen);
  } else { remove(tmp); fprintf(stderr, "texbake: %s -> nada\n", path); }
  return 0;
}

/* =========================================================================
 *  MODO ETC1MIP (NATIVO, sem cache/JPEG): reescreve o pak transcodando CADA .ktx
 *  ETC2-RGB (opaco) -> ETC1 preservando a cadeia de mips. Os .ktx de alpha
 *  (ETC2 punchthrough/RGBA 0x9276..0x9279) e o resto ficam INTACTOS (o hook
 *  decodifica ETC2-alpha -> RGBA8 em runtime). A engine carrega os .ktx via
 *  KtxImageLoader (KTX_REDIRECT) -> sem JPEG, sem cache, ~metade do disco (SOR4).
 * ========================================================================= */
static int bake_etc1mip(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "texbake: abrir %s\n", path); return -1; }
  fseek(f, 0, SEEK_END); long flen = ftell(f);
  unsigned char hdr[12];
  if (pread_at(f, 0, hdr, 12) || memcmp(hdr, "PAK\0V11\0", 8)) { fprintf(stderr, "texbake: %s nao e pak\n", path); fclose(f); return -1; }
  uint32_t idx = *(uint32_t *)(hdr + 8);
  long idxlen = flen - idx;
  if (idxlen <= 0 || idxlen > 64L*1024*1024) { fprintf(stderr, "texbake: indice %s\n", path); fclose(f); return -1; }
  unsigned char *idxbuf = malloc(idxlen);
  if (!idxbuf || pread_at(f, idx, idxbuf, idxlen)) { fprintf(stderr, "texbake: ler indice\n"); fclose(f); free(idxbuf); return -1; }

  long cap = 8192; Ent *ent = malloc(cap * sizeof(Ent)); int ne = 0; long p = 4;
  while (p < idxlen - 16) {
    long e = p; while (e < idxlen && idxbuf[e]) e++;
    if (e >= idxlen || e == p || e - p > 250) break;
    if (ne >= cap) { cap *= 2; ent = realloc(ent, cap * sizeof(Ent)); }
    int n = (int)(e - p);
    memcpy(ent[ne].name, idxbuf + p, n); ent[ne].name[n] = 0;
    ent[ne].fpos = e + 1;
    ent[ne].off = *(uint32_t *)(idxbuf + e + 1);
    ent[ne].size = *(uint32_t *)(idxbuf + e + 5);
    ent[ne].m0 = *(uint32_t *)(idxbuf + e + 9);
    ent[ne].m1 = *(uint32_t *)(idxbuf + e + 13);
    ne++; p = e + 1 + 16;
  }

  char tmp[600]; snprintf(tmp, sizeof tmp, "%s.bake", path);
  FILE *out = fopen(tmp, "wb");
  if (!out) { fprintf(stderr, "texbake: criar %s\n", tmp); fclose(f); free(idxbuf); free(ent); return -1; }
  { unsigned char *cp = malloc(1<<20); long left = idx, o = 0;
    while (left > 0) { long c = left < (1<<20) ? left : (1<<20);
      if (pread_at(f, o, cp, c)) { c = 0; } if (c <= 0) break;
      if (fwrite(cp, 1, c, out) != (size_t)c) { fprintf(stderr, "texbake: write curto (disco cheio?)\n"); fclose(out); fclose(f); free(cp); free(idxbuf); free(ent); remove(tmp); return -1; }
      o += c; left -= c; }
    free(cp);
  }
  long running = 0, done = 0, skipped = 0;
  for (int i = 0; i < ne; i++) {
    const char *nm = ent[i].name; int L = (int)strlen(nm);
    if (!(L > 4 && !strcmp(nm + L - 4, ".ktx")) || ent[i].size == 0) continue;
    unsigned char *kc = malloc(ent[i].size);
    if (!kc || pread_at(f, ent[i].off, kc, ent[i].size)) { free(kc); continue; }
    long klen; unsigned char *ktx = maybe_inflate(kc, ent[i].size, &klen);
    free(kc);
    if (!ktx) continue;
    long tlen; unsigned char *t = transcode_etc2rgb_to_etc1(ktx, klen, &tlen);
    free(ktx);
    if (!t) { skipped++; continue; }                 /* alpha/outro -> intacto */
    long blen; unsigned char *blob = deflate_buf(t, tlen, &blen);
    free(t);
    if (!blob || blen <= 0) { free(blob); continue; }
    fwrite(blob, 1, blen, out); free(blob);
    ent[i].off = (uint32_t)(idx + running); ent[i].size = (uint32_t)blen; ent[i].m1 = 1;
    running += blen; done++;
    if (done % 1000 == 0) fprintf(stderr, "  ... %ld transcodadas\n", done);
  }

  qsort(ent, ne, sizeof(Ent), ent_cmp);
  long newidx = idx + running;
  uint32_t cnt = (uint32_t)ne;
  fwrite(&cnt, 4, 1, out);
  for (int i = 0; i < ne; i++) {
    fwrite(ent[i].name, 1, strlen(ent[i].name) + 1, out);
    fwrite(&ent[i].off, 4, 1, out); fwrite(&ent[i].size, 4, 1, out);
    fwrite(&ent[i].m0, 4, 1, out); fwrite(&ent[i].m1, 4, 1, out);
  }
  long newlen = ftell(out);
  fseek(out, 8, SEEK_SET);
  uint32_t ni = (uint32_t)newidx, nl = (uint32_t)newlen;
  fwrite(&ni, 4, 1, out); fwrite(&nl, 4, 1, out);
  fclose(out); fclose(f); free(idxbuf); free(ent);
  if (done > 0) {
    if (rename(tmp, path)) { fprintf(stderr, "texbake: rename falhou\n"); return -1; }
    fprintf(stderr, "texbake[etc1mip]: %s -> %ld ETC1 (mips), %ld intactos, idx@%ld size=%ld\n",
            path, done, skipped, newidx, newlen);
  } else { remove(tmp); fprintf(stderr, "texbake[etc1mip]: %s -> nada\n", path); }
  return 0;
}

/* =========================================================================
 *  MODO LEANPAK (corta a duplicação de disco): REPACK do pak usando o CACHE
 *  ETC1 já gerado. Para cada textura .jpg/.png COBERTA pelo cache (opaca), grava
 *  um PLACEHOLDER sólido full-dim (~1-3KB) — o jogo lê os pixels REAIS do cache no
 *  hook (imports.c), então o conteúdo do placeholder é irrelevante (só importam as
 *  DIMS, que casam com o cache). Texturas NÃO cobertas (alpha) vêm REAIS (decodifica
 *  o .ktx se o slot estiver vazio, igual fixpak). Os .ktx ETC2 (mortos no runtime,
 *  só serviam de fonte) são DROPADOS. Resultado: pak ~enxuto + cache = ~metade do
 *  disco da v4 (sem JPEG/PNG real duplicando o cache). Runtime IDÊNTICO ao validado.
 * ========================================================================= */
typedef struct { char name[256]; int w, h; } Cov;
static Cov *g_cov = NULL; static long g_ncov = 0;
static int cov_cmp(const void *a, const void *b) { return ent_cmp_names(((const Cov *)a)->name, ((const Cov *)b)->name); }
static int cov_load(const char *path) {
  FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "texbake: cache %s nao abre\n", path); return -1; }
  unsigned char h[16]; if (fread(h, 1, 16, f) != 16 || memcmp(h, "ETC1CACH", 8)) { fclose(f); fprintf(stderr, "texbake: cache magic ruim\n"); return -1; }
  uint32_t count = *(uint32_t *)(h + 8);
  g_cov = malloc((size_t)count * sizeof(Cov)); g_ncov = 0;
  for (uint32_t i = 0; i < count; i++) {
    char nm[256]; int n = 0; int c;
    while ((c = fgetc(f)) > 0 && n < 255) nm[n++] = (char)c;
    nm[n] = 0; if (c < 0) break;
    uint16_t w, hh; uint32_t bo, bs;
    if (fread(&w,2,1,f)!=1||fread(&hh,2,1,f)!=1||fread(&bo,4,1,f)!=1||fread(&bs,4,1,f)!=1) break;
    strcpy(g_cov[g_ncov].name, nm); g_cov[g_ncov].w = w; g_cov[g_ncov].h = hh; g_ncov++;
  }
  fclose(f);
  qsort(g_cov, g_ncov, sizeof(Cov), cov_cmp);   /* já vem ordenado, mas garante */
  fprintf(stderr, "texbake[leanpak]: cache cobre %ld texturas\n", g_ncov);
  return 0;
}
static const Cov *cov_find(const char *name) {
  long lo = 0, hi = g_ncov - 1;
  while (lo <= hi) { long m = (lo + hi) / 2; int c = ent_cmp_names(g_cov[m].name, name);
    if (c == 0) return &g_cov[m]; if (c < 0) lo = m + 1; else hi = m - 1; }
  return NULL;
}
/* placeholder JPEG sólido (cinza) WxH — minúsculo. */
static unsigned char *solid_jpeg(int w, int h, long *len) {
  long n = (long)w * h * 4; unsigned char *rgba = malloc(n); if (!rgba) return NULL;
  for (long i = 0; i < n; i += 4) { rgba[i]=128; rgba[i+1]=128; rgba[i+2]=128; rgba[i+3]=255; }
  unsigned char *j = jpeg_encode_rgba(rgba, w, h, 4, 50, len); free(rgba); return j;
}

static int bake_leanpak(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "texbake: abrir %s\n", path); return -1; }
  fseek(f, 0, SEEK_END); long flen = ftell(f);
  unsigned char hdr[12];
  if (pread_at(f, 0, hdr, 12) || memcmp(hdr, "PAK\0V11\0", 8)) { fprintf(stderr, "texbake: %s nao e pak\n", path); fclose(f); return -1; }
  uint32_t idx = *(uint32_t *)(hdr + 8);
  long idxlen = flen - idx;
  unsigned char *idxbuf = malloc(idxlen);
  if (!idxbuf || pread_at(f, idx, idxbuf, idxlen)) { fprintf(stderr, "texbake: ler indice\n"); fclose(f); free(idxbuf); return -1; }
  long cap = 8192; Ent *ent = malloc(cap * sizeof(Ent)); int ne = 0; long p = 4;
  while (p < idxlen - 16) {
    long e = p; while (e < idxlen && idxbuf[e]) e++;
    if (e >= idxlen || e == p || e - p > 250) break;
    if (ne >= cap) { cap *= 2; ent = realloc(ent, cap * sizeof(Ent)); }
    int n = (int)(e - p); memcpy(ent[ne].name, idxbuf + p, n); ent[ne].name[n] = 0;
    ent[ne].off = *(uint32_t *)(idxbuf+e+1); ent[ne].size = *(uint32_t *)(idxbuf+e+5);
    ent[ne].m0 = *(uint32_t *)(idxbuf+e+9); ent[ne].m1 = *(uint32_t *)(idxbuf+e+13);
    ne++; p = e + 1 + 16;
  }
  free(idxbuf);

  char tmp[600]; snprintf(tmp, sizeof tmp, "%s.lean", path);
  FILE *out = fopen(tmp, "wb");
  if (!out) { fprintf(stderr, "texbake: criar %s\n", tmp); fclose(f); free(ent); return -1; }
  /* repack: header 16B (magic8 + idx@8 + totalLen@12) + dados compactos a partir de 16. */
  unsigned char zero16[16] = {0}; fwrite(zero16, 1, 16, out);
  long running = 16;
  Ent *keep = malloc(ne * sizeof(Ent)); int nk = 0;
  long ph = 0, filled = 0, kept = 0, dropped = 0;
  unsigned char *cpbuf = malloc(1 << 20);
  for (int i = 0; i < ne; i++) {
    const char *nm = ent[i].name; int L = (int)strlen(nm);
    int is_ktx = (L > 4 && !strcmp(nm + L - 4, ".ktx"));
    if (is_ktx) { dropped++; continue; }                 /* DROPA .ktx (morto no runtime) */
    int is_tex = (L > 4 && (!strcmp(nm + L - 4, ".jpg") || !strcmp(nm + L - 4, ".png")));
    Ent ke = ent[i]; ke.off = (uint32_t)running;
    const Cov *cv = is_tex ? cov_find(nm) : NULL;
    if (cv) {                                            /* coberta pelo cache -> placeholder */
      long jl; unsigned char *j = solid_jpeg(cv->w, cv->h, &jl);
      if (j && jl > 0) { fwrite(j, 1, jl, out); ke.size = (uint32_t)jl; ke.m1 = 0; running += jl; ph++; free(j); }
      else { ke.size = 0; free(j); }
    } else if (is_tex && ent[i].size == 0) {             /* slot vazio NÃO coberto -> preenche real do .ktx */
      char kn[270]; snprintf(kn, sizeof kn, "%s.ktx", nm);
      int ki = -1; for (int j = 0; j < ne; j++) if (!strcmp(ent[j].name, kn)) { ki = j; break; }
      unsigned char *blob = NULL; long blen = 0;
      if (ki >= 0 && ent[ki].size > 0) {
        unsigned char *kc = malloc(ent[ki].size);
        if (kc && !pread_at(f, ent[ki].off, kc, ent[ki].size)) {
          long kl; unsigned char *k = maybe_inflate(kc, ent[ki].size, &kl);
          if (k) { int w,h,al; unsigned char *rgba = ktx_to_rgba(k, kl, &w, &h, &al); free(k);
            if (rgba) {
              if (nm[L-3] == 'p') blob = rgba_to_png(rgba, w, h, &blen);
              else blob = jpeg_encode_rgba(rgba, w, h, 4, 88, &blen);
              free(rgba);
            } }
        }
        free(kc);
      }
      if (blob && blen > 0) { fwrite(blob, 1, blen, out); ke.size = (uint32_t)blen; ke.m1 = 0; running += blen; filled++; }
      else { ke.size = 0; }
      free(blob);
    } else {                                             /* mantém o dado original verbatim */
      long left = ent[i].size, o = ent[i].off;
      while (left > 0) { long c = left < (1<<20) ? left : (1<<20);
        if (pread_at(f, o, cpbuf, c)) break; fwrite(cpbuf, 1, c, out); o += c; left -= c; }
      running += ent[i].size; kept++;
    }
    keep[nk++] = ke;
  }
  free(cpbuf);
  /* índice: u32 COUNT + entradas ORDENADAS [nome\0 off size m0 m1] */
  qsort(keep, nk, sizeof(Ent), ent_cmp);
  long newidx = running; uint32_t cnt = (uint32_t)nk;
  fwrite(&cnt, 4, 1, out);
  for (int i = 0; i < nk; i++) {
    fwrite(keep[i].name, 1, strlen(keep[i].name) + 1, out);
    fwrite(&keep[i].off, 4, 1, out); fwrite(&keep[i].size, 4, 1, out);
    fwrite(&keep[i].m0, 4, 1, out); fwrite(&keep[i].m1, 4, 1, out);
  }
  long newlen = ftell(out);
  fseek(out, 0, SEEK_SET);
  fwrite("PAK\0V11\0", 1, 8, out);        /* magic (8B) */
  uint32_t ni = (uint32_t)newidx, nl = (uint32_t)newlen;
  fwrite(&ni, 4, 1, out);                 /* idx offset @8 */
  fwrite(&nl, 4, 1, out);                 /* total file len @12 */
  /* 🛡️ ATÔMICO: só sobrescreve o pak ORIGINAL se a escrita inteira deu certo
   * (disco cheio = ferror). Senão joga fora o .lean e PRESERVA o original (o
   * launcher cai pro fixpak). Nunca deixa um pak corrompido no lugar do bom. */
  int werr = ferror(out) || fflush(out) != 0;
  fclose(out); fclose(f); free(ent); free(keep);
  if (werr) { fprintf(stderr, "texbake[leanpak]: escrita falhou (disco cheio?) -> preserva original\n"); remove(tmp); return -1; }
  if (rename(tmp, path)) { fprintf(stderr, "texbake: rename %s falhou\n", tmp); remove(tmp); return -1; }
  fprintf(stderr, "texbake[leanpak]: %s -> placeholder=%ld filled=%ld kept=%ld dropped_ktx=%ld idx@%ld size=%ld\n",
          path, ph, filled, kept, dropped, newidx, newlen);
  return 0;
}

/* =========================================================================
 *  MODO SIDETABLE: NÃO mexe no pak. Gera um cache nome->ETC1 (só OPACAS) que o
 *  binário carrega e usa no hook my_glTexImage2D (troca o ENCODE em runtime por
 *  LOOKUP). A engine carrega o .jpg/.png normal (imagem completa, sem crash/pink);
 *  o hook substitui a opaca pela ETC1 pré-bakeada -> ZERO encode em runtime.
 *
 *  Formato do arquivo (.etc1cache):
 *    "ETC1CACH"(8) + u32 count + u32 data_off
 *    índice (ORDENADO por nome) count× [ nome\0 | u16 w | u16 h | u32 blob_off | u32 blob_size ]
 *    data @data_off: blobs ETC1 concatenados
 * ========================================================================= */
/* 🪶 STREAMING: NÃO guarda os blobs ETC1 na RAM (seriam ~600MB -> OOM no device 1GB).
 * Cada ETC1 é gravado NA HORA num arquivo .tmpdata; só o índice (nome+dims+offset)
 * fica na RAM (~4MB). No fim: header + índice ordenado + copia o .tmpdata. */
typedef struct { char name[256]; int w, h; long blob_off; long size; } CacheEnt;
static CacheEnt *g_cache = NULL; static long g_ncache = 0, g_ccap = 0;
static FILE *g_tmpdata = NULL; static long g_tmpoff = 0;
static char g_tmppath[1024];

static int cache_cmp(const void *a, const void *b) {
  return ent_cmp_names(((const CacheEnt *)a)->name, ((const CacheEnt *)b)->name);
}

static int g_nthreads = 0;   /* 0 = auto (nproc) */

/* bsearch em ent[] (que está ORDENADO por nome) — acha o .ktx irmao rapido. */
static int ent_find(Ent *ent, int ne, const char *key) {
  int lo = 0, hi = ne - 1;
  while (lo <= hi) { int m = (lo + hi) / 2; int c = ent_cmp_names(ent[m].name, key);
    if (c == 0) return m; if (c < 0) lo = m + 1; else hi = m - 1; }
  return -1;
}

/* cada worker processa um SLICE das texturas (round-robin), grava ETC1 no SEU temp
 * (sem contencao) e indexa local. Merge no fim. Igual SOR4: usa todos os nucleos. */
typedef struct {
  const char *path; Ent *ent; int ne; int *jobs; int njobs; int tid, nthreads;
  char tmppath[1100]; FILE *tmp; long tmpoff;
  CacheEnt *cache; long ncache, ccap; long done;
} Worker;

static void *worker_fn(void *arg) {
  Worker *w = (Worker *)arg;
  FILE *f = fopen(w->path, "rb");
  if (!f) return NULL;
  for (int j = w->tid; j < w->njobs; j += w->nthreads) {
    int i = w->jobs[j];
    const char *nm = w->ent[i].name;
    char kn[270]; snprintf(kn, sizeof kn, "%s.ktx", nm);
    int ki = ent_find(w->ent, w->ne, kn);
    unsigned char *rgba = NULL; int wd = 0, ht = 0, al = 0;
    if (ki >= 0 && w->ent[ki].size > 0) {
      unsigned char *kc = malloc(w->ent[ki].size);
      if (kc && !pread_at(f, w->ent[ki].off, kc, w->ent[ki].size)) {
        long kl; unsigned char *k = maybe_inflate(kc, w->ent[ki].size, &kl);
        if (k) { rgba = ktx_to_rgba(k, kl, &wd, &ht, &al); free(k); }
      }
      free(kc);
    }
    if (!rgba && w->ent[i].size > 0) {
      unsigned char *jc = malloc(w->ent[i].size);
      if (jc && !pread_at(f, w->ent[i].off, jc, w->ent[i].size)) { int c; rgba = stbi_load_from_memory(jc, w->ent[i].size, &wd, &ht, &c, 4); }
      free(jc);
    }
    if (!rgba) continue;
    if (has_real_alpha(rgba, wd, ht)) { free(rgba); continue; }
    /* downscale só em texturas >=128 (IGUAL ao gate do runtime my_glTexImage2D), senão
     * as dims do cache não casam com o upload e dá MISS-DIM. */
    int nw = wd, nh = ht;
    unsigned char *ds = (wd >= 128 && ht >= 128) ? downscale(rgba, wd, ht, g_scale, &nw, &nh) : NULL;
    const unsigned char *src = ds ? ds : rgba; wd = nw; ht = nh;
    if ((wd & 3) || (ht & 3) || wd < 8 || ht < 8) { free(ds); free(rgba); continue; }
    long sz = (long)(wd / 4) * (ht / 4) * 8;
    unsigned char *etc1 = malloc(sz);
    if (etc1) { etc1_encode_image(src, wd, ht, 4, etc1);
      /* 🔑 grava o bloco ETC1 CRU (sem zlib): formato VALIDADO `ETC1CACH`. O runtime
       * (imports.c) faz mmap PROT_READ -> as páginas ficam no disco, NÃO na RAM (não
       * estoura 1GB) e o upload é direto (zero inflate). O zlib obrigatório (ETC1CAZ1)
       * adicionava dependência de libz + risco e foi o que quebrou na sessão anterior. */
      if (fwrite(etc1, 1, sz, w->tmp) == (size_t)sz) {
        if (w->ncache >= w->ccap) { w->ccap = w->ccap ? w->ccap * 2 : 2048; w->cache = realloc(w->cache, w->ccap * sizeof(CacheEnt)); }
        CacheEnt *ce = &w->cache[w->ncache++];
        strncpy(ce->name, nm, 255); ce->name[255] = 0; ce->w = wd; ce->h = ht;
        ce->blob_off = w->tmpoff; ce->size = sz; w->tmpoff += sz;
        w->done++;
      }
      free(etc1);
    }
    free(ds); free(rgba);
  }
  fclose(f);
  return NULL;
}

static void collect_sidetable(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "texbake: abrir %s\n", path); return; }
  fseek(f, 0, SEEK_END); long flen = ftell(f);
  unsigned char hdr[12];
  if (pread_at(f, 0, hdr, 12) || memcmp(hdr, "PAK\0V11\0", 8)) { fclose(f); return; }
  uint32_t idx = *(uint32_t *)(hdr + 8);
  long idxlen = flen - idx;
  unsigned char *idxbuf = malloc(idxlen);
  if (!idxbuf || pread_at(f, idx, idxbuf, idxlen)) { fclose(f); free(idxbuf); return; }
  long cap = 8192; Ent *ent = malloc(cap * sizeof(Ent)); int ne = 0; long p = 4; /* pula count */
  while (p < idxlen - 16) {
    long e = p; while (e < idxlen && idxbuf[e]) e++;
    if (e >= idxlen || e == p || e - p > 250) break;
    if (ne >= cap) { cap *= 2; ent = realloc(ent, cap * sizeof(Ent)); }
    int n = (int)(e - p); memcpy(ent[ne].name, idxbuf + p, n); ent[ne].name[n] = 0;
    ent[ne].off = *(uint32_t *)(idxbuf + e + 1); ent[ne].size = *(uint32_t *)(idxbuf + e + 5);
    ne++; p = e + 1 + 16;
  }
  fclose(f);
  /* lista de jobs: índices das .jpg/.png */
  int *jobs = malloc((size_t)ne * sizeof(int)); int njobs = 0;
  for (int i = 0; i < ne; i++) {
    const char *nm = ent[i].name; int L = (int)strlen(nm);
    if (L > 4 && (!strcmp(nm + L - 4, ".jpg") || !strcmp(nm + L - 4, ".png"))) jobs[njobs++] = i;
  }
  int NT = g_nthreads > 0 ? g_nthreads : 1;
  Worker *ws = calloc(NT, sizeof(Worker));
  pthread_t *th = malloc(NT * sizeof(pthread_t));
  for (int t = 0; t < NT; t++) {
    ws[t].path = path; ws[t].ent = ent; ws[t].ne = ne; ws[t].jobs = jobs; ws[t].njobs = njobs;
    ws[t].tid = t; ws[t].nthreads = NT;
    snprintf(ws[t].tmppath, sizeof ws[t].tmppath, "%s.t%d", g_tmppath, t);
    ws[t].tmp = fopen(ws[t].tmppath, "wb+");
    pthread_create(&th[t], NULL, worker_fn, &ws[t]);
  }
  long total = 0;
  for (int t = 0; t < NT; t++) {
    pthread_join(th[t], NULL);
    Worker *w = &ws[t];
    long base = g_tmpoff;
    if (w->tmp) {              /* concatena o temp do worker no global, corrige offsets */
      fflush(w->tmp); fseek(w->tmp, 0, SEEK_SET);
      unsigned char *buf = malloc(1 << 20); size_t r;
      while (buf && (r = fread(buf, 1, 1 << 20, w->tmp)) > 0) fwrite(buf, 1, r, g_tmpdata);
      free(buf); fclose(w->tmp); remove(w->tmppath); g_tmpoff += w->tmpoff;
    }
    for (long c = 0; c < w->ncache; c++) {
      if (g_ncache >= g_ccap) { g_ccap = g_ccap ? g_ccap * 2 : 4096; g_cache = realloc(g_cache, g_ccap * sizeof(CacheEnt)); }
      CacheEnt *ce = &g_cache[g_ncache++]; *ce = w->cache[c]; ce->blob_off += base;
    }
    free(w->cache); total += w->done;
  }
  free(ws); free(th); free(jobs); free(idxbuf); free(ent);
  fprintf(stderr, "texbake[sidetable]: %s -> +%ld opacas (%d threads)\n", path, total, NT);
}

static void write_sidetable(const char *out) {
  if (g_tmpdata) { fflush(g_tmpdata); }
  qsort(g_cache, g_ncache, sizeof(CacheEnt), cache_cmp);
  FILE *o = fopen(out, "wb");
  if (!o) { fprintf(stderr, "texbake: criar %s\n", out); return; }
  /* data_off = header(16) + índice. blob_off de cada entrada já aponta no .tmpdata
   * (ordem de coleta); a ordenação só reordena o ÍNDICE, os offsets não mudam. */
  long idxsz = 0;
  for (long i = 0; i < g_ncache; i++) idxsz += strlen(g_cache[i].name) + 1 + 2 + 2 + 4 + 4;
  uint32_t count = (uint32_t)g_ncache, data_off = (uint32_t)(16 + idxsz);
  fwrite("ETC1CACH", 1, 8, o); fwrite(&count, 4, 1, o); fwrite(&data_off, 4, 1, o);  /* blobs ETC1 CRUS (validado) */
  for (long i = 0; i < g_ncache; i++) {
    fwrite(g_cache[i].name, 1, strlen(g_cache[i].name) + 1, o);
    uint16_t w = (uint16_t)g_cache[i].w, h = (uint16_t)g_cache[i].h;
    uint32_t bo = (uint32_t)g_cache[i].blob_off, bs = (uint32_t)g_cache[i].size;
    fwrite(&w, 2, 1, o); fwrite(&h, 2, 1, o); fwrite(&bo, 4, 1, o); fwrite(&bs, 4, 1, o);
  }
  /* copia o .tmpdata (os blobs ETC1) — em blocos, sem segurar tudo na RAM */
  if (g_tmpdata) {
    fseek(g_tmpdata, 0, SEEK_SET);
    unsigned char *buf = malloc(1 << 20); size_t r;
    while (buf && (r = fread(buf, 1, 1 << 20, g_tmpdata)) > 0) fwrite(buf, 1, r, o);
    free(buf); fclose(g_tmpdata); g_tmpdata = NULL; remove(g_tmppath);
  }
  long total = ftell(o); fclose(o);
  fprintf(stderr, "texbake[sidetable]: %s -> %ld texturas, %ld bytes\n", out, g_ncache, total);
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "uso: texbake <pak>... [--scale F] [--rgba8|--etc1] [--sidetable OUT] [--no-alpha-scan]\n"); return 2; }
  if (load_libs()) return 1;
  const char *paks[16]; int np = 0;
  const char *sidetable = NULL; int etc1mip = 0; const char *leancache = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--scale") && i + 1 < argc) g_scale = atof(argv[++i]);
    else if (!strcmp(argv[i], "--rgba8")) g_mode = M_RGBA8;
    else if (!strcmp(argv[i], "--etc1")) g_mode = M_ETC1;     /* opaca->ETC1, alpha->RGBA4444 */
    else if (!strcmp(argv[i], "--etc1mip")) etc1mip = 1;      /* NATIVO: transcoda .ktx ETC2-RGB->ETC1 (mips) */
    else if (!strcmp(argv[i], "--leanpak") && i + 1 < argc) leancache = argv[++i];  /* repack enxuto usando o cache */
    else if (!strcmp(argv[i], "--sidetable") && i + 1 < argc) sidetable = argv[++i];
    else if (!strcmp(argv[i], "--threads") && i + 1 < argc) g_nthreads = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--no-alpha-scan")) g_alpha_scan = 0;
    else if (np < 16) paks[np++] = argv[i];
  }
  if (sidetable) {   /* gera só o cache nome->ETC1, NÃO mexe nos paks */
    if (g_nthreads <= 0) {
      /* _CONF (configurado), NÃO _ONLN: em embedded os cores ficam offline/hotplug
       * e _ONLN reporta 1. Deixa 1 core pro sistema (não congela o device/sshd). */
      long n = sysconf(_SC_NPROCESSORS_CONF);
      if (n <= 0) n = 4;
      g_nthreads = (n > 1) ? (int)(n - 1) : 1;
    }
    if (g_nthreads > 16) g_nthreads = 16;
    fprintf(stderr, "texbake[sidetable]: scale=%.2f threads=%d -> %s (streaming)\n", g_scale, g_nthreads, sidetable);
    snprintf(g_tmppath, sizeof g_tmppath, "%s.tmpdata", sidetable);
    g_tmpdata = fopen(g_tmppath, "wb+");
    if (!g_tmpdata) { fprintf(stderr, "texbake: nao criou temp %s\n", g_tmppath); return 1; }
    for (int i = 0; i < np; i++) collect_sidetable(paks[i]);
    write_sidetable(sidetable);
    return 0;
  }
  if (etc1mip) {   /* NATIVO: reescreve o pak (ETC2-RGB .ktx -> ETC1 com mips), sem cache */
    fprintf(stderr, "texbake[etc1mip]: scale=%.2f (sem cache/JPEG)\n", g_scale);
    for (int i = 0; i < np; i++) bake_etc1mip(paks[i]);
    return 0;
  }
  if (leancache) { /* LEANPAK: repack enxuto (placeholder p/ coberto + drop .ktx) */
    if (cov_load(leancache)) return 1;
    for (int i = 0; i < np; i++) bake_leanpak(paks[i]);
    free(g_cov);
    return 0;
  }
  fprintf(stderr, "texbake: scale=%.2f mode=%s\n", g_scale, g_mode == M_RGBA8 ? "RGBA8" : "ETC1/4444");
  for (int i = 0; i < np; i++) bake_one(paks[i]);
  return 0;
}
