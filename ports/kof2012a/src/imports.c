#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <GLES/gl.h>

#include "imports.h"
#include "opensles_shim.h"
#include "util.h"

#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING 0x8894
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER_BINDING
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#endif

extern int __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle);
extern void __cxa_finalize(void *dso_handle);
extern int __cxa_guard_acquire(void *guard);
extern void __cxa_guard_release(void *guard);
extern void *_Znwm(size_t);
extern void *_Znam(size_t);
extern void _ZdlPv(void *);
extern void _ZdaPv(void *);

static uint64_t __stack_chk_guard_fake = 0x4b4f463230313241ULL;

static void __stack_chk_fail_stub(void) {
  debugPrintf("[stack] __stack_chk_fail ignored\n");
}

int __android_log_print_fake(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
  va_list ap;
  char buf[2048];
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap);
  va_end(ap);
  debugPrintf("[alog:%s] %s\n", tag ? tag : "?", buf);
  return 0;
}

static FILE *fopen_fake(const char *pathname, const char *mode) {
  return fopen(resolve_android_path(pathname), mode);
}

typedef struct HostMutexEntry {
  void *guest;
  pthread_mutex_t mutex;
  struct HostMutexEntry *next;
} HostMutexEntry;

static HostMutexEntry *g_mutexes;
static pthread_mutex_t g_mutex_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *host_mutex(void *guest, int create) {
  if (!guest) return NULL;
  pthread_mutex_lock(&g_mutex_lock);
  for (HostMutexEntry *e = g_mutexes; e; e = e->next) {
    if (e->guest == guest) {
      pthread_mutex_unlock(&g_mutex_lock);
      return &e->mutex;
    }
  }
  if (!create) {
    pthread_mutex_unlock(&g_mutex_lock);
    return NULL;
  }
  HostMutexEntry *e = calloc(1, sizeof(*e));
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&e->mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  e->guest = guest;
  e->next = g_mutexes;
  g_mutexes = e;
  pthread_mutex_unlock(&g_mutex_lock);
  return &e->mutex;
}

int pthread_mutex_init_fake(pthread_mutex_t *m, const void *attr) {
  (void)attr;
  return host_mutex(m, 1) ? 0 : -1;
}

int pthread_mutex_destroy_fake(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_lock_fake(pthread_mutex_t *m) { return pthread_mutex_lock(host_mutex(m, 1)); }
int pthread_mutex_unlock_fake(pthread_mutex_t *m) { return pthread_mutex_unlock(host_mutex(m, 1)); }

typedef struct {
  void *(*entry)(void *);
  void *arg;
} ThreadStart;

static void *thread_start(void *p) {
  ThreadStart *ts = p;
  void *(*entry)(void *) = ts->entry;
  void *arg = ts->arg;
  free(ts);
  return entry(arg);
}

int pthread_create_fake(pthread_t *thread, const void *attr, void *entry, void *arg) {
  (void)attr;
  ThreadStart *ts = malloc(sizeof(*ts));
  ts->entry = entry;
  ts->arg = arg;
  pthread_attr_t real_attr;
  pthread_attr_init(&real_attr);
  pthread_attr_setstacksize(&real_attr, 2 * 1024 * 1024);
  int ret = pthread_create(thread, &real_attr, thread_start, ts);
  pthread_attr_destroy(&real_attr);
  if (ret) free(ts);
  return ret;
}

typedef struct {
  FILE *f;
  size_t size;
  char path[2048];
} FakeAsset;

void *AAssetManager_fromJava_fake(void *env, void *assetManager) {
  (void)env; (void)assetManager;
  return (void *)0x4b0f2012;
}

static FILE *open_asset_path(const char *filename, char *out, size_t outsz) {
  const char *name = filename ? filename : "";
  const char *rel = name;
  if (!strncmp(rel, "assets/", 7)) rel += 7;
  if (!strncmp(rel, "./assets/", 9)) rel += 9;

  if (snprintf(out, outsz, "./assets/%s", rel) < (int)outsz) {
    FILE *f = fopen(out, "rb");
    if (f) return f;
  }
  if (snprintf(out, outsz, "./assets/data/%s", rel) < (int)outsz) {
    FILE *f = fopen(out, "rb");
    if (f) return f;
  }
  const char *base = strrchr(rel, '/');
  base = base ? base + 1 : rel;
  if (snprintf(out, outsz, "./assets/%s", base) < (int)outsz) {
    FILE *f = fopen(out, "rb");
    if (f) return f;
  }
  if (snprintf(out, outsz, "./assets/data/%s", base) < (int)outsz) {
    FILE *f = fopen(out, "rb");
    if (f) return f;
  }
  snprintf(out, outsz, "%s", name);
  return fopen(out, "rb");
}

void *AAssetManager_open_fake(void *mgr, const char *filename, int mode) {
  (void)mgr; (void)mode;
  char path[2048];
  FILE *f = open_asset_path(filename, path, sizeof(path));
  if (!f) {
    if (getenv("KOF_ASSETLOG"))
      debugPrintf("[asset] miss %s\n", filename ? filename : "(null)");
    return NULL;
  }
  FakeAsset *a = calloc(1, sizeof(*a));
  a->f = f;
  snprintf(a->path, sizeof(a->path), "%s", path);
  fseek(f, 0, SEEK_END);
  a->size = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  if (getenv("KOF_ASSETLOG"))
    debugPrintf("[asset] open %s -> %s (%zu)\n", filename, path, a->size);
  return a;
}

int AAsset_read_fake(void *asset, void *buf, size_t count) {
  FakeAsset *a = asset;
  if (!a || !a->f) return -1;
  return (int)fread(buf, 1, count, a->f);
}

void AAsset_close_fake(void *asset) {
  FakeAsset *a = asset;
  if (!a) return;
  if (a->f) fclose(a->f);
  free(a);
}

off_t AAsset_getLength_fake(void *asset) {
  FakeAsset *a = asset;
  return a ? (off_t)a->size : 0;
}

off_t AAsset_seek_fake(void *asset, off_t offset, int whence) {
  FakeAsset *a = asset;
  if (!a || !a->f) return -1;
  if (fseek(a->f, offset, whence) != 0) return -1;
  return (off_t)ftell(a->f);
}

int AAsset_openFileDescriptor_fake(void *asset, off_t *outStart, off_t *outLength) {
  FakeAsset *a = asset;
  if (!a || !a->f) return -1;
  if (outStart) *outStart = 0;
  if (outLength) *outLength = (off_t)a->size;
  int fd = open(a->path, O_RDONLY);
  return fd;
}

static int gl_log_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *v = getenv("KOF_GLLOG");
    cached = v && v[0] && strcmp(v, "0") != 0;
  }
  return cached;
}

