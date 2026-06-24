/*
 * imports.c -- .so import resolution for Syberia
 *
 * Maps all 355 undefined symbols from libsyberia1.so to real
 * libc/GL/EGL functions or our shim implementations.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <GLES/gl.h>

#include "android_shim.h"
#include "egl_shim.h"
#include "opensles_shim.h"
#include "so_util.h"
#include "util.h"

extern uintptr_t __cxa_atexit;
extern uintptr_t __cxa_finalize;
extern uintptr_t __stack_chk_fail;

FILE *stderr_fake = (FILE *)0x1337;

static uint8_t fake_sF[3][0x100];
static uint64_t __stack_chk_guard_fake = 0x4242424242424242;

/* ---- errno compat ---- */
static int *__errno_fake(void) { return &errno; }

/* ---- __android_log ---- */
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

int __android_log_vprint_fake(int prio, const char *tag, const char *fmt,
                               va_list ap) {
  (void)prio;
  static char string[0x1000];
  vsnprintf(string, sizeof(string), fmt, ap);
  debugPrintf("LOG [%s]: %s\n", tag, string);
  return 0;
}

/* ---- fortified libc stubs ---- */
// Android's bionic has fortified versions; we redirect to normal ones
void *__memcpy_chk(void *dst, const void *src, size_t n, size_t dst_len) {
  (void)dst_len;
  return memcpy(dst, src, n);
}

void *__memmove_chk(void *dst, const void *src, size_t n, size_t dst_len) {
  (void)dst_len;
  return memmove(dst, src, n);
}

char *__strchr_chk(const char *s, int c, size_t n) {
  (void)n;
  return strchr(s, c);
}

size_t __strlen_chk(const char *s, size_t max_len) {
  (void)max_len;
  return strlen(s);
}

char *__strncat_chk(char *dst, const char *src, size_t n, size_t dst_buf_size) {
  (void)dst_buf_size;
  return strncat(dst, src, n);
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
  return open(pathname, flags);
}

void __FD_SET_chk(int fd, fd_set *set) {
  FD_SET(fd, set);
}

/* ---- ctype compat ---- */
size_t __ctype_get_mb_cur_max_fake(void) { return 4; }

/* ---- dl_iterate_phdr stub ---- */
int dl_iterate_phdr_fake(void *callback, void *data) {
  (void)callback;
  (void)data;
  return 0;
}

/* ---- android_set_abort_message stub ---- */
void android_set_abort_message_fake(const char *msg) {
  debugPrintf("android_set_abort_message: %s\n", msg ? msg : "(null)");
}


/* ---- fopen wrapper for debugging ---- */
FILE *fopen_fake(const char *filename, const char *mode) {
  FILE *f = fopen(filename, mode);
  if (!f)
    debugPrintf("fopen(\"%s\", \"%s\") = NULL (errno=%d: %s)\n",
                filename, mode, errno, strerror(errno));
  else
    debugPrintf("fopen(\"%s\", \"%s\") = %p\n", filename, mode, f);
  return f;
}

/* ---- pthread wrappers (bionic struct sizes differ from glibc) ---- */
int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *mutexattr) {
  (void)mutexattr;
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;
  // Always create RECURSIVE mutexes. The game relies on recursive locking
  // (e.g. TeMusic::update() locks TeMutex then calls stop() which re-locks).
  // Our pthread_mutexattr_settype import was a no-op (ret0), so the game's
  // explicit PTHREAD_MUTEX_RECURSIVE requests were silently dropped.
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  int ret = pthread_mutex_init(m, &attr);
  pthread_mutexattr_destroy(&attr);
  if (ret < 0) { free(m); return -1; }
  *uid = m;
  return 0;
}

int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) {
    pthread_mutex_destroy(*uid);
    free(*uid);
    *uid = NULL;
  }
  return 0;
}

static int g_pthread_trace = 0;

