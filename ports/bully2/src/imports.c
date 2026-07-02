/* imports.c -- Bully2 original-first bionic/NDK shims.
 *
 * This file is intentionally narrow. It resolves Android/Bionic and Mali-450
 * GLES2 compatibility gaps. Texture half-res is an opt-in test path and never
 * writes converted assets or cache.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

volatile long g_asset_bytes_frame = 0;

static int env_enabled(const char *name) {
  const char *e = getenv(name);
  return e && strcmp(e, "0") != 0;
}

static const char *first_env(const char *a, const char *b) {
  const char *e = getenv(a);
  if (e && *e)
    return e;
  e = getenv(b);
  return (e && *e) ? e : NULL;
}

static int read_first_token(const char *path, char *buf, size_t len) {
  if (!path || !*path || !len)
    return 0;
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  size_t n = fread(buf, 1, len - 1, f);
  fclose(f);
  buf[n] = 0;
  for (size_t i = 0; i < n; i++) {
    if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == '\t')
      buf[i] = ' ';
  }
  while (*buf == ' ')
    memmove(buf, buf + 1, strlen(buf));
  return buf[0] != 0;
}

static int parse_start_texture_profile(const char *e, int *half, int *min_dim,
                                       const char **label) {
  if (!e || !*e)
    return 0;
  while (*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r')
    e++;
  if (!*e)
    return 0;
  if (!strncasecmp(e, "low", 3) || !strncasecmp(e, "256", 3) ||
      !strncasecmp(e, "extreme", 7)) {
    *half = 1;
    *min_dim = 256;
    *label = "low";
    return 1;
  }
  if (!strncasecmp(e, "medium", 6) || !strncasecmp(e, "med", 3) ||
      !strncasecmp(e, "512", 3)) {
    *half = 1;
    *min_dim = 512;
    *label = "medium";
    return 1;
  }
  if (!strncasecmp(e, "high", 4) || !strncasecmp(e, "full", 4) ||
      !strncasecmp(e, "native", 6) || !strncasecmp(e, "off", 3) ||
      !strcmp(e, "0")) {
    *half = 0;
    *min_dim = 1024;
    *label = "high";
    return 1;
  }
  return 0;
}

static int io_log_enabled(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = env_enabled("BULLY2_IOLOG") ? 1 : 0;
  return enabled;
}

typedef struct {
  unsigned target;
  unsigned ifmt;
  int w;
  int h;
  int orig_w;
  int orig_h;
  int half_storage;
  unsigned char cutout;   /* 1 = textura de recorte (alpha vazado): NUNCA mipmap (halo preto) */
  unsigned char has_mips; /* 1 = geramos glGenerateMipmap nesta base -> pode LINEAR_MIPMAP_LINEAR */
  uint32_t level_bytes[16];
  uint32_t total;
  unsigned alive;
} TexInfo;

static TexInfo *g_texinfo;
static size_t g_texinfo_cap;
static unsigned g_bound_2d;
static unsigned g_bound_cube;
static long long g_gltex_live_bytes;
static long long g_gltex_peak_bytes;
static long g_gltex_gen;
static long g_gltex_del;
static long g_gltex_upload;
static long g_gltex_half_upload;
static long long g_gltex_half_saved_bytes;
static unsigned g_shadow_bound_fbo;
static unsigned g_shadow_bound_rbo;
static unsigned g_shadow_active_texture = 0x84C0; /* GL_TEXTURE0 */

#define GLTRACE_LINES 96
#define GLTRACE_LEN 160
static char g_gltrace[GLTRACE_LINES][GLTRACE_LEN];
static unsigned g_gltrace_seq;

static int glmem_log_enabled(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = env_enabled("BULLY2_GLMEMLOG") ? 1 : 0;
  return enabled;
}

static int shadow_gl_log_enabled(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = env_enabled("BULLY2_SHADOWLOG") ? 1 : 0;
  return enabled;
}

/* diagnostico de sombra gated por env, cacheado (evita getenv por-draw em
 * device fraco): rastreio do passo de resolve + readback do FBO de resolve. */
static int resolvedraw_enabled(void) {
  static int e = -1;
  if (e < 0) e = getenv("BULLY2_RESOLVEDRAW") ? 1 : 0;
  return e;
}
static int resolvedump_enabled(void) {
  static int e = -1;
  if (e < 0) e = getenv("BULLY2_RESOLVEDUMP") ? 1 : 0;
  return e;
}

void bully_gltrace(const char *fmt, ...) {
  if (!shadow_gl_log_enabled())
    return;
  unsigned slot = __atomic_fetch_add(&g_gltrace_seq, 1, __ATOMIC_RELAXED) %
                  GLTRACE_LINES;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_gltrace[slot], GLTRACE_LEN, fmt, ap);
  va_end(ap);
}

void bully_gltrace_dump(FILE *out) {
  if (!out || !shadow_gl_log_enabled())
    return;
  unsigned end = __atomic_load_n(&g_gltrace_seq, __ATOMIC_RELAXED);
  unsigned start = end > GLTRACE_LINES ? end - GLTRACE_LINES : 0;
  fprintf(out, "[gltrace] last %u GL ops:\n", end - start);
  for (unsigned i = start; i < end; i++) {
    const char *line = g_gltrace[i % GLTRACE_LINES];
    if (line[0])
      fprintf(out, "[gltrace] %06u %s\n", i, line);
  }
}

static int drawbuffers_safe_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("BULLY2_DRAWBUFFERS_SAFE");
    if (e && *e)
      enabled = strcmp(e, "0") != 0;
    else
      enabled = access("/sys/module/mali/version", F_OK) == 0;
  }
  return enabled;
}

static int tex_ensure(unsigned id) {
  if (id == 0)
    return 0;
  if (id < g_texinfo_cap)
    return 1;
  size_t cap = g_texinfo_cap ? g_texinfo_cap : 1024;
  while (id >= cap)
    cap *= 2;
  TexInfo *n = realloc(g_texinfo, cap * sizeof(*g_texinfo));
  if (!n)
    return 0;
  memset(n + g_texinfo_cap, 0, (cap - g_texinfo_cap) * sizeof(*n));
  g_texinfo = n;
  g_texinfo_cap = cap;
  return 1;
}

static int target_is_cube_face(unsigned target) {
  return target >= 0x8515 && target <= 0x851A;
}

static unsigned bound_tex_for_target(unsigned target) {
  if (target == 0x0DE1)
    return g_bound_2d;
  if (target == 0x8513 || target_is_cube_face(target))
    return g_bound_cube;
  return 0;
}

static int is_pot(int x) { return x > 0 && (x & (x - 1)) == 0; }

static void tex_mark_cutout(unsigned id, int cut) {
  if (id && id < g_texinfo_cap)
    g_texinfo[id].cutout = cut ? 1 : 0;
}
static int tex_bound_2d_cutout(void) {
  unsigned id = g_bound_2d;
  return (id && id < g_texinfo_cap) ? g_texinfo[id].cutout : 0;
}
static void tex_mark_has_mips(unsigned id, int v) {
  if (id && id < g_texinfo_cap)
    g_texinfo[id].has_mips = v ? 1 : 0;
}
static int tex_bound_2d_has_mips(void) {
  unsigned id = g_bound_2d;
  return (id && id < g_texinfo_cap) ? g_texinfo[id].has_mips : 0;
}

static int bytes_per_pixel(unsigned ifmt, unsigned fmt, unsigned type) {
  (void)fmt;
  switch (type) {
  case 0x8363: /* GL_UNSIGNED_SHORT_5_6_5 */
  case 0x8033: /* GL_UNSIGNED_SHORT_4_4_4_4 */
  case 0x8034: /* GL_UNSIGNED_SHORT_5_5_5_1 */
    return 2;
  default:
    break;
  }

  switch (ifmt) {
  case 0x1908: /* GL_RGBA */
  case 0x8058: /* GL_RGBA8 */
    return 4;
  case 0x1907: /* GL_RGB */
  case 0x8051: /* GL_RGB8 */
    return 3;
  case 0x1909: /* GL_LUMINANCE */
  case 0x1906: /* GL_ALPHA */
    return 1;
  case 0x190A: /* GL_LUMINANCE_ALPHA */
  case 0x8D62: /* GL_RGB565 */
  case 0x8056: /* GL_RGBA4 */
  case 0x8057: /* GL_RGB5_A1 */
    return 2;
  default:
    return 4;
  }
}

static uint32_t tex_level_size(int w, int h, int bpp) {
  if (w <= 0 || h <= 0 || bpp <= 0)
    return 0;
  long long bytes = (long long)w * (long long)h * bpp;
  if (bytes < 0)
    return 0;
  if (bytes > 0x7fffffffLL)
    return 0x7fffffffU;
  return (uint32_t)bytes;
}

static volatile int g_tex_half_runtime = -1;
static volatile int g_tex_half_min_runtime = -1;

static void tex_runtime_init(void) {
  if (__atomic_load_n(&g_tex_half_runtime, __ATOMIC_RELAXED) >= 0)
    return;

  /* PADRAO DE INSTALACAO LIMPA = LOW (min_dim 256): da folga em devices 1GB.
   * Perfil salvo (texture_profile.cfg) e BULLY2_TEXTURE_PROFILE ainda tem
   * prioridade; Medium/High continuam disponiveis pelo menu. */
  int enabled = 1;
  int min_dim = 256;
  const char *label = "low";
  const char *half_env = first_env("BULLY2_TEX_HALF", "BULLY_TEX_HALF");
  const char *min_env = first_env("BULLY2_TEX_HALF_MIN", "BULLY_TEX_HALF_MIN");

  if (half_env) {
    enabled = strcmp(half_env, "0") != 0;
    min_dim = min_env ? atoi(min_env) : 1024;
    label = enabled ? "custom-half" : "high";
  } else {
    char file_profile[32];
    const char *profile = first_env("BULLY2_TEXTURE_PROFILE",
                                    "BULLY_TEXTURE_PROFILE");
    if (!profile)
      profile = first_env("BULLY2_TEX_HALF_MODE", "BULLY_TEX_HALF_MODE");
    if (!profile) {
      const char *path = first_env("BULLY2_TEX_PROFILE_SAVE",
                                   "BULLY_TEX_PROFILE_SAVE");
      if (!path || !*path)
        path = "texture_profile.cfg";
      if (read_first_token(path, file_profile, sizeof(file_profile)))
        profile = file_profile;
    }
    if (profile && !parse_start_texture_profile(profile, &enabled, &min_dim,
                                                &label)) {
      fprintf(stderr, "[tex] unsupported startup profile=%s; using low\n",
              profile);
      enabled = 1;
      min_dim = 256;
      label = "low";
    }
  }
  if (min_dim < 256)
    min_dim = 256;

  __atomic_store_n(&g_tex_half_min_runtime, min_dim, __ATOMIC_RELAXED);
  __atomic_store_n(&g_tex_half_runtime, enabled, __ATOMIC_RELEASE);
  fprintf(stderr, "[tex] startup profile=%s half=%d min=%d\n", label, enabled,
          min_dim);
}

static int tex_half_enabled(void) {
  tex_runtime_init();
  return __atomic_load_n(&g_tex_half_runtime, __ATOMIC_ACQUIRE) > 0;
}

static int tex_half_min_dim(void) {
  tex_runtime_init();
  int min_dim = __atomic_load_n(&g_tex_half_min_runtime, __ATOMIC_ACQUIRE);
  return min_dim < 256 ? 256 : min_dim;
}

void bully_tex_set_runtime_profile(int half, int min_dim, const char *why) {
  tex_runtime_init();
  if (min_dim < 256)
    min_dim = 256;
  __atomic_store_n(&g_tex_half_min_runtime, min_dim, __ATOMIC_RELEASE);
  __atomic_store_n(&g_tex_half_runtime, half ? 1 : 0, __ATOMIC_RELEASE);
  fprintf(stderr, "[texprofile] %s half=%d min=%d\n",
          why ? why : "runtime", half ? 1 : 0, min_dim);
}

int bully_tex_runtime_half_enabled(void) {
  return tex_half_enabled();
}

int bully_tex_runtime_half_min_dim(void) {
  return tex_half_min_dim();
}

static int tex_half_type_supported(unsigned fmt, unsigned type, int bpp) {
  if (bpp <= 0)
    return 0;
  if (type == 0x1401) { /* GL_UNSIGNED_BYTE */
    return fmt == 0x1908 || fmt == 0x1907 || fmt == 0x1909 ||
           fmt == 0x1906 || fmt == 0x190A;
  }
  return bpp == 2 &&
         (type == 0x8363 || type == 0x8033 || type == 0x8034);
}

