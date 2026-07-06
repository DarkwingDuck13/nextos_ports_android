/*
 * imports.c — shims bionic→glibc do NFS (os 18 imports que o dlsym fallback
 * não cobre: __sF/__errno/__assert2/__android_log/_ctype_/_tolower_tab_/
 * __cxa_type_match/__dso_handle/sigsetjmp/AndroidBitmap_*).
 * Exporta port_shims[] — main.c usa como base da tabela combinada.
 */
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "so_util.h"
#include "opensles_shim.h"

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef int GLint;

static void print_trace_caller(uintptr_t ret);

/* ---- bionic __sF[3] = stdin/out/err (libc++ usa p/ std::cerr/cout) ---- */
#define BIONIC_FILE_SZ 0x54
extern unsigned char __sF[];
static char bionic_sF[3][0x54];
static FILE *map_sF(void *fp) {
  if (fp == (void *)&bionic_sF[0]) return stdin;
  if (fp == (void *)&bionic_sF[1]) return stdout;
  if (fp == (void *)&bionic_sF[2]) return stderr;
  uintptr_t p = (uintptr_t)fp, base = (uintptr_t)__sF;
  if (p >= base && p < base + BIONIC_FILE_SZ * 3) {
    int idx = (int)((p - base) / BIONIC_FILE_SZ);
    if (idx == 0) return stdin;
    if (idx == 1) return stdout;
    return stderr;
  }
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

/* ---- errno / assert / log ---- */
static int *b_errno(void) { extern int *__errno_location(void); return __errno_location(); }
static void b_assert2(const char *f, int l, const char *fn, const char *m) {
  fprintf(stderr, "[assert] %s:%d %s: %s\n", f ? f : "?", l, fn ? fn : "?", m ? m : "?");
  abort();
}
static int b_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[%s] ", tag ? tag : "nfs"); vfprintf(stderr, fmt, ap);
  if (getenv("TASM2_LOG_CALLER") ||
      (tag && (strcmp(tag, "HEI") == 0 || strcmp(tag, "GLOTv3") == 0 ||
               strcmp(tag, "GameInstaller") == 0))) {
    fprintf(stderr, " ");
    print_trace_caller(ret);
  }
  fprintf(stderr, "\n"); va_end(ap); return 0;
}
static int b_log_write(int prio, const char *tag, const char *msg) {
  (void)prio;
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  fprintf(stderr, "[%s] %s", tag ? tag : "nfs", msg ? msg : "");
  if (getenv("TASM2_LOG_CALLER") ||
      (tag && (strcmp(tag, "HEI") == 0 || strcmp(tag, "GLOTv3") == 0 ||
               strcmp(tag, "GameInstaller") == 0))) {
    fprintf(stderr, " ");
    print_trace_caller(ret);
  }
  fprintf(stderr, "\n");
  return 0;
}
static int b_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  (void)prio;
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  fprintf(stderr, "[%s] ", tag ? tag : "tasm2");
  vfprintf(stderr, fmt ? fmt : "", ap);
  if (getenv("TASM2_LOG_CALLER") ||
      (tag && (strcmp(tag, "HEI") == 0 || strcmp(tag, "GLOTv3") == 0 ||
               strcmp(tag, "GameInstaller") == 0))) {
    fprintf(stderr, " ");
    print_trace_caller(ret);
  }
  fprintf(stderr, "\n");
  return 0;
}
static void b_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
  (void)cond; va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[%s ASSERT] ", tag ? tag : "nfs"); vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n"); va_end(ap); abort();
}

static void log_caller_offset(const char *tag, uintptr_t ret) {
  uintptr_t ret_arm = ret & ~(uintptr_t)1;
  uintptr_t text = (uintptr_t)text_base;
  fprintf(stderr, "%s caller=%p", tag, (void *)ret);
  if (ret_arm >= text && ret_arm < text + text_size)
    fprintf(stderr, " (libtasm2.so+0x%lx)",
            (unsigned long)(ret_arm - text));
  fprintf(stderr, "\n");
}

static int caller_is_game(uintptr_t ret) {
  uintptr_t ret_arm = ret & ~(uintptr_t)1;
  uintptr_t text = (uintptr_t)text_base;
  return ret_arm >= text && ret_arm < text + text_size;
}

