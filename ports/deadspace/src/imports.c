#define _GNU_SOURCE
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>

#include <GLES/gl.h>

#include "imports.h"

extern volatile uintptr_t g_load_base;
extern int deadspace_screen_width(void);
extern int deadspace_screen_height(void);

static volatile unsigned g_gl_draw_calls;
static volatile unsigned g_gl_draw_calls_default;
static volatile unsigned g_gl_draw_calls_fbo;
static volatile unsigned g_current_fbo;

#ifndef GL_FRAMEBUFFER_OES
#define GL_FRAMEBUFFER_OES 0x8D40
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE_OES
#define GL_FRAMEBUFFER_COMPLETE_OES 0x8CD5
#endif
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x8069
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_CLIENT_ACTIVE_TEXTURE
#define GL_CLIENT_ACTIVE_TEXTURE 0x84E1
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_FLOAT
#define GL_FLOAT 0x1406
#endif
#ifndef GL_FIXED
#define GL_FIXED 0x140C
#endif
#ifndef GL_SHORT
#define GL_SHORT 0x1402
#endif
#ifndef GL_UNSIGNED_SHORT
#define GL_UNSIGNED_SHORT 0x1403
#endif
#ifndef GL_BYTE
#define GL_BYTE 0x1400
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE 0x1401
#endif
#ifndef GL_MATRIX_MODE
#define GL_MATRIX_MODE 0x0BA0
#endif
#ifndef GL_MODELVIEW_MATRIX
#define GL_MODELVIEW_MATRIX 0x0BA6
#endif
#ifndef GL_PROJECTION_MATRIX
#define GL_PROJECTION_MATRIX 0x0BA7
#endif

typedef struct {
  int size;
  unsigned type;
  int stride;
  const void *ptr;
} GLClientPointerState;

unsigned deadspace_gl_take_draw_count(void) {
  return __sync_lock_test_and_set(&g_gl_draw_calls, 0);
}

static int env_enabled(const char *name) {
  const char *v = getenv(name);
  return v && *v && strcmp(v, "0") != 0;
}

void deadspace_gl_note_draw_call(void) {
  unsigned fbo = g_current_fbo;
  __sync_fetch_and_add(&g_gl_draw_calls, 1);
  if (fbo)
    __sync_fetch_and_add(&g_gl_draw_calls_fbo, 1);
  else
    __sync_fetch_and_add(&g_gl_draw_calls_default, 1);
}

void deadspace_gl_take_draw_stats(unsigned *total, unsigned *default_draws,
                                  unsigned *fbo_draws, unsigned *current_fbo) {
  if (total) *total = __sync_lock_test_and_set(&g_gl_draw_calls, 0);
  if (default_draws)
    *default_draws = __sync_lock_test_and_set(&g_gl_draw_calls_default, 0);
  if (fbo_draws) *fbo_draws = __sync_lock_test_and_set(&g_gl_draw_calls_fbo, 0);
  if (current_fbo) *current_fbo = g_current_fbo;
}

static int fbo_log_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) enabled = getenv("DS_FBOLOG") != NULL;
  return enabled;
}

static GLClientPointerState g_vertex_ptr;
static GLClientPointerState g_color_ptr;
static GLClientPointerState g_texcoord_ptr;
static volatile unsigned g_array_buffer;
static volatile unsigned g_element_array_buffer;

static int gl_read_component(const unsigned char *p, unsigned type, float *out) {
  switch (type) {
    case GL_FLOAT: {
      float v;
      memcpy(&v, p, sizeof(v));
      *out = v;
      return 4;
    }
    case GL_FIXED: {
      int32_t v;
      memcpy(&v, p, sizeof(v));
      *out = (float)v / 65536.0f;
      return 4;
    }
    case GL_SHORT: {
      int16_t v;
      memcpy(&v, p, sizeof(v));
      *out = (float)v;
      return 2;
    }
    case GL_UNSIGNED_SHORT: {
      uint16_t v;
      memcpy(&v, p, sizeof(v));
      *out = (float)v;
      return 2;
    }
    case GL_BYTE: {
      int8_t v;
      memcpy(&v, p, sizeof(v));
      *out = (float)v;
      return 1;
    }
    case GL_UNSIGNED_BYTE: {
      uint8_t v;
      memcpy(&v, p, sizeof(v));
      *out = (float)v;
      return 1;
    }
    default:
      *out = 0.0f;
      return 0;
  }
}

static void gl_describe_first_vertex(char *out, size_t outsz) {
  if (!out || outsz == 0) return;
  out[0] = 0;
  if (g_array_buffer || !g_vertex_ptr.ptr || (uintptr_t)g_vertex_ptr.ptr < 0x10000u) {
    snprintf(out, outsz, "vptr=%p vtype=0x%x vsize=%d vstride=%d abuf=%u ebuf=%u",
             g_vertex_ptr.ptr, g_vertex_ptr.type, g_vertex_ptr.size, g_vertex_ptr.stride,
             g_array_buffer, g_element_array_buffer);
    return;
  }

  float v[4] = {0, 0, 0, 1};
  const unsigned char *p = (const unsigned char *)g_vertex_ptr.ptr;
  int step = 0;
  int n = g_vertex_ptr.size;
  if (n < 0) n = 0;
  if (n > 4) n = 4;
  for (int i = 0; i < n; i++) {
    int bytes = gl_read_component(p + step, g_vertex_ptr.type, &v[i]);
    if (bytes <= 0) break;
    step += bytes;
  }

  snprintf(out, outsz,
           "vptr=%p vtype=0x%x vsize=%d vstride=%d abuf=%u ebuf=%u v0=%.3f,%.3f,%.3f,%.3f",
           g_vertex_ptr.ptr, g_vertex_ptr.type, g_vertex_ptr.size, g_vertex_ptr.stride,
           g_array_buffer, g_element_array_buffer, v[0], v[1], v[2], v[3]);
}

