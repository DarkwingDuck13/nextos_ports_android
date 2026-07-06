/*
 * imports.c -- .so import resolution for LSWTCS ARM64
 *
 * Maps all 286 undefined symbols from libTTapp.so to real
 * libc/GL/EGL functions or our shim implementations.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <syslog.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>

#include "android_shim.h"
#include "egl_shim.h"
#include "opensles_shim.h"
#include "so_util.h"
#include "util.h"

extern uintptr_t __cxa_atexit;
extern uintptr_t __cxa_finalize;

FILE *stderr_fake = (FILE *)0x1337;

static uint8_t fake_sF[3][0x100];
static uint64_t __stack_chk_guard_fake = 0x4242424242424242;

const GLubyte *glGetString_wrap(GLenum name);
void glCompressedTexImage2D_wrap(GLenum target, GLint level,
                                 GLenum internalformat, GLsizei width,
                                 GLsizei height, GLint border,
                                 GLsizei imageSize, const void *data);
void glTexImage2D_wrap(GLenum target, GLint level, GLint internalformat,
                       GLsizei width, GLsizei height, GLint border,
                       GLenum format, GLenum type, const void *pixels);
void glTexParameteri_wrap(GLenum target, GLenum pname, GLint param);
void glTexSubImage2D_wrap(GLenum target, GLint level, GLint xoffset,
                          GLint yoffset, GLsizei width, GLsizei height,
                          GLenum format, GLenum type, const void *pixels);
void glPixelStorei_wrap(GLenum pname, GLint param);
void glCopyTexImage2D_wrap(GLenum target, GLint level, GLenum internalformat,
                           GLint x, GLint y, GLsizei width, GLsizei height,
                           GLint border);
void glCopyTexSubImage2D_wrap(GLenum target, GLint level, GLint xoffset,
                              GLint yoffset, GLint x, GLint y, GLsizei width,
                              GLsizei height);

typedef struct HostMutexEntry {
  void *guest_addr;
  pthread_mutex_t mutex;
  struct HostMutexEntry *next;
} HostMutexEntry;

typedef struct HostCondEntry {
  void *guest_addr;
  pthread_cond_t cond;
  struct HostCondEntry *next;
} HostCondEntry;

static HostMutexEntry *g_mutex_entries = NULL;
static HostCondEntry *g_cond_entries = NULL;
static pthread_mutex_t g_mutex_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_cond_registry_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct HostRwlockEntry {
  void *guest_addr;
  pthread_rwlock_t lock;
  struct HostRwlockEntry *next;
} HostRwlockEntry;

typedef struct HostSemEntry {
  void *guest_addr;
  sem_t sem;
  struct HostSemEntry *next;
} HostSemEntry;

static HostRwlockEntry *g_rwlock_entries = NULL;
static HostSemEntry *g_sem_entries = NULL;
static pthread_mutex_t g_rwlock_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_sem_registry_lock = PTHREAD_MUTEX_INITIALIZER;
/* Vita-style: just log and return — no abort, no loop */
static void __stack_chk_fail_stub(void) {
  uintptr_t ra = (uintptr_t)__builtin_return_address(0);
  if (text_base && ra >= (uintptr_t)text_base &&
      ra < (uintptr_t)text_base + text_size) {
    debugPrintf("__stack_chk_fail called from %p (libTTapp.so+0x%lx)\n",
                (void *)ra, (unsigned long)(ra - (uintptr_t)text_base));
  } else if (data_base && ra >= (uintptr_t)data_base &&
             ra < (uintptr_t)data_base + data_size) {
    debugPrintf("__stack_chk_fail called from %p (libTTapp.so[data]+0x%lx)\n",
                (void *)ra, (unsigned long)(ra - (uintptr_t)data_base));
  } else {
    debugPrintf("__stack_chk_fail called from %p\n", (void *)ra);
  }
}

/* errno compat */
static int *__errno_fake(void) { return &errno; }

static FILE *map_sf(void *f) {
  uintptr_t p = (uintptr_t)f;
  uintptr_t base = (uintptr_t)fake_sF;
  if (p >= base && p < base + sizeof(fake_sF)) {
    int idx = (int)((p - base) / 0x100);
    return idx == 0 ? stdin : idx == 1 ? stdout : stderr;
  }
  return (FILE *)f;
}

static int fprintf_fake(void *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vfprintf(map_sf(f), fmt, ap);
  va_end(ap);
  return r;
}

static int vfprintf_fake(void *f, const char *fmt, va_list ap) {
  return vfprintf(map_sf(f), fmt, ap);
}

static int fputc_fake(int c, void *f) { return fputc(c, map_sf(f)); }
static size_t fwrite_fake(const void *p, size_t sz, size_t n, void *f) {
  return fwrite(p, sz, n, map_sf(f));
}
static int fflush_fake(void *f) { return fflush(f ? map_sf(f) : NULL); }
static int ferror_fake(void *f) { return ferror(map_sf(f)); }
static int fclose_fake(void *f) {
  FILE *mapped = map_sf(f);
  if (mapped == stdin || mapped == stdout || mapped == stderr)
    return 0;
  return fclose(mapped);
}

static pthread_mutex_t *lookup_host_mutex(void *guest_addr, int create) {
  if (!guest_addr)
    return NULL;

  pthread_mutex_lock(&g_mutex_registry_lock);
  for (HostMutexEntry *entry = g_mutex_entries; entry; entry = entry->next) {
    if (entry->guest_addr == guest_addr) {
      pthread_mutex_unlock(&g_mutex_registry_lock);
      return &entry->mutex;
    }
  }

  if (!create) {
    pthread_mutex_unlock(&g_mutex_registry_lock);
    return NULL;
  }

  HostMutexEntry *entry = calloc(1, sizeof(*entry));
  if (!entry) {
    pthread_mutex_unlock(&g_mutex_registry_lock);
    return NULL;
  }

  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&entry->mutex, &attr);
  pthread_mutexattr_destroy(&attr);

  entry->guest_addr = guest_addr;
  entry->next = g_mutex_entries;
  g_mutex_entries = entry;
  pthread_mutex_unlock(&g_mutex_registry_lock);
  return &entry->mutex;
}

static int destroy_host_mutex(void *guest_addr) {
  if (!guest_addr)
    return 0;

  pthread_mutex_lock(&g_mutex_registry_lock);
  HostMutexEntry **link = &g_mutex_entries;
  while (*link) {
    HostMutexEntry *entry = *link;
    if (entry->guest_addr == guest_addr) {
      *link = entry->next;
      pthread_mutex_unlock(&g_mutex_registry_lock);
      pthread_mutex_destroy(&entry->mutex);
      free(entry);
      return 0;
    }
    link = &entry->next;
  }
  pthread_mutex_unlock(&g_mutex_registry_lock);
  return 0;
}

static pthread_cond_t *lookup_host_cond(void *guest_addr, int create) {
  if (!guest_addr)
    return NULL;

  pthread_mutex_lock(&g_cond_registry_lock);
  for (HostCondEntry *entry = g_cond_entries; entry; entry = entry->next) {
    if (entry->guest_addr == guest_addr) {
      pthread_mutex_unlock(&g_cond_registry_lock);
      return &entry->cond;
    }
  }

  if (!create) {
    pthread_mutex_unlock(&g_cond_registry_lock);
    return NULL;
  }

  HostCondEntry *entry = calloc(1, sizeof(*entry));
  if (!entry) {
    pthread_mutex_unlock(&g_cond_registry_lock);
    return NULL;
  }

  pthread_cond_init(&entry->cond, NULL);
  entry->guest_addr = guest_addr;
  entry->next = g_cond_entries;
  g_cond_entries = entry;
  pthread_mutex_unlock(&g_cond_registry_lock);
  return &entry->cond;
}

static int destroy_host_cond(void *guest_addr) {
  if (!guest_addr)
    return 0;

  pthread_mutex_lock(&g_cond_registry_lock);
  HostCondEntry **link = &g_cond_entries;
  while (*link) {
    HostCondEntry *entry = *link;
    if (entry->guest_addr == guest_addr) {
      *link = entry->next;
      pthread_mutex_unlock(&g_cond_registry_lock);
      pthread_cond_destroy(&entry->cond);
      free(entry);
      return 0;
    }
    link = &entry->next;
  }
  pthread_mutex_unlock(&g_cond_registry_lock);
  return 0;
}

/* __android_log */
int __android_log_print_fake(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
  va_list list;
  static char string[0x1000];
  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);
  debugPrintf("LOG [%s]: %s\n", tag, string);
  return 0;
}

int __android_log_write_fake(int prio, const char *tag, const char *text) {
  (void)prio;
  debugPrintf("LOG [%s]: %s\n", tag ? tag : "(null)", text ? text : "(null)");
  return 0;
}

/* fortified libc stubs */
void *__memcpy_chk(void *dst, const void *src, size_t n, size_t dst_len) {
  (void)dst_len;
  return memcpy(dst, src, n);
}

void *__memmove_chk(void *dst, const void *src, size_t n, size_t dst_len) {
  (void)dst_len;
  return memmove(dst, src, n);
}

void *__memset_chk(void *dst, int c, size_t n, size_t dst_len) {
  (void)dst_len;
  return memset(dst, c, n);
}

char *__strcat_chk(char *dst, const char *src, size_t dst_buf_size) {
  (void)dst_buf_size;
  return strcat(dst, src);
}

char *__strcpy_chk(char *dst, const char *src, size_t dst_len) {
  (void)dst_len;
  return strcpy(dst, src);
}

size_t __strlen_chk(const char *s, size_t max_len) {
  (void)max_len;
  return strlen(s);
}

char *__strrchr_chk(const char *s, int c, size_t n) {
  (void)n;
  return strrchr(s, c);
}

int __vsprintf_chk(char *dst, int flags, size_t dst_len_from_compiler,
                    const char *fmt, va_list ap) {
  (void)flags;
  (void)dst_len_from_compiler;
  return vsprintf(dst, fmt, ap);
}

int __vsnprintf_chk(char *dst, size_t supplied_size, int flags,
                     size_t dst_len_from_compiler, const char *fmt,
                     va_list ap) {
  (void)flags;
  (void)dst_len_from_compiler;
  return vsnprintf(dst, supplied_size, fmt, ap);
}

ssize_t __read_chk(int fd, void *buf, size_t count, size_t buf_size) {
  (void)buf_size;
  return read(fd, buf, count);
}

int __open_2(const char *pathname, int flags) {
  const char *resolved = resolve_android_path(pathname);
  int fd = open(resolved, flags);
  if (strncmp(pathname, "/dev/", 5) != 0) {
    debugPrintf("open(\"%s\" -> \"%s\", 0x%x) = %d\n", pathname, resolved, flags, fd);
  }
  return fd;
}

/* open() wrapper for debugging — skip /dev/ spam */
int open_fake(const char *pathname, int flags, ...) {
  const char *resolved = resolve_android_path(pathname);
  int fd;
  mode_t mode = 0;
  int has_mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
    has_mode = 1;
    fd = open(resolved, flags, mode);
  } else {
    fd = open(resolved, flags);
  }
  if (strncmp(pathname, "/dev/", 5) != 0) {
    if (fd >= 0) {
      if (has_mode)
        debugPrintf("open(\"%s\" -> \"%s\", 0x%x, 0%o) = %d\n",
                    pathname, resolved, flags, (unsigned)mode, fd);
      else
        debugPrintf("open(\"%s\" -> \"%s\", 0x%x) = %d\n",
                    pathname, resolved, flags, fd);
    } else {
      if (has_mode)
        debugPrintf("open(\"%s\" -> \"%s\", 0x%x, 0%o) = %d (errno=%d: %s)\n",
                    pathname, resolved, flags, (unsigned)mode, fd, errno, strerror(errno));
      else
        debugPrintf("open(\"%s\" -> \"%s\", 0x%x) = %d (errno=%d: %s)\n",
                    pathname, resolved, flags, fd, errno, strerror(errno));
    }
  }
  return fd;
}

static int mkdir_fake(const char *pathname, mode_t mode) {
  const char *resolved = resolve_android_path(pathname);
  int ret = mkdir(resolved, mode);
  if (ret == 0)
    debugPrintf("mkdir(\"%s\" -> \"%s\", 0%o) = 0\n",
                pathname, resolved, (unsigned)mode);
  else
    debugPrintf("mkdir(\"%s\" -> \"%s\", 0%o) = -1 (errno=%d: %s)\n",
                pathname, resolved, (unsigned)mode, errno, strerror(errno));
  return ret;
}

static int remove_fake(const char *pathname) {
  const char *resolved = resolve_android_path(pathname);
  int ret = remove(resolved);
  if (ret == 0)
    debugPrintf("remove(\"%s\" -> \"%s\") = 0\n", pathname, resolved);
  else
    debugPrintf("remove(\"%s\" -> \"%s\") = -1 (errno=%d: %s)\n",
                pathname, resolved, errno, strerror(errno));
  return ret;
}

static int rename_fake(const char *oldpath, const char *newpath) {
  char resolved_old[2048];
  char resolved_new[2048];
  const char *resolved_old_src = resolve_android_path(oldpath);
  const char *resolved_new_src;
  int ret;

  SDL_strlcpy(resolved_old, resolved_old_src, sizeof(resolved_old));
  resolved_new_src = resolve_android_path(newpath);
  SDL_strlcpy(resolved_new, resolved_new_src, sizeof(resolved_new));
  ret = rename(resolved_old, resolved_new);
  if (ret == 0) {
    debugPrintf("rename(\"%s\" -> \"%s\", \"%s\" -> \"%s\") = 0\n",
                oldpath, resolved_old, newpath, resolved_new);
  } else {
    debugPrintf("rename(\"%s\" -> \"%s\", \"%s\" -> \"%s\") = -1 (errno=%d: %s)\n",
                oldpath, resolved_old, newpath, resolved_new, errno, strerror(errno));
  }
  return ret;
}

/* ctype compat */
size_t __ctype_get_mb_cur_max_fake(void) { return 4; }

/* dl_iterate_phdr stub */
int dl_iterate_phdr_fake(void *callback, void *data) {
  (void)callback;
  (void)data;
  return 0;
}

/* android_set_abort_message stub */
void android_set_abort_message_fake(const char *msg) {
  debugPrintf("android_set_abort_message: %s\n", msg ? msg : "(null)");
}

/* dlopen/dlsym stubs — game may dynamically load libGLESv2.so etc. */
void *dlopen_fake(const char *filename, int flags) {
  debugPrintf("dlopen(\"%s\", %d)\n", filename ? filename : "(null)", flags);
  return (void *)0xDEAD0001; /* non-NULL dummy handle */
}