static int ignore_game_sigsegv(const char *tag, int sig, uintptr_t ret) {
  if (sig != SIGSEGV || !caller_is_game(ret) || getenv("TASM2_ALLOW_GAME_SIGSEGV"))
    return 0;
  log_caller_offset(tag, ret);
  fprintf(stderr, "%s ignored game-requested SIGSEGV\n", tag);
  return 1;
}

static int b_raise(int sig) {
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  char tag[64];
  snprintf(tag, sizeof(tag), "[raise] sig=%d", sig);
  log_caller_offset(tag, ret);
  if (ignore_game_sigsegv(tag, sig, ret))
    return 0;
  if (getenv("TASM2_IGNORE_RAISE"))
    return 0;
  return kill(getpid(), sig);
}

static int b_kill(pid_t pid, int sig) {
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  char tag[96];
  snprintf(tag, sizeof(tag), "[kill] pid=%ld sig=%d", (long)pid, sig);
  if (sig == SIGSEGV || getenv("TASM2_SIGNAL_DEBUG"))
    log_caller_offset(tag, ret);
  if (ignore_game_sigsegv(tag, sig, ret))
    return 0;
  return kill(pid, sig);
}

static int b_tgkill(pid_t tgid, pid_t tid, int sig) {
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  char tag[128];
  snprintf(tag, sizeof(tag), "[tgkill] tgid=%ld tid=%ld sig=%d",
           (long)tgid, (long)tid, sig);
  if (sig == SIGSEGV || getenv("TASM2_SIGNAL_DEBUG"))
    log_caller_offset(tag, ret);
  if (ignore_game_sigsegv(tag, sig, ret))
    return 0;
#ifdef SYS_tgkill
  return (int)syscall(SYS_tgkill, tgid, tid, sig);
#else
  errno = ENOSYS;
  return -1;
#endif
}

static int b_pthread_kill(pthread_t thread, int sig) {
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  char tag[128];
  snprintf(tag, sizeof(tag), "[pthread_kill] thread=%lu sig=%d",
           (unsigned long)thread, sig);
  if (sig == SIGSEGV || getenv("TASM2_SIGNAL_DEBUG"))
    log_caller_offset(tag, ret);
  if (ignore_game_sigsegv(tag, sig, ret))
    return 0;
  return pthread_kill(thread, sig);
}

static void b_abort(void) {
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  log_caller_offset("[abort]", ret);
  fflush(stderr);
  raise(SIGABRT);
  _exit(128 + SIGABRT);
}

static int b_sigaction(int sig, const struct sigaction *act,
                       struct sigaction *oldact) {
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  if (getenv("TASM2_SIGNAL_DEBUG") || sig == SIGSEGV || sig == SIGABRT) {
    char tag[96];
    snprintf(tag, sizeof(tag), "[sigaction] sig=%d handler=%p flags=0x%lx",
             sig, act ? (void *)act->sa_handler : NULL,
             act ? (unsigned long)act->sa_flags : 0UL);
    log_caller_offset(tag, ret);
  }
  return sigaction(sig, act, oldact);
}

static void (*b_bsd_signal(int sig, void (*handler)(int)))(int) {
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  if (getenv("TASM2_SIGNAL_DEBUG") || sig == SIGSEGV || sig == SIGABRT) {
    char tag[96];
    snprintf(tag, sizeof(tag), "[bsd_signal] sig=%d handler=%p", sig,
             (void *)handler);
    log_caller_offset(tag, ret);
  }
  return signal(sig, handler);
}

