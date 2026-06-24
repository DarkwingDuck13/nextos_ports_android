/* imports.c (DEVICE) -- shims bionic/NDK do libGame.so como tabela DynLibFunction.
 * Igual aos shims validados no PC; aqui expostos como bully_stub_table[] p/ o
 * so_resolve do so_util AArch64 (fallback dlsym pega libc/GLES/EGL/openal/mpg123
 * do device). Ponte pthread bionic->glibc vem do pthread_bridge.c. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/syscall.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <EGL/egl.h>
#include <unistd.h>
#include <fcntl.h>

#include "so_util.h"
#include "jni_shim.h"
#include "zip_fs.h"

static int env_on(const char *name) {
  const char *v = getenv(name);
  return v && *v && strcmp(v, "0") != 0;
}
static int env_on2(const char *a, const char *b) {
  return env_on(a) || env_on(b);
}
static int path_ext_is(const char *path, const char *ext) {
  const char *dot = path ? strrchr(path, '.') : NULL;
  return dot && !strcasecmp(dot, ext);
}

/* caminho da textura atualmente aberta (chave da sidetable ETC1/diag). */
char bully_cur_tex_path[256];
int g_path_fresh = 0;
void bully_set_tex_path(const char *p) {
  if (!p) { bully_cur_tex_path[0] = '\0'; g_path_fresh = 0; return; }
  size_t n = strlen(p); if (n >= sizeof(bully_cur_tex_path)) n = sizeof(bully_cur_tex_path) - 1;
  memcpy(bully_cur_tex_path, p, n); bully_cur_tex_path[n] = '\0';
  g_path_fresh = 1;
}
static void note_texture_asset_path(const char *path) {
  if (path_ext_is(path, ".pvr") || path_ext_is(path, ".png") ||
      path_ext_is(path, ".tex") || path_ext_is(path, ".txd")) {
    bully_set_tex_path(path);
  }
}

static int lcs_asset_textlog_path(const char *path) {
  if (!path) return 0;
  return strcasestr(path, "font") || strcasestr(path, ".gxt") ||
         strcasestr(path, "text/") || strcasestr(path, "hud.txd");
}

static void lcs_asset_textlog(const char *path, const char *where, long len) {
  if (!lcs_asset_textlog_path(path)) return;
  static int logs = 0;
  int limit = 160;
  const char *e = getenv("LCS_ASSET_TEXTLOG_LIMIT");
  if (e && *e) {
    limit = atoi(e);
    if (limit < 0) limit = 0;
  }
  if (logs >= limit) return;
  fprintf(stderr, "[asset-text] open \"%s\" -> %s", path, where ? where : "?");
  if (len >= 0) fprintf(stderr, " (%ld)", len);
  fprintf(stderr, "\n");
  logs++;
}

/* ===================== CACHE ETC1 OFFLINE (bakeado, embarcado) =====================
 * O engine decodifica o .tex (formato console tiled/swizzled, fechado) e sobe RGBA/16-bit
 * via glTexImage2D -- e ele NAO tem caminho ETC1. Entao: bakeamos o ETC1 do NOSSO lado
 * (BULLY_BAKE=1 captura os pixels decodificados full-res e grava por nome do asset) e
 * embarcamos o cache no pacote (a textura e identica p/ todos -- mesmo APK 1.4.311).
 * No device do usuario: so LE ETC1 pronto e sobe GL_ETC1_RGB8_OES (4x menos VRAM ->
 * resolve o limite de MMU do Utgard) -- ZERO conversao/encode em runtime, zero engasgo.
 * So texturas OPACAS 565 (sem alpha); 4444/8888/luminance caem no caminho original. */
#include "etc1_encode.h"
#define GL_ETC1_RGB8_OES 0x8D64
static const char *bully_etc1_dir(void) {
  static const char *d = NULL; static int got = 0;
  if (!got) { d = getenv("BULLY_ETC1CACHE"); got = 1; }
  return d;
}
static int bully_etc1_bake(void) { static int m = -1; if (m < 0) m = getenv("BULLY_BAKE") ? 1 : 0; return m; }
/* BULLY_ETC1_FORCE: usar ETC1 TAMBEM no KMSDRM (devices KMSDRM <=1.2GB, ex R36S 1GB, que
 * PRECISAM da economia de VRAM). Nesses, tratamos a textura como no fbdev: MIN_FILTER=
 * LINEAR + SEM mipmap (o cache ETC1 so tem o nivel base). Setado pelo launcher por RAM. */
static int bully_etc1_force(void) { static int m = -1; if (m < 0) m = getenv("BULLY_ETC1_FORCE") ? 1 : 0; return m; }
/* BULLY_TRILINEAR: TESTE de qualidade no fbdev/Mali-450 -- DESLIGA o ETC1 e deixa o
 * trilinear+mipmaps do jogo passar (igual kmsdrm). Custa ~2.6x VRAM -> pode OOM em 1GB. */
static int bully_trilinear(void) { static int m = -1; if (m < 0) m = getenv("BULLY_TRILINEAR") ? 1 : 0; return m; }
static void bully_etc1_key(char *out, size_t n, const char *path, int w, int h) {
  char san[200]; size_t j = 0;
  for (const char *p = path; *p && j < sizeof(san) - 1; p++) san[j++] = (*p == '/' || *p == '\\') ? '_' : *p;
  san[j] = '\0';
  snprintf(out, n, "%s/%s_%dx%d.etc1", bully_etc1_dir(), san, w, h);
}
/* 565 (u16 LE) -> RGB888 */
static void bully_expand565(const unsigned char *s, int w, int h, unsigned char *rgb) {
  for (int i = 0; i < w * h; i++) {
    unsigned v = s[2*i] | (s[2*i+1] << 8);
    rgb[3*i]   = (unsigned char)((((v >> 11) & 31) * 255 + 15) / 31);
    rgb[3*i+1] = (unsigned char)((((v >> 5) & 63) * 255 + 31) / 63);
    rgb[3*i+2] = (unsigned char)(((v & 31) * 255 + 15) / 31);
  }
}
extern unsigned char *etc2_decode_rgba(unsigned, int, int, const void *, int);
/* VERIFICACAO DE CONTEUDO (anti-roxo): decodifica ~24 blocos do ETC1 do cache e compara
 * com os pixels 565 que o engine ia subir. MAD alto = o cache foi salvo no nome errado
 * (corrida no bake) -> rejeita e usa a textura original. Igual o anti-magenta do dysmantle. */
static int bully_etc1_verify(const unsigned char *blob, int w, int h, const unsigned char *px565) {
  int bw = w / 4, bh = h / 4; if (bw <= 0 || bh <= 0 || !px565) return -1;
  int gx = bw < 5 ? bw : 5, gy = bh < 5 ? bh : 5; long sum = 0, cnt = 0; int sampled = 0;
  for (int iy = 0; iy < gy; iy++) for (int ix = 0; ix < gx; ix++) {
    int bx = (int)((long)ix * (bw - 1) / (gx > 1 ? gx - 1 : 1));
    int by = (int)((long)iy * (bh - 1) / (gy > 1 ? gy - 1 : 1));
    unsigned char *dec = etc2_decode_rgba(0x9274, 4, 4, blob + ((long)by * bw + bx) * 8, 8);
    if (!dec) continue; sampled++;
    for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {
      long p = (long)(by * 4 + j) * w + (bx * 4 + i);
      unsigned v = px565[2 * p] | (px565[2 * p + 1] << 8);
      int sr = (((v >> 11) & 31) * 255 + 15) / 31, sg = (((v >> 5) & 63) * 255 + 31) / 63, sb = ((v & 31) * 255 + 15) / 31;
      const unsigned char *d = dec + (j * 4 + i) * 4;
      int dr = d[0] - sr, dg = d[1] - sg, db = d[2] - sb;
      sum += (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db); cnt += 3;
    }
    free(dec);
  }
  if (!sampled || !cnt) return -1;
  return (int)(sum / cnt);
}
static unsigned (*real_glCompressedTexImage2D2)(unsigned, int, unsigned, int, int, int, int, const void *) = NULL;
extern int bully_is_kmsdrm(void);
extern long g_tex_live;
static int lcs_tex_light_enabled(void) {
  static int v = -1;
  if (v < 0) v = env_on("LCS_TEX_LIGHT");
  return v;
}
static long lcs_tex_light_cap(void) {
  static long cap = -1;
  if (cap < 0) {
    const char *e = getenv("LCS_TEX_LIGHT_CAP");
    cap = e && *e ? atol(e) : 5600;
    if (cap < 128) cap = 128;
  }
  return cap;
}
static int lcs_tex_light_min_dim(void) {
  static int min_dim = -1;
  if (min_dim < 0) {
    const char *e = getenv("LCS_TEX_LIGHT_MIN_DIM");
    min_dim = e && *e ? atoi(e) : 0;
    if (min_dim < 0) min_dim = 0;
  }
  return min_dim;
}
static int lcs_tex_light_stub_compressed(unsigned target, int level,
                                         unsigned ifmt, int w, int h) {
  if (!lcs_tex_light_enabled()) return 0;
  if (target != 0x0DE1 || level != 0) return 0;  /* GL_TEXTURE_2D base only */
  if (ifmt != GL_ETC1_RGB8_OES) return 0;
  if (w <= 4 && h <= 4) return 0;
  int min_dim = lcs_tex_light_min_dim();
  if (min_dim > 0 && w < min_dim && h < min_dim) return 0;
  return g_tex_live >= lcs_tex_light_cap();
}
/* retorna 1 se tratou (subiu ETC1 do cache, ou pulou mip de textura ja-ETC1). */
static int bully_try_etc1(unsigned tgt, int lvl, int w, int h, unsigned fmt, unsigned type, const void *px) {
  static int cur_cached = 0;            /* base atual virou ETC1? -> pula seus mips */
  if (!bully_etc1_dir()) return 0;      /* cache desligado */
  if (bully_trilinear()) return 0;      /* TESTE trilinear: sem ETC1 (RGBA+mips) */
  if (bully_is_kmsdrm() && !bully_etc1_force()) return 0;  /* fbdev sempre; kmsdrm so com FORCE (low-RAM) */
  if (lvl > 0) return cur_cached;       /* pula mips da textura cacheada */
  cur_cached = 0;
  if (type != 0x8363 || fmt != 0x1907) return 0;          /* SO 565 opaco (RGB) */
  if (w < 64 || h < 64 || w > 2048 || h > 2048 || (w & 3) || (h & 3)) return 0;
  if (!bully_cur_tex_path[0]) return 0;
  char key[300]; bully_etc1_key(key, sizeof(key), bully_cur_tex_path, w, h);
  size_t etcsz = (size_t)(w / 4) * (h / 4) * 8;
  if (!real_glCompressedTexImage2D2) real_glCompressedTexImage2D2 = dlsym(RTLD_DEFAULT, "glCompressedTexImage2D");
  FILE *f = fopen(key, "rb");
  if (f) {                              /* RUNTIME: sobe o ETC1 pronto */
    unsigned char *buf = malloc(etcsz);
    size_t r = buf ? fread(buf, 1, etcsz, f) : 0; fclose(f);
    if (buf && r == etcsz && real_glCompressedTexImage2D2) {
      /* anti-roxo: so sobe o ETC1 se ele BATE com a textura que o engine ia subir. */
      static int vmax = -1; if (vmax < 0) { const char *e = getenv("BULLY_VERIFY_MAX"); vmax = e ? atoi(e) : 34; }
      int mad = (px && type == 0x8363) ? bully_etc1_verify(buf, w, h, (const unsigned char *)px) : 0;
      if (mad > vmax) {  /* cache no nome errado (corrida do bake) -> usa a original */
        static int rj = 0; if (rj < 12) { fprintf(stderr, "[etc1] REJEITA '%s' %dx%d mad=%d (usa original)\n", bully_cur_tex_path, w, h, mad); rj++; }
        free(buf); return 0;
      }
      real_glCompressedTexImage2D2(tgt, 0, GL_ETC1_RGB8_OES, w, h, 0, (int)etcsz, buf);
      free(buf); cur_cached = 1;
      static int up = 0; if (up < 8) { fprintf(stderr, "[etc1] up '%s' %dx%d\n", bully_cur_tex_path, w, h); up++; }
      return 1;
    }
    free(buf); return 0;
  }
  if (bully_etc1_bake() && px) {        /* BAKE (nosso lado): encoda full-res e grava */
    unsigned char *rgb = malloc((size_t)w * h * 3);
    unsigned char *etc = malloc(etcsz);
    if (rgb && etc) {
      bully_expand565(px, w, h, rgb);
      etc1_encode_image(rgb, w, h, 3, etc);
      FILE *wf = fopen(key, "wb");
      if (wf) { fwrite(etc, 1, etcsz, wf); fclose(wf); static int bk = 0; if (bk < 12) { fprintf(stderr, "[bake] '%s' %dx%d -> %zuB\n", bully_cur_tex_path, w, h, etcsz); bk++; } }
    }
    free(rgb); free(etc);
  }
  return 0;
}

/* ===================== PAGINACAO DE TEXTURA (resident-set / VM de textura) =====================
 * O motor do Bully NUNCA despeja textura (provado: gen=1386 del=1). Na UMA do Mali, textura GL =
 * RAM. Entao implementamos o despejo NOS: cada textura ETC1-cacheada vira "pageavel". A FONTE fica
 * no SD (etc1cache/). Acima do orcamento, despejamos a mais FRIA (re-define 1x1 -> libera a RAM, o
 * id continua valido). Quando o motor RE-BINDA uma despejada (page fault), re-subimos do cache.
 * Gate: BULLY_PAGE=1 ; orcamento: BULLY_PAGE_CAP_MB (default 220). Diag: BULLY_PAGELOG=1. */
extern long long g_texbytes_live;
unsigned bully_g_texbytes(unsigned id);            /* fwd (def. junto do array g_texbytes) */
void     bully_g_texbytes_set(unsigned id, unsigned b);
static char         *g_page_name[262144];   /* strdup do asset (NULL = nao pageavel) ~2MB */
static unsigned short g_page_w[262144], g_page_h[262144];
static unsigned char  g_page_present[262144];/* 1 = textura cheia (ETC1) carregada; 0 = despejada (1x1) */
static unsigned       g_page_use[262144];   /* relogio LRU */
static unsigned       g_page_clock = 0;
static long long      g_page_resident = 0;  /* bytes ETC1 PRESENTES (pageaveis) */
static unsigned       g_page_list[40000]; static int g_page_n = 0; /* lista compacta de ids pageaveis */
static long g_pf = 0, g_ev = 0;              /* contadores: page-faults / evicts */
#define PAGE_ETCSZ(id) ((size_t)(g_page_w[id]/4) * (g_page_h[id]/4) * 8)
static int bully_paging(void){ static int m=-1; if(m<0)m=getenv("BULLY_PAGE")?1:0; return m; }
static long long bully_page_cap(void){ static long long c=-1; if(c<0){ const char*e=getenv("BULLY_PAGE_CAP_MB"); c=(long long)(e?atoll(e):220)*1024*1024; } return c; }
extern unsigned bully_g_texbytes(unsigned id);          /* getter (def. junto do array) */
extern void     bully_g_texbytes_set(unsigned id, unsigned b);
/* registra uma textura ETC1 recem-subida como pageavel (chamado de dentro do bully_try_etc1) */
static void bully_page_register(unsigned id, const char *name, int w, int h, unsigned etcsz) {
  if (!bully_paging() || id >= 262144) return;
  if (!g_page_name[id]) { g_page_name[id] = strdup(name); if (g_page_n < 40000) g_page_list[g_page_n++] = id; }
  g_page_w[id] = (unsigned short)w; g_page_h[id] = (unsigned short)h; g_page_use[id] = ++g_page_clock;
  /* o my_glTexImage2D ja tinha setado g_texbytes[id] com a estimativa RGBA -> corrige p/ o ETC1 real */
  g_texbytes_live += (long long)etcsz - bully_g_texbytes(id);
  bully_g_texbytes_set(id, etcsz);
  if (!g_page_present[id]) { g_page_present[id] = 1; g_page_resident += etcsz; }  /* presente */
}
/* despeja as mais FRIAS ate voltar ao orcamento. 'keep'/'target' = a textura sendo usada agora
 * (nao despejar; re-bindar no fim). Re-define 1x1 -> libera a RAM da textura grande. */