void deadspace_gl_prepare_draw(const char *kind, unsigned mode, int count) {
  deadspace_gl_note_draw_call();

  if (env_enabled("DS_FORCE_TEXTURE2D")) glEnable(GL_TEXTURE_2D);
  if (env_enabled("DS_FORCE_WHITE_DRAW")) {
    glDisableClientState(GL_COLOR_ARRAY);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  }

  if (!env_enabled("DS_DRAWLOG")) return;
  static int log_count;
  if (log_count >= 256) return;

  GLint tex_binding = 0, active = 0, client_active = 0;
  GLfloat color[4] = {0, 0, 0, 0};
  GLboolean tex2d = glIsEnabled(GL_TEXTURE_2D);
  GLboolean blend = glIsEnabled(GL_BLEND);
  GLboolean vertex_array = glIsEnabled(GL_VERTEX_ARRAY);
  GLboolean color_array = glIsEnabled(GL_COLOR_ARRAY);
  GLboolean texcoord_array = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
  GLint matrix_mode = 0;
  GLfloat proj[16];
  GLfloat model[16];
  memset(proj, 0, sizeof(proj));
  memset(model, 0, sizeof(model));
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex_binding);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
  glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &client_active);
  glGetIntegerv(GL_MATRIX_MODE, &matrix_mode);
  glGetFloatv(GL_CURRENT_COLOR, color);
  glGetFloatv(GL_PROJECTION_MATRIX, proj);
  glGetFloatv(GL_MODELVIEW_MATRIX, model);
  unsigned err = glGetError();
  char vertex_desc[256];
  gl_describe_first_vertex(vertex_desc, sizeof(vertex_desc));

  fprintf(stderr,
          "[draw] %s mode=0x%x count=%d fbo=%u tex=%d active=0x%x client=0x%x "
          "en=T%d/B%d arr=V%d/C%d/T%d color=%.2f,%.2f,%.2f,%.2f %s err=0x%x\n",
          kind ? kind : "?", mode, count, g_current_fbo, tex_binding, active, client_active,
          tex2d, blend, vertex_array, color_array, texcoord_array,
          color[0], color[1], color[2], color[3], vertex_desc, err);
  fprintf(stderr,
          "[matrix] mode=0x%x P=%.4f,%.4f,%.4f,%.4f T=%.4f,%.4f,%.4f M=%.4f,%.4f,%.4f,%.4f MT=%.4f,%.4f,%.4f\n",
          matrix_mode,
          proj[0], proj[5], proj[10], proj[15], proj[12], proj[13], proj[14],
          model[0], model[5], model[10], model[15], model[12], model[13], model[14]);
  log_count++;
}

static int b_log_print(int prio, const char *tag, const char *fmt, ...) {
  fprintf(stderr, "[ALOG:%d %s] ", prio, tag ? tag : "?");
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt ? fmt : "", ap);
  va_end(ap);
  fputc('\n', stderr);
  return 0;
}
static int b_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  fprintf(stderr, "[ALOG:%d %s] ", prio, tag ? tag : "?");
  vfprintf(stderr, fmt ? fmt : "", ap);
  fputc('\n', stderr);
  return 0;
}
static int b_log_write(int prio, const char *tag, const char *text) {
  fprintf(stderr, "[ALOG:%d %s] %s\n", prio, tag ? tag : "?", text ? text : "");
  return 0;
}

static int *b_errno(void) { return __errno_location(); }
static void b_stack_chk_fail(void) { fprintf(stderr, "[bionic] __stack_chk_fail ignored\n"); }
uintptr_t b_stack_chk_guard = 0x24d5c0deu;
uintptr_t b_dso_handle = 0;
unsigned int b_page_size = 4096;

static int b_aeabi_atexit(void *obj, void (*dtor)(void *), void *dso) {
  (void)obj;
  (void)dtor;
  (void)dso;
  return 0;
}

static int b_atomic_cmpxchg(int oldv, int newv, volatile int *ptr) {
  return __sync_val_compare_and_swap(ptr, oldv, newv) == oldv ? 0 : 1;
}
static int b_atomic_swap(int newv, volatile int *ptr) {
  return __sync_lock_test_and_set(ptr, newv);
}
static int b_atomic_inc(volatile int *ptr) { return __sync_fetch_and_add(ptr, 1); }
static int b_atomic_dec(volatile int *ptr) { return __sync_fetch_and_sub(ptr, 1); }

static const char *b_tmp_root(void) {
  const char *p = getenv("DEADSPACE_TMP");
  return (p && *p) ? p : "/tmp";
}
static const char *b_external_root(void) {
  const char *p = getenv("DEADSPACE_HOME");
  return (p && *p) ? p : ".";
}

static int file_log_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) enabled = getenv("DS_FILELOG") != NULL;
  return enabled;
}

static const char *asset_root(void) {
  const char *p = getenv("DEADSPACE_ASSETS");
  return (p && *p) ? p : "assets";
}

static const char *home_root(void) {
  const char *p = getenv("DEADSPACE_HOME");
  return (p && *p) ? p : ".";
}

static const char *strip_asset_prefix(const char *path) {
  const char *p = path ? path : "";
  const char *published = strstr(p, "/published/");
  if (published) return published + 1;
  const char *files = strstr(p, "/files/");
  if (files && files[7] != 0) return files + 7;
  const char *assets = strstr(p, "/assets/");
  if (assets) return assets + 8;
  while (*p == '/') p++;
  if (strncmp(p, "assets/", 7) == 0) p += 7;
  if (strncmp(p, "android_asset/", 14) == 0) p += 14;
  if (strncmp(p, "android_assets/", 15) == 0) p += 15;
  return p;
}

static int path_exists(const char *path) {
  return path && *path && access(path, F_OK) == 0;
}

static void join_path(char *out, size_t outsz, const char *root, const char *path) {
  const char *r = (root && *root) ? root : ".";
  const char *p = path ? path : "";
  while (*p == '/') p++;
  snprintf(out, outsz, "%s/%s", r, p);
}

int deadspace_resolve_read_path(const char *path, char *out, size_t outsz) {
  if (!out || outsz == 0) return 0;
  out[0] = 0;
  if (!path || !*path) return 0;

  if (path_exists(path)) {
    snprintf(out, outsz, "%s", path);
    return 1;
  }

  const char *p = strip_asset_prefix(path);
  char cand[1024];

  if (path[0] != '/') {
    join_path(cand, sizeof(cand), home_root(), path);
    if (path_exists(cand)) {
      snprintf(out, outsz, "%s", cand);
      return 1;
    }
  }

  join_path(cand, sizeof(cand), asset_root(), p);
  if (path_exists(cand)) {
    snprintf(out, outsz, "%s", cand);
    return 1;
  }

  if (strncmp(p, "published/", 10) != 0) {
    char rel[1024];
    snprintf(rel, sizeof(rel), "published/%s", p);
    join_path(cand, sizeof(cand), asset_root(), rel);
    if (path_exists(cand)) {
      snprintf(out, outsz, "%s", cand);
      return 1;
    }
  }

  snprintf(out, outsz, "%s", path);
  return 0;
}

int deadspace_resolve_write_path(const char *path, char *out, size_t outsz) {
  if (!out || outsz == 0) return 0;
  out[0] = 0;
  if (!path || !*path) return 0;
  const char *files = strstr(path, "/files/");
  if (files && files[7] != 0) {
    join_path(out, outsz, home_root(), files + 7);
    return 1;
  }
  if (path[0] == '/') {
    snprintf(out, outsz, "%s", path);
    return 1;
  }
  join_path(out, outsz, home_root(), path);
  return 1;
}