static long b_syscall(long nr, ...) {
  va_list ap;
  va_start(ap, nr);
  long a1 = va_arg(ap, long);
  long a2 = va_arg(ap, long);
  long a3 = va_arg(ap, long);
  long a4 = va_arg(ap, long);
  long a5 = va_arg(ap, long);
  long a6 = va_arg(ap, long);
  va_end(ap);

  int signal_sig = 0;
#ifdef SYS_tgkill
  if (nr == SYS_tgkill)
    signal_sig = (int)a3;
#endif
#ifdef SYS_tkill
  if (nr == SYS_tkill)
    signal_sig = (int)a2;
#endif
#ifdef SYS_kill
  if (nr == SYS_kill)
    signal_sig = (int)a2;
#endif
  static int n;
  int signal_like = signal_sig == SIGSEGV ||
                    a1 == SIGSEGV || a2 == SIGSEGV || a3 == SIGSEGV ||
                    a4 == SIGSEGV;
  if (n++ < 40 || signal_like) {
    uintptr_t ret = (uintptr_t)__builtin_return_address(0);
    uintptr_t ret_arm = ret & ~(uintptr_t)1;
    uintptr_t text = (uintptr_t)text_base;
    fprintf(stderr,
            "[syscall] nr=%ld args=%lx,%lx,%lx,%lx,%lx,%lx caller=%p",
            nr, a1, a2, a3, a4, a5, a6, (void *)ret);
    if (ret_arm >= text && ret_arm < text + text_size)
      fprintf(stderr, " (libtasm2.so+0x%lx)",
              (unsigned long)(ret_arm - text));
    fprintf(stderr, "\n");
    if (signal_sig && ignore_game_sigsegv("[syscall]", signal_sig, ret))
      return 0;
  }

  if (signal_like && getenv("TASM2_IGNORE_SIGSEGV_SYSCALL"))
    return 0;

  return syscall(nr, a1, a2, a3, a4, a5, a6);
}

static void *b_dlsym(void *handle, const char *symbol) {
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  const char *name = symbol ? symbol : "";
  void *resolved = NULL;
  if (strcmp(name, "raise") == 0)
    resolved = (void *)b_raise;
  else if (strcmp(name, "abort") == 0)
    resolved = (void *)b_abort;
  else if (strcmp(name, "syscall") == 0)
    resolved = (void *)b_syscall;
  else if (strcmp(name, "kill") == 0)
    resolved = (void *)b_kill;
  else if (strcmp(name, "tgkill") == 0)
    resolved = (void *)b_tgkill;
  else if (strcmp(name, "pthread_kill") == 0)
    resolved = (void *)b_pthread_kill;
  else if (strcmp(name, "sigaction") == 0)
    resolved = (void *)b_sigaction;
  else if (strcmp(name, "bsd_signal") == 0)
    resolved = (void *)b_bsd_signal;
  if (resolved) {
    if (getenv("TASM2_SIGNAL_DEBUG") || strcmp(name, "pthread_kill") == 0 ||
        strcmp(name, "raise") == 0 || strcmp(name, "tgkill") == 0)
      log_caller_offset("[dlsym shim]", ret);
    return resolved;
  }
  return dlsym(handle, symbol);
}

/* ---- ctype tables bionic (_ctype_[c+1], _tolower_tab_[c+1]) ----
 * bionic flags: _U=1 _L=2 _N=4 _S=8 _P=0x10 _C=0x20 _X=0x40 _B=0x80.
 * Preenchidas da classificação glibc no constructor. */
static unsigned char b_ctype[1 + 256];
static unsigned char b_tolower[1 + 256];
static unsigned char b_toupper[1 + 256];
__attribute__((constructor)) static void init_ctype(void) {
  for (int c = 0; c < 256; c++) {
    unsigned char f = 0;
    if (isupper(c)) f |= 0x01;
    if (islower(c)) f |= 0x02;
    if (isdigit(c)) f |= 0x04;
    if (isspace(c)) f |= 0x08;
    if (ispunct(c)) f |= 0x10;
    if (iscntrl(c)) f |= 0x20;
    if (isxdigit(c)) f |= 0x40;
    if (c == ' ')   f |= 0x80;
    b_ctype[c + 1] = f;
    b_tolower[c + 1] = (unsigned char)tolower(c);
    b_toupper[c + 1] = (unsigned char)toupper(c);
  }
}

/* ---- stubs ---- */
static int b_dso_handle;                       /* __dso_handle = endereço dummy */
static void *b_cxa_type_match(void *a, void *b, char c) { (void)a; (void)b; (void)c; return (void *)0; }
extern int __sigsetjmp(void *, int);           /* glibc; bionic sigsetjmp == isso */
/* AndroidBitmap (jnigraphics) — stub: sinaliza erro p/ a engine cair no fallback */
static int abm_getInfo(void *env, void *bmp, void *info) { (void)env; (void)bmp; (void)info; return -1; }
static int abm_lock(void *env, void *bmp, void **pix) { (void)env; (void)bmp; if (pix) *pix = 0; return -1; }
static int abm_unlock(void *env, void *bmp) { (void)env; (void)bmp; return 0; }