static void bully_page_evict(unsigned target, unsigned keep) {
  static void (*rTexImg)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*) = NULL;
  static void (*rBind)(unsigned,unsigned) = NULL;
  if (!rTexImg) rTexImg = dlsym(RTLD_DEFAULT, "glTexImage2D");
  if (!rBind)   rBind   = dlsym(RTLD_DEFAULT, "glBindTexture");
  if (!rTexImg || !rBind) return;
  static const unsigned char black[4] = {0,0,0,0};
  int guard = 0;
  while (g_page_resident > bully_page_cap() && guard++ < 4096) {
    unsigned best = 0; unsigned bu = 0xffffffffu; int fi = -1;
    for (int i = 0; i < g_page_n; i++) { unsigned id = g_page_list[i];
      if (id == keep || bully_g_texbytes(id) == 0) continue;      /* pula a atual e as ja-despejadas */
      if (g_page_use[id] < bu) { bu = g_page_use[id]; best = id; fi = i; } }
    if (fi < 0) break;                                            /* nada pra despejar */
    rBind(0x0DE1, best);
    rTexImg(0x0DE1, 0, 0x1907, 1, 1, 0, 0x1907, 0x1401, black);   /* RGB 1x1 -> libera a grande */
    long long etc = (long long)PAGE_ETCSZ(best);
    g_texbytes_live += 3 - bully_g_texbytes(best); bully_g_texbytes_set(best, 3); /* agora ocupa ~3 bytes */
    g_page_resident -= etc; g_page_present[best] = 0; g_ev++;     /* despejada */
  }
  rBind(target, keep);                                            /* restaura o bind do motor */
}
/* page fault: a textura 'id' (BINDADA agora) foi despejada -> re-sobe o ETC1 do cache (SD). */
static void bully_page_fault(unsigned target, unsigned id) {
  char key[300]; bully_etc1_key(key, sizeof key, g_page_name[id], g_page_w[id], g_page_h[id]);
  int w = g_page_w[id], h = g_page_h[id]; size_t sz = (size_t)(w/4) * (h/4) * 8;
  FILE *f = fopen(key, "rb"); if (!f) return;
  unsigned char *buf = malloc(sz); size_t r = buf ? fread(buf, 1, sz, f) : 0; fclose(f);
  static unsigned (*rCompr)(unsigned,int,unsigned,int,int,int,int,const void*) = NULL;
  static void (*rParam)(unsigned,unsigned,int) = NULL;
  if (!rCompr) rCompr = dlsym(RTLD_DEFAULT, "glCompressedTexImage2D");
  if (!rParam) rParam = dlsym(RTLD_DEFAULT, "glTexParameteri");
  if (buf && r == sz && rCompr) {
    rCompr(target, 0, 0x8D64 /*ETC1*/, w, h, 0, (int)sz, buf);
    if (rParam) { rParam(target, 0x2801, 0x2601); rParam(target, 0x2800, 0x2601); } /* LINEAR (sem mip) */
    g_texbytes_live += (long long)sz - bully_g_texbytes(id);     /* estava em ~3 bytes (despejada) */
    bully_g_texbytes_set(id, (unsigned)sz);
    g_page_resident += sz; g_page_present[id] = 1; g_pf++;       /* presente de novo */
  }
  free(buf);
}
/* chamado do my_glBindTexture: toca o LRU, atende page fault, e despeja se acima do orcamento. */
void bully_page_on_bind(unsigned target, unsigned id) {
  if (!bully_paging() || target != 0x0DE1 || id >= 262144 || !g_page_name[id]) return;
  g_page_use[id] = ++g_page_clock;
  if (!g_page_present[id]) bully_page_fault(target, id);            /* re-sobe do SD */
  if (g_page_resident > bully_page_cap()) bully_page_evict(target, id);
  if (getenv("BULLY_PAGELOG")) { static long c=0; if ((c++ % 600)==0)
    fprintf(stderr, "[page] resident=%lldMB cap=%lldMB pf=%ld ev=%ld pageaveis=%d\n",
            g_page_resident/(1024*1024), bully_page_cap()/(1024*1024), g_pf, g_ev, g_page_n); }
}

/* ---- bionic libc bridges ---- */
static int *bionic___errno(void) { extern int *__errno_location(void); return __errno_location(); }
static size_t b_strlen_chk(const char *s, size_t n) { (void)n; return strlen(s); }
static char *b_strrchr_chk(const char *s, int c, size_t n) { (void)n; return strrchr(s, c); }
static char *b_strchr_chk(const char *s, int c, size_t n) { (void)n; return strchr(s, c); }
static char *b_strncpy_chk2(char *d, const char *s, size_t n, size_t dn, size_t sn) { (void)dn; (void)sn; return strncpy(d, s, n); }
static void b_assert2(const char *f, int l, const char *fn, const char *e) {
  fprintf(stderr, "assert: %s:%d %s: %s\n", f, l, fn, e); abort();
}
static int b_android_log(int prio, const char *tag, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[ALOG:%d %s] ", prio, tag ? tag : "?");
  vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap);
  return 0;
}
/* LCS importa estas variantes (Bully nao usava): */
static int b_android_log_write(int prio, const char *tag, const char *msg) {
  fprintf(stderr, "[ALOG:%d %s] %s\n", prio, tag ? tag : "?", msg ? msg : ""); return 0;
}
static int b_android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  fprintf(stderr, "[ALOG:%d %s] ", prio, tag ? tag : "?");
  vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); return 0;
}
static void b_android_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[ALOG-ASSERT %s] cond=%s ", tag ? tag : "?", cond ? cond : "?");
  if (fmt) vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n"); va_end(ap); abort();
}
/* __FD_SET_chk / __FD_ISSET_chk (bionic fortify do select) */
static void b_fd_set_chk(int fd, fd_set *s, size_t sz) { (void)sz; if (s) FD_SET(fd, s); }
static int  b_fd_isset_chk(int fd, fd_set *s, size_t sz) { (void)sz; return s ? FD_ISSET(fd, s) : 0; }

/* bionic __sF[3] = stdin/out/err. Wrappers traduzem p/ stream real. */
static char bionic_sF[3][512];
static FILE *map_sF(void *fp) {
  if (fp == (void *)&bionic_sF[0]) return stdin;
  if (fp == (void *)&bionic_sF[1]) return stdout;
  if (fp == (void *)&bionic_sF[2]) return stderr;
  return (FILE *)fp;
}
static int w_fprintf(void *fp, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); int r = vfprintf(map_sF(fp), fmt, ap); va_end(ap); return r;
}
static int w_vfprintf(void *fp, const char *fmt, va_list ap) { return vfprintf(map_sF(fp), fmt, ap); }
static size_t w_fwrite(const void *p, size_t s, size_t n, void *fp) { return fwrite(p, s, n, map_sF(fp)); }
static int w_fputs(const char *str, void *fp) { return fputs(str, map_sF(fp)); }
static int w_fputc(int c, void *fp) { return fputc(c, map_sF(fp)); }
static int w_fflush(void *fp) { return fflush(fp ? map_sF(fp) : NULL); }

/* _ctype_ legado (BSD): tabela 1+256, indexada de -1. */
static unsigned char ctype_tab[1 + 256];
static unsigned char *bionic_ctype_ptr = ctype_tab + 1;
#define _CT_U 0x01
#define _CT_L 0x02
#define _CT_N 0x04
#define _CT_S 0x08
#define _CT_P 0x10
#define _CT_C 0x20
#define _CT_X 0x40
#define _CT_B 0x80
static void ctype_init(void) {
  for (int c = 0; c < 256; c++) {
    unsigned char f = 0;
    if (isupper(c)) f |= _CT_U; if (islower(c)) f |= _CT_L;
    if (isdigit(c)) f |= _CT_N; if (isspace(c)) f |= _CT_S;
    if (ispunct(c)) f |= _CT_P; if (iscntrl(c)) f |= _CT_C;
    if (isxdigit(c)) f |= _CT_X; if (c == ' ') f |= _CT_B;
    ctype_tab[1 + c] = f;
  }
}

/* ---- NDK ANativeWindow (a janela é do egl_shim Mali fbdev) ---- */
static void *aw_fromSurface(void *env, void *surface) { (void)env; (void)surface; return (void *)0xAA11; }
static int aw_setBuffersGeometry(void *w, int x, int y, int f) { (void)w;(void)x;(void)y;(void)f; return 0; }
extern int bully_screen_w(void); extern int bully_screen_h(void);
static int aw_getWidth(void *w) { (void)w; return bully_screen_w(); }
static int aw_getHeight(void *w) { (void)w; return bully_screen_h(); }
static void aw_release(void *w) { (void)w; }

/* ---- NDK AAssetManager / AAsset (lê dos arquivos reais) ---- */
#ifndef ASSET_DIR
#define ASSET_DIR "assets"
#endif
/* AAsset agora suporta DISCO (fp) ou MEMORIA (mem/pos) — esta ultima serve
 * arquivos lidos de DENTRO do WAD da LCS via o WadArchive da engine. */
typedef struct { FILE *fp; long len; unsigned char *mem; long pos; } AAsset;
static void *am_fromJava(void *env, void *obj) { (void)env; (void)obj; return (void *)0xA55E7; }
static const char *asset_dir(void) {
  const char *d = getenv("LCS_DATA_DIR");
  return d ? d : ASSET_DIR;
}
static FILE *asset_fopen(const char *path) {
  char full[1024];
  snprintf(full, sizeof(full), "%s/%s", asset_dir(), path);
  FILE *fp = fopen(full, "rb");
  if (!fp) {
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    if (base != path) {
      snprintf(full, sizeof(full), "%s/%s", asset_dir(), base);
      fp = fopen(full, "rb");
    }
  }
  return fp;
}

/* ---- WAD da LCS via engine (WadArchive proprio; provado: 10112 entries) ----
 * O file-open da engine cai no AAssetManager (aa_open) p/ varios assets que
 * vivem DENTRO do data_main.wad. Montamos um WadArchive nosso e servimos esses
 * arquivos por nome (FSWadFile::Read ja descomprime/decifra). */
static char g_wad_main[256], g_wad_music[256];
static int  g_wad_ready = -1;
static void *(*pWadOpenFile)(void *, const char *, int) = NULL;
static long  (*pWadGetSize)(void *, const char *) = NULL;
static long  (*pFSWadRead)(void *, void *, long) = NULL;
static void  (*pFSWadDtor)(void *) = NULL;
static void wad_lazy_init(void) {
  if (g_wad_ready >= 0) return;
  g_wad_ready = 0;
  void *(*FileOpen)(const char *, int, int) = (void *)so_find_addr("_ZN8Platform18FileOpenOSFilePathEPKc8FileModei");
  void (*WadCtor)(void *) = (void *)so_find_addr("_ZN10WadArchiveC1Ev");
  int (*OpenWad)(void *, void *, int) = (void *)so_find_addr("_ZN10WadArchive7OpenWadEP5IFileb");
  int (*GetNum)(void *) = (void *)so_find_addr("_ZNK10WadArchive16GetNumFatEntriesEv");
  pWadOpenFile = (void *)so_find_addr("_ZN10WadArchive8OpenFileEPKc8FileMode");
  pWadGetSize  = (void *)so_find_addr("_ZN10WadArchive11GetFileSizeEPKc");
  pFSWadRead   = (void *)so_find_addr("_ZN9FSWadFile4ReadEPvl");
  pFSWadDtor   = (void *)so_find_addr("_ZN9FSWadFileD1Ev");
  if (!FileOpen || !WadCtor || !OpenWad || !pWadOpenFile || !pWadGetSize || !pFSWadRead) {
    fprintf(stderr, "[wadfs] simbolos faltando (FileOpen=%p Ctor=%p OpenWad=%p OpenFile=%p GetSize=%p Read=%p)\n",
            (void*)FileOpen,(void*)WadCtor,(void*)OpenWad,(void*)pWadOpenFile,(void*)pWadGetSize,(void*)pFSWadRead);
    return;
  }
  const char *dir = asset_dir();
  char path[512];
  memset(g_wad_main, 0, sizeof(g_wad_main)); memset(g_wad_music, 0, sizeof(g_wad_music));
  snprintf(path, sizeof(path), "%s/data_main.wad", dir);
  void *f1 = FileOpen(path, 0, 0); WadCtor(g_wad_main);
  int ok1 = f1 ? OpenWad(g_wad_main, f1, 1) : 0;
  snprintf(path, sizeof(path), "%s/data_music.wad", dir);
  void *f2 = FileOpen(path, 0, 0); WadCtor(g_wad_music);
  int ok2 = f2 ? OpenWad(g_wad_music, f2, 1) : 0;
  fprintf(stderr, "[wadfs] data_main ok=%d entries=%d | data_music ok=%d entries=%d\n",
          ok1, GetNum ? GetNum(g_wad_main) : -1, ok2, GetNum ? GetNum(g_wad_music) : -1);
  g_wad_ready = 1;
}
static const char *wad_alias_name(const char *name, const char **why) {
  if (why) *why = NULL;
  if (!name) return name;
  if (!strcasecmp(name, "Data/WEAPON_MULTI.DAT") || !strcasecmp(name, "Data\\WEAPON_MULTI.DAT")) {
    if (why) *why = "asset ausente no APK";
    return "Data/WEAPON.DAT";
  }
  return name;
}

/* le um arquivo do WAD por nome -> buffer malloc (caller free). NULL se ausente. */
static unsigned char *wad_read(const char *name, long *out_len) {
  wad_lazy_init();
  if (g_wad_ready != 1) return NULL;
  const char *orig_name = name;
  const char *alias_why = NULL;
  name = wad_alias_name(name, &alias_why);
  if (alias_why && orig_name && name != orig_name) {
    static int alog = 0;
    if (alog < 32) { fprintf(stderr, "[wadfs] alias \"%s\" -> \"%s\" (%s)\n", orig_name, name, alias_why); alog++; }
  }
  for (int w = 0; w < 2; w++) {
    void *wad = w == 0 ? g_wad_main : g_wad_music;
    long sz = pWadGetSize(wad, name);
    if (sz <= 0) continue;
    void *h = pWadOpenFile(wad, name, 0);
    if (!h) continue;
    unsigned char *buf = malloc(sz + 1);            /* +1 p/ poder null-terminar (TiXml/text) */
    long got = buf ? pFSWadRead(h, buf, sz) : -1;
    if (pFSWadDtor) pFSWadDtor(h);
    if (got != sz) { fprintf(stderr, "[wadfs] read \"%s\" sz=%ld got=%ld\n", name, sz, got); }
    if (buf && got > 0) { buf[got] = 0; *out_len = got; return buf; }
    free(buf);
  }
  return NULL;
}

/* SCMDIAG: quando main.scm e (re)aberto, imprime o FP-backtrace (libGame frames)
 * p/ achar QUEM recarrega o script todo frame (loop de re-init). Gate LCS_SCMDIAG. */