static void *gl_proc_override(const char *symbol) {
  if (!strcmp(symbol, "glGetString") || !strcmp(symbol, "glGetStringOES") ||
      !strcmp(symbol, "glGetStringEXT"))
    return glGetString_wrap;
  if (!strcmp(symbol, "glCompressedTexImage2D") ||
      !strcmp(symbol, "glCompressedTexImage2DOES") ||
      !strcmp(symbol, "glCompressedTexImage2DEXT"))
    return glCompressedTexImage2D_wrap;
  if (!strcmp(symbol, "glTexImage2D") || !strcmp(symbol, "glTexImage2DOES") ||
      !strcmp(symbol, "glTexImage2DEXT"))
    return glTexImage2D_wrap;
  if (!strcmp(symbol, "glTexParameteri") ||
      !strcmp(symbol, "glTexParameteriOES") ||
      !strcmp(symbol, "glTexParameteriEXT"))
    return glTexParameteri_wrap;
  if (!strcmp(symbol, "glTexSubImage2D") ||
      !strcmp(symbol, "glTexSubImage2DOES") ||
      !strcmp(symbol, "glTexSubImage2DEXT"))
    return glTexSubImage2D_wrap;
  if (!strcmp(symbol, "glPixelStorei") ||
      !strcmp(symbol, "glPixelStoreiOES") ||
      !strcmp(symbol, "glPixelStoreiEXT"))
    return glPixelStorei_wrap;
  if (!strcmp(symbol, "glCopyTexImage2D"))
    return glCopyTexImage2D_wrap;
  if (!strcmp(symbol, "glCopyTexSubImage2D"))
    return glCopyTexSubImage2D_wrap;
  return NULL;
}

void *dlsym_fake(void *handle, const char *symbol) {
  debugPrintf("dlsym(%p, \"%s\")\n", handle, symbol);
  if (!symbol)
    return NULL;
  if (!strcmp(symbol, "AMotionEvent_getAxisValue")) return AMotionEvent_getAxisValue_fake;
  if (!strcmp(symbol, "AInputEvent_getType")) return AInputEvent_getType_fake;
  if (!strcmp(symbol, "AInputEvent_getSource")) return AInputEvent_getSource_fake;
  if (!strcmp(symbol, "AKeyEvent_getAction")) return AKeyEvent_getAction_fake;
  if (!strcmp(symbol, "AKeyEvent_getKeyCode")) return AKeyEvent_getKeyCode_fake;
  if (!strcmp(symbol, "AMotionEvent_getAction")) return AMotionEvent_getAction_fake;
  if (!strcmp(symbol, "AMotionEvent_getPointerCount")) return AMotionEvent_getPointerCount_fake;
  if (!strcmp(symbol, "AMotionEvent_getPointerId")) return AMotionEvent_getPointerId_fake;
  if (!strcmp(symbol, "AMotionEvent_getX")) return AMotionEvent_getX_fake;
  if (!strcmp(symbol, "AMotionEvent_getY")) return AMotionEvent_getY_fake;
  if (!strcmp(symbol, "ANativeWindow_setFrameRate")) return ANativeWindow_setFrameRate_fake;
  void *override = gl_proc_override(symbol);
  if (override) return override;
  /* Try SDL GL proc address first (covers GL/EGL extensions) */
  void *ptr = SDL_GL_GetProcAddress(symbol);
  if (ptr) return ptr;
  debugPrintf("dlsym: NOT FOUND: %s\n", symbol);
  return NULL;
}

int dlclose_fake(void *handle) { (void)handle; return 0; }
char *dlerror_fake(void) { return NULL; }
int dladdr_fake(void *addr, void *info) { (void)addr; (void)info; return 0; }

/* getenv/setenv stubs */
char *getenv_fake(const char *name) {
  debugPrintf("getenv(\"%s\") -> NULL\n", name);
  return NULL;
}

int setenv_fake(const char *name, const char *value, int overwrite) {
  (void)name; (void)value; (void)overwrite;
  return 0;
}

/* __system_property_get stub */
int __system_property_get_fake(const char *name, char *value) {
  debugPrintf("__system_property_get(\"%s\")\n", name);
  value[0] = '\0';
  return 0;
}

/* Vita-style: log then call real function */
void abort_fake(void) {
  debugPrintf("abort() called from %p\n", __builtin_return_address(0));
}

void exit_fake(int status) {
  debugPrintf("exit(%d) called from %p\n", status, __builtin_return_address(0));
  _exit(status);
}

/* Vita-style: stub sigaction — game shouldn't install signal handlers */
int sigaction_fake(int signum, const void *act, void *oldact) {
  (void)signum; (void)act; (void)oldact;
  return 0;
}

/* fopen wrapper for debugging */
FILE *fopen_fake(const char *filename, const char *mode) {
  const char *resolved = resolve_android_path(filename);
  FILE *f;

  f = fopen(resolved, mode);
  if (!f) {
    debugPrintf("fopen(\"%s\" -> \"%s\", \"%s\") = NULL (errno=%d: %s)\n",
                filename, resolved, mode, errno, strerror(errno));
  } else {
    debugPrintf("fopen(\"%s\" -> \"%s\", \"%s\") = %p\n",
                filename, resolved, mode, f);
  }

  return f;
}

/* pthread wrappers: guest code passes inline bionic objects by address. */
int pthread_mutex_init_fake(pthread_mutex_t *uid, const int *mutexattr) {
  (void)mutexattr;
  return lookup_host_mutex(uid, 1) ? 0 : -1;
}

int pthread_mutex_destroy_fake(pthread_mutex_t *uid) {
  return destroy_host_mutex(uid);
}

int pthread_mutex_lock_fake(pthread_mutex_t *uid) {
  pthread_mutex_t *host = lookup_host_mutex(uid, 1);
  if (!host)
    return -1;
  int ret = pthread_mutex_lock(host);
  if (ret == 0) egl_shim_on_mutex_post_lock(uid);
  return ret;
}

int pthread_mutex_trylock_fake(pthread_mutex_t *uid) {
  pthread_mutex_t *host = lookup_host_mutex(uid, 1);
  if (!host)
    return -1;
  int ret = pthread_mutex_trylock(host);
  if (ret == 0) egl_shim_on_mutex_post_lock(uid);
  return ret;
}

int pthread_mutex_unlock_fake(pthread_mutex_t *uid) {
  pthread_mutex_t *host = lookup_host_mutex(uid, 1);
  if (!host)
    return -1;
  egl_shim_on_mutex_pre_unlock(uid);
  return pthread_mutex_unlock(host);
}

int pthread_cond_init_fake(pthread_cond_t *cnd, const int *condattr) {
  (void)condattr;
  return lookup_host_cond(cnd, 1) ? 0 : -1;
}

int pthread_cond_destroy_fake(pthread_cond_t *cnd) {
  return destroy_host_cond(cnd);
}

int pthread_cond_wait_fake(pthread_cond_t *cnd, pthread_mutex_t *mtx) {
  pthread_cond_t *host_cnd = lookup_host_cond(cnd, 1);
  pthread_mutex_t *host_mtx = lookup_host_mutex(mtx, 1);
  if (!host_cnd || !host_mtx)
    return -1;
  return pthread_cond_wait(host_cnd, host_mtx);
}

int pthread_cond_timedwait_fake(pthread_cond_t *cnd, pthread_mutex_t *mtx,
                                 const struct timespec *t) {
  pthread_cond_t *host_cnd = lookup_host_cond(cnd, 1);
  pthread_mutex_t *host_mtx = lookup_host_mutex(mtx, 1);
  if (!host_cnd || !host_mtx)
    return -1;
  return pthread_cond_timedwait(host_cnd, host_mtx, t);
}

int pthread_cond_signal_fake(pthread_cond_t *cnd) {
  pthread_cond_t *host_cnd = lookup_host_cond(cnd, 1);
  if (!host_cnd)
    return -1;
  return pthread_cond_signal(host_cnd);
}

int pthread_cond_broadcast_fake(pthread_cond_t *cnd) {
  pthread_cond_t *host_cnd = lookup_host_cond(cnd, 1);
  if (!host_cnd)
    return -1;
  return pthread_cond_broadcast(host_cnd);
}

typedef struct {
  void *(*entry)(void *);
  void *arg;
} ThreadWrapper;

static void *thread_wrapper_func(void *data) {
  ThreadWrapper *w = (ThreadWrapper *)data;
  void *(*entry)(void *) = w->entry;
  void *arg = w->arg;
  free(w);
  debugPrintf("[thread %lx] starting entry=%p arg=%p\n",
              (unsigned long)pthread_self(), (void *)entry, arg);
  void *ret = entry(arg);
  debugPrintf("[thread %lx] entry returned %p\n",
              (unsigned long)pthread_self(), ret);
  return ret;
}

int pthread_create_fake(pthread_t *thread, const void *attr, void *entry,
                         void *arg) {
  debugPrintf("pthread_create_fake(entry=%p, arg=%p)\n", entry, arg);
  ThreadWrapper *w = malloc(sizeof(ThreadWrapper));
  w->entry = entry;
  w->arg = arg;
  pthread_attr_t real_attr;
  pthread_attr_init(&real_attr);
  pthread_attr_setstacksize(&real_attr, 2 * 1024 * 1024); // 2MB stack
  int ret = pthread_create(thread, &real_attr, thread_wrapper_func, w);
  pthread_attr_destroy(&real_attr);
  if (ret != 0) free(w);
  return ret;
}

static void *pthread_getspecific_fake(pthread_key_t key) {
  (void)key;
  return pthread_getspecific(key);
}

static int pthread_setspecific_fake(pthread_key_t key, const void *value) {
  return pthread_setspecific(key, value);
}

int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  return pthread_once((pthread_once_t *)once_control, init_routine);
}

static pthread_rwlock_t *lookup_host_rwlock(void *guest_addr, int create) {
  if (!guest_addr)
    return NULL;
  pthread_mutex_lock(&g_rwlock_registry_lock);
  for (HostRwlockEntry *entry = g_rwlock_entries; entry; entry = entry->next) {
    if (entry->guest_addr == guest_addr) {
      pthread_mutex_unlock(&g_rwlock_registry_lock);
      return &entry->lock;
    }
  }
  if (!create) {
    pthread_mutex_unlock(&g_rwlock_registry_lock);
    return NULL;
  }
  HostRwlockEntry *entry = calloc(1, sizeof(*entry));
  if (!entry) {
    pthread_mutex_unlock(&g_rwlock_registry_lock);
    return NULL;
  }
  pthread_rwlock_init(&entry->lock, NULL);
  entry->guest_addr = guest_addr;
  entry->next = g_rwlock_entries;
  g_rwlock_entries = entry;
  pthread_mutex_unlock(&g_rwlock_registry_lock);
  return &entry->lock;
}

static int pthread_rwlock_rdlock_fake(void *rwlock) {
  pthread_rwlock_t *host = lookup_host_rwlock(rwlock, 1);
  return host ? pthread_rwlock_rdlock(host) : -1;
}

static int pthread_rwlock_wrlock_fake(void *rwlock) {
  pthread_rwlock_t *host = lookup_host_rwlock(rwlock, 1);
  return host ? pthread_rwlock_wrlock(host) : -1;
}

static int pthread_rwlock_unlock_fake(void *rwlock) {
  pthread_rwlock_t *host = lookup_host_rwlock(rwlock, 1);
  return host ? pthread_rwlock_unlock(host) : -1;
}

static sem_t *lookup_host_sem(void *guest_addr, int create, unsigned value) {
  if (!guest_addr)
    return NULL;
  pthread_mutex_lock(&g_sem_registry_lock);
  for (HostSemEntry *entry = g_sem_entries; entry; entry = entry->next) {
    if (entry->guest_addr == guest_addr) {
      pthread_mutex_unlock(&g_sem_registry_lock);
      return &entry->sem;
    }
  }
  if (!create) {
    pthread_mutex_unlock(&g_sem_registry_lock);
    return NULL;
  }
  HostSemEntry *entry = calloc(1, sizeof(*entry));
  if (!entry) {
    pthread_mutex_unlock(&g_sem_registry_lock);
    return NULL;
  }
  sem_init(&entry->sem, 0, value);
  entry->guest_addr = guest_addr;
  entry->next = g_sem_entries;
  g_sem_entries = entry;
  pthread_mutex_unlock(&g_sem_registry_lock);
  return &entry->sem;
}

static int sem_init_fake(void *sem, int pshared, unsigned value) {
  (void)pshared;
  return lookup_host_sem(sem, 1, value) ? 0 : -1;
}

static int sem_destroy_fake(void *sem) {
  pthread_mutex_lock(&g_sem_registry_lock);
  HostSemEntry **link = &g_sem_entries;
  while (*link) {
    HostSemEntry *entry = *link;
    if (entry->guest_addr == sem) {
      *link = entry->next;
      pthread_mutex_unlock(&g_sem_registry_lock);
      sem_destroy(&entry->sem);
      free(entry);
      return 0;
    }
    link = &entry->next;
  }
  pthread_mutex_unlock(&g_sem_registry_lock);
  return 0;
}

static int sem_wait_fake(void *sem) {
  sem_t *host = lookup_host_sem(sem, 1, 0);
  return host ? sem_wait(host) : -1;
}

static int sem_post_fake(void *sem) {
  sem_t *host = lookup_host_sem(sem, 1, 0);
  return host ? sem_post(host) : -1;
}

static int gettid_fake(void) { return (int)syscall(SYS_gettid); }

long syscall_fake(long number, ...) {
  va_list ap;
  long a1, a2, a3, a4, a5, a6;

  va_start(ap, number);
  a1 = va_arg(ap, long);
  a2 = va_arg(ap, long);
  a3 = va_arg(ap, long);
  a4 = va_arg(ap, long);
  a5 = va_arg(ap, long);
  a6 = va_arg(ap, long);
  va_end(ap);

#ifdef SYS_kill
  if (number == SYS_kill &&
      (a2 == SIGSEGV || a2 == SIGABRT || a2 == SIGILL || a2 == SIGBUS ||
       a2 == SIGFPE)) {
    debugPrintf("syscall(kill, pid=%ld, sig=%ld) blocked from %p\n", a1, a2,
                __builtin_return_address(0));
    return 0;
  }
#endif
#ifdef SYS_tkill
  if (number == SYS_tkill &&
      (a2 == SIGSEGV || a2 == SIGABRT || a2 == SIGILL || a2 == SIGBUS ||
       a2 == SIGFPE)) {
    debugPrintf("syscall(tkill, tid=%ld, sig=%ld) blocked from %p\n", a1, a2,
                __builtin_return_address(0));
    return 0;
  }
#endif
#ifdef SYS_tgkill
  if (number == SYS_tgkill &&
      (a3 == SIGSEGV || a3 == SIGABRT || a3 == SIGILL || a3 == SIGBUS ||
       a3 == SIGFPE)) {
    debugPrintf("syscall(tgkill, pid=%ld, tid=%ld, sig=%ld) blocked from %p\n",
                a1, a2, a3, __builtin_return_address(0));
    return 0;
  }
#endif

  return syscall(number, a1, a2, a3, a4, a5, a6);
}

static size_t strlcpy_fake(char *dst, const char *src, size_t size) {
  size_t len = strlen(src);
  if (size) {
    size_t copy = len >= size ? size - 1 : len;
    memcpy(dst, src, copy);
    dst[copy] = '\0';
  }
  return len;
}

static int statfs64_fake(const char *path, struct statfs *buf) {
  return statfs(resolve_android_path(path), buf);
}