extern const char *_ctype_;
extern const short *_toupper_tab_;
extern const short *_tolower_tab_;
extern size_t b_fwrite(const void *, size_t, size_t, void *);
extern size_t b_fread(void *, size_t, size_t, void *);
extern int b_fputs(const char *, void *);
extern int b_fputc(int, void *);
extern int b_fgetc(void *);
extern int b_fseek(void *, long, int);
extern long b_ftell(void *);
extern int b_fflush(void *);
extern int b_fclose(void *);
extern const SLInterfaceID sl_IID_ENGINE, sl_IID_PLAY, sl_IID_VOLUME,
    sl_IID_BUFFERQUEUE;
static unsigned b_page_size = 4096;
uintptr_t __stack_chk_guard = 0x27d92c41u;
static void b_stack_chk_fail(void) {
  static int n;
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  uintptr_t ret_arm = ret & ~(uintptr_t)1;
  uintptr_t text = (uintptr_t)text_base;
  if (n++ < 8) {
    fprintf(stderr, "[stack_chk_fail] ignored caller=%p", (void *)ret);
    if (ret_arm >= text && ret_arm < text + text_size)
      fprintf(stderr, " (libtasm2.so+0x%lx)",
              (unsigned long)(ret_arm - text));
    fprintf(stderr, "\n");
  }
}
static int b_aeabi_atexit(void *obj, void (*dtor)(void *), void *dso) {
  (void)obj; (void)dtor; (void)dso;
  return 0;
}

static const char *tasm2_root(void) {
  const char *root = getenv("TASM2_DATA");
  return (root && *root) ? root : "/storage/roms/ports/tasm2";
}