static void scm_backtrace(const char *path) {
  if (!getenv("LCS_SCMDIAG") || !strstr(path, "main.scm")) return;
  extern void *text_base; extern size_t text_size;
  uintptr_t tb = (uintptr_t)text_base, ts = text_size;
  static int sc = 0; if (sc >= 30) return; sc++;
  uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
  fprintf(stderr, "[scmbt] main.scm open #%d: ", sc);
  for (int i = 0; i < 12 && fp; i++) {
    uintptr_t *p = (uintptr_t *)fp; uintptr_t nfp = p[0], lr = p[1];
    if (lr >= tb && lr < tb + ts) fprintf(stderr, "+0x%lx ", (unsigned long)(lr - tb));
    if (nfp <= fp) break; fp = nfp;
  }
  fprintf(stderr, "\n");
}
static void *aa_open(void *mgr, const char *path, int mode) {
  (void)mgr; (void)mode;
  if (!path) return NULL;
  static int log = 0;
  int assetlog = env_on2("LCS_ASSETLOG", "BULLY_ASSETLOG");
  int missinglog = assetlog || env_on("LCS_MISSINGLOG");
  int loglimit = 2000;
  const char *le = getenv("LCS_ASSETLOG_LIMIT");
  if (le && *le) { loglimit = atoi(le); if (loglimit < 0) loglimit = 0; }
  note_texture_asset_path(path);
  scm_backtrace(path);
  /* 1) disco (gamedata/) */
  FILE *fp = asset_fopen(path);
  if (fp) {
    if (assetlog && log < loglimit) { fprintf(stderr, "[asset] open \"%s\" -> DISCO\n", path); log++; }
    lcs_asset_textlog(path, "DISCO", -1);
    AAsset *a = calloc(1, sizeof(AAsset)); a->fp = fp;
    fseek(fp, 0, SEEK_END); a->len = ftell(fp); fseek(fp, 0, SEEK_SET);
    return a;
  }
  /* 2) de dentro do WAD (memoria) */
  long len = 0; unsigned char *mem = wad_read(path, &len);
  if (mem) {
    /* arquivos de TEXTO: o engine copia getLength() bytes pro buffer dele e o
     * passa pro TiXml, que precisa de terminador null -> exponho len+1 (o byte
     * extra ja e 0 em mem[len], setado no wad_read) p/ o engine alocar/copiar o
     * null. Binarios ficam com len exato. */
    /* TESTE: NAO expor +1 p/ XML. O caller do PVS faz calloc(getSize+1) e null-termina
     * sozinho; nosso +1 deixava getSize errado (388808 vs 388807 real) -> a engine lia 1
     * byte a mais (nosso null) -> suspeita de corrupcao no parse do scene XML. LCS_XMLPLUS1
     * volta o comportamento antigo (+1) se precisar. */
    long elen = len;
    const char *dot = strrchr(path, '.');
    if (dot && !strcasecmp(dot, ".xml")) {
      if (getenv("LCS_XMLPLUS1")) elen = len + 1;
      if (env_on("LCS_XMLLOG"))
        fprintf(stderr, "[xmlhdr] \"%s\" len=%ld elen=%ld primeiros: %.40s\n", path, len, elen, (char *)mem);
    }
    if (assetlog && log < loglimit) { fprintf(stderr, "[asset] open \"%s\" -> WAD (%ld%s)\n", path, len, elen > len ? "+1" : ""); log++; }
    lcs_asset_textlog(path, "WAD", len);
    AAsset *a = calloc(1, sizeof(AAsset)); a->mem = mem; a->len = elen; a->pos = 0;
    return a;
  }
  if (missinglog && log < loglimit) { fprintf(stderr, "[asset] open \"%s\" -> FALTA\n", path); log++; }
  lcs_asset_textlog(path, "FALTA", -1);
  return NULL;
}
static int aa_read(void *h, void *buf, size_t n) {
  AAsset *a = h; if (!a) return -1;
  if (a->mem) { long r = a->len - a->pos; if ((long)n < r) r = n; if (r < 0) r = 0;
    memcpy(buf, a->mem + a->pos, r); a->pos += r; return (int)r; }
  return fread(buf, 1, n, a->fp);
}
static long aa_seek64(void *h, long off, int wh) {
  AAsset *a = h; if (!a) return -1;
  if (a->mem) { long p = wh == 1 ? a->pos + off : wh == 2 ? a->len + off : off;
    if (p < 0) p = 0; if (p > a->len) p = a->len; a->pos = p; return p; }
  fseek(a->fp, off, wh); return ftell(a->fp);
}
/* AAsset_getBuffer: ponteiro DIRETO pros dados do asset. O PVS::LoadPVSZones (e outros)
 * lem o XML por aqui em vez de AAsset_read -> sem isto retornava NULL -> TiXml parseava
 * de NULL+offset -> CRASH. Retorna a->mem (WAD ja carregou tudo); disco -> carrega. */
static const void *aa_getBuffer(void *h) {
  AAsset *a = h; if (!a) return NULL;
  if (a->mem) return a->mem;
  if (a->fp && a->len > 0) {
    unsigned char *m = malloc((size_t)a->len + 1);
    if (m) { long save = ftell(a->fp); fseek(a->fp, 0, SEEK_SET);
      long got = fread(m, 1, a->len, a->fp); fseek(a->fp, save, SEEK_SET);
      if (got > 0) { m[got] = 0; a->mem = m; a->pos = 0; return m; }
      free(m); } }
  return NULL;
}
static long aa_getLength64(void *h) { AAsset *a = h; return a ? a->len : 0; }
static long aa_getRemainingLength64(void *h) { AAsset *a = h; return a ? a->len - (a->mem ? a->pos : ftell(a->fp)) : 0; }
static void aa_close(void *h) { AAsset *a = h; if (a) { if (a->mem) free(a->mem); else if (a->fp) fclose(a->fp); free(a); } }

/* ---- fopen: disco; se falhar (leitura), serve de DENTRO dos data_*.zip ---- */
static FILE *w_fopen(const char *path, const char *mode) {
  static FILE *(*real)(const char *, const char *) = NULL;
  if (!real) real = dlsym(RTLD_DEFAULT, "fopen");
  FILE *f = real ? real(path, mode) : NULL;
  /* device: os dados ficam em assets/ (vfat sem symlink); o jogo fopena
   * "data_N.zip" no cwd -> redireciona p/ "assets/data_N.zip". */
  if (!f && real && path && mode && mode[0] == 'r' && strncmp(path, "assets/", 7) != 0) {
    char alt[1024]; snprintf(alt, sizeof(alt), "assets/%s", path);
    f = real(alt, mode);
  }
  /* LCS: muitos loads (TiXml/PVS/configs) usam fopen e os arquivos estao DENTRO
   * do WAD -> serve via fmemopen sobre um buffer lido do WAD (null-terminado p/
   * o TiXml nao estourar). */
  if (!f && path && mode && mode[0] == 'r') {
    long len = 0; unsigned char *mem = wad_read(path, &len);
    if (mem) {
      unsigned char *nt = realloc(mem, len + 1); if (nt) { nt[len] = 0; mem = nt; }
      f = fmemopen(mem, len + 1, "rb"); /* +1 inclui o terminador */
      static int wl = 0; if (env_on2("LCS_ASSETLOG", "BULLY_ASSETLOG") && wl < 2000) { fprintf(stderr, "[fopen] \"%s\" -> WAD (%ld)\n", path, len); wl++; }
      /* nota: fmemopen nao libera mem; vaza por arquivo de config (poucos) */
    }
  }
  if (path && (strstr(path, "settings.ini") || strstr(path, "storage.ini")))
    fprintf(stderr, "[cfg] fopen(\"%s\",\"%s\") -> %s\n", path, mode ? mode : "?", f ? "OK" : "FALHOU");
  return f;
}

/* ---- stat/lstat/fstat/fstatat (CAUSA do texto invisivel em glibc velha) ----
 * Em glibc < 2.33 os NOMES "stat"/"lstat"/"fstat" NAO sao simbolos exportados
 * (sao macros/wrappers inline -> __xstat(_STAT_VER,...)). O so_resolve resolve
 * imports por dlsym(NOME), entao nos devices de glibc antiga -- justamente os que
 * caem no bully.compat -- esses imports do libGame/libc++ ficam UNRESOLVED. O
 * jogo entao nao consegue stat() dos arquivos (ex.: as fontes) e o texto some,
 * mesmo com o resto renderizando. Resolvemos via SYSCALL crua: o kernel preenche
 * a `struct stat` no layout arm64 == layout do bionic (libGame), sem conversao.
 * Replica o fallback "assets/" do w_fopen (dados em vfat sem symlink). */
static int g_stat_log = 0;
static int stat_at(const char *path, void *buf, int flag) {
  int r = syscall(SYS_newfstatat, AT_FDCWD, path, buf, flag);
  if (r != 0 && path && strncmp(path, "assets/", 7) != 0) {
    char alt[1024]; snprintf(alt, sizeof(alt), "assets/%s", path);
    r = syscall(SYS_newfstatat, AT_FDCWD, alt, buf, flag);
  }
  if (g_stat_log < 24) { fprintf(stderr, "[stat] \"%s\" flag=%d -> %d\n", path ? path : "(null)", flag, r); g_stat_log++; }
  return r;
}
static int my_stat(const char *path, void *buf)  { return stat_at(path, buf, 0); }
static int my_lstat(const char *path, void *buf) { return stat_at(path, buf, AT_SYMLINK_NOFOLLOW); }
static int my_fstatat(int dfd, const char *path, void *buf, int flag) {
  if (dfd == AT_FDCWD) return stat_at(path, buf, flag);
  return syscall(SYS_newfstatat, dfd, path, buf, flag);
}
static int my_fstat(int fd, void *buf) {
  int r = syscall(SYS_fstat, fd, buf);
  if (g_stat_log < 24) { fprintf(stderr, "[stat] fstat(fd=%d) -> %d\n", fd, r); g_stat_log++; }
  return r;
}

/* ---- glGetString nunca-NULL ---- */
static const unsigned char *w_glGetString(unsigned name) {
  static const unsigned char *(*real)(unsigned) = NULL;
  if (!real) real = dlsym(RTLD_DEFAULT, "glGetString");
  const unsigned char *r = real ? real(name) : NULL;
  return r ? r : (const unsigned char *)"";
}

/* ---- bionic-only (não existem na glibc; libc++/libGame usam) ---- */
static void b_set_abort_message(const char *m) { fprintf(stderr, "[abort_msg] %s\n", m ? m : "?"); }
static int b_system_property_get(const char *name, char *value) { (void)name; if (value) value[0] = 0; return 0; }

/* ---- C++ thread-local init helpers (_ZTH*): no-op ---- */
static void tl_noop(void) {}

/* ================= GLES2 fixes Mali-450 Utgard (receitas do reVC) ============
 * O game importa glShaderSource/glTexImage2D/glTexParameteri direto -> a tabela
 * resolve p/ estes wrappers, que corrigem p/ o Utgard e chamam o real (Mali). */
static char *str_replace_all(const char *src, const char *find, const char *repl) {
  size_t fl = strlen(find), rl = strlen(repl), n = 0;
  for (const char *p = src; (p = strstr(p, find)); p += fl) n++;
  char *out = malloc(strlen(src) + n * (rl > fl ? rl - fl : 0) + 1);
  char *o = out; const char *p = src, *q;
  while ((q = strstr(p, find))) { memcpy(o, p, q - p); o += q - p; memcpy(o, repl, rl); o += rl; p = q + fl; }
  strcpy(o, p);
  return out;
}
extern int bully_is_kmsdrm(void);
static void (*real_glShaderSource)(unsigned, int, const char *const *, const int *) = NULL;
static void my_glShaderSource(unsigned sh, int count, const char *const *str, const int *len) {
  (void)len;
  if (!real_glShaderSource) real_glShaderSource = dlsym(RTLD_DEFAULT, "glShaderSource");
  size_t total = 1;
  for (int i = 0; i < count; i++) if (str && str[i]) total += strlen(str[i]);
  char *cat = malloc(total); cat[0] = 0;
  for (int i = 0; i < count; i++) if (str && str[i]) strcat(cat, str[i]);
  /* Utgard (Mali-400/450 GP) não suporta highp -> mediump (cores lavadas) */
  /* Mali-450: a GP (vertex) É FP32 e SUPORTA highp; só a PP (fragment) não tem.
   * Forçar mediump no VERTEX quebra a precisão do skinning -> braços/corpo do
   * Jimmy (muita deformação) colapsam/NaN -> invisíveis. Então só troca
   * highp->mediump nos shaders de FRAGMENTO (mantém highp no vertex). */
  /* só fragment: highp->mediump (Utgard PP não tem highp). Vertex mantém highp (skinning). */
  int is_vertex = strstr(cat, "gl_Position") != NULL;
  /* G310/kmsdrm: MANTEM highp em TUDO (qualidade). Mali-450/fbdev: fragment->mediump (PP sem highp). */
  char *s0 = (is_vertex || bully_is_kmsdrm()) ? strdup(cat) : str_replace_all(cat, "highp", "mediump");
  free(cat);
  /* alpha-test SÓ nos shaders de PERSONAGEM (têm `fadeandcolor`, exclusivo de
   * peds/Jimmy): a roupa do Jimmy é composta numa textura (RTT, 163 draws OK),
   * mas o alpha da textura composta sai baixo no Mali -> `if (a<0.7) discard`
   * corta a roupa -> aparece e some. Baixa p/ 0.04 só nesses shaders (folhagem
   * NÃO tem fadeandcolor -> intacta). */
  char *s1 = s0;
  if (!is_vertex && strstr(s0, "fadeandcolor")) {
    s1 = str_replace_all(s0, "< 0.7)", "< 0.04)");
    free(s0);
  }
  const char *one = s1;
  if (real_glShaderSource) real_glShaderSource(sh, 1, &one, NULL);
  free(s1);
}
/* forward (defs reais mais adiante): usados aqui no my_glTexParameteri */
#define RESMAP 262144
static unsigned char g_tex_alpha[RESMAP];   /* textura tem ALPHA (recorte) -> trilinear=LINEAR (sem halo preto) */
static unsigned g_cur_tex2d;
static void (*real_glTexParameteri)(unsigned, unsigned, int) = NULL;
static void my_glTexParameteri(unsigned target, unsigned pname, int param) {
  if (!real_glTexParameteri) real_glTexParameteri = dlsym(RTLD_DEFAULT, "glTexParameteri");
  if (pname == 0x813D) return;                                  /* GL_TEXTURE_MAX_LEVEL: inexistente em GLES2 */
  /* fbdev (Mali-450): mipmap filter -> GL_LINEAR (Utgard sem mipmap = preto).
   * kmsdrm (G310): deixa o trilinear do jogo passar (mipmaps gerados no glTexImage2D). */
  int fl = 0;
  if (!bully_trilinear()) { if (!bully_is_kmsdrm() || bully_etc1_force()) fl = 1; }       /* fbdev/etc1: sempre LINEAR */
  else if (g_cur_tex2d < RESMAP && g_tex_alpha[g_cur_tex2d]) fl = 1;                       /* trilinear: recorte = LINEAR */
  if (fl && (pname == 0x2801 || pname == 0x2800) && param >= 0x2700 && param <= 0x2703)
    param = 0x2601;
  if (real_glTexParameteri) real_glTexParameteri(target, pname, param);
}
/* log de erros de compile/link de shader (achar o shader do Jimmy que falha) */
static void (*real_glCompileShader)(unsigned) = NULL;
static void my_glCompileShader(unsigned sh) {
  if (!real_glCompileShader) real_glCompileShader = dlsym(RTLD_DEFAULT, "glCompileShader");
  if (real_glCompileShader) real_glCompileShader(sh);
  void (*giv)(unsigned, unsigned, int *) = dlsym(RTLD_DEFAULT, "glGetShaderiv");
  void (*gil)(unsigned, int, int *, char *) = dlsym(RTLD_DEFAULT, "glGetShaderInfoLog");
  int ok = 1; if (giv) giv(sh, 0x8B81, &ok); /* GL_COMPILE_STATUS */
  if (!ok) { char log[1500] = {0}; if (gil) gil(sh, 1500, NULL, log); fprintf(stderr, "[shader] COMPILE FAIL sh=%u: %s\n", sh, log); }
}
static void (*real_glLinkProgram)(unsigned) = NULL;
static void my_glLinkProgram(unsigned p) {
  if (!real_glLinkProgram) real_glLinkProgram = dlsym(RTLD_DEFAULT, "glLinkProgram");
  if (real_glLinkProgram) real_glLinkProgram(p);
  void (*giv)(unsigned, unsigned, int *) = dlsym(RTLD_DEFAULT, "glGetProgramiv");
  void (*gil)(unsigned, int, int *, char *) = dlsym(RTLD_DEFAULT, "glGetProgramInfoLog");
  int ok = 1; if (giv) giv(p, 0x8B82, &ok); /* GL_LINK_STATUS */
  if (!ok) { char log[1500] = {0}; if (gil) gil(p, 1500, NULL, log); fprintf(stderr, "[shader] LINK FAIL p=%u: %s\n", p, log); }
}
/* texturas comprimidas: Mali-450 só faz ETC1 (0x8D64). Loga formatos p/ achar
 * se a camisa/skin do Jimmy usa um formato que o Mali rejeita -> transparente. */