static int isdigit_l_fake(int c, locale_t l) { (void)l; return isdigit(c); }
static int islower_l_fake(int c, locale_t l) { (void)l; return islower(c); }
static int isupper_l_fake(int c, locale_t l) { (void)l; return isupper(c); }
static int isxdigit_l_fake(int c, locale_t l) { (void)l; return isxdigit(c); }
static int tolower_l_fake(int c, locale_t l) { (void)l; return tolower(c); }
static int toupper_l_fake(int c, locale_t l) { (void)l; return toupper(c); }
static int strcoll_l_fake(const char *a, const char *b, locale_t l) {
  (void)l;
  return strcoll(a, b);
}
static size_t strxfrm_l_fake(char *dst, const char *src, size_t n,
                             locale_t l) {
  (void)l;
  return strxfrm(dst, src, n);
}
static size_t strftime_l_fake(char *s, size_t max, const char *fmt,
                              const struct tm *tm, locale_t l) {
  (void)l;
  return strftime(s, max, fmt, tm);
}
static wint_t towlower_l_fake(wint_t c, locale_t l) {
  (void)l;
  return towlower(c);
}
static wint_t towupper_l_fake(wint_t c, locale_t l) {
  (void)l;
  return towupper(c);
}
static int iswalpha_l_fake(wint_t c, locale_t l) { (void)l; return iswalpha(c); }
static int iswblank_l_fake(wint_t c, locale_t l) { (void)l; return iswblank(c); }
static int iswcntrl_l_fake(wint_t c, locale_t l) { (void)l; return iswcntrl(c); }
static int iswdigit_l_fake(wint_t c, locale_t l) { (void)l; return iswdigit(c); }
static int iswlower_l_fake(wint_t c, locale_t l) { (void)l; return iswlower(c); }
static int iswprint_l_fake(wint_t c, locale_t l) { (void)l; return iswprint(c); }
static int iswpunct_l_fake(wint_t c, locale_t l) { (void)l; return iswpunct(c); }
static int iswspace_l_fake(wint_t c, locale_t l) { (void)l; return iswspace(c); }
static int iswupper_l_fake(wint_t c, locale_t l) { (void)l; return iswupper(c); }
static int iswxdigit_l_fake(wint_t c, locale_t l) { (void)l; return iswxdigit(c); }
static int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, locale_t l) {
  (void)l;
  return wcscoll(a, b);
}
static size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n,
                             locale_t l) {
  (void)l;
  return wcsxfrm(dst, src, n);
}

#undef setjmp
extern int setjmp(jmp_buf env);

/* GL logging wrappers — diagnose if game makes any GL calls after MakeCurrent */
typedef const GLubyte *(*PFNGLGETSTRINGIPROC)(GLenum name, GLuint index);

const GLubyte *glGetString_wrap(GLenum name) {
  switch (name) {
  case 0x1f00: /* GL_VENDOR */
  case 0x1f01: /* GL_RENDERER */
  case 0x1f02: /* GL_VERSION */
  case 0x8b8c: /* GL_SHADING_LANGUAGE_VERSION */ {
    const GLubyte *s = glGetString(name);
    /* debugPrintf("GL: glGetString(0x%x) = \"%s\"\n", name,
                s ? (const char *)s : "(null)")); */
    if (s)
      return s;

    switch (name) {
    case 0x1f00:
      return (const GLubyte *)"Imagination Technologies";
    case 0x1f01:
      return (const GLubyte *)"PowerVR Rogue GE8300";
    case 0x1f02:
      return (const GLubyte *)"OpenGL ES 2.0";
    default:
      return (const GLubyte *)"OpenGL ES GLSL ES 1.00";
    }
  }
  case 0x1f03: { /* GL_EXTENSIONS */
    static const GLubyte fallback_ext[] =
        "GL_OES_depth_texture "
        "GL_OES_depth24 "
        "GL_OES_packed_depth_stencil "
        "GL_OES_element_index_uint "
        "GL_OES_texture_npot "
        "GL_OES_rgb8_rgba8 "
        "GL_OES_vertex_array_object "
        "GL_OES_mapbuffer "
        "GL_EXT_texture_format_BGRA8888 "
        "GL_IMG_texture_compression_pvrtc "
        "GL_OES_compressed_ETC1_RGB8_texture";
    static GLubyte *ext_cache = NULL;
    static size_t ext_cache_size = 0;

    const GLubyte *ext = glGetString(name);
    if (ext && ext[0] != '\0') {
      /* debugPrintf("GL: glGetString(GL_EXTENSIONS) -> driver string (%zu bytes)\n",
                  strlen((const char *)ext)); */
      return ext;
    }

    PFNGLGETSTRINGIPROC glGetStringiProc =
        (PFNGLGETSTRINGIPROC)SDL_GL_GetProcAddress("glGetStringi");
    if (glGetStringiProc) {
      GLint ext_count = 0;
      glGetIntegerv(0x821D, &ext_count); /* GL_NUM_EXTENSIONS */
      if (ext_count > 0) {
        size_t needed = 1;
        for (GLint i = 0; i < ext_count; i++) {
          const GLubyte *item = glGetStringiProc(name, (GLuint)i);
          if (item && item[0] != '\0')
            needed += strlen((const char *)item) + 1;
        }
        if (needed > 1) {
          GLubyte *buf = realloc(ext_cache, needed);
          if (buf) {
            ext_cache = buf;
            ext_cache_size = needed;
            size_t pos = 0;
            ext_cache[0] = '\0';
            for (GLint i = 0; i < ext_count; i++) {
              const GLubyte *item = glGetStringiProc(name, (GLuint)i);
              if (!item || item[0] == '\0')
                continue;
              size_t len = strlen((const char *)item);
              if (pos + len + 1 >= ext_cache_size)
                break;
              memcpy(ext_cache + pos, item, len);
              pos += len;
              ext_cache[pos++] = ' ';
            }
            if (pos > 0)
              pos--;
            ext_cache[pos] = '\0';
            debugPrintf(
                "GL: glGetString(GL_EXTENSIONS) -> rebuilt from glGetStringi (%d entries, %zu bytes)\n",
                ext_count, pos);
            return ext_cache;
          }
        }
      }
    }

    /* debugPrintf("GL: glGetString(GL_EXTENSIONS) -> fallback list (%zu bytes)\n",
                sizeof(fallback_ext) - 1); */
    return fallback_ext;
  }
  default: {
    const GLubyte *s = glGetString(name);
    /* debugPrintf("GL: glGetString(0x%x) = \"%s\"\n", name, s ? (const char *)s : "(null)"); */
    return s;
  }
  }
}

static void glGetIntegerv_wrap(GLenum pname, GLint *data) {
  egl_shim_ensure_current();
  glGetIntegerv(pname, data);
  uintptr_t ra = (uintptr_t)__builtin_return_address(0);
  uintptr_t off = 0;
  if (text_base && ra >= (uintptr_t)text_base &&
      ra < (uintptr_t)text_base + text_size) {
    off = ra - (uintptr_t)text_base;
  }
  static uintptr_t logged_callers[32];
  static int logged_count = 0;
  static int total_count = 0;
  int should_log = 0;
  total_count++;
  for (int i = 0; i < logged_count; i++) {
    if (logged_callers[i] == off) {
      should_log = total_count <= 16 || total_count % 2000 == 0;
      break;
    }
  }
  if (!should_log && logged_count < (int)(sizeof(logged_callers) / sizeof(logged_callers[0]))) {
    logged_callers[logged_count++] = off;
    should_log = 1;
  }
  if (should_log) {
    debugPrintf("GL: glGetIntegerv(0x%x) = %d from libmain.so+0x%lx count=%d\n",
                pname, data ? *data : -1, (unsigned long)off, total_count);
  }
}

static void glFrontFace_wrap(GLenum mode) {
  egl_shim_ensure_current();
  /* debugPrintf("GL: glFrontFace(0x%x)\n", mode); */
  glFrontFace(mode);
}

static GLuint glCreateShader_wrap(GLenum type) {
  egl_shim_ensure_current();
  GLuint s = glCreateShader(type);
  /* debugPrintf("GL: glCreateShader(0x%x) = %u\n", type, s); */
  return s;
}

static GLuint glCreateProgram_wrap(void) {
  egl_shim_ensure_current();
  GLuint p = glCreateProgram();
  /* debugPrintf("GL: glCreateProgram() = %u\n", p); */
  return p;
}

static void glGenTextures_wrap(GLsizei n, GLuint *textures) {
  egl_shim_ensure_current();
  glGenTextures(n, textures);
  /* debugPrintf("GL: glGenTextures(%d) = %u\n", n, textures ? textures[0] : 0); */
}

static void glGenFramebuffers_wrap(GLsizei n, GLuint *framebuffers) {
  egl_shim_ensure_current();
  glGenFramebuffers(n, framebuffers);
  debugPrintf("GL: glGenFramebuffers(%d) = %u\n", n, framebuffers ? framebuffers[0] : 0);
}

static void glGenBuffers_wrap(GLsizei n, GLuint *buffers) {
  egl_shim_ensure_current();
  glGenBuffers(n, buffers);
  /* debugPrintf("GL: glGenBuffers(%d) = %u\n", n, buffers ? buffers[0] : 0); */
}

static _Thread_local GLuint g_cur_fbo = 0;

static void glBindFramebuffer_wrap(GLenum target, GLuint framebuffer) {
  egl_shim_ensure_current();
  /* Mali-450 (Utgard): escrita em FBO (paleta CLUT -> textura VRAM) pode nao
     sincronizar antes da amostragem no draw seguinte -> sprites CLUT pretos.
     Forcar flush ao SAIR de um FBO nao-zero garante o RTT visivel. */
  if (target == GL_FRAMEBUFFER && g_cur_fbo != 0 && framebuffer != g_cur_fbo)
    glFinish();
  if (target == GL_FRAMEBUFFER)
    g_cur_fbo = framebuffer;
  glBindFramebuffer(target, framebuffer);
}

static void glShaderSource_wrap(GLuint shader, GLsizei count,
                                const GLchar *const *string,
                                const GLint *length) {
  egl_shim_ensure_current();
  if (!string || count <= 0) {
    glShaderSource(shader, count, string, length);
    return;
  }

  GLchar **patched = calloc((size_t)count, sizeof(*patched));
  const GLchar **patched_ptrs = calloc((size_t)count, sizeof(*patched_ptrs));
  GLint *patched_len = length ? calloc((size_t)count, sizeof(*patched_len)) : NULL;
  int changed = 0;

  if (!patched || !patched_ptrs) {
    free(patched);
    free(patched_ptrs);
    free(patched_len);
    glShaderSource(shader, count, string, length);
    return;
  }

  /* CLUT: texturas de indice sobem como GL_ALPHA -> sample (0,0,0,idx), mas o
     shader le o indice por .r (=0). O indice real esta em .a. O shader vem em
     varios segmentos: u_texClut e o corpo com ".r, 0)" podem estar separados,
     entao detectar CLUT no shader INTEIRO antes de patchar cada segmento. */
  int shader_has_clut = 0;
  for (GLsizei i = 0; i < count; i++) {
    const char *s = string[i];
    int l = length && length[i] >= 0 ? length[i] : (s ? (int)strlen(s) : 0);
    if (s && l > 0 && strstr(s, "u_texClut")) {
      shader_has_clut = 1;
      break;
    }
  }

  for (GLsizei i = 0; i < count; i++) {
    const char *src = string[i];
    int len = length && length[i] >= 0 ? length[i] : (src ? (int)strlen(src) : 0);
    patched_ptrs[i] = src;
    if (patched_len)
      patched_len[i] = len;
    int has_highp = src && len > 0 && strstr(src, "highp");
    int has_clut = shader_has_clut && src && len > 0 &&
                   (strstr(src, "v_color.rgb * 2.0") != NULL);
    if (!has_highp && !has_clut)
      continue;

    char *copy = malloc((size_t)len * 2 + 1);
    if (!copy)
      continue;
    int in = 0;
    int out = 0;
    while (in < len) {
      if (has_highp && in + 5 <= len && memcmp(src + in, "highp", 5) == 0) {
        memcpy(copy + out, "mediump", 7);
        in += 5;
        out += 7;
        changed = 1;
      } else {
        copy[out++] = src[in++];
      }
    }
    copy[out] = '\0';
    /* EXPERIMENTO: tirar v_color do resultado final do shader CLUT para
       testar se o preto vem da cor de vertice zerada. */
    if (has_clut) {
      char *p = strstr(copy, "d.rgb = v_color.rgb * 2.0 * d.rgb;");
      if (p) {
        memcpy(p, "d.rgb =               2.0 * d.rgb;", 34);
        changed = 1;
        debugPrintf("GL: glShaderSource(%u) CLUT sem v_color (probe)\n",
                    shader);
      }
      char *q = strstr(copy, "col.rgb = v_color.rgb * 2.0 * col.rgb;");
      if (q) {
        memcpy(q, "col.rgb =               2.0 * col.rgb;", 38);
        changed = 1;
        debugPrintf("GL: glShaderSource(%u) CLUTdir sem v_color (probe)\n",
                    shader);
      }
    }
    patched[i] = copy;
    patched_ptrs[i] = copy;
    if (patched_len)
      patched_len[i] = out;
  }

  if (changed)
    debugPrintf("GL: glShaderSource(%u) patched highp->mediump\n", shader);

  /* Dump unico da fonte de cada shader p/ mapear pipeline PSX/CLUT. */
  {
    static int dump_count = 0;
    if (dump_count < 90) {
      debugPrintf("GL: SHADERSRC id=%u BEGIN\n", shader);
      for (GLsizei i = 0; i < count; i++) {
        const char *s = patched_ptrs[i];
        int len = patched_len ? patched_len[i]
                              : (length && length[i] >= 0 ? length[i]
                                                          : (s ? (int)strlen(s) : 0));
        if (s && len > 0)
          debugPrintf("%.*s\n", len, s);
      }
      debugPrintf("GL: SHADERSRC id=%u END\n", shader);
      dump_count++;
    }
  }
  glShaderSource(shader, count, patched_ptrs, patched_len ? patched_len : length);

  for (GLsizei i = 0; i < count; i++)
    free(patched[i]);
  free(patched);
  free(patched_ptrs);
  free(patched_len);
}

static void glCompileShader_wrap(GLuint shader) {
  egl_shim_ensure_current();
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    GLchar log[1024];
    GLsizei len = 0;
    glGetShaderInfoLog(shader, sizeof(log), &len, log);
    debugPrintf("GL: glCompileShader(%u) FAILED: %.*s\n", shader, (int)len, log);
  }
}

static void glAttachShader_wrap(GLuint program, GLuint shader) {
  egl_shim_ensure_current();
  glAttachShader(program, shader);
}

#define TEX_MAX 8192
static short g_tex_w[TEX_MAX];
static short g_tex_h[TEX_MAX];
static unsigned char g_tex_fmt_alpha[TEX_MAX]; /* 1 = ultima subida GL_ALPHA */

#define PSX_PROG_MAX 4096
static unsigned char g_psx_prog[PSX_PROG_MAX];
static GLint g_loc_clutaddr[PSX_PROG_MAX];
static GLint g_loc_rclutw[PSX_PROG_MAX];
static GLint g_loc_texsize[PSX_PROG_MAX];
static _Thread_local GLuint g_cur_prog = 0;

