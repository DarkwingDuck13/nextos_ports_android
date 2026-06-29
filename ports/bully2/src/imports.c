/* imports.c -- Bully2 original-first bionic/NDK shims.
 *
 * This file is intentionally narrow. It resolves Android/Bionic and Mali-450
 * GLES2 compatibility gaps, but does not cache, convert, downscale, or replace
 * game texture content.
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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "jni_shim.h"
#include "so_util.h"

volatile long g_asset_bytes_frame = 0;

static int env_enabled(const char *name) {
  const char *e = getenv(name);
  return e && strcmp(e, "0") != 0;
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

static int glmem_log_enabled(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = env_enabled("BULLY2_GLMEMLOG") ? 1 : 0;
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

void bully_glmem_report(const char *why) {
  if (!glmem_log_enabled())
    return;
  fprintf(stderr,
          "[glmem] %s live=%lld MB peak=%lld MB gen=%ld del=%ld upload=%ld "
          "bound2d=%u boundcube=%u\n",
          why ? why : "report", g_gltex_live_bytes / (1024 * 1024),
          g_gltex_peak_bytes / (1024 * 1024), g_gltex_gen, g_gltex_del,
          g_gltex_upload, g_bound_2d, g_bound_cube);
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

  int is_vertex = strstr(cat, "gl_Position") != NULL;
  char *patched = is_vertex ? strdup(cat) : str_replace_all(cat, "highp", "mediump");
  free(cat);
  if (!patched)
    return;
  const char *one = patched;
  if (real_glShaderSource)
    real_glShaderSource(sh, 1, &one, NULL);
  free(patched);
}

static void (*real_glTexParameteri)(unsigned, unsigned, int) = NULL;
static void my_glTexParameteri(unsigned target, unsigned pname, int param) {
  if (!real_glTexParameteri)
    real_glTexParameteri = dlsym(RTLD_DEFAULT, "glTexParameteri");
  if (pname == 0x813D)
    return;
  if (getenv("BULLY2_FORCE_LINEAR") && (pname == 0x2801 || pname == 0x2800) &&
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
  if (real_glBindTexture)
    real_glBindTexture(target, texture);
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
  if (real_glTexImage2D)
    real_glTexImage2D(tgt, lvl, ifmt, w, h, bord, fmt, type, px);
  tex_set_level(bound_tex_for_target(tgt), tgt, lvl, track_ifmt, w, h,
                tex_level_size(w, h, bytes_per_pixel(track_ifmt, fmt, type)));
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
  if (r_glDrawBuffers)
    r_glDrawBuffers(n, b);
}

static void (*r_glTexStorage2D)(unsigned, int, unsigned, int, int) = NULL;
static void my_glTexStorage2D(unsigned target, int levels, unsigned ifmt, int w,
                              int h) {
  if (!r_glTexStorage2D)
    r_glTexStorage2D = gl_proc2("glTexStorage2D", "glTexStorage2DEXT");
  if (r_glTexStorage2D) {
    r_glTexStorage2D(target, levels, ifmt, w, h);
    tex_set_storage(bound_tex_for_target(target), target, levels, ifmt, w, h);
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
  if (r_glTexSubImage2D)
    r_glTexSubImage2D(target, level, x, y, w, h, fmt, type, pixels);
}

static void tl_noop(void) {}

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
    {"glGetString", (uintptr_t)w_glGetString},
    {"glShaderSource", (uintptr_t)my_glShaderSource},
    {"glTexParameteri", (uintptr_t)my_glTexParameteri},
    {"glGenTextures", (uintptr_t)my_glGenTextures},
    {"glBindTexture", (uintptr_t)my_glBindTexture},
    {"glDeleteTextures", (uintptr_t)my_glDeleteTextures},
    {"glTexImage2D", (uintptr_t)my_glTexImage2D},
    {"glCompressedTexImage2D", (uintptr_t)my_glCompressedTexImage2D},
    {"glTexStorage2D", (uintptr_t)my_glTexStorage2D},
    {"glTexSubImage2D", (uintptr_t)my_glTexSubImage2D},
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