static void *half_pixels_nearest(const void *src, int w, int h, int bpp,
                                 int *out_w, int *out_h) {
  if (!src || w < 2 || h < 2 || bpp <= 0)
    return NULL;
  int hw = w / 2;
  int hh = h / 2;
  if (hw < 1 || hh < 1)
    return NULL;
  size_t out_size = (size_t)hw * (size_t)hh * (size_t)bpp;
  if (out_size == 0 || out_size > 128 * 1024 * 1024)
    return NULL;
  unsigned char *out = malloc(out_size);
  if (!out)
    return NULL;
  const unsigned char *in = src;
  for (int y = 0; y < hh; y++) {
    for (int x = 0; x < hw; x++) {
      memcpy(out + ((size_t)y * hw + x) * bpp,
             in + ((size_t)(y * 2) * w + x * 2) * bpp, (size_t)bpp);
    }
  }
  *out_w = hw;
  *out_h = hh;
  return out;
}

static void tex_set_level(unsigned id, unsigned target, int level, unsigned ifmt,
                          int w, int h, uint32_t bytes) {
  if (!id || level < 0 || level >= 16 || !tex_ensure(id))
    return;
  TexInfo *t = &g_texinfo[id];
  if (!t->alive) {
    t->alive = 1;
    t->target = target;
  }
  long long delta = (long long)bytes - (long long)t->level_bytes[level];
  t->level_bytes[level] = bytes;
  t->total = (uint32_t)((long long)t->total + delta);
  t->ifmt = ifmt;
  if (level == 0) {
    t->w = w;
    t->h = h;
  }
  g_gltex_live_bytes += delta;
  if (g_gltex_live_bytes > g_gltex_peak_bytes)
    g_gltex_peak_bytes = g_gltex_live_bytes;
  g_gltex_upload++;
}

static void tex_set_storage(unsigned id, unsigned target, int levels,
                            unsigned ifmt, int w, int h) {
  int bpp = bytes_per_pixel(ifmt, ifmt, 0x1401);
  if (levels <= 0)
    levels = 1;
  if (levels > 16)
    levels = 16;
  for (int level = 0; level < levels; level++) {
    int lw = w >> level;
    int lh = h >> level;
    if (lw < 1)
      lw = 1;
    if (lh < 1)
      lh = 1;
    tex_set_level(id, target, level, ifmt, lw, lh, tex_level_size(lw, lh, bpp));
  }
}

static void tex_delete_id(unsigned id) {
  if (!id || id >= g_texinfo_cap)
    return;
  TexInfo *t = &g_texinfo[id];
  if (!t->alive)
    return;
  g_gltex_live_bytes -= t->total;
  if (g_gltex_live_bytes < 0)
    g_gltex_live_bytes = 0;
  memset(t, 0, sizeof(*t));
  g_gltex_del++;
}

static void tex_delete_bound(unsigned id) {
  if (g_bound_2d == id)
    g_bound_2d = 0;
  if (g_bound_cube == id)
    g_bound_cube = 0;
  tex_delete_id(id);
}

long long bully_glmem_live_bytes(void) { return g_gltex_live_bytes; }
long long bully_glmem_peak_bytes(void) { return g_gltex_peak_bytes; }
long bully_glmem_gen_count(void) { return g_gltex_gen; }
long bully_glmem_del_count(void) { return g_gltex_del; }
long bully_glmem_upload_count(void) { return g_gltex_upload; }
long bully_glmem_half_upload_count(void) { return g_gltex_half_upload; }
long long bully_glmem_half_saved_bytes(void) { return g_gltex_half_saved_bytes; }

void bully_glmem_report(const char *why) {
  if (!glmem_log_enabled())
    return;
  fprintf(stderr,
          "[glmem] %s live=%lld MB peak=%lld MB gen=%ld del=%ld upload=%ld "
          "half=%ld saved=%lld MB bound2d=%u boundcube=%u\n",
          why ? why : "report", g_gltex_live_bytes / (1024 * 1024),
          g_gltex_peak_bytes / (1024 * 1024), g_gltex_gen, g_gltex_del,
          g_gltex_upload, g_gltex_half_upload,
          g_gltex_half_saved_bytes / (1024 * 1024), g_bound_2d, g_bound_cube);
}

static int *bionic___errno(void) {
  extern int *__errno_location(void);
  return __errno_location();
}

static size_t b_strlen_chk(const char *s, size_t n) {
  (void)n;
  return strlen(s);
}

static char *b_strrchr_chk(const char *s, int c, size_t n) {
  (void)n;
  return strrchr(s, c);
}

static char *b_strchr_chk(const char *s, int c, size_t n) {
  (void)n;
  return strchr(s, c);
}

static char *b_strncpy_chk2(char *d, const char *s, size_t n, size_t dn,
                            size_t sn) {
  (void)dn;
  (void)sn;
  return strncpy(d, s, n);
}

static void b_assert2(const char *f, int l, const char *fn, const char *e) {
  fprintf(stderr, "assert: %s:%d %s: %s\n", f ? f : "?", l, fn ? fn : "?",
          e ? e : "?");
  abort();
}

static int b_android_log(int prio, const char *tag, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[ALOG:%d %s] ", prio, tag ? tag : "?");
  vfprintf(stderr, fmt ? fmt : "", ap);
  fprintf(stderr, "\n");
  va_end(ap);
  return 0;
}

static void b_set_abort_message(const char *m) {
  fprintf(stderr, "[abort_msg] %s\n", m ? m : "?");
}

static int b_system_property_get(const char *name, char *value) {
  if (!value)
    return 0;
  value[0] = 0;
  if (!name)
    return 0;

  const char *e = NULL;
  if (!strcmp(name, "ro.product.model"))
    e = getenv("BULLY2_DEV_MODEL");
  else if (!strcmp(name, "ro.product.manufacturer"))
    e = getenv("BULLY2_DEV_MANUF");
  else if (!strcmp(name, "ro.board.platform") || !strcmp(name, "ro.hardware"))
    e = getenv("BULLY2_DEV_BOARD");

  if (e && *e) {
    strncpy(value, e, 91);
    value[91] = 0;
    return (int)strlen(value);
  }
  return 0;
}

static void b_stack_chk_fail(void) {
  fprintf(stderr, "[stack_chk_fail] ignored after bionic TLS guard bridge\n");
}

static char bionic_sF[3][512];
static FILE *map_sF(void *fp) {
  if (fp == (void *)&bionic_sF[0])
    return stdin;
  if (fp == (void *)&bionic_sF[1])
    return stdout;
  if (fp == (void *)&bionic_sF[2])
    return stderr;
  return (FILE *)fp;
}

static int w_fprintf(void *fp, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vfprintf(map_sF(fp), fmt, ap);
  va_end(ap);
  return r;
}

static int w_vfprintf(void *fp, const char *fmt, va_list ap) {
  return vfprintf(map_sF(fp), fmt, ap);
}

static size_t w_fwrite(const void *p, size_t s, size_t n, void *fp) {
  return fwrite(p, s, n, map_sF(fp));
}

static int w_fputs(const char *str, void *fp) {
  return fputs(str, map_sF(fp));
}

static int w_fputc(int c, void *fp) {
  return fputc(c, map_sF(fp));
}

static int w_fflush(void *fp) {
  return fflush(fp ? map_sF(fp) : NULL);
}

static unsigned char ctype_tab[1 + 256];
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
    if (isupper(c))
      f |= _CT_U;
    if (islower(c))
      f |= _CT_L;
    if (isdigit(c))
      f |= _CT_N;
    if (isspace(c))
      f |= _CT_S;
    if (ispunct(c))
      f |= _CT_P;
    if (iscntrl(c))
      f |= _CT_C;
    if (isxdigit(c))
      f |= _CT_X;
    if (c == ' ')
      f |= _CT_B;
    ctype_tab[1 + c] = f;
  }
}

extern int bully_screen_w(void);
extern int bully_screen_h(void);

static void *aw_fromSurface(void *env, void *surface) {
  (void)env;
  (void)surface;
  return (void *)0xB211FACE;
}

static int aw_setBuffersGeometry(void *w, int x, int y, int f) {
  (void)w;
  (void)x;
  (void)y;
  (void)f;
  return 0;
}

static int aw_getWidth(void *w) {
  (void)w;
  return bully_screen_w();
}

static int aw_getHeight(void *w) {
  (void)w;
  return bully_screen_h();
}

static void aw_release(void *w) {
  (void)w;
}

#ifndef ASSET_DIR
#define ASSET_DIR "assets"
#endif

typedef struct {
  FILE *fp;
  long len;
} AAsset;

static void *am_fromJava(void *env, void *obj) {
  (void)env;
  (void)obj;
  return (void *)0xA55E7;
}

static void *aa_open(void *mgr, const char *path, int mode) {
  (void)mgr;
  (void)mode;
  char full[1024];
  const char *p = path ? path : "";
  while (*p == '/')
    p++;

  FILE *fp = NULL;
  if (strncmp(p, ASSET_DIR "/", sizeof(ASSET_DIR)) == 0)
    fp = fopen(p, "rb");
  if (!fp) {
    snprintf(full, sizeof(full), "%s/%s", ASSET_DIR, p);
    fp = fopen(full, "rb");
  }
  if (!fp)
    fp = fopen(p, "rb");
  if (!fp) {
    if (io_log_enabled())
      fprintf(stderr, "[asset] missing %s\n", p);
    return NULL;
  }
  if (io_log_enabled())
    fprintf(stderr, "[asset] open %s\n", p);
  AAsset *a = calloc(1, sizeof(*a));
  if (!a) {
    fclose(fp);
    return NULL;
  }
  a->fp = fp;
  fseek(fp, 0, SEEK_END);
  a->len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  return a;
}

static int aa_read(void *h, void *buf, size_t n) {
  AAsset *a = h;
  if (!a)
    return -1;
  size_t r = fread(buf, 1, n, a->fp);
  g_asset_bytes_frame += (long)r;
  return (int)r;
}

static long aa_seek64(void *h, long off, int wh) {
  AAsset *a = h;
  if (!a)
    return -1;
  fseek(a->fp, off, wh);
  return ftell(a->fp);
}

static long aa_getLength64(void *h) {
  AAsset *a = h;
  return a ? a->len : 0;
}

static long aa_getRemainingLength64(void *h) {
  AAsset *a = h;
  return a ? a->len - ftell(a->fp) : 0;
}

static void aa_close(void *h) {
  AAsset *a = h;
  if (a) {
    fclose(a->fp);
    free(a);
  }
}

static FILE *w_fopen(const char *path, const char *mode) {
  static FILE *(*real)(const char *, const char *) = NULL;
  if (!real)
    real = dlsym(RTLD_DEFAULT, "fopen");
  FILE *f = real ? real(path, mode) : NULL;

  if (!f && real && path && mode && mode[0] == 'r' &&
      strncmp(path, "assets/", 7) != 0) {
    char alt[1024];
    snprintf(alt, sizeof(alt), "assets/%s", path);
    f = real(alt, mode);
  }
  return f;
}

static int stat_at(const char *path, void *buf, int flag) {
  int r = syscall(SYS_newfstatat, AT_FDCWD, path, buf, flag);
  if (r != 0 && path && strncmp(path, "assets/", 7) != 0) {
    char alt[1024];
    snprintf(alt, sizeof(alt), "assets/%s", path);
    r = syscall(SYS_newfstatat, AT_FDCWD, alt, buf, flag);
  }
  return r;
}

static int my_stat(const char *path, void *buf) {
  return stat_at(path, buf, 0);
}

static int my_lstat(const char *path, void *buf) {
  return stat_at(path, buf, AT_SYMLINK_NOFOLLOW);
}

static int my_fstatat(int dfd, const char *path, void *buf, int flag) {
  if (dfd == AT_FDCWD)
    return stat_at(path, buf, flag);
  return syscall(SYS_newfstatat, dfd, path, buf, flag);
}

static int my_fstat(int fd, void *buf) {
  return syscall(SYS_fstat, fd, buf);
}

/* RAM total do device (MB), lida uma vez de /proc/meminfo. */
static int imports_mem_total_mb(void) {
  static int mb = -2;
  if (mb != -2)
    return mb;
  mb = -1;
  FILE *f = fopen("/proc/meminfo", "r");
  if (f) {
    char line[128];
    while (fgets(line, sizeof(line), f)) {
      unsigned long kb;
      if (sscanf(line, "MemTotal: %lu kB", &kb) == 1) {
        mb = (int)(kb / 1024);
        break;
      }
    }
    fclose(f);
  }
  return mb;
}