static void glLinkProgram_wrap(GLuint program) {
  egl_shim_ensure_current();
  glLinkProgram(program);
  {
    static int link_log = 0;
    if (link_log < 90) {
      GLuint sh[4] = {0, 0, 0, 0};
      GLsizei n = 0;
      glGetAttachedShaders(program, 4, &n, sh);
      debugPrintf("GL: LINKMAP program=%u shaders=%u,%u\n", program, sh[0],
                  sh[1]);
      link_log++;
    }
  }
  if (program < PSX_PROG_MAX) {
    GLint lc = glGetUniformLocation(program, "u_texClut");
    if (lc >= 0) {
      g_psx_prog[program] = 1;
      g_loc_clutaddr[program] = glGetUniformLocation(program, "u_clutAddr");
      g_loc_rclutw[program] = glGetUniformLocation(program, "u_rclutWSize");
      g_loc_texsize[program] = glGetUniformLocation(program, "u_texSize");
      debugPrintf("GL: PSXPROG %u clutAddr@%d rclutW@%d texSize@%d\n", program,
                  g_loc_clutaddr[program], g_loc_rclutw[program],
                  g_loc_texsize[program]);
    }
  }
  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    GLchar log[1024];
    GLsizei len = 0;
    glGetProgramInfoLog(program, sizeof(log), &len, log);
    debugPrintf("GL: glLinkProgram(%u) FAILED: %.*s\n", program, (int)len, log);
  }
}

static void glBindBuffer_wrap(GLenum target, GLuint buffer) {
  egl_shim_ensure_current();
  glBindBuffer(target, buffer);
}

static void glBufferData_wrap(GLenum target, GLsizeiptr size, const void *data,
                              GLenum usage) {
  egl_shim_ensure_current();
  glBufferData(target, size, data, usage);
}

static void glBufferSubData_wrap(GLenum target, GLintptr offset, GLsizeiptr size,
                                 const void *data) {
  egl_shim_ensure_current();
  glBufferSubData(target, offset, size, data);
}

static void glBindTexture_wrap(GLenum target, GLuint texture) {
  egl_shim_ensure_current();
  glBindTexture(target, texture);
}

static void gl_drain_errors(const char *where);

typedef struct PixelStoreCompat {
  GLint pack_alignment;
  GLint unpack_alignment;
  GLint pack_row_length;
  GLint pack_skip_rows;
  GLint pack_skip_pixels;
  GLint pack_image_height;
  GLint pack_skip_images;
  GLint unpack_row_length;
  GLint unpack_skip_rows;
  GLint unpack_skip_pixels;
  GLint unpack_image_height;
  GLint unpack_skip_images;
} PixelStoreCompat;

typedef struct PixelUploadCompat {
  const void *pixels;
  unsigned char *copy;
  int copied;
  int forced_alignment;
} PixelUploadCompat;

static _Thread_local PixelStoreCompat g_pixel_store = {
    .pack_alignment = 4,
    .unpack_alignment = 4,
};

static void gl_drain_errors(const char *where) {
  GLenum err = GL_NO_ERROR;
  int count = 0;
  static int log_count = 0;
  while ((err = glGetError()) != GL_NO_ERROR && count < 16) {
    if (log_count < 80) {
      debugPrintf("GL: cleared stale error 0x%x before %s\n", err, where);
      log_count++;
    }
    count++;
  }
}

static void gl_log_and_clear_error(const char *where) {
  GLenum err = glGetError();
  static int log_count = 0;
  if (err == GL_NO_ERROR)
    return;
  if (log_count < 120) {
    debugPrintf("GL: %s generated/left error 0x%x\n", where, err);
    log_count++;
  }
  gl_drain_errors(where);
}

static int gl_valid_alignment(GLint param) {
  return param == 1 || param == 2 || param == 4 || param == 8;
}

static GLint gl_nonnegative(GLint param) {
  return param > 0 ? param : 0;
}

static size_t align_up_size(size_t value, GLint alignment) {
  size_t a = gl_valid_alignment(alignment) ? (size_t)alignment : 4u;
  return (value + a - 1u) & ~(a - 1u);
}

static int gl_pixel_size_bytes(GLenum format, GLenum type) {
  if (type == GL_UNSIGNED_BYTE) {
    switch (format) {
    case GL_RGBA:
    case 0x80e1: /* GL_BGRA_EXT */
      return 4;
    case GL_RGB:
      return 3;
    case GL_LUMINANCE_ALPHA:
      return 2;
    case GL_ALPHA:
    case GL_LUMINANCE:
      return 1;
    default:
      return 0;
    }
  }

  if (type == GL_UNSIGNED_SHORT_5_6_5 && format == GL_RGB)
    return 2;
  if ((type == GL_UNSIGNED_SHORT_4_4_4_4 ||
       type == GL_UNSIGNED_SHORT_5_5_5_1) &&
      (format == GL_RGBA || format == 0x80e1))
    return 2;
  return 0;
}

static void log_upload_sample(const char *op, GLenum target, GLint level,
                              GLint internalformat, GLsizei width,
                              GLsizei height, GLenum format, GLenum type,
                              const void *pixels, int copied) {
  static int log_count = 0;
  if (!pixels || width <= 0 || height <= 0 || log_count >= 260)
    return;

  int bpp = gl_pixel_size_bytes(format, type);
  if (bpp <= 0)
    return;

  int interesting = copied || (width >= 128 && height >= 128);
  if (!interesting)
    return;

  const unsigned char *p = (const unsigned char *)pixels;
  size_t total_pixels = (size_t)width * (size_t)height;
  size_t step = total_pixels / 192u;
  if (step == 0)
    step = 1;

  unsigned int nonzero = 0;
  unsigned int rgb_nonzero = 0;
  unsigned int alpha_nonzero = 0;
  unsigned int samples = 0;
  unsigned int rgb_min = 255;
  unsigned int rgb_max = 0;
  unsigned int alpha_min = 255;
  unsigned int alpha_max = 0;
  int has_alpha = (format == GL_RGBA || format == 0x80e1 ||
                   format == GL_LUMINANCE_ALPHA);
  int alpha_index = (format == GL_LUMINANCE_ALPHA) ? 1 : 3;

  for (size_t i = 0; i < total_pixels; i += step) {
    const unsigned char *px = p + i * (size_t)bpp;
    for (int c = 0; c < bpp; c++) {
      if (px[c] != 0) {
        nonzero++;
        break;
      }
    }
    if (format == GL_RGBA || format == 0x80e1 || format == GL_RGB) {
      int channels = format == GL_RGB ? 3 : 3;
      for (int c = 0; c < channels; c++) {
        unsigned int v = px[c];
        if (v < rgb_min)
          rgb_min = v;
        if (v > rgb_max)
          rgb_max = v;
        if (v != 0) {
          rgb_nonzero++;
          break;
        }
      }
    } else if (format == GL_LUMINANCE || format == GL_LUMINANCE_ALPHA) {
      unsigned int v = px[0];
      if (v < rgb_min)
        rgb_min = v;
      if (v > rgb_max)
        rgb_max = v;
      if (v != 0)
        rgb_nonzero++;
    }
    if (has_alpha && bpp > alpha_index) {
      unsigned int a = px[alpha_index];
      if (a < alpha_min)
        alpha_min = a;
      if (a > alpha_max)
        alpha_max = a;
      if (a)
        alpha_nonzero++;
    }
    samples++;
  }
  if (!has_alpha) {
    alpha_min = 0;
    alpha_max = 0;
  }
  if (rgb_min == 255 && rgb_max == 0) {
    rgb_min = 0;
    rgb_max = 0;
  }

  GLint tex = 0;
  if (target == GL_TEXTURE_2D)
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);

  uintptr_t ra = (uintptr_t)__builtin_return_address(0);
  uintptr_t off = 0;
  if (text_base && ra >= (uintptr_t)text_base &&
      ra < (uintptr_t)text_base + text_size)
    off = ra - (uintptr_t)text_base;

  debugPrintf("GL: %s upload tex=%d level=%d ifmt=0x%x fmt=0x%x type=0x%x "
              "size=%dx%d copied=%d ps(row=%d skip=%d,%d align=%d) "
              "first=%02x %02x %02x %02x nz=%u/%u rgb=%u/%u "
              "rgb_range=%u-%u alpha=%u/%u alpha_range=%u-%u "
              "caller=libmain.so+0x%lx\n",
              op, tex, level, internalformat, format, type, width, height,
              copied, g_pixel_store.unpack_row_length,
              g_pixel_store.unpack_skip_pixels, g_pixel_store.unpack_skip_rows,
              g_pixel_store.unpack_alignment, p[0], bpp > 1 ? p[1] : 0,
              bpp > 2 ? p[2] : 0, bpp > 3 ? p[3] : 0, nonzero, samples,
              rgb_nonzero, samples, rgb_min, rgb_max, alpha_nonzero, samples,
              alpha_min, alpha_max, (unsigned long)off);
  log_count++;
}

static void pixel_upload_prepare(GLsizei width, GLsizei height, GLenum format,
                                 GLenum type, const void *pixels,
                                 int force_tight,
                                 PixelUploadCompat *upload) {
  memset(upload, 0, sizeof(*upload));
  upload->pixels = pixels;
  if (!pixels || width <= 0 || height <= 0)
    return;

  int bpp = gl_pixel_size_bytes(format, type);
  if (bpp <= 0)
    return;

  GLint row_pixels = g_pixel_store.unpack_row_length > 0
                         ? g_pixel_store.unpack_row_length
                         : width;
  if (row_pixels < width + g_pixel_store.unpack_skip_pixels)
    row_pixels = width + g_pixel_store.unpack_skip_pixels;

  size_t src_row_bytes =
      align_up_size((size_t)row_pixels * (size_t)bpp,
                    g_pixel_store.unpack_alignment);
  size_t dst_row_bytes = (size_t)width * (size_t)bpp;
  size_t total_bytes = dst_row_bytes * (size_t)height;
  int active = force_tight || g_pixel_store.unpack_row_length > 0 ||
               g_pixel_store.unpack_skip_rows > 0 ||
               g_pixel_store.unpack_skip_pixels > 0 ||
               g_pixel_store.unpack_skip_images > 0;

  if (!active)
    return;

  unsigned char *copy = (unsigned char *)malloc(total_bytes);
  if (!copy)
    return;

  GLint image_rows = g_pixel_store.unpack_image_height > 0
                         ? g_pixel_store.unpack_image_height
                         : height;
  const unsigned char *src = (const unsigned char *)pixels;
  src += (size_t)g_pixel_store.unpack_skip_images * (size_t)image_rows *
         src_row_bytes;
  src += (size_t)g_pixel_store.unpack_skip_rows * src_row_bytes;
  src += (size_t)g_pixel_store.unpack_skip_pixels * (size_t)bpp;

  for (GLsizei y = 0; y < height; y++)
    memcpy(copy + (size_t)y * dst_row_bytes, src + (size_t)y * src_row_bytes,
           dst_row_bytes);

  upload->pixels = copy;
  upload->copy = copy;
  upload->copied = 1;
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  upload->forced_alignment = 1;

  static int log_count = 0;
  if (log_count < 120) {
    debugPrintf("GL: emulated unpack row=%d skip=%d,%d image=%d/%d align=%d "
                "upload=%dx%d fmt=0x%x type=0x%x bpp=%d\n",
                g_pixel_store.unpack_row_length,
                g_pixel_store.unpack_skip_pixels,
                g_pixel_store.unpack_skip_rows,
                g_pixel_store.unpack_image_height,
                g_pixel_store.unpack_skip_images,
                g_pixel_store.unpack_alignment, width, height, format, type,
                bpp);
    log_count++;
  }
}

static void pixel_upload_finish(PixelUploadCompat *upload) {
  if (upload->forced_alignment)
    glPixelStorei(GL_UNPACK_ALIGNMENT, g_pixel_store.unpack_alignment);
  free(upload->copy);
}

static unsigned char *convert_bgra_to_rgba(GLsizei width, GLsizei height,
                                           const void *pixels) {
  if (!pixels || width <= 0 || height <= 0)
    return NULL;
  size_t total = (size_t)width * (size_t)height;
  unsigned char *rgba = (unsigned char *)malloc(total * 4u);
  if (!rgba)
    return NULL;
  const unsigned char *src = (const unsigned char *)pixels;
  for (size_t i = 0; i < total; i++) {
    rgba[i * 4u + 0u] = src[i * 4u + 2u];
    rgba[i * 4u + 1u] = src[i * 4u + 1u];
    rgba[i * 4u + 2u] = src[i * 4u + 0u];
    rgba[i * 4u + 3u] = src[i * 4u + 3u];
  }
  return rgba;
}

void glPixelStorei_wrap(GLenum pname, GLint param) {
  egl_shim_ensure_current();
  gl_drain_errors("glPixelStorei");
  GLenum err = GL_NO_ERROR;

  switch (pname) {
  case GL_PACK_ALIGNMENT:
    if (!gl_valid_alignment(param)) {
      debugPrintf("GL: ignored invalid glPixelStorei PACK_ALIGNMENT=%d\n",
                  param);
      return;
    }
    g_pixel_store.pack_alignment = param;
    glPixelStorei(pname, param);
    err = glGetError();
    if (err != GL_NO_ERROR) {
      debugPrintf("GL: glPixelStorei error=0x%x pname=0x%x param=%d\n",
                  err, pname, param);
      gl_drain_errors("glPixelStorei");
    }
    return;
  case GL_UNPACK_ALIGNMENT:
    if (!gl_valid_alignment(param)) {
      debugPrintf("GL: ignored invalid glPixelStorei UNPACK_ALIGNMENT=%d\n",
                  param);
      return;
    }
    g_pixel_store.unpack_alignment = param;
    glPixelStorei(pname, param);
    err = glGetError();
    if (err != GL_NO_ERROR) {
      debugPrintf("GL: glPixelStorei error=0x%x pname=0x%x param=%d\n",
                  err, pname, param);
      gl_drain_errors("glPixelStorei");
    }
    return;
  case 0x0d02: /* GL_PACK_ROW_LENGTH */
    g_pixel_store.pack_row_length = gl_nonnegative(param);
    return;
  case 0x0d03: /* GL_PACK_SKIP_ROWS */
    g_pixel_store.pack_skip_rows = gl_nonnegative(param);
    return;
  case 0x0d04: /* GL_PACK_SKIP_PIXELS */
    g_pixel_store.pack_skip_pixels = gl_nonnegative(param);
    return;
  case 0x806b: /* GL_PACK_SKIP_IMAGES */
    g_pixel_store.pack_skip_images = gl_nonnegative(param);
    return;
  case 0x806c: /* GL_PACK_IMAGE_HEIGHT */
    g_pixel_store.pack_image_height = gl_nonnegative(param);
    return;
  case 0x0cf2: /* GL_UNPACK_ROW_LENGTH */
    g_pixel_store.unpack_row_length = gl_nonnegative(param);
    return;
  case 0x0cf3: /* GL_UNPACK_SKIP_ROWS */
    g_pixel_store.unpack_skip_rows = gl_nonnegative(param);
    return;
  case 0x0cf4: /* GL_UNPACK_SKIP_PIXELS */
    g_pixel_store.unpack_skip_pixels = gl_nonnegative(param);
    return;
  case 0x806d: /* GL_UNPACK_SKIP_IMAGES */
    g_pixel_store.unpack_skip_images = gl_nonnegative(param);
    return;
  case 0x806e: /* GL_UNPACK_IMAGE_HEIGHT */
    g_pixel_store.unpack_image_height = gl_nonnegative(param);
    return;
  case 0x0cf0: /* GL_UNPACK_SWAP_BYTES */
  case 0x0cf1: /* GL_UNPACK_LSB_FIRST */
  case 0x0d00: /* GL_PACK_SWAP_BYTES */
  case 0x0d01: /* GL_PACK_LSB_FIRST */
    if (param != 0) {
      static int log_count = 0;
      if (log_count < 32) {
        debugPrintf("GL: ignored unsupported glPixelStorei pname=0x%x param=%d\n",
                    pname, param);
        log_count++;
      }
    }
    return;
  default:
    glPixelStorei(pname, param);
    err = glGetError();
    if (err != GL_NO_ERROR) {
      static int log_count = 0;
      if (log_count < 80) {
        debugPrintf("GL: glPixelStorei error=0x%x pname=0x%x param=%d\n",
                    err, pname, param);
        log_count++;
      }
      gl_drain_errors("glPixelStorei");
    }
    return;
  }
}