static GLenum drain_gl_error(void) {
  GLenum first = GL_NO_ERROR;
  for (;;) {
    GLenum err = glGetError();
    if (err == GL_NO_ERROR)
      break;
    if (first == GL_NO_ERROR)
      first = err;
  }
  return first;
}

static void log_gl_state_after_draw(void) {
  GLint array_buffer = -1;
  GLint element_buffer = -1;
  GLint texture = -1;
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
  glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_buffer);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
  debugPrintf("[glstate] vertex=%d texcoord=%d color=%d normal=%d tex2d=%d "
              "array_buf=%d elem_buf=%d texture=%d\n",
              glIsEnabled(GL_VERTEX_ARRAY) ? 1 : 0,
              glIsEnabled(GL_TEXTURE_COORD_ARRAY) ? 1 : 0,
              glIsEnabled(GL_COLOR_ARRAY) ? 1 : 0,
              glIsEnabled(GL_NORMAL_ARRAY) ? 1 : 0,
              glIsEnabled(GL_TEXTURE_2D) ? 1 : 0, array_buffer,
              element_buffer, texture);
  drain_gl_error();
}

static void log_gl_result(const char *fn, unsigned call, const char *details,
                          GLenum pre_err, int is_draw) {
  if (!gl_log_enabled())
    return;

  GLenum post_err = drain_gl_error();
  if (pre_err || post_err || call < 80) {
    debugPrintf("[gl] %s #%u %s pre=0x%x post=0x%x\n", fn, call, details,
                pre_err, post_err);
    if (is_draw && (pre_err || post_err))
      log_gl_state_after_draw();
  }
}

static void glCompressedTexImage2D_log(GLenum target, GLint level,
                                       GLenum internalformat, GLsizei width,
                                       GLsizei height, GLint border,
                                       GLsizei imageSize,
                                       const GLvoid *data) {
  static unsigned calls;
  unsigned call = ++calls;
  GLenum pre_err = gl_log_enabled() ? drain_gl_error() : GL_NO_ERROR;
  glCompressedTexImage2D(target, level, internalformat, width, height, border,
                         imageSize, data);
  char details[160];
  snprintf(details, sizeof(details),
           "target=0x%x level=%d fmt=0x%x %dx%d border=%d size=%d data=%p",
           target, level, internalformat, width, height, border, imageSize,
           data);
  log_gl_result("glCompressedTexImage2D", call, details, pre_err, 0);
}