/* MODO DE ALTA QUALIDADE (RendererES3) — estudo 2026-07-02:
 * No X5M (Mali-G310 ES3.2 real, 4GB) deixar o motor usar RendererES3 (nao
 * spoofar ES2) da uma imagem visivelmente mais rica (iluminacao/color grading/
 * post-process do caminho ES3 pra que o jogo mobile foi feito). Estavel 15900+
 * frames no mundo aberto. Em Mali-450 (Utgard) e Mali-G31/R36S (1GB) o ES3 dava
 * tela preta -> mantem o spoof ES2. Gate: AUTO liga so em RAM>=2600MB (classe
 * 4GB) E GL real ES3.x. BULLY2_RENDERER=es3|es2 forca; BULLY2_REAL_GL_VERSION=1
 * tambem forca ES3 (compat). Default 1GB = ES2 (intocado). */
static int es3_quality_mode(void) {
  static int mode = -1;
  if (mode >= 0)
    return mode;
  const char *r = first_env("BULLY2_RENDERER", "BULLY_RENDERER");
  if (r && (!strcasecmp(r, "es3") || !strcasecmp(r, "gles3"))) {
    mode = 1;
  } else if (r && (!strcasecmp(r, "es2") || !strcasecmp(r, "gles2"))) {
    mode = 0;
  } else if (getenv("BULLY2_REAL_GL_VERSION")) {
    mode = 1;
  } else {
    /* auto: precisa de RAM suficiente E contexto GL ES3 real.
     * Piso 1700MB = classe 2GB+ (2GB reporta ~1800-2000 de MemTotal; 1GB
     * reporta ~630-950 e fica no ES2 spoof intocado). Ajustavel via
     * BULLY2_ES3_MIN_MB; escape BULLY2_RENDERER=es2 se algum device 2GB
     * (ex. Mali-G31) regredir com o RendererES3. */
    int es3 = 0;
    const unsigned char *(*rgs)(unsigned) = dlsym(RTLD_DEFAULT, "glGetString");
    if (rgs) {
      const unsigned char *v = rgs(0x1F02); /* GL_VERSION real */
      if (v && strstr((const char *)v, "OpenGL ES 3"))
        es3 = 1;
    }
    int min_mb = 1700;
    const char *m = first_env("BULLY2_ES3_MIN_MB", "BULLY_ES3_MIN_MB");
    if (m && atoi(m) > 0)
      min_mb = atoi(m);
    mode = (imports_mem_total_mb() >= min_mb && es3) ? 1 : 0;
  }
  fprintf(stderr, "[gl] renderer mode=%s (mem=%dMB)\n",
          mode ? "ES3 (alta qualidade)" : "ES2 (spoof)",
          imports_mem_total_mb());
  return mode;
}

static const unsigned char *w_glGetString(unsigned name) {
  static const unsigned char *(*real)(unsigned) = NULL;
  if (!real)
    real = dlsym(RTLD_DEFAULT, "glGetString");
  if (name == 0x1F01) {
    const char *e = getenv("BULLY2_GPU_RENDERER");
    if (e && *e)
      return (const unsigned char *)e;
  }
  if (name == 0x1F00) {
    const char *e = getenv("BULLY2_GPU_VENDOR");
    if (e && *e)
      return (const unsigned char *)e;
  }
  /* GL_VERSION (0x1F02) / GL_SHADING_LANGUAGE_VERSION (0x8B8C): FORCA o caminho ES2.
   * libGame.so tem RendererES2 E RendererES3 e escolhe pela versao GL reportada.
   * Em Mali-G31 o contexto (mesmo pedido como ES2) reporta "OpenGL ES 3.2" -> o motor
   * instancia RendererES3 (glTexStorage2D imutavel + shaders ES3), caminho que o bully2
   * nao emula -> render preto. Reportando "OpenGL ES 2.0" o motor usa RendererES2
   * (glTexImage2D + GLSL 1.00), que roda no proprio contexto ES3.2 (backward-compat).
   * Desliga com BULLY2_REAL_GL_VERSION=1. */
  if (!es3_quality_mode()) {
    if (name == 0x1F02) {
      const char *e = getenv("BULLY2_GL_VERSION");
      return (const unsigned char *)((e && *e) ? e : "OpenGL ES 2.0");
    }
    if (name == 0x8B8C) {
      const char *e = getenv("BULLY2_GLSL_VERSION");
      return (const unsigned char *)((e && *e) ? e : "OpenGL ES GLSL ES 1.00");
    }
  }
  const unsigned char *r = real ? real(name) : NULL;
  return r ? r : (const unsigned char *)"";
}

static char *str_replace_all(const char *src, const char *find,
                             const char *repl) {
  size_t fl = strlen(find), rl = strlen(repl), n = 0;
  for (const char *p = src; (p = strstr(p, find)); p += fl)
    n++;
  char *out = malloc(strlen(src) + n * (rl > fl ? rl - fl : 0) + 1);
  if (!out)
    return NULL;
  char *o = out;
  const char *p = src, *q;
  while ((q = strstr(p, find))) {
    memcpy(o, p, (size_t)(q - p));
    o += q - p;
    memcpy(o, repl, rl);
    o += rl;
    p = q + fl;
  }
  strcpy(o, p);
  return out;
}

/* diag uniforms de sombra (BULLY2_UNIFLOG=1): rastreia location de
 * shadowparam/rendersize e loga os valores em glUniform4fv. Se shadowparam.x=0
 * ou rendersize.zw errado -> sombra nao aparece (bug de port, nao driver). */
static int g_loc_shadowparam = -999, g_loc_rendersize = -999;
static int my_glGetUniformLocation(unsigned prog, const char *name) {
  static int (*r)(unsigned, const char *) = NULL;
  if (!r) r = dlsym(RTLD_DEFAULT, "glGetUniformLocation");
  int loc = r ? r(prog, name) : -1;
  if (getenv("BULLY2_UNIFLOG") && name) {
    if (!strcmp(name, "shadowparam")) {
      g_loc_shadowparam = loc;
      static int c; if (c++<4) fprintf(stderr, "[uniflog] loc shadowparam=%d (prog %u)\n", loc, prog);
    } else if (!strcmp(name, "rendersize")) {
      g_loc_rendersize = loc;
      static int c; if (c++<4) fprintf(stderr, "[uniflog] loc rendersize=%d (prog %u)\n", loc, prog);
    } else if (!strcmp(name, "shadowtex") || !strcmp(name, "shadowdepth")) {
      static int c; if (c++<4) fprintf(stderr, "[uniflog] loc %s=%d (prog %u)\n", name, loc, prog);
    }
  }
  return loc;
}
static void my_glUniform4f(int loc, float a, float b, float c, float d) {
  static void (*r)(int, float, float, float, float) = NULL;
  if (!r) r = dlsym(RTLD_DEFAULT, "glUniform4f");
  if (getenv("BULLY2_UNIFLOG")) {
    static int c1, c2;
    if (loc == g_loc_shadowparam && c1++ < 6)
      fprintf(stderr, "[uniflog] shadowparam4f = %.3f %.3f %.3f %.3f\n", a, b, c, d);
    else if (loc == g_loc_rendersize && c2++ < 6)
      fprintf(stderr, "[uniflog] rendersize4f = %.1f %.1f %.5f %.5f\n", a, b, c, d);
  }
  if (r) r(loc, a, b, c, d);
}
static void my_glUniform4fv(int loc, int count, const float *v) {
  static void (*r)(int, int, const float *) = NULL;
  if (!r) r = dlsym(RTLD_DEFAULT, "glUniform4fv");
  if (getenv("BULLY2_UNIFLOG") && v && count > 0) {
    static int c1, c2;
    if (loc == g_loc_shadowparam && c1++ < 6)
      fprintf(stderr, "[uniflog] shadowparam = %.3f %.3f %.3f %.3f\n", v[0],
              v[1], v[2], v[3]);
    else if (loc == g_loc_rendersize && c2++ < 6)
      fprintf(stderr, "[uniflog] rendersize = %.1f %.1f %.5f %.5f\n", v[0],
              v[1], v[2], v[3]);
  }
  if (r) r(loc, count, v);
}

/* === SHADOWTRACE (BULLY2_SHADOWTRACE=1): rastreio profundo da cadeia de
 * sombra deferred. Mantem mapa fbo->attachments, textura bound por unidade,
 * contagem de draws por FBO/frame (identifica o pass do shadow map e se ha
 * CASTERS desenhados nele), dump do estado no draw do RESOLVE (detecta
 * feedback-loop: amostrar textura anexada ao FBO atual — Adreno tolera, Mali
 * nao) e readback do output do resolve (shadowtex vazio = compare falhou).
 * Opt-in, zero efeito sem a env. === */
static int shadowtrace_enabled(void) {
  static int v = -1;
  if (v < 0)
    v = env_enabled("BULLY2_SHADOWTRACE") ? 1 : 0;
  return v;
}
#define ST_FBO_MAX 512
#define ST_RBO_FLAG 0x40000000u
typedef struct { unsigned color0, depth, stencil; } StFboAtt;
static StFboAtt g_st_att[ST_FBO_MAX];
static unsigned g_st_unit_tex[32];
static unsigned g_st_frame;
static unsigned g_st_draw_fbo_ids[24];
static unsigned g_st_draw_dep[24];   /* depth-att no momento do draw */
static unsigned g_st_draw_col[24];   /* color0-att no momento do draw */
static unsigned char g_st_draw_dm[24]; /* depth mask no momento do draw */
static unsigned g_st_draw_fbo_num[24];
static int g_st_draw_fbo_cnt;
static unsigned char g_st_depthmask = 1;
static unsigned g_st_shadowtex;    /* color0 do FBO do resolve (= shadowtex) */
static unsigned g_st_world_draws;  /* draws com shadowtex amostrada */

static void st_texdesc(unsigned tex, char *out, size_t n) {
  if (tex & ST_RBO_FLAG)
    snprintf(out, n, "rbo%u", tex & ~ST_RBO_FLAG);
  else if (tex && tex < g_texinfo_cap && g_texinfo[tex].alive)
    snprintf(out, n, "%u(%dx%d,ifmt=0x%x)", tex, g_texinfo[tex].w,
             g_texinfo[tex].h, g_texinfo[tex].ifmt);
  else
    snprintf(out, n, "%u", tex);
}

static void st_count_draw(void) {
  if (!shadowtrace_enabled())
    return;
  unsigned f = g_shadow_bound_fbo;
  unsigned dep = f < ST_FBO_MAX ? g_st_att[f].depth : 0;
  unsigned col = f < ST_FBO_MAX ? g_st_att[f].color0 : 0;
  if (g_st_shadowtex) {
    for (int u = 0; u < 16; u++)
      if (g_st_unit_tex[u] == g_st_shadowtex) { g_st_world_draws++; break; }
  }
  for (int i = 0; i < g_st_draw_fbo_cnt; i++)
    if (g_st_draw_fbo_ids[i] == f && g_st_draw_dep[i] == dep &&
        g_st_draw_col[i] == col && g_st_draw_dm[i] == g_st_depthmask) {
      g_st_draw_fbo_num[i]++;
      return;
    }
  if (g_st_draw_fbo_cnt < 24) {
    g_st_draw_fbo_ids[g_st_draw_fbo_cnt] = f;
    g_st_draw_dep[g_st_draw_fbo_cnt] = dep;
    g_st_draw_col[g_st_draw_fbo_cnt] = col;
    g_st_draw_dm[g_st_draw_fbo_cnt] = g_st_depthmask;
    g_st_draw_fbo_num[g_st_draw_fbo_cnt++] = 1;
  }
}

int bully2_feedback_fix_enabled_fwd(void);
void bully_shadowtrace_frame(void) {
  if (!shadowtrace_enabled()) {
    if (bully2_feedback_fix_enabled_fwd())
      g_st_frame++; /* fbfix precisa do frame p/ 1 blit/frame */
    return;
  }
  g_st_frame++;
  int hot = (g_st_frame <= 5) || (g_st_frame % 300 == 0);
  if (hot) {
    for (int i = 0; i < g_st_draw_fbo_cnt; i++) {
      unsigned f = g_st_draw_fbo_ids[i];
      char c0[48], dp[48];
      const char *tag = "";
      st_texdesc(g_st_draw_col[i], c0, sizeof(c0));
      st_texdesc(g_st_draw_dep[i], dp, sizeof(dp));
      if (!g_st_draw_col[i] && g_st_draw_dep[i])
        tag = " <== DEPTH-ONLY (shadow map?)";
      fprintf(stderr,
              "[shadowtrace] frame=%u fbo=%u draws=%u color0=%s depth=%s "
              "dmask=%d%s\n",
              g_st_frame, f, g_st_draw_fbo_num[i], c0, dp, g_st_draw_dm[i],
              tag);
    }
    if (g_st_shadowtex)
      fprintf(stderr,
              "[shadowtrace] frame=%u draws-amostrando-shadowtex(%u)=%u\n",
              g_st_frame, g_st_shadowtex, g_st_world_draws);
  }
  g_st_draw_fbo_cnt = 0;
  g_st_world_draws = 0;
}