typedef int (*astc_decode_fn)(const unsigned char *data, unsigned long len,
                              int w, int h, int bx, int by,
                              unsigned char *out_rgba);

static int astc_block_dims(GLenum fmt, int *bx, int *by) {
  GLenum f = fmt;
  if (f >= 0x93d0 && f <= 0x93dd)
    f -= 0x20;
  switch (f) {
  case 0x93b0: *bx = 4; *by = 4; return 1;
  case 0x93b1: *bx = 5; *by = 4; return 1;
  case 0x93b2: *bx = 5; *by = 5; return 1;
  case 0x93b3: *bx = 6; *by = 5; return 1;
  case 0x93b4: *bx = 6; *by = 6; return 1;
  case 0x93b5: *bx = 8; *by = 5; return 1;
  case 0x93b6: *bx = 8; *by = 6; return 1;
  case 0x93b7: *bx = 8; *by = 8; return 1;
  case 0x93b8: *bx = 10; *by = 5; return 1;
  case 0x93b9: *bx = 10; *by = 6; return 1;
  case 0x93ba: *bx = 10; *by = 8; return 1;
  case 0x93bb: *bx = 10; *by = 10; return 1;
  case 0x93bc: *bx = 12; *by = 10; return 1;
  case 0x93bd: *bx = 12; *by = 12; return 1;
  default: return 0;
  }
}

static astc_decode_fn get_astc_decoder(void) {
  static int tried = 0;
  static astc_decode_fn fn = NULL;
  const char *paths[] = {
      "./libs/libsor4astc.so",
      "/storage/roms/ports/legendofmana/libs/libsor4astc.so",
      "libsor4astc.so",
      NULL,
  };

  if (tried)
    return fn;
  tried = 1;
  for (int i = 0; paths[i] && !fn; i++) {
    void *h = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
      debugPrintf("ASTC: dlopen %s failed: %s\n", paths[i], dlerror());
      continue;
    }
    fn = (astc_decode_fn)dlsym(h, "sor4_astc_decode");
    if (!fn)
      debugPrintf("ASTC: sor4_astc_decode missing in %s\n", paths[i]);
    else
      debugPrintf("ASTC: decoder loaded from %s\n", paths[i]);
  }
  return fn;
}

static void fill_astc_fallback(unsigned char *rgba, int width, int height) {
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      unsigned char *p = rgba + ((size_t)y * (size_t)width + (size_t)x) * 4;
      int checker = ((x >> 3) ^ (y >> 3)) & 1;
      p[0] = checker ? 180 : 70;
      p[1] = checker ? 40 : 70;
      p[2] = checker ? 180 : 70;
      p[3] = 255;
    }
  }
}

static int upload_astc_rgba(GLenum target, GLint level, GLenum astc_fmt,
                            GLsizei width, GLsizei height, GLint border,
                            const void *pixels, size_t source_size,
                            const char *caller) {
  int bx = 0, by = 0;
  if (!astc_block_dims(astc_fmt, &bx, &by) || width <= 0 || height <= 0)
    return 0;

  size_t rgba_size = (size_t)width * (size_t)height * 4u;
  size_t expected_size =
      (size_t)((width + bx - 1) / bx) * (size_t)((height + by - 1) / by) *
      16u;
  size_t decode_size = source_size ? source_size : expected_size;
  unsigned char *rgba = (unsigned char *)malloc(rgba_size);
  static int astc_log_count = 0;
  int decoded = -1;

  if (!rgba)
    return 0;

  astc_decode_fn dec = get_astc_decoder();
  if (dec && pixels)
    decoded = dec((const unsigned char *)pixels, (unsigned long)decode_size,
                  width, height, bx, by, rgba);
  if (decoded != 0)
    fill_astc_fallback(rgba, width, height);
  int astc_index = astc_log_count++;
  if (astc_index < 120 || (width >= 128 && height >= 128 &&
                           (astc_index % 64) == 0)) {
    GLint tex = 0;
    if (target == GL_TEXTURE_2D)
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    unsigned int nonzero = 0;
    unsigned int alpha_nonzero = 0;
    unsigned int samples = 0;
    size_t total_pixels = (size_t)width * (size_t)height;
    size_t step = total_pixels / 192u;
    if (step == 0)
      step = 1;
    for (size_t i = 0; i < total_pixels; i += step) {
      const unsigned char *p = rgba + i * 4u;
      if (p[0] || p[1] || p[2] || p[3])
        nonzero++;
      if (p[3])
        alpha_nonzero++;
      samples++;
    }
    debugPrintf("ASTC: %s tex=%d fmt=0x%x %dx%d blk=%dx%d src=%zu "
                "expected=%zu %s first=%02x %02x %02x %02x nz=%u/%u "
                "alpha=%u/%u\n",
                caller, tex, astc_fmt, width, height, bx, by, decode_size,
                expected_size, decoded == 0 ? "decoded" : "fallback",
                rgba[0], rgba[1], rgba[2], rgba[3], nonzero, samples,
                alpha_nonzero, samples);
  }
  glTexImage2D(target, level, GL_RGBA, width, height, border, GL_RGBA,
               GL_UNSIGNED_BYTE, rgba);
  if (level == 0 && target == GL_TEXTURE_2D) {
    GLint bt = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bt);
    if (bt > 0 && bt < TEX_MAX) {
      g_tex_w[bt] = (short)width;
      g_tex_h[bt] = (short)height;
      g_tex_fmt_alpha[bt] = 2; /* 2 = veio de ASTC */
    }
    GLint mnf = 0;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &mnf);
    if (mnf == 0x2702)
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  }
  free(rgba);
  return 1;
}

void glCompressedTexImage2D_wrap(GLenum target, GLint level,
                                 GLenum internalformat, GLsizei width,
                                 GLsizei height, GLint border,
                                 GLsizei imageSize, const void *data) {
  egl_shim_ensure_current();

  if (upload_astc_rgba(target, level, internalformat, width, height, border,
                      data, imageSize > 0 ? (size_t)imageSize : 0,
                      "glCompressedTexImage2D"))
    return;

  glCompressedTexImage2D(target, level, internalformat, width, height, border,
                         imageSize, data);
}

void glTexImage2D_wrap(GLenum target, GLint level, GLint internalformat,
                       GLsizei width, GLsizei height, GLint border,
                       GLenum format, GLenum type, const void *pixels) {
  egl_shim_ensure_current();
  gl_drain_errors("glTexImage2D");

  int bx = 0, by = 0;
  GLenum astc_fmt = 0;
  if (astc_block_dims((GLenum)internalformat, &bx, &by))
    astc_fmt = (GLenum)internalformat;
  else if (astc_block_dims(format, &bx, &by))
    astc_fmt = format;

  if (upload_astc_rgba(target, level, astc_fmt, width, height, border, pixels,
                      0, "glTexImage2D"))
    return;

  switch (internalformat) {
  case 0x8058: /* GL_RGBA8 */
  case 0x8057: /* GL_RGB5_A1 */
  case 0x8056: /* GL_RGBA4 */
  case 0x80e1: /* GL_BGRA_EXT */
    internalformat = GL_RGBA;
    break;
  case 0x8051: /* GL_RGB8 */
  case 0x8d62: /* GL_RGB565 */
    internalformat = GL_RGB;
    break;
  default:
    break;
  }

  int convert_bgra = (format == 0x80e1 && type == GL_UNSIGNED_BYTE);
  /* NAO relabelar GL_ALPHA: a cadeia de fontes/texto le .a (relabel p/
     LUMINANCE deixou as fontes feias — confirmado no device 2026-07-06). */
  PixelUploadCompat upload;
  pixel_upload_prepare(width, height, format, type, pixels, convert_bgra,
                       &upload);
  const void *upload_pixels = upload.pixels;
  unsigned char *rgba = NULL;
  if (convert_bgra) {
    if (upload_pixels)
      rgba = convert_bgra_to_rgba(width, height, upload_pixels);
    if (rgba)
      upload_pixels = rgba;
    format = GL_RGBA;
    internalformat = GL_RGBA;
  }

  if (!pixels) {
    static int alloc_log = 0;
    if (alloc_log < 200) {
      GLint tex = 0;
      if (target == GL_TEXTURE_2D)
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
      debugPrintf("GL: glTexImage2D alloc tex=%d level=%d ifmt=0x%x fmt=0x%x "
                  "type=0x%x size=%dx%d\n",
                  tex, level, internalformat, format, type, width, height);
      alloc_log++;
    }
  }

  if (level == 0 && target == GL_TEXTURE_2D) {
    GLint bt = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bt);
    if (bt > 0 && bt < TEX_MAX) {
      g_tex_w[bt] = (short)width;
      g_tex_h[bt] = (short)height;
      g_tex_fmt_alpha[bt] = (format == GL_ALPHA) ? 1 : 0;
    }
    /* GLES2: default de MIN_FILTER e mipmap (0x2702). O jogo nao seta filtro
       em varias texturas (indices CLUT etc.) e nunca gera mipmaps -> textura
       INCOMPLETA -> sample = preto. Se o filtro ainda e o default intocado,
       trocar por LINEAR na propria textura bound (sem trocar binding — o
       glTextureSafeDefaults antigo quebrava por mexer em binding). */
    GLint mnf = 0;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &mnf);
    if (mnf == 0x2702)
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  }
  /* Paletas CLUT: uploads minusculos (ex.: 16x1). Logar conteudo bruto para
     saber se a cor ja chega preta ou se some depois (transferencia FBO). */
  if (upload_pixels && width <= 64 && height <= 4 && type == GL_UNSIGNED_BYTE) {
    static int pal_log = 0;
    if (pal_log < 80) {
      GLint bt = 0;
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &bt);
      int bpp = gl_pixel_size_bytes(format, type);
      const unsigned char *p = (const unsigned char *)upload_pixels;
      char hex[3 * 32 + 1];
      int nb = width * (bpp > 0 ? bpp : 1);
      if (nb > 32)
        nb = 32;
      for (int i = 0; i < nb; i++)
        sprintf(hex + i * 3, "%02x ", p[i]);
      hex[nb * 3] = '\0';
      debugPrintf("GL: PAL upload tex=%d fmt=0x%x %dx%d: %s\n", bt, format,
                  width, height, hex);
      pal_log++;
    }
  }
  log_upload_sample("glTexImage2D", target, level, internalformat, width,
                    height, format, type, upload_pixels, upload.copied);
  glTexImage2D(target, level, internalformat, width, height, border, format, type,
               upload_pixels);
  free(rgba);
  pixel_upload_finish(&upload);
  GLenum err = glGetError();
  if (err != GL_NO_ERROR) {
    static int log_count = 0;
    if (log_count < 120) {
      debugPrintf("GL: glTexImage2D error=0x%x target=0x%x level=%d "
                  "ifmt=0x%x fmt=0x%x type=0x%x size=%dx%d pixels=%p\n",
                  err, target, level, internalformat, format, type, width,
                  height, pixels);
      log_count++;
    }
    gl_drain_errors("glTexImage2D");
  }
}

void glTexSubImage2D_wrap(GLenum target, GLint level, GLint xoffset,
                          GLint yoffset, GLsizei width, GLsizei height,
                          GLenum format, GLenum type, const void *pixels) {
  egl_shim_ensure_current();
  gl_drain_errors("glTexSubImage2D");

  int convert_bgra = (format == 0x80e1 && type == GL_UNSIGNED_BYTE);
  PixelUploadCompat upload;
  pixel_upload_prepare(width, height, format, type, pixels, convert_bgra,
                       &upload);
  const void *upload_pixels = upload.pixels;
  unsigned char *rgba = NULL;
  if (convert_bgra) {
    if (upload_pixels)
      rgba = convert_bgra_to_rgba(width, height, upload_pixels);
    if (rgba)
      upload_pixels = rgba;
    format = GL_RGBA;
  }

  log_upload_sample("glTexSubImage2D", target, level, 0, width, height,
                    format, type, upload_pixels, upload.copied);
  glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type,
                  upload_pixels);
  free(rgba);
  pixel_upload_finish(&upload);
  GLenum err = glGetError();
  if (err != GL_NO_ERROR) {
    static int log_count = 0;
    if (log_count < 80) {
      debugPrintf("GL: glTexSubImage2D error=0x%x target=0x%x level=%d "
                  "fmt=0x%x type=0x%x off=%d,%d size=%dx%d pixels=%p\n",
                  err, target, level, format, type, xoffset, yoffset, width,
                  height, pixels);
      log_count++;
    }
    gl_drain_errors("glTexSubImage2D");
  }
}

void glTexParameteri_wrap(GLenum target, GLenum pname, GLint param) {
  egl_shim_ensure_current();
  gl_drain_errors("glTexParameteri");
  if (pname == 0x813d || pname == 0x8072)
    return;
  if ((pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T) &&
      param == 0x2900) {
    param = GL_CLAMP_TO_EDGE;
  }
  if (pname == GL_TEXTURE_MIN_FILTER &&
      (param == 0x2700 || param == 0x2701 || param == 0x2702 ||
       param == 0x2703)) {
    param = GL_LINEAR;
  }
  if (pname == GL_TEXTURE_MAG_FILTER &&
      param != GL_NEAREST && param != GL_LINEAR) {
    param = GL_LINEAR;
  }
  glTexParameteri(target, pname, param);
  gl_log_and_clear_error("glTexParameteri");
}

static void glUseProgram_wrap(GLuint program) {
  egl_shim_ensure_current();
  g_cur_prog = program;
  glUseProgram(program);
}

static void glUniform1f_wrap(GLint location, GLfloat v0) {
  egl_shim_ensure_current();
  if (g_cur_prog < PSX_PROG_MAX && g_psx_prog[g_cur_prog] &&
      location == g_loc_rclutw[g_cur_prog]) {
    static int rw_log = 0;
    if (rw_log < 60) {
      debugPrintf("GL: PSX rclutWSize prog=%u = %f\n", g_cur_prog, v0);
      rw_log++;
    }
  }
  glUniform1f(location, v0);
}