static void (*real_glCompressedTexImage2D)(unsigned,int,unsigned,int,int,int,int,const void*) = NULL;
/* tracer de GL-op p/ achar o wedge: reescreve 1 linha + fdatasync (LCS_GLTRACE). */
static int g_gltrace_fd = -2;
static void gltrace(const char *fmt, ...) {
  if (g_gltrace_fd == -2) g_gltrace_fd = getenv("LCS_GLTRACE") ? open("/storage/roms/ports/lcs/gltrace.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644) : -1;
  if (g_gltrace_fd < 0) return;
  char b[160]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
  if (n > 0) { lseek(g_gltrace_fd, 0, SEEK_SET); if (write(g_gltrace_fd, b, n)) {} fdatasync(g_gltrace_fd); }
}
static int g_drawhb_fd = -2;
static void drawhb(const char *fmt, ...) {
  extern unsigned long g_frame_no;
  static int enabled = -1;
  static unsigned long start_frame = 0;
  if (enabled < 0) {
    const char *on = getenv("LCS_DRAWHB");
    const char *start = getenv("LCS_DRAWHB_START");
    enabled = on && *on ? 1 : 0;
    start_frame = start && *start ? strtoul(start, NULL, 0) : 0;
  }
  if (!enabled || g_frame_no < start_frame) return;
  if (g_drawhb_fd == -2)
    g_drawhb_fd = open("/storage/roms/ports/lcs/drawhb.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (g_drawhb_fd < 0) return;
  char b[256]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
  if (n > 0) {
    if (n >= (int)sizeof(b)) n = (int)sizeof(b) - 1;
    memset(b + n, ' ', sizeof(b) - (size_t)n);
    b[sizeof(b) - 1] = '\n';
    lseek(g_drawhb_fd, 0, SEEK_SET);
    if (write(g_drawhb_fd, b, sizeof(b))) {}
    fdatasync(g_drawhb_fd);
  }
}

static void lcs_track_compressed_tex_bytes(int bytes, const char *tag) {
  if (bytes <= 0 || g_cur_tex2d >= RESMAP) return;
  unsigned old = bully_g_texbytes(g_cur_tex2d);
  g_texbytes_live += (long long)bytes - old;
  bully_g_texbytes_set(g_cur_tex2d, (unsigned)bytes);
  if (getenv("LCS_COMP_TEXLOG")) {
    static int logs = 0;
    if (logs < 120) {
      fprintf(stderr, "[ctex] %s tex=%u bytes=%d old=%u live=%lldMB\n",
              tag ? tag : "?", g_cur_tex2d, bytes, old,
              g_texbytes_live / (1024 * 1024));
      logs++;
    }
  }
}

static void my_glCompressedTexImage2D(unsigned t,int l,unsigned ifmt,int w,int h,int b,int sz,const void*d) {
  static long c=0; gltrace("compressedTex #%ld fmt=0x%x %dx%d lvl=%d\n", ++c, ifmt, w, h, l);
  if (!real_glCompressedTexImage2D) real_glCompressedTexImage2D = dlsym(RTLD_DEFAULT, "glCompressedTexImage2D");
  static int n = 0;
  if (n < 40) { fprintf(stderr, "[tex] compressed fmt=0x%x %dx%d sz=%d lvl=%d\n", ifmt, w, h, sz, l); n++; }
  /* Mali-450 Utgard: a MMU de textura estoura ao streamar a cidade -> wedge.
   * Pula os MIPS (lvl>0) das texturas ETC comprimidas: -33% memoria, e o
   * MIN_FILTER ja e forcado p/ LINEAR (mip incompleto e ok). LCS_KEEPMIP reativa. */
  if (l > 0 && !getenv("LCS_KEEPMIP")) {
    /* 🔑 MEIA-RES DE GRACA p/ RAM (anti-swap/trava dirigindo): usa o MIP 1 (ja
     * fornecido pelo motor) como nivel 0 -> textura fica metade da res = -75% RAM
     * por textura mipada, SEM re-encodar. LCS_TEX_HALF. So as >= LCS_TEX_HALF_MIN
     * (default 512: meia-res so nas grandes, preserva HUD/texto pequeno).
     * O nivel 0 cheio ja foi mandado, este re-upload nivel-0 REALLOCA p/ metade. */
    static int half = -1, hmin = 0;
    if (half < 0) { half = getenv("LCS_TEX_HALF") ? 1 : 0;
                    hmin = getenv("LCS_TEX_HALF_MIN") ? atoi(getenv("LCS_TEX_HALF_MIN")) : 512; }
    if (half && l == 1 && (w*2 >= hmin || h*2 >= hmin) && real_glCompressedTexImage2D) {
      real_glCompressedTexImage2D(t, 0, ifmt, w, h, b, sz, d);
      if (t == 0x0DE1) lcs_track_compressed_tex_bytes(sz, "half");
    }
    return;
  }
  if (lcs_tex_light_stub_compressed(t, l, ifmt, w, h)) {
    static const unsigned char blank_etc1[8] = {0};
    static int logs = 0;
    if (real_glCompressedTexImage2D) {
      real_glCompressedTexImage2D(t, 0, ifmt, 4, 4, 0, sizeof(blank_etc1), blank_etc1);
      if (t == 0x0DE1) lcs_track_compressed_tex_bytes((int)sizeof(blank_etc1), "stub");
    }
    if (logs < 24 || env_on("LCS_TEX_LIGHT_LOG")) {
      fprintf(stderr,
              "[texlight] stub compressed live=%ld cap=%ld fmt=0x%x %dx%d -> 4x4\n",
              g_tex_live, lcs_tex_light_cap(), ifmt, w, h);
      logs++;
    }
    return;
  }
  if (real_glCompressedTexImage2D) {
    real_glCompressedTexImage2D(t,l,ifmt,w,h,b,sz,d);
    if (t == 0x0DE1 && l == 0) lcs_track_compressed_tex_bytes(sz, "base");
  }
}
static int lcs_glstate_enabled(void) { static int m=-1; if (m < 0) m = getenv("LCS_GLSTATE") ? 1 : 0; return m; }
static unsigned g_cur_program = 0;
static unsigned g_active_tex_unit = 0;
static unsigned g_cull_face = 0x0405; /* GL_BACK */
static unsigned char g_depth_mask = 1, g_color_mask[4] = {1,1,1,1};
static unsigned char g_cap_blend = 0, g_cap_depth = 0, g_cap_cull = 0;
static void (*real_glUseProgram)(unsigned) = NULL;
static void my_glUseProgram(unsigned program) {
  g_cur_program = program;
  if (!real_glUseProgram) real_glUseProgram = dlsym(RTLD_DEFAULT, "glUseProgram");
  if (real_glUseProgram) real_glUseProgram(program);
}
static void (*real_glActiveTexture)(unsigned) = NULL;
static void my_glActiveTexture(unsigned texture) {
  if (texture >= 0x84C0 && texture < 0x84E0) g_active_tex_unit = texture - 0x84C0;
  if (!real_glActiveTexture) real_glActiveTexture = dlsym(RTLD_DEFAULT, "glActiveTexture");
  if (real_glActiveTexture) real_glActiveTexture(texture);
}
static void (*real_glDepthMask)(unsigned char) = NULL;
static void my_glDepthMask(unsigned char flag) {
  g_depth_mask = flag ? 1 : 0;
  if (!real_glDepthMask) real_glDepthMask = dlsym(RTLD_DEFAULT, "glDepthMask");
  if (real_glDepthMask) real_glDepthMask(flag);
}
static void (*real_glColorMask)(unsigned char, unsigned char, unsigned char, unsigned char) = NULL;
static void my_glColorMask(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
  g_color_mask[0] = r ? 1 : 0; g_color_mask[1] = g ? 1 : 0;
  g_color_mask[2] = b ? 1 : 0; g_color_mask[3] = a ? 1 : 0;
  if (!real_glColorMask) real_glColorMask = dlsym(RTLD_DEFAULT, "glColorMask");
  if (real_glColorMask) real_glColorMask(r,g,b,a);
}
static void (*real_glCullFace)(unsigned) = NULL;
static void my_glCullFace(unsigned mode) {
  g_cull_face = mode;
  if (!real_glCullFace) real_glCullFace = dlsym(RTLD_DEFAULT, "glCullFace");
  if (real_glCullFace) real_glCullFace(mode);
}
/* TESTE: ignora glEnable(GL_BLEND) -> se a camisa do Jimmy aparecer OPACA,
 * confirma que ela some por alpha-blend (alpha~0). (Quebra transparências
 * legítimas; é só p/ diagnóstico.) */
static void (*real_glEnable)(unsigned) = NULL;
static void my_glEnable(unsigned cap) {
  if (cap == 0x0BE2) g_cap_blend = 1;       /* GL_BLEND */
  else if (cap == 0x0B71) g_cap_depth = 1;  /* GL_DEPTH_TEST */
  else if (cap == 0x0B44) g_cap_cull = 1;   /* GL_CULL_FACE */
  if (!real_glEnable) real_glEnable = dlsym(RTLD_DEFAULT, "glEnable");
  if (real_glEnable) real_glEnable(cap); /* (skip de GL_BLEND revertido: não era blend) */
}
static void (*real_glDisable)(unsigned) = NULL;
static void my_glDisable(unsigned cap) {
  if (cap == 0x0BE2) g_cap_blend = 0;
  else if (cap == 0x0B71) g_cap_depth = 0;
  else if (cap == 0x0B44) g_cap_cull = 0;
  if (!real_glDisable) real_glDisable = dlsym(RTLD_DEFAULT, "glDisable");
  if (real_glDisable) real_glDisable(cap);
}
static void (*real_glClear)(unsigned) = NULL;
static int g_cleardbg = 0;
unsigned g_pending_clear = 0;   /* clear adiado dentro de FBO (só efetiva se vier draw) */
static int g_defer_clear = -1;
void bully_flush_pending_clear(void) { /* chamado pelos draws dentro do FBO */
  if (g_pending_clear) {
    if (!real_glClear) real_glClear = dlsym(RTLD_DEFAULT, "glClear");
    if (real_glClear) real_glClear(g_pending_clear);
    g_pending_clear = 0;
  }
}
static void my_glClear(unsigned mask) {
  extern int g_in_fbo, g_rtt_clears;
  extern unsigned long g_frame_no;
  if (!real_glClear) real_glClear = dlsym(RTLD_DEFAULT, "glClear");
  if (g_in_fbo) g_rtt_clears++;
  /* LCS faz clears de profundidade entre passes do mundo/HUD. Forcar COLOR aqui
   * apaga o mundo 3D depois que ele desenha. O hack antigo fica opt-in para
   * diagnostico, mas o default precisa preservar a mascara nativa. */
  unsigned m = (!g_in_fbo && getenv("LCS_FORCE_COLOR_CLEAR")) ? (mask | 0x4000) : mask;
  extern long g_clears_screen; if (!g_in_fbo) g_clears_screen++;
  if (g_cleardbg < 12 || (lcs_glstate_enabled() && g_cleardbg < 160)) {
    fprintf(stderr, "[gl] f=%lu glClear in_fbo=%d mask=0x%x -> 0x%x\n", g_frame_no, g_in_fbo, mask, m);
    g_cleardbg++;
  }
  drawhb("ENTER glClear f=%lu fbo=%d mask=0x%x m=0x%x\n",
         g_frame_no, g_in_fbo, mask, m);
  if (real_glClear) real_glClear(m);
  drawhb("LEAVE glClear f=%lu fbo=%d mask=0x%x m=0x%x\n",
         g_frame_no, g_in_fbo, mask, m);
}
/* ===== INSTRUMENTACAO DE VAZAMENTO (recursos GL vivos) =====
 * Conta texturas/FBOs/renderbuffers vivos (gen - delete) + bytes estimados, p/
 * descobrir O QUE vaza (testers: RAM enche e OOM em ~30min no R36S). Report a
 * cada 120 frames via bully_resource_report(). So medicao; nao altera render. */
long g_tex_live = 0, g_fb_live = 0, g_rb_live = 0, g_tex_gen = 0, g_tex_del = 0;
long long g_texbytes_live = 0;
/* medicao p/ decidir halving SELETIVO: full-bytes (em res cheia) das texturas que
 * o TEX_HALF reduz, separadas por classe. delta de manter 512 cheias = 3/4 do full. */
long long g_half512_full = 0, g_half1024_full = 0; long g_half512_cnt = 0, g_half1024_cnt = 0;
/* RESMAP + g_tex_alpha declarados acima (antes do my_glTexParameteri) */
static unsigned g_texbytes[RESMAP];   /* bytes correntes por id de textura */
unsigned bully_g_texbytes(unsigned id) { return id < RESMAP ? g_texbytes[id] : 0; }
void bully_g_texbytes_set(unsigned id, unsigned b) { if (id < RESMAP) g_texbytes[id] = b; }
static unsigned g_rbbytes[RESMAP];    /* idem renderbuffer */
static unsigned char g_half_seen[RESMAP]; /* medicao: id ja contado no tally do halving */
static unsigned g_cur_tex2d = 0, g_cur_rb = 0;
static int bpp_internal(unsigned ifmt) {
  switch (ifmt) {
    case 0x8056: case 0x8D62: case 0x8033: case 0x8034: case 0x8363: return 2; /* RGBA4/565/4444/5551 */
    case 0x1907: case 0x81A6: return 3;       /* RGB / DEPTH24 */
    default: return 4;                          /* RGBA8/RGBA/DEPTH24_STENCIL8/etc */
  }
}
static long tex_chain_bytes(unsigned ifmt, int w, int h, int levels) {
  if (levels < 1) levels = 1;
  long bpp = bpp_internal(ifmt), t = 0;
  for (int i = 0; i < levels && (w > 0 || h > 0); i++) {
    int ww = w > 0 ? w : 1, hh = h > 0 ? h : 1;
    t += (long)ww * hh * bpp; w >>= 1; h >>= 1;
  }
  return t;
}
static void (*real_glBindTexture)(unsigned, unsigned) = NULL;
extern void bully_page_on_bind(unsigned target, unsigned id);
static void my_glBindTexture(unsigned target, unsigned tex) {
  if (target == 0x0DE1 && g_active_tex_unit == 0) g_cur_tex2d = tex;   /* GL_TEXTURE_2D */
  if (!real_glBindTexture) real_glBindTexture = dlsym(RTLD_DEFAULT, "glBindTexture");
  if (real_glBindTexture) real_glBindTexture(target, tex);
  bully_page_on_bind(target, tex);           /* PAGINACAO: LRU + page-fault + despejo (gated BULLY_PAGE) */
}
static void (*real_glGenTextures)(int, unsigned*) = NULL;
static void my_glGenTextures(int n, unsigned *ids) {
  if (!real_glGenTextures) real_glGenTextures = dlsym(RTLD_DEFAULT, "glGenTextures");
  if (real_glGenTextures) real_glGenTextures(n, ids);
  g_tex_live += n; g_tex_gen += n;
}
static void (*real_glDeleteTextures)(int, const unsigned*) = NULL;
static void my_glDeleteTextures(int n, const unsigned *ids) {
  for (int i = 0; ids && i < n; i++) { unsigned id = ids[i];
    if (id < RESMAP) {
      if (g_page_name[id]) {   /* PAGINACAO: motor deletou uma pageavel -> limpa estado (id pode ser reusado) */
        if (g_page_present[id]) g_page_resident -= (long long)PAGE_ETCSZ(id);
        free(g_page_name[id]); g_page_name[id] = NULL; g_page_present[id] = 0;
      }
      g_texbytes_live -= g_texbytes[id]; g_texbytes[id] = 0; } }
  if (!real_glDeleteTextures) real_glDeleteTextures = dlsym(RTLD_DEFAULT, "glDeleteTextures");
  if (real_glDeleteTextures) real_glDeleteTextures(n, ids);
  g_tex_live -= n; g_tex_del += n;
}
static void (*real_glGenFramebuffers)(int, unsigned*) = NULL;
static void my_glGenFramebuffers(int n, unsigned *ids) {
  if (!real_glGenFramebuffers) real_glGenFramebuffers = dlsym(RTLD_DEFAULT, "glGenFramebuffers");
  if (real_glGenFramebuffers) real_glGenFramebuffers(n, ids);
  g_fb_live += n;
}
static void (*real_glDeleteFramebuffers)(int, const unsigned*) = NULL;
static void my_glDeleteFramebuffers(int n, const unsigned *ids) {
  if (!real_glDeleteFramebuffers) real_glDeleteFramebuffers = dlsym(RTLD_DEFAULT, "glDeleteFramebuffers");
  if (real_glDeleteFramebuffers) real_glDeleteFramebuffers(n, ids);
  g_fb_live -= n;
}
static void (*real_glGenRenderbuffers)(int, unsigned*) = NULL;
static void my_glGenRenderbuffers(int n, unsigned *ids) {
  if (!real_glGenRenderbuffers) real_glGenRenderbuffers = dlsym(RTLD_DEFAULT, "glGenRenderbuffers");
  if (real_glGenRenderbuffers) real_glGenRenderbuffers(n, ids);
  g_rb_live += n;
}
static void (*real_glDeleteRenderbuffers)(int, const unsigned*) = NULL;
static void my_glDeleteRenderbuffers(int n, const unsigned *ids) {
  for (int i = 0; ids && i < n; i++) { unsigned id = ids[i];
    if (id < RESMAP) { g_texbytes_live -= g_rbbytes[id]; g_rbbytes[id] = 0; } }
  if (!real_glDeleteRenderbuffers) real_glDeleteRenderbuffers = dlsym(RTLD_DEFAULT, "glDeleteRenderbuffers");
  if (real_glDeleteRenderbuffers) real_glDeleteRenderbuffers(n, ids);
  g_rb_live -= n;
}
static void (*real_glBindRenderbuffer)(unsigned, unsigned) = NULL;
static void my_glBindRenderbuffer(unsigned target, unsigned rb) {
  g_cur_rb = rb;
  if (!real_glBindRenderbuffer) real_glBindRenderbuffer = dlsym(RTLD_DEFAULT, "glBindRenderbuffer");
  if (real_glBindRenderbuffer) real_glBindRenderbuffer(target, rb);
}
static void (*real_glRenderbufferStorage)(unsigned, unsigned, int, int) = NULL;
static void my_glRenderbufferStorage(unsigned target, unsigned ifmt, int w, int h) {
  long b = (long)(w > 0 ? w : 1) * (h > 0 ? h : 1) * bpp_internal(ifmt);
  if (g_cur_rb < RESMAP) { g_texbytes_live += b - g_rbbytes[g_cur_rb]; g_rbbytes[g_cur_rb] = b; }
  if (!real_glRenderbufferStorage) real_glRenderbufferStorage = dlsym(RTLD_DEFAULT, "glRenderbufferStorage");
  if (real_glRenderbufferStorage) real_glRenderbufferStorage(target, ifmt, w, h);
}
/* chamado pelo loop de render (jni_shim) a cada 120 frames */
void bully_resource_report(void) {
  fprintf(stderr, "[leak] tex_live=%ld (gen=%ld del=%ld) fbo_live=%ld rb_live=%ld | ~%lld MB em texturas/RB vivos\n",
          g_tex_live, g_tex_gen, g_tex_del, g_fb_live, g_rb_live, g_texbytes_live / (1024*1024));
  /* medicao halving seletivo: full = bytes em res CHEIA das texturas reduzidas.
   * Hoje (todas reduzidas) usam full/4. Manter 512 cheias custaria +3/4 do full-512. */
  fprintf(stderr, "[texmem] 512-range: %ld tex full=%lld MB (manter-cheias custa +%lld MB) | 1024+: %ld tex full=%lld MB (reduzidas ja economizam %lld MB)\n",
          g_half512_cnt, g_half512_full/(1024*1024), (g_half512_full*3/4)/(1024*1024),
          g_half1024_cnt, g_half1024_full/(1024*1024), (g_half1024_full*3/4)/(1024*1024));
}

/* glTexStorage2D (GLES3) — a camisa do Jimmy pode vir por aqui (não pelo
 * glTexImage2D). Loga formato/níveis. */
static void (*real_glTexStorage2D)(unsigned, int, unsigned, int, int) = NULL;
static void my_glTexStorage2D(unsigned t, int levels, unsigned ifmt, int w, int h) {
  if (!real_glTexStorage2D) real_glTexStorage2D = dlsym(RTLD_DEFAULT, "glTexStorage2D");
  if (t == 0x0DE1) g_path_fresh = 0;
  static int n = 0;
  if (n < 30) { fprintf(stderr, "[tex] STORAGE levels=%d ifmt=0x%x %dx%d\n", levels, ifmt, w, h); n++; }
  { long b = tex_chain_bytes(ifmt, w, h, levels);
    if (g_cur_tex2d < RESMAP) { g_texbytes_live += b - g_texbytes[g_cur_tex2d]; g_texbytes[g_cur_tex2d] = b; } }
  if (real_glTexStorage2D) real_glTexStorage2D(t, levels, ifmt, w, h);
}
static void (*real_glTexSubImage2D)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*) = NULL;
static void my_glTexSubImage2D(unsigned t,int l,int xo,int yo,int w,int h,unsigned fmt,unsigned type,const void*px) {
  extern unsigned long g_frame_no;
  if (!real_glTexSubImage2D) real_glTexSubImage2D = dlsym(RTLD_DEFAULT, "glTexSubImage2D");
  if (t == 0x0DE1 && l == 0) g_path_fresh = 0;
  static int n = 0;
  if (n < 30 && l == 0) { fprintf(stderr, "[tex] SUB fmt=0x%x type=0x%x %dx%d\n", fmt, type, w, h); n++; }
  drawhb("ENTER glTexSubImage2D f=%lu tex=%u tgt=0x%x lvl=%d xy=%d,%d wh=%dx%d fmt=0x%x type=0x%x px=%d texN=%ld texMB=%lld\n",
         g_frame_no, g_cur_tex2d, t, l, xo, yo, w, h, fmt, type, px ? 1 : 0,
         g_tex_live, g_texbytes_live / (1024 * 1024));
  if (real_glTexSubImage2D) real_glTexSubImage2D(t,l,xo,yo,w,h,fmt,type,px);
  drawhb("LEAVE glTexSubImage2D f=%lu tex=%u tgt=0x%x lvl=%d xy=%d,%d wh=%dx%d fmt=0x%x type=0x%x px=%d texN=%ld texMB=%lld\n",
         g_frame_no, g_cur_tex2d, t, l, xo, yo, w, h, fmt, type, px ? 1 : 0,
         g_tex_live, g_texbytes_live / (1024 * 1024));
}
/* status do FBO (render-to-texture da roupa) — se INCOMPLETO no Mali, a textura
 * do corpo fica vazia -> camisa preta+discard. */
static void (*real_glBindFramebuffer)(unsigned, unsigned) = NULL;
unsigned long g_fbo_binds = 0; /* contador p/ medir RTT */
unsigned long g_frame_no = 0;  /* setado pelo loop de render (jni_shim) */
int g_in_fbo = 0;              /* >0 = dentro de um render-to-texture */
int g_rtt_draws = 0, g_rtt_clears = 0; /* trace: draws/clears no FBO atual */
unsigned g_rtt_tex = 0;        /* textura anexada ao FBO atual */
static int g_rtt_trace = 0;
static void my_glBindFramebuffer(unsigned t, unsigned fb) {
  if (!real_glBindFramebuffer) real_glBindFramebuffer = dlsym(RTLD_DEFAULT, "glBindFramebuffer");
  if (g_rtt_trace == 0) g_rtt_trace = getenv("BULLY_RTT_TRACE") ? 1 : 0;
  if (fb != 0) { g_in_fbo = 1; g_fbo_binds++; g_rtt_draws = 0; g_rtt_clears = 0; g_rtt_tex = 0; g_pending_clear = 0; }
  drawhb("ENTER glBindFramebuffer f=%lu target=0x%x fb=%u in_fbo=%d draws=%d tex=%u\n",
         g_frame_no, t, fb, g_in_fbo, g_rtt_draws, g_rtt_tex);
  if (real_glBindFramebuffer) real_glBindFramebuffer(t, fb);
  drawhb("LEAVE glBindFramebuffer f=%lu target=0x%x fb=%u in_fbo=%d draws=%d tex=%u\n",
         g_frame_no, t, fb, g_in_fbo, g_rtt_draws, g_rtt_tex);
  /* ao SAIR do render-to-texture (roupa do Jimmy), o Mali Utgard não GRAVA o
   * render na textura sem sync -> modelo amostra vazio (roupa pisca e some).
   * glFinish ESPERA a GPU gravar antes de amostrar. Só DEPOIS do frame 300
   * (loading pesado já passou -> não satura o device). */
  if (fb == 0 && g_in_fbo) {
    g_in_fbo = 0;
    g_pending_clear = 0; /* pula clear-only (não apaga a roupa já composta) */
    if (g_rtt_trace && g_frame_no > 60) {
      static int tn = 0;
      if (tn < 400) { fprintf(stderr, "[rtt] composite tex=%u draws=%d clears=%d (frame %lu)\n", g_rtt_tex, g_rtt_draws, g_rtt_clears, g_frame_no); tn++; }
    }
    if (g_frame_no > 300) {
      /* glFinish trava a GPU Utgard ate gravar -> necessario p/ a ROUPA do Jimmy
       * (RTT pesado, ~163 draws) ser amostrada certa. MAS em CADA RTT (a cena
       * recompoe a cada movimento) satura o A53 e ESTOURA o audio (causa-raiz do
       * estouro em movimento; loading e limpo pq nao tem RTT). HEURISTICA: glFinish
       * so nos RTT PESADOS (>= MINDRAWS draws = a roupa); glFlush (nao-bloqueante)
       * nos leves (shadow/post) -> libera CPU p/ a mixagem -> audio limpo ao mover,
       * roupa preservada. BULLY_RTT_FINISH_MINDRAWS (default 0 = sempre glFinish). */
      static int mind = -1;
      if (mind < 0) { const char *e = getenv("BULLY_RTT_FINISH_MINDRAWS"); mind = e ? atoi(e) : 0; }
      if (g_rtt_draws >= mind) {
        static void (*fin)(void) = NULL;
        if (!fin) fin = dlsym(RTLD_DEFAULT, "glFinish");
        drawhb("ENTER rtt glFinish f=%lu draws=%d tex=%u\n", g_frame_no, g_rtt_draws, g_rtt_tex);
        if (fin) fin();
        drawhb("LEAVE rtt glFinish f=%lu draws=%d tex=%u\n", g_frame_no, g_rtt_draws, g_rtt_tex);
      } else {
        static void (*fl)(void) = NULL;
        if (!fl) fl = dlsym(RTLD_DEFAULT, "glFlush");
        drawhb("ENTER rtt glFlush f=%lu draws=%d tex=%u\n", g_frame_no, g_rtt_draws, g_rtt_tex);
        if (fl) fl();
        drawhb("LEAVE rtt glFlush f=%lu draws=%d tex=%u\n", g_frame_no, g_rtt_draws, g_rtt_tex);
      }
    } else {
      static void (*fl)(void) = NULL;
      if (!fl) fl = dlsym(RTLD_DEFAULT, "glFlush");
      drawhb("ENTER rtt glFlush f=%lu draws=%d tex=%u\n", g_frame_no, g_rtt_draws, g_rtt_tex);
      if (fl) fl();
      drawhb("LEAVE rtt glFlush f=%lu draws=%d tex=%u\n", g_frame_no, g_rtt_draws, g_rtt_tex);
    }
  }
}
static void (*real_glFramebufferTexture2D)(unsigned,unsigned,unsigned,unsigned,int) = NULL;
static void my_glFramebufferTexture2D(unsigned t,unsigned att,unsigned tt,unsigned tex,int lvl) {
  extern unsigned g_rtt_tex;
  if (att == 0x8CE0) g_rtt_tex = tex;     /* trace: textura-cor anexada ao FBO atual */
  if (!real_glFramebufferTexture2D) real_glFramebufferTexture2D = dlsym(RTLD_DEFAULT, "glFramebufferTexture2D");
  if (real_glFramebufferTexture2D) real_glFramebufferTexture2D(t,att,tt,tex,lvl);
  static int n = 0;
  if (n < 14) {
    unsigned (*chk)(unsigned) = dlsym(RTLD_DEFAULT, "glCheckFramebufferStatus");
    unsigned s = chk ? chk(0x8D40) : 0;
    unsigned tb = (tex < RESMAP) ? g_texbytes[tex] : 0;  /* bytes da textura -> infere a resolucao do RT */
    fprintf(stderr, "[fbo] ATTACH att=0x%x tex=%u lvl=%d -> status=0x%x %s | RT~%u KB (%s)\n",
            att, tex, lvl, s, s == 0x8CD5 ? "OK" : "INCOMPLETO", tb/1024,
            tb > 2500000 ? "ALTA" : tb > 800000 ? "media" : "baixa");
    n++;
  }
}
static void (*real_glReadPixels)(int,int,int,int,unsigned,unsigned,void*) = NULL;
static void my_glReadPixels(int x,int y,int w,int h,unsigned fmt,unsigned type,void*px) {
  if (!real_glReadPixels) real_glReadPixels = dlsym(RTLD_DEFAULT, "glReadPixels");
  static int n = 0;
  if (n < 12) { fprintf(stderr, "[fbo] READPIXELS %dx%d fmt=0x%x type=0x%x\n", w, h, fmt, type); n++; }
  if (real_glReadPixels) real_glReadPixels(x,y,w,h,fmt,type,px);
}
static unsigned (*real_glCheckFramebufferStatus)(unsigned) = NULL;
static unsigned my_glCheckFramebufferStatus(unsigned t) {
  if (!real_glCheckFramebufferStatus) real_glCheckFramebufferStatus = dlsym(RTLD_DEFAULT, "glCheckFramebufferStatus");
  unsigned s = real_glCheckFramebufferStatus ? real_glCheckFramebufferStatus(t) : 0;
  static int n = 0;
  if (n < 20) { fprintf(stderr, "[fbo] CheckStatus=0x%x %s\n", s, s == 0x8CD5 ? "COMPLETE" : "INCOMPLETO!"); n++; }
  return s;
}
static void (*real_glClearColor)(float, float, float, float) = NULL;
static int g_ccdbg = 0;
static void my_glClearColor(float r, float g, float b, float a) {
  if (!real_glClearColor) real_glClearColor = dlsym(RTLD_DEFAULT, "glClearColor");
  if (g_ccdbg < 8) { fprintf(stderr, "[gl] glClearColor %.2f %.2f %.2f %.2f\n", r, g, b, a); g_ccdbg++; }
  if (real_glClearColor) real_glClearColor(r, g, b, a);
}
static int bpp_of(unsigned fmt, unsigned type) {
  if (type == 0x1401) return fmt == 0x1908 ? 4 : fmt == 0x1907 ? 3 : fmt == 0x190A ? 2 : 1;
  if (type == 0x8033 || type == 0x8034 || type == 0x8363) return 2; /* 4444/5551/565 */
  return 0; /* desconhecido -> não reduz */
}
static int g_tex_half = -1;
static void (*real_glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *) = NULL;
static void (*real_glGenerateMipmap)(unsigned) = NULL;
static void my_glTexImage2D(unsigned tgt, int lvl, int ifmt, int w, int h, int bord, unsigned fmt, unsigned type, const void *px) {
  if (!real_glTexImage2D) real_glTexImage2D = dlsym(RTLD_DEFAULT, "glTexImage2D");
  int pf = (tgt == 0x0DE1 && lvl == 0) ? g_path_fresh : 0;
  if (tgt == 0x0DE1 && lvl == 0) g_path_fresh = 0;
  if (g_tex_half < 0) g_tex_half = getenv("BULLY_TEX_HALF") ? 1 : 0;
  /* GROUND-TRUTH p/ a sidetable ETC1: casa nome do .tex <-> upload real. */
  if (env_on2("LCS_TEXDIAG", "BULLY_TEXDIAG")) {
    static int td = 0;
    if (td < 400) { fprintf(stderr, "[texdiag] '%s' fresh=%d lvl=%d %dx%d ifmt=0x%x fmt=0x%x type=0x%x px=%d\n",
        bully_cur_tex_path[0] ? bully_cur_tex_path : "(none)", pf, lvl, w, h, ifmt, fmt, type, px ? 1 : 0); td++; }
  }
  /* DUMP os pixels DECODIFICADOS pelo engine (ground-truth p/ crackear o .tex offline).
   * BULLY_DUMPTEX=substring -> grava /tmp/bullydump/<basename>_L<lvl>_<w>x<h>_t<type>.bin */
  { const char *want = getenv("BULLY_DUMPTEX");
    if (want && want[0] && px && bully_cur_tex_path[0] && strstr(bully_cur_tex_path, want)) {
      int bb = bpp_of(fmt, type); if (bb <= 0) bb = 4;
      const char *bn = strrchr(bully_cur_tex_path, '/'); bn = bn ? bn+1 : bully_cur_tex_path;
      char fn[256]; snprintf(fn, sizeof(fn), "/tmp/bullydump/%s_L%d_%dx%d_t%x.bin", bn, lvl, w, h, type);
      mkdir("/tmp/bullydump", 0777);
      FILE *df = fopen(fn, "wb");
      if (df) { fwrite(px, 1, (size_t)w*h*bb, df); fclose(df); fprintf(stderr, "[dumptex] %s (%d bytes)\n", fn, w*h*bb); }
    }
  }
  /* leak-track: bytes do nível 0 da textura 2D corrente (RTT geralmente vem por aqui c/ px=NULL) */
  if (lvl == 0 && g_cur_tex2d < RESMAP) {
    int bb = bpp_of(fmt, type); if (bb <= 0) bb = 4;
    long b = (long)(w > 0 ? w : 1) * (h > 0 ? h : 1) * bb;
    g_texbytes_live += b - g_texbytes[g_cur_tex2d]; g_texbytes[g_cur_tex2d] = b;
  }
  /* CACHE ETC1 offline: textura opaca 565 com ETC1 bakeado -> sobe ETC1 (4x menos VRAM)
   * e PULA o resto (conversoes/halving/mips). No bake, grava e segue o caminho normal. */
  if ((lvl > 0 || pf) && bully_try_etc1(tgt, lvl, w, h, fmt, type, px)) {
    if (lvl == 0 && g_cur_tex2d < RESMAP)   /* registra a textura ETC1 como pageavel */
      bully_page_register(g_cur_tex2d, bully_cur_tex_path, w, h, (unsigned)((size_t)(w/4)*(h/4)*8));
    return;
  }
  /* BAKE-ALL: ja encodamos o ETC1 (565) acima; agora sobe um STUB 1x1 em vez da textura
   * cheia -> a GL fica minuscula (sem estourar a MMU) e NAO precisamos deletar GL textures
   * (era o que crashava por use-after-free no wrap do ring). O sprite desenha 1x1 (lixo,
   * tanto faz: e so um passe de conversao). */
  { static int g_bakeall = -1; if (g_bakeall < 0) g_bakeall = getenv("BULLY_BAKEALL") ? 1 : 0;
    if (g_bakeall) {
      if (lvl == 0) { static const unsigned char stub[4] = {0,0,0,255};
        if (real_glTexImage2D) real_glTexImage2D(tgt, 0, 0x1908, 1, 1, 0, 0x1908, 0x1401, stub); }
      return;
    } }
  if (ifmt == 0x8058) ifmt = 0x1908;       /* GL_RGBA8 -> GL_RGBA (GLES2 não aceita sized) */
  else if (ifmt == 0x8051) ifmt = 0x1907;  /* GL_RGB8 -> GL_RGB */
  /* pula mipmaps: como forço MIN_FILTER=LINEAR, os níveis >0 nunca são usados ->
   * só desperdiçam memória de textura da GPU (o Mali Utgard trava ao estourar). */
  if (g_tex_half && lvl > 0) return;
  /* LUMINANCE vazia (px=NULL) = alvo de render-to-texture da roupa; Mali não
   * renderiza p/ LUMINANCE -> aloca RGBA (renderável). Sem reduzir (é o alvo). */
  if ((fmt == 0x1909 || fmt == 0x190A) && type == 0x1401 && !px && w > 0 && h > 0) {
    if (real_glTexImage2D) real_glTexImage2D(tgt, lvl, 0x1908, w, h, bord, 0x1908, 0x1401, NULL);
    return;
  }
  /* monta os dados finais; converte LUMINANCE->RGBA (Mali lê L como (L,L,L,L)) */
  const unsigned char *data = px;
  unsigned ufmt = fmt, utype = type;
  unsigned char *conv = NULL;
  if ((fmt == 0x1909 || fmt == 0x190A) && type == 0x1401 && px && w > 0 && h > 0) {
    int la = (fmt == 0x190A);
    const unsigned char *src = px;
    conv = malloc((size_t)w * h * 4);
    if (conv) {
      for (int i = 0; i < w * h; i++) {
        unsigned char L = src[la ? i * 2 : i];
        conv[i*4] = L; conv[i*4+1] = L; conv[i*4+2] = L; conv[i*4+3] = la ? src[i*2+1] : 255;
      }
      data = conv; ufmt = 0x1908; utype = 0x1401; ifmt = 0x1908;
    }
  }
  /* HALVING SELETIVO (v8.3 qualidade): reduz pela metade so as texturas >=1024
   * (as MAIORES, grosso da memoria). As 512-range ficam em RES CHEIA (nitidas) ->
   * mundo mais nitido. Custo ~+42MB (cabe na folga; despejo segura excursoes).
   * Antes era >=512 (tudo reduzido). UV normalizado -> reduzir nao quebra coords.
   * Override: BULLY_TEX_HALF_MIN (default 1024; 512 = comportamento antigo). */
  /* TRILINEAR: detecta textura de RECORTE (alpha vazado: folhas/cercas/portoes/corrimaos).
   * Recorte NAO pode ter mipmap (o mip mistura o RGB preto dos texels transparentes -> HALO
   * PRETO FORTE). Detecta por SCAN do alpha (so recortes REAIS, nao todo RGBA -> nao quebra
   * o resto). is_cutout -> LINEAR (sem mip), setado AQUI no upload E no glTexParameteri
   * (robusto a ordem param/upload -- foi o que quebrou antes). [2026-06-20] */
  int is_cutout = 0;
  if (bully_trilinear() && lvl == 0 && data) {
    if (utype == 0x8033 || utype == 0x8034) is_cutout = 1;          /* 4444/5551 = formato com alpha */
    else if (ufmt == 0x1908 && utype == 0x1401 && w > 0 && h > 0) { /* RGBA8888: scaneia o alpha */
      int n = w * h, step = n > 4096 ? n / 4096 : 1, tr = 0;
      for (int i = 0; i < n; i += step) if (((const unsigned char *)data)[(size_t)i*4+3] < 250) { if (++tr > 8) break; }
      is_cutout = (tr > 8);
    }
  }
  if (lvl == 0 && g_cur_tex2d < RESMAP) g_tex_alpha[g_cur_tex2d] = is_cutout ? 1 : 0;
  /* DIAG (BULLY_TEXLOG): lista texturas com alpha -> achar as folhas/papeis do chao e ver
   * se sao detectadas como recorte. So qdo o env liga (sem custo no normal). */
  if (getenv("BULLY_TEXLOG") && lvl == 0 && data && (ufmt == 0x1908 || utype == 0x8033 || utype == 0x8034)) {
    static int lg = 0; if (lg < 300) { fprintf(stderr, "[texlog] %s %dx%d ufmt=%x type=%x cutout=%d\n",
      bully_cur_tex_path[0]?bully_cur_tex_path:"?", w, h, ufmt, utype, is_cutout); lg++; }
  }
  /* ALPHA BLEED: nos recortes (folhas/trepadeiras) os texels transparentes tem RGB PRETO;
   * o bilinear na borda mistura esse preto na parte visivel = CONTORNO PRETO FORTE. Preenche
   * o RGB dos transparentes com o do vizinho opaco (2 passes = 2 aneis). So muda RGB, alpha
   * intacto (alpha-test inalterado). RGBA8888 recorte. [2026-06-20] */
  if (is_cutout && data && w >= 2 && h >= 2 && lvl == 0) {
    int W = w, H = h;
    if (ufmt == 0x1908 && utype == 0x1401) {            /* RGBA8888 (4 bytes/texel) */
      unsigned char *d = malloc((size_t)W * H * 4);
      if (d) { memcpy(d, data, (size_t)W * H * 4);
        for (int p = 0; p < 2; p++) for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
          size_t o = ((size_t)y * W + x) * 4;  if (d[o+3] >= 128) continue;
          int nx[4] = {x-1,x+1,x,x}, ny[4] = {y,y,y-1,y+1};
          for (int k = 0; k < 4; k++) { if (nx[k]<0||nx[k]>=W||ny[k]<0||ny[k]>=H) continue; size_t n = ((size_t)ny[k]*W+nx[k])*4;
            if (d[n+3] >= 128) { d[o]=d[n]; d[o+1]=d[n+1]; d[o+2]=d[n+2]; break; } } }
        if (conv) free(conv); conv = d; data = d; }
    } else if (utype == 0x8033) {                        /* RGBA4444: u16, alpha=val&0xF, rgb=val&0xFFF0 */
      unsigned short *d = malloc((size_t)W * H * 2);
      if (d) { memcpy(d, data, (size_t)W * H * 2);
        for (int p = 0; p < 2; p++) for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
          size_t o = (size_t)y * W + x;  if ((d[o] & 0xF) >= 8) continue;
          int nx[4] = {x-1,x+1,x,x}, ny[4] = {y,y,y-1,y+1};
          for (int k = 0; k < 4; k++) { if (nx[k]<0||nx[k]>=W||ny[k]<0||ny[k]>=H) continue; size_t n = (size_t)ny[k]*W+nx[k];
            if ((d[n] & 0xF) >= 8) { d[o] = (unsigned short)((d[n] & 0xFFF0) | (d[o] & 0xF)); break; } } }
        if (conv) free(conv); conv = (unsigned char*)d; data = (unsigned char*)d; }
    } else if (utype == 0x8034) {                        /* RGBA5551: u16, alpha=val&1, rgb=val&0xFFFE */
      unsigned short *d = malloc((size_t)W * H * 2);
      if (d) { memcpy(d, data, (size_t)W * H * 2);
        for (int p = 0; p < 2; p++) for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
          size_t o = (size_t)y * W + x;  if (d[o] & 1) continue;
          int nx[4] = {x-1,x+1,x,x}, ny[4] = {y,y,y-1,y+1};
          for (int k = 0; k < 4; k++) { if (nx[k]<0||nx[k]>=W||ny[k]<0||ny[k]>=H) continue; size_t n = (size_t)ny[k]*W+nx[k];
            if (d[n] & 1) { d[o] = (unsigned short)((d[n] & 0xFFFE) | (d[o] & 1)); break; } } }
        if (conv) free(conv); conv = (unsigned char*)d; data = (unsigned char*)d; }
    }
  }
  int bpp = bpp_of(ufmt, utype);
  static int half_min = -1;
  if (half_min < 0) { const char *e = getenv("BULLY_TEX_HALF_MIN"); half_min = e ? atoi(e) : 1024; if (half_min < 256) half_min = 256; }
  if (g_tex_half && data && bpp > 0 && (w >= half_min || h >= half_min)) {
    /* tally p/ decidir halving seletivo (1x por id) */
    if (g_cur_tex2d < RESMAP && !g_half_seen[g_cur_tex2d]) {
      g_half_seen[g_cur_tex2d] = 1;
      long full = (long)w * h * bpp; int dim = (w > h) ? w : h;
      if (dim < 1024) { g_half512_cnt++; g_half512_full += full; }
      else { g_half1024_cnt++; g_half1024_full += full; }
    }
    int nw = w / 2, nh = h / 2;
    unsigned char *sm = (nw > 0 && nh > 0) ? malloc((size_t)nw * nh * bpp) : NULL;
    if (sm) {
      /* nearest (1 pixel a cada 2). NOTA: box-filter 2x2 foi testado e BUGOU a tela
       * (amacia o alpha das texturas de recorte/mascara -> bordas erradas). Mantido
       * nearest, que preserva valores exatos (alpha-test/cutout intactos). */
      for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++)
          memcpy(sm + ((size_t)y * nw + x) * bpp, data + ((size_t)(y*2) * w + x*2) * bpp, bpp);
      if (real_glTexImage2D) real_glTexImage2D(tgt, lvl, ifmt, nw, nh, bord, ufmt, utype, sm);
      free(sm); free(conv);
      /* TRILINEAR: a textura halved tb precisa de mipmap, senao trilinear amostra niveis
       * que nao existem -> PRETO (era por isso que os personagens (atlas >=768, halved)
       * ficavam pretos e o mundo (tiles <768, nao-halved) ficava nitido). [teste 2026-06-20] */
      if (lvl == 0 && bully_trilinear()) {
        if (is_cutout) {  /* recorte: LINEAR (sem mip = sem halo preto) */
          if (!real_glTexParameteri) real_glTexParameteri = dlsym(RTLD_DEFAULT, "glTexParameteri");
          if (real_glTexParameteri) real_glTexParameteri(tgt, 0x2801, 0x2601);
        } else {          /* opaca: trilinear+mip */
          if (!real_glGenerateMipmap) real_glGenerateMipmap = dlsym(RTLD_DEFAULT, "glGenerateMipmap");
          if (real_glGenerateMipmap) real_glGenerateMipmap(tgt);
        }
      }
      return;
    }
  }
  if (real_glTexImage2D) real_glTexImage2D(tgt, lvl, ifmt, w, h, bord, ufmt, utype, data);
  /* kmsdrm (G310): gera a cadeia de mipmap completa p/ o trilinear funcionar (sem
   * isso, MIN_FILTER mipmap -> textura preta). So no nivel 0 com dados reais. */
  if (lvl == 0 && data && ((bully_is_kmsdrm() && !bully_etc1_force()) || bully_trilinear())) {
    if (bully_trilinear() && is_cutout) {  /* recorte: LINEAR (sem mip = sem halo preto) */
      if (!real_glTexParameteri) real_glTexParameteri = dlsym(RTLD_DEFAULT, "glTexParameteri");
      if (real_glTexParameteri) real_glTexParameteri(tgt, 0x2801, 0x2601);
    } else {                                /* opaca (ou kmsdrm): trilinear+mip */
      if (!real_glGenerateMipmap) real_glGenerateMipmap = dlsym(RTLD_DEFAULT, "glGenerateMipmap");
      if (real_glGenerateMipmap) real_glGenerateMipmap(tgt);
    }
  }
  free(conv);
}