static int fopen_is_write_mode(const char *mode) {
  return mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'));
}

static FILE *b_fopen(const char *path, const char *mode) {
  char resolved[1024];
  if (fopen_is_write_mode(mode)) {
    deadspace_resolve_write_path(path, resolved, sizeof(resolved));
  } else {
    deadspace_resolve_read_path(path, resolved, sizeof(resolved));
  }
  FILE *fp = fopen(resolved[0] ? resolved : path, mode);
  if (file_log_enabled()) {
    fprintf(stderr, "[file] fopen %s mode=%s -> %s %s\n",
            path ? path : "", mode ? mode : "", resolved[0] ? resolved : "",
            fp ? "OK" : strerror(errno));
  }
  return fp;
}

static int b_open(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }

  char resolved[1024];
  if ((flags & (O_CREAT | O_WRONLY | O_RDWR)) != 0) {
    deadspace_resolve_write_path(path, resolved, sizeof(resolved));
  } else {
    deadspace_resolve_read_path(path, resolved, sizeof(resolved));
  }
  int fd = (flags & O_CREAT) ? open(resolved[0] ? resolved : path, flags, mode)
                             : open(resolved[0] ? resolved : path, flags);
  if (file_log_enabled()) {
    fprintf(stderr, "[file] open %s flags=0x%x -> %s fd=%d %s\n",
            path ? path : "", flags, resolved[0] ? resolved : "", fd,
            fd >= 0 ? "OK" : strerror(errno));
  }
  return fd;
}

static DIR *b_opendir(const char *path) {
  char resolved[1024];
  deadspace_resolve_read_path(path, resolved, sizeof(resolved));
  DIR *d = opendir(resolved[0] ? resolved : path);
  if (file_log_enabled()) {
    fprintf(stderr, "[file] opendir %s -> %s %s\n",
            path ? path : "", resolved[0] ? resolved : "",
            d ? "OK" : strerror(errno));
  }
  return d;
}

typedef struct {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
} BionicDirent;

static void *b_readdir(DIR *dir) {
  static __thread BionicDirent out;
  struct dirent *e = readdir(dir);
  if (!e) return NULL;
  memset(&out, 0, sizeof(out));
  out.d_ino = (uint64_t)e->d_ino;
  out.d_off = (int64_t)e->d_off;
  out.d_reclen = sizeof(out);
  out.d_type = e->d_type;
  snprintf(out.d_name, sizeof(out.d_name), "%s", e->d_name);
  if (getenv("DS_DIRLOG")) fprintf(stderr, "[file] readdir -> %s\n", out.d_name);
  return &out;
}

static int b_chdir(const char *path) {
  char resolved[1024];
  deadspace_resolve_read_path(path, resolved, sizeof(resolved));
  return chdir(resolved[0] ? resolved : path);
}
static int b_chmod(const char *path, mode_t mode) {
  char resolved[1024];
  deadspace_resolve_write_path(path, resolved, sizeof(resolved));
  return chmod(resolved[0] ? resolved : path, mode);
}
static int b_mkdir(const char *path, mode_t mode) {
  char resolved[1024];
  deadspace_resolve_write_path(path, resolved, sizeof(resolved));
  return mkdir(resolved[0] ? resolved : path, mode);
}
static int b_remove(const char *path) {
  char resolved[1024];
  deadspace_resolve_write_path(path, resolved, sizeof(resolved));
  return remove(resolved[0] ? resolved : path);
}
static int b_unlink(const char *path) {
  char resolved[1024];
  deadspace_resolve_write_path(path, resolved, sizeof(resolved));
  return unlink(resolved[0] ? resolved : path);
}
static int b_fsync(int fd) { return fsync(fd); }
static int b_ftruncate(int fd, off_t len) { return ftruncate(fd, len); }
static int b_execv(const char *path, char *const argv[]) {
  fprintf(stderr, "[stub] execv(%s) blocked\n", path ? path : "?");
  (void)argv;
  errno = ENOSYS;
  return -1;
}
static pid_t b_fork(void) {
  errno = ENOSYS;
  return -1;
}
static int b_raise(int sig) {
  fprintf(stderr, "[stub] raise(%d)\n", sig);
  if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) return 0;
  int (*real_raise)(int) = dlsym(RTLD_NEXT, "raise");
  return real_raise ? real_raise(sig) : 0;
}
static void b_abort(void) {
  fprintf(stderr, "[stub] abort()\n");
}
void *deadspace_raise_stub(void) { return (void *)b_raise; }
void *deadspace_abort_stub(void) { return (void *)b_abort; }

extern int _setjmp(void *);
extern void _longjmp(void *, int) __attribute__((noreturn));

extern size_t b_fwrite(const void *, size_t, size_t, void *);
extern size_t b_fread(void *, size_t, size_t, void *);
extern int b_fputs(const char *, void *);
extern int b_fputc(int, void *);
extern int b_fgetc(void *);
extern char *b_fgets(char *, int, void *);
extern int b_fseek(void *, long, int);
extern long b_ftell(void *);
extern int b_fflush(void *);
extern int b_fclose(void *);
extern int b_ungetc(int, void *);
extern int b_fwide(void *, int);
extern int b_fprintf(void *, const char *, ...);
extern int bionic_fstat(int, void *);
extern int bionic_stat(const char *, void *);
extern int bionic_lstat(const char *, void *);
extern int bionic_fstatat(int, const char *, void *, int);

static void *my_memcpy_chk(void *d, const void *s, size_t n, size_t dn) {
  (void)dn;
  return memcpy(d, s, n);
}
static void my_aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
static void my_aeabi_memmove(void *d, const void *s, size_t n) { memmove(d, s, n); }
static void my_aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }

#define DS_EXIDX_VADDR 0x4557d4u
#define DS_EXIDX_SIZE  0x1feb8u
#define DS_TEXT_SIZE   0x47568cu
static void *b_find_exidx(uintptr_t pc, int *pcount) {
  uintptr_t lb = g_load_base;
  if (lb && pc >= lb && pc < lb + DS_TEXT_SIZE) {
    if (pcount) *pcount = (int)(DS_EXIDX_SIZE / 8);
    return (void *)(lb + DS_EXIDX_VADDR);
  }
  static void *(*real)(uintptr_t, int *);
  if (!real) real = dlsym(RTLD_NEXT, "__gnu_Unwind_Find_exidx");
  if (real && real != b_find_exidx) return real(pc, pcount);
  if (pcount) *pcount = 0;
  return NULL;
}