int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  if (g_pthread_trace)
    debugPrintf("pthread_mutex_lock_fake(%p -> %p)\n", uid, uid ? *uid : NULL);
  if (!*uid) pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) {
    int attr = 1;
    pthread_mutex_init_fake(uid, &attr);
  }
  return pthread_mutex_lock(*uid);
}

int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
  if (!*uid) pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) {
    int attr = 1;
    pthread_mutex_init_fake(uid, &attr);
  }
  return pthread_mutex_trylock(*uid);
}

int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  if (!*uid) pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) {
    int attr = 1;
    pthread_mutex_init_fake(uid, &attr);
  }
  return pthread_mutex_unlock(*uid);
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
  (void)condattr;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  if (pthread_cond_init(c, NULL) < 0) { free(c); return -1; }
  *cnd = c;
  return 0;
}

int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (cnd && *cnd) {
    pthread_cond_destroy(*cnd);
    free(*cnd);
    *cnd = NULL;
  }
  return 0;
}

int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  if (g_pthread_trace)
    debugPrintf("pthread_cond_wait_fake(cnd=%p->%p, mtx=%p->%p)\n",
                cnd, cnd ? *cnd : NULL, mtx, mtx ? *mtx : NULL);
  if (!*cnd) pthread_cond_init_fake(cnd, NULL);
  return pthread_cond_wait(*cnd, *mtx);
}

int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx,
                                 const struct timespec *t) {
  if (g_pthread_trace)
    debugPrintf("pthread_cond_timedwait_fake(cnd=%p->%p, mtx=%p->%p)\n",
                cnd, cnd ? *cnd : NULL, mtx, mtx ? *mtx : NULL);
  if (!*cnd) pthread_cond_init_fake(cnd, NULL);
  return pthread_cond_timedwait(*cnd, *mtx, t);
}

int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  if (g_pthread_trace)
    debugPrintf("pthread_cond_signal_fake(cnd=%p->%p)\n", cnd, cnd ? *cnd : NULL);
  if (!*cnd) pthread_cond_init_fake(cnd, NULL);
  return pthread_cond_signal(*cnd);
}

int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  if (g_pthread_trace)
    debugPrintf("pthread_cond_broadcast_fake(cnd=%p->%p)\n", cnd, cnd ? *cnd : NULL);
  if (!*cnd) pthread_cond_init_fake(cnd, NULL);
  return pthread_cond_broadcast(*cnd);
}

int pthread_create_fake(pthread_t *thread, const void *attr, void *entry,
                         void *arg) {
  debugPrintf("pthread_create_fake(entry=%p, arg=%p)\n", entry, arg);
  (void)attr;
  return pthread_create(thread, NULL, entry, arg);
}

int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine) return -1;
  if (__sync_lock_test_and_set(once_control, 1) == 0)
    (*init_routine)();
  return 0;
}

/* ---- Import table ---- */