static int starts_with(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int ends_with(const char *s, const char *suffix) {
  size_t slen = strlen(s), suffix_len = strlen(suffix);
  return slen >= suffix_len && strcmp(s + slen - suffix_len, suffix) == 0;
}

static const char *join_resolved(const char *subdir, const char *suffix) {
  static char bufs[4][1024];
  static int idx = 0;
  char *out = bufs[idx++ & 3];
  snprintf(out, 1024, "%s/%s%s", tasm2_root(), subdir, suffix ? suffix : "");
  return out;
}

static const char *join_root_resolved(const char *suffix) {
  static char bufs[4][1024];
  static int idx = 0;
  char *out = bufs[idx++ & 3];
  snprintf(out, 1024, "%s%s", tasm2_root(), suffix ? suffix : "");
  return out;
}

static const char *map_obb_zip_alias(const char *path) {
  if (!path || !ends_with(path, ".obb.zip"))
    return path;

  static char bufs[4][1024];
  static int idx = 0;
  char *out = bufs[idx++ & 3];
  size_t len = strlen(path) - strlen(".zip");
  if (len >= 1024)
    len = 1023;
  memcpy(out, path, len);
  out[len] = '\0';
  return out;
}

static const char *resolve_android_path(const char *path) {
  static const char *pkg = "com.gameloft.android.ANMP.GloftASHM";
  char prefix[256];
  size_t n;

  if (!path || !*path)
    return path;

  if (strcmp(path, "/filesConfig.dat") == 0)
    return join_resolved("files", "/filesConfig.dat");

  snprintf(prefix, sizeof(prefix), "/sdcard/Android/obb/%s/", pkg);
  if (starts_with(path, prefix))
    return map_obb_zip_alias(join_resolved("obb/", path + strlen(prefix)));

  snprintf(prefix, sizeof(prefix), "/storage/emulated/0/Android/obb/%s/", pkg);
  if (starts_with(path, prefix))
    return map_obb_zip_alias(join_resolved("obb/", path + strlen(prefix)));

  snprintf(prefix, sizeof(prefix), "sdcard/Android/obb/%s/", pkg);
  if (starts_with(path, prefix))
    return map_obb_zip_alias(join_resolved("obb/", path + strlen(prefix)));

  snprintf(prefix, sizeof(prefix), "storage/emulated/0/Android/obb/%s/", pkg);
  if (starts_with(path, prefix))
    return map_obb_zip_alias(join_resolved("obb/", path + strlen(prefix)));

  snprintf(prefix, sizeof(prefix), "./sdcard/Android/obb/%s/", pkg);
  if (starts_with(path, prefix))
    return map_obb_zip_alias(join_resolved("obb/", path + strlen(prefix)));

  snprintf(prefix, sizeof(prefix), "./storage/emulated/0/Android/obb/%s/", pkg);
  if (starts_with(path, prefix))
    return map_obb_zip_alias(join_resolved("obb/", path + strlen(prefix)));

  snprintf(prefix, sizeof(prefix), "/sdcard/Android/data/%s/files", pkg);
  n = strlen(prefix);
  if (starts_with(path, prefix))
    return join_resolved("files", path[n] ? path + n : "");

  snprintf(prefix, sizeof(prefix), "/storage/emulated/0/Android/data/%s/files", pkg);
  n = strlen(prefix);
  if (starts_with(path, prefix))
    return join_resolved("files", path[n] ? path + n : "");

  snprintf(prefix, sizeof(prefix), "sdcard/Android/data/%s/files", pkg);
  n = strlen(prefix);
  if (starts_with(path, prefix))
    return join_resolved("files", path[n] ? path + n : "");

  snprintf(prefix, sizeof(prefix), "storage/emulated/0/Android/data/%s/files", pkg);
  n = strlen(prefix);
  if (starts_with(path, prefix))
    return join_resolved("files", path[n] ? path + n : "");

  snprintf(prefix, sizeof(prefix), "./sdcard/Android/data/%s/files", pkg);
  n = strlen(prefix);
  if (starts_with(path, prefix))
    return join_resolved("files", path[n] ? path + n : "");

  snprintf(prefix, sizeof(prefix), "./storage/emulated/0/Android/data/%s/files", pkg);
  n = strlen(prefix);
  if (starts_with(path, prefix))
    return join_resolved("files", path[n] ? path + n : "");

  snprintf(prefix, sizeof(prefix), "/data/data/%s", pkg);
  n = strlen(prefix);
  if (starts_with(path, prefix))
    return join_root_resolved(path[n] ? path + n : "");

  if (starts_with(path, "/sdcard/gameloft"))
    return join_resolved("gameloft", path + strlen("/sdcard/gameloft"));
  if (starts_with(path, "sdcard/gameloft"))
    return join_resolved("gameloft", path + strlen("sdcard/gameloft"));

  return map_obb_zip_alias(path);
}

static int io_debug_enabled(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = getenv("TASM2_IO_DEBUG") ? 1 : 0;
  return enabled;
}

static int obb_trace_enabled(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = getenv("TASM2_OBB_TRACE") ? 1 : 0;
  return enabled;
}

static void log_io_result(const char *op, const char *path,
                          const char *resolved, long result) {
  if (!io_debug_enabled())
    return;
  int failed = result < 0 || result == 0;
  if (failed || strstr(path ? path : "", "tasm2") ||
      strstr(path ? path : "", "gameloft") ||
      strstr(path ? path : "", "Android") ||
      strstr(resolved ? resolved : "", "/storage/roms/ports/tasm2")) {
    fprintf(stderr, "[io] %s('%s' -> '%s') = %ld errno=%d\n", op,
            path ? path : "", resolved ? resolved : "", result, errno);
  }
}

#define MAX_TRACKED_FILES 128
struct tracked_file {
  void *fp;
  char path[192];
};
static struct tracked_file g_tracked_files[MAX_TRACKED_FILES];
static int g_obb_trace_lines;

static int is_obb_path(const char *path) {
  return path && strstr(path, ".obb") != NULL;
}

static struct tracked_file *tracked_file_find(void *fp) {
  for (int i = 0; i < MAX_TRACKED_FILES; i++) {
    if (g_tracked_files[i].fp == fp)
      return &g_tracked_files[i];
  }
  return NULL;
}

static void tracked_file_add(void *fp, const char *path) {
  if (!fp || !is_obb_path(path))
    return;
  for (int i = 0; i < MAX_TRACKED_FILES; i++) {
    if (!g_tracked_files[i].fp) {
      g_tracked_files[i].fp = fp;
      snprintf(g_tracked_files[i].path, sizeof(g_tracked_files[i].path), "%s",
               path ? path : "");
      return;
    }
  }
}

static void tracked_file_remove(void *fp) {
  struct tracked_file *tf = tracked_file_find(fp);
  if (tf)
    memset(tf, 0, sizeof(*tf));
}

static void print_trace_caller(uintptr_t ret) {
  uintptr_t ret_arm = ret & ~(uintptr_t)1;
  uintptr_t text = (uintptr_t)text_base;
  fprintf(stderr, " caller=%p", (void *)ret);
  if (ret_arm >= text && ret_arm < text + text_size)
    fprintf(stderr, " (libtasm2.so+0x%lx)",
            (unsigned long)(ret_arm - text));
}

static void trace_obb_stdio(const char *op, void *fp, long a, long b, long c,
                            long result, uintptr_t ret) {
  if (!obb_trace_enabled() || g_obb_trace_lines >= 1800)
    return;
  struct tracked_file *tf = tracked_file_find(fp);
  if (!tf)
    return;
  g_obb_trace_lines++;
  fprintf(stderr, "[obb] %s fp=%p a=%ld b=%ld c=%ld -> %ld path=%s", op, fp,
          a, b, c, result, tf->path);
  print_trace_caller(ret);
  fprintf(stderr, "\n");
}

static FILE *tasm2_fopen(const char *path, const char *mode) {
  const char *resolved = resolve_android_path(path);
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  FILE *f = fopen(resolved, mode);
  tracked_file_add(f, resolved);
  log_io_result("fopen", path, resolved, f ? 1 : 0);
  if (f && obb_trace_enabled() && is_obb_path(resolved) &&
      g_obb_trace_lines < 1800) {
    g_obb_trace_lines++;
    fprintf(stderr, "[obb] fopen fp=%p mode=%s path=%s", (void *)f,
            mode ? mode : "", resolved ? resolved : "");
    print_trace_caller(ret);
    fprintf(stderr, "\n");
  }
  return f;
}

static int tasm2_open(const char *path, int flags, ...) {
  const char *resolved = resolve_android_path(path);
  int fd;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode_t mode = (mode_t)va_arg(ap, int);
    va_end(ap);
    fd = open(resolved, flags, mode);
  } else {
    fd = open(resolved, flags);
  }
  log_io_result("open", path, resolved, fd);
  return fd;
}

