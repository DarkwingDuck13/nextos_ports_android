/* etc2_halve.c -- re-processa o cache ETC2 cheio -> META-RESOLUCAO (jogo inteiro a 1/2).
 * Decodifica cada <base>_WxH.etc2, faz box-downscale 2x p/ RGBA, re-encoda ETC2_RGBA8_EAC
 * e grava em <out>/<base>_(W/2)x(H/2).etc2. Texturas pequenas (<64 ou nao mult-de-8) sao
 * COPIADAS tal qual (nao halva, mas NAO esconde nada -> o cache half fica COMPLETO).
 * Modo BULLY_HALVEBAKE (roda 1x no device, resumivel via arquivos ja existentes).
 * Objetivo: 4x menos GPU/RAM nas texturas grandes -> caber no R36S 1GB sem OOM no load. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdint.h>
#include "eac_encode.h"

extern unsigned char *etc2_decode_rgba(unsigned fmt, int w, int h, const void *data, int size);

/* REGRA de reducao (DEVE ser identica no runtime, que chama bully_halfdim em imports.c):
 * CAP por dimensao maxima = BULLY_TEX_MAXDIM (default 256). Reduz por FATOR potencia-de-2 ate
 * caber no cap (1024->256 fator4, 512->256 fator2, <=256 fica). Resultado mult de 4 (ETC2-valido).
 * Texturas que ja cabem (<=maxdim) sao COPIADAS tal qual (nada escondido). */
int bully_halfdim(int w, int h, int *hw, int *hh) {
  /* MEIA-RES (mais leve, sobreviveu ao load): halva tudo >=64 mult-de-8; depois CAPa em
   * BULLY_TEX_MAXDIM (se setado, default sem cap) p/ apertar mais ainda as grandes. */
  static int maxd = -1;
  if (maxd < 0) { const char *e = getenv("BULLY_TEX_MAXDIM"); maxd = e ? atoi(e) : 100000; if (maxd < 16) maxd = 100000; }
  int cw = w, ch = h;
  if (w >= 64 && h >= 64 && (w % 8) == 0 && (h % 8) == 0) { cw = w / 2; ch = h / 2; }  /* halve */
  while ((cw > maxd || ch > maxd) && (cw & 1) == 0 && (ch & 1) == 0 && cw >= 16 && ch >= 16) { cw /= 2; ch /= 2; } /* cap extra */
  if ((cw == w && ch == h) || (cw & 3) || (ch & 3) || cw < 8 || ch < 8) { *hw = w; *hh = h; return 0; }
  *hw = cw; *hh = ch; return 1;
}

/* box-downscale por fator F (potencia de 2): media de blocos FxF. */
static void boxf(const unsigned char *s, int sw, unsigned char *d, int dw, int dh, int f) {
  int n = f * f;
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      int a[4] = {0, 0, 0, 0};
      for (int yy = 0; yy < f; yy++)
        for (int xx = 0; xx < f; xx++) {
          const unsigned char *p = s + (((size_t)(y * f + yy) * sw) + (x * f + xx)) * 4;
          a[0] += p[0]; a[1] += p[1]; a[2] += p[2]; a[3] += p[3];
        }
      d[(y * dw + x) * 4 + 0] = (unsigned char)(a[0] / n);
      d[(y * dw + x) * 4 + 1] = (unsigned char)(a[1] / n);
      d[(y * dw + x) * 4 + 2] = (unsigned char)(a[2] / n);
      d[(y * dw + x) * 4 + 3] = (unsigned char)(a[3] / n);
    }
}

void bully_halve_cache(void) {
  const char *in  = getenv("BULLY_ETC2CACHE_SRC");   /* cache cheio (fonte) */
  const char *out = getenv("BULLY_ETC2CACHE");       /* cache half (destino) */
  if (!in || !out) { fprintf(stderr, "[halve] faltou BULLY_ETC2CACHE_SRC/BULLY_ETC2CACHE\n"); return; }
  mkdir(out, 0777);
  DIR *d = opendir(in);
  if (!d) { fprintf(stderr, "[halve] opendir %s falhou\n", in); return; }
  struct dirent *e;
  long done = 0, halved = 0, copied = 0, skipped = 0;
  while ((e = readdir(d))) {
    const char *nm = e->d_name;
    size_t L = strlen(nm);
    if (L < 8 || strcmp(nm + L - 5, ".etc2")) continue;
    char stem[480]; if (L - 5 >= sizeof stem) continue;
    memcpy(stem, nm, L - 5); stem[L - 5] = 0;             /* "<base>_WxH" */
    char *us = strrchr(stem, '_'); if (!us) continue;
    int W = 0, H = 0; if (sscanf(us + 1, "%dx%d", &W, &H) != 2) continue;
    if (W < 4 || H < 4 || (W & 3) || (H & 3) || W > 4096 || H > 4096) continue;
    *us = 0;                                              /* stem = <base> */
    int hw, hh; int do_half = bully_halfdim(W, H, &hw, &hh);
    char outp[600]; snprintf(outp, sizeof outp, "%s/%s_%dx%d.etc2", out, stem, hw, hh);
    struct stat st; if (stat(outp, &st) == 0) { done++; continue; }  /* ja feito (resume) */
    char inp[600]; snprintf(inp, sizeof inp, "%s/%s", in, nm);
    FILE *f = fopen(inp, "rb"); if (!f) { skipped++; continue; }
    size_t insz = (size_t)(W / 4) * (H / 4) * 16;
    unsigned char *buf = malloc(insz);
    if (!buf || fread(buf, 1, insz, f) != insz) { free(buf); fclose(f); skipped++; continue; }
    fclose(f);
    if (!do_half) {                                       /* pequena: copia tal qual */
      FILE *o = fopen(outp, "wb"); if (o) { fwrite(buf, 1, insz, o); fclose(o); copied++; }
      free(buf);
    } else {
      unsigned char *rgba = etc2_decode_rgba(0x9278, W, H, buf, (int)insz);
      free(buf);
      if (!rgba) { skipped++; continue; }
      unsigned char *small = malloc((size_t)hw * hh * 4);
      if (small) {
        boxf(rgba, W, small, hw, hh, W / hw);
        size_t osz = (size_t)(hw / 4) * (hh / 4) * 16;
        unsigned char *enc = malloc(osz);
        if (enc) {
          eac_encode_image_rgba(small, hw, hh, 4, enc);
          FILE *o = fopen(outp, "wb"); if (o) { fwrite(enc, 1, osz, o); fclose(o); halved++; }
          free(enc);
        }
        free(small);
      }
      free(rgba);
    }
    done++;
    if ((done % 500) == 0) fprintf(stderr, "[halve] %ld (halved=%ld copied=%ld skip=%ld)\n", done, halved, copied, skipped);
  }
  closedir(d);
  char donemark[600]; snprintf(donemark, sizeof donemark, "%s/.halve_done", out);
  FILE *m = fopen(donemark, "w"); if (m) { fprintf(m, "%ld\n", done); fclose(m); }
  fprintf(stderr, "[halve] CONCLUIDO total=%ld halved=%ld copied=%ld skip=%ld\n", done, halved, copied, skipped);
}
