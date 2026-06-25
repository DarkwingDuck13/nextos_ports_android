/* fmv.c -- decode de FMV: container IVF (VP8) via libvpx -> RGBA.
 * Os .webm do FF7 (VP8+Vorbis) sao pre-extraidos p/ .ivf (video VP8 puro) e
 * ficam em <datapath>/movies_ivf/<nome>.ivf. Aqui so' parseamos IVF (trivial) e
 * decodificamos com libvpx; a conversao YUV(I420)->RGBA e' na CPU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>

#include "fmv.h"
#include "util.h"
#include "opensles_shim.h"

typedef struct {
  FILE *f;
  int w, h;
  uint32_t num_frames;
  vpx_codec_ctx_t codec;
  int codec_ok;
  uint8_t *rgba;          /* w*h*4 */
  uint8_t *framebuf;
  size_t framebuf_cap;
  int eof;
  int cur_frame;
  FILE *af;               /* .pcm (audio s16 44100 stereo) */
  uint32_t audio_per_frame; /* bytes de audio por frame de video */
  uint8_t *abuf;
} FMV;

static FMV g;
static char g_pending_ivf[1024];   /* path do .ivf do filme atual (lazy open) */
static char g_open_ivf[1024];      /* path do .ivf ja' aberto */

static int u16le(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t u32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void fmv_reset(void) {
  if (g.codec_ok) { vpx_codec_destroy(&g.codec); g.codec_ok = 0; }
  if (g.f) { fclose(g.f); g.f = NULL; }
  if (g.af) { fclose(g.af); g.af = NULL; ff7_fmv_audio_stop(); }
  g.audio_per_frame = 0;
  g.eof = 0; g.cur_frame = 0; g.w = g.h = 0;
  g_open_ivf[0] = 0;
}

/* Deriva <datapath>/movies_ivf/<base>.ivf a partir do path do .webm aberto. */
void fmv_set_movie_from_webm(const char *webm_path) {
  if (!webm_path) return;
  const char *base = strrchr(webm_path, '/');
  base = base ? base + 1 : webm_path;
  /* corta o diretorio ".../movies/<base>.webm" -> precisamos do dir pai do movies */
  /* monta: <dir-do-webm>/../movies_ivf/<base sem .webm>.ivf, mas e' mais simples
   * trocar "/movies/" por "/movies_ivf/" e a extensao .webm->.ivf. */
  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s", webm_path);
  char *m = strstr(tmp, "/movies/");
  char out[1024];
  /* base sem extensao */
  char stem[256];
  snprintf(stem, sizeof(stem), "%s", base);
  char *dot = strrchr(stem, '.');
  if (dot) *dot = 0;
  if (m) {
    *m = 0; /* tmp = prefixo ate antes de /movies/ */
    /* preserva sufixo de lang (ex .../lang-en/movies/) -> tmp ja' tem ate /lang-en */
    snprintf(out, sizeof(out), "%s/movies_ivf/%s.ivf", tmp, stem);
  } else {
    /* fallback: mesmo dir, .ivf */
    snprintf(out, sizeof(out), "%s", webm_path);
    char *d2 = strrchr(out, '.');
    if (d2) snprintf(d2, sizeof(out) - (d2 - out), ".ivf");
  }
  snprintf(g_pending_ivf, sizeof(g_pending_ivf), "%s", out);
  debugPrintf("FMV: pending movie %s -> %s\n", webm_path, g_pending_ivf);
}

/* AVI_open(name) do engine entrega o nome do filme (ex "opening" / "opening.avi").
 * Monta <FF7_DATA>/ff7_1.02/data/[lang-en/]movies_ivf/<stem>.ivf e forca reabrir. */
void fmv_set_movie_by_name(const char *name) {
  if (!name) return;
  const char *base = strrchr(name, '/'); base = base ? base + 1 : name;
  char stem[256]; snprintf(stem, sizeof stem, "%s", base);
  char *dot = strrchr(stem, '.'); if (dot) *dot = 0;
  const char *dp = getenv("FF7_DATA"); if (!dp) dp = "/roms/ports/ff7/gamedata";
  char out[1024];
  snprintf(out, sizeof out, "%s/ff7_1.02/data/movies_ivf/%s.ivf", dp, stem);
  FILE *t = fopen(out, "rb");
  if (t) { fclose(t); }
  else { snprintf(out, sizeof out, "%s/ff7_1.02/data/lang-en/movies_ivf/%s.ivf", dp, stem); }
  snprintf(g_pending_ivf, sizeof g_pending_ivf, "%s", out);
  g_open_ivf[0] = 0; /* forca reabrir do frame 0 (mesmo se for o mesmo filme) */
  debugPrintf("FMV: AVI_open '%s' -> %s\n", name, g_pending_ivf);
  /* abre JA' (le o header) p/ o MyDecoder saber total/dims antes do 1o render */
  extern int fmv_ensure_open(void);
  fmv_ensure_open();
}

/* accessors p/ o MyDecoder (jni_shim) simular um filme TOCANDO */
int fmv_total_frames(void);
int fmv_cur_frame(void);
int fmv_eof(void);

static int fmv_open_path(const char *path) {
  fmv_reset();
  FILE *f = fopen(path, "rb");
  if (!f) { debugPrintf("FMV: nao abriu %s\n", path); return -1; }
  uint8_t hdr[32];
  if (fread(hdr, 1, 32, f) != 32 || memcmp(hdr, "DKIF", 4) != 0) {
    debugPrintf("FMV: %s nao e' IVF\n", path); fclose(f); return -1;
  }
  g.f = f;
  g.w = u16le(hdr + 12);
  g.h = u16le(hdr + 14);
  g.num_frames = u32le(hdr + 24);
  vpx_codec_dec_cfg_t cfg; memset(&cfg, 0, sizeof(cfg));
  cfg.threads = 2; cfg.w = g.w; cfg.h = g.h;
  if (vpx_codec_dec_init(&g.codec, vpx_codec_vp8_dx(), &cfg, 0) != VPX_CODEC_OK) {
    debugPrintf("FMV: codec init falhou\n"); fclose(f); g.f = NULL; return -1;
  }
  g.codec_ok = 1;
  g.rgba = realloc(g.rgba, (size_t)g.w * g.h * 4);
  g.eof = 0; g.cur_frame = 0;
  snprintf(g_open_ivf, sizeof(g_open_ivf), "%s", path);
  debugPrintf("FMV: aberto %s %dx%d frames=%u\n", path, g.w, g.h, g.num_frames);
  /* audio: <mesmo path>.pcm (s16 44100 stereo). bytes/frame = tamanho/num_frames. */
  if (getenv("FF7_NOFMVAUDIO") == NULL && g.num_frames > 0) {
    char ap[1024]; snprintf(ap, sizeof ap, "%s", path);
    char *d = strrchr(ap, '.'); if (d) snprintf(d, sizeof(ap) - (d - ap), ".pcm");
    g.af = fopen(ap, "rb");
    if (g.af) {
      fseek(g.af, 0, SEEK_END); long sz = ftell(g.af); fseek(g.af, 0, SEEK_SET);
      uint32_t apf = (uint32_t)(sz / g.num_frames);
      apf = (apf / 4) * 4; if (apf == 0) apf = 4;
      g.audio_per_frame = apf;
      g.abuf = realloc(g.abuf, apf);
      debugPrintf("FMV: audio %s sz=%ld per_frame=%u\n", ap, sz, apf);
    }
  }
  return 0;
}

static void i420_to_rgba(const vpx_image_t *img, uint8_t *rgba) {
  int w = (int)img->d_w, h = (int)img->d_h;
  const uint8_t *Y = img->planes[0]; int ys = img->stride[0];
  const uint8_t *U = img->planes[1]; int us = img->stride[1];
  const uint8_t *V = img->planes[2]; int vs = img->stride[2];
  for (int y = 0; y < h; y++) {
    const uint8_t *yr = Y + y * ys;
    const uint8_t *ur = U + (y >> 1) * us;
    const uint8_t *vr = V + (y >> 1) * vs;
    uint8_t *o = rgba + (size_t)y * w * 4;
    for (int x = 0; x < w; x++) {
      int c = yr[x] - 16;
      int d = ur[x >> 1] - 128;
      int e = vr[x >> 1] - 128;
      int r = (298 * c + 409 * e + 128) >> 8;
      int gg = (298 * c - 100 * d - 208 * e + 128) >> 8;
      int b = (298 * c + 516 * d + 128) >> 8;
      o[0] = r < 0 ? 0 : (r > 255 ? 255 : r);
      o[1] = gg < 0 ? 0 : (gg > 255 ? 255 : gg);
      o[2] = b < 0 ? 0 : (b > 255 ? 255 : b);
      o[3] = 255;
      o += 4;
    }
  }
}

/* abre o pending se ainda nao aberto (le header) — chamado eager pelo setter e
 * por fmv_next_frame. Retorna 1 se ha' um filme aberto. */
int fmv_ensure_open(void) {
  if (g_pending_ivf[0] && strcmp(g_pending_ivf, g_open_ivf) != 0) {
    if (fmv_open_path(g_pending_ivf) != 0) { g_pending_ivf[0] = 0; return 0; }
  }
  return g.f != NULL;
}

int fmv_total_frames(void) { return (int)g.num_frames; }
int fmv_cur_frame(void) { return g.cur_frame; }
int fmv_eof(void) { return g.eof || (g.num_frames && g.cur_frame >= (int)g.num_frames); }

int fmv_next_frame(void) {
  fmv_ensure_open();
  if (!g.f || g.eof) return 0;
  uint8_t fh[12];
  if (fread(fh, 1, 12, g.f) != 12) { g.eof = 1; return 0; }
  uint32_t sz = u32le(fh);
  if (sz == 0 || sz > 8u * 1024 * 1024) { g.eof = 1; return 0; }
  if (sz > g.framebuf_cap) { g.framebuf = realloc(g.framebuf, sz); g.framebuf_cap = sz; }
  if (fread(g.framebuf, 1, sz, g.f) != sz) { g.eof = 1; return 0; }
  if (vpx_codec_decode(&g.codec, g.framebuf, sz, NULL, 0) != VPX_CODEC_OK) {
    debugPrintf("FMV: decode erro frame %d\n", g.cur_frame); return 0;
  }
  vpx_codec_iter_t it = NULL;
  vpx_image_t *img = vpx_codec_get_frame(&g.codec, &it);
  if (!img) return 0;
  i420_to_rgba(img, g.rgba);
  /* alimenta o chunk de audio correspondente a este frame de video (sync) */
  if (g.af && g.audio_per_frame && g.abuf) {
    size_t got = fread(g.abuf, 1, g.audio_per_frame, g.af);
    if (got > 0) ff7_fmv_audio_feed(g.abuf, (uint32_t)got);
  }
  g.cur_frame++;
  return 1;
}

const char *fmv_current_name(void) {
  const char *p = g_pending_ivf[0] ? g_pending_ivf : g_open_ivf;
  const char *b = strrchr(p, '/'); return b ? b + 1 : p;
}
const uint8_t *fmv_rgba(void) { return g.rgba; }
int fmv_w(void) { return g.w; }
int fmv_h(void) { return g.h; }
int fmv_has_movie(void) { return g.f != NULL || g_pending_ivf[0] != 0; }
