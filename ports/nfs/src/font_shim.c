/*
 * font_shim.c — rasterização de texto p/ NFS (substitui o caminho Android
 * BitmapGraphics/Paint/Canvas, que no Android desenha glifos via java.graphics).
 *
 * A engine cria um Paint (createPaintFromFile com .ttf REAL do jogo, ex
 * published/fonts/gothambook.ttf), mede texto (measureText/getFontMetricsInt) e
 * desenha strings num Bitmap via BitmapGraphics.drawString; depois lê os pixels
 * (AndroidBitmap_lockPixels) e sobe como textura GL. Aqui rasterizamos com
 * stb_truetype a fonte real → texto aparece de verdade.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

typedef struct {
  int used;
  unsigned char *ttf;
  stbtt_fontinfo info;
  float size, scale;
  int ascent, descent, linegap; /* em pixels (já escalados) */
} FontPaint;

#define MAXP 256
static FontPaint g_paints[MAXP];
static FontPaint g_dummy; /* sentinela used=0 p/ falhas (engine não pode receber NULL) */

static FontPaint *alloc_paint(void) {
  for (int i = 0; i < MAXP; i++) if (!g_paints[i].used) return &g_paints[i];
  return &g_paints[0];
}

static unsigned char *read_file(const char *path, long *szout) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n <= 0) { fclose(f); return NULL; }
  unsigned char *b = (unsigned char *)malloc(n);
  if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
  fclose(f);
  if (szout) *szout = n;
  return b;
}

static void recompute_metrics(FontPaint *p) {
  p->scale = stbtt_ScaleForPixelHeight(&p->info, p->size);
  int a, d, l; stbtt_GetFontVMetrics(&p->info, &a, &d, &l);
  p->ascent = (int)roundf(a * p->scale);
  p->descent = (int)roundf(d * p->scale);   /* negativo (abaixo da baseline) */
  p->linegap = (int)roundf(l * p->scale);
}

void *font_create_from_file(const char *path, float size) {
  if (!path || size <= 0) return NULL;
  long n = 0; unsigned char *ttf = read_file(path, &n);
  if (!ttf) { fprintf(stderr, "[font] FALHOU abrir %s\n", path); return &g_dummy; }
  FontPaint *p = alloc_paint();
  if (p->ttf) free(p->ttf);
  memset(p, 0, sizeof *p);
  if (!stbtt_InitFont(&p->info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
    fprintf(stderr, "[font] InitFont falhou %s\n", path); free(ttf); return &g_dummy;
  }
  p->ttf = ttf; p->size = size; p->used = 1;
  recompute_metrics(p);
  if (getenv("NFS_FONTLOG"))
    fprintf(stderr, "[font] file %s size=%.1f asc=%d desc=%d -> %p\n", path, size, p->ascent, p->descent, (void *)p);
  return p;
}

/* família → arquivo .ttf (a engine tb usa nomes de família p/ fontes padrão).
 * base = dir das fontes (.../files/published/fonts). Mapeia por heurística. */
void *font_create_from_family(const char *fontsdir, const char *family, float size) {
  char path[1024];
  const char *file = "gothambook.ttf";
  if (family) {
    if (strstr(family, "lack") || strstr(family, "Black")) file = "gothamblack.ttf";
    else if (strstr(family, "old") || strstr(family, "Bold")) file = "gothambold.ttf";
    else if (strstr(family, "mono") || strstr(family, "Mono")) file = "gothambook_mono.ttf";
    else if (strstr(family, "umber") || strstr(family, "digit")) file = "hawaiidigitalnumbers.ttf";
  }
  snprintf(path, sizeof path, "%s/%s", fontsdir ? fontsdir : ".", file);
  if (getenv("NFS_FONTLOG"))
    fprintf(stderr, "[font] family '%s' -> %s\n", family ? family : "(null)", path);
  return font_create_from_file(path, size);
}

void font_set_size(void *paintv, float size) {
  FontPaint *p = (FontPaint *)paintv;
  if (!p || !p->used || size <= 0) return;
  p->size = size; recompute_metrics(p);
}

float font_get_size(void *paintv) {
  FontPaint *p = (FontPaint *)paintv;
  return (p && p->used) ? p->size : 0.0f;
}

/* largura do texto em pixels */
float font_measure(void *paintv, const char *str) {
  FontPaint *p = (FontPaint *)paintv;
  if (!p || !p->used || !str) return 0.0f;
  float w = 0;
  for (const unsigned char *s = (const unsigned char *)str; *s;) {
    int cp = *s++;
    int aw, lsb; stbtt_GetCodepointHMetrics(&p->info, cp, &aw, &lsb);
    w += aw * p->scale;
    if (*s) w += stbtt_GetCodepointKernAdvance(&p->info, cp, *s) * p->scale;
  }
  return w;
}

/* FontMetricsInt do Android: ascent NEGATIVO (acima da baseline), descent
 * positivo. top<=ascent, bottom>=descent. */
void font_metrics(void *paintv, int *ascent, int *descent, int *top, int *bottom, int *leading) {
  FontPaint *p = (FontPaint *)paintv;
  int asc = (p && p->used) ? p->ascent : 0;     /* >0 */
  int desc = (p && p->used) ? p->descent : 0;   /* <0 */
  int lg = (p && p->used) ? p->linegap : 0;
  if (ascent) *ascent = -asc;     /* negativo */
  if (descent) *descent = -desc;  /* positivo */
  if (top) *top = -asc - 1;
  if (bottom) *bottom = -desc + 1;
  if (leading) *leading = lg;
}

/* rasteriza str no buffer RGBA (branco + alpha=cobertura) com baseline em y. */
void font_draw(void *paintv, const char *str, int x, int y,
               unsigned char *buf, int bw, int bh, int stride) {
  FontPaint *p = (FontPaint *)paintv;
  if (!p || !p->used || !buf || !str) return;
  float xpos = (float)x;
  for (const unsigned char *s = (const unsigned char *)str; *s;) {
    int cp = *s++;
    int aw, lsb; stbtt_GetCodepointHMetrics(&p->info, cp, &aw, &lsb);
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&p->info, cp, p->scale, p->scale, &x0, &y0, &x1, &y1);
    int gw = x1 - x0, gh = y1 - y0;
    if (gw > 0 && gh > 0) {
      unsigned char *gb = (unsigned char *)malloc((size_t)gw * gh);
      if (gb) {
        stbtt_MakeCodepointBitmap(&p->info, gb, gw, gh, gw, p->scale, p->scale, cp);
        int ox = (int)floorf(xpos) + x0;
        int oy = y + y0;             /* y = baseline; y0 negativo (topo do glifo) */
        for (int gy = 0; gy < gh; gy++) {
          int py = oy + gy; if (py < 0 || py >= bh) continue;
          for (int gx = 0; gx < gw; gx++) {
            int px = ox + gx; if (px < 0 || px >= bw) continue;
            unsigned char cov = gb[gy * gw + gx];
            if (!cov) continue;
            unsigned char *d = buf + (size_t)py * stride + (size_t)px * 4;
            if (cov > d[3]) { d[0] = 255; d[1] = 255; d[2] = 255; d[3] = cov; }
          }
        }
        free(gb);
      }
    }
    xpos += aw * p->scale;
    if (*s) xpos += stbtt_GetCodepointKernAdvance(&p->info, cp, *s) * p->scale;
  }
}
