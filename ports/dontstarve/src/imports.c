/*
 * imports.c -- Android/bionic imports for Don't Starve Pocket Edition.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <GLES2/gl2.h>

#include "android_shim.h"
#include "egl_shim.h"
#include "opensles_shim.h"
#include "so_util.h"

extern int dys_screen_w, dys_screen_h;
extern unsigned char *etc2_decode_rgba(unsigned fmt, int w, int h,
                                       const void *data, int size);
extern void etc1_encode_image(const unsigned char *rgba, int w, int h,
                              int channels, uint8_t *out);

#ifndef GL_ETC1_RGB8_OES
#define GL_ETC1_RGB8_OES 0x8D64
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#endif
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

/* ---------------- logging / bionic basics ---------------- */
static int b_log_print(int prio, const char *tag, const char *fmt, ...) {
  fprintf(stderr, "[ALOG:%d %s] ", prio, tag ? tag : "?");
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt ? fmt : "", ap);
  va_end(ap);
  fputc('\n', stderr);
  return 0;
}

static int b_log_write(int prio, const char *tag, const char *text) {
  fprintf(stderr, "[ALOG:%d %s] %s\n", prio, tag ? tag : "?",
          text ? text : "");
  return 0;
}

static void b_log_assert(const char *cond, const char *tag, const char *fmt,
                         ...) {
  fprintf(stderr, "[ALOG-ASSERT %s] %s ", tag ? tag : "?",
          cond ? cond : "");
  if (fmt) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
  }
  fputc('\n', stderr);
}

static int *b_errno(void) { return __errno_location(); }

static void b_assert2(const char *file, int line, const char *func,
                      const char *expr) {
  fprintf(stderr, "__assert2: %s:%d %s: %s\n", file ? file : "?", line,
          func ? func : "?", expr ? expr : "?");
}

static void b_set_abort_message(const char *msg) {
  fprintf(stderr, "[abort_message] %s\n", msg ? msg : "");
}

static int w_raise(int sig) {
  fprintf(stderr, "[raise] swallowed sig=%d\n", sig);
  return 0;
}

static void w_abort(void) { fprintf(stderr, "[abort] swallowed\n"); }

static void b_libcpp_verbose_abort(const char *fmt, ...) {
  fprintf(stderr, "[libcpp_verbose_abort] ");
  if (fmt) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
  }
  fputc('\n', stderr);
}

static int b_property_get(const char *name, char *value) {
  const char *v = "";
  if (!name || !value)
    return 0;
  if (!strcmp(name, "ro.build.version.sdk"))
    v = "25";
  else if (!strcmp(name, "ro.product.cpu.abi"))
    v = "arm64-v8a";
  else if (!strcmp(name, "ro.product.manufacturer"))
    v = "NextOS";
  else if (!strcmp(name, "ro.product.model"))
    v = "NextOS";
  else if (!strcmp(name, "ro.hardware"))
    v = "amlogic";
  strcpy(value, v);
  return (int)strlen(v);
}

static pid_t b_gettid(void) { return (pid_t)syscall(SYS_gettid); }

static int b_memfd_create(const char *name, unsigned flags) {
#ifdef SYS_memfd_create
  return (int)syscall(SYS_memfd_create, name, flags);
#else
  (void)name;
  (void)flags;
  errno = ENOSYS;
  return -1;
#endif
}

static size_t b_strlen_chk(const char *s, size_t n) {
  (void)n;
  return strlen(s);
}
static void *b_memcpy_chk(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return memcpy(dst, src, n);
}
static void *b_memmove_chk(void *dst, const void *src, size_t n,
                           size_t dstlen) {
  (void)dstlen;
  return memmove(dst, src, n);
}
static void *b_memset_chk(void *dst, int c, size_t n, size_t dstlen) {
  (void)dstlen;
  return memset(dst, c, n);
}
static char *b_strcpy_chk(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcpy(dst, src);
}
static char *b_strncpy_chk(char *dst, const char *src, size_t n,
                           size_t dstlen) {
  (void)dstlen;
  return strncpy(dst, src, n);
}
static char *b_strncpy_chk2(char *dst, const char *src, size_t n,
                            size_t dstlen, size_t srclen) {
  (void)dstlen;
  (void)srclen;
  return strncpy(dst, src, n);
}
static char *b_strcat_chk(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcat(dst, src);
}
static char *b_strncat_chk(char *dst, const char *src, size_t n,
                           size_t dstlen) {
  (void)dstlen;
  return strncat(dst, src, n);
}
static char *b_strchr_chk(const char *s, int c, size_t n) {
  (void)n;
  return strchr(s, c);
}
static ssize_t b_read_chk(int fd, void *buf, size_t n, size_t buflen) {
  if (n > buflen)
    n = buflen;
  return read(fd, buf, n);
}
static ssize_t b_write_chk(int fd, const void *buf, size_t n, size_t buflen) {
  if (n > buflen)
    n = buflen;
  return write(fd, buf, n);
}
static void b_FD_SET_chk(int fd, fd_set *set, size_t set_size) {
  if (fd >= 0 && (size_t)fd < set_size * 8)
    FD_SET(fd, set);
}

extern int _setjmp(void *);
extern void _longjmp(void *, int) __attribute__((noreturn));

static int b_cxa_guard_acquire(uint64_t *guard) {
  return ((*guard & 1) == 0);
}
static void b_cxa_guard_release(uint64_t *guard) { *guard |= 1; }
static void b_cxa_guard_abort(uint64_t *guard) { (void)guard; }
static void b_cxa_pure_virtual(void) {
  fprintf(stderr, "[__cxa_pure_virtual] swallowed\n");
}

/* ---------------- stdio path helpers / __sF ---------------- */
static char bionic_sF[3][512];

static FILE *map_sF(void *fp) {
  if (!fp)
    return stderr;
  uintptr_t p = (uintptr_t)fp;
  uintptr_t base = (uintptr_t)&bionic_sF[0];
  if (p >= base && p < base + sizeof(bionic_sF)) {
    uintptr_t off = p - base;
    if (off < 0x98)
      return stdin;
    if (off < 0x130)
      return stdout;
    return stderr;
  }
  if (fp == (void *)&bionic_sF[0])
    return stdin;
  if (fp == (void *)&bionic_sF[1])
    return stdout;
  if (fp == (void *)&bionic_sF[2])
    return stderr;
  return (FILE *)fp;
}

static void mkdir_parents(char *path) {
  for (char *p = path + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      mkdir(path, 0775);
      *p = '/';
    }
  }
}

static void ensure_parent_dir(const char *path) {
  if (!path)
    return;
  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s", path);
  char *slash = strrchr(tmp, '/');
  if (!slash)
    return;
  *slash = 0;
  mkdir_parents(tmp);
  mkdir(tmp, 0775);
}

static const char *asset_base(void) {
  const char *b = getenv("DONTSTARVE_ASSETS");
  return (b && *b) ? b : "assets";
}

static const char *strip_data_android(const char *path) {
  const char *prefix = "data-android/";
  if (path && !strncmp(path, prefix, strlen(prefix)))
    return path + strlen(prefix);
  return NULL;
}

static void register_asset_range(const unsigned char *base, size_t len,
                                 const char *path);
static void unregister_asset_range(const unsigned char *base);
static int asset_path_for_ptr(const void *ptr, size_t len, char *out,
                              size_t outsz);

static int is_texture_asset_path(const char *path) {
  if (!path)
    return 0;
  size_t n = strlen(path);
  return (n > 4 && !strcmp(path + n - 4, ".tex")) ||
         (n > 4 && !strcmp(path + n - 4, ".ktx"));
}

static int should_write_mode(const char *mode) {
  return mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'));
}

#define MAX_FILE_PATHS 256
static pthread_mutex_t g_file_path_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
  FILE *fp;
  char path[1024];
} g_file_paths[MAX_FILE_PATHS];

static void register_file_path(FILE *fp, const char *path) {
  if (!fp || !path)
    return;
  pthread_mutex_lock(&g_file_path_lock);
  for (int i = 0; i < MAX_FILE_PATHS; i++) {
    if (g_file_paths[i].fp == fp) {
      snprintf(g_file_paths[i].path, sizeof(g_file_paths[i].path), "%s", path);
      pthread_mutex_unlock(&g_file_path_lock);
      return;
    }
  }
  for (int i = 0; i < MAX_FILE_PATHS; i++) {
    if (!g_file_paths[i].fp) {
      g_file_paths[i].fp = fp;
      snprintf(g_file_paths[i].path, sizeof(g_file_paths[i].path), "%s", path);
      break;
    }
  }
  pthread_mutex_unlock(&g_file_path_lock);
}

static void unregister_file_path(FILE *fp) {
  if (!fp)
    return;
  pthread_mutex_lock(&g_file_path_lock);
  for (int i = 0; i < MAX_FILE_PATHS; i++) {
    if (g_file_paths[i].fp == fp) {
      g_file_paths[i].fp = NULL;
      g_file_paths[i].path[0] = 0;
      break;
    }
  }
  pthread_mutex_unlock(&g_file_path_lock);
}

static int file_path_for(FILE *fp, char *out, size_t outsz) {
  if (!fp || !out || !outsz)
    return 0;
  pthread_mutex_lock(&g_file_path_lock);
  for (int i = 0; i < MAX_FILE_PATHS; i++) {
    if (g_file_paths[i].fp == fp) {
      snprintf(out, outsz, "%s", g_file_paths[i].path);
      pthread_mutex_unlock(&g_file_path_lock);
      return 1;
    }
  }
  pthread_mutex_unlock(&g_file_path_lock);
  return 0;
}

static void resolve_game_path(const char *path, char *out, size_t outsz) {
  if (!path || !*path) {
    snprintf(out, outsz, "%s", path ? path : "");
    return;
  }
  const char *needle = "/Android/data/com.kleientertainment.doNotStarvePocket/files/";
  const char *p = strstr(path, needle);
  if (p) {
    snprintf(out, outsz, "gamedata/%s", p + strlen(needle));
    return;
  }
  if (!strncmp(path, "file://", 7))
    path += 7;
  snprintf(out, outsz, "%s", path);
}

static void *w_fopen(const char *path, const char *mode) {
  char fixed[1024];
  resolve_game_path(path, fixed, sizeof(fixed));
  if (should_write_mode(mode))
    ensure_parent_dir(fixed);
  FILE *fp = fopen(fixed, mode);
  if (!fp && path && path[0] != '/' && !should_write_mode(mode)) {
    char alt[1024];
    snprintf(alt, sizeof(alt), "%s/%s", asset_base(), path);
    fp = fopen(alt, mode);
    const char *stripped = strip_data_android(path);
    if (!fp && stripped) {
      snprintf(alt, sizeof(alt), "%s/%s", asset_base(), stripped);
      fp = fopen(alt, mode);
    }
    if (!fp && stripped)
      fp = fopen(stripped, mode);
    if (!fp) {
      snprintf(alt, sizeof(alt), "gamedata/%s", path);
      fp = fopen(alt, mode);
    }
  }
  static int n = 0;
  if (n < 160) {
    fprintf(stderr, "[fopen] '%s' -> '%s' %s = %s\n", path ? path : "",
            fixed, mode ? mode : "", fp ? "OK" : "FAIL");
    n++;
  }
  if (fp)
    register_file_path(fp, path && *path ? path : fixed);
  return fp;
}

static void *w_freopen(const char *path, const char *mode, void *fp) {
  char fixed[1024];
  resolve_game_path(path, fixed, sizeof(fixed));
  if (should_write_mode(mode))
    ensure_parent_dir(fixed);
  return freopen(fixed, mode, map_sF(fp));
}
static int w_fclose(void *fp) {
  FILE *f = map_sF(fp);
  if (f == stdin || f == stdout || f == stderr)
    return 0;
  unregister_file_path(f);
  return fclose(f);
}
static size_t w_fread(void *p, size_t s, size_t n, void *fp) {
  if (!p && s && n)
    return 0;
  FILE *f = map_sF(fp);
  size_t r = fread(p, s, n, f);
  if (p && r && s) {
    char path[1024];
    if (file_path_for(f, path, sizeof(path)) && is_texture_asset_path(path))
      register_asset_range((const unsigned char *)p, r * s, path);
  }
  return r;
}
static size_t w_fwrite(const void *p, size_t s, size_t n, void *fp) {
  if (!p && s && n)
    return 0;
  return fwrite(p, s, n, map_sF(fp));
}
static int w_fseek(void *fp, long off, int wh) { return fseek(map_sF(fp), off, wh); }
static int w_fseeko(void *fp, long off, int wh) { return fseeko(map_sF(fp), off, wh); }
static long w_ftell(void *fp) { return ftell(map_sF(fp)); }
static long w_ftello(void *fp) { return ftello(map_sF(fp)); }
static int w_feof(void *fp) { return feof(map_sF(fp)); }
static int w_ferror(void *fp) { return ferror(map_sF(fp)); }
static void w_clearerr(void *fp) { clearerr(map_sF(fp)); }
static int w_fflush(void *fp) { return fflush(fp ? map_sF(fp) : NULL); }
static char *w_fgets(char *s, int n, void *fp) { return fgets(s, n, map_sF(fp)); }
static int w_getc(void *fp) { return getc(map_sF(fp)); }
static int w_ungetc(int c, void *fp) { return ungetc(c, map_sF(fp)); }
static int w_fputc(int c, void *fp) { return fputc(c, map_sF(fp)); }
static int w_fputs(const char *s, void *fp) {
  if (!s) {
    fprintf(stderr, "[fputs] NULL string\n");
    s = "";
  }
  return fputs(s, map_sF(fp));
}
static int w_fprintf(void *fp, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vfprintf(map_sF(fp), fmt ? fmt : "", ap);
  va_end(ap);
  return r;
}
static int w_vfprintf(void *fp, const char *fmt, va_list ap) {
  return vfprintf(map_sF(fp), fmt ? fmt : "", ap);
}