static void *resolve_gl(const char *name) {
  void *p = dlsym(RTLD_DEFAULT, name);
  if (p) return p;
  void *(*eglGetProcAddress_p)(const char *) = dlsym(RTLD_DEFAULT, "eglGetProcAddress");
  if (eglGetProcAddress_p) return eglGetProcAddress_p(name);
  return NULL;
}

static void *glMapBufferOES_wrap(unsigned target, unsigned access) {
  static void *(*fn)(unsigned, unsigned);
  if (!fn) fn = resolve_gl("glMapBufferOES");
  return fn ? fn(target, access) : NULL;
}
static unsigned char glUnmapBufferOES_wrap(unsigned target) {
  static unsigned char (*fn)(unsigned);
  if (!fn) fn = resolve_gl("glUnmapBufferOES");
  return fn ? fn(target) : 1;
}
static void glBlendEquationOES_wrap(unsigned mode) {
  static void (*fn)(unsigned);
  if (!fn) fn = resolve_gl("glBlendEquation");
  if (fn) fn(mode);
}
static void glBlendEquationSeparateOES_wrap(unsigned rgb, unsigned alpha) {
  static void (*fn)(unsigned, unsigned);
  if (!fn) fn = resolve_gl("glBlendEquationSeparate");
  if (fn) fn(rgb, alpha);
}
static void glBlendFuncSeparateOES_wrap(unsigned src_rgb, unsigned dst_rgb,
                                        unsigned src_alpha, unsigned dst_alpha) {
  static void (*fn)(unsigned, unsigned, unsigned, unsigned);
  if (!fn) fn = resolve_gl("glBlendFuncSeparate");
  if (fn) fn(src_rgb, dst_rgb, src_alpha, dst_alpha);
}

static void glBindBuffer_wrap(unsigned target, unsigned buffer) {
  static void (*fn)(unsigned, unsigned);
  static int log_count;
  if (!fn) fn = resolve_gl("glBindBuffer");
  if (target == GL_ARRAY_BUFFER) g_array_buffer = buffer;
  else if (target == GL_ELEMENT_ARRAY_BUFFER) g_element_array_buffer = buffer;
  if (env_enabled("DS_BUFFERLOG") && log_count < 128) {
    fprintf(stderr, "[buf] bind target=0x%x buffer=%u%s\n", target, buffer, fn ? "" : " missing");
    log_count++;
  }
  if (fn) fn(target, buffer);
}

static void glBufferData_wrap(unsigned target, ptrdiff_t size, const void *data, unsigned usage) {
  static void (*fn)(unsigned, ptrdiff_t, const void *, unsigned);
  static int log_count;
  if (!fn) fn = resolve_gl("glBufferData");
  if (env_enabled("DS_BUFFERLOG") && log_count < 128) {
    fprintf(stderr, "[buf] data target=0x%x size=%ld data=%p usage=0x%x%s\n",
            target, (long)size, data, usage, fn ? "" : " missing");
    log_count++;
  }
  if (fn) fn(target, size, data, usage);
}

static void glBufferSubData_wrap(unsigned target, ptrdiff_t offset, ptrdiff_t size,
                                 const void *data) {
  static void (*fn)(unsigned, ptrdiff_t, ptrdiff_t, const void *);
  static int log_count;
  if (!fn) fn = resolve_gl("glBufferSubData");
  if (env_enabled("DS_BUFFERLOG") && log_count < 128) {
    fprintf(stderr, "[buf] subdata target=0x%x offset=%ld size=%ld data=%p%s\n",
            target, (long)offset, (long)size, data, fn ? "" : " missing");
    log_count++;
  }
  if (fn) fn(target, offset, size, data);
}

static void glGenBuffers_wrap(int n, unsigned *buffers) {
  static void (*fn)(int, unsigned *);
  if (!fn) fn = resolve_gl("glGenBuffers");
  if (fn) fn(n, buffers);
}

static void glDeleteBuffers_wrap(int n, const unsigned *buffers) {
  static void (*fn)(int, const unsigned *);
  if (!fn) fn = resolve_gl("glDeleteBuffers");
  if (fn) fn(n, buffers);
}

static void glVertexPointer_wrap(int size, unsigned type, int stride, const void *pointer) {
  static void (*fn)(int, unsigned, int, const void *);
  static int log_count;
  if (!fn) fn = resolve_gl("glVertexPointer");
  g_vertex_ptr.size = size;
  g_vertex_ptr.type = type;
  g_vertex_ptr.stride = stride;
  g_vertex_ptr.ptr = pointer;
  if (env_enabled("DS_BUFFERLOG") && log_count < 128) {
    fprintf(stderr, "[ptr] vertex size=%d type=0x%x stride=%d ptr=%p abuf=%u%s\n",
            size, type, stride, pointer, g_array_buffer, fn ? "" : " missing");
    log_count++;
  }
  if (fn) fn(size, type, stride, pointer);
}

static void glColorPointer_wrap(int size, unsigned type, int stride, const void *pointer) {
  static void (*fn)(int, unsigned, int, const void *);
  static int log_count;
  if (!fn) fn = resolve_gl("glColorPointer");
  g_color_ptr.size = size;
  g_color_ptr.type = type;
  g_color_ptr.stride = stride;
  g_color_ptr.ptr = pointer;
  if (env_enabled("DS_BUFFERLOG") && log_count < 128) {
    fprintf(stderr, "[ptr] color size=%d type=0x%x stride=%d ptr=%p abuf=%u%s\n",
            size, type, stride, pointer, g_array_buffer, fn ? "" : " missing");
    log_count++;
  }
  if (fn) fn(size, type, stride, pointer);
}

static void glTexCoordPointer_wrap(int size, unsigned type, int stride, const void *pointer) {
  static void (*fn)(int, unsigned, int, const void *);
  static int log_count;
  if (!fn) fn = resolve_gl("glTexCoordPointer");
  g_texcoord_ptr.size = size;
  g_texcoord_ptr.type = type;
  g_texcoord_ptr.stride = stride;
  g_texcoord_ptr.ptr = pointer;
  if (env_enabled("DS_BUFFERLOG") && log_count < 128) {
    fprintf(stderr, "[ptr] texcoord size=%d type=0x%x stride=%d ptr=%p abuf=%u%s\n",
            size, type, stride, pointer, g_array_buffer, fn ? "" : " missing");
    log_count++;
  }
  if (fn) fn(size, type, stride, pointer);
}