/* trace: conta os desenhos dentro de cada render-to-texture (roupa) */
long g_draws_screen = 0, g_draws_fbo = 0, g_clears_screen = 0;
void lcs_gl_report(void) {
  fprintf(stderr, "[glstats] draws screen=%ld fbo=%ld | clears screen=%ld\n",
          g_draws_screen, g_draws_fbo, g_clears_screen);
}
static void (*real_glDrawElements)(unsigned, int, unsigned, const void *) = NULL;
static void my_glDrawElements(unsigned mode, int count, unsigned type, const void *idx) {
  extern int g_in_fbo, g_rtt_draws; extern void bully_flush_pending_clear(void);
  extern unsigned long g_frame_no; extern long g_tex_live; extern long long g_texbytes_live;
  { static long c=0; gltrace("drawElements #%ld mode=0x%x count=%d fbo=%d\n", ++c, mode, count, g_in_fbo); }
  if (g_in_fbo) { bully_flush_pending_clear(); g_rtt_draws++; g_draws_fbo++; } else g_draws_screen++;
  drawhb("ENTER glDrawElements f=%lu fbo=%d screen=%ld fbod=%ld mode=0x%x count=%d type=0x%x idx=%p prog=%u tex=%u texN=%ld texMB=%lld\n",
         g_frame_no, g_in_fbo, g_draws_screen, g_draws_fbo, mode, count, type, idx,
         g_cur_program, g_cur_tex2d, g_tex_live, g_texbytes_live / (1024 * 1024));
  if (lcs_glstate_enabled()) {
    static int n = 0;
    if (!g_in_fbo && g_frame_no > 35 && count > 24 && n < 180) {
      fprintf(stderr,
              "[gldraw] f=%lu mode=0x%x count=%d type=0x%x idx=%p prog=%u tex0=%u unit=%u depth=%u dmask=%u color=%u%u%u%u blend=%u cull=%u cf=0x%x\n",
              g_frame_no, mode, count, type, idx, g_cur_program, g_cur_tex2d, g_active_tex_unit,
              g_cap_depth, g_depth_mask, g_color_mask[0], g_color_mask[1], g_color_mask[2], g_color_mask[3],
              g_cap_blend, g_cap_cull, g_cull_face);
      n++;
    }
  }
  if (!real_glDrawElements) real_glDrawElements = dlsym(RTLD_DEFAULT, "glDrawElements");
  if (real_glDrawElements) real_glDrawElements(mode, count, type, idx);
  drawhb("LEAVE glDrawElements f=%lu fbo=%d screen=%ld fbod=%ld mode=0x%x count=%d type=0x%x idx=%p prog=%u tex=%u texN=%ld texMB=%lld\n",
         g_frame_no, g_in_fbo, g_draws_screen, g_draws_fbo, mode, count, type, idx,
         g_cur_program, g_cur_tex2d, g_tex_live, g_texbytes_live / (1024 * 1024));
}
static void (*real_glDrawArrays)(unsigned, int, int) = NULL;
static void my_glDrawArrays(unsigned mode, int first, int count) {
  extern int g_in_fbo, g_rtt_draws; extern void bully_flush_pending_clear(void);
  extern unsigned long g_frame_no; extern long g_tex_live; extern long long g_texbytes_live;
  if (g_in_fbo) { bully_flush_pending_clear(); g_rtt_draws++; g_draws_fbo++; } else g_draws_screen++;
  drawhb("ENTER glDrawArrays f=%lu fbo=%d screen=%ld fbod=%ld mode=0x%x first=%d count=%d prog=%u tex=%u texN=%ld texMB=%lld\n",
         g_frame_no, g_in_fbo, g_draws_screen, g_draws_fbo, mode, first, count,
         g_cur_program, g_cur_tex2d, g_tex_live, g_texbytes_live / (1024 * 1024));
  if (!real_glDrawArrays) real_glDrawArrays = dlsym(RTLD_DEFAULT, "glDrawArrays");
  if (real_glDrawArrays) real_glDrawArrays(mode, first, count);
  drawhb("LEAVE glDrawArrays f=%lu fbo=%d screen=%ld fbod=%ld mode=0x%x first=%d count=%d prog=%u tex=%u texN=%ld texMB=%lld\n",
         g_frame_no, g_in_fbo, g_draws_screen, g_draws_fbo, mode, first, count,
         g_cur_program, g_cur_tex2d, g_tex_live, g_texbytes_live / (1024 * 1024));
}