DynLibFunction dynlib_functions[] = {
    /* ---- Android stubs ---- */
    {"__sF", (uintptr_t)&fake_sF},
    {"__errno", (uintptr_t)&__errno_fake},
    {"__stack_chk_fail", (uintptr_t)&__stack_chk_fail},
    {"__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake},
    {"__cxa_atexit", (uintptr_t)&__cxa_atexit},
    {"__cxa_finalize", (uintptr_t)&__cxa_finalize},
    {"__android_log_print", (uintptr_t)&__android_log_print_fake},
    {"__android_log_vprint", (uintptr_t)&__android_log_vprint_fake},
    {"android_set_abort_message", (uintptr_t)&android_set_abort_message_fake},
    {"__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake},
    {"dl_iterate_phdr", (uintptr_t)&dl_iterate_phdr_fake},
    {"stderr", (uintptr_t)&stderr_fake},

    /* ---- Fortified libc ---- */
    {"__memcpy_chk", (uintptr_t)&__memcpy_chk},
    {"__memmove_chk", (uintptr_t)&__memmove_chk},
    {"__strchr_chk", (uintptr_t)&__strchr_chk},
    {"__strlen_chk", (uintptr_t)&__strlen_chk},
    {"__strncat_chk", (uintptr_t)&__strncat_chk},
    {"__vsprintf_chk", (uintptr_t)&__vsprintf_chk},
    {"__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk},
    {"__read_chk", (uintptr_t)&__read_chk},
    {"__open_2", (uintptr_t)&__open_2},
    {"__FD_SET_chk", (uintptr_t)&__FD_SET_chk},

    /* ---- pthread (wrapped for bionic compat) ---- */
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
    {"pthread_once", (uintptr_t)&pthread_once_fake},
    {"pthread_setschedparam", (uintptr_t)&ret0},
    {"pthread_attr_init", (uintptr_t)&ret0},
    {"pthread_attr_setdetachstate", (uintptr_t)&ret0},
    {"pthread_key_create", (uintptr_t)&pthread_key_create},
    {"pthread_getspecific", (uintptr_t)&pthread_getspecific},
    {"pthread_setspecific", (uintptr_t)&pthread_setspecific},
    {"sched_yield", (uintptr_t)&sched_yield},
    {"posix_memalign", (uintptr_t)&posix_memalign},

    /* ---- Memory ---- */
    {"malloc", (uintptr_t)&malloc},
    {"calloc", (uintptr_t)&calloc},
    {"realloc", (uintptr_t)&realloc},
    {"free", (uintptr_t)&free},

    /* ---- String ---- */
    {"memcmp", (uintptr_t)&memcmp},
    {"memcpy", (uintptr_t)&memcpy},
    {"memmove", (uintptr_t)&memmove},
    {"memset", (uintptr_t)&memset},
    {"memchr", (uintptr_t)&memchr},
    {"strcmp", (uintptr_t)&strcmp},
    {"strncmp", (uintptr_t)&strncmp},
    {"strcpy", (uintptr_t)&strcpy},
    {"strncpy", (uintptr_t)&strncpy},
    {"strcat", (uintptr_t)&strcat},
    {"strchr", (uintptr_t)&strchr},
    {"strrchr", (uintptr_t)&strrchr},
    {"strstr", (uintptr_t)&strstr},
    {"strpbrk", (uintptr_t)&strpbrk},
    {"strspn", (uintptr_t)&strspn},
    {"strlen", (uintptr_t)&strlen},
    {"strerror", (uintptr_t)&strerror},
    {"strerror_r", (uintptr_t)&strerror_r},
    {"strcoll", (uintptr_t)&strcoll},
    {"strxfrm", (uintptr_t)&strxfrm},
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
    {"atoi", (uintptr_t)&atoi},
    {"atol", (uintptr_t)&atol},
    {"atof", (uintptr_t)&atof},

    /* ---- ctype ---- */
    {"isalnum", (uintptr_t)&isalnum},
    {"isalpha", (uintptr_t)&isalpha},
    {"iscntrl", (uintptr_t)&iscntrl},
    {"isgraph", (uintptr_t)&isgraph},
    {"islower", (uintptr_t)&islower},
    {"ispunct", (uintptr_t)&ispunct},
    {"isspace", (uintptr_t)&isspace},
    {"isupper", (uintptr_t)&isupper},
    {"isxdigit", (uintptr_t)&isxdigit},
    {"tolower", (uintptr_t)&tolower},
    {"toupper", (uintptr_t)&toupper},

    /* ---- wctype / wchar ---- */
    {"towlower", (uintptr_t)&towlower},
    {"towupper", (uintptr_t)&towupper},
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
    {"wcsxfrm", (uintptr_t)&wcsxfrm},
    {"wmemcmp", (uintptr_t)&wmemcmp},
    {"wmemcpy", (uintptr_t)&wmemcpy},
    {"wmemmove", (uintptr_t)&wmemmove},
    {"wmemset", (uintptr_t)&wmemset},
    {"wmemchr", (uintptr_t)&wmemchr},
    {"mbrtowc", (uintptr_t)&mbrtowc},
    {"wcrtomb", (uintptr_t)&wcrtomb},
    {"mbrlen", (uintptr_t)&mbrlen},
    {"mbtowc", (uintptr_t)&mbtowc},
    {"mbsrtowcs", (uintptr_t)&mbsrtowcs},
    {"mbsnrtowcs", (uintptr_t)&mbsnrtowcs},
    {"wcsnrtombs", (uintptr_t)&wcsnrtombs},
    {"swprintf", (uintptr_t)&swprintf},

    /* ---- stdio ---- */
    {"printf", (uintptr_t)&printf},
    {"fprintf", (uintptr_t)&fprintf},
    {"sprintf", (uintptr_t)&sprintf},
    {"snprintf", (uintptr_t)&snprintf},
    {"vprintf", (uintptr_t)&vprintf},
    {"vfprintf", (uintptr_t)&vfprintf},
    {"vsprintf", (uintptr_t)&vsprintf},
    {"vsnprintf", (uintptr_t)&vsnprintf},
    {"vasprintf", (uintptr_t)&vasprintf},
    {"sscanf", (uintptr_t)&sscanf},
    {"fscanf", (uintptr_t)&fscanf},
    {"vsscanf", (uintptr_t)&vsscanf},
    {"fopen", (uintptr_t)&fopen_fake},
    {"fclose", (uintptr_t)&fclose},
    {"fdopen", (uintptr_t)&fdopen},
    {"freopen", (uintptr_t)&freopen},
    {"fflush", (uintptr_t)&fflush},
    {"fread", (uintptr_t)&fread},
    {"fwrite", (uintptr_t)&fwrite},
    {"fgets", (uintptr_t)&fgets},
    {"fputs", (uintptr_t)&fputs},
    {"fputc", (uintptr_t)&fputc},
    {"fseek", (uintptr_t)&fseek},
    {"ftell", (uintptr_t)&ftell},
    {"feof", (uintptr_t)&feof},
    {"ferror", (uintptr_t)&ferror},
    {"clearerr", (uintptr_t)&clearerr},
    {"setvbuf", (uintptr_t)&setvbuf},
    {"getc", (uintptr_t)&getc},
    {"putc", (uintptr_t)&putc},
    {"putchar", (uintptr_t)&putchar},
    {"puts", (uintptr_t)&puts},
    {"ungetc", (uintptr_t)&ungetc},
    {"tmpfile", (uintptr_t)&tmpfile},
    {"tmpnam", (uintptr_t)&tmpnam},

    /* ---- POSIX I/O ---- */
    {"open", (uintptr_t)&open},
    {"close", (uintptr_t)&close},
    {"read", (uintptr_t)&read},
    {"write", (uintptr_t)&write},
    {"lseek", (uintptr_t)&lseek},
    {"access", (uintptr_t)&access},
    {"mkdir", (uintptr_t)&mkdir},
    {"rmdir", (uintptr_t)&rmdir},
    {"unlink", (uintptr_t)&unlink},
    {"rename", (uintptr_t)&rename},
    {"remove", (uintptr_t)&remove},
    {"stat", (uintptr_t)&stat},
    {"lstat", (uintptr_t)&lstat},
    {"fstat", (uintptr_t)&fstat},
    {"chdir", (uintptr_t)&chdir},
    {"getcwd", (uintptr_t)&getcwd},
    {"opendir", (uintptr_t)&opendir},
    {"readdir", (uintptr_t)&readdir},
    {"closedir", (uintptr_t)&closedir},
    {"fcntl", (uintptr_t)&fcntl},
    {"pipe", (uintptr_t)&pipe},
    {"mktemp", (uintptr_t)&mktemp},
    {"select", (uintptr_t)&select},
    {"system", (uintptr_t)&system},
    {"sysconf", (uintptr_t)&sysconf},
    {"syscall", (uintptr_t)&syscall},

    /* ---- stdlib ---- */
    {"abort", (uintptr_t)&abort},
    {"exit", (uintptr_t)&exit},
    {"getenv", (uintptr_t)&getenv},
    {"qsort", (uintptr_t)&qsort},
    {"bsearch", (uintptr_t)&bsearch},
    {"rand", (uintptr_t)&rand},
    {"srand", (uintptr_t)&srand},

    /* ---- math ---- */
    {"acos", (uintptr_t)&acos},
    {"acosf", (uintptr_t)&acosf},
    {"asin", (uintptr_t)&asin},
    {"asinf", (uintptr_t)&asinf},
    {"atan", (uintptr_t)&atan},
    {"atanf", (uintptr_t)&atanf},
    {"atan2", (uintptr_t)&atan2},
    {"atan2f", (uintptr_t)&atan2f},
    {"cos", (uintptr_t)&cos},
    {"cosf", (uintptr_t)&cosf},
    {"cosh", (uintptr_t)&cosh},
    {"sin", (uintptr_t)&sin},
    {"sinf", (uintptr_t)&sinf},
    {"sinh", (uintptr_t)&sinh},
    {"sincos", (uintptr_t)&sincos},
    {"sincosf", (uintptr_t)&sincosf},
    {"tan", (uintptr_t)&tan},
    {"tanf", (uintptr_t)&tanf},
    {"tanh", (uintptr_t)&tanh},
    {"exp", (uintptr_t)&exp},
    {"log", (uintptr_t)&log},
    {"log10", (uintptr_t)&log10},
    {"log10f", (uintptr_t)&log10f},
    {"pow", (uintptr_t)&pow},
    {"sqrt", (uintptr_t)&sqrt},
    {"sqrtf", (uintptr_t)&sqrtf},
    {"fmod", (uintptr_t)&fmod},
    {"fmodf", (uintptr_t)&fmodf},
    {"modf", (uintptr_t)&modf},
    {"floor", (uintptr_t)&floor},
    {"floorf", (uintptr_t)&floorf},
    {"ceil", (uintptr_t)&ceil},
    {"ceilf", (uintptr_t)&ceilf},
    {"ldexp", (uintptr_t)&ldexp},
    {"frexp", (uintptr_t)&frexp},

    /* ---- time ---- */
    {"time", (uintptr_t)&time},
    {"clock", (uintptr_t)&clock},
    {"clock_gettime", (uintptr_t)&clock_gettime},
    {"gettimeofday", (uintptr_t)&gettimeofday},
    {"localtime", (uintptr_t)&localtime},
    {"gmtime", (uintptr_t)&gmtime},
    {"mktime", (uintptr_t)&mktime},
    {"difftime", (uintptr_t)&difftime},
    {"strftime", (uintptr_t)&strftime},
    {"nanosleep", (uintptr_t)&nanosleep},
    {"usleep", (uintptr_t)&usleep},

    /* ---- locale ---- */
    {"setlocale", (uintptr_t)&setlocale},
    {"localeconv", (uintptr_t)&localeconv},
    {"newlocale", (uintptr_t)&newlocale},
    {"uselocale", (uintptr_t)&uselocale},
    {"freelocale", (uintptr_t)&freelocale},

    /* ---- setjmp / longjmp ---- */
    {"setjmp", (uintptr_t)&setjmp},
    {"longjmp", (uintptr_t)&longjmp},

    /* ---- syslog ---- */
    {"openlog", (uintptr_t)&openlog},
    {"closelog", (uintptr_t)&closelog},
    {"syslog", (uintptr_t)&syslog},

    /* ---- networking ---- */
    {"socket", (uintptr_t)&socket},
    {"bind", (uintptr_t)&bind},
    {"listen", (uintptr_t)&listen},
    {"accept", (uintptr_t)&accept},
    {"connect", (uintptr_t)&connect},
    {"sendto", (uintptr_t)&sendto},
    {"recvfrom", (uintptr_t)&recvfrom},
    {"shutdown", (uintptr_t)&shutdown},
    {"setsockopt", (uintptr_t)&setsockopt},
    {"gethostbyname", (uintptr_t)&gethostbyname},
    {"gethostname", (uintptr_t)&gethostname},
    {"inet_ntoa", (uintptr_t)&inet_ntoa},

    /* ---- EGL (our shim) ---- */
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
    {"eglGetError", (uintptr_t)&egl_shim_GetError},
    {"eglGetProcAddress", (uintptr_t)&egl_shim_GetProcAddress},

    /* ---- OpenGL ES 1.x (real GL functions) ---- */
    {"glEnable", (uintptr_t)&glEnable},
    {"glDisable", (uintptr_t)&glDisable},
    {"glClear", (uintptr_t)&glClear},
    {"glClearColorx", (uintptr_t)&glClearColorx},
    {"glClearDepthx", (uintptr_t)&glClearDepthx},
    {"glClearDepthf", (uintptr_t)&glClearDepthf},
    {"glClearStencil", (uintptr_t)&glClearStencil},
    {"glViewport", (uintptr_t)&glViewport},
    {"glScissor", (uintptr_t)&glScissor},
    {"glDepthFunc", (uintptr_t)&glDepthFunc},
    {"glDepthMask", (uintptr_t)&glDepthMask},
    {"glBlendFunc", (uintptr_t)&glBlendFunc},
    {"glCullFace", (uintptr_t)&glCullFace},
    {"glColorMask", (uintptr_t)&glColorMask},
    {"glHint", (uintptr_t)&glHint},
    {"glGetIntegerv", (uintptr_t)&glGetIntegerv},
    {"glGetString", (uintptr_t)&glGetString},
    {"glPixelStorei", (uintptr_t)&glPixelStorei},
    {"glReadPixels", (uintptr_t)&glReadPixels},
    {"glBindTexture", (uintptr_t)&glBindTexture},
    {"glGenTextures", (uintptr_t)&glGenTextures},
    {"glDeleteTextures", (uintptr_t)&glDeleteTextures},
    {"glTexImage2D", (uintptr_t)&glTexImage2D},
    {"glTexSubImage2D", (uintptr_t)&glTexSubImage2D},
    {"glTexParameteri", (uintptr_t)&glTexParameteri},
    {"glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D},
    {"glCompressedTexSubImage2D", (uintptr_t)&glCompressedTexSubImage2D},
    {"glCopyTexSubImage2D", (uintptr_t)&glCopyTexSubImage2D},
    {"glMatrixMode", (uintptr_t)&glMatrixMode},
    {"glLoadMatrixx", (uintptr_t)&glLoadMatrixx},
    {"glLoadMatrixf", (uintptr_t)&glLoadMatrixf},
    {"glPushMatrix", (uintptr_t)&glPushMatrix},
    {"glPopMatrix", (uintptr_t)&glPopMatrix},
    {"glVertexPointer", (uintptr_t)&glVertexPointer},
    {"glNormalPointer", (uintptr_t)&glNormalPointer},
    {"glColorPointer", (uintptr_t)&glColorPointer},
    {"glTexCoordPointer", (uintptr_t)&glTexCoordPointer},
    {"glEnableClientState", (uintptr_t)&glEnableClientState},
    {"glDisableClientState", (uintptr_t)&glDisableClientState},
    {"glDrawElements", (uintptr_t)&glDrawElements},
    {"glColor4x", (uintptr_t)&glColor4x},
    {"glLightx", (uintptr_t)&glLightx},
    {"glLightxv", (uintptr_t)&glLightxv},
    {"glLightModelxv", (uintptr_t)&glLightModelxv},
    {"glMaterialx", (uintptr_t)&glMaterialx},
    {"glMaterialxv", (uintptr_t)&glMaterialxv},
    {"glShadeModel", (uintptr_t)&glShadeModel},
    {"glTexEnvx", (uintptr_t)&glTexEnvx},
    {"glTexEnvxv", (uintptr_t)&glTexEnvxv},
    {"glAlphaFuncx", (uintptr_t)&glAlphaFuncx},

    /* ---- OpenSL ES (our shim) ---- */
    {"slCreateEngine", (uintptr_t)&slCreateEngine_shim},
    {"SL_IID_ENGINE", (uintptr_t)&sl_IID_ENGINE},
    {"SL_IID_PLAY", (uintptr_t)&sl_IID_PLAY},
    {"SL_IID_VOLUME", (uintptr_t)&sl_IID_VOLUME},
    {"SL_IID_BUFFERQUEUE", (uintptr_t)&sl_IID_BUFFERQUEUE},
    {"SL_IID_EFFECTSEND", (uintptr_t)&sl_IID_EFFECTSEND},

    /* ---- Android NDK (our shim) ---- */
    {"ANativeActivity_finish", (uintptr_t)&ANativeActivity_finish},
    {"ALooper_prepare", (uintptr_t)&ALooper_prepare},
    {"ALooper_addFd", (uintptr_t)&ALooper_addFd},
    {"ALooper_pollAll", (uintptr_t)&ALooper_pollAll},
    {"AInputQueue_attachLooper", (uintptr_t)&AInputQueue_attachLooper},
    {"AInputQueue_detachLooper", (uintptr_t)&AInputQueue_detachLooper},
    {"AInputQueue_getEvent", (uintptr_t)&AInputQueue_getEvent},
    {"AInputQueue_preDispatchEvent", (uintptr_t)&AInputQueue_preDispatchEvent},
    {"AInputQueue_finishEvent", (uintptr_t)&AInputQueue_finishEvent},
    {"AInputEvent_getType", (uintptr_t)&AInputEvent_getType},
    {"AKeyEvent_getAction", (uintptr_t)&AKeyEvent_getAction},
    {"AKeyEvent_getKeyCode", (uintptr_t)&AKeyEvent_getKeyCode},
    {"AMotionEvent_getX", (uintptr_t)&AMotionEvent_getX},
    {"AMotionEvent_getY", (uintptr_t)&AMotionEvent_getY},
    {"AMotionEvent_getAction", (uintptr_t)&AMotionEvent_getAction},
    {"AMotionEvent_getPointerCount", (uintptr_t)&AMotionEvent_getPointerCount},
    {"AMotionEvent_getPointerId", (uintptr_t)&AMotionEvent_getPointerId},
    {"AMotionEvent_getAxisValue", (uintptr_t)&AMotionEvent_getAxisValue},
    {"AInputEvent_getSource", (uintptr_t)&AInputEvent_getSource},
    {"AConfiguration_new", (uintptr_t)&AConfiguration_new},
    {"AConfiguration_delete", (uintptr_t)&AConfiguration_delete},
    {"AConfiguration_fromAssetManager", (uintptr_t)&AConfiguration_fromAssetManager},
    {"AConfiguration_setLocale", (uintptr_t)&AConfiguration_setLocale},
    {"AConfiguration_getLanguage", (uintptr_t)&AConfiguration_getLanguage},
    {"AConfiguration_getCountry", (uintptr_t)&AConfiguration_getCountry},
    {"AConfiguration_getDensity", (uintptr_t)&AConfiguration_getDensity},
    {"AConfiguration_getOrientation", (uintptr_t)&AConfiguration_getOrientation},
    {"AConfiguration_setOrientation", (uintptr_t)&AConfiguration_setOrientation},
    {"AConfiguration_getScreenSize", (uintptr_t)&AConfiguration_getScreenSize},
    {"ASensorManager_getInstance", (uintptr_t)&ASensorManager_getInstance},
    {"ASensorManager_getDefaultSensor", (uintptr_t)&ASensorManager_getDefaultSensor},
    {"ASensorManager_createEventQueue", (uintptr_t)&ASensorManager_createEventQueue},
    {"ASensorEventQueue_enableSensor", (uintptr_t)&ASensorEventQueue_enableSensor},
    {"ASensorEventQueue_setEventRate", (uintptr_t)&ASensorEventQueue_setEventRate},
};

size_t dynlib_numfunctions =
    sizeof(dynlib_functions) / sizeof(*dynlib_functions);