static void glViewport_wrap(int x, int y, int width, int height) {
  static void (*fn)(int, int, int, int);
  static int log_count;
  if (!fn) fn = resolve_gl("glViewport");
  if (width <= 0 || height <= 0) {
    if (getenv("DS_GLLOG") && log_count < 64) {
      fprintf(stderr, "[gl] viewport invalid %d,%d %dx%d -> 0,0 %dx%d\n",
              x, y, width, height, deadspace_screen_width(), deadspace_screen_height());
      log_count++;
    }
    x = 0;
    y = 0;
    width = deadspace_screen_width();
    height = deadspace_screen_height();
  } else if (getenv("DS_GLLOG") && log_count < 64) {
    fprintf(stderr, "[gl] viewport %d,%d %dx%d\n", x, y, width, height);
    log_count++;
  }
  if (fn) fn(x, y, width, height);
}

static void glScissor_wrap(int x, int y, int width, int height) {
  static void (*fn)(int, int, int, int);
  static int log_count;
  if (!fn) fn = resolve_gl("glScissor");
  if (width <= 0 || height <= 0) {
    if (getenv("DS_GLLOG") && log_count < 64) {
      fprintf(stderr, "[gl] scissor invalid %d,%d %dx%d -> disabled full %dx%d\n",
              x, y, width, height, deadspace_screen_width(), deadspace_screen_height());
      log_count++;
    }
    x = 0;
    y = 0;
    width = deadspace_screen_width();
    height = deadspace_screen_height();
  } else if (getenv("DS_GLLOG") && log_count < 64) {
    fprintf(stderr, "[gl] scissor %d,%d %dx%d\n", x, y, width, height);
    log_count++;
  }
  if (fn) fn(x, y, width, height);
}

static void glDrawArrays_wrap(unsigned mode, int first, int count) {
  static void (*fn)(unsigned, int, int);
  if (!fn) fn = resolve_gl("glDrawArrays");
  deadspace_gl_prepare_draw("arrays", mode, count);
  if (fn) fn(mode, first, count);
}

static void glDrawElements_wrap(unsigned mode, int count, unsigned type, const void *indices) {
  static void (*fn)(unsigned, int, unsigned, const void *);
  if (!fn) fn = resolve_gl("glDrawElements");
  deadspace_gl_prepare_draw("elements", mode, count);
  if (fn) fn(mode, count, type, indices);
}

static void glBindFramebufferOES_wrap(unsigned target, unsigned framebuffer) {
  static void (*fn)(unsigned, unsigned);
  static int log_count;
  if (!fn) fn = resolve_gl("glBindFramebufferOES");
  if (fn) fn(target, framebuffer);
  if (target == GL_FRAMEBUFFER_OES) g_current_fbo = framebuffer;
  if (fbo_log_enabled() && log_count < 128) {
    fprintf(stderr, "[fbo] bind target=0x%x fbo=%u%s\n", target, framebuffer, fn ? "" : " missing");
    log_count++;
  }
}

static void glGenFramebuffersOES_wrap(int n, unsigned *framebuffers) {
  static void (*fn)(int, unsigned *);
  static int log_count;
  if (!fn) fn = resolve_gl("glGenFramebuffersOES");
  if (fn) fn(n, framebuffers);
  if (fbo_log_enabled() && log_count < 32) {
    fprintf(stderr, "[fbo] gen n=%d first=%u%s\n", n,
            (n > 0 && framebuffers) ? framebuffers[0] : 0, fn ? "" : " missing");
    log_count++;
  }
}

static void glDeleteFramebuffersOES_wrap(int n, const unsigned *framebuffers) {
  static void (*fn)(int, const unsigned *);
  static int log_count;
  if (!fn) fn = resolve_gl("glDeleteFramebuffersOES");
  if (fn) fn(n, framebuffers);
  if (fbo_log_enabled() && log_count < 32) {
    fprintf(stderr, "[fbo] delete n=%d first=%u%s\n", n,
            (n > 0 && framebuffers) ? framebuffers[0] : 0, fn ? "" : " missing");
    log_count++;
  }
}

static unsigned char glIsFramebufferOES_wrap(unsigned framebuffer) {
  static unsigned char (*fn)(unsigned);
  if (!fn) fn = resolve_gl("glIsFramebufferOES");
  return fn ? fn(framebuffer) : (framebuffer != 0);
}

static unsigned glCheckFramebufferStatusOES_wrap(unsigned target) {
  static unsigned (*fn)(unsigned);
  static int log_count;
  if (!fn) fn = resolve_gl("glCheckFramebufferStatusOES");
  unsigned status = fn ? fn(target) : GL_FRAMEBUFFER_COMPLETE_OES;
  if (fbo_log_enabled() && log_count < 128) {
    fprintf(stderr, "[fbo] status target=0x%x -> 0x%x%s\n", target, status, fn ? "" : " missing");
    log_count++;
  }
  return status;
}

static void glBindRenderbufferOES_wrap(unsigned target, unsigned renderbuffer) {
  static void (*fn)(unsigned, unsigned);
  static int log_count;
  if (!fn) fn = resolve_gl("glBindRenderbufferOES");
  if (fn) fn(target, renderbuffer);
  if (fbo_log_enabled() && log_count < 128) {
    fprintf(stderr, "[fbo] bind-rb target=0x%x rb=%u%s\n", target, renderbuffer, fn ? "" : " missing");
    log_count++;
  }
}

static void glGenRenderbuffersOES_wrap(int n, unsigned *renderbuffers) {
  static void (*fn)(int, unsigned *);
  static int log_count;
  if (!fn) fn = resolve_gl("glGenRenderbuffersOES");
  if (fn) fn(n, renderbuffers);
  if (fbo_log_enabled() && log_count < 32) {
    fprintf(stderr, "[fbo] gen-rb n=%d first=%u%s\n", n,
            (n > 0 && renderbuffers) ? renderbuffers[0] : 0, fn ? "" : " missing");
    log_count++;
  }
}

static void glDeleteRenderbuffersOES_wrap(int n, const unsigned *renderbuffers) {
  static void (*fn)(int, const unsigned *);
  static int log_count;
  if (!fn) fn = resolve_gl("glDeleteRenderbuffersOES");
  if (fn) fn(n, renderbuffers);
  if (fbo_log_enabled() && log_count < 32) {
    fprintf(stderr, "[fbo] delete-rb n=%d first=%u%s\n", n,
            (n > 0 && renderbuffers) ? renderbuffers[0] : 0, fn ? "" : " missing");
    log_count++;
  }
}

static unsigned char glIsRenderbufferOES_wrap(unsigned renderbuffer) {
  static unsigned char (*fn)(unsigned);
  if (!fn) fn = resolve_gl("glIsRenderbufferOES");
  return fn ? fn(renderbuffer) : (renderbuffer != 0);
}

