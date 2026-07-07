/* libc_shim.c -- bionic-compatible libc wrappers (Linux/glibc port)
 *
 * Where the bionic and glibc ABIs differ (struct layouts, flag values, missing
 * functions) we provide converting wrappers here; everything that matches is
 * passed straight through from imports.c. Android-absolute paths are collapsed
 * onto the game directory by fix_path().
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <time.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "libc_shim.h"

// ---------------------------------------------------------------------------
// misc bionic functions
// ---------------------------------------------------------------------------

int __system_property_get_fake(const char *name, char *value) {
  (void)name; value[0] = '\0'; return 0;
}
unsigned long getauxval_fake(unsigned long type) { (void)type; return 0; }

int gettid_fake(void) { return (int)syscall(SYS_gettid); }

#define ARM64_SYS_GETTID 178
long syscall_fake(long number, ...) {
  switch (number) {
    case ARM64_SYS_GETTID: return gettid_fake();
  }
  debugPrintf("libc: syscall(%ld) -> ENOSYS\n", number);
  errno = ENOSYS;
  return -1;
}

void sincosf_fake(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }

int sched_get_priority_max_fake(int policy) { (void)policy; return 0; }
int sched_get_priority_min_fake(int policy) { (void)policy; return 0; }
int pthread_setname_np_fake(void *thread, const char *name) { (void)thread; (void)name; return 0; }
int pthread_setschedparam_fake(void *thread, int policy, const void *param) { (void)thread; (void)policy; (void)param; return 0; }
void android_set_abort_message_fake(const char *msg) { debugPrintf("abort message: %s\n", msg ? msg : "(null)"); }
size_t __ctype_get_mb_cur_max_fake(void) { return 1; }
int __register_atfork_fake(void) { return 0; }
int __cxa_thread_atexit_impl_fake(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }

#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98
long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE: return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN: return 4;
    case BIONIC_SC_PHYS_PAGES: return (1ll * 1024 * 1024 * 1024) / 0x1000;
    default: debugPrintf("libc: sysconf(%d) -> -1\n", name); return -1;
  }
}

// ---------------------------------------------------------------------------
// path remapping: collapse Android-absolute prefixes onto the game dir (cwd)
// ---------------------------------------------------------------------------

const char *fix_path(const char *path) {
  static _Thread_local char buf[2][1024];
  static _Thread_local int which = 0;

  if (!path || path[0] != '/')
    return path;

  static const char *const prefixes[] = { WRITE_PATH, CACHE_PATH, SAVE_PATH };
  for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
    const char *rest = strstr(path, prefixes[i]);
    if (rest) {
      rest += strlen(prefixes[i]);
      if (*rest == '/') rest++;
      char *out = buf[which]; which ^= 1;
      if (*rest) snprintf(out, sizeof(buf[0]), "./%s", rest);
      else       snprintf(out, sizeof(buf[0]), ".");
      return out;
    }
  }
  {
    const char *r = strstr(path, "/" PACKAGE "/");
    if (r) {
      r += strlen("/" PACKAGE "/");
      char *out = buf[which]; which ^= 1;
      snprintf(out, sizeof(buf[0]), "./%s", r);
      return out;
    }
  }
  {
    const char *r = strstr(path, "/TTGames/");
    if (r) {
      char *out = buf[which]; which ^= 1;
      snprintf(out, sizeof(buf[0]), ".%s", r);
      return out;
    }
  }
  return path;
}

// ---------------------------------------------------------------------------
// open() flag translation (bionic/linux -> glibc; identical on aarch64 Linux)
// ---------------------------------------------------------------------------

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000

static int convert_open_flags(int flags) {
  int out = flags & 3;
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  return out;
}

int open_fake(const char *path, int flags, ...) {
  int mode = 0666;
  if (flags & LINUX_O_CREAT) {
    va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va);
  }
  return open(fix_path(path), convert_open_flags(flags), mode);
}
int open2_fake(const char *path, int flags) {
  return open(fix_path(path), convert_open_flags(flags), 0666);
}
int mkdir_fake(const char *path, unsigned int mode) { return mkdir(fix_path(path), mode); }
int remove_fake(const char *path) { return remove(fix_path(path)); }
int rename_fake(const char *from, const char *to) {
  const char *f = fix_path(from);
  const char *t = fix_path(to);
  remove(t);
  return rename(f, t);
}

// ---------------------------------------------------------------------------
// struct stat conversion (bionic aarch64 layout)
// ---------------------------------------------------------------------------

struct bionic_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct bionic_stat {
  uint64_t st_dev; uint64_t st_ino; uint32_t st_mode; uint32_t st_nlink;
  uint32_t st_uid; uint32_t st_gid; uint64_t st_rdev; uint64_t __pad1;
  int64_t st_size; int32_t st_blksize; int32_t __pad2; int64_t st_blocks;
  struct bionic_timespec st_atim, st_mtim, st_ctim;
  uint32_t __unused4; uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev; out->st_ino = in->st_ino; out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink; out->st_uid = in->st_uid; out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev; out->st_size = in->st_size;
  out->st_blksize = in->st_blksize; out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime; out->st_mtim.tv_sec = in->st_mtime;
  out->st_ctim.tv_sec = in->st_ctime;
}

int stat_fake(const char *path, struct bionic_stat *st) {
  struct stat real;
  const int ret = stat(fix_path(path), &real);
  if (ret == 0) convert_stat(&real, st);
  return ret;
}
int fstat_fake(int fd, struct bionic_stat *st) {
  struct stat real;
  const int ret = fstat(fd, &real);
  if (ret == 0) convert_stat(&real, st);
  return ret;
}
int lstat_fake(const char *path, struct bionic_stat *st) { return stat_fake(path, st); }

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p) return ENOMEM;
  *out = p; return 0;
}

char *realpath_fake(const char *path, char *resolved) {
  if (!resolved) resolved = malloc(0x1000);
  strcpy(resolved, path);
  return resolved;
}
int strerror_r_fake(int err, char *buf, size_t len) {
  snprintf(buf, len, "%s", strerror(err));
  return 0;
}

// ---------------------------------------------------------------------------
// stdio over the fake bionic __sF (stdin/stdout/stderr)
// ---------------------------------------------------------------------------

uint8_t fake_sF[3][0x100];

static int is_fake_file(const void *f) {
  const uint8_t *p = f;
  const uint8_t *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) return n;
  return fwrite(ptr, size, n, f);
}
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) return 0;
  return fread(ptr, size, n, f);
}
int fputc_fake(int c, FILE *f) { if (is_fake_file(f)) return c; return fputc(c, f); }
int fputs_fake(const char *s, FILE *f) {
  if (is_fake_file(f)) { debugPrintf("stdio: %s", s); return 0; }
  return fputs(s, f);
}
int fflush_fake(FILE *f) { if (is_fake_file(f) || f == NULL) return 0; return fflush(f); }
int fclose_fake(FILE *f) { if (is_fake_file(f)) return 0; return fclose(f); }
int ferror_fake(FILE *f) { if (is_fake_file(f)) return 0; return ferror(f); }
int fileno_fake(FILE *f) {
  if (is_fake_file(f)) return ((const uint8_t *)f - &fake_sF[0][0]) / 0x100;
  return fileno(f);
}
int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) {
    char buf[0x400];
    int ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
    return ret;
  }
  return vfprintf(f, fmt, va);
}
int fseek_fake(FILE *f, long off, int whence) { if (is_fake_file(f)) return -1; return fseek(f, off, whence); }
int ungetc_fake(int c, FILE *f) { if (is_fake_file(f)) return -1; return ungetc(c, f); }

// ---------------------------------------------------------------------------
// AAsset emulation + game-archive fopen
// ---------------------------------------------------------------------------

typedef struct { FILE *f; long size; } Asset;

void *AAssetManager_fromJava_fake(void *env, void *mgr) { (void)env; (void)mgr; return (void *)1; }

static void mkdir_p(const char *filepath) {
  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s", filepath);
  char *slash = strrchr(tmp, '/');
  if (!slash) return;
  *slash = '\0';
  for (char *q = tmp + 1; *q; q++) {
    if (*q == '/') { *q = '\0'; mkdir(tmp, 0777); *q = '/'; }
  }
  mkdir(tmp, 0777);
}

FILE *fopen_fake(const char *path, const char *mode) {
  const char *p = fix_path(path);
  FILE *f = fopen(p, mode);
  if (!f && (strchr(mode, 'w') || strchr(mode, 'a'))) {
    mkdir_p(p);
    f = fopen(p, mode);
  }
  if (f && strchr(mode, 'r')) {
    const char *ext = strrchr(p, '.');
    if (ext && (strcasecmp(ext, ".fib") == 0 || strcasecmp(ext, ".dat") == 0))
      setvbuf(f, NULL, _IOFBF, 256 * 1024);
  }
  // Make save I/O observable: log every open of savegame.dat/config.dat with the
  // resulting size so a Continue/load can be confirmed from the log.
  if (f && strstr(p, "savegame.dat")) {
    long cur = ftell(f); fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, cur, SEEK_SET);
    debugPrintf("SAVE: fopen savegame.dat mode=%s -> OK size=%ld\n", mode, sz);
  } else if (f && strstr(p, "config.dat")) {
    debugPrintf("SAVE: fopen config.dat mode=%s -> OK\n", mode);
  }
  if (!f)
    debugPrintf("fopen(%s => %s, %s) FAILED\n", path, p, mode);
  return f;
}

void *AAssetManager_open_fake(void *mgr, const char *path, int mode) {
  (void)mgr; (void)mode;
  char full[1024];
  snprintf(full, sizeof(full), "%s/%s", GAMEDATA_DIR, path);
  FILE *f = fopen(full, "rb");
  if (!f) { snprintf(full, sizeof(full), "assets/%s", path); f = fopen(full, "rb"); }
  debugPrintf("AAsset: open(%s) -> %s\n", path, f ? "ok" : "MISSING");
  if (!f) return NULL;
  setvbuf(f, NULL, _IOFBF, 16 * 1024);
  Asset *a = calloc(1, sizeof(*a));
  a->f = f;
  fseek(f, 0, SEEK_END); a->size = ftell(f); fseek(f, 0, SEEK_SET);
  return a;
}

int AAsset_openFileDescriptor_fake(void *asset, long *outStart, long *outLength) {
  Asset *a = asset;
  if (!a) return -1;
  fflush(a->f);
  int fd = dup(fileno(a->f));
  if (fd < 0) return -1;
  if (outStart)  *outStart = 0;
  if (outLength) *outLength = a->size;
  return fd;
}
void AAsset_close_fake(void *asset) { Asset *a = asset; if (a) { fclose(a->f); free(a); } }
int AAsset_read_fake(void *asset, void *buf, size_t count) {
  Asset *a = asset; return a ? (int)fread(buf, 1, count, a->f) : -1;
}
long AAsset_seek_fake(void *asset, long off, int whence) {
  Asset *a = asset;
  if (!a || fseek(a->f, off, whence) < 0) return -1;
  return ftell(a->f);
}
long AAsset_getLength_fake(void *asset) { Asset *a = asset; return a ? a->size : 0; }
long AAsset_getRemainingLength_fake(void *asset) { Asset *a = asset; return a ? a->size - ftell(a->f) : 0; }

// ---------------------------------------------------------------------------
// ANativeWindow: the wrapper owns the real window (SDL); hand back dummy tokens
// ---------------------------------------------------------------------------

void *ANativeWindow_fromSurface_fake(void *env, void *surface) { (void)env; (void)surface; return (void *)0x414e5731; }
int ANativeWindow_getWidth_fake(void *win) { (void)win; return screen_width; }
int ANativeWindow_getHeight_fake(void *win) { (void)win; return screen_height; }
void ANativeWindow_release_fake(void *win) { (void)win; }
int ANativeWindow_setBuffersGeometry_fake(void *win, int w, int h, int format) {
  (void)win; (void)format;
  debugPrintf("ANativeWindow_setBuffersGeometry(%d, %d)\n", w, h);
  return 0;
}

// ---------------------------------------------------------------------------
// semaphores via pointer indirection (POSIX sem_t behind the bionic storage)
// ---------------------------------------------------------------------------

int sem_init_fake(void **s, int pshared, unsigned int value) {
  sem_t *real = malloc(sizeof(sem_t));
  sem_init(real, pshared, value);
  *s = real;
  return 0;
}
int sem_destroy_fake(void **s) {
  if (s && *s) { sem_destroy((sem_t *)*s); free(*s); *s = NULL; }
  return 0;
}
int sem_post_fake(void **s) { if (s && *s) sem_post((sem_t *)*s); return 0; }
int sem_wait_fake(void **s) {
  // never block while holding the single GL context
  extern void egl_gl_ownership_park(void);
  egl_gl_ownership_park();
  if (s && *s) sem_wait((sem_t *)*s);
  return 0;
}
int sem_trywait_fake(void **s) {
  if (s && *s && sem_trywait((sem_t *)*s) == 0) return 0;
  errno = EAGAIN; return -1;
}