static int w_open(const char *path, int flags, ...) {
  mode_t mode = 0666;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  char fixed[1024];
  resolve_game_path(path, fixed, sizeof(fixed));
  if (flags & (O_CREAT | O_WRONLY | O_RDWR))
    ensure_parent_dir(fixed);
  int fd = open(fixed, flags, mode);
  if (fd < 0 && path && path[0] != '/' && !(flags & (O_CREAT | O_WRONLY | O_RDWR))) {
    char alt[1024];
    snprintf(alt, sizeof(alt), "%s/%s", asset_base(), path);
    fd = open(alt, flags, mode);
    const char *stripped = strip_data_android(path);
    if (fd < 0 && stripped) {
      snprintf(alt, sizeof(alt), "%s/%s", asset_base(), stripped);
      fd = open(alt, flags, mode);
    }
    if (fd < 0 && stripped)
      fd = open(stripped, flags, mode);
  }
  return fd;
}
static int w_open_2(const char *path, int flags) { return w_open(path, flags); }
static int w_stat(const char *path, struct stat *st) {
  char fixed[1024];
  resolve_game_path(path, fixed, sizeof(fixed));
  int r = stat(fixed, st);
  if (r < 0 && path && path[0] != '/') {
    char alt[1024];
    snprintf(alt, sizeof(alt), "%s/%s", asset_base(), path);
    r = stat(alt, st);
    const char *stripped = strip_data_android(path);
    if (r < 0 && stripped) {
      snprintf(alt, sizeof(alt), "%s/%s", asset_base(), stripped);
      r = stat(alt, st);
    }
    if (r < 0 && stripped)
      r = stat(stripped, st);
  }
  return r;
}

static void *w_opendir(const char *path) {
  char fixed[1024];
  resolve_game_path(path, fixed, sizeof(fixed));
  if (strstr(fixed, "gamedata") || strstr(fixed, "save"))
    mkdir(fixed, 0775);
  DIR *d = opendir(fixed);
  if (!d && path && path[0] != '/') {
    const char *stripped = strip_data_android(path);
    if (stripped) {
      char alt[1024];
      snprintf(alt, sizeof(alt), "%s/%s", asset_base(), stripped);
      d = opendir(alt);
      if (!d)
        d = opendir(stripped);
    }
  }
  static int n = 0;
  if (n < 80) {
    fprintf(stderr, "[opendir] '%s' -> '%s' = %s\n", path ? path : "",
            fixed, d ? "OK" : strerror(errno));
    n++;
  }
  return d;
}

static void *w_readdir(void *dirp) {
  if (!dirp) {
    static int n = 0;
    if (n < 20) {
      fprintf(stderr, "[readdir] NULL -> EOF\n");
      n++;
    }
    return NULL;
  }
  return readdir((DIR *)dirp);
}

static int w_closedir(void *dirp) {
  if (!dirp)
    return 0;
  return closedir((DIR *)dirp);
}

static int w_mkdir(const char *path, mode_t mode) {
  char fixed[1024];
  resolve_game_path(path, fixed, sizeof(fixed));
  mkdir_parents(fixed);
  int r = mkdir(fixed, mode);
  if (r < 0 && errno == EEXIST)
    return 0;
  return r;
}

static int w_remove_path(const char *path) {
  char fixed[1024];
  resolve_game_path(path, fixed, sizeof(fixed));
  return remove(fixed);
}

static int w_unlink_path(const char *path) {
  char fixed[1024];
  resolve_game_path(path, fixed, sizeof(fixed));
  return unlink(fixed);
}

static int w_rename_path(const char *oldpath, const char *newpath) {
  char oldfixed[1024], newfixed[1024];
  resolve_game_path(oldpath, oldfixed, sizeof(oldfixed));
  resolve_game_path(newpath, newfixed, sizeof(newfixed));
  ensure_parent_dir(newfixed);
  return rename(oldfixed, newfixed);
}

/* ---------------- Android assets ---------------- */
typedef struct {
  FILE *fp;
  long len;
  unsigned char *buf;
  char path[1024];
} DSAsset;

#define MAX_ASSET_RANGES 512
static pthread_mutex_t g_asset_ranges_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
  const unsigned char *base;
  size_t len;
  char path[1024];
} g_asset_ranges[MAX_ASSET_RANGES];

static void register_asset_range(const unsigned char *base, size_t len,
                                 const char *path) {
  if (!base || !len || !path)
    return;
  pthread_mutex_lock(&g_asset_ranges_lock);
  for (int i = 0; i < MAX_ASSET_RANGES; i++) {
    if (g_asset_ranges[i].base == base) {
      g_asset_ranges[i].len = len;
      snprintf(g_asset_ranges[i].path, sizeof(g_asset_ranges[i].path), "%s",
               path);
      pthread_mutex_unlock(&g_asset_ranges_lock);
      return;
    }
  }
  for (int i = 0; i < MAX_ASSET_RANGES; i++) {
    if (!g_asset_ranges[i].base) {
      g_asset_ranges[i].base = base;
      g_asset_ranges[i].len = len;
      snprintf(g_asset_ranges[i].path, sizeof(g_asset_ranges[i].path), "%s",
               path);
      pthread_mutex_unlock(&g_asset_ranges_lock);
      return;
    }
  }
  pthread_mutex_unlock(&g_asset_ranges_lock);
}

static void unregister_asset_range(const unsigned char *base) {
  if (!base)
    return;
  pthread_mutex_lock(&g_asset_ranges_lock);
  for (int i = 0; i < MAX_ASSET_RANGES; i++) {
    if (g_asset_ranges[i].base == base) {
      g_asset_ranges[i].base = NULL;
      g_asset_ranges[i].len = 0;
      g_asset_ranges[i].path[0] = 0;
      break;
    }
  }
  pthread_mutex_unlock(&g_asset_ranges_lock);
}

static int asset_path_for_ptr(const void *ptr, size_t len, char *out,
                              size_t outsz) {
  if (!ptr || !out || !outsz)
    return 0;
  uintptr_t p = (uintptr_t)ptr;
  uintptr_t e = p + len;
  pthread_mutex_lock(&g_asset_ranges_lock);
  for (int i = 0; i < MAX_ASSET_RANGES; i++) {
    if (!g_asset_ranges[i].base || !g_asset_ranges[i].len)
      continue;
    uintptr_t b = (uintptr_t)g_asset_ranges[i].base;
    uintptr_t end = b + g_asset_ranges[i].len;
    if (p >= b && e <= end) {
      snprintf(out, outsz, "%s", g_asset_ranges[i].path);
      pthread_mutex_unlock(&g_asset_ranges_lock);
      return 1;
    }
  }
  pthread_mutex_unlock(&g_asset_ranges_lock);
  return 0;
}

static void *aam_fromJava(void *env, void *obj) {
  (void)env;
  (void)obj;
  return (void *)1;
}

static FILE *try_asset_open(const char *fn, char *path, size_t pathsz) {
  snprintf(path, pathsz, "%s/%s", asset_base(), fn ? fn : "");
  FILE *fp = fopen(path, "rb");
  if (!fp && fn && !strncmp(fn, "assets/", 7)) {
    snprintf(path, pathsz, "%s", fn);
    fp = fopen(path, "rb");
  }
  const char *stripped = strip_data_android(fn);
  if (!fp && stripped) {
    snprintf(path, pathsz, "%s/%s", asset_base(), stripped);
    fp = fopen(path, "rb");
  }
  if (!fp && stripped) {
    snprintf(path, pathsz, "%s", stripped);
    fp = fopen(path, "rb");
  }
  if (!fp && fn && fn[0] != '/') {
    snprintf(path, pathsz, "%s", fn);
    fp = fopen(path, "rb");
  }
  return fp;
}

static void *aam_open(void *mgr, const char *fn, int mode) {
  (void)mgr;
  (void)mode;
  char path[1024];
  FILE *fp = try_asset_open(fn, path, sizeof(path));
  if (!fp) {
    static int misses = 0;
    if (misses < 120) {
      fprintf(stderr, "[AAsset] MISS %s\n", fn ? fn : "");
      misses++;
    }
    return NULL;
  }
  DSAsset *a = calloc(1, sizeof(*a));
  a->fp = fp;
  fseek(fp, 0, SEEK_END);
  a->len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  snprintf(a->path, sizeof(a->path), "%s", path);
  static int opens = 0;
  if (opens < 160) {
    fprintf(stderr, "[AAsset] open %s len=%ld\n", fn ? fn : "", a->len);
    opens++;
  }
  return a;
}

static int aa_read(void *h, void *buf, size_t n) {
  DSAsset *a = h;
  if (!a || !a->fp)
    return -1;
  return (int)fread(buf, 1, n, a->fp);
}
static long aa_seek(void *h, long off, int wh) {
  DSAsset *a = h;
  if (!a || !a->fp)
    return -1;
  if (fseek(a->fp, off, wh) != 0)
    return -1;
  return ftell(a->fp);
}
static long aa_getLength(void *h) {
  DSAsset *a = h;
  return a ? a->len : 0;
}
static void *aa_getBuffer(void *h) {
  DSAsset *a = h;
  if (!a || !a->fp)
    return NULL;
  if (!a->buf) {
    a->buf = malloc(a->len > 0 ? (size_t)a->len : 1);
    if (!a->buf)
      return NULL;
    long pos = ftell(a->fp);
    fseek(a->fp, 0, SEEK_SET);
    fread(a->buf, 1, a->len, a->fp);
    fseek(a->fp, pos, SEEK_SET);
    register_asset_range(a->buf, a->len > 0 ? (size_t)a->len : 1, a->path);
  }
  return a->buf;
}
static void aa_close(void *h) {
  DSAsset *a = h;
  if (!a)
    return;
  if (a->fp)
    fclose(a->fp);
  if (a->buf)
    unregister_asset_range(a->buf);
  free(a->buf);
  free(a);
}

/* ---------------- Android window / looper extras ---------------- */
static int aw_getWidth(void *w) {
  (void)w;
  return dys_screen_w;
}
static int aw_getHeight(void *w) {
  (void)w;
  return dys_screen_h;
}
static int aw_setBuffersGeometry(void *w, int width, int height, int format) {
  (void)w;
  (void)width;
  (void)height;
  (void)format;
  return 0;
}
static int al_pollOnce(int t, int *fd, int *ev, void **data) {
  return ALooper_pollAll(t, fd, ev, data);
}

/* ---------------- pthread attr / sem bridge ---------------- */
typedef struct {
  uint32_t flags;
  void *stack_base;
  size_t stack_size;
  size_t guard_size;
  int32_t sched_policy;
  int32_t sched_priority;
} BionicPthreadAttr;

static int b_pthread_attr_init(void *attr) {
  if (attr) {
    BionicPthreadAttr *a = attr;
    memset(a, 0, sizeof(*a));
    a->stack_size = 1024 * 1024;
  }
  return 0;
}
static int b_pthread_attr_destroy(void *attr) {
  (void)attr;
  return 0;
}
static int b_pthread_attr_setdetachstate(void *attr, int state) {
  if (attr)
    ((BionicPthreadAttr *)attr)->flags = (uint32_t)state;
  return 0;
}
static int b_pthread_attr_setstacksize(void *attr, size_t size) {
  if (attr)
    ((BionicPthreadAttr *)attr)->stack_size = size;
  return 0;
}
static int b_pthread_attr_setschedparam(void *attr, const void *param) {
  (void)attr;
  (void)param;
  return 0;
}
static int b_pthread_attr_setschedpolicy(void *attr, int policy) {
  if (attr)
    ((BionicPthreadAttr *)attr)->sched_policy = policy;
  return 0;
}
static int b_pthread_create(pthread_t *thread, const void *attr,
                            void *(*start)(void *), void *arg) {
  pthread_attr_t ga;
  pthread_attr_init(&ga);
  if (attr) {
    const BionicPthreadAttr *ba = attr;
    if (ba->stack_size >= 32768 && ba->stack_size < 256 * 1024 * 1024)
      pthread_attr_setstacksize(&ga, ba->stack_size);
    if ((int)ba->flags == PTHREAD_CREATE_DETACHED)
      pthread_attr_setdetachstate(&ga, PTHREAD_CREATE_DETACHED);
  }
  int r = pthread_create(thread, &ga, start, arg);
  pthread_attr_destroy(&ga);
  return r;
}
static int b_pthread_setname_np(pthread_t t, const char *name) {
#ifdef __linux__
  char tmp[16];
  snprintf(tmp, sizeof(tmp), "%s", name ? name : "thread");
  return pthread_setname_np(t, tmp);
#else
  (void)t;
  (void)name;
  return 0;
#endif
}
static int b_pthread_setschedparam(pthread_t t, int policy, const void *param) {
  (void)t;
  (void)policy;
  (void)param;
  return 0;
}

#define MAX_SEM_MAP 128
static pthread_mutex_t g_sem_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
  void *key;
  sem_t *sem;
} g_sem_map[MAX_SEM_MAP];