void bully_imports_init(void) { ctype_init(); }

/* tabela de overrides (resolvida ANTES do fallback dlsym do so_resolve) */

/* KMSDRM: o eglSwapBuffers cru nao faz page-flip (so SDL_GL_SwapWindow faz).
 * fbdev (mali): mantem o raw (Amlogic-old intacto). */
extern void bully_swap_buffers(void);
extern int  bully_is_kmsdrm(void);
static unsigned (*real_eglSwapBuffers)(void*, void*) = NULL;
static unsigned my_eglSwapBuffers(void *dpy, void *surf) {
  drawhb("ENTER eglSwapBuffers f=%lu screen=%ld fbod=%ld texN=%ld texMB=%lld\n",
         g_frame_no, g_draws_screen, g_draws_fbo, g_tex_live,
         g_texbytes_live / (1024 * 1024));
  /* durante o bake: pinta a tela "Convertendo texturas (PT/EN/RU) + barra/contador"
   * por cima, ANTES do swap -> some o jogo cru/preto, fica um loading limpo. */
  { extern int bully_bake_active, bully_bake_cur, bully_bake_total; extern void bully_bake_ui(int, int);
    if (bully_bake_active) { static int n=0; if(n<3){fprintf(stderr,"[swap] via my_eglSwapBuffers (bake)\n");n++;} bully_bake_ui(bully_bake_cur, bully_bake_total); } }
  /* screenshot autoritativo: `touch /dev/shm/lcs_shot` -> glReadPixels do
   * backbuffer ANTES do swap (path real de render da engine). */
  if (access("/dev/shm/lcs_shot", F_OK) == 0) {
    unlink("/dev/shm/lcs_shot");
    int vp[4] = {0,0,0,0};
    void (*p_glGetIntegerv)(unsigned, int *) = dlsym(RTLD_DEFAULT, "glGetIntegerv");
    void (*p_glReadPixels)(int,int,int,int,unsigned,unsigned,void*) = dlsym(RTLD_DEFAULT, "glReadPixels");
    /* DIAG FBO: loga qual framebuffer esta ligado no swap (se !=0, a engine deixou
     * um render-target ligado -> glReadPixels leria o FBO, nao o backbuffer). */
    { int fbb = 0; if (p_glGetIntegerv) p_glGetIntegerv(0x8CA6 /*GL_FRAMEBUFFER_BINDING*/, &fbb);
      static int fl=0; if(fl<20){ fprintf(stderr,"[shot] FRAMEBUFFER_BINDING=%d no swap\n", fbb); fl++; } }
    if (p_glGetIntegerv && p_glReadPixels) {
      p_glGetIntegerv(0x0BA2 /*GL_VIEWPORT*/, vp);
      int w = vp[2] > 0 ? vp[2] : 1280, h = vp[3] > 0 ? vp[3] : 720;
      unsigned char *b = malloc((size_t)w*h*4);
      if (b) { p_glReadPixels(0,0,w,h,0x1908/*RGBA*/,0x1401/*UBYTE*/,b);
        long nz=0, sr=0,sg=0,sb=0, n=(long)w*h;
        for (long i=0;i<n;i++){ unsigned char*p=b+i*4; if(p[0]|p[1]|p[2])nz++; sr+=p[0];sg+=p[1];sb+=p[2]; }
        static long best_nz = -1;
        int keep_last = getenv("LCS_SHOT_KEEP_LAST") != NULL;
        int write_shot = keep_last || best_nz < 0 || nz > best_nz;
        if (write_shot) {
          best_nz = nz;
          FILE *o=fopen("/dev/shm/lcs_shot.raw","wb"); if(o){fwrite(b,1,(size_t)w*h*4,o);fclose(o);}
          FILE *t=fopen("/dev/shm/lcs_shot.txt","w"); if(t){fprintf(t,"%d %d\n",w,h);fclose(t);}
        }
        fprintf(stderr,"[shot] %dx%d nz=%ld avg=%ld,%ld,%ld%s\n",w,h,nz,sr/n,sg/n,sb/n,write_shot?" kept":" skipped");
        free(b); }
    }
  }
  /* 🔑 FIX "diamante preto SÓ na TV (não no glReadPixels)": a surface GL tem canal
   * ALPHA (egl_shim cria com alpha=8) e o compositor do Amlogic mistura o plano GL
   * com o fundo (preto) usando o alpha do framebuffer. Onde o chão é desenhado com
   * alpha < 1, o fundo preto VAZA por baixo -> "diamante". glReadPixels lê só RGB ->
   * não vê. Luz aditiva (farol/sol) escreve opaco -> tampa local. LightsMult (RGB)
   * não corrige (é alpha). FIX: forçar alpha=1 no buffer inteiro antes do swap =
   * plano opaco, sem vazamento. Default ON; LCS_NO_ALPHA_FIX desliga. */
  if (!getenv("LCS_NO_ALPHA_FIX")) {
    static void (*p_cmask)(unsigned char,unsigned char,unsigned char,unsigned char);
    static void (*p_ccol)(float,float,float,float);
    static void (*p_clear)(unsigned); static void (*p_dis)(unsigned); static int r=0;
    if (!r) { p_cmask=(void*)dlsym(RTLD_DEFAULT,"glColorMask"); p_ccol=(void*)dlsym(RTLD_DEFAULT,"glClearColor");
              p_clear=(void*)dlsym(RTLD_DEFAULT,"glClear"); p_dis=(void*)dlsym(RTLD_DEFAULT,"glDisable"); r=1; }
    if (p_cmask && p_ccol && p_clear) {
      if (p_dis) { p_dis(0x0C11 /*SCISSOR_TEST*/); p_dis(0x0BD0 /*DITHER*/); }
      p_cmask(0,0,0,1); p_ccol(0,0,0,1); p_clear(0x00004000 /*COLOR_BUFFER_BIT*/); p_cmask(1,1,1,1);
    }
  }
  if (bully_is_kmsdrm()) {
    bully_swap_buffers();
    drawhb("LEAVE eglSwapBuffers kms f=%lu screen=%ld fbod=%ld texN=%ld texMB=%lld\n",
           g_frame_no, g_draws_screen, g_draws_fbo, g_tex_live,
           g_texbytes_live / (1024 * 1024));
    return 1;
  }
  /* 🔑 VSYNC fbdev (plano B): o eglSwapInterval foi ignorado pelo mali fbdev ->
   * TEARING (pior dirigindo/rapido, quase nada andando). Esperar o VBLANK do
   * /dev/fb0 (FBIO_WAITFORVSYNC) ANTES de apresentar sincroniza o swap com o
   * refresh -> sem tearing. LCS_NO_VSYNC desliga. */
  if (!getenv("LCS_NO_VSYNC")) {
    static int fbfd = -2;
    if (fbfd == -2) { fbfd = open("/dev/fb0", O_RDWR);
      fprintf(stderr, "[vsync] /dev/fb0 fd=%d (FBIO_WAITFORVSYNC)\n", fbfd); }
    if (fbfd >= 0) { unsigned arg = 0; ioctl(fbfd, 0x4620 /*FBIO_WAITFORVSYNC*/, &arg); }
  }
  if (!real_eglSwapBuffers) real_eglSwapBuffers = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
  unsigned ret = real_eglSwapBuffers ? real_eglSwapBuffers(dpy, surf) : 1;
  drawhb("LEAVE eglSwapBuffers f=%lu ret=%u screen=%ld fbod=%ld texN=%ld texMB=%lld\n",
         g_frame_no, ret, g_draws_screen, g_draws_fbo, g_tex_live,
         g_texbytes_live / (1024 * 1024));
  return ret;
}