static void glRenderbufferStorageOES_wrap(unsigned target, unsigned internalformat,
                                          int width, int height) {
  static void (*fn)(unsigned, unsigned, int, int);
  static int log_count;
  if (!fn) fn = resolve_gl("glRenderbufferStorageOES");
  if (fbo_log_enabled() && log_count < 128) {
    fprintf(stderr, "[fbo] rb-storage target=0x%x fmt=0x%x %dx%d%s\n",
            target, internalformat, width, height, fn ? "" : " missing");
    log_count++;
  }
  if (fn) fn(target, internalformat, width, height);
}

static void glFramebufferTexture2DOES_wrap(unsigned target, unsigned attachment,
                                           unsigned textarget, unsigned texture, int level) {
  static void (*fn)(unsigned, unsigned, unsigned, unsigned, int);
  static int log_count;
  if (!fn) fn = resolve_gl("glFramebufferTexture2DOES");
  if (fbo_log_enabled() && log_count < 128) {
    fprintf(stderr, "[fbo] attach-tex target=0x%x att=0x%x texTarget=0x%x tex=%u level=%d%s\n",
            target, attachment, textarget, texture, level, fn ? "" : " missing");
    log_count++;
  }
  if (fn) fn(target, attachment, textarget, texture, level);
}

static void glFramebufferRenderbufferOES_wrap(unsigned target, unsigned attachment,
                                              unsigned renderbuffertarget,
                                              unsigned renderbuffer) {
  static void (*fn)(unsigned, unsigned, unsigned, unsigned);
  static int log_count;
  if (!fn) fn = resolve_gl("glFramebufferRenderbufferOES");
  if (fbo_log_enabled() && log_count < 128) {
    fprintf(stderr, "[fbo] attach-rb target=0x%x att=0x%x rbTarget=0x%x rb=%u%s\n",
            target, attachment, renderbuffertarget, renderbuffer, fn ? "" : " missing");
    log_count++;
  }
  if (fn) fn(target, attachment, renderbuffertarget, renderbuffer);
}

static void glGetRenderbufferParameterivOES_wrap(unsigned target, unsigned pname, int *params) {
  static void (*fn)(unsigned, unsigned, int *);
  if (!fn) fn = resolve_gl("glGetRenderbufferParameterivOES");
  if (fn) fn(target, pname, params);
  else if (params) *params = 0;
}

static void glGetFramebufferAttachmentParameterivOES_wrap(unsigned target, unsigned attachment,
                                                          unsigned pname, int *params) {
  static void (*fn)(unsigned, unsigned, unsigned, int *);
  if (!fn) fn = resolve_gl("glGetFramebufferAttachmentParameterivOES");
  if (fn) fn(target, attachment, pname, params);
  else if (params) *params = 0;
}

static void glDrawTexiOES_wrap(int x, int y, int z, int width, int height) {
  static void (*fn)(int, int, int, int, int);
  if (!fn) fn = resolve_gl("glDrawTexiOES");
  deadspace_gl_prepare_draw("drawtexi", 0, 1);
  if (fn) fn(x, y, z, width, height);
}

static void glDrawTexivOES_wrap(const int *coords) {
  static void (*fn)(const int *);
  if (!fn) fn = resolve_gl("glDrawTexivOES");
  deadspace_gl_prepare_draw("drawtexiv", 0, 1);
  if (fn) fn(coords);
}

static void glDrawTexsOES_wrap(short x, short y, short z, short width, short height) {
  static void (*fn)(short, short, short, short, short);
  if (!fn) fn = resolve_gl("glDrawTexsOES");
  deadspace_gl_prepare_draw("drawtexs", 0, 1);
  if (fn) fn(x, y, z, width, height);
}

static void glDrawTexsvOES_wrap(const short *coords) {
  static void (*fn)(const short *);
  if (!fn) fn = resolve_gl("glDrawTexsvOES");
  deadspace_gl_prepare_draw("drawtexsv", 0, 1);
  if (fn) fn(coords);
}

static void glDrawTexxOES_wrap(int x, int y, int z, int width, int height) {
  static void (*fn)(int, int, int, int, int);
  if (!fn) fn = resolve_gl("glDrawTexxOES");
  deadspace_gl_prepare_draw("drawtexx", 0, 1);
  if (fn) fn(x, y, z, width, height);
}

static void glDrawTexxvOES_wrap(const int *coords) {
  static void (*fn)(const int *);
  if (!fn) fn = resolve_gl("glDrawTexxvOES");
  deadspace_gl_prepare_draw("drawtexxv", 0, 1);
  if (fn) fn(coords);
}

static void glDrawTexfvOES_wrap(const float *coords) {
  static void (*fn)(const float *);
  if (!fn) fn = resolve_gl("glDrawTexfvOES");
  deadspace_gl_prepare_draw("drawtexfv", 0, 1);
  if (fn) fn(coords);
}

static int palette_format_info(unsigned internalformat, int *bits, int *entry_bytes) {
  switch (internalformat) {
    case GL_PALETTE4_RGB8_OES: *bits = 4; *entry_bytes = 3; return 1;
    case GL_PALETTE4_RGBA8_OES: *bits = 4; *entry_bytes = 4; return 1;
    case GL_PALETTE4_R5_G6_B5_OES: *bits = 4; *entry_bytes = 2; return 1;
    case GL_PALETTE4_RGBA4_OES: *bits = 4; *entry_bytes = 2; return 1;
    case GL_PALETTE4_RGB5_A1_OES: *bits = 4; *entry_bytes = 2; return 1;
    case GL_PALETTE8_RGB8_OES: *bits = 8; *entry_bytes = 3; return 1;
    case GL_PALETTE8_RGBA8_OES: *bits = 8; *entry_bytes = 4; return 1;
    case GL_PALETTE8_R5_G6_B5_OES: *bits = 8; *entry_bytes = 2; return 1;
    case GL_PALETTE8_RGBA4_OES: *bits = 8; *entry_bytes = 2; return 1;
    case GL_PALETTE8_RGB5_A1_OES: *bits = 8; *entry_bytes = 2; return 1;
    default: return 0;
  }
}