static void st_resolve_dump(const char *kind, unsigned prog, int count) {
  static int shots = 0, window = -1;
  int wnd = (int)(g_st_frame / 600);
  if (wnd != window) { window = wnd; shots = 0; }
  if (shots >= 8)
    return;
  shots++;
  unsigned f = g_shadow_bound_fbo;
  char c0[48] = "-", dp[48] = "-";
  if (f < ST_FBO_MAX) {
    st_texdesc(g_st_att[f].color0, c0, sizeof(c0));
    st_texdesc(g_st_att[f].depth, dp, sizeof(dp));
    if (g_st_att[f].color0 && !(g_st_att[f].color0 & ST_RBO_FLAG))
      g_st_shadowtex = g_st_att[f].color0;
  }
  fprintf(stderr,
          "[shadowtrace] RESOLVE(%s) frame=%u prog=%u count=%d fbo=%u "
          "color0=%s depth=%s\n",
          kind, g_st_frame, prog, count, f, c0, dp);
  for (int u = 0; u < 8; u++) {
    unsigned t = g_st_unit_tex[u];
    if (!t)
      continue;
    char td[48];
    st_texdesc(t, td, sizeof(td));
    char role[80];
    role[0] = 0;
    if (f < ST_FBO_MAX &&
        (t == g_st_att[f].color0 || t == g_st_att[f].depth ||
         t == g_st_att[f].stencil))
      snprintf(role, sizeof(role), " **FEEDBACK: anexada ao FBO ATUAL %u**", f);
    else
      for (unsigned f2 = 0; f2 < ST_FBO_MAX; f2++) {
        if (g_st_att[f2].depth == t) {
          snprintf(role, sizeof(role), " [depth-att do fbo %u]", f2);
          break;
        }
        if (g_st_att[f2].color0 == t) {
          snprintf(role, sizeof(role), " [color-att do fbo %u]", f2);
          break;
        }
      }
    fprintf(stderr, "[shadowtrace]   unit%d tex=%s%s\n", u, td, role);
  }
}

static void st_resolve_readback(void) {
  static int shots = 0, window = -1;
  int wnd = (int)(g_st_frame / 600);
  if (wnd != window) { window = wnd; shots = 0; }
  if (shots >= 6)
    return;
  shots++;
  static void (*rp)(int, int, int, int, unsigned, unsigned, void *) = NULL;
  static unsigned (*ge)(void) = NULL;
  static void (*gi)(unsigned, int *) = NULL;
  if (!rp) rp = dlsym(RTLD_DEFAULT, "glReadPixels");
  if (!ge) ge = dlsym(RTLD_DEFAULT, "glGetError");
  if (!gi) gi = dlsym(RTLD_DEFAULT, "glGetIntegerv");
  if (!rp || !gi)
    return;
  int vp[4] = {0, 0, 0, 0};
  gi(0x0BA2 /*GL_VIEWPORT*/, vp);
  int w = vp[2], h = vp[3];
  if (w < 8 || h < 8)
    return;
  int rw = w < 64 ? w : 64, rh = h < 64 ? h : 64;
  int rx = vp[0] + (w - rw) / 2, ry = vp[1] + (h - rh) / 2;
  unsigned char *buf = malloc((size_t)rw * rh * 4);
  if (!buf)
    return;
  if (ge) ge();
  rp(rx, ry, rw, rh, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, buf);
  unsigned err = ge ? ge() : 0;
  unsigned mn = 255, mx = 0;
  unsigned long sum = 0;
  for (int i = 0; i < rw * rh; i++) {
    unsigned char r = buf[i * 4];
    if (r < mn) mn = r;
    if (r > mx) mx = r;
    sum += r;
  }
  fprintf(stderr,
          "[shadowtrace] RESOLVE out %dx%d@%d,%d R: min=%u max=%u avg=%.1f "
          "err=0x%x (min=max=255 -> resolve nao escreveu sombra)\n",
          rw, rh, rx, ry, mn, mx, (double)sum / (rw * rh), err);
  free(buf);
}

/* === FEEDBACK FIX (BULLY2_FEEDBACK_FIX=1): quebra o feedback-loop do pass
 * de sombra. A engine amostra `framedepth` (depth-stencil da cena) ENQUANTO
 * ele esta anexado ao FBO em que desenha — comportamento INDEFINIDO no spec.
 * Adreno tolera (sombra funciona no celular); Mali devolve lixo/0 -> sombra
 * some no jogo inteiro. Fix: blit do depth p/ uma textura-copia e amostrar a
 * COPIA durante o draw do resolve, restaurando o binding depois. === */
static int feedback_fix_enabled(void) {
  static int v = -1;
  if (v < 0)
    v = env_enabled("BULLY2_FEEDBACK_FIX") ? 1 : 0;
  return v;
}
int bully2_feedback_fix_enabled_fwd(void) { return feedback_fix_enabled(); }
static unsigned g_fbfix_tex, g_fbfix_fbo;
static int g_fbfix_w, g_fbfix_h;
static unsigned g_fbfix_frame_blitted; /* frame do ultimo blit (1 blit/frame) */

/* retorna unit+1 se trocou o binding (orig_tex sai com a textura original) */
static int st_feedback_fix_begin(unsigned *orig_tex) {
  unsigned f = g_shadow_bound_fbo;
  if (!f || f >= ST_FBO_MAX)
    return 0;
  unsigned dep = g_st_att[f].depth;
  if (!dep || (dep & ST_RBO_FLAG))
    return 0;
  int unit = -1;
  for (int u = 0; u < 8; u++)
    if (g_st_unit_tex[u] == dep) { unit = u; break; }
  if (unit < 0)
    return 0;
  if (!(dep < g_texinfo_cap && g_texinfo[dep].alive && g_texinfo[dep].w > 0))
    return 0;
  int w = g_texinfo[dep].w, h = g_texinfo[dep].h;
  unsigned ifmt = g_texinfo[dep].ifmt;

  static void (*p_genTex)(int, unsigned *), (*p_bindTex)(unsigned, unsigned);
  static void (*p_texImage)(unsigned, int, int, int, int, int, unsigned,
                            unsigned, const void *);
  static void (*p_texPar)(unsigned, unsigned, int);
  static void (*p_genFbo)(int, unsigned *), (*p_bindFbo)(unsigned, unsigned);
  static void (*p_fboTex)(unsigned, unsigned, unsigned, unsigned, int);
  static void (*p_blit)(int, int, int, int, int, int, int, int, unsigned,
                        unsigned);
  static void (*p_active)(unsigned);
  if (!p_blit) {
    p_genTex = dlsym(RTLD_DEFAULT, "glGenTextures");
    p_bindTex = dlsym(RTLD_DEFAULT, "glBindTexture");
    p_texImage = dlsym(RTLD_DEFAULT, "glTexImage2D");
    p_texPar = dlsym(RTLD_DEFAULT, "glTexParameteri");
    p_genFbo = dlsym(RTLD_DEFAULT, "glGenFramebuffers");
    p_bindFbo = dlsym(RTLD_DEFAULT, "glBindFramebuffer");
    p_fboTex = dlsym(RTLD_DEFAULT, "glFramebufferTexture2D");
    p_blit = dlsym(RTLD_DEFAULT, "glBlitFramebuffer");
    p_active = dlsym(RTLD_DEFAULT, "glActiveTexture");
  }
  if (!p_blit || !p_genTex || !p_texImage || !p_fboTex)
    return 0;

  unsigned saved_active = g_shadow_active_texture;

  if (g_fbfix_tex && (g_fbfix_w != w || g_fbfix_h != h)) {
    /* resolucao mudou: recria (raro) */
    static void (*p_delTex)(int, const unsigned *) = NULL;
    static void (*p_delFbo)(int, const unsigned *) = NULL;
    if (!p_delTex) p_delTex = dlsym(RTLD_DEFAULT, "glDeleteTextures");
    if (!p_delFbo) p_delFbo = dlsym(RTLD_DEFAULT, "glDeleteFramebuffers");
    if (p_delTex) p_delTex(1, &g_fbfix_tex);
    if (p_delFbo) p_delFbo(1, &g_fbfix_fbo);
    g_fbfix_tex = g_fbfix_fbo = 0;
  }
  if (!g_fbfix_tex) {
    unsigned fmt, type;
    unsigned att = 0x8D00; /* GL_DEPTH_ATTACHMENT */
    if (ifmt == 0x88F0 /*DEPTH24_STENCIL8*/) {
      fmt = 0x84F9; type = 0x84FA; att = 0x821A; /* DEPTH_STENCIL */
    } else { /* DEPTH_COMPONENT24/16/32F */
      fmt = 0x1902; type = 0x1405;
    }
    p_genTex(1, &g_fbfix_tex);
    p_active(0x84C0 + unit);
    p_bindTex(0x0DE1, g_fbfix_tex);
    p_texImage(0x0DE1, 0, (int)ifmt, w, h, 0, fmt, type, NULL);
    p_texPar(0x0DE1, 0x2801, 0x2600); /* MIN NEAREST */
    p_texPar(0x0DE1, 0x2800, 0x2600); /* MAG NEAREST */
    p_texPar(0x0DE1, 0x2802, 0x812F); /* WRAP_S CLAMP */
    p_texPar(0x0DE1, 0x2803, 0x812F); /* WRAP_T CLAMP */
    p_texPar(0x0DE1, 0x884C, 0);      /* COMPARE_MODE NONE */
    p_genFbo(1, &g_fbfix_fbo);
    p_bindFbo(0x8CA9 /*DRAW*/, g_fbfix_fbo);
    p_fboTex(0x8CA9, att, 0x0DE1, g_fbfix_tex, 0);
    p_bindFbo(0x8D40 /*FRAMEBUFFER*/, f);
    g_fbfix_w = w; g_fbfix_h = h;
    fprintf(stderr,
            "[fbfix] copia de depth criada tex=%u fbo=%u %dx%d ifmt=0x%x "
            "(unit%d feedback de tex %u no fbo %u)\n",
            g_fbfix_tex, g_fbfix_fbo, w, h, ifmt, unit, dep, f);
  }
  /* 1 blit por frame cobre todos os draws de resolve do frame */
  if (g_fbfix_frame_blitted != g_st_frame + 1) {
    static unsigned char (*p_isEn)(unsigned) = NULL;
    static void (*p_dis)(unsigned) = NULL, (*p_en)(unsigned) = NULL;
    if (!p_isEn) {
      p_isEn = dlsym(RTLD_DEFAULT, "glIsEnabled");
      p_dis = dlsym(RTLD_DEFAULT, "glDisable");
      p_en = dlsym(RTLD_DEFAULT, "glEnable");
    }
    int scissor = p_isEn ? p_isEn(0x0C11 /*SCISSOR_TEST*/) : 0;
    if (scissor && p_dis) p_dis(0x0C11); /* blit respeita scissor */
    p_bindFbo(0x8CA8 /*READ*/, f);
    p_bindFbo(0x8CA9 /*DRAW*/, g_fbfix_fbo);
    p_blit(0, 0, w, h, 0, 0, w, h, 0x100 /*DEPTH_BUFFER_BIT*/, 0x2600);
    p_bindFbo(0x8D40, f);
    if (scissor && p_en) p_en(0x0C11);
    g_fbfix_frame_blitted = g_st_frame + 1;
  }
  *orig_tex = dep;
  p_active(0x84C0 + unit);
  p_bindTex(0x0DE1, g_fbfix_tex);
  p_active(saved_active);
  return unit + 1;
}

static void st_feedback_fix_end(int unit, unsigned orig_tex) {
  static void (*p_bindTex)(unsigned, unsigned) = NULL;
  static void (*p_active)(unsigned) = NULL;
  if (!p_bindTex) {
    p_bindTex = dlsym(RTLD_DEFAULT, "glBindTexture");
    p_active = dlsym(RTLD_DEFAULT, "glActiveTexture");
  }
  unsigned saved_active = g_shadow_active_texture;
  p_active(0x84C0 + (unsigned)unit);
  p_bindTex(0x0DE1, orig_tex);
  p_active(saved_active);
}