/* ==== INTERCEPTACAO EGL (LCS) ============================================
 * Diferente do Bully (que seeda OS_EGL globals), a engine da LCS cria a propria
 * surface DENTRO de viewOnSurfaceChanged (eglGetDisplay->eglInitialize->
 * eglChooseConfig->eglCreateWindowSurface->eglCreateContext->eglMakeCurrent).
 * O raw eglCreateWindowSurface(fbdev_window) da BAD_ALLOC no Mali (licao Bully).
 * Solucao: criamos o contexto via SDL2-mali (bully_init_gl) e devolvemos os
 * objetos do SDL pras chamadas EGL da engine. Tudo passa a usar o MESMO ctx. */
extern int bully_init_gl(void);
extern int bully_make_current(void);
#define EGL_OK 1
static void *my_eglGetDisplay(void *id) { (void)id; bully_init_gl();
  return eglGetCurrentDisplay(); }
static unsigned my_eglInitialize(void *dpy, int *maj, int *min) { (void)dpy;
  if (maj) *maj = 1; if (min) *min = 4; return EGL_OK; }
static unsigned my_eglTerminate(void *dpy) { (void)dpy; return EGL_OK; }
static unsigned my_eglChooseConfig(void *dpy, const int *attr, void **cfgs,
    int size, int *num) { (void)dpy; (void)attr;
  /* config dummy nao-nula; a engine so a repassa p/ create*Surface/Context que
   * tambem interceptamos. */
  if (cfgs && size > 0) cfgs[0] = (void *)0xC0FF16;
  if (num) *num = 1; return EGL_OK; }