static sem_t *sem_lookup(void *key, int create, unsigned value) {
  pthread_mutex_lock(&g_sem_lock);
  for (int i = 0; i < MAX_SEM_MAP; i++) {
    if (g_sem_map[i].key == key) {
      sem_t *s = g_sem_map[i].sem;
      pthread_mutex_unlock(&g_sem_lock);
      return s;
    }
  }
  if (!create) {
    pthread_mutex_unlock(&g_sem_lock);
    return NULL;
  }
  for (int i = 0; i < MAX_SEM_MAP; i++) {
    if (!g_sem_map[i].key) {
      g_sem_map[i].key = key;
      g_sem_map[i].sem = calloc(1, sizeof(sem_t));
      sem_init(g_sem_map[i].sem, 0, value);
      sem_t *s = g_sem_map[i].sem;
      pthread_mutex_unlock(&g_sem_lock);
      return s;
    }
  }
  pthread_mutex_unlock(&g_sem_lock);
  return NULL;
}
static int b_sem_init(void *sem, int pshared, unsigned value) {
  (void)pshared;
  return sem_lookup(sem, 1, value) ? 0 : -1;
}
static int b_sem_wait(void *sem) {
  sem_t *s = sem_lookup(sem, 1, 0);
  return s ? sem_wait(s) : -1;
}
static int b_sem_post(void *sem) {
  sem_t *s = sem_lookup(sem, 1, 0);
  return s ? sem_post(s) : -1;
}
static int b_sem_destroy(void *sem) {
  pthread_mutex_lock(&g_sem_lock);
  for (int i = 0; i < MAX_SEM_MAP; i++) {
    if (g_sem_map[i].key == sem) {
      sem_destroy(g_sem_map[i].sem);
      free(g_sem_map[i].sem);
      g_sem_map[i].key = NULL;
      g_sem_map[i].sem = NULL;
      break;
    }
  }
  pthread_mutex_unlock(&g_sem_lock);
  return 0;
}

/* ---------------- OpenSL dynamic loading for FMOD ---------------- */
static const void *SL_IID_ENGINE_v = NULL;
static const void *SL_IID_PLAY_v = NULL;
static const void *SL_IID_VOLUME_v = NULL;
static const void *SL_IID_BUFFERQUEUE_v = NULL;
static const void *SL_IID_ANDROIDSIMPLEBUFFERQUEUE_v = NULL;
static const void *SL_IID_RECORD_v = NULL;
static const void *SL_IID_ANDROIDCONFIGURATION_v = "ANDROIDCONFIGURATION";
static const void *SL_IID_OUTPUTMIX_v = "OUTPUTMIX";
static const void *SL_IID_ENVIRONMENTALREVERB_v = NULL;

__attribute__((constructor)) static void init_sl_iids(void) {
  SL_IID_ENGINE_v = sl_IID_ENGINE;
  SL_IID_PLAY_v = sl_IID_PLAY;
  SL_IID_VOLUME_v = sl_IID_VOLUME;
  SL_IID_BUFFERQUEUE_v = sl_IID_BUFFERQUEUE;
  SL_IID_ANDROIDSIMPLEBUFFERQUEUE_v = sl_IID_BUFFERQUEUE;
  SL_IID_RECORD_v = sl_IID_RECORD;
  SL_IID_ENVIRONMENTALREVERB_v = sl_IID_ENVIRONMENTALREVERB;
}

#define FAKE_OPENSL_HANDLE ((void *)0x514f5045)

static void *w_dlopen(const char *name, int flags) {
  if (name && strstr(name, "libOpenSLES.so")) {
    fprintf(stderr, "[dlopen] %s -> opensles_shim\n", name);
    return FAKE_OPENSL_HANDLE;
  }
  if (name && strstr(name, "libaaudio.so")) {
    fprintf(stderr, "[dlopen] %s -> blocked (sdk25 path)\n", name);
    return NULL;
  }
  return dlopen(name, flags);
}