static void glBindTexture_log(GLenum target, GLuint texture) {
  static unsigned calls;
  unsigned call = ++calls;
  GLenum pre_err = gl_log_enabled() ? drain_gl_error() : GL_NO_ERROR;
  glBindTexture(target, texture);
  char details[80];
  snprintf(details, sizeof(details), "target=0x%x texture=%u", target,
           texture);
  log_gl_result("glBindTexture", call, details, pre_err, 0);
}

static void glTexParameterx_log(GLenum target, GLenum pname, GLfixed param) {
  static unsigned calls;
  unsigned call = ++calls;
  GLenum pre_err = gl_log_enabled() ? drain_gl_error() : GL_NO_ERROR;
  glTexParameterx(target, pname, param);
  char details[96];
  snprintf(details, sizeof(details), "target=0x%x pname=0x%x param=0x%x",
           target, pname, (unsigned)param);
  log_gl_result("glTexParameterx", call, details, pre_err, 0);
}

static void glTexImage2D_log(GLenum target, GLint level, GLint internalformat,
                             GLsizei width, GLsizei height, GLint border,
                             GLenum format, GLenum type,
                             const GLvoid *pixels) {
  static unsigned calls;
  unsigned call = ++calls;
  GLenum pre_err = gl_log_enabled() ? drain_gl_error() : GL_NO_ERROR;
  glTexImage2D(target, level, internalformat, width, height, border, format,
               type, pixels);
  char details[160];
  snprintf(details, sizeof(details),
           "target=0x%x level=%d ifmt=0x%x %dx%d border=%d fmt=0x%x type=0x%x "
           "pixels=%p",
           target, level, internalformat, width, height, border, format, type,
           pixels);
  log_gl_result("glTexImage2D", call, details, pre_err, 0);
}

static void glTexSubImage2D_log(GLenum target, GLint level, GLint xoffset,
                                GLint yoffset, GLsizei width, GLsizei height,
                                GLenum format, GLenum type,
                                const GLvoid *pixels) {
  static unsigned calls;
  unsigned call = ++calls;
  GLenum pre_err = gl_log_enabled() ? drain_gl_error() : GL_NO_ERROR;
  glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type,
                  pixels);
  char details[160];
  snprintf(details, sizeof(details),
           "target=0x%x level=%d xy=%d,%d %dx%d fmt=0x%x type=0x%x pixels=%p",
           target, level, xoffset, yoffset, width, height, format, type,
           pixels);
  log_gl_result("glTexSubImage2D", call, details, pre_err, 0);
}

static void glEnable_log(GLenum cap) {
  static unsigned calls;
  unsigned call = ++calls;
  GLenum pre_err = gl_log_enabled() ? drain_gl_error() : GL_NO_ERROR;
  glEnable(cap);
  char details[64];
  snprintf(details, sizeof(details), "cap=0x%x", cap);
  log_gl_result("glEnable", call, details, pre_err, 0);
}

static void glDisable_log(GLenum cap) {
  static unsigned calls;
  unsigned call = ++calls;
  GLenum pre_err = gl_log_enabled() ? drain_gl_error() : GL_NO_ERROR;
  glDisable(cap);
  char details[64];
  snprintf(details, sizeof(details), "cap=0x%x", cap);
  log_gl_result("glDisable", call, details, pre_err, 0);
}

static void glEnableClientState_log(GLenum array) {
  static unsigned calls;
  unsigned call = ++calls;
  GLenum pre_err = gl_log_enabled() ? drain_gl_error() : GL_NO_ERROR;
  glEnableClientState(array);
  char details[64];
  snprintf(details, sizeof(details), "array=0x%x", array);
  log_gl_result("glEnableClientState", call, details, pre_err, 0);
}

static void glDisableClientState_log(GLenum array) {
  static unsigned calls;
  unsigned call = ++calls;
  GLenum pre_err = gl_log_enabled() ? drain_gl_error() : GL_NO_ERROR;
  glDisableClientState(array);
  char details[64];
  snprintf(details, sizeof(details), "array=0x%x", array);
  log_gl_result("glDisableClientState", call, details, pre_err, 0);
}

static void glVertexPointer_log(GLint size, GLenum type, GLsizei stride,
                                const GLvoid *pointer) {
  static unsigned calls;
  unsigned call = ++calls;
  GLenum pre_err = gl_log_enabled() ? drain_gl_error() : GL_NO_ERROR;
  glVertexPointer(size, type, stride, pointer);
  char details[112];
  snprintf(details, sizeof(details), "size=%d type=0x%x stride=%d ptr=%p",
           size, type, stride, pointer);
  log_gl_result("glVertexPointer", call, details, pre_err, 0);
}