static unsigned my_eglGetConfigAttrib(void *dpy, void *cfg, int attr, int *val) {
  (void)dpy; (void)cfg; if (val) *val = 0; return EGL_OK; }
static void *my_eglCreateWindowSurface(void *dpy, void *cfg, void *win,
    const int *attr) { (void)dpy; (void)cfg; (void)win; (void)attr;
  bully_init_gl(); return eglGetCurrentSurface(EGL_DRAW); }
static void *my_eglCreatePbufferSurface(void *dpy, void *cfg, const int *attr) {
  (void)dpy; (void)cfg; (void)attr; bully_init_gl();
  return eglGetCurrentSurface(EGL_DRAW); }
static unsigned my_eglDestroySurface(void *dpy, void *surf) { (void)dpy; (void)surf; return EGL_OK; }
static void *my_eglCreateContext(void *dpy, void *cfg, void *share,
    const int *attr) { (void)dpy; (void)cfg; (void)share; (void)attr;
  bully_init_gl(); return eglGetCurrentContext(); }
static unsigned my_eglDestroyContext(void *dpy, void *ctx) { (void)dpy; (void)ctx; return EGL_OK; }
static unsigned my_eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx) {
  (void)dpy; (void)draw; (void)read; (void)ctx; return bully_make_current(); }
static unsigned my_eglQuerySurface(void *dpy, void *surf, int attr, int *val) {
  (void)dpy; (void)surf;
  if (val) { if (attr == 0x3057) *val = bully_screen_w();      /* EGL_WIDTH */
             else if (attr == 0x3056) *val = bully_screen_h(); /* EGL_HEIGHT */
             else *val = 0; }
  return EGL_OK; }
static unsigned my_eglSwapInterval(void *dpy, int iv) { (void)dpy; (void)iv; return EGL_OK; }
static unsigned my_eglBindAPI(unsigned api) { (void)api; return EGL_OK; }
static int my_eglGetError(void) { return 0x3000; /* EGL_SUCCESS */ }
static void (*real_glFlush_diag)(void) = NULL;
static void my_glFlush(void) {
  if (!real_glFlush_diag) real_glFlush_diag = dlsym(RTLD_DEFAULT, "glFlush");
  drawhb("ENTER glFlush f=%lu screen=%ld fbod=%ld texN=%ld texMB=%lld\n",
         g_frame_no, g_draws_screen, g_draws_fbo, g_tex_live,
         g_texbytes_live / (1024 * 1024));
  if (real_glFlush_diag) real_glFlush_diag();
  drawhb("LEAVE glFlush f=%lu screen=%ld fbod=%ld texN=%ld texMB=%lld\n",
         g_frame_no, g_draws_screen, g_draws_fbo, g_tex_live,
         g_texbytes_live / (1024 * 1024));
}

/* glFinish TRAVA o Mali-450 (Utgard) na fase de loading (lição do Bully) ->
 * no-op (ou glFlush leve). Causa do wedge no GameStart/transicao pro jogo. */
static void my_glFinish(void) {
  drawhb("ENTER glFinish f=%lu real=%d screen=%ld fbod=%ld texN=%ld texMB=%lld\n",
         g_frame_no, getenv("LCS_REALFINISH") ? 1 : 0, g_draws_screen, g_draws_fbo,
         g_tex_live, g_texbytes_live / (1024 * 1024));
  if (getenv("LCS_REALFINISH")) {
    void (*r)(void) = dlsym(RTLD_DEFAULT, "glFinish");
    if (r) r();
    drawhb("LEAVE glFinish real f=%lu screen=%ld fbod=%ld texN=%ld texMB=%lld\n",
           g_frame_no, g_draws_screen, g_draws_fbo, g_tex_live,
           g_texbytes_live / (1024 * 1024));
    return;
  }
  my_glFlush();
  drawhb("LEAVE glFinish flush f=%lu screen=%ld fbod=%ld texN=%ld texMB=%lld\n",
         g_frame_no, g_draws_screen, g_draws_fbo, g_tex_live,
         g_texbytes_live / (1024 * 1024));
}

/* VAO / MRT: o libGame importa as versoes CORE (glGenVertexArrays etc + glDrawBuffers)
 * que existem no Mali-G31 (GLES3.2) mas NAO como simbolo no Mali-450 Utgard (GLES2)
 * -> ficavam *** UNRESOLVED ***. Resolvemos via eglGetProcAddress tentando o CORE e
 * depois a EXTENSAO (OES p/ VAO, EXT p/ draw_buffers); no-op seguro se o device nao
 * tiver nenhuma (hoje o jogo nao chama esse caminho no GLES2 -> nunca executa, mas o
 * no-op evita slot invalido caso chame). No X5M/GLES3 pega a core real (VAO de verdade). */
static void *gl_proc2(const char *core, const char *ext) {
  void *(*gpa)(const char *) = dlsym(RTLD_DEFAULT, "eglGetProcAddress");
  void *p = gpa ? gpa(core) : NULL;
  if (!p && gpa && ext) p = gpa(ext);
  if (!p) p = dlsym(RTLD_DEFAULT, core);
  if (!p && ext) p = dlsym(RTLD_DEFAULT, ext);
  return p;
}
static void (*r_glGenVAO)(int, unsigned *) = NULL;
static void my_glGenVertexArrays(int n, unsigned *a) {
  if (!r_glGenVAO) r_glGenVAO = gl_proc2("glGenVertexArrays", "glGenVertexArraysOES");
  if (r_glGenVAO) r_glGenVAO(n, a);
  else if (a) for (int i = 0; i < n; i++) a[i] = 0;
}
static void (*r_glBindVAO)(unsigned) = NULL;
static void my_glBindVertexArray(unsigned a) {
  if (!r_glBindVAO) r_glBindVAO = gl_proc2("glBindVertexArray", "glBindVertexArrayOES");
  if (r_glBindVAO) r_glBindVAO(a);
}
static void (*r_glDelVAO)(int, const unsigned *) = NULL;
static void my_glDeleteVertexArrays(int n, const unsigned *a) {
  if (!r_glDelVAO) r_glDelVAO = gl_proc2("glDeleteVertexArrays", "glDeleteVertexArraysOES");
  if (r_glDelVAO) r_glDelVAO(n, a);
}
static void (*r_glDrawBuffers)(int, const unsigned *) = NULL;
static void my_glDrawBuffers(int n, const unsigned *b) {
  if (!r_glDrawBuffers) r_glDrawBuffers = gl_proc2("glDrawBuffers", "glDrawBuffersEXT");
  if (r_glDrawBuffers) r_glDrawBuffers(n, b);
}

/* __stack_chk_fail neutralizado (insurance): com o TLS pad do main.c a canary
 * bionic ja fica estavel e isto nunca dispara; mas se um path nao-coberto ler
 * tpidr+0x28 instavel, melhor logar do que abortar o jogo. */
static void b_stack_chk_fail(void) {
  static int n = 0;
  if (n++ < 3) fprintf(stderr, "[stack_chk_fail] FALSO-POSITIVO TLS ignorado\n");
}

DynLibFunction bully_stub_table[] = {
  {"__stack_chk_fail", (uintptr_t)b_stack_chk_fail},
  {"eglSwapBuffers", (uintptr_t)my_eglSwapBuffers},
  /* LCS: a engine cria a propria EGL surface -> interceptamos tudo p/ o ctx SDL2-mali */
  {"eglGetDisplay", (uintptr_t)my_eglGetDisplay},
  {"eglInitialize", (uintptr_t)my_eglInitialize},
  {"eglTerminate", (uintptr_t)my_eglTerminate},
  {"eglChooseConfig", (uintptr_t)my_eglChooseConfig},
  {"eglGetConfigAttrib", (uintptr_t)my_eglGetConfigAttrib},
  {"eglCreateWindowSurface", (uintptr_t)my_eglCreateWindowSurface},
  {"eglCreatePbufferSurface", (uintptr_t)my_eglCreatePbufferSurface},
  {"eglDestroySurface", (uintptr_t)my_eglDestroySurface},
  {"eglCreateContext", (uintptr_t)my_eglCreateContext},
  {"eglDestroyContext", (uintptr_t)my_eglDestroyContext},
  {"eglMakeCurrent", (uintptr_t)my_eglMakeCurrent},
  {"eglQuerySurface", (uintptr_t)my_eglQuerySurface},
  {"eglSwapInterval", (uintptr_t)my_eglSwapInterval},
  {"eglBindAPI", (uintptr_t)my_eglBindAPI},
  {"eglGetError", (uintptr_t)my_eglGetError},
  {"glFlush", (uintptr_t)my_glFlush},
  {"glFinish", (uintptr_t)my_glFinish},
  {"OS_GamepadIsConnected", (uintptr_t)LCS_OS_GamepadIsConnected},
  {"OS_GamepadAxis", (uintptr_t)LCS_OS_GamepadAxis},
  {"__errno", (uintptr_t)bionic___errno}, {"__assert2", (uintptr_t)b_assert2},
  {"__strlen_chk", (uintptr_t)b_strlen_chk}, {"__strrchr_chk", (uintptr_t)b_strrchr_chk},
  {"__strchr_chk", (uintptr_t)b_strchr_chk}, {"__strncpy_chk2", (uintptr_t)b_strncpy_chk2},
  {"__android_log_print", (uintptr_t)b_android_log},
  {"android_set_abort_message", (uintptr_t)b_set_abort_message},
  {"__system_property_get", (uintptr_t)b_system_property_get},
  {"__sF", (uintptr_t)bionic_sF},
  {"fprintf", (uintptr_t)w_fprintf}, {"vfprintf", (uintptr_t)w_vfprintf}, {"fwrite", (uintptr_t)w_fwrite},
  {"fputs", (uintptr_t)w_fputs}, {"fputc", (uintptr_t)w_fputc}, {"fflush", (uintptr_t)w_fflush},
  {"_ctype_", (uintptr_t)&bionic_ctype_ptr},
  {"ANativeWindow_fromSurface", (uintptr_t)aw_fromSurface},
  {"ANativeWindow_setBuffersGeometry", (uintptr_t)aw_setBuffersGeometry},
  {"ANativeWindow_getWidth", (uintptr_t)aw_getWidth}, {"ANativeWindow_getHeight", (uintptr_t)aw_getHeight},
  {"ANativeWindow_release", (uintptr_t)aw_release},
  {"AAssetManager_fromJava", (uintptr_t)am_fromJava}, {"AAssetManager_open", (uintptr_t)aa_open},
  {"AAsset_read", (uintptr_t)aa_read}, {"AAsset_seek64", (uintptr_t)aa_seek64},
  {"AAsset_getBuffer", (uintptr_t)aa_getBuffer},
  {"AAsset_getLength64", (uintptr_t)aa_getLength64}, {"AAsset_getRemainingLength64", (uintptr_t)aa_getRemainingLength64},
  {"AAsset_close", (uintptr_t)aa_close},
  /* LCS: variantes 32-bit (mesma impl) + log/FD que o Bully nao importava */
  {"AAsset_seek", (uintptr_t)aa_seek64},
  {"AAsset_getLength", (uintptr_t)aa_getLength64},
  {"AAsset_getRemainingLength", (uintptr_t)aa_getRemainingLength64},
  {"__android_log_write", (uintptr_t)b_android_log_write},
  {"__android_log_vprint", (uintptr_t)b_android_log_vprint},
  {"__android_log_assert", (uintptr_t)b_android_log_assert},
  {"__FD_SET_chk", (uintptr_t)b_fd_set_chk},
  {"__FD_ISSET_chk", (uintptr_t)b_fd_isset_chk},
  {"glGetString", (uintptr_t)w_glGetString},
  {"glShaderSource", (uintptr_t)my_glShaderSource},
  {"glTexParameteri", (uintptr_t)my_glTexParameteri},
  {"glTexImage2D", (uintptr_t)my_glTexImage2D},
  {"glClear", (uintptr_t)my_glClear},
  {"glClearColor", (uintptr_t)my_glClearColor},
  {"glUseProgram", (uintptr_t)my_glUseProgram},
  {"glActiveTexture", (uintptr_t)my_glActiveTexture},
  {"glDepthMask", (uintptr_t)my_glDepthMask},
  {"glColorMask", (uintptr_t)my_glColorMask},
  {"glCullFace", (uintptr_t)my_glCullFace},
  {"glCompileShader", (uintptr_t)my_glCompileShader},
  {"glLinkProgram", (uintptr_t)my_glLinkProgram},
  {"glCompressedTexImage2D", (uintptr_t)my_glCompressedTexImage2D},
  {"glEnable", (uintptr_t)my_glEnable},
  {"glDisable", (uintptr_t)my_glDisable},
  {"glTexStorage2D", (uintptr_t)my_glTexStorage2D},
  {"glTexSubImage2D", (uintptr_t)my_glTexSubImage2D},
  /* leak-track (medicao do vazamento de recursos GL) */
  {"glBindTexture", (uintptr_t)my_glBindTexture},
  {"glGenTextures", (uintptr_t)my_glGenTextures},
  {"glDeleteTextures", (uintptr_t)my_glDeleteTextures},
  {"glGenFramebuffers", (uintptr_t)my_glGenFramebuffers},
  {"glDeleteFramebuffers", (uintptr_t)my_glDeleteFramebuffers},
  {"glGenRenderbuffers", (uintptr_t)my_glGenRenderbuffers},
  {"glDeleteRenderbuffers", (uintptr_t)my_glDeleteRenderbuffers},
  {"glBindRenderbuffer", (uintptr_t)my_glBindRenderbuffer},
  {"glRenderbufferStorage", (uintptr_t)my_glRenderbufferStorage},
  {"glCheckFramebufferStatus", (uintptr_t)my_glCheckFramebufferStatus},
  {"glBindFramebuffer", (uintptr_t)my_glBindFramebuffer},
  {"glReadPixels", (uintptr_t)my_glReadPixels},
  {"glFramebufferTexture2D", (uintptr_t)my_glFramebufferTexture2D},
  {"glDrawElements", (uintptr_t)my_glDrawElements},
  {"glDrawArrays", (uintptr_t)my_glDrawArrays},
  /* VAO/MRT: core no Mali-G31(GLES3), via OES/EXT no Utgard, no-op se nao tiver */
  {"glGenVertexArrays", (uintptr_t)my_glGenVertexArrays},
  {"glBindVertexArray", (uintptr_t)my_glBindVertexArray},
  {"glDeleteVertexArrays", (uintptr_t)my_glDeleteVertexArrays},
  {"glDrawBuffers", (uintptr_t)my_glDrawBuffers},
  {"fopen", (uintptr_t)w_fopen},
  /* stat: ausentes como simbolo em glibc<2.33 -> via syscall (texto/fontes) */
  {"stat", (uintptr_t)my_stat}, {"lstat", (uintptr_t)my_lstat},
  {"fstat", (uintptr_t)my_fstat}, {"fstatat", (uintptr_t)my_fstatat},
  {"stat64", (uintptr_t)my_stat}, {"lstat64", (uintptr_t)my_lstat},
  {"fstat64", (uintptr_t)my_fstat}, {"fstatat64", (uintptr_t)my_fstatat},
  {"_ZTH7gString", (uintptr_t)tl_noop}, {"_ZTH8gString2", (uintptr_t)tl_noop},
  {"_ZTHN10ALCcontext13sLocalContextE", (uintptr_t)tl_noop},
  {"_Z24NVThreadGetCurrentJNIEnvv", (uintptr_t)NVThreadGetCurrentJNIEnv},
};
const int bully_stub_count = sizeof(bully_stub_table) / sizeof(bully_stub_table[0]);