static void glUniform2f_wrap(GLint location, GLfloat v0, GLfloat v1) {
  egl_shim_ensure_current();
  if (g_cur_prog < PSX_PROG_MAX && g_psx_prog[g_cur_prog]) {
    if (location == g_loc_clutaddr[g_cur_prog]) {
      /* u_clutAddr vem enorme (endereco VRAM PSX); no highp do device real o
         texture2D usa so a parte fracionaria (wrap). No mediump do Mali-450 o
         valor estoura fp16 -> lookup preto. Pre-fract na CPU (a soma com o
         indice fica < 1, entao serve com CLAMP ou REPEAT). */
      float f0 = v0 - floorf(v0);
      float f1 = v1 - floorf(v1);
      static int ca_log = 0;
      if (ca_log < 60) {
        debugPrintf("GL: PSX clutAddr prog=%u %f,%f -> fract %f,%f\n",
                    g_cur_prog, v0, v1, f0, f1);
        ca_log++;
      }
      glUniform2f(location, f0, f1);
      return;
    } else if (location == g_loc_texsize[g_cur_prog]) {
      static int ts_log = 0;
      if (ts_log < 60) {
        debugPrintf("GL: PSX texSize prog=%u = %f,%f\n", g_cur_prog, v0, v1);
        ts_log++;
      }
    }
  }
  glUniform2f(location, v0, v1);
}

static void glUniform3f_wrap(GLint location, GLfloat v0, GLfloat v1,
                             GLfloat v2) {
  egl_shim_ensure_current();
  glUniform3f(location, v0, v1, v2);
}

static void glUniform4f_wrap(GLint location, GLfloat v0, GLfloat v1,
                             GLfloat v2, GLfloat v3) {
  egl_shim_ensure_current();
  glUniform4f(location, v0, v1, v2, v3);
}

static void glUniform1i_wrap(GLint location, GLint v0) {
  egl_shim_ensure_current();
  glUniform1i(location, v0);
}

void glUniform1fv_wrap(GLint location, GLsizei count,
                       const GLfloat *value) {
  egl_shim_ensure_current();
  glUniform1fv(location, count, value);
}

void glUniform2fv_wrap(GLint location, GLsizei count,
                       const GLfloat *value) {
  egl_shim_ensure_current();
  if (g_cur_prog < PSX_PROG_MAX && g_psx_prog[g_cur_prog] && value &&
      count == 1 && location == g_loc_clutaddr[g_cur_prog]) {
    float f[2] = {value[0] - floorf(value[0]), value[1] - floorf(value[1])};
    static int ca_log = 0;
    if (ca_log < 40) {
      debugPrintf("GL: PSX clutAddrV prog=%u %f,%f -> fract %f,%f\n",
                  g_cur_prog, value[0], value[1], f[0], f[1]);
      ca_log++;
    }
    glUniform2fv(location, 1, f);
    return;
  }
  glUniform2fv(location, count, value);
}

void glUniform3fv_wrap(GLint location, GLsizei count,
                       const GLfloat *value) {
  egl_shim_ensure_current();
  glUniform3fv(location, count, value);
}

void glUniform4fv_wrap(GLint location, GLsizei count,
                       const GLfloat *value) {
  egl_shim_ensure_current();
  glUniform4fv(location, count, value);
}

static void glUniform1iv_wrap(GLint location, GLsizei count,
                              const GLint *value) {
  egl_shim_ensure_current();
  glUniform1iv(location, count, value);
}

static void glUniform2iv_wrap(GLint location, GLsizei count,
                              const GLint *value) {
  egl_shim_ensure_current();
  glUniform2iv(location, count, value);
}

static void glUniform3iv_wrap(GLint location, GLsizei count,
                              const GLint *value) {
  egl_shim_ensure_current();
  glUniform3iv(location, count, value);
}

static void glUniform4iv_wrap(GLint location, GLsizei count,
                              const GLint *value) {
  egl_shim_ensure_current();
  glUniform4iv(location, count, value);
}

static void glUniformMatrix2fv_wrap(GLint location, GLsizei count,
                                    GLboolean transpose,
                                    const GLfloat *value) {
  egl_shim_ensure_current();
  glUniformMatrix2fv(location, count, transpose, value);
}

static void glUniformMatrix3fv_wrap(GLint location, GLsizei count,
                                    GLboolean transpose,
                                    const GLfloat *value) {
  egl_shim_ensure_current();
  glUniformMatrix3fv(location, count, transpose, value);
}

static void glUniformMatrix4fv_wrap(GLint location, GLsizei count,
                                    GLboolean transpose,
                                    const GLfloat *value) {
  egl_shim_ensure_current();
  glUniformMatrix4fv(location, count, transpose, value);
}

void glCopyTexImage2D_wrap(GLenum target, GLint level, GLenum internalformat,
                           GLint x, GLint y, GLsizei width, GLsizei height,
                           GLint border) {
  egl_shim_ensure_current();
  static int ct_log = 0;
  if (ct_log < 120) {
    GLint tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    debugPrintf("GL: glCopyTexImage2D tex=%d level=%d ifmt=0x%x %d,%d %dx%d\n",
                tex, level, internalformat, x, y, width, height);
    ct_log++;
  }
  glCopyTexImage2D(target, level, internalformat, x, y, width, height, border);
}

void glCopyTexSubImage2D_wrap(GLenum target, GLint level, GLint xoffset,
                              GLint yoffset, GLint x, GLint y, GLsizei width,
                              GLsizei height) {
  egl_shim_ensure_current();
  static int cts_log = 0;
  if (cts_log < 200) {
    GLint tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    debugPrintf("GL: glCopyTexSubImage2D tex=%d level=%d off=%d,%d src=%d,%d "
                "%dx%d\n",
                tex, level, xoffset, yoffset, x, y, width, height);
    cts_log++;
  }
  glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
}

static void glFramebufferTexture2D_wrap(GLenum target, GLenum attachment,
                                        GLenum textarget, GLuint texture,
                                        GLint level) {
  egl_shim_ensure_current();
  /* GLES2: min-filter DEFAULT e mipmap (0x2702). Render target (VRAM/CLUT
     PSX) nunca tem mipmaps -> textura INCOMPLETA -> toda amostragem = preto
     opaco (logo do titulo/personagens pretos). Ao anexar num FBO, garantir
     min-filter sem mipmap. So afeta render targets. */
  if (textarget == GL_TEXTURE_2D && texture > 0) {
    GLint prev = 0, mn = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &mn);
    if (mn == 0x2700 || mn == 0x2701 || mn == 0x2702 || mn == 0x2703) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      debugPrintf("GL: FBO-attach tex=%u min 0x%x -> NEAREST\n", texture, mn);
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
  }
  glFramebufferTexture2D(target, attachment, textarget, texture, level);
  static int fbt_log_count = 0;
  if (fbt_log_count < 80) {
    debugPrintf("GL: glFramebufferTexture2D(att=0x%x tex=%u level=%d)\n",
                attachment, texture, level);
    fbt_log_count++;
  }
}

static GLenum glCheckFramebufferStatus_wrap(GLenum target) {
  egl_shim_ensure_current();
  GLenum status = glCheckFramebufferStatus(target);
  static _Thread_local int fb_log_count = 0;
  if (status != GL_FRAMEBUFFER_COMPLETE || fb_log_count < 12) {
    debugPrintf("GL: glCheckFramebufferStatus(0x%x) = 0x%x\n", target, status);
    fb_log_count++;
  }
  return status;
}

static void glClearColor_wrap(GLfloat red, GLfloat green, GLfloat blue,
                              GLfloat alpha) {
  egl_shim_ensure_current();
  static _Thread_local int clear_color_log_count = 0;
  if (clear_color_log_count < 12) {
    debugPrintf("GL: glClearColor(%.3f, %.3f, %.3f, %.3f)\n",
                red, green, blue, alpha);
    clear_color_log_count++;
  }
  glClearColor(red, green, blue, alpha);
}

static void glClear_wrap(GLbitfield mask) {
  egl_shim_ensure_current();
  /* debugPrintf("GL: glClear(0x%x)\n", mask); */
  glClear(mask);
}

static void gl_log_draw_state(const char *op, GLsizei count);

static void glDrawArrays_wrap(GLenum mode, GLint first, GLsizei count) {
  egl_shim_ensure_current();
  gl_log_draw_state("arrays", count);
  glDrawArrays(mode, first, count);
}

int g_draw_probe = 0; /* set via FIFO p/ logar draws da tela atual */

static void gl_log_draw_state(const char *op, GLsizei count) {
  static _Thread_local int draw_other = 0;
  static _Thread_local int draw_fbo = 0;
  static _Thread_local int draw_psx = 0;
  static _Thread_local int probe_left = 0;
  if (g_draw_probe) {
    g_draw_probe = 0;
    probe_left = 200; /* loga os proximos 200 draws de qualquer tipo */
    /* Readback da VRAM PSX (tex 3, 1024x512): a paleta dos sprites mora aqui.
       Le linhas candidatas e conta pixels nao-pretos — decide se o preto vem
       da TRANSFERENCIA (linha vazia) ou do LOOKUP (linha colorida). */
    {
      GLint prev_fbo = 0;
      glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
      static GLuint probe_fbo = 0;
      if (!probe_fbo)
        glGenFramebuffers(1, &probe_fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, probe_fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, 3, 0);
      GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      if (st == GL_FRAMEBUFFER_COMPLETE) {
        static unsigned char row[1024 * 4];
        /* varre TODAS as linhas; imprime faixas continuas com dados */
        int run_start = -1;
        for (int y = 0; y < 512; y++) {
          glReadPixels(0, y, 1024, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
          int nonblack = 0;
          int first_nb = -1;
          for (int x = 0; x < 1024; x++) {
            const unsigned char *px = row + x * 4;
            if (px[0] | px[1] | px[2]) {
              nonblack++;
              if (first_nb < 0)
                first_nb = x;
            }
          }
          int has = nonblack > 0;
          if (has && run_start < 0) {
            run_start = y;
            debugPrintf("GL: VRAMROW y=%d first_x=%d n=%d "
                        "px=%02x%02x%02x%02x\n",
                        y, first_nb, nonblack, row[first_nb * 4],
                        row[first_nb * 4 + 1], row[first_nb * 4 + 2],
                        row[first_nb * 4 + 3]);
          } else if (!has && run_start >= 0) {
            debugPrintf("GL: VRAMROW faixa %d..%d\n", run_start, y - 1);
            run_start = -1;
          }
        }
        if (run_start >= 0)
          debugPrintf("GL: VRAMROW faixa %d..511\n", run_start);
        /* pixels exatos onde os retratos leem a paleta */
        {
          int prows[] = {481, 484, 489, 490, 448, 500};
          for (unsigned pi = 0; pi < sizeof(prows) / sizeof(prows[0]); pi++) {
            glReadPixels(0, prows[pi], 32, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
            char hex[32 * 9 + 1];
            int off = 0;
            for (int x = 0; x < 16; x++)
              off += sprintf(hex + off, "%02x%02x%02x%02x ", row[x * 4],
                             row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3]);
            debugPrintf("GL: VRAMPIX y=%d x0-15: %s\n", prows[pi], hex);
          }
        }
        GLint mn = 0, mg = 0, prev_tex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
        glBindTexture(GL_TEXTURE_2D, 3);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &mn);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mg);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
        debugPrintf("GL: VRAMTEX3 min=0x%x mag=0x%x\n", mn, mg);
      } else {
        debugPrintf("GL: VRAMREAD fbo status=0x%x (attach tex3 falhou)\n", st);
      }
      glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    }
  }
  /* Fora do probe, nao tocar em estado GL (glGet* por draw asfixia o Utgard e
     pode travar o device). So instrumenta quando explicitamente pedido. */
  if (probe_left <= 0)
    return;
  GLint fbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
  int is_psx = (g_cur_prog < PSX_PROG_MAX && g_psx_prog[g_cur_prog]);
  const char *tag;
  int probing = probe_left > 0;
  if (probing)
    probe_left--;
  if (fbo != 0) {
    if (!probing && draw_fbo >= 800)
      return;
    draw_fbo++;
    tag = "VRAMW";
  } else if (is_psx) {
    if (!probing && draw_psx >= 400)
      return;
    draw_psx++;
    tag = "PSX";
  } else {
    if (!probing && draw_other >= 80)
      return;
    draw_other++;
    tag = "scr";
  }
  GLint prog = 0, active = 0, tex0 = 0, tex1 = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
  glActiveTexture(GL_TEXTURE1);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex1);
  glActiveTexture((GLenum)active);
  float ca[2] = {0, 0};
  if (is_psx && prog > 0 && prog < PSX_PROG_MAX &&
      g_loc_clutaddr[prog] >= 0)
    glGetUniformfv((GLuint)prog, g_loc_clutaddr[prog], ca);
  int t0w = (tex0 > 0 && tex0 < TEX_MAX) ? g_tex_w[tex0] : 0;
  int t0h = (tex0 > 0 && tex0 < TEX_MAX) ? g_tex_h[tex0] : 0;
  int t0a = (tex0 > 0 && tex0 < TEX_MAX) ? g_tex_fmt_alpha[tex0] : 0;
  int t1w = (tex1 > 0 && tex1 < TEX_MAX) ? g_tex_w[tex1] : 0;
  int t1h = (tex1 > 0 && tex1 < TEX_MAX) ? g_tex_h[tex1] : 0;
  const char *t0k = t0a == 1 ? "A" : (t0a == 2 ? "S" : "");
  debugPrintf("GL: DRAW-%s %s prog=%d tex0=%d(%dx%d%s) tex1=%d(%dx%d) fbo=%d "
              "count=%d clutAddr=%.3f,%.3f\n",
              tag, op, prog, tex0, t0w, t0h, t0k, tex1, t1w, t1h,
              fbo, count, ca[0], ca[1]);
}

static void glDrawElements_wrap(GLenum mode, GLsizei count, GLenum type,
                                const void *indices) {
  egl_shim_ensure_current();
  gl_log_draw_state("elems", count);
  glDrawElements(mode, count, type, indices);
}

static void glViewport_wrap(GLint x, GLint y, GLsizei width, GLsizei height) {
  egl_shim_ensure_current();
  static _Thread_local int viewport_log_count = 0;
  if (viewport_log_count < 20) {
    /* debugPrintf("GL: glViewport(%d, %d, %d, %d)\n", x, y, width, height); */
    viewport_log_count++;
  }
  glViewport(x, y, width, height);
}