static void glTexCoordPointer_log(GLint size, GLenum type, GLsizei stride,
                                  const GLvoid *pointer) {
  static unsigned calls;
  unsigned call = ++calls;
  GLenum pre_err = gl_log_enabled() ? drain_gl_error() : GL_NO_ERROR;
  glTexCoordPointer(size, type, stride, pointer);
  char details[112];
  snprintf(details, sizeof(details), "size=%d type=0x%x stride=%d ptr=%p",
           size, type, stride, pointer);
  log_gl_result("glTexCoordPointer", call, details, pre_err, 0);
}

static void glAlphaFunc_log(GLenum func, GLclampf ref) {
  static unsigned calls;
  unsigned call = ++calls;
  GLenum pre_err = gl_log_enabled() ? drain_gl_error() : GL_NO_ERROR;
  glAlphaFunc(func, ref);
  char details[80];
  snprintf(details, sizeof(details), "func=0x%x ref=%f", func, ref);
  log_gl_result("glAlphaFunc", call, details, pre_err, 0);
}

static void glBlendFunc_log(GLenum sfactor, GLenum dfactor) {
  static unsigned calls;
  unsigned call = ++calls;
  GLenum pre_err = gl_log_enabled() ? drain_gl_error() : GL_NO_ERROR;
  glBlendFunc(sfactor, dfactor);
  char details[80];
  snprintf(details, sizeof(details), "src=0x%x dst=0x%x", sfactor, dfactor);
  log_gl_result("glBlendFunc", call, details, pre_err, 0);
}

static void glDrawArrays_log(GLenum mode, GLint first, GLsizei count) {
  static unsigned calls;
  unsigned call = ++calls;
  GLenum pre_err = gl_log_enabled() ? drain_gl_error() : GL_NO_ERROR;
  glDrawArrays(mode, first, count);
  char details[96];
  snprintf(details, sizeof(details), "mode=0x%x first=%d count=%d", mode, first,
           count);
  log_gl_result("glDrawArrays", call, details, pre_err, 1);
}

static void glDrawElements_log(GLenum mode, GLsizei count, GLenum type,
                               const GLvoid *indices) {
  static unsigned calls;
  unsigned call = ++calls;
  GLenum pre_err = gl_log_enabled() ? drain_gl_error() : GL_NO_ERROR;
  glDrawElements(mode, count, type, indices);
  char details[112];
  snprintf(details, sizeof(details), "mode=0x%x count=%d type=0x%x indices=%p",
           mode, count, type, indices);
  log_gl_result("glDrawElements", call, details, pre_err, 1);
}