static void palette_read_rgba(unsigned internalformat, const unsigned char *pal,
                              int idx, unsigned char *rgba) {
  switch (internalformat) {
    case GL_PALETTE4_RGB8_OES:
    case GL_PALETTE8_RGB8_OES:
      rgba[0] = pal[idx * 3 + 0];
      rgba[1] = pal[idx * 3 + 1];
      rgba[2] = pal[idx * 3 + 2];
      rgba[3] = 255;
      break;
    case GL_PALETTE4_RGBA8_OES:
    case GL_PALETTE8_RGBA8_OES:
      rgba[0] = pal[idx * 4 + 0];
      rgba[1] = pal[idx * 4 + 1];
      rgba[2] = pal[idx * 4 + 2];
      rgba[3] = pal[idx * 4 + 3];
      break;
    case GL_PALETTE4_R5_G6_B5_OES:
    case GL_PALETTE8_R5_G6_B5_OES: {
      uint16_t v = (uint16_t)pal[idx * 2 + 0] | ((uint16_t)pal[idx * 2 + 1] << 8);
      rgba[0] = (unsigned char)((((v >> 11) & 31) * 255 + 15) / 31);
      rgba[1] = (unsigned char)((((v >> 5) & 63) * 255 + 31) / 63);
      rgba[2] = (unsigned char)(((v & 31) * 255 + 15) / 31);
      rgba[3] = 255;
      break;
    }
    case GL_PALETTE4_RGBA4_OES:
    case GL_PALETTE8_RGBA4_OES: {
      uint16_t v = (uint16_t)pal[idx * 2 + 0] | ((uint16_t)pal[idx * 2 + 1] << 8);
      rgba[0] = (unsigned char)((((v >> 12) & 15) * 255 + 7) / 15);
      rgba[1] = (unsigned char)((((v >> 8) & 15) * 255 + 7) / 15);
      rgba[2] = (unsigned char)((((v >> 4) & 15) * 255 + 7) / 15);
      rgba[3] = (unsigned char)(((v & 15) * 255 + 7) / 15);
      break;
    }
    case GL_PALETTE4_RGB5_A1_OES:
    case GL_PALETTE8_RGB5_A1_OES: {
      uint16_t v = (uint16_t)pal[idx * 2 + 0] | ((uint16_t)pal[idx * 2 + 1] << 8);
      rgba[0] = (unsigned char)((((v >> 11) & 31) * 255 + 15) / 31);
      rgba[1] = (unsigned char)((((v >> 6) & 31) * 255 + 15) / 31);
      rgba[2] = (unsigned char)((((v >> 1) & 31) * 255 + 15) / 31);
      rgba[3] = (v & 1) ? 255 : 0;
      break;
    }
    default:
      rgba[0] = rgba[1] = rgba[2] = 0;
      rgba[3] = 255;
      break;
  }
}