/* diag: checa status de compilacao dos shaders (BULLY2_SHADERCHK=1) —
 * se o resolve de sombra (sampler2DShadow) falhar compilar, shadowtex fica
 * vazio e nao ha sombra. */
/* rastreia se o RESOLVE de sombra (sampler2DShadow) realmente DESENHA.
 * shader resolve -> programa -> glDrawArrays com esse programa ativo. */
unsigned g_resolve_shaders[16]; int g_resolve_shn = 0;
static unsigned g_resolve_progs[16]; static int g_resolve_pn = 0;
unsigned g_cur_prog = 0;
static int is_resolve_prog(unsigned p) {
  for (int i = 0; i < g_resolve_pn; i++)
    if (g_resolve_progs[i] == p) return 1;
  return 0;
}
int is_resolve_prog_ext(unsigned p) { return is_resolve_prog(p); }
static void my_glAttachShader(unsigned prog, unsigned sh) {
  static void (*r)(unsigned, unsigned) = NULL;
  if (!r) r = dlsym(RTLD_DEFAULT, "glAttachShader");
  if (sh) {
    for (int i = 0; i < g_resolve_shn; i++)
      if (g_resolve_shaders[i] == sh && g_resolve_pn < 16) {
        if (!is_resolve_prog(prog)) g_resolve_progs[g_resolve_pn++] = prog;
        if (getenv("BULLY2_RESOLVEDRAW"))
          fprintf(stderr, "[resolvedraw] resolve shader %u -> prog %u\n", sh, prog);
      }
  }
  if (r) r(prog, sh);
}
static void my_glDrawArrays(unsigned mode, int first, int count) {
  static void (*r)(unsigned, int, int) = NULL;
  if (!r) r = dlsym(RTLD_DEFAULT, "glDrawArrays");
  if (resolvedraw_enabled() && is_resolve_prog(g_cur_prog)) {
    static int c;
    if (c++ < 6)
      fprintf(stderr, "[resolvedraw] RESOLVE DESENHOU prog=%u mode=0x%x count=%d fbo=%u\n",
              g_cur_prog, mode, count, g_shadow_bound_fbo);
  }
  st_count_draw();
  int st_res = shadowtrace_enabled() && is_resolve_prog(g_cur_prog);
  if (st_res)
    st_resolve_dump("arrays", g_cur_prog, count);
  unsigned fbfix_orig = 0;
  int fbfix_unit = 0;
  if (feedback_fix_enabled() && is_resolve_prog(g_cur_prog))
    fbfix_unit = st_feedback_fix_begin(&fbfix_orig);
  if (r) r(mode, first, count);
  if (fbfix_unit)
    st_feedback_fix_end(fbfix_unit - 1, fbfix_orig);
  if (st_res)
    st_resolve_readback();
}

static void my_glCompileShader(unsigned sh) {
  static void (*rc)(unsigned) = NULL;
  static void (*giv)(unsigned, unsigned, int *) = NULL;
  static void (*gil)(unsigned, int, int *, char *) = NULL;
  static const unsigned char *(*rgs)(unsigned) = NULL;
  if (!rc) rc = dlsym(RTLD_DEFAULT, "glCompileShader");
  if (rc) rc(sh);
  if (getenv("BULLY2_SHADERCHK")) {
    if (!giv) giv = dlsym(RTLD_DEFAULT, "glGetShaderiv");
    if (!gil) gil = dlsym(RTLD_DEFAULT, "glGetShaderInfoLog");
    int ok = 1;
    if (giv) giv(sh, 0x8B81 /*COMPILE_STATUS*/, &ok);
    if (!ok) {
      char log[512]; log[0]=0;
      if (gil) gil(sh, sizeof(log), NULL, log);
      fprintf(stderr, "[shaderchk] shader %u FALHOU compilar: %s\n", sh, log);
    }
  }
}

static void (*real_glShaderSource)(unsigned, int, const char *const *,
                                   const int *) = NULL;
static void my_glShaderSource(unsigned sh, int count, const char *const *str,
                              const int *len) {
  (void)len;
  if (!real_glShaderSource)
    real_glShaderSource = dlsym(RTLD_DEFAULT, "glShaderSource");

  size_t total = 1;
  for (int i = 0; i < count; i++)
    if (str && str[i])
      total += strlen(str[i]);

  char *cat = malloc(total);
  if (!cat)
    return;
  cat[0] = 0;
  for (int i = 0; i < count; i++)
    if (str && str[i])
      strcat(cat, str[i]);

  /* highp->mediump so em GPU fraca (ES2 spoof, ex. Mali-450 Utgard que nao
   * tem highp bom no fragment). No modo ES3 (GPU capaz) MANTEM highp: a
   * amostragem de shadow map / comparacao de profundidade precisa de precisao
   * alta -- em mediump (fp16) a sombra sai errada/invisivel. */
  /* dump opt-in dos shaders p/ diagnostico (BULLY2_DUMP_SHADERS=1) */
  if (getenv("BULLY2_DUMP_SHADERS")) {
    static int sn = 0;
    char path[128];
    snprintf(path, sizeof(path), "/tmp/shaders/sh_%04d.glsl", sn++);
    mkdir("/tmp/shaders", 0777);
    FILE *sf = fopen(path, "w");
    if (sf) {
      fwrite(cat, 1, strlen(cat), sf);
      fclose(sf);
    }
    if (strstr(cat, "hadow") || strstr(cat, "Shadow"))
      fprintf(stderr, "[shaderdump] %s tem 'shadow' (%zu bytes)\n", path,
              strlen(cat));
  }
  if (strstr(cat, "sampler2DShadow") && strstr(cat, "shadowdepth")) {
    extern unsigned g_resolve_shaders[16]; extern int g_resolve_shn;
    if (g_resolve_shn < 16) g_resolve_shaders[g_resolve_shn++] = sh;
    if (getenv("BULLY2_RESOLVEDRAW"))
      fprintf(stderr, "[resolvedraw] resolve fragment shader = %u\n", sh);
  }

  /* DIAGNOSTICO: forcar a saida do resolve p/ isolar qual fator zera a sombra.
   * BULLY2_RESOLVE_FORCE=1 const 1.0 (testa write path) | 2 shadowparam.x |
   * 3 textureProj cru | 4 frameSample | 5 shadowPos.z */
  const char *rf = getenv("BULLY2_RESOLVE_FORCE");
  if (rf && strstr(cat, "shadowAmt * shadowparam.x")) {
    const char *rep = "1.0";
    if (rf[0] == '2') rep = "shadowparam.x";
    else if (rf[0] == '3') rep = "textureProj(shadowdepth, shadowPos)";
    else if (rf[0] == '4') rep = "frameSample";
    else if (rf[0] == '5') rep = "clamp(shadowPos.z, 0.0, 1.0)";
    char *nc = str_replace_all(cat, "shadowAmt * shadowparam.x", rep);
    if (nc) { free(cat); cat = nc; }
    fprintf(stderr, "[resolveforce] resolve saida forcada -> %s\n", rep);
  }

  int is_vertex = strstr(cat, "gl_Position") != NULL;
  char *patched;
  if (is_vertex || es3_quality_mode())
    patched = strdup(cat);
  else
    patched = str_replace_all(cat, "highp", "mediump");
  free(cat);
  if (!patched)
    return;
  const char *one = patched;
  if (real_glShaderSource)
    real_glShaderSource(sh, 1, &one, NULL);
  free(patched);
}

/* TRILINEAR nos perfis Low/Medium (default ON): sem isso, o half path descarta a
 * cadeia de mipmap (so nivel 0 rebaixado) e e obrigado a usar GL_LINEAR -> ruas/
 * texturas serrilham a distancia. Com trilinear, geramos a cadeia via
 * glGenerateMipmap na base rebaixada e liberamos LINEAR_MIPMAP_LINEAR.
 * Desliga com BULLY2_TRILINEAR=0 (volta ao GL_LINEAR puro). */