DynLibFunction dynlib_functions[] = {
    {"AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake},
    {"AAssetManager_open", (uintptr_t)&AAssetManager_open_fake},
    {"AAsset_close", (uintptr_t)&AAsset_close_fake},
    {"AAsset_getLength", (uintptr_t)&AAsset_getLength_fake},
    {"AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor_fake},
    {"AAsset_read", (uintptr_t)&AAsset_read_fake},
    {"AAsset_seek", (uintptr_t)&AAsset_seek_fake},

    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&sl_IID_BUFFERQUEUE},
    {"SL_IID_ENGINE", (uintptr_t)&sl_IID_ENGINE},
    {"SL_IID_PLAY", (uintptr_t)&sl_IID_PLAY},
    {"SL_IID_VOLUME", (uintptr_t)&sl_IID_VOLUME},
    {"slCreateEngine", (uintptr_t)&slCreateEngine_shim},

    {"__android_log_print", (uintptr_t)&__android_log_print_fake},
    {"__cxa_atexit", (uintptr_t)&__cxa_atexit},
    {"__cxa_finalize", (uintptr_t)&__cxa_finalize},
    {"__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire},
    {"__cxa_guard_release", (uintptr_t)&__cxa_guard_release},
    {"__stack_chk_fail", (uintptr_t)&__stack_chk_fail_stub},
    {"__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake},
    {"_ZdaPv", (uintptr_t)&_ZdaPv},
    {"_ZdlPv", (uintptr_t)&_ZdlPv},
    {"_Znam", (uintptr_t)&_Znam},
    {"_Znwm", (uintptr_t)&_Znwm},

    {"atan2", (uintptr_t)&atan2},
    {"atan2f", (uintptr_t)&atan2f},
    {"calloc", (uintptr_t)&calloc},
    {"clock", (uintptr_t)&clock},
    {"clock_gettime", (uintptr_t)&clock_gettime},
    {"cos", (uintptr_t)&cos},
    {"cosf", (uintptr_t)&cosf},
    {"exp", (uintptr_t)&exp},
    {"exp2f", (uintptr_t)&exp2f},
    {"fclose", (uintptr_t)&fclose},
    {"fgetc", (uintptr_t)&fgetc},
    {"fopen", (uintptr_t)&fopen_fake},
    {"fread", (uintptr_t)&fread},
    {"free", (uintptr_t)&free},
    {"fseek", (uintptr_t)&fseek},
    {"ftell", (uintptr_t)&ftell},
    {"fwrite", (uintptr_t)&fwrite},
    {"ldexp", (uintptr_t)&ldexp},
    {"localtime", (uintptr_t)&localtime},
    {"log", (uintptr_t)&log},
    {"malloc", (uintptr_t)&malloc},
    {"memcmp", (uintptr_t)&memcmp},
    {"memcpy", (uintptr_t)&memcpy},
    {"memmove", (uintptr_t)&memmove},
    {"memset", (uintptr_t)&memset},
    {"mktime", (uintptr_t)&mktime},
    {"pow", (uintptr_t)&pow},
    {"printf", (uintptr_t)&printf},
    {"qsort", (uintptr_t)&qsort},
    {"realloc", (uintptr_t)&realloc},
    {"select", (uintptr_t)&select},
    {"sin", (uintptr_t)&sin},
    {"sprintf", (uintptr_t)&sprintf},
    {"sqrt", (uintptr_t)&sqrt},
    {"sqrtf", (uintptr_t)&sqrtf},
    {"strcmp", (uintptr_t)&strcmp},
    {"strcpy", (uintptr_t)&strcpy},
    {"strlen", (uintptr_t)&strlen},
    {"strncpy", (uintptr_t)&strncpy},
    {"strstr", (uintptr_t)&strstr},
    {"time", (uintptr_t)&time},
    {"usleep", (uintptr_t)&usleep},

    {"pthread_create", (uintptr_t)&pthread_create_fake},
    {"pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake},
    {"pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake},
    {"pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake},
    {"pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake},

    {"glAlphaFunc", (uintptr_t)&glAlphaFunc},
    {"glBindTexture", (uintptr_t)&glBindTexture},
    {"glBlendFunc", (uintptr_t)&glBlendFunc},
    {"glClear", (uintptr_t)&glClear},
    {"glClearColor", (uintptr_t)&glClearColor},
    {"glColor4f", (uintptr_t)&glColor4f},
    {"glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D},
    {"glCopyTexImage2D", (uintptr_t)&glCopyTexImage2D},
    {"glDeleteTextures", (uintptr_t)&glDeleteTextures},
    {"glDepthFunc", (uintptr_t)&glDepthFunc},
    {"glDepthMask", (uintptr_t)&glDepthMask},
    {"glDisable", (uintptr_t)&glDisable},
    {"glDisableClientState", (uintptr_t)&glDisableClientState},
    {"glDrawArrays", (uintptr_t)&glDrawArrays},
    {"glDrawElements", (uintptr_t)&glDrawElements},
    {"glEnable", (uintptr_t)&glEnable},
    {"glEnableClientState", (uintptr_t)&glEnableClientState},
    {"glFlush", (uintptr_t)&glFlush},
    {"glFogf", (uintptr_t)&glFogf},
    {"glFogfv", (uintptr_t)&glFogfv},
    {"glGenTextures", (uintptr_t)&glGenTextures},
    {"glGetError", (uintptr_t)&glGetError},
    {"glGetString", (uintptr_t)&glGetString},
    {"glHint", (uintptr_t)&glHint},
    {"glLoadIdentity", (uintptr_t)&glLoadIdentity},
    {"glMatrixMode", (uintptr_t)&glMatrixMode},
    {"glOrthof", (uintptr_t)&glOrthof},
    {"glPixelStorei", (uintptr_t)&glPixelStorei},
    {"glReadPixels", (uintptr_t)&glReadPixels},
    {"glScissor", (uintptr_t)&glScissor},
    {"glShadeModel", (uintptr_t)&glShadeModel},
    {"glTexCoordPointer", (uintptr_t)&glTexCoordPointer},
    {"glTexImage2D", (uintptr_t)&glTexImage2D},
    {"glTexParameterx", (uintptr_t)&glTexParameterx},
    {"glTexSubImage2D", (uintptr_t)&glTexSubImage2D},
    {"glVertexPointer", (uintptr_t)&glVertexPointer},
    {"glViewport", (uintptr_t)&glViewport},
};

const int dynlib_functions_count = sizeof(dynlib_functions) / sizeof(dynlib_functions[0]);