static int tasm2_open_2(const char *path, int flags) {
  const char *resolved = resolve_android_path(path);
  int fd = open(resolved, flags);
  log_io_result("__open_2", path, resolved, fd);
  return fd;
}

static int tasm2_stat(const char *path, struct stat *st) {
  const char *resolved = resolve_android_path(path);
  int r = stat(resolved, st);
  log_io_result("stat", path, resolved, r);
  return r;
}

static int tasm2_access(const char *path, int mode) {
  const char *resolved = resolve_android_path(path);
  int r = access(resolved, mode);
  log_io_result("access", path, resolved, r);
  return r;
}

static DIR *tasm2_opendir(const char *path) {
  const char *resolved = resolve_android_path(path);
  DIR *d = opendir(resolved);
  log_io_result("opendir", path, resolved, d ? 1 : 0);
  return d;
}

static int tasm2_mkdir(const char *path, mode_t mode) {
  const char *resolved = resolve_android_path(path);
  int r = mkdir(resolved, mode);
  log_io_result("mkdir", path, resolved, r);
  return r;
}

static int tasm2_remove(const char *path) {
  if (!path || !*path) {
    log_io_result("remove", path, path, 0);
    return 0;
  }
  const char *resolved = resolve_android_path(path);
  int r = remove(resolved);
  log_io_result("remove", path, resolved, r);
  return r;
}

static int tasm2_rename(const char *oldpath, const char *newpath) {
  char oldbuf[1024], newbuf[1024];
  snprintf(oldbuf, sizeof(oldbuf), "%s", resolve_android_path(oldpath));
  snprintf(newbuf, sizeof(newbuf), "%s", resolve_android_path(newpath));
  return rename(oldbuf, newbuf);
}

static size_t tasm2_fread(void *ptr, size_t size, size_t nmemb, void *fp) {
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  long pos = b_ftell(fp);
  size_t r = b_fread(ptr, size, nmemb, fp);
  trace_obb_stdio("fread", fp, pos, (long)size, (long)nmemb, (long)r, ret);
  return r;
}