static void *w_dlsym(void *handle, const char *name) {
  if (handle == FAKE_OPENSL_HANDLE) {
    if (!strcmp(name, "slCreateEngine"))
      return (void *)slCreateEngine_shim;
    if (!strcmp(name, "SL_IID_ENGINE"))
      return &SL_IID_ENGINE_v;
    if (!strcmp(name, "SL_IID_PLAY"))
      return &SL_IID_PLAY_v;
    if (!strcmp(name, "SL_IID_VOLUME"))
      return &SL_IID_VOLUME_v;
    if (!strcmp(name, "SL_IID_BUFFERQUEUE"))
      return &SL_IID_BUFFERQUEUE_v;
    if (!strcmp(name, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"))
      return &SL_IID_ANDROIDSIMPLEBUFFERQUEUE_v;
    if (!strcmp(name, "SL_IID_RECORD"))
      return &SL_IID_RECORD_v;
    if (!strcmp(name, "SL_IID_ANDROIDCONFIGURATION"))
      return &SL_IID_ANDROIDCONFIGURATION_v;
    if (!strcmp(name, "SL_IID_OUTPUTMIX"))
      return &SL_IID_OUTPUTMIX_v;
    if (!strcmp(name, "SL_IID_ENVIRONMENTALREVERB"))
      return &SL_IID_ENVIRONMENTALREVERB_v;
    fprintf(stderr, "[dlsym OpenSL] miss %s\n", name ? name : "");
    return NULL;
  }
  return dlsym(handle, name);
}

static int w_dlclose(void *handle) {
  if (handle == FAKE_OPENSL_HANDLE)
    return 0;
  return dlclose(handle);
}

/* ---------------- GL wrappers ---------------- */
static void rgl(const char *name, void **slot) {
  if (!*slot)
    *slot = dlsym(RTLD_DEFAULT, name);
}

static const GLubyte *my_glGetString(GLenum name) {
  switch (name) {
  case GL_VENDOR:
    return (const GLubyte *)"NextOS";
  case GL_RENDERER:
    return (const GLubyte *)"Mali-450 (GLES2)";
  case GL_VERSION:
    return (const GLubyte *)"OpenGL ES 2.0";
  case GL_SHADING_LANGUAGE_VERSION:
    return (const GLubyte *)"OpenGL ES GLSL ES 1.00";
  case GL_EXTENSIONS:
    return (const GLubyte *)"GL_OES_compressed_ETC1_RGB8_texture "
                            "GL_OES_rgb8_rgba8 "
                            "GL_OES_depth24 "
                            "GL_OES_packed_depth_stencil "
                            "GL_OES_element_index_uint "
                            "GL_OES_texture_npot";
  default: {
    static const GLubyte *(*real)(GLenum) = NULL;
    rgl("glGetString", (void **)&real);
    return real ? real(name) : (const GLubyte *)"";
  }
  }
}

static void my_glGetIntegerv(GLenum pname, GLint *params) {
  static void (*real)(GLenum, GLint *) = NULL;
  if (!params)
    return;
  if (pname == 0x86A2) {
    *params = 1;
    return;
  }
  if (pname == 0x86A3) {
    params[0] = 0x8D64;
    return;
  }
  if (pname == GL_MAX_TEXTURE_SIZE) {
    *params = 4096;
    return;
  }
  rgl("glGetIntegerv", (void **)&real);
  if (real)
    real(pname, params);
}

#define DS_MAX_TEX_UNITS 16
#define DS_ALPHA_UNIT 4
#define MAX_ALPHA_SIDECARS 2048
#define MAX_ALPHA_PROGRAMS 512
#define MAX_TEXTURE_POLICIES 4096

static int g_active_tex_unit = 0;
static GLuint g_bound_2d[DS_MAX_TEX_UNITS];
static GLuint g_current_program = 0;
static GLuint g_white_alpha_tex = 0;

typedef struct {
  GLuint base;
  GLuint alpha;
  int enabled;
  int force_dual;
  char path[160];
} AlphaSidecar;

typedef struct {
  GLuint program;
  GLint loc;
  GLint loc_enabled;
  int checked;
  int uses_alpha;
} AlphaProgram;

typedef struct {
  GLuint tex;
  int scale;
  int is_ground;
} TexturePolicy;

static AlphaSidecar g_alpha_sidecars[MAX_ALPHA_SIDECARS];
static AlphaProgram g_alpha_programs[MAX_ALPHA_PROGRAMS];
static TexturePolicy g_texture_policies[MAX_TEXTURE_POLICIES];

static int tex_unit_index(GLenum texture) {
  if (texture < GL_TEXTURE0)
    return -1;
  int unit = (int)(texture - GL_TEXTURE0);
  return (unit >= 0 && unit < DS_MAX_TEX_UNITS) ? unit : -1;
}

static AlphaSidecar *alpha_sidecar_for(GLuint base, int create) {
  if (!base)
    return NULL;
  for (int i = 0; i < MAX_ALPHA_SIDECARS; i++) {
    if (g_alpha_sidecars[i].base == base)
      return &g_alpha_sidecars[i];
  }
  if (!create)
    return NULL;
  for (int i = 0; i < MAX_ALPHA_SIDECARS; i++) {
    if (!g_alpha_sidecars[i].base) {
      g_alpha_sidecars[i].base = base;
      return &g_alpha_sidecars[i];
    }
  }
  return NULL;
}

static TexturePolicy *texture_policy_for(GLuint tex, int create) {
  if (!tex)
    return NULL;
  for (int i = 0; i < MAX_TEXTURE_POLICIES; i++) {
    if (g_texture_policies[i].tex == tex)
      return &g_texture_policies[i];
  }
  if (!create)
    return NULL;
  for (int i = 0; i < MAX_TEXTURE_POLICIES; i++) {
    if (!g_texture_policies[i].tex) {
      g_texture_policies[i].tex = tex;
      return &g_texture_policies[i];
    }
  }
  return NULL;
}

static GLuint enabled_alpha_for(GLuint base) {
  AlphaSidecar *sc = alpha_sidecar_for(base, 0);
  return (sc && sc->enabled) ? sc->alpha : 0;
}

static AlphaProgram *alpha_program_for(GLuint program, int create) {
  if (!program)
    return NULL;
  for (int i = 0; i < MAX_ALPHA_PROGRAMS; i++) {
    if (g_alpha_programs[i].program == program)
      return &g_alpha_programs[i];
  }
  if (!create)
    return NULL;
  for (int i = 0; i < MAX_ALPHA_PROGRAMS; i++) {
    if (!g_alpha_programs[i].program) {
      g_alpha_programs[i].program = program;
      g_alpha_programs[i].loc = -1;
      g_alpha_programs[i].loc_enabled = -1;
      return &g_alpha_programs[i];
    }
  }
  return NULL;
}

static int path_has_suffix(const char *path, const char *suffix) {
  if (!path || !suffix)
    return 0;
  size_t lp = strlen(path), ls = strlen(suffix);
  return lp >= ls && !strcmp(path + lp - ls, suffix);
}

static int is_ground_tile_path(const char *path) {
  if (!path || !strstr(path, "levels/tiles/"))
    return 0;
  if (path_has_suffix(path, "/map.tex") ||
      path_has_suffix(path, "/map_edge.tex") ||
      path_has_suffix(path, "/falloff.tex"))
    return 0;
  return 1;
}

static int dual_alpha_mode_requested(void) {
  const char *mode = getenv("DONTSTARVE_ETC1_ALPHA");
  return mode && *mode && strcmp(mode, "0") && strcmp(mode, "off") &&
         strcmp(mode, "false");
}

static int dual_alpha_allowed_for_path(const char *path) {
  const char *mode = getenv("DONTSTARVE_ETC1_ALPHA");
  if (!dual_alpha_mode_requested())
    return 0;
  if (!strcmp(mode, "all"))
    return 1;
  if (!strcmp(mode, "tiles"))
    return path && strstr(path, "levels/tiles/");
  if (!strcmp(mode, "ground"))
    return is_ground_tile_path(path);
  return path && strstr(path, mode);
}

typedef struct {
  uint16_t w;
  uint16_t h;
  uint32_t size;
  uint32_t hash;
} GroundTileHash;

static const GroundTileHash g_ground_tile_hashes[] = {
    {1024, 1024, 1048576u, 0x3cb675ecu}, /* blocks */
    {1024, 1024, 1048576u, 0xac054c9au}, /* blocky */
    {1024, 1024, 1048576u, 0x250b1aefu}, /* carpet */
    {1024, 1024, 1048576u, 0x72c6938du}, /* cave */
    {1024, 1024, 1048576u, 0x0ca3aaf1u}, /* cobblestone */
    {1024, 1024, 1048576u, 0xf269c21bu}, /* deciduous */
    {1024, 1024, 1048576u, 0x1356b7efu}, /* desert_dirt */
    {1024, 1024, 1048576u, 0x2a27c6e6u}, /* dirt */
    {2048, 2048, 4194304u, 0xc7edfeceu}, /* forest */
    {2048, 2048, 4194304u, 0x1eedf384u}, /* grass */
    {2048, 2048, 4194304u, 0x69cb2dbcu}, /* grass2 */
    {1024, 1024, 1048576u, 0x4a4fa003u}, /* marsh */
    {1024, 1024, 1048576u, 0xa3f907dcu}, /* marsh_pond */
    {1024, 1024, 1048576u, 0x0e8e0238u}, /* moat */
    {1024, 1024, 1048576u, 0x960be91fu}, /* rocky */
    {1024, 1024, 1048576u, 0x8a71c3d5u}, /* test */
    {2048, 2048, 4194304u, 0xd3ecb2adu}, /* walls */
    {1024, 1024, 1048576u, 0x54318695u}, /* web */
    {1024, 1024, 1048576u, 0xcfe1d54du}, /* yellowgrass */
};

static uint32_t fnv1a32(const void *data, size_t size) {
  const unsigned char *p = (const unsigned char *)data;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < size; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

static int ground_tile_top_hash_matches_raw(GLsizei width, GLsizei height,
                                            GLsizei imageSize,
                                            const void *data) {
  if (!data || imageSize <= 0)
    return 0;
  int candidate = 0;
  for (size_t i = 0;
       i < sizeof(g_ground_tile_hashes) / sizeof(g_ground_tile_hashes[0]);
       i++) {
    if (g_ground_tile_hashes[i].w == (uint16_t)width &&
        g_ground_tile_hashes[i].h == (uint16_t)height &&
        g_ground_tile_hashes[i].size == (uint32_t)imageSize) {
      candidate = 1;
      break;
    }
  }
  if (!candidate)
    return 0;
  uint32_t h = fnv1a32(data, (size_t)imageSize);
  for (size_t i = 0;
       i < sizeof(g_ground_tile_hashes) / sizeof(g_ground_tile_hashes[0]);
       i++) {
    if (g_ground_tile_hashes[i].w == (uint16_t)width &&
        g_ground_tile_hashes[i].h == (uint16_t)height &&
        g_ground_tile_hashes[i].size == (uint32_t)imageSize &&
        g_ground_tile_hashes[i].hash == h)
      return 1;
  }
  return 0;
}

static int ground_tile_top_hash_matches(GLsizei width, GLsizei height,
                                        GLsizei imageSize,
                                        const void *data) {
  return dual_alpha_mode_requested() &&
         ground_tile_top_hash_matches_raw(width, height, imageSize, data);
}

static unsigned char *make_alpha_rgba(const unsigned char *rgba, int w, int h) {
  if (!rgba || w <= 0 || h <= 0)
    return NULL;
  size_t n = (size_t)w * (size_t)h;
  unsigned char *out = malloc(n * 4);
  if (!out)
    return NULL;
  for (size_t i = 0; i < n; i++) {
    unsigned char a = rgba[i * 4 + 3];
    out[i * 4 + 0] = a;
    out[i * 4 + 1] = a;
    out[i * 4 + 2] = a;
    out[i * 4 + 3] = 255;
  }
  return out;
}

static GLuint ensure_white_alpha_texture(void) {
  if (g_white_alpha_tex)
    return g_white_alpha_tex;

  static void (*gen)(GLsizei, GLuint *) = NULL;
  static void (*active)(GLenum) = NULL;
  static void (*bind)(GLenum, GLuint) = NULL;
  static void (*tex2d)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                       GLenum, const void *) = NULL;
  static void (*parami)(GLenum, GLenum, GLint) = NULL;
  rgl("glGenTextures", (void **)&gen);
  rgl("glActiveTexture", (void **)&active);
  rgl("glBindTexture", (void **)&bind);
  rgl("glTexImage2D", (void **)&tex2d);
  rgl("glTexParameteri", (void **)&parami);
  if (!gen || !active || !bind || !tex2d)
    return 0;

  int prev_unit = g_active_tex_unit;
  GLuint prev_alpha_binding =
      (DS_ALPHA_UNIT < DS_MAX_TEX_UNITS) ? g_bound_2d[DS_ALPHA_UNIT] : 0;
  unsigned char white = 255;
  gen(1, &g_white_alpha_tex);
  active(GL_TEXTURE0 + DS_ALPHA_UNIT);
  bind(GL_TEXTURE_2D, g_white_alpha_tex);
  if (parami) {
    parami(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    parami(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    parami(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    parami(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  }
  tex2d(GL_TEXTURE_2D, 0, GL_LUMINANCE, 1, 1, 0, GL_LUMINANCE,
        GL_UNSIGNED_BYTE, &white);
  bind(GL_TEXTURE_2D, prev_alpha_binding);
  active(GL_TEXTURE0 + prev_unit);
  return g_white_alpha_tex;
}

static int ensure_alpha_program_uniform(GLuint program) {
  if (!program)
    return 0;
  AlphaProgram *ap = alpha_program_for(program, 1);
  if (!ap)
    return 0;
  static GLint (*getloc)(GLuint, const GLchar *) = NULL;
  static void (*uni1i)(GLint, GLint) = NULL;
  static void (*uni1f)(GLint, GLfloat) = NULL;
  rgl("glGetUniformLocation", (void **)&getloc);
  rgl("glUniform1i", (void **)&uni1i);
  rgl("glUniform1f", (void **)&uni1f);
  if (!getloc || !uni1i)
    return 0;
  if (!ap->checked) {
    ap->loc = getloc(program, "DS_ALPHA0");
    ap->loc_enabled = getloc(program, "DS_ALPHA0_ENABLED");
    ap->uses_alpha = ap->loc >= 0 && ap->loc_enabled >= 0;
    ap->checked = 1;
    if (ap->uses_alpha) {
      static int n = 0;
      if (n < 16) {
        fprintf(stderr, "[ETC1A] program %u uses DS_ALPHA0 unit=%d\n",
                program, DS_ALPHA_UNIT);
        n++;
      }
    }
  }
  if (ap->uses_alpha) {
    uni1i(ap->loc, DS_ALPHA_UNIT);
    if (uni1f)
      uni1f(ap->loc_enabled, 0.0f);
  }
  return ap->uses_alpha;
}

static int current_program_uses_alpha(void) {
  AlphaProgram *ap = alpha_program_for(g_current_program, 0);
  if (!ap || !ap->checked)
    return ensure_alpha_program_uniform(g_current_program);
  return ap->uses_alpha;
}

static int bind_alpha_for_draw(int *prev_unit, GLuint *prev_alpha_binding) {
  if (!current_program_uses_alpha())
    return 0;
  GLuint alpha = enabled_alpha_for(g_bound_2d[0]);
  int has_sidecar = alpha != 0;
  if (!alpha)
    alpha = ensure_white_alpha_texture();
  AlphaProgram *ap = alpha_program_for(g_current_program, 0);
  static void (*uni1f)(GLint, GLfloat) = NULL;
  rgl("glUniform1f", (void **)&uni1f);
  if (ap && ap->loc_enabled >= 0 && uni1f)
    uni1f(ap->loc_enabled, has_sidecar ? 1.0f : 0.0f);
  if (!alpha)
    return 0;

  static void (*active)(GLenum) = NULL;
  static void (*bind)(GLenum, GLuint) = NULL;
  rgl("glActiveTexture", (void **)&active);
  rgl("glBindTexture", (void **)&bind);
  if (!active || !bind)
    return 0;

  *prev_unit = g_active_tex_unit;
  *prev_alpha_binding = g_bound_2d[DS_ALPHA_UNIT];
  active(GL_TEXTURE0 + DS_ALPHA_UNIT);
  bind(GL_TEXTURE_2D, alpha);
  g_bound_2d[DS_ALPHA_UNIT] = alpha;
  active(GL_TEXTURE0 + *prev_unit);
  return 1;
}

static void restore_alpha_after_draw(int prev_unit, GLuint prev_alpha_binding) {
  static void (*active)(GLenum) = NULL;
  static void (*bind)(GLenum, GLuint) = NULL;
  rgl("glActiveTexture", (void **)&active);
  rgl("glBindTexture", (void **)&bind);
  if (!active || !bind)
    return;
  active(GL_TEXTURE0 + DS_ALPHA_UNIT);
  bind(GL_TEXTURE_2D, prev_alpha_binding);
  g_bound_2d[DS_ALPHA_UNIT] = prev_alpha_binding;
  active(GL_TEXTURE0 + prev_unit);
}

static int rgba_has_alpha(const unsigned char *rgba, int w, int h) {
  size_t n = (size_t)w * (size_t)h;
  for (size_t i = 0; i < n; i++) {
    if (rgba[i * 4 + 3] < 250)
      return 1;
  }
  return 0;
}

static unsigned char *pack_rgba4444(const unsigned char *rgba, int w, int h) {
  size_t n = (size_t)w * (size_t)h;
  unsigned char *out = malloc(n * 2);
  if (!out)
    return NULL;
  for (size_t i = 0; i < n; i++) {
    uint16_t v = (uint16_t)((rgba[i * 4 + 0] >> 4) << 12 |
                            (rgba[i * 4 + 1] >> 4) << 8 |
                            (rgba[i * 4 + 2] >> 4) << 4 |
                            (rgba[i * 4 + 3] >> 4));
    out[i * 2 + 0] = (unsigned char)(v & 0xff);
    out[i * 2 + 1] = (unsigned char)(v >> 8);
  }
  return out;
}

static unsigned char *pack_rgb565(const unsigned char *rgba, int w, int h) {
  size_t n = (size_t)w * (size_t)h;
  unsigned char *out = malloc(n * 2);
  if (!out)
    return NULL;
  for (size_t i = 0; i < n; i++) {
    uint16_t v = (uint16_t)(((rgba[i * 4 + 0] >> 3) << 11) |
                            ((rgba[i * 4 + 1] >> 2) << 5) |
                            (rgba[i * 4 + 2] >> 3));
    out[i * 2 + 0] = (unsigned char)(v & 0xff);
    out[i * 2 + 1] = (unsigned char)(v >> 8);
  }
  return out;
}

static int texture_downscale_factor(void) {
  static int factor = -1;
  if (factor < 0) {
    const char *env = getenv("DONTSTARVE_TEX_DOWNSCALE");
    factor = env && *env ? atoi(env) : 3;
    if (factor < 1)
      factor = 1;
    if (factor > 4)
      factor = 4;
  }
  return factor;
}

static int env_flag_enabled(const char *name, int fallback) {
  const char *v = getenv(name);
  if (!v || !*v)
    return fallback;
  return strcmp(v, "0") && strcmp(v, "off") && strcmp(v, "false") &&
         strcmp(v, "no");
}

static int alpha_texture_downscale_factor(void) {
  static int factor = -1;
  if (factor < 0) {
    int base = texture_downscale_factor();
    const char *env = getenv("DONTSTARVE_TEX_ALPHA_DOWNSCALE");
    factor = env && *env ? atoi(env) : (base <= 2 ? 3 : base);
    if (factor < base)
      factor = base;
    if (factor < 1)
      factor = 1;
    if (factor > 4)
      factor = 4;
  }
  return factor;
}

static int current_bound_texture(void) {
  return (g_active_tex_unit >= 0 && g_active_tex_unit < DS_MAX_TEX_UNITS)
             ? (int)g_bound_2d[g_active_tex_unit]
             : 0;
}

static int compressed_texture_downscale_factor(
    const char *tag, GLenum fmt, GLint level, GLsizei width, GLsizei height,
    GLsizei imageSize, const void *data, const char *asset_path,
    int source_has_alpha) {
  int base_scale = texture_downscale_factor();
  if (!env_flag_enabled("DONTSTARVE_TEX_HYBRID", 1))
    return base_scale;

  GLuint tex = (GLuint)current_bound_texture();
  TexturePolicy *policy = texture_policy_for(tex, 0);
  if (policy && policy->scale > 0)
    return policy->scale;

  int is_ground =
      (asset_path && is_ground_tile_path(asset_path)) ||
      (level == 0 && ground_tile_top_hash_matches_raw(width, height, imageSize,
                                                      data));
  int scale = base_scale;
  if (!is_ground && source_has_alpha && (width >= 1024 || height >= 1024)) {
    scale = alpha_texture_downscale_factor();
  }

  if (level == 0 && tex) {
    policy = texture_policy_for(tex, 1);
    if (policy) {
      policy->scale = scale;
      policy->is_ground = is_ground;
    }
    if (scale != base_scale) {
      static int n = 0;
      if (n < 80) {
        fprintf(stderr,
                "[TEXSCALE] %s fmt=0x%x %dx%d alpha=%d ground=%d scale=%d "
                "base=%d tex=%u\n",
                tag ? tag : "?", fmt, width, height, source_has_alpha,
                is_ground, scale, base_scale, tex);
        n++;
      }
    }
  }

  return scale;
}

static unsigned char *downscale_rgba(const unsigned char *rgba, int w, int h,
                                     int factor, int *out_w, int *out_h) {
  int nw = w;
  int nh = h;
  if (factor > 1) {
    if (w > 1)
      nw = (w + factor - 1) / factor;
    if (h > 1)
      nh = (h + factor - 1) / factor;
  }
  if (out_w)
    *out_w = nw;
  if (out_h)
    *out_h = nh;
  if (nw == w && nh == h)
    return NULL;

  unsigned char *out = malloc((size_t)nw * (size_t)nh * 4);
  if (!out)
    return NULL;

  for (int y = 0; y < nh; y++) {
    int sy0 = y * factor;
    int sy1 = sy0 + factor;
    if (sy1 > h)
      sy1 = h;
    for (int x = 0; x < nw; x++) {
      int sx0 = x * factor;
      int sx1 = sx0 + factor;
      if (sx1 > w)
        sx1 = w;
      unsigned int r = 0, g = 0, b = 0, a = 0, n = 0;
      for (int sy = sy0; sy < sy1; sy++) {
        for (int sx = sx0; sx < sx1; sx++) {
          const unsigned char *p = rgba + ((size_t)sy * (size_t)w + sx) * 4;
          r += p[0];
          g += p[1];
          b += p[2];
          a += p[3];
          n++;
        }
      }
      unsigned char *d = out + ((size_t)y * (size_t)nw + x) * 4;
      d[0] = (unsigned char)(r / n);
      d[1] = (unsigned char)(g / n);
      d[2] = (unsigned char)(b / n);
      d[3] = (unsigned char)(a / n);
    }
  }
  return out;
}

static int is_pow2_u64(uint64_t v) { return v && !(v & (v - 1)); }

static uint64_t floor_pow2_u64(uint64_t v) {
  if (!v)
    return 0;
  uint64_t p = 1;
  while (p <= (UINT64_MAX >> 1) && (p << 1) <= v)
    p <<= 1;
  return p;
}

static int scaled_mip_dim(int dim, int level, int factor) {
  if (factor <= 1 || dim <= 1)
    return dim;
  int safe_level = level > 20 ? 20 : level;
  uint64_t base = (uint64_t)dim << safe_level;
  uint64_t down_base = (base + (uint64_t)factor - 1) / (uint64_t)factor;
  if (is_pow2_u64(base) && !is_pow2_u64(down_base)) {
    uint64_t pot = floor_pow2_u64(down_base);
    if (pot)
      down_base = pot;
  }
  uint64_t out = down_base >> safe_level;
  return out > 0 ? (int)out : 1;
}

static unsigned char *downscale_rgba_to(const unsigned char *rgba, int w, int h,
                                        int nw, int nh) {
  if (nw <= 0 || nh <= 0 || w <= 0 || h <= 0)
    return NULL;
  if (nw == w && nh == h)
    return NULL;

  unsigned char *out = malloc((size_t)nw * (size_t)nh * 4);
  if (!out)
    return NULL;

  for (int y = 0; y < nh; y++) {
    int sy0 = (int)(((int64_t)y * h) / nh);
    int sy1 = (int)(((int64_t)(y + 1) * h + nh - 1) / nh);
    if (sy1 <= sy0)
      sy1 = sy0 + 1;
    if (sy1 > h)
      sy1 = h;
    for (int x = 0; x < nw; x++) {
      int sx0 = (int)(((int64_t)x * w) / nw);
      int sx1 = (int)(((int64_t)(x + 1) * w + nw - 1) / nw);
      if (sx1 <= sx0)
        sx1 = sx0 + 1;
      if (sx1 > w)
        sx1 = w;
      unsigned int r = 0, g = 0, b = 0, a = 0, n = 0;
      for (int sy = sy0; sy < sy1; sy++) {
        for (int sx = sx0; sx < sx1; sx++) {
          const unsigned char *p = rgba + ((size_t)sy * (size_t)w + sx) * 4;
          r += p[0];
          g += p[1];
          b += p[2];
          a += p[3];
          n++;
        }
      }
      unsigned char *d = out + ((size_t)y * (size_t)nw + x) * 4;
      d[0] = (unsigned char)(r / n);
      d[1] = (unsigned char)(g / n);
      d[2] = (unsigned char)(b / n);
      d[3] = (unsigned char)(a / n);
    }
  }
  return out;
}

static unsigned char *etc1_encode_padded_rgba(const unsigned char *rgba, int w,
                                              int h, size_t *out_size) {
  int pw = (w + 3) & ~3;
  int ph = (h + 3) & ~3;
  size_t sz = (size_t)(pw / 4) * (size_t)(ph / 4) * 8;
  unsigned char *etc1 = malloc(sz);
  if (!etc1)
    return NULL;

  if (pw == w && ph == h) {
    etc1_encode_image(rgba, w, h, 4, etc1);
  } else {
    unsigned char *pad = malloc((size_t)pw * (size_t)ph * 4);
    if (!pad) {
      free(etc1);
      return NULL;
    }
    for (int y = 0; y < ph; y++) {
      int sy = y < h ? y : h - 1;
      for (int x = 0; x < pw; x++) {
        int sx = x < w ? x : w - 1;
        memcpy(pad + ((size_t)y * (size_t)pw + x) * 4,
               rgba + ((size_t)sy * (size_t)w + sx) * 4, 4);
      }
    }
    etc1_encode_image(pad, pw, ph, 4, etc1);
    free(pad);
  }

  if (out_size)
    *out_size = sz;
  return etc1;
}

static int upload_etc1_alpha_dual(GLenum target, GLint level, GLenum fmt,
                                  GLsizei width, GLsizei height, GLint border,
                                  const unsigned char *rgba, int upload_w,
                                  int upload_h, const char *path,
                                  int allow_dual,
                                  void (*real)(GLenum, GLint, GLenum, GLsizei,
                                               GLsizei, GLint, GLsizei,
                                               const void *)) {
  if (!real || !rgba || target != GL_TEXTURE_2D || upload_w <= 0 ||
      upload_h <= 0 || !allow_dual)
    return 0;

  GLuint base = g_bound_2d[g_active_tex_unit];
  if (!base)
    return 0;

  size_t color_size = 0, alpha_size = 0;
  unsigned char *color =
      etc1_encode_padded_rgba(rgba, upload_w, upload_h, &color_size);
  unsigned char *alpha_rgba = make_alpha_rgba(rgba, upload_w, upload_h);
  unsigned char *alpha = alpha_rgba
                             ? etc1_encode_padded_rgba(alpha_rgba, upload_w,
                                                       upload_h, &alpha_size)
                             : NULL;
  free(alpha_rgba);
  if (!color || !alpha) {
    free(color);
    free(alpha);
    return 0;
  }

  static void (*gen)(GLsizei, GLuint *) = NULL;
  static void (*bind)(GLenum, GLuint) = NULL;
  static void (*parami)(GLenum, GLenum, GLint) = NULL;
  static GLenum (*gerr)(void) = NULL;
  rgl("glGenTextures", (void **)&gen);
  rgl("glBindTexture", (void **)&bind);
  rgl("glTexParameteri", (void **)&parami);
  rgl("glGetError", (void **)&gerr);
  if (!gen || !bind) {
    free(color);
    free(alpha);
    return 0;
  }

  AlphaSidecar *sc = alpha_sidecar_for(base, 1);
  if (!sc) {
    free(color);
    free(alpha);
    return 0;
  }
  if (!sc->alpha) {
    gen(1, &sc->alpha);
    snprintf(sc->path, sizeof(sc->path), "%s", path ? path : "?");
    bind(GL_TEXTURE_2D, sc->alpha);
    if (parami) {
      parami(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      parami(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      parami(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
      parami(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    bind(GL_TEXTURE_2D, base);
  }

  while (gerr && gerr() != GL_NO_ERROR) {
  }

  bind(GL_TEXTURE_2D, sc->alpha);
  real(target, level, GL_ETC1_RGB8_OES, upload_w, upload_h, border,
       (GLsizei)alpha_size, alpha);
  GLenum alpha_err = gerr ? gerr() : 0;
  bind(GL_TEXTURE_2D, base);
  g_bound_2d[g_active_tex_unit] = base;

  if (alpha_err) {
    sc->enabled = 0;
    static int n = 0;
    if (n < 60) {
      fprintf(stderr,
              "[ETC1A] alpha upload err=0x%x %s fmt=0x%x %dx%d lvl=%d\n",
              alpha_err, path ? path : "?", fmt, upload_w, upload_h, level);
      n++;
    }
    free(color);
    free(alpha);
    return 0;
  }

  real(target, level, GL_ETC1_RGB8_OES, upload_w, upload_h, border,
       (GLsizei)color_size, color);
  GLenum color_err = gerr ? gerr() : 0;
  if (color_err) {
    sc->enabled = 0;
    static int n = 0;
    if (n < 60) {
      fprintf(stderr,
              "[ETC1A] color upload err=0x%x %s fmt=0x%x %dx%d lvl=%d\n",
              color_err, path ? path : "?", fmt, upload_w, upload_h, level);
      n++;
    }
    free(color);
    free(alpha);
    return 0;
  }

  sc->enabled = 1;
  size_t rgba4444_bytes = (size_t)upload_w * (size_t)upload_h * 2;
  size_t dual_bytes = color_size + alpha_size;
  static size_t saved_total = 0;
  if (rgba4444_bytes > dual_bytes)
    saved_total += rgba4444_bytes - dual_bytes;
  static int n = 0;
  if (n < 220) {
    fprintf(stderr,
            "[ETC1A] %s fmt=0x%x %dx%d->%dx%d lvl=%d base=%u alpha=%u "
            "dual=%zu rgba4444=%zu saved_total=%zu\n",
            path ? path : "?", fmt, width, height, upload_w, upload_h, level,
            base, sc->alpha, dual_bytes, rgba4444_bytes, saved_total);
    n++;
  }
  free(color);
  free(alpha);
  return 1;
}

static uint16_t rd16le(const unsigned char *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static unsigned char expand4(unsigned v);
static unsigned char expand5(unsigned v);
static unsigned char expand6(unsigned v);

static uint64_t rd64le(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--)
    v = (v << 8) | p[i];
  return v;
}

static void color565(uint16_t c, unsigned char out[3]) {
  out[0] = expand5((c >> 11) & 0x1f);
  out[1] = expand6((c >> 5) & 0x3f);
  out[2] = expand5(c & 0x1f);
}

static unsigned char *dxt_decode_rgba(unsigned fmt, int w, int h,
                                      const void *data, int size) {
  if (!data || w <= 0 || h <= 0)
    return NULL;
  int has_alpha_block =
      (fmt == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT ||
       fmt == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT);
  int block_size = has_alpha_block ? 16 : 8;
  int bw = (w + 3) / 4;
  int bh = (h + 3) / 4;
  if ((int64_t)bw * (int64_t)bh * block_size > size)
    return NULL;

  unsigned char *rgba = malloc((size_t)w * (size_t)h * 4);
  if (!rgba)
    return NULL;

  const unsigned char *src = (const unsigned char *)data;
  for (int by = 0; by < bh; by++) {
    for (int bx = 0; bx < bw; bx++) {
      const unsigned char *b = src + ((size_t)by * (size_t)bw + bx) * block_size;
      unsigned char alpha[16];
      for (int i = 0; i < 16; i++)
        alpha[i] = 255;

      int color_off = 0;
      if (fmt == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT) {
        for (int row = 0; row < 4; row++) {
          uint16_t av = rd16le(b + row * 2);
          for (int x = 0; x < 4; x++)
            alpha[row * 4 + x] = expand4((av >> (x * 4)) & 0xf);
        }
        color_off = 8;
      } else if (fmt == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT) {
        unsigned char a0 = b[0];
        unsigned char a1 = b[1];
        unsigned char atab[8];
        atab[0] = a0;
        atab[1] = a1;
        if (a0 > a1) {
          atab[2] = (unsigned char)((6 * a0 + 1 * a1) / 7);
          atab[3] = (unsigned char)((5 * a0 + 2 * a1) / 7);
          atab[4] = (unsigned char)((4 * a0 + 3 * a1) / 7);
          atab[5] = (unsigned char)((3 * a0 + 4 * a1) / 7);
          atab[6] = (unsigned char)((2 * a0 + 5 * a1) / 7);
          atab[7] = (unsigned char)((1 * a0 + 6 * a1) / 7);
        } else {
          atab[2] = (unsigned char)((4 * a0 + 1 * a1) / 5);
          atab[3] = (unsigned char)((3 * a0 + 2 * a1) / 5);
          atab[4] = (unsigned char)((2 * a0 + 3 * a1) / 5);
          atab[5] = (unsigned char)((1 * a0 + 4 * a1) / 5);
          atab[6] = 0;
          atab[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 5; i >= 0; i--)
          bits = (bits << 8) | b[2 + i];
        for (int i = 0; i < 16; i++) {
          alpha[i] = atab[bits & 7];
          bits >>= 3;
        }
        color_off = 8;
      }

      const unsigned char *cblk = b + color_off;
      uint16_t c0 = rd16le(cblk + 0);
      uint16_t c1 = rd16le(cblk + 2);
      unsigned char colors[4][4];
      color565(c0, colors[0]);
      color565(c1, colors[1]);
      colors[0][3] = 255;
      colors[1][3] = 255;
      if (c0 > c1 || fmt != GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) {
        for (int k = 0; k < 3; k++) {
          colors[2][k] = (unsigned char)((2 * colors[0][k] + colors[1][k]) / 3);
          colors[3][k] = (unsigned char)((colors[0][k] + 2 * colors[1][k]) / 3);
        }
        colors[2][3] = colors[3][3] = 255;
      } else {
        for (int k = 0; k < 3; k++) {
          colors[2][k] = (unsigned char)((colors[0][k] + colors[1][k]) / 2);
          colors[3][k] = 0;
        }
        colors[2][3] = 255;
        colors[3][3] = 0;
      }

      uint32_t idx = (uint32_t)(cblk[4] | (cblk[5] << 8) | (cblk[6] << 16) |
                                (cblk[7] << 24));
      for (int py = 0; py < 4; py++) {
        int y = by * 4 + py;
        if (y >= h)
          continue;
        for (int px = 0; px < 4; px++) {
          int x = bx * 4 + px;
          if (x >= w)
            continue;
          unsigned ci = idx & 3;
          idx >>= 2;
          unsigned char *d = rgba + ((size_t)y * (size_t)w + x) * 4;
          d[0] = colors[ci][0];
          d[1] = colors[ci][1];
          d[2] = colors[ci][2];
          d[3] = (fmt == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) ? colors[ci][3]
                                                           : alpha[py * 4 + px];
        }
      }
    }
  }
  return rgba;
}

static int upload_decoded_rgba(GLenum target, GLint level, GLenum fmt,
                               GLsizei width, GLsizei height, GLint border,
                               unsigned char *rgba, const char *tag,
                               void (*real)(GLenum, GLint, GLenum, GLsizei,
                                            GLsizei, GLint, GLsizei,
                                            const void *),
                               void (*tex2d)(GLenum, GLint, GLint, GLsizei,
                                             GLsizei, GLint, GLenum, GLenum,
                                             const void *)) {
  if (!rgba)
    return 0;
  if (!tex2d) {
    free(rgba);
    return 0;
  }

  int source_has_alpha = rgba_has_alpha(rgba, width, height);
  int scale = compressed_texture_downscale_factor(
      tag, fmt, level, width, height, 0, NULL, NULL, source_has_alpha);
  int upload_w = scaled_mip_dim(width, level, scale);
  int upload_h = scaled_mip_dim(height, level, scale);
  unsigned char *scaled =
      downscale_rgba_to(rgba, width, height, upload_w, upload_h);
  if (scaled) {
    free(rgba);
    rgba = scaled;
  }

  int has_alpha = rgba_has_alpha(rgba, upload_w, upload_h);
  if (!has_alpha && real) {
    size_t etc1_size = 0;
    unsigned char *etc1 =
        etc1_encode_padded_rgba(rgba, upload_w, upload_h, &etc1_size);
    if (etc1) {
      real(target, level, GL_ETC1_RGB8_OES, upload_w, upload_h, border,
           (GLsizei)etc1_size, etc1);
      free(etc1);
      static GLenum (*gerr)(void) = NULL;
      rgl("glGetError", (void **)&gerr);
      GLenum err = gerr ? gerr() : 0;
      if (!err) {
        free(rgba);
        static int ok_etc1 = 0;
        if (ok_etc1 < 220) {
          fprintf(stderr, "[%s] fmt=0x%x %dx%d->%dx%d lvl=%d -> ETC1\n",
                  tag, fmt, width, height, upload_w, upload_h, level);
          ok_etc1++;
        }
        return 1;
      }
      static int bad_etc1 = 0;
      if (bad_etc1 < 80) {
        fprintf(stderr, "[%s] ETC1 upload err=0x%x fmt=0x%x %dx%d lvl=%d\n",
                tag, err, fmt, upload_w, upload_h, level);
        bad_etc1++;
      }
    }
  }

  unsigned char *rgba4444 = pack_rgba4444(rgba, upload_w, upload_h);
  free(rgba);
  if (rgba4444) {
    tex2d(target, level, GL_RGBA, upload_w, upload_h, border, GL_RGBA,
          GL_UNSIGNED_SHORT_4_4_4_4, rgba4444);
    free(rgba4444);
    static int ok_4444 = 0;
    if (ok_4444 < 220) {
      fprintf(stderr, "[%s] fmt=0x%x %dx%d->%dx%d lvl=%d -> RGBA4444 alpha=%d\n",
              tag, fmt, width, height, upload_w, upload_h, level, has_alpha);
      ok_4444++;
    }
    return 1;
  }

  return 0;
}

static unsigned char expand4(unsigned v) { return (unsigned char)((v << 4) | v); }
static unsigned char expand5(unsigned v) { return (unsigned char)((v << 3) | (v >> 2)); }
static unsigned char expand6(unsigned v) { return (unsigned char)((v << 2) | (v >> 4)); }

static int unpack_alignment(void) {
  static void (*getiv)(GLenum, GLint *) = NULL;
  GLint a = 4;
  rgl("glGetIntegerv", (void **)&getiv);
  if (getiv)
    getiv(0x0CF5, &a);
  if (a != 1 && a != 2 && a != 4 && a != 8)
    a = 4;
  return a;
}

static unsigned char *unpack_teximage_rgba(GLenum format, GLenum type, int w,
                                           int h) {
  (void)format;
  (void)type;
  if (w <= 0 || h <= 0)
    return NULL;
  return malloc((size_t)w * (size_t)h * 4);
}

static unsigned char *copy_teximage_to_rgba(GLenum format, GLenum type, int w,
                                            int h, const void *pixels) {
  if (!pixels || w <= 0 || h <= 0)
    return NULL;

  unsigned char *rgba = unpack_teximage_rgba(format, type, w, h);
  if (!rgba)
    return NULL;

  int src_bpp = 0;
  if (type == GL_UNSIGNED_BYTE) {
    if (format == GL_RGBA)
      src_bpp = 4;
    else if (format == GL_RGB)
      src_bpp = 3;
    else if (format == GL_ALPHA || format == GL_LUMINANCE)
      src_bpp = 1;
    else if (format == GL_LUMINANCE_ALPHA)
      src_bpp = 2;
  } else if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
    src_bpp = 2;
  } else if (type == GL_UNSIGNED_SHORT_5_6_5) {
    src_bpp = 2;
  }
  if (!src_bpp) {
    free(rgba);
    return NULL;
  }

  int align = unpack_alignment();
  size_t row = (size_t)w * (size_t)src_bpp;
  size_t stride = (row + (size_t)align - 1) & ~((size_t)align - 1);
  const unsigned char *src = (const unsigned char *)pixels;

  for (int y = 0; y < h; y++) {
    const unsigned char *s = src + stride * (size_t)y;
    for (int x = 0; x < w; x++) {
      unsigned char *d = rgba + ((size_t)y * (size_t)w + x) * 4;
      if (type == GL_UNSIGNED_BYTE && format == GL_RGBA) {
        d[0] = s[x * 4 + 0];
        d[1] = s[x * 4 + 1];
        d[2] = s[x * 4 + 2];
        d[3] = s[x * 4 + 3];
      } else if (type == GL_UNSIGNED_BYTE && format == GL_RGB) {
        d[0] = s[x * 3 + 0];
        d[1] = s[x * 3 + 1];
        d[2] = s[x * 3 + 2];
        d[3] = 255;
      } else if (type == GL_UNSIGNED_BYTE && format == GL_ALPHA) {
        d[0] = 255;
        d[1] = 255;
        d[2] = 255;
        d[3] = s[x];
      } else if (type == GL_UNSIGNED_BYTE && format == GL_LUMINANCE) {
        d[0] = d[1] = d[2] = s[x];
        d[3] = 255;
      } else if (type == GL_UNSIGNED_BYTE && format == GL_LUMINANCE_ALPHA) {
        d[0] = d[1] = d[2] = s[x * 2 + 0];
        d[3] = s[x * 2 + 1];
      } else if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
        uint16_t v = (uint16_t)(s[x * 2 + 0] | (s[x * 2 + 1] << 8));
        d[0] = expand4((v >> 12) & 0xf);
        d[1] = expand4((v >> 8) & 0xf);
        d[2] = expand4((v >> 4) & 0xf);
        d[3] = expand4(v & 0xf);
      } else if (type == GL_UNSIGNED_SHORT_5_6_5) {
        uint16_t v = (uint16_t)(s[x * 2 + 0] | (s[x * 2 + 1] << 8));
        d[0] = expand5((v >> 11) & 0x1f);
        d[1] = expand6((v >> 5) & 0x3f);
        d[2] = expand5(v & 0x1f);
        d[3] = 255;
      }
    }
  }
  return rgba;
}

static void my_glTexImage2D(GLenum target, GLint level, GLint internalformat,
                            GLsizei width, GLsizei height, GLint border,
                            GLenum format, GLenum type, const void *pixels) {
  static void (*real)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                      GLenum, const void *) = NULL;
  rgl("glTexImage2D", (void **)&real);
  if (!real)
    return;

  int scale = texture_downscale_factor();
  if (pixels && scale > 1 && width > 1 && height > 1) {
    unsigned char *rgba =
        copy_teximage_to_rgba(format, type, width, height, pixels);
    if (rgba) {
      int upload_w = width;
      int upload_h = height;
      unsigned char *scaled =
          downscale_rgba(rgba, width, height, scale, &upload_w, &upload_h);
      if (scaled) {
        free(rgba);
        rgba = scaled;
      }

      int has_alpha = rgba_has_alpha(rgba, upload_w, upload_h);
      if (has_alpha) {
        unsigned char *rgba4444 = pack_rgba4444(rgba, upload_w, upload_h);
        free(rgba);
        if (rgba4444) {
          real(target, level, GL_RGBA, upload_w, upload_h, border, GL_RGBA,
               GL_UNSIGNED_SHORT_4_4_4_4, rgba4444);
          free(rgba4444);
          static int n = 0;
          if (n < 80) {
            fprintf(stderr,
                    "[TEX2D] fmt=0x%x type=0x%x %dx%d->%dx%d lvl=%d -> RGBA4444\n",
                    format, type, width, height, upload_w, upload_h, level);
            n++;
          }
          return;
        }
      } else {
        unsigned char *rgb565 = pack_rgb565(rgba, upload_w, upload_h);
        free(rgba);
        if (rgb565) {
          real(target, level, GL_RGB, upload_w, upload_h, border, GL_RGB,
               GL_UNSIGNED_SHORT_5_6_5, rgb565);
          free(rgb565);
          static int n = 0;
          if (n < 80) {
            fprintf(stderr,
                    "[TEX2D] fmt=0x%x type=0x%x %dx%d->%dx%d lvl=%d -> RGB565\n",
                    format, type, width, height, upload_w, upload_h, level);
            n++;
          }
          return;
        }
      }
    }
  }

  real(target, level, internalformat, width, height, border, format, type,
       pixels);
}

static void my_glCompressedTexImage2D(GLenum target, GLint level,
                                      GLenum internalformat, GLsizei width,
                                      GLsizei height, GLint border,
                                      GLsizei imageSize, const void *data) {
  static void (*real)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei,
                      const void *) = NULL;
  static void (*tex2d)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                       GLenum, const void *) = NULL;
  rgl("glCompressedTexImage2D", (void **)&real);
  GLenum fmt = internalformat;
  if (data && fmt >= GL_COMPRESSED_RGB_S3TC_DXT1_EXT &&
      fmt <= GL_COMPRESSED_RGBA_S3TC_DXT5_EXT) {
    rgl("glTexImage2D", (void **)&tex2d);
    unsigned char *rgba =
        dxt_decode_rgba((unsigned)fmt, width, height, data, imageSize);
    if (rgba) {
    if (upload_decoded_rgba(target, level, fmt, width, height, border, rgba,
                              "DXT", real, tex2d))
        return;
    }
    static int dxt_fail = 0;
    if (dxt_fail < 80) {
      fprintf(stderr, "[DXT] decode/upload falhou fmt=0x%x %dx%d lvl=%d size=%d\n",
              fmt, width, height, level, imageSize);
      dxt_fail++;
    }
  }
  if (data && fmt >= 0x9274 && fmt <= 0x9279) {
    rgl("glTexImage2D", (void **)&tex2d);
    char asset_path[1024] = {0};
    asset_path_for_ptr(data, imageSize > 0 ? (size_t)imageSize : 0,
                       asset_path, sizeof(asset_path));
    unsigned char *rgba =
        etc2_decode_rgba((unsigned)fmt, width, height, data, imageSize);
    if (rgba && tex2d) {
      int source_has_alpha = rgba_has_alpha(rgba, width, height);
      int scale = compressed_texture_downscale_factor(
          "ETC2", fmt, level, width, height, imageSize, data, asset_path,
          source_has_alpha);
      int upload_w = scaled_mip_dim(width, level, scale);
      int upload_h = scaled_mip_dim(height, level, scale);
      unsigned char *scaled =
          downscale_rgba_to(rgba, width, height, upload_w, upload_h);
      if (scaled) {
        free(rgba);
        rgba = scaled;
      }

      int has_alpha = rgba_has_alpha(rgba, upload_w, upload_h);
      if (!has_alpha && real) {
        size_t etc1_size = 0;
        unsigned char *etc1 =
            etc1_encode_padded_rgba(rgba, upload_w, upload_h, &etc1_size);
        if (etc1) {
          real(target, level, GL_ETC1_RGB8_OES, upload_w, upload_h, border,
               (GLsizei)etc1_size, etc1);
          free(etc1);
          static GLenum (*gerr)(void) = NULL;
          rgl("glGetError", (void **)&gerr);
          GLenum err = gerr ? gerr() : 0;
          if (!err) {
            free(rgba);
            static int ok_etc1 = 0;
            if (ok_etc1 < 160) {
              fprintf(stderr,
                      "[ETC2] fmt=0x%x %dx%d->%dx%d lvl=%d -> ETC1\n", fmt,
                      width, height, upload_w, upload_h, level);
              ok_etc1++;
            }
            return;
          }
          static int bad_etc1 = 0;
          if (bad_etc1 < 60) {
            fprintf(stderr,
                    "[ETC2] ETC1 upload err=0x%x fmt=0x%x %dx%d lvl=%d\n",
                    err, fmt, upload_w, upload_h, level);
            bad_etc1++;
          }
        }
      }
      int allow_dual = 0;
      const char *dual_label = asset_path[0] ? asset_path : NULL;
      GLuint base_tex = (g_active_tex_unit >= 0 &&
                         g_active_tex_unit < DS_MAX_TEX_UNITS)
                            ? g_bound_2d[g_active_tex_unit]
                            : 0;
      AlphaSidecar *sc = alpha_sidecar_for(base_tex, 0);
      if (sc && sc->force_dual) {
        allow_dual = 1;
        dual_label = sc->path[0] ? sc->path : "hash:ground-tile";
      } else if (asset_path[0] && dual_alpha_allowed_for_path(asset_path)) {
        allow_dual = 1;
        sc = alpha_sidecar_for(base_tex, 1);
        if (sc)
          sc->force_dual = 1;
      } else if (level == 0 &&
                 ground_tile_top_hash_matches(width, height, imageSize, data)) {
        allow_dual = 1;
        dual_label = "hash:ground-tile";
        sc = alpha_sidecar_for(base_tex, 1);
        if (sc) {
          sc->force_dual = 1;
          snprintf(sc->path, sizeof(sc->path), "%s", dual_label);
        }
      }

      if (has_alpha &&
          upload_etc1_alpha_dual(target, level, fmt, width, height, border,
                                 rgba, upload_w, upload_h, dual_label,
                                 allow_dual, real)) {
        free(rgba);
        return;
      }
      unsigned char *rgba4444 = pack_rgba4444(rgba, upload_w, upload_h);
      free(rgba);
      if (rgba4444) {
        tex2d(target, level, GL_RGBA, upload_w, upload_h, border, GL_RGBA,
              GL_UNSIGNED_SHORT_4_4_4_4, rgba4444);
        free(rgba4444);
        static int ok_4444 = 0;
        if (ok_4444 < 160) {
          fprintf(stderr,
                  "[ETC2] fmt=0x%x %dx%d->%dx%d lvl=%d -> RGBA4444 alpha=%d\n",
                  fmt, width, height, upload_w, upload_h, level, has_alpha);
          ok_4444++;
        }
        return;
      }
    }
    free(rgba);
    static int n = 0;
    if (n < 80) {
      fprintf(stderr,
              "[ETC2] decode falhou fmt=0x%x %dx%d lvl=%d size=%d, trying raw\n",
              fmt, width, height, level, imageSize);
      n++;
    }
  }
  if (!real)
    return;
  real(target, level, fmt, width, height, border, imageSize, data);
  static GLenum (*gerr)(void) = NULL;
  rgl("glGetError", (void **)&gerr);
  GLenum err = gerr ? gerr() : 0;
  if (err) {
    static int e = 0;
    if (e < 120) {
      fprintf(stderr, "[CTEX] err=0x%x fmt=0x%x %dx%d lvl=%d size=%d\n", err,
              internalformat, width, height, level, imageSize);
      e++;
    }
  }
}

static void my_glShaderSource(GLuint shader, GLsizei count,
                              const GLchar *const *string,
                              const GLint *length) {
  static void (*real)(GLuint, GLsizei, const GLchar *const *, const GLint *) =
      NULL;
  rgl("glShaderSource", (void **)&real);
  if (!real)
    return;

  size_t total = 0;
  for (GLsizei i = 0; i < count; i++) {
    if (!string || !string[i])
      continue;
    total += (length && length[i] >= 0) ? (size_t)length[i]
                                        : strlen(string[i]);
  }

  char *joined = NULL;
  if (total > 0 && total < 65536) {
    joined = malloc(total + 1);
    if (joined) {
      size_t off = 0;
      for (GLsizei i = 0; i < count; i++) {
        if (!string || !string[i])
          continue;
        size_t n = (length && length[i] >= 0) ? (size_t)length[i]
                                              : strlen(string[i]);
        memcpy(joined + off, string[i], n);
        off += n;
      }
      joined[off] = 0;
    }
  }

  static const char ground_fix[] =
      "#if defined(GL_ES)\n"
      "precision highp float;\n"
      "#endif\n"
      "uniform sampler2D SAMPLER[4];\n"
      "#define BASE_TEXTURE SAMPLER[0]\n"
      "#define NOISE_TEXTURE SAMPLER[1]\n"
      "#define MULTILAYER_TEXTURE SAMPLER[2]\n"
      "uniform float NOISE_REPEAT_SIZE;\n"
      "uniform vec3 BLEND_FACTOR;\n"
      "uniform vec4 GROUND_COL0;\n"
      "uniform vec4 GROUND_COL1;\n"
      "uniform vec4 GROUND_COL2;\n"
      "uniform vec3 AMBIENT;\n"
      "uniform vec4 LIGHTMAP_WORLD_EXTENTS;\n"
      "varying vec2 PS_TEXCOORD;\n"
      "varying vec3 PS_POS;\n"
      "void main(){\n"
      "  vec4 base_colour = texture2D(BASE_TEXTURE, PS_TEXCOORD);\n"
      "  vec2 noise_uv = PS_POS.xz / max(NOISE_REPEAT_SIZE, 1.0);\n"
      "  vec4 noise = texture2D(NOISE_TEXTURE, noise_uv);\n"
      "  vec3 n = max(noise.rgb, vec3(0.62));\n"
      "  vec3 c = base_colour.rgb;\n"
      "  if (dot(c, vec3(1.0)) < 0.035) c = vec3(0.24, 0.22, 0.15);\n"
      "  vec3 layers = texture2D(MULTILAYER_TEXTURE, noise_uv).rgb * BLEND_FACTOR;\n"
      "  c = mix(c, GROUND_COL0.rgb, clamp(layers.r * GROUND_COL0.a, 0.0, 1.0));\n"
      "  c = mix(c, GROUND_COL1.rgb, clamp(layers.g * GROUND_COL1.a, 0.0, 1.0));\n"
      "  c = mix(c, GROUND_COL2.rgb, clamp(layers.b * GROUND_COL2.a, 0.0, 1.0));\n"
      "  c *= n;\n"
      "  float a = clamp(base_colour.a, 0.0, 1.0);\n"
      "  gl_FragColor = vec4(c * a, a);\n"
      "}\n";

  static const char ground_fix_alpha[] =
      "#if defined(GL_ES)\n"
      "precision highp float;\n"
      "#endif\n"
      "uniform sampler2D SAMPLER[4];\n"
      "uniform sampler2D DS_ALPHA0;\n"
      "uniform float DS_ALPHA0_ENABLED;\n"
      "#define BASE_TEXTURE SAMPLER[0]\n"
      "#define NOISE_TEXTURE SAMPLER[1]\n"
      "#define MULTILAYER_TEXTURE SAMPLER[2]\n"
      "uniform float NOISE_REPEAT_SIZE;\n"
      "uniform vec3 BLEND_FACTOR;\n"
      "uniform vec4 GROUND_COL0;\n"
      "uniform vec4 GROUND_COL1;\n"
      "uniform vec4 GROUND_COL2;\n"
      "uniform vec3 AMBIENT;\n"
      "uniform vec4 LIGHTMAP_WORLD_EXTENTS;\n"
      "varying vec2 PS_TEXCOORD;\n"
      "varying vec3 PS_POS;\n"
      "void main(){\n"
      "  vec4 base_colour = texture2D(BASE_TEXTURE, PS_TEXCOORD);\n"
      "  vec2 noise_uv = PS_POS.xz / max(NOISE_REPEAT_SIZE, 1.0);\n"
      "  vec4 noise = texture2D(NOISE_TEXTURE, noise_uv);\n"
      "  vec3 n = max(noise.rgb, vec3(0.62));\n"
      "  vec3 c = base_colour.rgb;\n"
      "  if (dot(c, vec3(1.0)) < 0.035) c = vec3(0.24, 0.22, 0.15);\n"
      "  vec3 layers = texture2D(MULTILAYER_TEXTURE, noise_uv).rgb * BLEND_FACTOR;\n"
      "  c = mix(c, GROUND_COL0.rgb, clamp(layers.r * GROUND_COL0.a, 0.0, 1.0));\n"
      "  c = mix(c, GROUND_COL1.rgb, clamp(layers.g * GROUND_COL1.a, 0.0, 1.0));\n"
      "  c = mix(c, GROUND_COL2.rgb, clamp(layers.b * GROUND_COL2.a, 0.0, 1.0));\n"
      "  c *= n;\n"
      "  float side_a = texture2D(DS_ALPHA0, PS_TEXCOORD).r;\n"
      "  float a = clamp(mix(base_colour.a, min(base_colour.a, side_a), DS_ALPHA0_ENABLED), 0.0, 1.0);\n"
      "  gl_FragColor = vec4(c * a, a);\n"
      "}\n";

  static const char road_fix[] =
      "#if defined(GL_ES)\n"
      "precision highp float;\n"
      "#endif\n"
      "uniform sampler2D SAMPLER[4];\n"
      "#define BASE_TEXTURE SAMPLER[0]\n"
      "#define NOISE_TEXTURE SAMPLER[1]\n"
      "#define MULTILAYER_TEXTURE SAMPLER[2]\n"
      "uniform vec2 GROUND_REPEAT_VEC;\n"
      "uniform vec3 BLEND_FACTOR;\n"
      "uniform vec4 GROUND_COL0;\n"
      "uniform vec4 GROUND_COL1;\n"
      "uniform vec4 GROUND_COL2;\n"
      "uniform vec3 AMBIENT;\n"
      "uniform vec4 LIGHTMAP_WORLD_EXTENTS;\n"
      "varying vec2 PS_TEXCOORD;\n"
      "varying vec3 PS_POS;\n"
      "void main(){\n"
      "  vec2 noise_uv = PS_POS.xz / max(GROUND_REPEAT_VEC.x, 1.0);\n"
      "  vec2 world_noise_uv = PS_POS.xz / max(GROUND_REPEAT_VEC.y, 1.0);\n"
      "  vec4 base_colour = texture2D(BASE_TEXTURE, PS_TEXCOORD);\n"
      "  vec4 noise = texture2D(NOISE_TEXTURE, noise_uv);\n"
      "  vec3 c = base_colour.rgb / max(base_colour.a, 0.25);\n"
      "  if (dot(c, vec3(1.0)) < 0.035) c = vec3(0.28, 0.24, 0.14);\n"
      "  vec3 layers = texture2D(MULTILAYER_TEXTURE, world_noise_uv).rgb * BLEND_FACTOR;\n"
      "  c = mix(c, GROUND_COL0.rgb, clamp(layers.r * GROUND_COL0.a, 0.0, 1.0));\n"
      "  c = mix(c, GROUND_COL1.rgb, clamp(layers.g * GROUND_COL1.a, 0.0, 1.0));\n"
      "  c = mix(c, GROUND_COL2.rgb, clamp(layers.b * GROUND_COL2.a, 0.0, 1.0));\n"
      "  c *= max(noise.rgb, vec3(0.62));\n"
      "  float a = clamp(noise.a * base_colour.a, 0.0, 1.0);\n"
      "  gl_FragColor = vec4(c * a, a);\n"
      "}\n";

  if (joined && strstr(joined, "uniform vec3 BLEND_FACTOR;") &&
      strstr(joined, "uniform float NOISE_REPEAT_SIZE;") &&
      strstr(joined, "base_colour.rgb *= noise.rgb;")) {
    const GLchar *fixed =
        dual_alpha_mode_requested() ? ground_fix_alpha : ground_fix;
    real(shader, 1, &fixed, NULL);
    static int n = 0;
    if (n < 8) {
      fprintf(stderr, "[shader] ground fix applied%s\n",
              dual_alpha_mode_requested() ? " +etc1a" : "");
      n++;
    }
    free(joined);
    return;
  }

  if (joined && strstr(joined, "uniform vec2 GROUND_REPEAT_VEC;") &&
      strstr(joined, "base_colour.rgb /= base_colour.a;")) {
    const GLchar *fixed = road_fix;
    real(shader, 1, &fixed, NULL);
    static int n = 0;
    if (n < 8) {
      fprintf(stderr, "[shader] road fix applied\n");
      n++;
    }
    free(joined);
    return;
  }

  free(joined);
  real(shader, count, string, length);
}

static void my_glCompileShader(GLuint shader) {
  static void (*real)(GLuint) = NULL;
  static void (*getiv)(GLuint, GLenum, GLint *) = NULL;
  static void (*logfn)(GLuint, GLsizei, GLsizei *, GLchar *) = NULL;
  rgl("glCompileShader", (void **)&real);
  rgl("glGetShaderiv", (void **)&getiv);
  rgl("glGetShaderInfoLog", (void **)&logfn);
  if (real)
    real(shader);
  GLint ok = 1;
  if (getiv)
    getiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok && logfn) {
    char log[2048];
    GLsizei len = 0;
    logfn(shader, sizeof(log) - 1, &len, log);
    log[len] = 0;
    fprintf(stderr, "[shader compile fail] %s\n", log);
  }
}

static void my_glLinkProgram(GLuint program) {
  static void (*real)(GLuint) = NULL;
  static void (*getiv)(GLuint, GLenum, GLint *) = NULL;
  static void (*logfn)(GLuint, GLsizei, GLsizei *, GLchar *) = NULL;
  rgl("glLinkProgram", (void **)&real);
  rgl("glGetProgramiv", (void **)&getiv);
  rgl("glGetProgramInfoLog", (void **)&logfn);
  if (real)
    real(program);
  GLint ok = 1;
  if (getiv)
    getiv(program, GL_LINK_STATUS, &ok);
  if (!ok && logfn) {
    char log[2048];
    GLsizei len = 0;
    logfn(program, sizeof(log) - 1, &len, log);
    log[len] = 0;
    fprintf(stderr, "[program link fail] %s\n", log);
  }
}

static void my_glActiveTexture(GLenum texture) {
  static void (*real)(GLenum) = NULL;
  rgl("glActiveTexture", (void **)&real);
  if (real)
    real(texture);
  int unit = tex_unit_index(texture);
  if (unit >= 0)
    g_active_tex_unit = unit;
}

static void my_glBindTexture(GLenum target, GLuint texture) {
  static void (*real)(GLenum, GLuint) = NULL;
  rgl("glBindTexture", (void **)&real);
  if (real)
    real(target, texture);
  if (target == GL_TEXTURE_2D && g_active_tex_unit >= 0 &&
      g_active_tex_unit < DS_MAX_TEX_UNITS)
    g_bound_2d[g_active_tex_unit] = texture;
}

static void my_glTexParameteri(GLenum target, GLenum pname, GLint param) {
  static void (*real)(GLenum, GLenum, GLint) = NULL;
  static void (*bind)(GLenum, GLuint) = NULL;
  rgl("glTexParameteri", (void **)&real);
  if (!real)
    return;
  real(target, pname, param);

  if (target != GL_TEXTURE_2D)
    return;
  GLuint base = g_bound_2d[g_active_tex_unit];
  GLuint alpha = enabled_alpha_for(base);
  if (!alpha)
    return;
  rgl("glBindTexture", (void **)&bind);
  if (!bind)
    return;
  bind(GL_TEXTURE_2D, alpha);
  real(target, pname, param);
  bind(GL_TEXTURE_2D, base);
  g_bound_2d[g_active_tex_unit] = base;
}

static void my_glUseProgram(GLuint program) {
  static void (*real)(GLuint) = NULL;
  rgl("glUseProgram", (void **)&real);
  if (real)
    real(program);
  g_current_program = program;
  if (program)
    ensure_alpha_program_uniform(program);
}

static void my_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
  static void (*real)(GLenum, GLint, GLsizei) = NULL;
  rgl("glDrawArrays", (void **)&real);
  if (!real)
    return;
  int prev_unit = g_active_tex_unit;
  GLuint prev_alpha = g_bound_2d[DS_ALPHA_UNIT];
  int did = bind_alpha_for_draw(&prev_unit, &prev_alpha);
  real(mode, first, count);
  if (did)
    restore_alpha_after_draw(prev_unit, prev_alpha);
}

static void my_glDrawElements(GLenum mode, GLsizei count, GLenum type,
                              const void *indices) {
  static void (*real)(GLenum, GLsizei, GLenum, const void *) = NULL;
  rgl("glDrawElements", (void **)&real);
  if (!real)
    return;
  int prev_unit = g_active_tex_unit;
  GLuint prev_alpha = g_bound_2d[DS_ALPHA_UNIT];
  int did = bind_alpha_for_draw(&prev_unit, &prev_alpha);
  real(mode, count, type, indices);
  if (did)
    restore_alpha_after_draw(prev_unit, prev_alpha);
}

static void my_glDeleteTextures(GLsizei n, const GLuint *textures) {
  static void (*real)(GLsizei, const GLuint *) = NULL;
  rgl("glDeleteTextures", (void **)&real);
  if (!real)
    return;
  if (textures) {
    for (GLsizei i = 0; i < n; i++) {
      GLuint tex = textures[i];
      for (int u = 0; u < DS_MAX_TEX_UNITS; u++) {
        if (g_bound_2d[u] == tex)
          g_bound_2d[u] = 0;
      }
      AlphaSidecar *sc = alpha_sidecar_for(tex, 0);
      if (sc && sc->alpha) {
        GLuint alpha = sc->alpha;
        real(1, &alpha);
        memset(sc, 0, sizeof(*sc));
      }
      TexturePolicy *tp = texture_policy_for(tex, 0);
      if (tp)
        memset(tp, 0, sizeof(*tp));
    }
  }
  real(n, textures);
}

void *dysmantle_gl_proc_override(const char *name) {
  if (!name)
    return NULL;
  if (!strcmp(name, "glActiveTexture"))
    return (void *)my_glActiveTexture;
  if (!strcmp(name, "glBindTexture"))
    return (void *)my_glBindTexture;
  if (!strcmp(name, "glGetString"))
    return (void *)my_glGetString;
  if (!strcmp(name, "glGetIntegerv"))
    return (void *)my_glGetIntegerv;
  if (!strcmp(name, "glCompressedTexImage2D"))
    return (void *)my_glCompressedTexImage2D;
  if (!strcmp(name, "glTexParameteri"))
    return (void *)my_glTexParameteri;
  if (!strcmp(name, "glShaderSource"))
    return (void *)my_glShaderSource;
  if (!strcmp(name, "glCompileShader"))
    return (void *)my_glCompileShader;
  if (!strcmp(name, "glLinkProgram"))
    return (void *)my_glLinkProgram;
  if (!strcmp(name, "glUseProgram"))
    return (void *)my_glUseProgram;
  if (!strcmp(name, "glDrawArrays"))
    return (void *)my_glDrawArrays;
  if (!strcmp(name, "glDrawElements"))
    return (void *)my_glDrawElements;
  if (!strcmp(name, "glDeleteTextures"))
    return (void *)my_glDeleteTextures;
  return NULL;
}

/* ---------------- import table ---------------- */
DynLibFunction dontstarve_overrides[] = {
    {"__android_log_print", (uintptr_t)&b_log_print},
    {"__android_log_write", (uintptr_t)&b_log_write},
    {"__android_log_assert", (uintptr_t)&b_log_assert},
    {"__assert2", (uintptr_t)&b_assert2},
    {"android_set_abort_message", (uintptr_t)&b_set_abort_message},
    {"raise", (uintptr_t)&w_raise},
    {"abort", (uintptr_t)&w_abort},
    {"_ZNSt6__ndk122__libcpp_verbose_abortEPKcz",
     (uintptr_t)&b_libcpp_verbose_abort},
    {"__errno", (uintptr_t)&b_errno},
    {"__system_property_get", (uintptr_t)&b_property_get},
    {"gettid", (uintptr_t)&b_gettid},
    {"memfd_create", (uintptr_t)&b_memfd_create},
    {"__strlen_chk", (uintptr_t)&b_strlen_chk},
    {"__memcpy_chk", (uintptr_t)&b_memcpy_chk},
    {"__memmove_chk", (uintptr_t)&b_memmove_chk},
    {"__memset_chk", (uintptr_t)&b_memset_chk},
    {"__strcpy_chk", (uintptr_t)&b_strcpy_chk},
    {"__strncpy_chk", (uintptr_t)&b_strncpy_chk},
    {"__strncpy_chk2", (uintptr_t)&b_strncpy_chk2},
    {"__strcat_chk", (uintptr_t)&b_strcat_chk},
    {"__strncat_chk", (uintptr_t)&b_strncat_chk},
    {"__strchr_chk", (uintptr_t)&b_strchr_chk},
    {"__read_chk", (uintptr_t)&b_read_chk},
    {"__write_chk", (uintptr_t)&b_write_chk},
    {"__FD_SET_chk", (uintptr_t)&b_FD_SET_chk},
    {"__open_2", (uintptr_t)&w_open_2},
    {"setjmp", (uintptr_t)&_setjmp},
    {"longjmp", (uintptr_t)&_longjmp},
    {"__cxa_guard_acquire", (uintptr_t)&b_cxa_guard_acquire},
    {"__cxa_guard_release", (uintptr_t)&b_cxa_guard_release},
    {"__cxa_guard_abort", (uintptr_t)&b_cxa_guard_abort},
    {"__cxa_pure_virtual", (uintptr_t)&b_cxa_pure_virtual},
    {"__sF", (uintptr_t)&bionic_sF},

    {"fopen", (uintptr_t)&w_fopen},
    {"freopen", (uintptr_t)&w_freopen},
    {"fclose", (uintptr_t)&w_fclose},
    {"fread", (uintptr_t)&w_fread},
    {"fwrite", (uintptr_t)&w_fwrite},
    {"fseek", (uintptr_t)&w_fseek},
    {"fseeko", (uintptr_t)&w_fseeko},
    {"ftell", (uintptr_t)&w_ftell},
    {"ftello", (uintptr_t)&w_ftello},
    {"feof", (uintptr_t)&w_feof},
    {"ferror", (uintptr_t)&w_ferror},
    {"clearerr", (uintptr_t)&w_clearerr},
    {"fflush", (uintptr_t)&w_fflush},
    {"fgets", (uintptr_t)&w_fgets},
    {"getc", (uintptr_t)&w_getc},
    {"ungetc", (uintptr_t)&w_ungetc},
    {"fputc", (uintptr_t)&w_fputc},
    {"fputs", (uintptr_t)&w_fputs},
    {"fprintf", (uintptr_t)&w_fprintf},
    {"vfprintf", (uintptr_t)&w_vfprintf},
    {"open", (uintptr_t)&w_open},
    {"stat", (uintptr_t)&w_stat},
    {"opendir", (uintptr_t)&w_opendir},
    {"readdir", (uintptr_t)&w_readdir},
    {"closedir", (uintptr_t)&w_closedir},
    {"mkdir", (uintptr_t)&w_mkdir},
    {"remove", (uintptr_t)&w_remove_path},
    {"unlink", (uintptr_t)&w_unlink_path},
    {"rename", (uintptr_t)&w_rename_path},

    {"dlopen", (uintptr_t)&w_dlopen},
    {"dlsym", (uintptr_t)&w_dlsym},
    {"dlclose", (uintptr_t)&w_dlclose},
    {"slCreateEngine", (uintptr_t)&slCreateEngine_shim},
    {"SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE_v},
    {"SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY_v},
    {"SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME_v},
    {"SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE_v},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE",
     (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE_v},
    {"SL_IID_RECORD", (uintptr_t)&SL_IID_RECORD_v},
    {"SL_IID_ANDROIDCONFIGURATION",
     (uintptr_t)&SL_IID_ANDROIDCONFIGURATION_v},
    {"SL_IID_ENVIRONMENTALREVERB", (uintptr_t)&SL_IID_ENVIRONMENTALREVERB_v},

    {"pthread_attr_init", (uintptr_t)&b_pthread_attr_init},
    {"pthread_attr_destroy", (uintptr_t)&b_pthread_attr_destroy},
    {"pthread_attr_setdetachstate", (uintptr_t)&b_pthread_attr_setdetachstate},
    {"pthread_attr_setstacksize", (uintptr_t)&b_pthread_attr_setstacksize},
    {"pthread_attr_setschedparam", (uintptr_t)&b_pthread_attr_setschedparam},
    {"pthread_attr_setschedpolicy", (uintptr_t)&b_pthread_attr_setschedpolicy},
    {"pthread_create", (uintptr_t)&b_pthread_create},
    {"pthread_setname_np", (uintptr_t)&b_pthread_setname_np},
    {"pthread_setschedparam", (uintptr_t)&b_pthread_setschedparam},
    {"sem_init", (uintptr_t)&b_sem_init},
    {"sem_wait", (uintptr_t)&b_sem_wait},
    {"sem_post", (uintptr_t)&b_sem_post},
    {"sem_destroy", (uintptr_t)&b_sem_destroy},

    {"AAssetManager_fromJava", (uintptr_t)&aam_fromJava},
    {"AAssetManager_open", (uintptr_t)&aam_open},
    {"AAsset_read", (uintptr_t)&aa_read},
    {"AAsset_seek", (uintptr_t)&aa_seek},
    {"AAsset_getLength", (uintptr_t)&aa_getLength},
    {"AAsset_getBuffer", (uintptr_t)&aa_getBuffer},
    {"AAsset_close", (uintptr_t)&aa_close},
    {"ANativeWindow_getWidth", (uintptr_t)&aw_getWidth},
    {"ANativeWindow_getHeight", (uintptr_t)&aw_getHeight},
    {"ANativeWindow_setBuffersGeometry", (uintptr_t)&aw_setBuffersGeometry},
    {"ALooper_pollOnce", (uintptr_t)&al_pollOnce},

    {"AConfiguration_new", (uintptr_t)&AConfiguration_new},
    {"AConfiguration_delete", (uintptr_t)&AConfiguration_delete},
    {"AConfiguration_fromAssetManager",
     (uintptr_t)&AConfiguration_fromAssetManager},
    {"AConfiguration_getLanguage", (uintptr_t)&AConfiguration_getLanguage},
    {"AConfiguration_getCountry", (uintptr_t)&AConfiguration_getCountry},
    {"ALooper_prepare", (uintptr_t)&ALooper_prepare},
    {"ALooper_addFd", (uintptr_t)&ALooper_addFd},
    {"ALooper_pollAll", (uintptr_t)&ALooper_pollAll},
    {"AInputQueue_attachLooper", (uintptr_t)&AInputQueue_attachLooper},
    {"AInputQueue_detachLooper", (uintptr_t)&AInputQueue_detachLooper},
    {"AInputQueue_getEvent", (uintptr_t)&AInputQueue_getEvent},
    {"AInputQueue_preDispatchEvent", (uintptr_t)&AInputQueue_preDispatchEvent},
    {"AInputQueue_finishEvent", (uintptr_t)&AInputQueue_finishEvent},
    {"AInputEvent_getType", (uintptr_t)&AInputEvent_getType},
    {"AInputEvent_getSource", (uintptr_t)&AInputEvent_getSource},
    {"AInputEvent_getDeviceId", (uintptr_t)&AInputEvent_getDeviceId},
    {"AKeyEvent_getAction", (uintptr_t)&AKeyEvent_getAction},
    {"AKeyEvent_getKeyCode", (uintptr_t)&AKeyEvent_getKeyCode},
    {"AKeyEvent_getMetaState", (uintptr_t)&AKeyEvent_getMetaState},
    {"AKeyEvent_getRepeatCount", (uintptr_t)&AKeyEvent_getRepeatCount},
    {"AKeyEvent_getScanCode", (uintptr_t)&AKeyEvent_getScanCode},
    {"AMotionEvent_getAction", (uintptr_t)&AMotionEvent_getAction},
    {"AMotionEvent_getAxisValue", (uintptr_t)&AMotionEvent_getAxisValue},
    {"AMotionEvent_getEventTime", (uintptr_t)&AMotionEvent_getEventTime},
    {"AMotionEvent_getPointerCount", (uintptr_t)&AMotionEvent_getPointerCount},
    {"AMotionEvent_getPointerId", (uintptr_t)&AMotionEvent_getPointerId},
    {"AMotionEvent_getX", (uintptr_t)&AMotionEvent_getX},
    {"AMotionEvent_getY", (uintptr_t)&AMotionEvent_getY},

    {"eglGetDisplay", (uintptr_t)&egl_shim_GetDisplay},
    {"eglInitialize", (uintptr_t)&egl_shim_Initialize},
    {"eglTerminate", (uintptr_t)&egl_shim_Terminate},
    {"eglChooseConfig", (uintptr_t)&egl_shim_ChooseConfig},
    {"eglCreateWindowSurface", (uintptr_t)&egl_shim_CreateWindowSurface},
    {"eglCreateContext", (uintptr_t)&egl_shim_CreateContext},
    {"eglMakeCurrent", (uintptr_t)&egl_shim_MakeCurrent},
    {"eglSwapBuffers", (uintptr_t)&egl_shim_SwapBuffers},
    {"eglDestroySurface", (uintptr_t)&egl_shim_DestroySurface},
    {"eglDestroyContext", (uintptr_t)&egl_shim_DestroyContext},
    {"eglQuerySurface", (uintptr_t)&egl_shim_QuerySurface},
    {"eglGetConfigAttrib", (uintptr_t)&egl_shim_GetConfigAttrib},
    {"eglGetCurrentSurface", (uintptr_t)&egl_shim_GetCurrentSurface},
    {"eglGetProcAddress", (uintptr_t)&egl_shim_GetProcAddress},
    {"eglGetError", (uintptr_t)&egl_shim_GetError},
    {"eglSwapInterval", (uintptr_t)&egl_shim_SwapInterval},
    {"eglBindAPI", (uintptr_t)&egl_shim_BindAPI},
    {"eglQueryString", (uintptr_t)&egl_shim_QueryString},

    {"glActiveTexture", (uintptr_t)&my_glActiveTexture},
    {"glBindTexture", (uintptr_t)&my_glBindTexture},
    {"glGetString", (uintptr_t)&my_glGetString},
    {"glGetIntegerv", (uintptr_t)&my_glGetIntegerv},
    {"glCompressedTexImage2D", (uintptr_t)&my_glCompressedTexImage2D},
    {"glTexParameteri", (uintptr_t)&my_glTexParameteri},
    {"glShaderSource", (uintptr_t)&my_glShaderSource},
    {"glCompileShader", (uintptr_t)&my_glCompileShader},
    {"glLinkProgram", (uintptr_t)&my_glLinkProgram},
    {"glUseProgram", (uintptr_t)&my_glUseProgram},
    {"glDrawArrays", (uintptr_t)&my_glDrawArrays},
    {"glDrawElements", (uintptr_t)&my_glDrawElements},
    {"glDeleteTextures", (uintptr_t)&my_glDeleteTextures},
};

const int dontstarve_overrides_count =
    sizeof(dontstarve_overrides) / sizeof(dontstarve_overrides[0]);