static int glCompressedTexImage2D_palette_fallback(unsigned target, int level,
                                                   unsigned internalformat,
                                                   int width, int height,
                                                   int border, int imageSize,
                                                   const void *data) {
  int bits = 0, entry_bytes = 0;
  if (!data || width <= 0 || height <= 0 ||
      !palette_format_info(internalformat, &bits, &entry_bytes))
    return 0;

  int entries = bits == 4 ? 16 : 256;
  int palette_bytes = entries * entry_bytes;
  if (imageSize <= palette_bytes) return 0;

  const unsigned char *src = (const unsigned char *)data;
  const unsigned char *pal = src;
  const unsigned char *idx = src + palette_bytes;
  int remaining = imageSize - palette_bytes;
  int w = width;
  int h = height;
  int mip = level;
  int uploaded = 0;

  while (w > 0 && h > 0 && remaining > 0) {
    int pixels = w * h;
    int idx_bytes = bits == 4 ? (pixels + 1) / 2 : pixels;
    if (idx_bytes > remaining) break;

    unsigned char *rgba = malloc((size_t)pixels * 4u);
    if (!rgba) break;

    for (int i = 0; i < pixels; i++) {
      int pi;
      if (bits == 8) {
        pi = idx[i];
      } else {
        unsigned char b = idx[i >> 1];
        pi = (i & 1) ? (b & 0x0f) : (b >> 4);
      }
      if (pi >= entries) pi = 0;
      palette_read_rgba(internalformat, pal, pi, rgba + i * 4);
    }

    glTexImage2D(target, mip, GL_RGBA, w, h, border, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
    uploaded++;

    idx += idx_bytes;
    remaining -= idx_bytes;
    w = w > 1 ? w >> 1 : 1;
    h = h > 1 ? h >> 1 : 1;
    mip++;
    if (w == 1 && h == 1 && remaining <= 0) break;
  }

  if (uploaded > 0) {
    if (uploaded == 1) {
      glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    if (getenv("DS_TEXLOG")) {
      fprintf(stderr, "[tex] paletted fallback fmt=0x%x %dx%d level=%d size=%d mips=%d\n",
              internalformat, width, height, level, imageSize, uploaded);
    }
    return 1;
  }
  return 0;
}

static void glCompressedTexImage2D_wrap(unsigned target, int level, unsigned internalformat,
                                        int width, int height, int border, int imageSize,
                                        const void *data) {
  static void (*fn)(unsigned, int, unsigned, int, int, int, int, const void *);
  static int log_count;
  if (!fn) fn = resolve_gl("glCompressedTexImage2D");

  if (glCompressedTexImage2D_palette_fallback(target, level, internalformat, width, height,
                                              border, imageSize, data)) {
    return;
  }

  if (getenv("DS_TEXLOG") && log_count < 128) {
    fprintf(stderr, "[tex] compressed pass fmt=0x%x %dx%d level=%d size=%d\n",
            internalformat, width, height, level, imageSize);
    log_count++;
  }
  if (fn) fn(target, level, internalformat, width, height, border, imageSize, data);
  if (getenv("DS_TEXLOG") && log_count < 128) {
    unsigned err = glGetError();
    if (err) fprintf(stderr, "[tex] compressed err=0x%x fmt=0x%x\n", err, internalformat);
  }
}

DynLibFunction deadspace_overrides[] = {
    {"__android_log_print", (uintptr_t)b_log_print},
    {"__android_log_vprint", (uintptr_t)b_log_vprint},
    {"__android_log_write", (uintptr_t)b_log_write},
    {"abort", (uintptr_t)b_abort},
    {"setjmp", (uintptr_t)_setjmp},
    {"longjmp", (uintptr_t)_longjmp},
    {"__errno", (uintptr_t)b_errno},
    {"__stack_chk_fail", (uintptr_t)b_stack_chk_fail},
    {"__stack_chk_guard", (uintptr_t)&b_stack_chk_guard},
    {"__dso_handle", (uintptr_t)&b_dso_handle},
    {"__page_size", (uintptr_t)&b_page_size},
    {"__aeabi_atexit", (uintptr_t)b_aeabi_atexit},
    {"__gnu_Unwind_Find_exidx", (uintptr_t)b_find_exidx},
    {"__atomic_cmpxchg", (uintptr_t)b_atomic_cmpxchg},
    {"__atomic_swap", (uintptr_t)b_atomic_swap},
    {"__atomic_inc", (uintptr_t)b_atomic_inc},
    {"__atomic_dec", (uintptr_t)b_atomic_dec},
    {"_Z17androidGetTmpRootv", (uintptr_t)b_tmp_root},
    {"_Z22androidGetExternalRootv", (uintptr_t)b_external_root},

    {"fwrite", (uintptr_t)b_fwrite},
    {"fread", (uintptr_t)b_fread},
    {"fputs", (uintptr_t)b_fputs},
    {"fputc", (uintptr_t)b_fputc},
    {"fgetc", (uintptr_t)b_fgetc},
    {"fgets", (uintptr_t)b_fgets},
    {"fseek", (uintptr_t)b_fseek},
    {"ftell", (uintptr_t)b_ftell},
    {"fflush", (uintptr_t)b_fflush},
    {"fclose", (uintptr_t)b_fclose},
    {"ungetc", (uintptr_t)b_ungetc},
    {"fwide", (uintptr_t)b_fwide},
    {"fprintf", (uintptr_t)b_fprintf},
    {"fstat", (uintptr_t)bionic_fstat},
    {"stat", (uintptr_t)bionic_stat},
    {"lstat", (uintptr_t)bionic_lstat},
    {"fstatat", (uintptr_t)bionic_fstatat},

    {"__memcpy_chk", (uintptr_t)my_memcpy_chk},
    {"__aeabi_memcpy", (uintptr_t)my_aeabi_memcpy},
    {"__aeabi_memcpy4", (uintptr_t)my_aeabi_memcpy},
    {"__aeabi_memcpy8", (uintptr_t)my_aeabi_memcpy},
    {"__aeabi_memmove", (uintptr_t)my_aeabi_memmove},
    {"__aeabi_memmove4", (uintptr_t)my_aeabi_memmove},
    {"__aeabi_memmove8", (uintptr_t)my_aeabi_memmove},
    {"__aeabi_memset", (uintptr_t)my_aeabi_memset},
    {"__aeabi_memset4", (uintptr_t)my_aeabi_memset},
    {"__aeabi_memset8", (uintptr_t)my_aeabi_memset},

    {"fopen", (uintptr_t)b_fopen},
    {"open", (uintptr_t)b_open},
    {"opendir", (uintptr_t)b_opendir},
    {"readdir", (uintptr_t)b_readdir},
    {"chdir", (uintptr_t)b_chdir},
    {"chmod", (uintptr_t)b_chmod},
    {"mkdir", (uintptr_t)b_mkdir},
    {"remove", (uintptr_t)b_remove},
    {"unlink", (uintptr_t)b_unlink},
    {"execv", (uintptr_t)b_execv},
    {"fork", (uintptr_t)b_fork},
    {"raise", (uintptr_t)b_raise},
    {"fsync", (uintptr_t)b_fsync},
    {"ftruncate", (uintptr_t)b_ftruncate},
    {"glMapBufferOES", (uintptr_t)glMapBufferOES_wrap},
    {"glUnmapBufferOES", (uintptr_t)glUnmapBufferOES_wrap},
    {"glBlendEquationOES", (uintptr_t)glBlendEquationOES_wrap},
    {"glBlendEquationSeparateOES", (uintptr_t)glBlendEquationSeparateOES_wrap},
    {"glBlendFuncSeparateOES", (uintptr_t)glBlendFuncSeparateOES_wrap},
    {"glBindBuffer", (uintptr_t)glBindBuffer_wrap},
    {"glBufferData", (uintptr_t)glBufferData_wrap},
    {"glBufferSubData", (uintptr_t)glBufferSubData_wrap},
    {"glGenBuffers", (uintptr_t)glGenBuffers_wrap},
    {"glDeleteBuffers", (uintptr_t)glDeleteBuffers_wrap},
    {"glVertexPointer", (uintptr_t)glVertexPointer_wrap},
    {"glColorPointer", (uintptr_t)glColorPointer_wrap},
    {"glTexCoordPointer", (uintptr_t)glTexCoordPointer_wrap},
    {"glViewport", (uintptr_t)glViewport_wrap},
    {"glScissor", (uintptr_t)glScissor_wrap},
    {"glDrawArrays", (uintptr_t)glDrawArrays_wrap},
    {"glDrawElements", (uintptr_t)glDrawElements_wrap},
    {"glBindFramebufferOES", (uintptr_t)glBindFramebufferOES_wrap},
    {"glGenFramebuffersOES", (uintptr_t)glGenFramebuffersOES_wrap},
    {"glDeleteFramebuffersOES", (uintptr_t)glDeleteFramebuffersOES_wrap},
    {"glIsFramebufferOES", (uintptr_t)glIsFramebufferOES_wrap},
    {"glCheckFramebufferStatusOES", (uintptr_t)glCheckFramebufferStatusOES_wrap},
    {"glBindRenderbufferOES", (uintptr_t)glBindRenderbufferOES_wrap},
    {"glGenRenderbuffersOES", (uintptr_t)glGenRenderbuffersOES_wrap},
    {"glDeleteRenderbuffersOES", (uintptr_t)glDeleteRenderbuffersOES_wrap},
    {"glIsRenderbufferOES", (uintptr_t)glIsRenderbufferOES_wrap},
    {"glRenderbufferStorageOES", (uintptr_t)glRenderbufferStorageOES_wrap},
    {"glFramebufferTexture2DOES", (uintptr_t)glFramebufferTexture2DOES_wrap},
    {"glFramebufferRenderbufferOES", (uintptr_t)glFramebufferRenderbufferOES_wrap},
    {"glGetRenderbufferParameterivOES", (uintptr_t)glGetRenderbufferParameterivOES_wrap},
    {"glGetFramebufferAttachmentParameterivOES", (uintptr_t)glGetFramebufferAttachmentParameterivOES_wrap},
    {"glDrawTexiOES", (uintptr_t)glDrawTexiOES_wrap},
    {"glDrawTexivOES", (uintptr_t)glDrawTexivOES_wrap},
    {"glDrawTexsOES", (uintptr_t)glDrawTexsOES_wrap},
    {"glDrawTexsvOES", (uintptr_t)glDrawTexsvOES_wrap},
    {"glDrawTexxOES", (uintptr_t)glDrawTexxOES_wrap},
    {"glDrawTexxvOES", (uintptr_t)glDrawTexxvOES_wrap},
    {"glDrawTexfvOES", (uintptr_t)glDrawTexfvOES_wrap},
    {"glCompressedTexImage2D", (uintptr_t)glCompressedTexImage2D_wrap},
};

const int deadspace_overrides_count =
    sizeof(deadspace_overrides) / sizeof(deadspace_overrides[0]);