static int tasm2_fseek(void *fp, long offset, int whence) {
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  int r = b_fseek(fp, offset, whence);
  long pos = b_ftell(fp);
  trace_obb_stdio("fseek", fp, offset, whence, pos, r, ret);
  return r;
}

static long tasm2_ftell(void *fp) {
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  long r = b_ftell(fp);
  trace_obb_stdio("ftell", fp, 0, 0, 0, r, ret);
  return r;
}

static int tasm2_fclose(void *fp) {
  uintptr_t ret = (uintptr_t)__builtin_return_address(0);
  trace_obb_stdio("fclose", fp, 0, 0, 0, 0, ret);
  tracked_file_remove(fp);
  return b_fclose(fp);
}

void *dysmantle_gl_proc_override(const char *name) {
  (void)name;
  return NULL;
}

static void glDeleteFencesNV_stub(GLsizei n, const GLuint *fences) {
  (void)n; (void)fences;
}
static void glGenFencesNV_stub(GLsizei n, GLuint *fences) {
  static GLuint next = 1;
  for (GLsizei i = 0; fences && i < n; i++) fences[i] = next++;
}
static void glSetFenceNV_stub(GLuint fence, GLenum condition) {
  (void)fence; (void)condition;
}
static void glFinishFenceNV_stub(GLuint fence) {
  (void)fence;
}
static unsigned char glTestFenceNV_stub(GLuint fence) {
  (void)fence;
  return 1;
}
static void glTexImage3DOES_stub(GLenum target, GLint level, GLint internalformat,
                                 GLsizei width, GLsizei height, GLsizei depth,
                                 GLint border, GLenum format, GLenum type,
                                 const void *pixels) {
  (void)target; (void)level; (void)internalformat; (void)width; (void)height;
  (void)depth; (void)border; (void)format; (void)type; (void)pixels;
}
static void glTexSubImage3DOES_stub(GLenum target, GLint level, GLint xoffset,
                                    GLint yoffset, GLint zoffset, GLsizei width,
                                    GLsizei height, GLsizei depth, GLenum format,
                                    GLenum type, const void *pixels) {
  (void)target; (void)level; (void)xoffset; (void)yoffset; (void)zoffset;
  (void)width; (void)height; (void)depth; (void)format; (void)type; (void)pixels;
}
static void glCompressedTexImage3DOES_stub(GLenum target, GLint level,
                                           GLenum internalformat, GLsizei width,
                                           GLsizei height, GLsizei depth,
                                           GLint border, GLsizei imageSize,
                                           const void *data) {
  (void)target; (void)level; (void)internalformat; (void)width; (void)height;
  (void)depth; (void)border; (void)imageSize; (void)data;
}
static void glCompressedTexSubImage3DOES_stub(GLenum target, GLint level,
                                              GLint xoffset, GLint yoffset,
                                              GLint zoffset, GLsizei width,
                                              GLsizei height, GLsizei depth,
                                              GLenum format, GLsizei imageSize,
                                              const void *data) {
  (void)target; (void)level; (void)xoffset; (void)yoffset; (void)zoffset;
  (void)width; (void)height; (void)depth; (void)format; (void)imageSize; (void)data;
}
static void *ALooper_forThread_stub(void) { return (void *)1; }
static int ASensorEventQueue_getEvents_stub(void *queue, void *events, size_t count) {
  (void)queue; (void)events; (void)count;
  return 0;
}
static int ASensorEventQueue_disableSensor_stub(void *queue, void *sensor) {
  (void)queue; (void)sensor;
  return 0;
}