/* Import table */
DynLibFunction dynlib_functions[] = {
    /* Android stubs */
    {"__sF", (uintptr_t)&fake_sF},
    {"__errno", (uintptr_t)&__errno_fake},
    {"__stack_chk_fail", (uintptr_t)&__stack_chk_fail_stub},
    {"__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake},
    {"__cxa_atexit", (uintptr_t)&__cxa_atexit},
    {"__cxa_finalize", (uintptr_t)&__cxa_finalize},
    {"__android_log_print", (uintptr_t)&__android_log_print_fake},
    {"__android_log_write", (uintptr_t)&__android_log_write_fake},
    {"android_set_abort_message", (uintptr_t)&android_set_abort_message_fake},
    {"__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake},
    {"dl_iterate_phdr", (uintptr_t)&dl_iterate_phdr_fake},
    {"__system_property_get", (uintptr_t)&__system_property_get_fake},

    /* libdl */
    {"dlopen", (uintptr_t)&dlopen_fake},
    {"dlsym", (uintptr_t)&dlsym_fake},
    {"dlclose", (uintptr_t)&dlclose_fake},
    {"dlerror", (uintptr_t)&dlerror_fake},
    {"dladdr", (uintptr_t)&dladdr_fake},

    /* Fortified libc */
    {"__memcpy_chk", (uintptr_t)&__memcpy_chk},
    {"__memmove_chk", (uintptr_t)&__memmove_chk},
    {"__memset_chk", (uintptr_t)&__memset_chk},
    {"__strcat_chk", (uintptr_t)&__strcat_chk},
    {"__strcpy_chk", (uintptr_t)&__strcpy_chk},
    {"__strlen_chk", (uintptr_t)&__strlen_chk},
    {"__strrchr_chk", (uintptr_t)&__strrchr_chk},
    {"__vsprintf_chk", (uintptr_t)&__vsprintf_chk},
    {"__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk},
    {"__read_chk", (uintptr_t)&__read_chk},
    {"__open_2", (uintptr_t)&__open_2},

    /* pthread (wrapped for bionic compat) */
    {"pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake},
    {"pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake},
    {"pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake},
    {"pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake},
    {"pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake},
    {"pthread_mutexattr_init", (uintptr_t)&ret0},
    {"pthread_mutexattr_settype", (uintptr_t)&ret0},
    {"pthread_mutexattr_destroy", (uintptr_t)&ret0},
    {"pthread_cond_init", (uintptr_t)&pthread_cond_init_fake},
    {"pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake},
    {"pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake},
    {"pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake},
    {"pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake},
    {"pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake},
    {"pthread_create", (uintptr_t)&pthread_create_fake},
    {"pthread_join", (uintptr_t)&pthread_join},
    {"pthread_self", (uintptr_t)&pthread_self},
    {"pthread_equal", (uintptr_t)&pthread_equal},
    {"pthread_detach", (uintptr_t)&pthread_detach},
    {"pthread_exit", (uintptr_t)&pthread_exit},
    {"pthread_once", (uintptr_t)&pthread_once_fake},
    {"pthread_attr_init", (uintptr_t)&ret0},
    {"pthread_attr_destroy", (uintptr_t)&ret0},
    {"pthread_attr_setdetachstate", (uintptr_t)&ret0},
    {"pthread_attr_setstacksize", (uintptr_t)&ret0},
    {"pthread_attr_getschedparam", (uintptr_t)&ret0},
    {"pthread_attr_setschedparam", (uintptr_t)&ret0},
    {"pthread_attr_setschedpolicy", (uintptr_t)&ret0},
    {"pthread_getschedparam", (uintptr_t)&ret0},
    {"pthread_setschedparam", (uintptr_t)&ret0},
    {"pthread_key_create", (uintptr_t)&pthread_key_create},
    {"pthread_key_delete", (uintptr_t)&pthread_key_delete},
    {"pthread_getspecific", (uintptr_t)&pthread_getspecific_fake},
    {"pthread_setspecific", (uintptr_t)&pthread_setspecific_fake},
    {"pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake},
    {"pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake},
    {"pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake},
    {"sched_yield", (uintptr_t)&sched_yield},
    {"sched_get_priority_max", (uintptr_t)&sched_get_priority_max},
    {"sched_get_priority_min", (uintptr_t)&sched_get_priority_min},
    {"sem_init", (uintptr_t)&sem_init_fake},
    {"sem_destroy", (uintptr_t)&sem_destroy_fake},
    {"sem_wait", (uintptr_t)&sem_wait_fake},
    {"sem_post", (uintptr_t)&sem_post_fake},

    /* Memory */
    {"malloc", (uintptr_t)&malloc},
    {"calloc", (uintptr_t)&calloc},
    {"realloc", (uintptr_t)&realloc},
    {"free", (uintptr_t)&free},
    {"posix_memalign", (uintptr_t)&posix_memalign},

    /* String/memory */
    {"memcmp", (uintptr_t)&memcmp},
    {"memcpy", (uintptr_t)&memcpy},
    {"memmove", (uintptr_t)&memmove},
    {"memset", (uintptr_t)&memset},
    {"memchr", (uintptr_t)&memchr},
    {"strcmp", (uintptr_t)&strcmp},
    {"strcasecmp", (uintptr_t)&strcasecmp},
    {"strncpy", (uintptr_t)&strncpy},
    {"strcat", (uintptr_t)&strcat},
    {"strchr", (uintptr_t)&strchr},
    {"strrchr", (uintptr_t)&strrchr},
    {"strlen", (uintptr_t)&strlen},
    {"strncmp", (uintptr_t)&strncmp},
    {"strerror_r", (uintptr_t)&strerror_r},
    {"strerror", (uintptr_t)&strerror},
    {"strcoll", (uintptr_t)&strcoll},
    {"strcoll_l", (uintptr_t)&strcoll_l_fake},
    {"strxfrm", (uintptr_t)&strxfrm},
    {"strxfrm_l", (uintptr_t)&strxfrm_l_fake},
    {"strcpy", (uintptr_t)&strcpy},
    {"strlcpy", (uintptr_t)&strlcpy_fake},
    {"strtod", (uintptr_t)&strtod},
    {"strtof", (uintptr_t)&strtof},
    {"strtol", (uintptr_t)&strtol},
    {"strtoul", (uintptr_t)&strtoul},
    {"strtoll", (uintptr_t)&strtoll},
    {"strtoull", (uintptr_t)&strtoull},
    {"strtold", (uintptr_t)&strtold},
    {"strtold_l", (uintptr_t)&strtold},
    {"strtoll_l", (uintptr_t)&strtoll},
    {"strtoull_l", (uintptr_t)&strtoull},
    {"strftime", (uintptr_t)&strftime},
    {"strftime_l", (uintptr_t)&strftime_l_fake},

    /* ctype */
    {"islower", (uintptr_t)&islower},
    {"isupper", (uintptr_t)&isupper},
    {"isxdigit", (uintptr_t)&isxdigit},
    {"tolower", (uintptr_t)&tolower},
    {"toupper", (uintptr_t)&toupper},
    {"isdigit_l", (uintptr_t)&isdigit_l_fake},
    {"islower_l", (uintptr_t)&islower_l_fake},
    {"isupper_l", (uintptr_t)&isupper_l_fake},
    {"isxdigit_l", (uintptr_t)&isxdigit_l_fake},
    {"tolower_l", (uintptr_t)&tolower_l_fake},
    {"toupper_l", (uintptr_t)&toupper_l_fake},

    /* wctype / wchar */
    {"towlower", (uintptr_t)&towlower},
    {"towupper", (uintptr_t)&towupper},
    {"towlower_l", (uintptr_t)&towlower_l_fake},
    {"towupper_l", (uintptr_t)&towupper_l_fake},
    {"iswalpha", (uintptr_t)&iswalpha},
    {"iswblank", (uintptr_t)&iswblank},
    {"iswcntrl", (uintptr_t)&iswcntrl},
    {"iswdigit", (uintptr_t)&iswdigit},
    {"iswlower", (uintptr_t)&iswlower},
    {"iswprint", (uintptr_t)&iswprint},
    {"iswpunct", (uintptr_t)&iswpunct},
    {"iswspace", (uintptr_t)&iswspace},
    {"iswupper", (uintptr_t)&iswupper},
    {"iswxdigit", (uintptr_t)&iswxdigit},
    {"iswalpha_l", (uintptr_t)&iswalpha_l_fake},
    {"iswblank_l", (uintptr_t)&iswblank_l_fake},
    {"iswcntrl_l", (uintptr_t)&iswcntrl_l_fake},
    {"iswdigit_l", (uintptr_t)&iswdigit_l_fake},
    {"iswlower_l", (uintptr_t)&iswlower_l_fake},
    {"iswprint_l", (uintptr_t)&iswprint_l_fake},
    {"iswpunct_l", (uintptr_t)&iswpunct_l_fake},
    {"iswspace_l", (uintptr_t)&iswspace_l_fake},
    {"iswupper_l", (uintptr_t)&iswupper_l_fake},
    {"iswxdigit_l", (uintptr_t)&iswxdigit_l_fake},
    {"wctob", (uintptr_t)&wctob},
    {"btowc", (uintptr_t)&btowc},
    {"wcstol", (uintptr_t)&wcstol},
    {"wcstoul", (uintptr_t)&wcstoul},
    {"wcstoll", (uintptr_t)&wcstoll},
    {"wcstoull", (uintptr_t)&wcstoull},
    {"wcstod", (uintptr_t)&wcstod},
    {"wcstof", (uintptr_t)&wcstof},
    {"wcstold", (uintptr_t)&wcstold},
    {"wcslen", (uintptr_t)&wcslen},
    {"wcscoll", (uintptr_t)&wcscoll},
    {"wcscoll_l", (uintptr_t)&wcscoll_l_fake},
    {"wcsxfrm", (uintptr_t)&wcsxfrm},
    {"wcsxfrm_l", (uintptr_t)&wcsxfrm_l_fake},
    {"wmemcmp", (uintptr_t)&wmemcmp},
    {"wmemcpy", (uintptr_t)&wmemcpy},
    {"wmemmove", (uintptr_t)&wmemmove},
    {"wmemset", (uintptr_t)&wmemset},
    {"wmemchr", (uintptr_t)&wmemchr},
    {"mbrtowc", (uintptr_t)&mbrtowc},
    {"wcrtomb", (uintptr_t)&wcrtomb},
    {"wctomb", (uintptr_t)&wctomb},
    {"mbrlen", (uintptr_t)&mbrlen},
    {"mbtowc", (uintptr_t)&mbtowc},
    {"mbsrtowcs", (uintptr_t)&mbsrtowcs},
    {"mbsnrtowcs", (uintptr_t)&mbsnrtowcs},
    {"wcsnrtombs", (uintptr_t)&wcsnrtombs},
    {"swprintf", (uintptr_t)&swprintf},
    {"vswprintf", (uintptr_t)&vswprintf},

    /* stdio */
    {"printf", (uintptr_t)&printf},
    {"fprintf", (uintptr_t)&fprintf_fake},
    {"sprintf", (uintptr_t)&sprintf},
    {"snprintf", (uintptr_t)&snprintf},
    {"vfprintf", (uintptr_t)&vfprintf_fake},
    {"vsprintf", (uintptr_t)&vsprintf},
    {"vsnprintf", (uintptr_t)&vsnprintf},
    {"vasprintf", (uintptr_t)&vasprintf},
    {"sscanf", (uintptr_t)&sscanf},
    {"vsscanf", (uintptr_t)&vsscanf},
    {"fopen", (uintptr_t)&fopen_fake},
    {"fclose", (uintptr_t)&fclose_fake},
    {"fflush", (uintptr_t)&fflush_fake},
    {"ferror", (uintptr_t)&ferror_fake},
    {"fread", (uintptr_t)&fread},
    {"fwrite", (uintptr_t)&fwrite_fake},
    {"fputc", (uintptr_t)&fputc_fake},
    {"fseek", (uintptr_t)&fseek},
    {"fseeko", (uintptr_t)&fseeko},
    {"ftell", (uintptr_t)&ftell},
    {"ftello", (uintptr_t)&ftello},
    {"getc", (uintptr_t)&getc},
    {"putc", (uintptr_t)&putc},
    {"putchar", (uintptr_t)&putchar},
    {"puts", (uintptr_t)&puts},
    {"ungetc", (uintptr_t)&ungetc},

    /* POSIX I/O */
    {"open", (uintptr_t)&open_fake},
    {"close", (uintptr_t)&close},
    {"read", (uintptr_t)&read},
    {"write", (uintptr_t)&write},
    {"lseek", (uintptr_t)&lseek},
    {"fallocate", (uintptr_t)&fallocate},
    {"pipe", (uintptr_t)&pipe},
    {"mkdir", (uintptr_t)&mkdir_fake},
    {"rmdir", (uintptr_t)&rmdir},
    {"chdir", (uintptr_t)&chdir},
    {"remove", (uintptr_t)&remove_fake},
    {"rename", (uintptr_t)&rename_fake},
    {"unlink", (uintptr_t)&unlink},
    {"stat", (uintptr_t)&stat},
    {"statfs64", (uintptr_t)&statfs64_fake},
    {"opendir", (uintptr_t)&opendir},
    {"readdir", (uintptr_t)&readdir},
    {"closedir", (uintptr_t)&closedir},

    /* stdlib */
    {"abort", (uintptr_t)&abort_fake},
    {"exit", (uintptr_t)&exit_fake},
    {"getenv", (uintptr_t)&getenv_fake},
    {"setenv", (uintptr_t)&setenv_fake},
    {"atoi", (uintptr_t)&atoi},
    {"atof", (uintptr_t)&atof},
    {"atoll", (uintptr_t)&atoll},
    {"qsort", (uintptr_t)&qsort},
    {"rand", (uintptr_t)&rand},
    {"srand", (uintptr_t)&srand},
    {"lrand48", (uintptr_t)&lrand48},
    {"srand48", (uintptr_t)&srand48},
    {"div", (uintptr_t)&div},
    {"setjmp", (uintptr_t)&setjmp},
    {"longjmp", (uintptr_t)&longjmp},

    /* math */
    {"acos", (uintptr_t)&acos},
    {"acosf", (uintptr_t)&acosf},
    {"atan", (uintptr_t)&atan},
    {"asinf", (uintptr_t)&asinf},
    {"atanf", (uintptr_t)&atanf},
    {"atan2", (uintptr_t)&atan2},
    {"atan2f", (uintptr_t)&atan2f},
    {"cos", (uintptr_t)&cos},
    {"cosf", (uintptr_t)&cosf},
    {"sinf", (uintptr_t)&sinf},
    {"sin", (uintptr_t)&sin},
    {"sincos", (uintptr_t)&sincos},
    {"sincosf", (uintptr_t)&sincosf},
    {"exp", (uintptr_t)&exp},
    {"exp2", (uintptr_t)&exp2},
    {"exp2f", (uintptr_t)&exp2f},
    {"expf", (uintptr_t)&expf},
    {"frexp", (uintptr_t)&frexp},
    {"log", (uintptr_t)&log},
    {"log10", (uintptr_t)&log10},
    {"log10f", (uintptr_t)&log10f},
    {"logf", (uintptr_t)&logf},
    {"modf", (uintptr_t)&modf},
    {"pow", (uintptr_t)&pow},
    {"powf", (uintptr_t)&powf},
    {"sqrtf", (uintptr_t)&sqrtf},
    {"ldexp", (uintptr_t)&ldexp},
    {"ldexpf", (uintptr_t)&ldexpf},

    /* time */
    {"time", (uintptr_t)&time},
    {"gmtime", (uintptr_t)&gmtime},
    {"localtime", (uintptr_t)&localtime},
    {"mktime", (uintptr_t)&mktime},
    {"gettimeofday", (uintptr_t)&gettimeofday},
    {"clock_gettime", (uintptr_t)&clock_gettime},
    {"nanosleep", (uintptr_t)&nanosleep},
    {"sleep", (uintptr_t)&sleep},
    {"usleep", (uintptr_t)&usleep},

    /* locale */
    {"setlocale", (uintptr_t)&setlocale},
    {"localeconv", (uintptr_t)&localeconv},
    {"newlocale", (uintptr_t)&newlocale},
    {"uselocale", (uintptr_t)&uselocale},
    {"freelocale", (uintptr_t)&freelocale},

    /* syslog */
    {"openlog", (uintptr_t)&openlog},
    {"closelog", (uintptr_t)&closelog},
    {"syslog", (uintptr_t)&syslog},

    /* signals */
    {"raise", (uintptr_t)&ret0},
    {"sigaction", (uintptr_t)&sigaction_fake},

    /* syscall */
    {"getpid", (uintptr_t)&getpid},
    {"gettid", (uintptr_t)&gettid_fake},
    {"syscall", (uintptr_t)&syscall_fake},
    {"sysconf", (uintptr_t)&sysconf},

    /* EGL (our shim) */
    {"eglGetDisplay", (uintptr_t)&egl_shim_GetDisplay},
    {"eglInitialize", (uintptr_t)&egl_shim_Initialize},
    {"eglGetConfigs", (uintptr_t)&egl_shim_GetConfigs},
    {"eglChooseConfig", (uintptr_t)&egl_shim_ChooseConfig},
    {"eglCreateWindowSurface", (uintptr_t)&egl_shim_CreateWindowSurface},
    {"eglCreatePbufferSurface", (uintptr_t)&egl_shim_CreatePbufferSurface},
    {"eglCreateContext", (uintptr_t)&egl_shim_CreateContext},
    {"eglMakeCurrent", (uintptr_t)&egl_shim_MakeCurrent},
    {"eglSwapBuffers", (uintptr_t)&egl_shim_SwapBuffers},
    {"eglDestroySurface", (uintptr_t)&egl_shim_DestroySurface},
    {"eglDestroyContext", (uintptr_t)&egl_shim_DestroyContext},
    {"eglTerminate", (uintptr_t)&egl_shim_Terminate},
    {"eglQuerySurface", (uintptr_t)&egl_shim_QuerySurface},
    {"eglGetConfigAttrib", (uintptr_t)&egl_shim_GetConfigAttrib},
    {"eglGetError", (uintptr_t)&egl_shim_GetError},
    {"eglGetProcAddress", (uintptr_t)&egl_shim_GetProcAddress},
    {"eglBindAPI", (uintptr_t)&egl_shim_BindAPI},

    /* OpenGL ES 2.0 (direct passthrough) */
    {"glActiveTexture", (uintptr_t)&glActiveTexture},
    {"glAttachShader", (uintptr_t)&glAttachShader_wrap},
    {"glBindAttribLocation", (uintptr_t)&glBindAttribLocation},
    {"glBindBuffer", (uintptr_t)&glBindBuffer_wrap},
    {"glBindFramebuffer", (uintptr_t)&glBindFramebuffer_wrap},
    {"glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer},
    {"glBindTexture", (uintptr_t)&glBindTexture_wrap},
    {"glBlendEquation", (uintptr_t)&glBlendEquation},
    {"glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate},
    {"glBlendFunc", (uintptr_t)&glBlendFunc},
    {"glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate},
    {"glBufferData", (uintptr_t)&glBufferData_wrap},
    {"glBufferSubData", (uintptr_t)&glBufferSubData_wrap},
    {"glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus_wrap},
    {"glClear", (uintptr_t)&glClear_wrap},
    {"glClearColor", (uintptr_t)&glClearColor_wrap},
    {"glClearStencil", (uintptr_t)&glClearStencil},
    {"glColorMask", (uintptr_t)&glColorMask},
    {"glCompileShader", (uintptr_t)&glCompileShader_wrap},
    {"glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D_wrap},
    {"glCopyTexImage2D", (uintptr_t)&glCopyTexImage2D_wrap},
    {"glCopyTexSubImage2D", (uintptr_t)&glCopyTexSubImage2D_wrap},
    {"glCreateProgram", (uintptr_t)&glCreateProgram_wrap},
    {"glCreateShader", (uintptr_t)&glCreateShader_wrap},
    {"glCullFace", (uintptr_t)&glCullFace},
    {"glDeleteBuffers", (uintptr_t)&glDeleteBuffers},
    {"glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers},
    {"glDeleteProgram", (uintptr_t)&glDeleteProgram},
    {"glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers},
    {"glDeleteShader", (uintptr_t)&glDeleteShader},
    {"glDeleteTextures", (uintptr_t)&glDeleteTextures},
    {"glDepthFunc", (uintptr_t)&glDepthFunc},
    {"glDepthMask", (uintptr_t)&glDepthMask},
    {"glDisable", (uintptr_t)&glDisable},
    {"glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray},
    {"glDrawArrays", (uintptr_t)&glDrawArrays_wrap},
    {"glDrawElements", (uintptr_t)&glDrawElements_wrap},
    {"glEnable", (uintptr_t)&glEnable},
    {"glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray},
    {"glFinish", (uintptr_t)&glFinish},
    {"glFlush", (uintptr_t)&glFlush},
    {"glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D_wrap},
    {"glFrontFace", (uintptr_t)&glFrontFace_wrap},
    {"glGenBuffers", (uintptr_t)&glGenBuffers_wrap},
    {"glGenFramebuffers", (uintptr_t)&glGenFramebuffers_wrap},
    {"glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers},
    {"glGenTextures", (uintptr_t)&glGenTextures_wrap},
    {"glGenerateMipmap", (uintptr_t)&glGenerateMipmap},
    {"glGetActiveAttrib", (uintptr_t)&glGetActiveAttrib},
    {"glGetActiveUniform", (uintptr_t)&glGetActiveUniform},
    {"glGetAttachedShaders", (uintptr_t)&glGetAttachedShaders},
    {"glGetAttribLocation", (uintptr_t)&glGetAttribLocation},
    {"glGetError", (uintptr_t)&glGetError},
    {"glGetIntegerv", (uintptr_t)&glGetIntegerv_wrap},
    {"glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog},
    {"glGetProgramiv", (uintptr_t)&glGetProgramiv},
    {"glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog},
    {"glGetShaderSource", (uintptr_t)&glGetShaderSource},
    {"glGetShaderiv", (uintptr_t)&glGetShaderiv},
    {"glGetString", (uintptr_t)&glGetString_wrap},
    {"glGetUniformLocation", (uintptr_t)&glGetUniformLocation},
    {"glGetVertexAttribPointerv", (uintptr_t)&glGetVertexAttribPointerv},
    {"glGetVertexAttribiv", (uintptr_t)&glGetVertexAttribiv},
    {"glLinkProgram", (uintptr_t)&glLinkProgram_wrap},
    {"glLineWidth", (uintptr_t)&glLineWidth},
    {"glPixelStorei", (uintptr_t)&glPixelStorei_wrap},
    {"glReadPixels", (uintptr_t)&glReadPixels},
    {"glReleaseShaderCompiler", (uintptr_t)&glReleaseShaderCompiler},
    {"glScissor", (uintptr_t)&glScissor},
    {"glShaderSource", (uintptr_t)&glShaderSource_wrap},
    {"glStencilFunc", (uintptr_t)&glStencilFunc},
    {"glStencilMask", (uintptr_t)&glStencilMask},
    {"glStencilOp", (uintptr_t)&glStencilOp},
    {"glTexImage2D", (uintptr_t)&glTexImage2D_wrap},
    {"glTexParameteri", (uintptr_t)&glTexParameteri_wrap},
    {"glTexSubImage2D", (uintptr_t)&glTexSubImage2D_wrap},
    {"glUniform1f", (uintptr_t)&glUniform1f_wrap},
    {"glUniform1fv", (uintptr_t)&glUniform1fv_wrap},
    {"glUniform1i", (uintptr_t)&glUniform1i_wrap},
    {"glUniform1iv", (uintptr_t)&glUniform1iv_wrap},
    {"glUniform2f", (uintptr_t)&glUniform2f_wrap},
    {"glUniform2fv", (uintptr_t)&glUniform2fv_wrap},
    {"glUniform2iv", (uintptr_t)&glUniform2iv_wrap},
    {"glUniform3f", (uintptr_t)&glUniform3f_wrap},
    {"glUniform3fv", (uintptr_t)&glUniform3fv_wrap},
    {"glUniform3iv", (uintptr_t)&glUniform3iv_wrap},
    {"glUniform4f", (uintptr_t)&glUniform4f_wrap},
    {"glUniform4fv", (uintptr_t)&glUniform4fv_wrap},
    {"glUniform4iv", (uintptr_t)&glUniform4iv_wrap},
    {"glUniformMatrix2fv", (uintptr_t)&glUniformMatrix2fv_wrap},
    {"glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv_wrap},
    {"glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv_wrap},
    {"glUseProgram", (uintptr_t)&glUseProgram_wrap},
    {"glValidateProgram", (uintptr_t)&ret0},
    {"glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer},
    {"glViewport", (uintptr_t)&glViewport_wrap},

    /* OpenSL ES (our shim) */
    {"slCreateEngine", (uintptr_t)&slCreateEngine_shim},
    {"SL_IID_ENGINE", (uintptr_t)&sl_IID_ENGINE},
    {"SL_IID_PLAY", (uintptr_t)&sl_IID_PLAY},
    {"SL_IID_VOLUME", (uintptr_t)&sl_IID_VOLUME},
    {"SL_IID_BUFFERQUEUE", (uintptr_t)&sl_IID_BUFFERQUEUE},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&sl_IID_BUFFERQUEUE},
    {"SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&sl_IID_ANDROIDCONFIGURATION},
    {"SL_IID_ENGINECAPABILITIES", (uintptr_t)&sl_IID_ENGINECAPABILITIES},
    {"SL_IID_ENVIRONMENTALREVERB", (uintptr_t)&sl_IID_ENVIRONMENTALREVERB},
    {"SL_IID_EFFECTSEND", (uintptr_t)&sl_IID_EFFECTSEND},
    {"SL_IID_PLAYBACKRATE", (uintptr_t)&sl_IID_PLAYBACKRATE},
    {"SL_IID_SEEK", (uintptr_t)&sl_IID_SEEK},

    /* Android NDK (our shim) */
    {"AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake},
    {"AAssetManager_open", (uintptr_t)&AAssetManager_open_fake},
    {"AAsset_close", (uintptr_t)&AAsset_close_fake},
    {"AAsset_read", (uintptr_t)&AAsset_read_fake},
    {"AAsset_getLength", (uintptr_t)&AAsset_getLength_fake},
    {"AAsset_getRemainingLength", (uintptr_t)&AAsset_getRemainingLength_fake},
    {"AAsset_seek", (uintptr_t)&AAsset_seek_fake},
    {"AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor_fake},
    {"ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface_fake},
    {"ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth_fake},
    {"ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight_fake},
    {"ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry_fake},
    {"ANativeActivity_finish", (uintptr_t)&ANativeActivity_finish_fake},

    {"AConfiguration_new", (uintptr_t)&AConfiguration_new_fake},
    {"AConfiguration_delete", (uintptr_t)&AConfiguration_delete_fake},
    {"AConfiguration_fromAssetManager", (uintptr_t)&AConfiguration_fromAssetManager_fake},
    {"AConfiguration_getLanguage", (uintptr_t)&AConfiguration_getLanguage_fake},
    {"AConfiguration_getCountry", (uintptr_t)&AConfiguration_getCountry_fake},
    {"AConfiguration_getOrientation", (uintptr_t)&AConfiguration_getOrientation_fake},

    {"ALooper_prepare", (uintptr_t)&ALooper_prepare_fake},
    {"ALooper_addFd", (uintptr_t)&ALooper_addFd_fake},
    {"ALooper_pollAll", (uintptr_t)&ALooper_pollAll_fake},
    {"AInputQueue_attachLooper", (uintptr_t)&AInputQueue_attachLooper_fake},
    {"AInputQueue_detachLooper", (uintptr_t)&AInputQueue_detachLooper_fake},
    {"AInputQueue_getEvent", (uintptr_t)&AInputQueue_getEvent_fake},
    {"AInputQueue_preDispatchEvent", (uintptr_t)&AInputQueue_preDispatchEvent_fake},
    {"AInputQueue_finishEvent", (uintptr_t)&AInputQueue_finishEvent_fake},
    {"AInputEvent_getType", (uintptr_t)&AInputEvent_getType_fake},
    {"AInputEvent_getDeviceId", (uintptr_t)&AInputEvent_getDeviceId_fake},
    {"AInputEvent_getSource", (uintptr_t)&AInputEvent_getSource_fake},
    {"AKeyEvent_getAction", (uintptr_t)&AKeyEvent_getAction_fake},
    {"AKeyEvent_getKeyCode", (uintptr_t)&AKeyEvent_getKeyCode_fake},
    {"AKeyEvent_getMetaState", (uintptr_t)&AKeyEvent_getMetaState_fake},
    {"AKeyEvent_getRepeatCount", (uintptr_t)&AKeyEvent_getRepeatCount_fake},
    {"AMotionEvent_getAction", (uintptr_t)&AMotionEvent_getAction_fake},
    {"AMotionEvent_getFlags", (uintptr_t)&AMotionEvent_getFlags_fake},
    {"AMotionEvent_getPointerCount", (uintptr_t)&AMotionEvent_getPointerCount_fake},
    {"AMotionEvent_getPointerId", (uintptr_t)&AMotionEvent_getPointerId_fake},
    {"AMotionEvent_getX", (uintptr_t)&AMotionEvent_getX_fake},
    {"AMotionEvent_getY", (uintptr_t)&AMotionEvent_getY_fake},
    {"AMotionEvent_getRawX", (uintptr_t)&AMotionEvent_getRawX_fake},
    {"AMotionEvent_getRawY", (uintptr_t)&AMotionEvent_getRawY_fake},

    {"ASensorManager_getInstance", (uintptr_t)&ASensorManager_getInstance_fake},
    {"ASensorManager_getDefaultSensor", (uintptr_t)&ASensorManager_getDefaultSensor_fake},
    {"ASensorManager_createEventQueue", (uintptr_t)&ASensorManager_createEventQueue_fake},
    {"ASensorEventQueue_enableSensor", (uintptr_t)&ASensorEventQueue_enableSensor_fake},
    {"ASensorEventQueue_disableSensor", (uintptr_t)&ASensorEventQueue_disableSensor_fake},
    {"ASensorEventQueue_setEventRate", (uintptr_t)&ASensorEventQueue_setEventRate_fake},
    {"ASensorEventQueue_getEvents", (uintptr_t)&ASensorEventQueue_getEvents_fake},
};

size_t dynlib_numfunctions =
    sizeof(dynlib_functions) / sizeof(*dynlib_functions);