static int bully2_trilinear(void) {
  static int v = -1;
  if (v < 0) {
    const char *e = first_env("BULLY2_TRILINEAR", "BULLY_TRILINEAR");
    v = (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N')) ? 0 : 1;
  }
  return v;
}

static void (*real_glTexParameteri)(unsigned, unsigned, int) = NULL;
static void my_glTexParameteri(unsigned target, unsigned pname, int param) {
  if (!real_glTexParameteri)
    real_glTexParameteri = dlsym(RTLD_DEFAULT, "glTexParameteri");
  /* diag sombra: COMPARE_MODE(0x884C)/COMPARE_FUNC(0x884D) — sampler2DShadow
   * so funciona se COMPARE_MODE=COMPARE_REF_TO_TEXTURE(0x884E). */
  if ((pname == 0x884C || pname == 0x884D) && getenv("BULLY2_SHADOWLOG")) {
    static int c;
    if (c++ < 20)
      fprintf(stderr, "[shadowlog] glTexParameteri pname=0x%x param=0x%x\n",
              pname, param);
  }
  if (pname == 0x813D)
    return;
  /* Em half mode, LINEAR_MIPMAP_* so e permitido em texturas que REALMENTE tem a
   * cadeia (has_mips=1, gerada por nos so nas opacas POT). Todo o resto -> GL_LINEAR:
   * cutout, NPOT, render-to-texture (minimapa), UI/menu, prédios/lonas sem mip. Isso
   * evita textura mipmap-incompleta = PRETO. FORCE_LINEAR forca linear em tudo. */
  if ((getenv("BULLY2_FORCE_LINEAR") ||
       (tex_half_enabled() && !tex_bound_2d_has_mips())) &&
      (pname == 0x2801 || pname == 0x2800) &&
      param >= 0x2700 && param <= 0x2703)
    param = 0x2601;
  if (real_glTexParameteri)
    real_glTexParameteri(target, pname, param);
}

static void (*real_glGenTextures)(int, unsigned *) = NULL;
static void my_glGenTextures(int n, unsigned *textures) {
  if (!real_glGenTextures)
    real_glGenTextures = dlsym(RTLD_DEFAULT, "glGenTextures");
  if (real_glGenTextures)
    real_glGenTextures(n, textures);
  if (!textures || n <= 0)
    return;
  for (int i = 0; i < n; i++) {
    if (textures[i]) {
      tex_ensure(textures[i]);
      g_gltex_gen++;
    }
  }
}

static void (*real_glBindTexture)(unsigned, unsigned) = NULL;
static void my_glBindTexture(unsigned target, unsigned texture) {
  if (!real_glBindTexture)
    real_glBindTexture = dlsym(RTLD_DEFAULT, "glBindTexture");
  if (g_shadow_bound_fbo && shadow_gl_log_enabled() && target == 0x0DE1)
    bully_gltrace("glBindTexture unit=0x%x target=0x%x tex=%u fbo=%u",
                  g_shadow_active_texture, target, texture, g_shadow_bound_fbo);
  if (real_glBindTexture)
    real_glBindTexture(target, texture);
  if (target == 0x0DE1) {
    unsigned st_u = g_shadow_active_texture - 0x84C0;
    if (st_u < 32)
      g_st_unit_tex[st_u] = texture;
  }
  if (target == 0x0DE1)
    g_bound_2d = texture;
  else if (target == 0x8513 || target_is_cube_face(target))
    g_bound_cube = texture;
  if (texture && tex_ensure(texture)) {
    TexInfo *t = &g_texinfo[texture];
    if (!t->target)
      t->target = target;
  }
}

static void (*real_glActiveTexture)(unsigned) = NULL;
static void my_glActiveTexture(unsigned texture) {
  if (!real_glActiveTexture)
    real_glActiveTexture = dlsym(RTLD_DEFAULT, "glActiveTexture");
  g_shadow_active_texture = texture;
  if (g_shadow_bound_fbo && shadow_gl_log_enabled())
    bully_gltrace("glActiveTexture 0x%x fbo=%u", texture, g_shadow_bound_fbo);
  if (real_glActiveTexture)
    real_glActiveTexture(texture);
}

static void (*real_glBindFramebuffer)(unsigned, unsigned) = NULL;
static void my_glBindFramebuffer(unsigned target, unsigned framebuffer) {
  if (!real_glBindFramebuffer)
    real_glBindFramebuffer = dlsym(RTLD_DEFAULT, "glBindFramebuffer");
  if (target == 0x8D40)
    g_shadow_bound_fbo = framebuffer;
  if (shadow_gl_log_enabled())
    bully_gltrace("glBindFramebuffer target=0x%x fbo=%u", target, framebuffer);
  if (real_glBindFramebuffer)
    real_glBindFramebuffer(target, framebuffer);
}

static void (*real_glBindRenderbuffer)(unsigned, unsigned) = NULL;
static void my_glBindRenderbuffer(unsigned target, unsigned renderbuffer) {
  if (!real_glBindRenderbuffer)
    real_glBindRenderbuffer = dlsym(RTLD_DEFAULT, "glBindRenderbuffer");
  if (target == 0x8D41)
    g_shadow_bound_rbo = renderbuffer;
  if (shadow_gl_log_enabled())
    bully_gltrace("glBindRenderbuffer target=0x%x rbo=%u fbo=%u", target,
                  renderbuffer, g_shadow_bound_fbo);
  if (real_glBindRenderbuffer)
    real_glBindRenderbuffer(target, renderbuffer);
}

static void (*real_glRenderbufferStorage)(unsigned, unsigned, int, int) = NULL;
static void my_glRenderbufferStorage(unsigned target, unsigned internalformat,
                                     int width, int height) {
  if (!real_glRenderbufferStorage)
    real_glRenderbufferStorage = dlsym(RTLD_DEFAULT, "glRenderbufferStorage");
  if (shadow_gl_log_enabled() && width == height && width >= 512)
    bully_gltrace("glRenderbufferStorage rbo=%u fmt=0x%x %dx%d fbo=%u",
                  g_shadow_bound_rbo, internalformat, width, height,
                  g_shadow_bound_fbo);
  if (real_glRenderbufferStorage)
    real_glRenderbufferStorage(target, internalformat, width, height);
}

static void (*real_glFramebufferRenderbuffer)(unsigned, unsigned, unsigned,
                                              unsigned) = NULL;
static void my_glFramebufferRenderbuffer(unsigned target, unsigned attachment,
                                         unsigned renderbuffertarget,
                                         unsigned renderbuffer) {
  if (!real_glFramebufferRenderbuffer)
    real_glFramebufferRenderbuffer =
        dlsym(RTLD_DEFAULT, "glFramebufferRenderbuffer");
  if (shadow_gl_log_enabled())
    bully_gltrace("glFramebufferRenderbuffer fbo=%u att=0x%x rbo=%u target=0x%x",
                  g_shadow_bound_fbo, attachment, renderbuffer,
                  renderbuffertarget);
  if (g_shadow_bound_fbo < ST_FBO_MAX) {
    StFboAtt *a = &g_st_att[g_shadow_bound_fbo];
    unsigned v = renderbuffer ? (ST_RBO_FLAG | renderbuffer) : 0;
    if (attachment == 0x8CE0) a->color0 = v;
    else if (attachment == 0x8D00) a->depth = v;
    else if (attachment == 0x8D20) a->stencil = v;
    else if (attachment == 0x821A) { a->depth = v; a->stencil = v; }
  }
  if (real_glFramebufferRenderbuffer)
    real_glFramebufferRenderbuffer(target, attachment, renderbuffertarget,
                                   renderbuffer);
}

static void (*real_glFramebufferTexture2D)(unsigned, unsigned, unsigned,
                                           unsigned, int) = NULL;
static void my_glFramebufferTexture2D(unsigned target, unsigned attachment,
                                      unsigned textarget, unsigned texture,
                                      int level) {
  if (!real_glFramebufferTexture2D)
    real_glFramebufferTexture2D = dlsym(RTLD_DEFAULT, "glFramebufferTexture2D");
  if (shadow_gl_log_enabled())
    bully_gltrace("glFramebufferTexture2D fbo=%u att=0x%x tex=%u target=0x%x lvl=%d",
                  g_shadow_bound_fbo, attachment, texture, textarget, level);
  if (g_shadow_bound_fbo < ST_FBO_MAX) {
    StFboAtt *a = &g_st_att[g_shadow_bound_fbo];
    if (attachment == 0x8CE0) a->color0 = texture;
    else if (attachment == 0x8D00) a->depth = texture;
    else if (attachment == 0x8D20) a->stencil = texture;
    else if (attachment == 0x821A) { a->depth = texture; a->stencil = texture; }
  }
  if (real_glFramebufferTexture2D)
    real_glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

static void (*real_glViewport)(int, int, int, int) = NULL;
static void my_glViewport(int x, int y, int w, int h) {
  if (!real_glViewport)
    real_glViewport = dlsym(RTLD_DEFAULT, "glViewport");
  if (g_shadow_bound_fbo && shadow_gl_log_enabled())
    bully_gltrace("glViewport fbo=%u %d,%d %dx%d", g_shadow_bound_fbo, x, y, w,
                  h);
  if (real_glViewport)
    real_glViewport(x, y, w, h);
}

static void (*real_glDepthMask)(unsigned char) = NULL;
static void my_glDepthMask(unsigned char flag) {
  if (!real_glDepthMask)
    real_glDepthMask = dlsym(RTLD_DEFAULT, "glDepthMask");
  g_st_depthmask = flag ? 1 : 0;
  if (real_glDepthMask)
    real_glDepthMask(flag);
}

static void (*real_glClear)(unsigned) = NULL;
static void my_glClear(unsigned mask) {
  if (!real_glClear)
    real_glClear = dlsym(RTLD_DEFAULT, "glClear");
  if (g_shadow_bound_fbo && shadow_gl_log_enabled())
    bully_gltrace("glClear fbo=%u mask=0x%x", g_shadow_bound_fbo, mask);
  if (real_glClear)
    real_glClear(mask);
}

static void (*real_glUseProgram)(unsigned) = NULL;
static void my_glUseProgram(unsigned program) {
  if (!real_glUseProgram)
    real_glUseProgram = dlsym(RTLD_DEFAULT, "glUseProgram");
  if (g_shadow_bound_fbo && shadow_gl_log_enabled())
    bully_gltrace("glUseProgram fbo=%u program=%u", g_shadow_bound_fbo,
                  program);
  extern unsigned g_cur_prog; g_cur_prog = program;
  if (real_glUseProgram)
    real_glUseProgram(program);
}

static void (*real_glDrawElements)(unsigned, int, unsigned, const void *) = NULL;
static void my_glDrawElements(unsigned mode, int count, unsigned type,
                              const void *indices) {
  if (!real_glDrawElements)
    real_glDrawElements = dlsym(RTLD_DEFAULT, "glDrawElements");
  if (g_shadow_bound_fbo && shadow_gl_log_enabled())
    bully_gltrace("glDrawElements fbo=%u mode=0x%x count=%d type=0x%x idx=%p",
                  g_shadow_bound_fbo, mode, count, type, indices);
  extern int is_resolve_prog_ext(unsigned);
  extern unsigned g_cur_prog, g_shadow_bound_fbo;
  int resolve_here = resolvedraw_enabled() && is_resolve_prog_ext(g_cur_prog);
  if (resolve_here) {
    static int c;
    if (c++ < 6)
      fprintf(stderr, "[resolvedraw] RESOLVE DESENHOU(elem) prog=%u mode=0x%x count=%d fbo=%u\n",
              g_cur_prog, mode, count, g_shadow_bound_fbo);
  }
  st_count_draw();
  int st_res = shadowtrace_enabled() && is_resolve_prog_ext(g_cur_prog);
  if (st_res)
    st_resolve_dump("elements", g_cur_prog, count);
  unsigned fbfix_orig = 0;
  int fbfix_unit = 0;
  if (feedback_fix_enabled() && is_resolve_prog_ext(g_cur_prog))
    fbfix_unit = st_feedback_fix_begin(&fbfix_orig);
  if (real_glDrawElements)
    real_glDrawElements(mode, count, type, indices);
  if (fbfix_unit)
    st_feedback_fix_end(fbfix_unit - 1, fbfix_orig);
  if (st_res)
    st_resolve_readback();
  if (resolve_here && resolvedump_enabled()) {
    static int d;
    if (d++ < 3) {
      /* ler de volta o conteudo REAL do FBO de resolve: consulta o
       * DRAW_FRAMEBUFFER_BINDING atual e o binda p/ leitura (o engine pode ter
       * usado GL_DRAW_FRAMEBUFFER separado, senao liamos o fb default=preto). */
      void (*rgp)(int,int,int,int,unsigned,unsigned,void*) = dlsym(RTLD_DEFAULT,"glReadPixels");
      unsigned (*rge)(unsigned) = dlsym(RTLD_DEFAULT,"glGetError");
      void (*rgiv)(unsigned,int*) = dlsym(RTLD_DEFAULT,"glGetIntegerv");
      void (*rbf)(unsigned,unsigned) = dlsym(RTLD_DEFAULT,"glBindFramebuffer");
      if (rge) rge(0);
      int draw_fbo=0, old_read=0;
      if (rgiv){ rgiv(0x8CA6/*DRAW_FRAMEBUFFER_BINDING*/,&draw_fbo);
                 rgiv(0x8CAA/*READ_FRAMEBUFFER_BINDING*/,&old_read); }
      if (rbf) rbf(0x8CA8/*READ_FRAMEBUFFER*/,(unsigned)draw_fbo);
      int W=256,H=256; static unsigned char buf[256*256*4];
      if (rgp) {
        rgp(0,0,W,H,0x1908/*RGBA*/,0x1401/*UBYTE*/,buf);
        unsigned err = rge?rge(0):0;
        long nz=0,rmin=255,rmax=0,rsum=0;
        for (int i=0;i<W*H;i++){unsigned char r=buf[i*4];if(r)nz++;if(r<rmin)rmin=r;if(r>rmax)rmax=r;rsum+=r;}
        fprintf(stderr,"[resolvedump] drawfbo=%d err=0x%x .r nz=%ld min=%ld max=%ld avg=%ld\n",
                draw_fbo,err,nz,rmin,rmax,rsum/(W*H));
      }
      if (rbf) rbf(0x8CA8,(unsigned)old_read);
    }
  }
}

static void (*real_glDeleteTextures)(int, const unsigned *) = NULL;
static void my_glDeleteTextures(int n, const unsigned *textures) {
  if (!real_glDeleteTextures)
    real_glDeleteTextures = dlsym(RTLD_DEFAULT, "glDeleteTextures");
  if (real_glDeleteTextures)
    real_glDeleteTextures(n, textures);
  if (!textures || n <= 0)
    return;
  for (int i = 0; i < n; i++)
    tex_delete_bound(textures[i]);
}

static void (*real_glTexImage2D)(unsigned, int, int, int, int, int, unsigned,
                                 unsigned, const void *) = NULL;
static void my_glTexImage2D(unsigned tgt, int lvl, int ifmt, int w, int h,
                            int bord, unsigned fmt, unsigned type,
                            const void *px) {
  if (!real_glTexImage2D)
    real_glTexImage2D = dlsym(RTLD_DEFAULT, "glTexImage2D");
  unsigned track_ifmt = (unsigned)ifmt;
  if (ifmt == 0x8058)
    ifmt = 0x1908;
  else if (ifmt == 0x8051)
    ifmt = 0x1907;
  track_ifmt = (unsigned)ifmt;

  int rw = w;
  int rh = h;
  const void *rpx = px;
  void *half = NULL;
  int bpp = bytes_per_pixel(track_ifmt, fmt, type);

  /* CUTOUT (alpha vazado: folhas/cercas/portoes): mipmap mistura o RGB preto dos
   * texels transparentes -> HALO PRETO. Detecta por scan barato do alpha e marca
   * a textura p/ NUNCA receber mipmap (fica LINEAR). So p/ TEXTURE_2D, nivel 0. */
  if (bully2_trilinear() && lvl == 0 && px && tgt == 0x0DE1) {
    /* base nova -> invalida a cadeia antiga; so volta a ter mips se gerarmos abaixo */
    tex_mark_has_mips(g_bound_2d, 0);
    int is_cutout = 0;
    if (type == 0x8033 || type == 0x8034) {
      is_cutout = 1;
    } else if (fmt == 0x1908 && type == 0x1401 && w > 0 && h > 0) {
      int n = w * h, step = n > 4096 ? n / 4096 : 1, tr = 0;
      for (int i = 0; i < n; i += step)
        if (((const unsigned char *)px)[(size_t)i * 4 + 3] < 250)
          if (++tr > 8)
            break;
      is_cutout = (tr > 8);
    }
    tex_mark_cutout(g_bound_2d, is_cutout);
  }

  if (tex_half_enabled() && lvl > 0)
    return;

  if (tex_half_enabled() && lvl == 0 && px && bord == 0 &&
      (w >= tex_half_min_dim() || h >= tex_half_min_dim()) &&
      (tgt == 0x0DE1 || target_is_cube_face(tgt)) &&
      tex_half_type_supported(fmt, type, bpp)) {
    int hw = 0;
    int hh = 0;
    half = half_pixels_nearest(px, w, h, bpp, &hw, &hh);
    if (half) {
      uint32_t old_bytes = tex_level_size(w, h, bpp);
      uint32_t new_bytes = tex_level_size(hw, hh, bpp);
      rw = hw;
      rh = hh;
      rpx = half;
      g_gltex_half_upload++;
      if (old_bytes > new_bytes)
        g_gltex_half_saved_bytes += (long long)old_bytes - new_bytes;
      if (!real_glTexParameteri)
        real_glTexParameteri = dlsym(RTLD_DEFAULT, "glTexParameteri");
      if (real_glTexParameteri) {
        /* MIN: trilinear (LINEAR_MIPMAP_LINEAR) so nas OPACAS e POT (cadeia
         * gerada abaixo). Cutout -> GL_LINEAR (mipmap = halo preto). NPOT -> GL_LINEAR
         * (Utgard/ES2 nao mipmapa NPOT: glGenerateMipmap falha -> textura incompleta =
         * PRETO; ex.: fundos de loading e decais do mapa). MAG sempre LINEAR. */
        int tri = bully2_trilinear() && !tex_bound_2d_cutout() &&
                  is_pot(hw) && is_pot(hh);
        real_glTexParameteri(tgt, 0x2801, tri ? 0x2703 : 0x2601);
        real_glTexParameteri(tgt, 0x2800, 0x2601);
      }
    }
  }

  if (real_glTexImage2D)
    real_glTexImage2D(tgt, lvl, ifmt, rw, rh, bord, fmt, type, rpx);
  /* TRILINEAR (Low/Medium): em half mode os niveis>0 do jogo foram descartados
   * acima -> geramos a cadeia com glGenerateMipmap p/ que LINEAR_MIPMAP_LINEAR
   * funcione (tira o serrilhado). Cobre TODA textura 2D uncompressed com dados,
   * OPACA (cutout=halo preto) e POT (Utgard nao mipmapa NPOT = preto) -- rebaixada
   * (ruas/predios grandes) OU nao (texturas pequenas < min_dim). High nao entra
   * aqui (usa os mips nativos do jogo). */
  if (lvl == 0 && rpx && tex_half_enabled() && bully2_trilinear() &&
      tgt == 0x0DE1 && !tex_bound_2d_cutout() && is_pot(rw) && is_pot(rh)) {
    static void (*r_genmip)(unsigned) = NULL;
    if (!r_genmip)
      r_genmip = (void (*)(unsigned))dlsym(RTLD_DEFAULT, "glGenerateMipmap");
    if (r_genmip) {
      r_genmip(tgt);
      tex_mark_has_mips(g_bound_2d, 1); /* cadeia completa -> pode trilinear */
    }
  }
  free(half);
  tex_set_level(bound_tex_for_target(tgt), tgt, lvl, track_ifmt, rw, rh,
                tex_level_size(rw, rh, bpp));
}

static void (*real_glCompressedTexImage2D)(unsigned, int, unsigned, int, int,
                                           int, int, const void *) = NULL;
static void my_glCompressedTexImage2D(unsigned target, int level,
                                      unsigned internalformat, int width,
                                      int height, int border, int imageSize,
                                      const void *data) {
  if (!real_glCompressedTexImage2D)
    real_glCompressedTexImage2D = dlsym(RTLD_DEFAULT, "glCompressedTexImage2D");
  if (real_glCompressedTexImage2D)
    real_glCompressedTexImage2D(target, level, internalformat, width, height,
                                border, imageSize, data);
  tex_set_level(bound_tex_for_target(target), target, level, internalformat,
                width, height, imageSize > 0 ? (uint32_t)imageSize : 0);
}

static void *gl_proc2(const char *core, const char *ext) {
  void *(*gpa)(const char *) = dlsym(RTLD_DEFAULT, "eglGetProcAddress");
  void *p = gpa ? gpa(core) : NULL;
  if (!p && gpa && ext)
    p = gpa(ext);
  if (!p)
    p = dlsym(RTLD_DEFAULT, core);
  if (!p && ext)
    p = dlsym(RTLD_DEFAULT, ext);
  return p;
}

static void (*r_glGenVAO)(int, unsigned *) = NULL;
static void my_glGenVertexArrays(int n, unsigned *a) {
  if (!r_glGenVAO)
    r_glGenVAO = gl_proc2("glGenVertexArrays", "glGenVertexArraysOES");
  if (r_glGenVAO)
    r_glGenVAO(n, a);
  else if (a)
    for (int i = 0; i < n; i++)
      a[i] = 0;
}

static void (*r_glBindVAO)(unsigned) = NULL;
static void my_glBindVertexArray(unsigned a) {
  if (!r_glBindVAO)
    r_glBindVAO = gl_proc2("glBindVertexArray", "glBindVertexArrayOES");
  if (r_glBindVAO)
    r_glBindVAO(a);
}

static void (*r_glDelVAO)(int, const unsigned *) = NULL;
static void my_glDeleteVertexArrays(int n, const unsigned *a) {
  if (!r_glDelVAO)
    r_glDelVAO = gl_proc2("glDeleteVertexArrays", "glDeleteVertexArraysOES");
  if (r_glDelVAO)
    r_glDelVAO(n, a);
}

static void (*r_glDrawBuffers)(int, const unsigned *) = NULL;
static void my_glDrawBuffers(int n, const unsigned *b) {
  if (!r_glDrawBuffers)
    r_glDrawBuffers = gl_proc2("glDrawBuffers", "glDrawBuffersEXT");
  unsigned b0 = (b && n > 0) ? b[0] : 0;
  bully_gltrace("glDrawBuffers fbo=%u n=%d b0=0x%x safe=%d real=%p",
                g_shadow_bound_fbo, n, b0, drawbuffers_safe_enabled(),
                (void *)r_glDrawBuffers);
  if (drawbuffers_safe_enabled() && n == 1 && b0 == 0x8CE0)
    return;
  if (r_glDrawBuffers)
    r_glDrawBuffers(n, b);
}

/* KMSDRM: o eglSwapBuffers CRU nao faz page-flip (so SDL_GL_SwapWindow faz o
 * drmModePageFlip). O jogo (RenderThread) presenta chamando eglSwapBuffers DIRETO
 * nos objetos EGL que ele seeda (OS_EGL globals) -> essa chamada resolve p/ nosso
 * import. Em KMSDRM roteamos p/ bully_swap_buffers() (SDL_GL_SwapWindow); em
 * mali/fbdev mantemos o raw (Amlogic-old intacto) + screenshot sob demanda.
 * SEM este hook o present do jogo nunca chega no scanout -> tela preta (com audio). */
extern void bully_swap_buffers(void);
extern int bully_is_kmsdrm(void);
extern void bully_maybe_screenshot(void);
extern void *bully_sdl_surface(void);

/* PIN DE SURFACE (fix tela-preta H700/Knulli e afins):
 * a engine destroi a surface EGL do SDL e recria a propria via
 * eglCreateWindowSurface sobre o native-window fake 0xB211FACE. No blob
 * Utgard (Mali-450) isso devolve a surface do fb0 e funciona; no blob do
 * Mali-G31 r20p0 (H700/CubeXX Knulli) a surface recriada nao fica ligada ao
 * scanout -> jogo renderiza no vazio (audio+frames, painel congelado na
 * splash). Interceptamos create/destroy p/ SEMPRE devolver a surface de
 * scan-out do SDL (a que apresenta de verdade) e nunca destrui-la.
 * Auto-ON em fbdev (nao-kmsdrm); em kmsdrm o swap ja vai por SDL_GL_SwapWindow.
 * BULLY2_PIN_SURFACE=0 desliga, =1 forca. */
static int surface_pin_enabled(void) {
  static int en = -1;
  if (en < 0) {
    const char *e = getenv("BULLY2_PIN_SURFACE");
    if (e && *e)
      en = strcmp(e, "0") != 0;
    else
      en = bully_is_kmsdrm() ? 0 : 1; /* auto: pin em fbdev, dispensa em kmsdrm */
    fprintf(stderr, "[egl] surface pin=%d (kmsdrm=%d)\n", en, bully_is_kmsdrm());
  }
  return en;
}

static void *(*real_eglCreateWindowSurface)(void *, void *, void *,
                                            const int *) = NULL;
static void *my_eglCreateWindowSurface(void *dpy, void *cfg, void *win,
                                       const int *attr) {
  if (surface_pin_enabled()) {
    void *s = bully_sdl_surface();
    if (s) {
      fprintf(stderr,
              "[egl] CreateWindowSurface PINNED -> SDL scanout %p (win=%p)\n", s,
              win);
      return s;
    }
  }
  if (!real_eglCreateWindowSurface)
    real_eglCreateWindowSurface =
        dlsym(RTLD_DEFAULT, "eglCreateWindowSurface");
  return real_eglCreateWindowSurface
             ? real_eglCreateWindowSurface(dpy, cfg, win, attr)
             : NULL;
}

static unsigned (*real_eglDestroySurface)(void *, void *) = NULL;
static unsigned my_eglDestroySurface(void *dpy, void *surf) {
  if (surface_pin_enabled() && surf == bully_sdl_surface()) {
    fprintf(stderr, "[egl] DestroySurface no-op (SDL scanout preservada) %p\n",
            surf);
    return 1; /* EGL_TRUE */
  }
  if (!real_eglDestroySurface)
    real_eglDestroySurface = dlsym(RTLD_DEFAULT, "eglDestroySurface");
  return real_eglDestroySurface ? real_eglDestroySurface(dpy, surf) : 1;
}

extern int bully_mali_swap_sdl(void);
static unsigned (*real_eglSwapBuffers)(void *, void *) = NULL;
static unsigned my_eglSwapBuffers(void *dpy, void *surf) {
  static unsigned long n;
  void *engine_surf = surf;
  bully_shadowtrace_frame();
  /* kmsdrm/wayland/x11, ou mali-G/H700 (blitter): present pelo caminho do SDL
   * (SDL_GL_SwapWindow), que aciona o page-flip/blitter. O eglSwapBuffers cru
   * NAO apresenta nesses backends. bully_swap_buffers decide raw vs SwapWindow. */
  if (bully_is_kmsdrm() || bully_mali_swap_sdl()) {
    bully_swap_buffers();
    if (n == 0 || (n % 600) == 0)
      fprintf(stderr, "[present] swap #%lu via SDL (kmsdrm/blitter) pinned=%d\n",
              n, surface_pin_enabled());
    n++;
    return 1;
  }
  bully_maybe_screenshot();
  /* Amlogic Mali-4xx: eglSwapBuffers cru na surface de scan-out do SDL (pin). */
  if (surface_pin_enabled()) {
    void *s = bully_sdl_surface();
    if (s)
      surf = s;
  }
  if (!real_eglSwapBuffers)
    real_eglSwapBuffers = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
  unsigned r = real_eglSwapBuffers ? real_eglSwapBuffers(dpy, surf) : 1;
  if (n == 0 || (n % 600) == 0)
    fprintf(stderr,
            "[present] swap #%lu surf=%p engine_surf=%p pinned=%d ret=%u\n", n,
            surf, engine_surf, surface_pin_enabled(), r);
  n++;
  return r;
}

static void *(*real_eglGetProcAddress)(const char *) = NULL;
static void *my_eglGetProcAddress(const char *name) {
  if (!real_eglGetProcAddress)
    real_eglGetProcAddress = dlsym(RTLD_DEFAULT, "eglGetProcAddress");
  if (name && (!strcmp(name, "glDrawBuffers") ||
               !strcmp(name, "glDrawBuffersEXT"))) {
    bully_gltrace("eglGetProcAddress(%s) -> glDrawBuffers shim", name);
    return (void *)my_glDrawBuffers;
  }
  void *p = real_eglGetProcAddress ? real_eglGetProcAddress(name) : NULL;
  if (shadow_gl_log_enabled() && name &&
      (strstr(name, "Framebuffer") || strstr(name, "Renderbuffer") ||
       strstr(name, "Draw")))
    bully_gltrace("eglGetProcAddress(%s) -> %p", name, p);
  return p;
}

static void (*r_glTexStorage2D)(unsigned, int, unsigned, int, int) = NULL;
static void my_glTexStorage2D(unsigned target, int levels, unsigned ifmt, int w,
                              int h) {
  if (!r_glTexStorage2D)
    r_glTexStorage2D = gl_proc2("glTexStorage2D", "glTexStorage2DEXT");
  int rw = w;
  int rh = h;
  int rlevels = levels;
  int do_half = tex_half_enabled() && (w >= tex_half_min_dim() || h >= tex_half_min_dim()) &&
                (target == 0x0DE1 || target_is_cube_face(target));
  if (do_half) {
    rw = w / 2;
    rh = h / 2;
    if (rw < 1)
      rw = 1;
    if (rh < 1)
      rh = 1;
    rlevels = 1;
    if (!real_glTexParameteri)
      real_glTexParameteri = dlsym(RTLD_DEFAULT, "glTexParameteri");
    if (real_glTexParameteri) {
      real_glTexParameteri(target, 0x2801, 0x2601);
      real_glTexParameteri(target, 0x2800, 0x2601);
    }
  }
  if (r_glTexStorage2D) {
    r_glTexStorage2D(target, rlevels, ifmt, rw, rh);
    unsigned id = bound_tex_for_target(target);
    tex_set_storage(id, target, rlevels, ifmt, rw, rh);
    if (id && tex_ensure(id)) {
      TexInfo *t = &g_texinfo[id];
      t->orig_w = w;
      t->orig_h = h;
      t->half_storage = do_half ? 1 : 0;
    }
  } else
    fprintf(stderr, "[gl] glTexStorage2D unavailable (%dx%d levels=%d)\n", w, h,
            levels);
}

static void (*r_glTexSubImage2D)(unsigned, int, int, int, int, int, unsigned,
                                 unsigned, const void *) = NULL;
static void my_glTexSubImage2D(unsigned target, int level, int x, int y, int w,
                               int h, unsigned fmt, unsigned type,
                               const void *pixels) {
  if (!r_glTexSubImage2D)
    r_glTexSubImage2D = dlsym(RTLD_DEFAULT, "glTexSubImage2D");
  unsigned id = bound_tex_for_target(target);
  TexInfo *t = (id && id < g_texinfo_cap) ? &g_texinfo[id] : NULL;
  if (t && t->half_storage) {
    if (level > 0)
      return;
    int bpp = bytes_per_pixel(t->ifmt, fmt, type);
    if (pixels && x == 0 && y == 0 && w == t->orig_w && h == t->orig_h &&
        tex_half_type_supported(fmt, type, bpp)) {
      int hw = 0;
      int hh = 0;
      void *half = half_pixels_nearest(pixels, w, h, bpp, &hw, &hh);
      if (half) {
        if (r_glTexSubImage2D)
          r_glTexSubImage2D(target, 0, 0, 0, hw, hh, fmt, type, half);
        uint32_t old_bytes = tex_level_size(w, h, bpp);
        uint32_t new_bytes = tex_level_size(hw, hh, bpp);
        g_gltex_half_upload++;
        if (old_bytes > new_bytes)
          g_gltex_half_saved_bytes += (long long)old_bytes - new_bytes;
        free(half);
        return;
      }
    }
  }
  if (r_glTexSubImage2D)
    r_glTexSubImage2D(target, level, x, y, w, h, fmt, type, pixels);
}

static void tl_noop(void) {}

/* === OpenAL mute-stubs ===
 * Registrados SO quando o libopenal.so.1 do sistema nao existe (muOS/Knulli
 * sem OpenAL-soft): emulam um AL "funcional porem mudo" — jogo roda SEM SOM
 * em vez de saltar pra NULL na init de audio. Handles fake nao-NULL; gen*
 * devolvem nomes 1..n; SOURCE_STATE reporta AL_STOPPED. */
static void *alstub_open_device(const char *n) {
  (void)n;
  fprintf(stderr, "[al-stub] alcOpenDevice: OpenAL ausente -> audio MUDO\n");
  return (void *)0xA15;
}
static void *alstub_create_context(void *d, const int *a) {
  (void)d;
  (void)a;
  return (void *)0xA16;
}
static int alstub_ret1(void) { return 1; }
static void alstub_gen(int n, unsigned *ids) {
  for (int i = 0; i < n; i++)
    ids[i] = (unsigned)(i + 1);
}
static void alstub_get_srci(unsigned s, int param, int *out) {
  (void)s;
  if (out)
    *out = (param == 0x1010 /*AL_SOURCE_STATE*/) ? 0x1014 /*AL_STOPPED*/ : 0;
}
static void alstub_get_srcf(unsigned s, int param, float *out) {
  (void)s;
  (void)param;
  if (out)
    *out = 0.0f;
}
static void alstub_get_src3f(unsigned s, int param, float *a, float *b,
                             float *c) {
  (void)s;
  (void)param;
  if (a)
    *a = 0.0f;
  if (b)
    *b = 0.0f;
  if (c)
    *c = 0.0f;
}
static void alstub_get_bufi(unsigned b, int param, int *out) {
  (void)b;
  (void)param;
  if (out)
    *out = 0;
}

DynLibFunction bully_al_stub_table[] = {
    {"alcOpenDevice", (uintptr_t)alstub_open_device},
    {"alcCloseDevice", (uintptr_t)alstub_ret1},
    {"alcCreateContext", (uintptr_t)alstub_create_context},
    {"alcMakeContextCurrent", (uintptr_t)alstub_ret1},
    {"alcIsExtensionPresent", (uintptr_t)ret0},
    {"alGetError", (uintptr_t)ret0},
    {"alGenBuffers", (uintptr_t)alstub_gen},
    {"alGenSources", (uintptr_t)alstub_gen},
    {"alGenFilters", (uintptr_t)alstub_gen},
    {"alDeleteBuffers", (uintptr_t)ret0},
    {"alDeleteSources", (uintptr_t)ret0},
    {"alDeleteFilters", (uintptr_t)ret0},
    {"alIsFilter", (uintptr_t)ret0},
    {"alFilterf", (uintptr_t)ret0},
    {"alFilteri", (uintptr_t)ret0},
    {"alBufferData", (uintptr_t)ret0},
    {"alGetBufferi", (uintptr_t)alstub_get_bufi},
    {"alGetSourcei", (uintptr_t)alstub_get_srci},
    {"alGetSourcef", (uintptr_t)alstub_get_srcf},
    {"alGetSource3f", (uintptr_t)alstub_get_src3f},
    {"alSourcei", (uintptr_t)ret0},
    {"alSourcef", (uintptr_t)ret0},
    {"alSource3f", (uintptr_t)ret0},
    {"alSourcePlay", (uintptr_t)ret0},
    {"alSourcePause", (uintptr_t)ret0},
    {"alSourceStop", (uintptr_t)ret0},
    {"alSourceRewind", (uintptr_t)ret0},
    {"alSourceQueueBuffers", (uintptr_t)ret0},
    {"alSourceUnqueueBuffers", (uintptr_t)ret0},
    {"alListener3f", (uintptr_t)ret0},
    {"alListenerfv", (uintptr_t)ret0},
};
int bully_al_stub_count =
    sizeof(bully_al_stub_table) / sizeof(bully_al_stub_table[0]);

void bully_imports_init(void) {
  ctype_init();
}

DynLibFunction bully_stub_table[] = {
    {"__stack_chk_fail", (uintptr_t)b_stack_chk_fail},
    {"__errno", (uintptr_t)bionic___errno},
    {"__assert2", (uintptr_t)b_assert2},
    {"__strlen_chk", (uintptr_t)b_strlen_chk},
    {"__strrchr_chk", (uintptr_t)b_strrchr_chk},
    {"__strchr_chk", (uintptr_t)b_strchr_chk},
    {"__strncpy_chk2", (uintptr_t)b_strncpy_chk2},
    {"__android_log_print", (uintptr_t)b_android_log},
    {"android_set_abort_message", (uintptr_t)b_set_abort_message},
    {"__system_property_get", (uintptr_t)b_system_property_get},
    {"__sF", (uintptr_t)bionic_sF},
    {"fprintf", (uintptr_t)w_fprintf},
    {"vfprintf", (uintptr_t)w_vfprintf},
    {"fwrite", (uintptr_t)w_fwrite},
    {"fputs", (uintptr_t)w_fputs},
    {"fputc", (uintptr_t)w_fputc},
    {"fflush", (uintptr_t)w_fflush},
    {"_ctype_", (uintptr_t)(ctype_tab + 1)},
    {"ANativeWindow_fromSurface", (uintptr_t)aw_fromSurface},
    {"ANativeWindow_setBuffersGeometry", (uintptr_t)aw_setBuffersGeometry},
    {"ANativeWindow_getWidth", (uintptr_t)aw_getWidth},
    {"ANativeWindow_getHeight", (uintptr_t)aw_getHeight},
    {"ANativeWindow_release", (uintptr_t)aw_release},
    {"AAssetManager_fromJava", (uintptr_t)am_fromJava},
    {"AAssetManager_open", (uintptr_t)aa_open},
    {"AAsset_read", (uintptr_t)aa_read},
    {"AAsset_seek64", (uintptr_t)aa_seek64},
    {"AAsset_getLength64", (uintptr_t)aa_getLength64},
    {"AAsset_getRemainingLength64", (uintptr_t)aa_getRemainingLength64},
    {"AAsset_close", (uintptr_t)aa_close},
    {"eglGetProcAddress", (uintptr_t)my_eglGetProcAddress},
    {"eglSwapBuffers", (uintptr_t)my_eglSwapBuffers},
    {"eglCreateWindowSurface", (uintptr_t)my_eglCreateWindowSurface},
    {"eglDestroySurface", (uintptr_t)my_eglDestroySurface},
    {"glGetString", (uintptr_t)w_glGetString},
    {"glShaderSource", (uintptr_t)my_glShaderSource},
    {"glTexParameteri", (uintptr_t)my_glTexParameteri},
    {"glActiveTexture", (uintptr_t)my_glActiveTexture},
    {"glGenTextures", (uintptr_t)my_glGenTextures},
    {"glBindTexture", (uintptr_t)my_glBindTexture},
    {"glDeleteTextures", (uintptr_t)my_glDeleteTextures},
    {"glTexImage2D", (uintptr_t)my_glTexImage2D},
    {"glCompressedTexImage2D", (uintptr_t)my_glCompressedTexImage2D},
    {"glTexStorage2D", (uintptr_t)my_glTexStorage2D},
    {"glTexSubImage2D", (uintptr_t)my_glTexSubImage2D},
    {"glBindFramebuffer", (uintptr_t)my_glBindFramebuffer},
    {"glBindRenderbuffer", (uintptr_t)my_glBindRenderbuffer},
    {"glRenderbufferStorage", (uintptr_t)my_glRenderbufferStorage},
    {"glFramebufferRenderbuffer", (uintptr_t)my_glFramebufferRenderbuffer},
    {"glFramebufferTexture2D", (uintptr_t)my_glFramebufferTexture2D},
    {"glViewport", (uintptr_t)my_glViewport},
    {"glClear", (uintptr_t)my_glClear},
    {"glDepthMask", (uintptr_t)my_glDepthMask},
    {"glUseProgram", (uintptr_t)my_glUseProgram},
    {"glDrawElements", (uintptr_t)my_glDrawElements},
    {"glAttachShader", (uintptr_t)my_glAttachShader},
    {"glDrawArrays", (uintptr_t)my_glDrawArrays},
    {"glGenVertexArrays", (uintptr_t)my_glGenVertexArrays},
    {"glBindVertexArray", (uintptr_t)my_glBindVertexArray},
    {"glDeleteVertexArrays", (uintptr_t)my_glDeleteVertexArrays},
    {"glDrawBuffers", (uintptr_t)my_glDrawBuffers},
    {"fopen", (uintptr_t)w_fopen},
    {"stat", (uintptr_t)my_stat},
    {"lstat", (uintptr_t)my_lstat},
    {"fstat", (uintptr_t)my_fstat},
    {"fstatat", (uintptr_t)my_fstatat},
    {"stat64", (uintptr_t)my_stat},
    {"lstat64", (uintptr_t)my_lstat},
    {"fstat64", (uintptr_t)my_fstat},
    {"fstatat64", (uintptr_t)my_fstatat},
    {"_ZTH7gString", (uintptr_t)tl_noop},
    {"_ZTH8gString2", (uintptr_t)tl_noop},
    {"_ZTHN10ALCcontext13sLocalContextE", (uintptr_t)tl_noop},
    {"_Z24NVThreadGetCurrentJNIEnvv", (uintptr_t)NVThreadGetCurrentJNIEnv},
};

const int bully_stub_count =
    sizeof(bully_stub_table) / sizeof(bully_stub_table[0]);