DynLibFunction port_shims[] = {
    {"__sF", (uintptr_t)__sF},
    {"__page_size", (uintptr_t)&b_page_size},
    {"__stack_chk_guard", (uintptr_t)&__stack_chk_guard},
    {"__stack_chk_fail", (uintptr_t)b_stack_chk_fail},
    {"__aeabi_atexit", (uintptr_t)b_aeabi_atexit},
    {"fprintf", (uintptr_t)w_fprintf}, {"vfprintf", (uintptr_t)w_vfprintf},
    {"fopen", (uintptr_t)tasm2_fopen},
    {"open", (uintptr_t)tasm2_open},
    {"__open_2", (uintptr_t)tasm2_open_2},
    {"stat", (uintptr_t)tasm2_stat},
    {"access", (uintptr_t)tasm2_access},
    {"opendir", (uintptr_t)tasm2_opendir},
    {"mkdir", (uintptr_t)tasm2_mkdir},
    {"remove", (uintptr_t)tasm2_remove},
    {"rename", (uintptr_t)tasm2_rename},
    {"fwrite", (uintptr_t)b_fwrite}, {"fread", (uintptr_t)tasm2_fread},
    {"fputs", (uintptr_t)b_fputs}, {"fputc", (uintptr_t)b_fputc},
    {"fgetc", (uintptr_t)b_fgetc}, {"fseek", (uintptr_t)tasm2_fseek},
    {"ftell", (uintptr_t)tasm2_ftell}, {"fclose", (uintptr_t)tasm2_fclose},
    {"fflush", (uintptr_t)b_fflush},
    {"__errno", (uintptr_t)b_errno},
    {"__assert2", (uintptr_t)b_assert2},
    {"abort", (uintptr_t)b_abort},
    {"raise", (uintptr_t)b_raise},
    {"syscall", (uintptr_t)b_syscall},
    {"kill", (uintptr_t)b_kill},
    {"tgkill", (uintptr_t)b_tgkill},
    {"pthread_kill", (uintptr_t)b_pthread_kill},
    {"dlsym", (uintptr_t)b_dlsym},
    {"sigaction", (uintptr_t)b_sigaction},
    {"bsd_signal", (uintptr_t)b_bsd_signal},
    {"__android_log_print", (uintptr_t)b_log_print},
    {"__android_log_write", (uintptr_t)b_log_write},
    {"__android_log_vprint", (uintptr_t)b_log_vprint},
    {"__android_log_assert", (uintptr_t)b_log_assert},
    {"_ctype_", (uintptr_t)&_ctype_},
    {"_tolower_tab_", (uintptr_t)&_tolower_tab_},
    {"_toupper_tab_", (uintptr_t)&_toupper_tab_},
    {"__dso_handle", (uintptr_t)&b_dso_handle},
    {"__cxa_type_match", (uintptr_t)b_cxa_type_match},
    {"sigsetjmp", (uintptr_t)__sigsetjmp},
    {"AndroidBitmap_getInfo", (uintptr_t)abm_getInfo},
    {"AndroidBitmap_lockPixels", (uintptr_t)abm_lock},
    {"AndroidBitmap_unlockPixels", (uintptr_t)abm_unlock},
    {"glDeleteFencesNV", (uintptr_t)glDeleteFencesNV_stub},
    {"glGenFencesNV", (uintptr_t)glGenFencesNV_stub},
    {"glSetFenceNV", (uintptr_t)glSetFenceNV_stub},
    {"glFinishFenceNV", (uintptr_t)glFinishFenceNV_stub},
    {"glTestFenceNV", (uintptr_t)glTestFenceNV_stub},
    {"glTexImage3DOES", (uintptr_t)glTexImage3DOES_stub},
    {"glTexSubImage3DOES", (uintptr_t)glTexSubImage3DOES_stub},
    {"glCompressedTexImage3DOES", (uintptr_t)glCompressedTexImage3DOES_stub},
    {"glCompressedTexSubImage3DOES", (uintptr_t)glCompressedTexSubImage3DOES_stub},
    {"ALooper_forThread", (uintptr_t)ALooper_forThread_stub},
    {"ASensorEventQueue_getEvents", (uintptr_t)ASensorEventQueue_getEvents_stub},
    {"ASensorEventQueue_disableSensor", (uintptr_t)ASensorEventQueue_disableSensor_stub},
    {"slCreateEngine", (uintptr_t)slCreateEngine_shim},
    {"SL_IID_ENGINE", (uintptr_t)&sl_IID_ENGINE},
    {"SL_IID_PLAY", (uintptr_t)&sl_IID_PLAY},
    {"SL_IID_VOLUME", (uintptr_t)&sl_IID_VOLUME},
    {"SL_IID_BUFFERQUEUE", (uintptr_t)&sl_IID_BUFFERQUEUE},
};
int port_shims_count = sizeof(port_shims) / sizeof(port_shims[0]);
