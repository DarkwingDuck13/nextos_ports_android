#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <locale.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <GLES2/gl2.h>
#include <SDL2/SDL.h>

#include "android_shim.h"
#include "egl_shim.h"
#include "imports.h"
#include "opensles_shim.h"
#include "so_util.h"
#include "util.h"

#undef feof
#undef ferror

#undef feof
#undef ferror
static uint8_t fake_sF[3][0x100];
static uint64_t __stack_chk_guard_fake = 0x4242424242424242;

/* A engine usa stdio BIONIC: stdin/out/err == &__sF[0/1/2] (mapeado p/ fake_sF).
 * Nossas fopen/fread/... sao do glibc e rejeitam um FILE* fake ("invalid stdio
 * handle" -> abort). Traduz qualquer ponteiro dentro de fake_sF p/ o stream
 * glibc real (proven SOTN). */
static FILE *map_sf(void *f) {
  uintptr_t p = (uintptr_t)f, base = (uintptr_t)fake_sF;
  if (p >= base && p < base + sizeof(fake_sF)) {
    int idx = (int)((p - base) / 0x100);
    return idx == 0 ? stdin : idx == 1 ? stdout : stderr;
  }
  return (FILE *)f;
}
static int sf_fprintf(void *f, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int r = vfprintf(map_sf(f), fmt, ap); va_end(ap); return r;
}
static int sf_vfprintf(void *f, const char *fmt, va_list ap) { return vfprintf(map_sf(f), fmt, ap); }
static int sf_fputc(int c, void *f) { return fputc(c, map_sf(f)); }
static int sf_fputs(const char *s, void *f) { return fputs(s, map_sf(f)); }
static size_t sf_fwrite(const void *p, size_t sz, size_t n, void *f) { return fwrite(p, sz, n, map_sf(f)); }
static int sf_fflush(void *f) { return fflush(f ? map_sf(f) : NULL); }
static int sf_ferror(void *f) { return ferror(map_sf(f)); }
static int sf_feof(void *f) { return feof(map_sf(f)); }
static int sf_fileno(void *f) { return fileno(map_sf(f)); }
static int sf_fgetc(void *f) { return fgetc(map_sf(f)); }
static char *sf_fgets(char *s, int n, void *f) { return fgets(s, n, map_sf(f)); }
extern uintptr_t __cxa_atexit;
extern uintptr_t __cxa_finalize;

extern void *text_base;
static void __stack_chk_fail_stub(void) {
  uintptr_t tls = 0;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
  uintptr_t g = tls ? *(uintptr_t *)(tls + 0x28) : 0;
  void *ra = __builtin_return_address(0);
  debugPrintf("__stack_chk_fail! caller=%p (libchrono+0x%lx) tls+0x28=0x%lx\n",
              ra, (unsigned long)((uintptr_t)ra - (uintptr_t)text_base),
              (unsigned long)g);
}

static int *__errno_fake(void) { return &errno; }

static unsigned long limbo_off(void *ra) {
  return text_base ? (unsigned long)((uintptr_t)ra - (uintptr_t)text_base) : 0;
}

static void log_audio_trace(const char *kind, const char *tag,
                            const char *text, void *ra0, void *ra1, void *ra2,
                            void *ra3) {
  if (!getenv("LIMBO_LOGTRACE"))
    return;
  if ((!tag || !strstr(tag, "Limbo-Audio")) &&
      (!text || (!strstr(text, "AKSound") && !strstr(text, "Audio base") &&
                 !strstr(text, "Sound Engine"))))
    return;

  debugPrintf("LOGTRACE %s tag=%s ra0=%p/+0x%lx ra1=%p/+0x%lx ra2=%p/+0x%lx "
              "ra3=%p/+0x%lx text=\"%s\"\n",
              kind, tag ? tag : "(null)", ra0, limbo_off(ra0), ra1,
              limbo_off(ra1), ra2, limbo_off(ra2), ra3, limbo_off(ra3),
              text ? text : "(null)");
}

int __android_log_print_fake(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
  va_list list;
  char string[0x1000];
  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);
  debugPrintf("LOG [%s]: %s\n", tag, string);
  log_audio_trace("print", tag, string, __builtin_return_address(0),
                  __builtin_return_address(1), __builtin_return_address(2),
                  __builtin_return_address(3));
  return 0;
}

int __android_log_write_fake(int prio, const char *tag, const char *text) {
  (void)prio;
  debugPrintf("LOG [%s]: %s\n", tag, text);
  log_audio_trace("write", tag, text, __builtin_return_address(0),
                  __builtin_return_address(1), __builtin_return_address(2),
                  __builtin_return_address(3));
  return 0;
}

void android_set_abort_message_fake(const char *msg) {
  debugPrintf("android_set_abort_message: %s\n", msg ? msg : "(null)");
}

static void abort_fake(void) {
  debugPrintf("abort() called by guest\n");
  _exit(134);
}

static void libcxx_verbose_abort_fake(const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt ? fmt : "(null)", ap);
  va_end(ap);
  debugPrintf("__libcpp_verbose_abort: %s\n", msg);
  _exit(134);
}

static int kill_fake(pid_t pid, int sig) {
  void *ra = __builtin_return_address(0);
  if (sig == SIGSEGV) {
    debugPrintf("guest kill(pid=%d, sig=%d) caller=%p libLimbo+0x%lx -> suppressed\n",
                (int)pid, sig, ra,
                text_base ? (unsigned long)((uintptr_t)ra - (uintptr_t)text_base) : 0);
    return 0;
  }
  return kill(pid, sig);
}

static int pthread_kill_fake(pthread_t thread, int sig) {
  void *ra = __builtin_return_address(0);
  if (sig == SIGSEGV) {
    debugPrintf("guest pthread_kill(thread=%lu, sig=%d) caller=%p libLimbo+0x%lx -> suppressed\n",
                (unsigned long)thread, sig, ra,
                text_base ? (unsigned long)((uintptr_t)ra - (uintptr_t)text_base) : 0);
    return 0;
  }
  return pthread_kill(thread, sig);
}

void *__memcpy_chk(void *dst, const void *src, size_t n, size_t dst_len) {
  (void)dst_len;
  return memcpy(dst, src, n);
}

void *__memmove_chk(void *dst, const void *src, size_t n, size_t dst_len) {
  (void)dst_len;
  return memmove(dst, src, n);
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

char *__strchr_chk(const char *s, int c, size_t slen) {
  (void)slen;
  return strchr(s, c);
}

char *__strncpy_chk2(char *dst, const char *src, size_t n, size_t dst_len,
                     size_t src_len) {
  (void)dst_len;
  (void)src_len;
  return strncpy(dst, src, n);
}

int __vsprintf_chk(char *dst, int flags, size_t dst_len, const char *fmt,
                   va_list ap) {
  (void)flags;
  (void)dst_len;
  return vsprintf(dst, fmt, ap);
}

int __vsnprintf_chk(char *dst, size_t supplied_size, int flags, size_t dst_len,
                    const char *fmt, va_list ap) {
  (void)flags;
  (void)dst_len;
  return vsnprintf(dst, supplied_size, fmt, ap);
}

ssize_t __read_chk(int fd, void *buf, size_t count, size_t buf_size) {
  (void)buf_size;
  return read(fd, buf, count);
}

void __FD_CLR_chk(int fd, fd_set *set, size_t setlen) {
  (void)setlen;
  FD_CLR(fd, set);
}
void __FD_SET_chk(int fd, fd_set *set, size_t setlen) {
  (void)setlen;
  FD_SET(fd, set);
}
int __FD_ISSET_chk(int fd, fd_set *set, size_t setlen) {
  (void)setlen;
  return FD_ISSET(fd, set);
}

static FILE *my_fopen(const char *pathname, const char *mode) {
  return fopen(resolve_android_path(pathname), mode);
}

static int my_open(const char *pathname, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
    return open(resolve_android_path(pathname), flags, mode);
  }
  return open(resolve_android_path(pathname), flags);
}

static int my_open_2(const char *pathname, int flags) {
  return open(resolve_android_path(pathname), flags);
}

static int mkdir_fake(const char *pathname, mode_t mode) {
  const char *resolved = resolve_android_path(pathname);
  int ret = mkdir(resolved, mode);
  if (ret == 0)
    debugPrintf("mkdir(\"%s\" -> \"%s\", 0%o) = 0\n", pathname, resolved,
                (unsigned)mode);
  else
    debugPrintf("mkdir(\"%s\" -> \"%s\", 0%o) = -1 (errno=%d: %s)\n", pathname,
                resolved, (unsigned)mode, errno, strerror(errno));
  return ret;
}

static int remove_fake(const char *pathname) {
  const char *resolved = resolve_android_path(pathname);
  int ret = remove(resolved);
  if (ret == 0)
    debugPrintf("remove(\"%s\" -> \"%s\") = 0\n", pathname, resolved);
  else
    debugPrintf("remove(\"%s\" -> \"%s\") = -1 (errno=%d: %s)\n", pathname,
                resolved, errno, strerror(errno));
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
    debugPrintf("rename(\"%s\" -> \"%s\", \"%s\" -> \"%s\") = 0\n", oldpath,
                resolved_old, newpath, resolved_new);
  } else {
    debugPrintf(
        "rename(\"%s\" -> \"%s\", \"%s\" -> \"%s\") = -1 (errno=%d: %s)\n",
        oldpath, resolved_old, newpath, resolved_new, errno, strerror(errno));
  }
  return ret;
}

// LSW PTHREAD HACKS

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

typedef struct HostSemEntry {
  void *guest_addr;
  sem_t sem;
  struct HostSemEntry *next;
} HostSemEntry;

static HostMutexEntry *g_mutex_entries = NULL;
static HostCondEntry *g_cond_entries = NULL;
static HostSemEntry *g_sem_entries = NULL;
static pthread_mutex_t g_mutex_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_cond_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_sem_registry_lock = PTHREAD_MUTEX_INITIALIZER;
uintptr_t g_limbo_audio_obj = 0;
uintptr_t g_limbo_audio_sem = 0;

static int semlog_enabled(void) { return getenv("LIMBO_SEMLOG") != NULL; }

static int semlog_all(void) {
  const char *v = getenv("LIMBO_SEMLOG");
  return v && strcmp(v, "all") == 0;
}

static int is_audio_sem(void *sem) {
  return g_limbo_audio_sem && (uintptr_t)sem == g_limbo_audio_sem;
}

static uint32_t limbo_audio_u32(size_t off) {
  uint32_t v = 0;
  if (g_limbo_audio_obj)
    memcpy(&v, (void *)(g_limbo_audio_obj + off), sizeof(v));
  return v;
}

static uintptr_t limbo_audio_ptr(size_t off) {
  uintptr_t v = 0;
  if (g_limbo_audio_obj)
    memcpy(&v, (void *)(g_limbo_audio_obj + off), sizeof(v));
  return v;
}

static void mark_limbo_audio_sem(void *sem, void *ra) {
  unsigned long caller = limbo_off(ra);
  if (caller < 0x1ad640 || caller > 0x1ad660)
    return;

  g_limbo_audio_sem = (uintptr_t)sem;
  if ((uintptr_t)sem >= 0x198)
    g_limbo_audio_obj = (uintptr_t)sem - 0x198;
  if (semlog_enabled()) {
    debugPrintf("LIMBO_SEM audio-sem detected sem=%p audio_obj=%p "
                "caller=+0x%lx\n",
                sem, (void *)g_limbo_audio_obj, caller);
  }
}

static void log_limbo_sem(const char *op, void *sem, int ret, void *ra) {
  if (!semlog_enabled())
    return;
  if (!is_audio_sem(sem) && !semlog_all())
    return;

  debugPrintf("LIMBO_SEM %s sem=%p audio=%s ret=%d caller=+0x%lx "
              "audio_obj=%p queue_head=%p free=%p tail=%p count=%u cap=%u\n",
              op, sem, is_audio_sem(sem) ? "yes" : "no", ret,
              limbo_off(ra), (void *)g_limbo_audio_obj,
              (void *)limbo_audio_ptr(472), (void *)limbo_audio_ptr(488),
              (void *)limbo_audio_ptr(496), limbo_audio_u32(508),
              limbo_audio_u32(504));
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

int pthread_mutex_init_fake(pthread_mutex_t *uid, const int *mutexattr) {
  return lookup_host_mutex(uid, 1) ? 0 : -1;
}
int pthread_mutex_destroy_fake(pthread_mutex_t *uid) {
  return destroy_host_mutex(uid);
}
int pthread_mutex_lock_fake(pthread_mutex_t *uid) {
  return pthread_mutex_lock(lookup_host_mutex(uid, 1));
}
int pthread_mutex_trylock_fake(pthread_mutex_t *uid) {
  return pthread_mutex_trylock(lookup_host_mutex(uid, 1));
}
int pthread_mutex_unlock_fake(pthread_mutex_t *uid) {
  return pthread_mutex_unlock(lookup_host_mutex(uid, 1));
}

int pthread_cond_init_fake(pthread_cond_t *cnd, const int *condattr) {
  return lookup_host_cond(cnd, 1) ? 0 : -1;
}
int pthread_cond_destroy_fake(pthread_cond_t *cnd) {
  return destroy_host_cond(cnd);
}
int pthread_cond_wait_fake(pthread_cond_t *cnd, pthread_mutex_t *mtx) {
  return pthread_cond_wait(lookup_host_cond(cnd, 1), lookup_host_mutex(mtx, 1));
}
int pthread_cond_timedwait_fake(pthread_cond_t *cnd, pthread_mutex_t *mtx,
                                const struct timespec *t) {
  return pthread_cond_timedwait(lookup_host_cond(cnd, 1),
                                lookup_host_mutex(mtx, 1), t);
}
int pthread_cond_signal_fake(pthread_cond_t *cnd) {
  return pthread_cond_signal(lookup_host_cond(cnd, 1));
}
int pthread_cond_broadcast_fake(pthread_cond_t *cnd) {
  return pthread_cond_broadcast(lookup_host_cond(cnd, 1));
}

static sem_t *lookup_host_sem(void *guest_addr, int create, unsigned value) {
  if (!guest_addr)
    guest_addr = (void *)0x1;
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
  entry->guest_addr = guest_addr;
  sem_init(&entry->sem, 0, value);
  entry->next = g_sem_entries;
  g_sem_entries = entry;
  pthread_mutex_unlock(&g_sem_registry_lock);
  return &entry->sem;
}

static int destroy_host_sem(void *guest_addr) {
  pthread_mutex_lock(&g_sem_registry_lock);
  HostSemEntry **link = &g_sem_entries;
  while (*link) {
    HostSemEntry *entry = *link;
    if (entry->guest_addr == guest_addr) {
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

static int sem_init_fake(sem_t *sem, int pshared, unsigned value) {
  (void)pshared;
  lookup_host_sem(sem, 1, value);
  void *ra = __builtin_return_address(0);
  mark_limbo_audio_sem(sem, ra);
  if (semlog_enabled() && (is_audio_sem(sem) || semlog_all())) {
    debugPrintf("LIMBO_SEM init sem=%p audio=%s value=%u caller=+0x%lx\n",
                sem, is_audio_sem(sem) ? "yes" : "no", value,
                limbo_off(ra));
  }
  return 0;
}

static int sem_destroy_fake(sem_t *sem) {
  int ret = destroy_host_sem(sem);
  log_limbo_sem("destroy", sem, ret, __builtin_return_address(0));
  return ret;
}

static int sem_post_fake(sem_t *sem) {
  int ret = sem_post(lookup_host_sem(sem, 1, 0));
  log_limbo_sem("post", sem, ret, __builtin_return_address(0));
  return ret;
}

static int sem_wait_fake(sem_t *sem) {
  log_limbo_sem("wait-enter", sem, 0, __builtin_return_address(0));
  int ret = sem_wait(lookup_host_sem(sem, 1, 0));
  log_limbo_sem("wait-leave", sem, ret, __builtin_return_address(0));
  return ret;
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
  if (getenv("LIMBO_THREADLOG"))
    debugPrintf("pthread thread start entry=%p/+0x%lx arg=%p\n", entry,
                limbo_off((void *)entry), arg);
  void *ret = entry(arg);
  if (getenv("LIMBO_THREADLOG"))
    debugPrintf("pthread thread end entry=%p/+0x%lx arg=%p ret=%p\n", entry,
                limbo_off((void *)entry), arg, ret);
  return ret;
}
int pthread_create_fake(pthread_t *thread, const void *attr, void *entry,
                        void *arg) {
  egl_shim_release_current_if_pbuffer();
  if (getenv("LIMBO_THREADLOG"))
    debugPrintf("pthread_create_fake thread=%p attr=%p entry=%p/+0x%lx "
                "arg=%p caller=+0x%lx\n",
                (void *)thread, attr, entry, limbo_off(entry), arg,
                limbo_off(__builtin_return_address(0)));
  ThreadWrapper *w = malloc(sizeof(ThreadWrapper));
  w->entry = entry;
  w->arg = arg;
  pthread_attr_t real_attr;
  pthread_attr_init(&real_attr);
  pthread_attr_setstacksize(&real_attr, 2 * 1024 * 1024);
  int ret = pthread_create(thread, &real_attr, thread_wrapper_func, w);
  pthread_attr_destroy(&real_attr);
  if (ret != 0)
    free(w);
  if (getenv("LIMBO_THREADLOG"))
    debugPrintf("pthread_create_fake -> %d host_thread=%lu entry=%p/+0x%lx\n",
                ret, thread ? (unsigned long)*thread : 0, entry,
                limbo_off(entry));
  return ret;
}

/* pthread_attr_t bionic (56B) < glibc (64B). A engine aloca o attr na PILHA
 * (tamanho bionic); o glibc pthread_attr_init/setschedparam escreveria 64B ->
 * estoura o buffer -> corrompe a stack-canary (visto em SQEX sead
 * DelegateManager::Initialize). Como pthread_create_fake IGNORA o attr passado
 * (usa um attr glibc proprio), estes podem ser no-ops seguros. */
static int my_pthread_attr_init_noop(void *attr) {
  (void)attr;
  return 0;
}
static int my_pthread_attr_setschedparam_noop(void *attr, const void *param) {
  (void)attr; (void)param;
  return 0;
}
static int my_pthread_attr_setstacksize_noop(void *attr, size_t stacksize) {
  (void)attr; (void)stacksize;
  return 0;
}
static int my_pthread_attr_setdetachstate_noop(void *attr, int state) {
  (void)attr; (void)state;
  return 0;
}
static int my_pthread_attr_destroy_noop(void *attr) {
  (void)attr;
  return 0;
}

static void *pthread_getspecific_fake(pthread_key_t key) {
  return pthread_getspecific(key);
}
static int pthread_setspecific_fake(pthread_key_t key, const void *value) {
  return pthread_setspecific(key, value);
}
int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  return pthread_once((pthread_once_t *)once_control, init_routine);
}

// AASSETMANAGER EMULATION
// this bit here redirects apk reads directly to the assets folder

typedef struct {
  FILE *f;
  size_t size;
} FakeAsset;

typedef struct {
  DIR *d;
} FakeAssetDir;

void *AAssetManager_fromJava_fake(void *env, void *assetManager) {
  (void)env;
  (void)assetManager;
  return (void *)0x1337;
}

void *AAssetManager_open_fake(void *mgr, const char *filename, int mode) {
  (void)mgr;
  (void)mode;
  char path[1024];

  const char *relative_path = filename;
  if (strncmp(filename, "assets/", 7) == 0) {
    relative_path = filename + 7;
  } else if (strncmp(filename, "./assets/", 9) == 0) {
    relative_path = filename + 9;
  }

  snprintf(path, sizeof(path), "./assets/%s", relative_path);
  FILE *f = fopen(path, "rb");
  if (!f) {
    f = fopen(resolve_android_path(filename), "rb");
  }

  /* NAO logar por padrao: o jogo abre resources.bin centenas de vezes/frame e
     debugPrintf faz fopen+fclose de debug.log a cada chamada -> stall brutal de
     I/O no main loop -> decode de audio morre de fome (gagueira). CHRONO_ASSETLOG=1
     reativa p/ debug. */
  if (getenv("CHRONO_ASSETLOG"))
    debugPrintf("AAssetManager_open(\"%s\" -> \"%s\") = %p\n", filename, path, f);

  if (!f)
    return NULL;

  FakeAsset *asset = malloc(sizeof(FakeAsset));
  asset->f = f;
  fseek(f, 0, SEEK_END);
  asset->size = ftell(f);
  fseek(f, 0, SEEK_SET);
  return asset;
}

int AAsset_read_fake(void *asset, void *buf, size_t count) {
  if (!asset)
    return -1;
  FakeAsset *a = (FakeAsset *)asset;
  return fread(buf, 1, count, a->f);
}

void AAsset_close_fake(void *asset) {
  if (!asset)
    return;
  FakeAsset *a = (FakeAsset *)asset;
  fclose(a->f);
  free(a);
}

off_t AAsset_getLength_fake(void *asset) {
  if (!asset)
    return 0;
  FakeAsset *a = (FakeAsset *)asset;
  return a->size;
}

off_t AAsset_getRemainingLength_fake(void *asset) {
  if (!asset)
    return 0;
  FakeAsset *a = (FakeAsset *)asset;
  off_t cur = ftell(a->f);
  return a->size - cur;
}

off_t AAsset_seek_fake(void *asset, off_t offset, int whence) {
  if (!asset)
    return -1;
  FakeAsset *a = (FakeAsset *)asset;
  fseek(a->f, offset, whence);
  return ftell(a->f);
}

void *AAssetManager_openDir_fake(void *mgr, const char *dirName) {
  (void)mgr;
  char path[1024];
  snprintf(path, sizeof(path), "./assets/%s", dirName);
  DIR *d = opendir(path);
  if (!d)
    return NULL;
  FakeAssetDir *adir = malloc(sizeof(FakeAssetDir));
  adir->d = d;
  return adir;
}

const char *AAssetDir_getNextFileName_fake(void *assetDir) {
  if (!assetDir)
    return NULL;
  FakeAssetDir *ad = (FakeAssetDir *)assetDir;
  struct dirent *ent = readdir(ad->d);
  if (!ent)
    return NULL;
  return ent->d_name;
}

void AAssetDir_close_fake(void *assetDir) {
  if (!assetDir)
    return;
  FakeAssetDir *ad = (FakeAssetDir *)assetDir;
  closedir(ad->d);
  free(ad);
}

size_t __ctype_get_mb_cur_max_fake(void) { return 4; }

int dl_iterate_phdr_fake(void *callback, void *data) { return 0; }
static unsigned long getauxval_stub(unsigned long type) { return 0; }
static void sincos_stub(double x, double *s, double *c) {
  *s = sin(x);
  *c = cos(x);
}
static void sincosf_stub(float x, float *s, float *c) {
  *s = sinf(x);
  *c = cosf(x);
}

static const unsigned char *limbo_glGetString(unsigned name) {
  switch (name) {
  case 0x1F00:
    return (const unsigned char *)"ARM";
  case 0x1F01:
    return (const unsigned char *)"Mali-450 MP";
  case 0x1F02:
    return (const unsigned char *)"OpenGL ES 2.0";
  case 0x8B8C:
    return (const unsigned char *)"OpenGL ES GLSL ES 1.00";
  case 0x1F03:
    return (const unsigned char *)
        "GL_OES_texture_npot GL_OES_vertex_array_object "
        "GL_OES_compressed_ETC1_RGB8_texture "
        "GL_EXT_compressed_ETC1_RGB8_sub_texture "
        "GL_OES_standard_derivatives GL_OES_EGL_image GL_OES_depth24 "
        "GL_ARM_rgba8 GL_OES_depth_texture GL_OES_packed_depth_stencil "
        "GL_EXT_texture_format_BGRA8888 GL_OES_rgb8_rgba8 "
        "GL_OES_mapbuffer GL_EXT_map_buffer_range "
        "GL_EXT_texture_rg GL_EXT_texture_storage "
        "GL_APPLE_texture_max_level "
        "GL_OES_required_internalformat GL_EXT_discard_framebuffer "
        "GL_EXT_debug_label";
  default: {
    static const unsigned char *(*real)(unsigned) = NULL;
    if (!real)
      real = dlsym(RTLD_DEFAULT, "glGetString");
    const unsigned char *r = real ? real(name) : NULL;
    return r ? r : (const unsigned char *)"";
  }
  }
}

static void limbo_glGetIntegerv(unsigned pname, int *params);
static void limbo_glDiscardFramebufferEXT(unsigned target, int numAttachments,
                                          const unsigned *attachments);
static void limbo_glLabelObjectEXT(unsigned type, unsigned object, int length,
                                   const char *label);
static void limbo_glPushGroupMarkerEXT(int length, const char *marker);
static void limbo_glPopGroupMarkerEXT(void);
static unsigned limbo_glCheckFramebufferStatus(unsigned target);
static void limbo_glFramebufferTexture2D(unsigned target, unsigned attachment,
                                         unsigned textarget, unsigned texture,
                                         int level);
static void limbo_glFramebufferRenderbuffer(unsigned target,
                                            unsigned attachment,
                                            unsigned renderbuffertarget,
                                            unsigned renderbuffer);
static void limbo_glRenderbufferStorage(unsigned target, unsigned internalformat,
                                        int width, int height);
static void limbo_glTexImage2D(unsigned target, int level, int internalformat,
                               int width, int height, int border,
                               unsigned format, unsigned type,
                               const void *pixels);
static void limbo_glTexSubImage2D(unsigned target, int level, int xoffset,
                                  int yoffset, int width, int height,
                                  unsigned format, unsigned type,
                                  const void *pixels);

void *limbo_gl_proc_override(const char *name) {
  if (name && strcmp(name, "glGetString") == 0)
    return (void *)limbo_glGetString;
  if (name && strcmp(name, "glGetIntegerv") == 0)
    return (void *)limbo_glGetIntegerv;
  if (name && strcmp(name, "glDiscardFramebufferEXT") == 0)
    return (void *)limbo_glDiscardFramebufferEXT;
  if (name && strcmp(name, "glLabelObjectEXT") == 0)
    return (void *)limbo_glLabelObjectEXT;
  if (name && strcmp(name, "glPushGroupMarkerEXT") == 0)
    return (void *)limbo_glPushGroupMarkerEXT;
  if (name && strcmp(name, "glPopGroupMarkerEXT") == 0)
    return (void *)limbo_glPopGroupMarkerEXT;
  if (name && strcmp(name, "glCheckFramebufferStatus") == 0)
    return (void *)limbo_glCheckFramebufferStatus;
  if (name && strcmp(name, "glFramebufferTexture2D") == 0)
    return (void *)limbo_glFramebufferTexture2D;
  if (name && strcmp(name, "glFramebufferRenderbuffer") == 0)
    return (void *)limbo_glFramebufferRenderbuffer;
  if (name && strcmp(name, "glRenderbufferStorage") == 0)
    return (void *)limbo_glRenderbufferStorage;
  if (name && strcmp(name, "glTexImage2D") == 0)
    return (void *)limbo_glTexImage2D;
  if (name && strcmp(name, "glTexSubImage2D") == 0)
    return (void *)limbo_glTexSubImage2D;
  return NULL;
}

static void limbo_glGetIntegerv(unsigned pname, int *params) {
  if (!params)
    return;

  switch (pname) {
  case 0x86A2: /* GL_NUM_COMPRESSED_TEXTURE_FORMATS */
  case 0x8DF9: /* GL_NUM_SHADER_BINARY_FORMATS */
  case 0x87FE: /* GL_NUM_PROGRAM_BINARY_FORMATS */
  case 0x86A3: /* GL_COMPRESSED_TEXTURE_FORMATS */
  case 0x8DF8: /* GL_SHADER_BINARY_FORMATS */
  case 0x87FF: /* GL_PROGRAM_BINARY_FORMATS */
    params[0] = 0;
    return;
  default:
    glGetIntegerv(pname, params);
    return;
  }
}

static void limbo_glDiscardFramebufferEXT(unsigned target, int numAttachments,
                                          const unsigned *attachments) {
  (void)target; (void)numAttachments; (void)attachments;
}

static void limbo_glLabelObjectEXT(unsigned type, unsigned object, int length,
                                   const char *label) {
  (void)type; (void)object; (void)length; (void)label;
}

static void limbo_glPushGroupMarkerEXT(int length, const char *marker) {
  (void)length; (void)marker;
}

static void limbo_glPopGroupMarkerEXT(void) {}

static int limbo_fbolog_enabled(void) { return getenv("LIMBO_FBOLOG") != NULL; }

static unsigned limbo_gl_drain_errors(void) {
  unsigned first = glGetError();
  if (first == 0)
    return 0;
  for (int i = 0; i < 15; i++) {
    if (glGetError() == 0)
      break;
  }
  return first;
}

static const char *limbo_gl_enum_name(unsigned v) {
  switch (v) {
  case 0: return "GL_NONE";
  case 0x1401: return "GL_UNSIGNED_BYTE";
  case 0x1403: return "GL_UNSIGNED_SHORT";
  case 0x140B: return "GL_HALF_FLOAT_OES";
  case 0x1702: return "GL_TEXTURE";
  case 0x1902: return "GL_DEPTH_COMPONENT";
  case 0x1906: return "GL_ALPHA";
  case 0x1907: return "GL_RGB";
  case 0x1908: return "GL_RGBA";
  case 0x1909: return "GL_LUMINANCE";
  case 0x190A: return "GL_LUMINANCE_ALPHA";
  case 0x8056: return "GL_RGBA4";
  case 0x8057: return "GL_RGB5_A1";
  case 0x81A5: return "GL_DEPTH_COMPONENT16";
  case 0x8363: return "GL_UNSIGNED_SHORT_5_6_5";
  case 0x8364: return "GL_UNSIGNED_SHORT_4_4_4_4";
  case 0x8366: return "GL_UNSIGNED_SHORT_5_5_5_1";
  case 0x881A: return "GL_RGBA16F_EXT";
  case 0x881B: return "GL_RGB16F_EXT";
  case 0x88F0: return "GL_DEPTH24_STENCIL8_OES";
  case 0x8C40: return "GL_SRGB_EXT";
  case 0x8C41: return "GL_SRGB_ALPHA_EXT";
  case 0x8CD5: return "GL_FRAMEBUFFER_COMPLETE";
  case 0x8CD6: return "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
  case 0x8CD7: return "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
  case 0x8CD9: return "GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS";
  case 0x8CDD: return "GL_FRAMEBUFFER_UNSUPPORTED";
  case 0x8CE0: return "GL_COLOR_ATTACHMENT0";
  case 0x8D00: return "GL_DEPTH_ATTACHMENT";
  case 0x8D20: return "GL_STENCIL_ATTACHMENT";
  case 0x8D40: return "GL_FRAMEBUFFER";
  case 0x8D41: return "GL_RENDERBUFFER";
  case 0x8D48: return "GL_STENCIL_INDEX8";
  default: return "?";
  }
}

static void limbo_log_fbo_attachment(unsigned target, unsigned attachment) {
  int type = 0, name = 0;
  glGetFramebufferAttachmentParameteriv(
      target, attachment, 0x8CD0, &type); /* OBJECT_TYPE */
  glGetFramebufferAttachmentParameteriv(
      target, attachment, 0x8CD1, &name); /* OBJECT_NAME */
  debugPrintf("LIMBO_FBO attach %s type=%s(0x%x) name=%d\n",
              limbo_gl_enum_name(attachment), limbo_gl_enum_name(type), type,
              name);
}

static unsigned limbo_glCheckFramebufferStatus(unsigned target) {
  limbo_gl_drain_errors();
  unsigned status = glCheckFramebufferStatus(target);
  if (status != 0x8CD5 || limbo_fbolog_enabled()) {
    debugPrintf("LIMBO_FBO check target=%s(0x%x) -> %s(0x%x)\n",
                limbo_gl_enum_name(target), target, limbo_gl_enum_name(status),
                status);
    limbo_log_fbo_attachment(target, 0x8CE0);
    limbo_log_fbo_attachment(target, 0x8D00);
    limbo_log_fbo_attachment(target, 0x8D20);
  }
  return status;
}

static void limbo_glFramebufferTexture2D(unsigned target, unsigned attachment,
                                         unsigned textarget, unsigned texture,
                                         int level) {
  glFramebufferTexture2D(target, attachment, textarget, texture, level);
  if (limbo_fbolog_enabled()) {
    debugPrintf("LIMBO_FBO texture target=%s attachment=%s textarget=0x%x "
                "texture=%u level=%d\n",
                limbo_gl_enum_name(target), limbo_gl_enum_name(attachment),
                textarget, texture, level);
  }
}

static void limbo_glFramebufferRenderbuffer(unsigned target,
                                            unsigned attachment,
                                            unsigned renderbuffertarget,
                                            unsigned renderbuffer) {
  glFramebufferRenderbuffer(target, attachment, renderbuffertarget,
                            renderbuffer);
  if (limbo_fbolog_enabled()) {
    debugPrintf("LIMBO_FBO renderbuffer target=%s attachment=%s rbtarget=%s "
                "renderbuffer=%u\n",
                limbo_gl_enum_name(target), limbo_gl_enum_name(attachment),
                limbo_gl_enum_name(renderbuffertarget), renderbuffer);
  }
}

static void limbo_glRenderbufferStorage(unsigned target, unsigned internalformat,
                                        int width, int height) {
  glRenderbufferStorage(target, internalformat, width, height);
  unsigned err = limbo_gl_drain_errors();
  if (limbo_fbolog_enabled()) {
    debugPrintf("LIMBO_FBO rb-storage target=%s format=%s(0x%x) %dx%d "
                "err=0x%x\n",
                limbo_gl_enum_name(target), limbo_gl_enum_name(internalformat),
                internalformat, width, height, err);
  }
}

static int limbo_is_legacy_luma_format(unsigned format) {
  return format == 0x1906 || format == 0x1909 || format == 0x190A;
}

static void *limbo_convert_luma_pixels(unsigned format, int width, int height,
                                       const void *pixels) {
  if (!pixels || width <= 0 || height <= 0 || !limbo_is_legacy_luma_format(format))
    return NULL;

  size_t count = (size_t)width * (size_t)height;
  if ((size_t)width != count / (size_t)height || count > SIZE_MAX / 4)
    return NULL;

  unsigned char *rgba = malloc(count * 4);
  if (!rgba)
    return NULL;

  const unsigned char *src = (const unsigned char *)pixels;
  for (size_t i = 0; i < count; i++) {
    if (format == 0x190A) {
      unsigned char l = src[i * 2 + 0];
      unsigned char a = src[i * 2 + 1];
      rgba[i * 4 + 0] = l;
      rgba[i * 4 + 1] = l;
      rgba[i * 4 + 2] = l;
      rgba[i * 4 + 3] = a;
    } else if (format == 0x1909) {
      unsigned char l = src[i];
      rgba[i * 4 + 0] = l;
      rgba[i * 4 + 1] = l;
      rgba[i * 4 + 2] = l;
      rgba[i * 4 + 3] = 255;
    } else {
      unsigned char a = src[i];
      rgba[i * 4 + 0] = 0;
      rgba[i * 4 + 1] = 0;
      rgba[i * 4 + 2] = 0;
      rgba[i * 4 + 3] = a;
    }
  }
  return rgba;
}

static void limbo_glTexImage2D(unsigned target, int level, int internalformat,
                               int width, int height, int border,
                               unsigned format, unsigned type,
                               const void *pixels) {
  unsigned upload_internal = (unsigned)internalformat;
  unsigned upload_format = format;
  void *converted = NULL;

  if (limbo_is_legacy_luma_format((unsigned)internalformat) &&
      format == (unsigned)internalformat && type == 0x1401 && width > 0 &&
      height > 0) {
    upload_internal = 0x1908;
    upload_format = 0x1908;
    if (pixels) {
      converted = limbo_convert_luma_pixels((unsigned)internalformat, width,
                                            height, pixels);
      pixels = converted;
    }
  }

  if (converted || upload_internal != (unsigned)internalformat ||
      upload_format != format) {
    glTexImage2D(target, level, (int)upload_internal, width, height, border,
                 upload_format, type, pixels);
  } else {
    glTexImage2D(target, level, internalformat, width, height, border, format,
                 type, pixels);
  }
  if (limbo_fbolog_enabled()) {
    unsigned err = limbo_gl_drain_errors();
    debugPrintf("LIMBO_FBO tex-image target=0x%x level=%d internal=%s(0x%x) "
                "%dx%d format=%s(0x%x) upload=%s(0x%x) type=%s(0x%x) "
                "converted=%s err=0x%x\n",
                target, level, limbo_gl_enum_name((unsigned)internalformat),
                (unsigned)internalformat, width, height,
                limbo_gl_enum_name(format), format,
                limbo_gl_enum_name(upload_internal), upload_internal,
                limbo_gl_enum_name(type), type, converted ? "yes" : "no",
                err);
  }
  if (!limbo_fbolog_enabled())
    limbo_gl_drain_errors();
  free(converted);
}

static void limbo_glTexSubImage2D(unsigned target, int level, int xoffset,
                                  int yoffset, int width, int height,
                                  unsigned format, unsigned type,
                                  const void *pixels) {
  unsigned upload_format = format;
  void *converted = NULL;

  if (limbo_is_legacy_luma_format(format) && type == 0x1401 && width > 0 &&
      height > 0) {
    upload_format = 0x1908;
    converted = limbo_convert_luma_pixels(format, width, height, pixels);
    if (pixels && !converted) {
      if (limbo_fbolog_enabled()) {
        debugPrintf("LIMBO_FBO tex-subimage target=0x%x level=%d %dx%d "
                    "format=%s(0x%x) conversion failed\n",
                    target, level, width, height, limbo_gl_enum_name(format),
                    format);
      }
      return;
    }
    pixels = converted;
  }

  glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                  upload_format, type, pixels);
  unsigned err = limbo_gl_drain_errors();
  if (limbo_fbolog_enabled()) {
    debugPrintf("LIMBO_FBO tex-subimage target=0x%x level=%d xy=%d,%d %dx%d "
                "format=%s(0x%x) upload=%s(0x%x) type=%s(0x%x) "
                "converted=%s err=0x%x\n",
                target, level, xoffset, yoffset, width, height,
                limbo_gl_enum_name(format), format,
                limbo_gl_enum_name(upload_format), upload_format,
                limbo_gl_enum_name(type), type, converted ? "yes" : "no",
                err);
  }
  free(converted);
}

static int limbo_dl_self_token;
static int limbo_dl_android_token;
static char limbo_dl_error[160];

static void limbo_dl_set_error(const char *fmt, const char *name) {
  snprintf(limbo_dl_error, sizeof(limbo_dl_error), fmt, name ? name : "(null)");
}

static const char *limbo_dl_basename(const char *path) {
  const char *slash;
  if (!path)
    return NULL;
  slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static int limbo_dl_is_android_lib(const char *name) {
  const char *base = limbo_dl_basename(name);
  if (!base)
    return 1;
  return strstr(base, "libEGL") || strstr(base, "libGLES") ||
         strstr(base, "libOpenSLES") || strstr(base, "libandroid") ||
         strstr(base, "liblog") || strstr(base, "libc++_shared") ||
         strstr(base, "libLimbo") || strstr(base, "libaaaaaaaa");
}

static void *limbo_dlopen(const char *filename, int flags) {
  (void)flags;
  if (getenv("LIMBO_DLLOG"))
    debugPrintf("dlopen(\"%s\", 0x%x) ra=+0x%lx\n",
                filename ? filename : "(null)", flags,
                limbo_off(__builtin_return_address(0)));

  if (!filename)
    return &limbo_dl_self_token;

  const char *base = limbo_dl_basename(filename);
  if (base && strstr(base, "libaaudio") && !getenv("LIMBO_ALLOW_AAUDIO")) {
    limbo_dl_set_error("dlopen disabled Android AAudio backend: %s", filename);
    return NULL;
  }

  if (limbo_dl_is_android_lib(filename))
    return &limbo_dl_android_token;

  /* Most Android dlopen calls only need a non-null handle followed by dlsym.
   * Falling back to a fake handle avoids feeding Android sonames to glibc. */
  return &limbo_dl_android_token;
}

static void *limbo_dlsym(void *handle, const char *symbol) {
  (void)handle;
  if (!symbol) {
    limbo_dl_set_error("dlsym missing symbol: %s", symbol);
    return NULL;
  }

  void *ov = limbo_gl_proc_override(symbol);
  if (ov) {
    if (getenv("LIMBO_DLLOG"))
      debugPrintf("dlsym(%p, \"%s\") -> %p ra=+0x%lx\n", handle, symbol, ov,
                  limbo_off(__builtin_return_address(0)));
    return ov;
  }

  for (int i = 0; i < dynlib_functions_count; i++) {
    if (strcmp(symbol, dynlib_functions[i].symbol) == 0) {
      if (getenv("LIMBO_DLLOG"))
        debugPrintf("dlsym(%p, \"%s\") -> %p ra=+0x%lx\n", handle, symbol,
                    (void *)dynlib_functions[i].func,
                    limbo_off(__builtin_return_address(0)));
      return (void *)dynlib_functions[i].func;
    }
  }

  uintptr_t aux = so_aux_find_export(symbol);
  if (aux) {
    if (getenv("LIMBO_DLLOG"))
      debugPrintf("dlsym(%p, \"%s\") -> %p ra=+0x%lx\n", handle, symbol,
                  (void *)aux, limbo_off(__builtin_return_address(0)));
    return (void *)aux;
  }

  void *gl = SDL_GL_GetProcAddress(symbol);
  if (gl) {
    if (getenv("LIMBO_DLLOG"))
      debugPrintf("dlsym(%p, \"%s\") -> %p ra=+0x%lx\n", handle, symbol, gl,
                  limbo_off(__builtin_return_address(0)));
    return gl;
  }

  void *real = dlsym(RTLD_DEFAULT, symbol);
  if (real) {
    if (getenv("LIMBO_DLLOG"))
      debugPrintf("dlsym(%p, \"%s\") -> %p ra=+0x%lx\n", handle, symbol,
                  real, limbo_off(__builtin_return_address(0)));
    return real;
  }

  limbo_dl_set_error("dlsym unresolved: %s", symbol);
  if (getenv("LIMBO_DLLOG"))
    debugPrintf("dlsym(%p, \"%s\") -> NULL ra=+0x%lx\n", handle, symbol,
                limbo_off(__builtin_return_address(0)));
  return NULL;
}

static int limbo_dlclose(void *handle) {
  if (getenv("LIMBO_DLLOG"))
    debugPrintf("dlclose(%p) -> 0 ra=+0x%lx\n", handle,
                limbo_off(__builtin_return_address(0)));
  return 0;
}

static char *limbo_dlerror(void) {
  if (getenv("LIMBO_DLLOG"))
    debugPrintf("dlerror() -> %s ra=+0x%lx\n",
                limbo_dl_error[0] ? limbo_dl_error : "(null)",
                limbo_off(__builtin_return_address(0)));
  return limbo_dl_error[0] ? limbo_dl_error : NULL;
}

static long double strtold_l_stub(const char *nptr, char **endptr, void *loc) {
  return strtold(nptr, endptr);
}
static long long strtoll_l_stub(const char *nptr, char **endptr, int base,
                                void *loc) {
  return strtoll(nptr, endptr, base);
}
static unsigned long long strtoull_l_stub(const char *nptr, char **endptr,
                                          int base, void *loc) {
  return strtoull(nptr, endptr, base);
}
static void *newlocale_stub(int category_mask, const char *locale, void *base) {
  return NULL;
}
static void freelocale_stub(void *locobj) {}
static void *uselocale_stub(void *newloc) { return NULL; }
static int initgroups_stub(const char *user, gid_t group) { return 0; }
static int sysinfo_stub(void *info) { return 0; }
static void syslog_stub(int priority, const char *format, ...) {}
static void closelog_stub(void) {}
static void openlog_stub(const char *ident, int option, int facility) {}
static long syscall_fake(long number, ...) {
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

  if (number == 131 && a3 == SIGSEGV) {
    void *ra = __builtin_return_address(0);
    debugPrintf("guest syscall tgkill(pid=%ld, tid=%ld, sig=%ld) caller=%p libLimbo+0x%lx -> suppressed\n",
                a1, a2, a3, ra,
                text_base ? (unsigned long)((uintptr_t)ra - (uintptr_t)text_base) : 0);
    return 0;
  }
  return syscall(number, a1, a2, a3, a4, a5, a6);
}
static int tcgetattr_stub(int fd, void *termios_p) { return 0; }
static int tcsetattr_stub(int fd, int optional_actions, const void *termios_p) {
  return 0;
}
static int mlock_stub(const void *addr, size_t len) { return 0; }
static int cxa_thread_atexit_stub(void (*dtor)(void *), void *obj,
                                  void *dso_symbol) {
  (void)dtor; (void)obj; (void)dso_symbol;
  return 0;
}
static void cxa_pure_virtual_stub(void) { abort(); }
static int pthread_setschedparam_fake(pthread_t thread, int policy,
                                      const void *param) {
  (void)thread; (void)policy; (void)param;
  return 0;
}
static void *funopen_stub(const void *cookie,
                          int (*readfn)(void *, char *, int),
                          int (*writefn)(void *, const char *, int),
                          long (*seekfn)(void *, long, int),
                          int (*closefn)(void *)) {
  return NULL;
}
static int sigsetjmp_stub(void *env, int savesigs) { return 0; }
static void siglongjmp_stub(void *env, int val) {}

static int sigaction_fake(int signum, const void *act, void *oldact) {
  (void)signum;
  (void)act;
  (void)oldact;
  return 0;
}

/* ---- stubs bionic-only (glibc nao tem; senao slot vira PLT0 -> crash) ---- */
static int my___system_property_get(const char *name, char *value) {
  (void)name;
  if (value) value[0] = '\0';
  return 0;
}
static void my___android_log_assert(const char *cond, const char *tag,
                                    const char *fmt, ...) {
  (void)cond; (void)tag; (void)fmt;
  debugPrintf("[android_log_assert] cond=%s tag=%s\n", cond ? cond : "?",
              tag ? tag : "?");
  /* nao aborta: deixa o engine seguir (boot). */
}

DynLibFunction dynlib_functions[] = {
    {"__system_property_get", (uintptr_t)&my___system_property_get},
    {"__android_log_assert", (uintptr_t)&my___android_log_assert},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&sl_IID_BUFFERQUEUE},
    {"SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&sl_IID_ANDROIDCONFIGURATION},
    {"abort", (uintptr_t)&abort_fake},
    {"accept", (uintptr_t)&accept},
    {"acos", (uintptr_t)&acos},
    {"acosf", (uintptr_t)&acosf},
    {"__android_log_print", (uintptr_t)&__android_log_print_fake},
    {"__android_log_write", (uintptr_t)&__android_log_write_fake},
    {"android_set_abort_message", (uintptr_t)&android_set_abort_message_fake},
    {"asin", (uintptr_t)&asin},
    {"atan", (uintptr_t)&atan},
    {"atan2f", (uintptr_t)&atan2f},
    {"atanf", (uintptr_t)&atanf},
    {"atof", (uintptr_t)&atof},
    {"atoi", (uintptr_t)&atoi},
    {"atoll", (uintptr_t)&atoll},
    {"bind", (uintptr_t)&bind},
    {"btowc", (uintptr_t)&btowc},
    {"calloc", (uintptr_t)&calloc},
    {"clock_gettime", (uintptr_t)&clock_gettime},
    {"close", (uintptr_t)&close},
    {"closedir", (uintptr_t)&closedir},
    {"closelog", (uintptr_t)&closelog_stub},
    {"connect", (uintptr_t)&connect},
    {"cos", (uintptr_t)&cos},
    {"cosf", (uintptr_t)&cosf},
    {"__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake},
    {"__cxa_atexit", (uintptr_t)&__cxa_atexit},
    {"__cxa_finalize", (uintptr_t)&__cxa_finalize},
    {"__cxa_pure_virtual", (uintptr_t)&cxa_pure_virtual_stub},
    {"__cxa_thread_atexit", (uintptr_t)&cxa_thread_atexit_stub},
    {"_ZNSt6__ndk122__libcpp_verbose_abortEPKcz", (uintptr_t)&libcxx_verbose_abort_fake},
    {"dlclose", (uintptr_t)&limbo_dlclose},
    {"dlerror", (uintptr_t)&limbo_dlerror},
    {"dl_iterate_phdr", (uintptr_t)&dl_iterate_phdr_fake},
    {"dlopen", (uintptr_t)&limbo_dlopen},
    {"dlsym", (uintptr_t)&limbo_dlsym},
    {"__errno", (uintptr_t)&__errno_fake},
    {"_exit", (uintptr_t)&_exit},
    {"exit", (uintptr_t)&exit},
    {"exp", (uintptr_t)&exp},
    {"expf", (uintptr_t)&expf},
    {"fclose", (uintptr_t)&fclose},
    {"fcntl", (uintptr_t)&fcntl},
    {"__FD_CLR_chk", (uintptr_t)&__FD_CLR_chk},
    {"__FD_ISSET_chk", (uintptr_t)&__FD_ISSET_chk},
    {"__FD_SET_chk", (uintptr_t)&__FD_SET_chk},
    {"feof", (uintptr_t)&sf_feof},
    {"ferror", (uintptr_t)&sf_ferror},
    {"fflush", (uintptr_t)&sf_fflush},
    {"fgetc", (uintptr_t)&sf_fgetc},
    {"fgets", (uintptr_t)&sf_fgets},
    {"fileno", (uintptr_t)&sf_fileno},
    {"fmodf", (uintptr_t)&fmodf},
    {"fopen", (uintptr_t)&my_fopen},
    {"fprintf", (uintptr_t)&sf_fprintf},
    {"fputc", (uintptr_t)&sf_fputc},
    {"fputs", (uintptr_t)&sf_fputs},
    {"fread", (uintptr_t)&fread},
    {"free", (uintptr_t)&free},
    {"freeaddrinfo", (uintptr_t)&freeaddrinfo},
    {"freelocale", (uintptr_t)&freelocale_stub},
    {"fseek", (uintptr_t)&fseek},
    {"fseeko", (uintptr_t)&fseeko},
    {"fstat", (uintptr_t)&fstat},
    {"fstat64", (uintptr_t)&fstat},
    {"ftell", (uintptr_t)&ftell},
    {"ftello", (uintptr_t)&ftello},
    {"ftruncate", (uintptr_t)&ftruncate},
    {"funopen", (uintptr_t)&funopen_stub},
    {"fwrite", (uintptr_t)&sf_fwrite},
    {"gai_strerror", (uintptr_t)&gai_strerror},
    {"getaddrinfo", (uintptr_t)&getaddrinfo},
    {"getauxval", (uintptr_t)&getauxval_stub},
    {"getenv", (uintptr_t)&getenv},
    {"gethostbyname", (uintptr_t)&gethostbyname},
    {"getnameinfo", (uintptr_t)&getnameinfo},
    {"getpagesize", (uintptr_t)&getpagesize},
    {"getpeername", (uintptr_t)&getpeername},
    {"getpid", (uintptr_t)&getpid},
    {"getpwuid", (uintptr_t)&getpwuid},
    {"getrlimit", (uintptr_t)&getrlimit},
    {"getsockname", (uintptr_t)&getsockname},
    {"getsockopt", (uintptr_t)&getsockopt},
    {"gettimeofday", (uintptr_t)&gettimeofday},
    {"getuid", (uintptr_t)&getuid},

    {"glActiveTexture", (uintptr_t)&glActiveTexture},
    {"glAttachShader", (uintptr_t)&glAttachShader},
    {"glBindAttribLocation", (uintptr_t)&glBindAttribLocation},
    {"glBindBuffer", (uintptr_t)&glBindBuffer},
    {"glBindFramebuffer", (uintptr_t)&glBindFramebuffer},
    {"glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer},
    {"glBindTexture", (uintptr_t)&glBindTexture},
    {"glBlendFunc", (uintptr_t)&glBlendFunc},
    {"glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate},
    {"glBufferData", (uintptr_t)&glBufferData},
    {"glBufferSubData", (uintptr_t)&glBufferSubData},
    {"glCheckFramebufferStatus", (uintptr_t)&limbo_glCheckFramebufferStatus},
    {"glClear", (uintptr_t)&glClear},
    {"glClearColor", (uintptr_t)&glClearColor},
    {"glClearDepthf", (uintptr_t)&glClearDepthf},
    {"glClearStencil", (uintptr_t)&glClearStencil},
    {"glColorMask", (uintptr_t)&glColorMask},
    {"glCompileShader", (uintptr_t)&glCompileShader},
    {"glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D},
    {"glCopyTexImage2D", (uintptr_t)&glCopyTexImage2D},
    {"glCreateProgram", (uintptr_t)&glCreateProgram},
    {"glCreateShader", (uintptr_t)&glCreateShader},
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
    {"glDrawArrays", (uintptr_t)&glDrawArrays},
    {"glDrawElements", (uintptr_t)&glDrawElements},
    {"glEnable", (uintptr_t)&glEnable},
    {"glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray},
    {"glFinish", (uintptr_t)&glFinish},
    {"glFramebufferRenderbuffer", (uintptr_t)&limbo_glFramebufferRenderbuffer},
    {"glFramebufferTexture2D", (uintptr_t)&limbo_glFramebufferTexture2D},
    {"glFrontFace", (uintptr_t)&glFrontFace},
    {"glGenBuffers", (uintptr_t)&glGenBuffers},
    {"glGenerateMipmap", (uintptr_t)&glGenerateMipmap},
    {"glGenFramebuffers", (uintptr_t)&glGenFramebuffers},
    {"glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers},
    {"glGenTextures", (uintptr_t)&glGenTextures},
    {"glGetFramebufferAttachmentParameteriv",
     (uintptr_t)&glGetFramebufferAttachmentParameteriv},
    {"glGetIntegerv", (uintptr_t)&limbo_glGetIntegerv},
    {"glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog},
    {"glGetProgramiv", (uintptr_t)&glGetProgramiv},
    {"glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog},
    {"glGetShaderiv", (uintptr_t)&glGetShaderiv},
    {"glGetString", (uintptr_t)&limbo_glGetString},
    {"glGetActiveUniform", (uintptr_t)&glGetActiveUniform},
    {"glGetUniformLocation", (uintptr_t)&glGetUniformLocation},
    {"glLinkProgram", (uintptr_t)&glLinkProgram},
    {"glDetachShader", (uintptr_t)&glDetachShader},
    {"glPixelStorei", (uintptr_t)&glPixelStorei},
    {"glReadPixels", (uintptr_t)&glReadPixels},
    {"glRenderbufferStorage", (uintptr_t)&limbo_glRenderbufferStorage},
    {"glScissor", (uintptr_t)&glScissor},
    {"glShaderSource", (uintptr_t)&glShaderSource},
    {"glStencilFunc", (uintptr_t)&glStencilFunc},
    {"glStencilMask", (uintptr_t)&glStencilMask},
    {"glStencilOp", (uintptr_t)&glStencilOp},
    {"glTexImage2D", (uintptr_t)&limbo_glTexImage2D},
    {"glTexParameteri", (uintptr_t)&glTexParameteri},
    {"glTexSubImage2D", (uintptr_t)&limbo_glTexSubImage2D},
    {"glUniform1fv", (uintptr_t)&glUniform1fv},
    {"glUniform1f", (uintptr_t)&glUniform1f},
    {"glUniform1i", (uintptr_t)&glUniform1i},
    {"glUniform2fv", (uintptr_t)&glUniform2fv},
    {"glUniform3fv", (uintptr_t)&glUniform3fv},
    {"glUniform4fv", (uintptr_t)&glUniform4fv},
    {"glUniformMatrix2fv", (uintptr_t)&glUniformMatrix2fv},
    {"glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv},
    {"glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv},
    {"glUseProgram", (uintptr_t)&glUseProgram},
    {"glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer},
    {"glViewport", (uintptr_t)&glViewport},

    {"gmtime_r", (uintptr_t)&gmtime_r},
    {"inet_ntop", (uintptr_t)&inet_ntop},
    {"inet_pton", (uintptr_t)&inet_pton},
    {"initgroups", (uintptr_t)&initgroups_stub},
    {"ioctl", (uintptr_t)&ioctl},
    {"isalnum", (uintptr_t)&isalnum},
    {"isalpha", (uintptr_t)&isalpha},
    {"islower", (uintptr_t)&islower},
    {"isspace", (uintptr_t)&isspace},
    {"isupper", (uintptr_t)&isupper},
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
    {"isxdigit", (uintptr_t)&isxdigit},
    {"kill", (uintptr_t)&kill_fake},
    {"ldexp", (uintptr_t)&ldexp},
    {"ldexpf", (uintptr_t)&ldexpf},
    {"listen", (uintptr_t)&listen},
    {"localeconv", (uintptr_t)&localeconv},
    {"localtime", (uintptr_t)&localtime},
    {"localtime_r", (uintptr_t)&localtime_r},
    {"log", (uintptr_t)&log},
    {"log10", (uintptr_t)&log10},
    {"log10f", (uintptr_t)&log10f},
    {"logf", (uintptr_t)&logf},
    {"lseek", (uintptr_t)&lseek},
    {"madvise", (uintptr_t)&madvise},
    {"malloc", (uintptr_t)&malloc},
    {"mbrlen", (uintptr_t)&mbrlen},
    {"mbrtowc", (uintptr_t)&mbrtowc},
    {"mbsnrtowcs", (uintptr_t)&mbsnrtowcs},
    {"mbsrtowcs", (uintptr_t)&mbsrtowcs},
    {"mbstowcs", (uintptr_t)&mbstowcs},
    {"mbtowc", (uintptr_t)&mbtowc},
    {"memchr", (uintptr_t)&memchr},
    {"memcmp", (uintptr_t)&memcmp},
    {"memcpy", (uintptr_t)&memcpy},
    {"__memcpy_chk", (uintptr_t)&__memcpy_chk},
    {"memmove", (uintptr_t)&memmove},
    {"__memmove_chk", (uintptr_t)&__memmove_chk},
    {"memset", (uintptr_t)&memset},
    {"mkdir", (uintptr_t)&mkdir_fake},
    {"mktime", (uintptr_t)&mktime},
    {"mlock", (uintptr_t)&mlock_stub},
    {"mmap", (uintptr_t)&mmap},
    {"mmap64", (uintptr_t)&mmap},
    {"modf", (uintptr_t)&modf},
    {"mprotect", (uintptr_t)&mprotect},
    {"munmap", (uintptr_t)&munmap},
    {"nanosleep", (uintptr_t)&nanosleep},
    {"newlocale", (uintptr_t)&newlocale_stub},
    {"open", (uintptr_t)&my_open},
    {"__open_2", (uintptr_t)&my_open_2},
    {"opendir", (uintptr_t)&opendir},
    {"openlog", (uintptr_t)&openlog_stub},
    {"perror", (uintptr_t)&perror},
    {"pipe", (uintptr_t)&pipe},
    {"poll", (uintptr_t)&poll},
    {"posix_memalign", (uintptr_t)&posix_memalign},
    {"pow", (uintptr_t)&pow},
    {"powf", (uintptr_t)&powf},
    {"printf", (uintptr_t)&printf},

    {"pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake},
    {"pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake},
    {"pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake},
    {"pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake},
    {"pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake},
    {"pthread_create", (uintptr_t)&pthread_create_fake},
    {"pthread_attr_init", (uintptr_t)&my_pthread_attr_init_noop},
    {"pthread_attr_setschedparam", (uintptr_t)&my_pthread_attr_setschedparam_noop},
    {"pthread_attr_setstacksize", (uintptr_t)&my_pthread_attr_setstacksize_noop},
    {"pthread_attr_setdetachstate", (uintptr_t)&my_pthread_attr_setdetachstate_noop},
    {"pthread_attr_destroy", (uintptr_t)&my_pthread_attr_destroy_noop},
    {"pthread_detach", (uintptr_t)&pthread_detach},
    {"pthread_equal", (uintptr_t)&pthread_equal},
    {"pthread_exit", (uintptr_t)&pthread_exit},
    {"pthread_getspecific", (uintptr_t)&pthread_getspecific_fake},
    {"pthread_join", (uintptr_t)&pthread_join},
    {"pthread_key_create", (uintptr_t)&pthread_key_create},
    {"pthread_key_delete", (uintptr_t)&pthread_key_delete},
    {"pthread_kill", (uintptr_t)&pthread_kill_fake},
    {"pthread_mutexattr_destroy", (uintptr_t)&ret0},
    {"pthread_mutexattr_init", (uintptr_t)&ret0},
    {"pthread_mutexattr_settype", (uintptr_t)&ret0},
    {"pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake},
    {"pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake},
    {"pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake},
    {"pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake},
    {"pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake},
    {"pthread_once", (uintptr_t)&pthread_once_fake},
    {"pthread_rwlock_destroy", (uintptr_t)&pthread_rwlock_destroy},
    {"pthread_rwlock_init", (uintptr_t)&pthread_rwlock_init},
    {"pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock},
    {"pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock},
    {"pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock},
    {"pthread_self", (uintptr_t)&pthread_self},
    {"pthread_setspecific", (uintptr_t)&pthread_setspecific_fake},
    {"pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake},

    {"puts", (uintptr_t)&puts},
    {"qsort", (uintptr_t)&qsort},
    {"rand", (uintptr_t)&rand},
    {"read", (uintptr_t)&read},
    {"__read_chk", (uintptr_t)&__read_chk},
    {"readdir", (uintptr_t)&readdir},
    {"realloc", (uintptr_t)&realloc},
    {"recv", (uintptr_t)&recv},
    {"recvfrom", (uintptr_t)&recvfrom},
    {"remove", (uintptr_t)&remove_fake},
    {"rename", (uintptr_t)&rename_fake},
    {"rewind", (uintptr_t)&rewind},
    {"rmdir", (uintptr_t)&rmdir},
    {"sched_yield", (uintptr_t)&sched_yield},
    {"select", (uintptr_t)&select},
    {"sem_destroy", (uintptr_t)&sem_destroy_fake},
    {"sem_init", (uintptr_t)&sem_init_fake},
    {"sem_post", (uintptr_t)&sem_post_fake},
    {"sem_wait", (uintptr_t)&sem_wait_fake},
    {"send", (uintptr_t)&send},
    {"sendto", (uintptr_t)&sendto},
    {"setgid", (uintptr_t)&setgid},
    {"setlocale", (uintptr_t)&setlocale},
    {"setsockopt", (uintptr_t)&setsockopt},
    {"setuid", (uintptr_t)&setuid},
    {"__sF", (uintptr_t)&fake_sF},
    {"shutdown", (uintptr_t)&shutdown},
    {"sigaction", (uintptr_t)&sigaction_fake},
    {"sigaddset", (uintptr_t)&sigaddset},
    {"sigdelset", (uintptr_t)&sigdelset},
    {"sigemptyset", (uintptr_t)&sigemptyset},
    {"sigfillset", (uintptr_t)&sigfillset},
    {"siglongjmp", (uintptr_t)&siglongjmp_stub},
    {"signal", (uintptr_t)&signal},
    {"sigprocmask", (uintptr_t)&sigprocmask},
    {"sigsetjmp", (uintptr_t)&sigsetjmp_stub},
    {"sin", (uintptr_t)&sin},
    {"sincos", (uintptr_t)&sincos_stub},
    {"sincosf", (uintptr_t)&sincosf_stub},
    {"sinf", (uintptr_t)&sinf},

    {"slCreateEngine", (uintptr_t)&slCreateEngine_shim},
    {"SL_IID_BUFFERQUEUE", (uintptr_t)&sl_IID_BUFFERQUEUE},
    {"SL_IID_ENGINE", (uintptr_t)&sl_IID_ENGINE},
    {"SL_IID_ENVIRONMENTALREVERB", (uintptr_t)&sl_IID_ENVIRONMENTALREVERB},
    {"SL_IID_PLAY", (uintptr_t)&sl_IID_PLAY},
    {"SL_IID_VOLUME", (uintptr_t)&sl_IID_VOLUME},

    {"snprintf", (uintptr_t)&snprintf},
    {"socket", (uintptr_t)&socket},
    {"sprintf", (uintptr_t)&sprintf},
    {"srand", (uintptr_t)&srand},
    {"sscanf", (uintptr_t)&sscanf},
    {"__stack_chk_fail", (uintptr_t)&__stack_chk_fail_stub},
    {"__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake},
    {"stat", (uintptr_t)&stat},
    {"stat64", (uintptr_t)&stat},
    {"strcasecmp", (uintptr_t)&strcasecmp},
    {"strcat", (uintptr_t)&strcat},
    {"__strcat_chk", (uintptr_t)&__strcat_chk},
    {"strchr", (uintptr_t)&strchr},
    {"__strchr_chk", (uintptr_t)&__strchr_chk},
    {"strcmp", (uintptr_t)&strcmp},
    {"strcoll", (uintptr_t)&strcoll},
    {"strcpy", (uintptr_t)&strcpy},
    {"__strcpy_chk", (uintptr_t)&__strcpy_chk},
    {"strcspn", (uintptr_t)&strcspn},
    {"strdup", (uintptr_t)&strdup},
    {"strerror", (uintptr_t)&strerror},
    {"strerror_r", (uintptr_t)&strerror_r},
    {"strftime", (uintptr_t)&strftime},
    {"strlen", (uintptr_t)&strlen},
    {"__strlen_chk", (uintptr_t)&__strlen_chk},
    {"strncasecmp", (uintptr_t)&strncasecmp},
    {"strncmp", (uintptr_t)&strncmp},
    {"strncpy", (uintptr_t)&strncpy},
    {"__strncpy_chk2", (uintptr_t)&__strncpy_chk2},
    {"strrchr", (uintptr_t)&strrchr},
    {"__strrchr_chk", (uintptr_t)&__strrchr_chk},
    {"strspn", (uintptr_t)&strspn},
    {"strstr", (uintptr_t)&strstr},
    {"strtod", (uintptr_t)&strtod},
    {"strtof", (uintptr_t)&strtof},
    {"strtol", (uintptr_t)&strtol},
    {"strtold", (uintptr_t)&strtold},
    {"strtold_l", (uintptr_t)&strtold_l_stub},
    {"strtoll", (uintptr_t)&strtoll},
    {"strtoll_l", (uintptr_t)&strtoll_l_stub},
    {"strtoul", (uintptr_t)&strtoul},
    {"strtoull", (uintptr_t)&strtoull},
    {"strtoull_l", (uintptr_t)&strtoull_l_stub},
    {"strxfrm", (uintptr_t)&strxfrm},
    {"swprintf", (uintptr_t)&swprintf},
    {"syscall", (uintptr_t)&syscall_fake},
    {"sysconf", (uintptr_t)&sysconf},
    {"sysinfo", (uintptr_t)&sysinfo_stub},
    {"syslog", (uintptr_t)&syslog_stub},
    {"system", (uintptr_t)&system},
    {"tan", (uintptr_t)&tan},
    {"tanf", (uintptr_t)&tanf},
    {"tcgetattr", (uintptr_t)&tcgetattr_stub},
    {"tcsetattr", (uintptr_t)&tcsetattr_stub},
    {"time", (uintptr_t)&time},
    {"tolower", (uintptr_t)&tolower},
    {"toupper", (uintptr_t)&toupper},
    {"towlower", (uintptr_t)&towlower},
    {"towupper", (uintptr_t)&towupper},
    {"uname", (uintptr_t)&uname},
    {"unlink", (uintptr_t)&unlink},
    {"uselocale", (uintptr_t)&uselocale_stub},
    {"usleep", (uintptr_t)&usleep},
    {"vasprintf", (uintptr_t)&vasprintf},
    {"vfprintf", (uintptr_t)&sf_vfprintf},
    {"vsnprintf", (uintptr_t)&vsnprintf},
    {"__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk},
    {"vsprintf", (uintptr_t)&vsprintf},
    {"__vsprintf_chk", (uintptr_t)&__vsprintf_chk},
    {"vsscanf", (uintptr_t)&vsscanf},
    {"vswprintf", (uintptr_t)&vswprintf},
    {"wcrtomb", (uintptr_t)&wcrtomb},
    {"wcscat", (uintptr_t)&wcscat},
    {"wcscoll", (uintptr_t)&wcscoll},
    {"wcscpy", (uintptr_t)&wcscpy},
    {"wcslen", (uintptr_t)&wcslen},
    {"wcsnrtombs", (uintptr_t)&wcsnrtombs},
    {"wcstod", (uintptr_t)&wcstod},
    {"wcstof", (uintptr_t)&wcstof},
    {"wcstol", (uintptr_t)&wcstol},
    {"wcstold", (uintptr_t)&wcstold},
    {"wcstoll", (uintptr_t)&wcstoll},
    {"wcstombs", (uintptr_t)&wcstombs},
    {"wcstoul", (uintptr_t)&wcstoul},
    {"wcstoull", (uintptr_t)&wcstoull},
    {"wcsxfrm", (uintptr_t)&wcsxfrm},
    {"wctob", (uintptr_t)&wctob},
    {"wmemchr", (uintptr_t)&wmemchr},
    {"wmemcmp", (uintptr_t)&wmemcmp},
    {"wmemcpy", (uintptr_t)&wmemcpy},
    {"wmemmove", (uintptr_t)&wmemmove},
    {"wmemset", (uintptr_t)&wmemset},
    {"write", (uintptr_t)&write},

    {"AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake},
    {"AAssetManager_open", (uintptr_t)&AAssetManager_open_fake},
    {"AAsset_close", (uintptr_t)&AAsset_close_fake},
    {"AAsset_read", (uintptr_t)&AAsset_read_fake},
    {"AAsset_getLength", (uintptr_t)&AAsset_getLength_fake},
    {"AAsset_getRemainingLength", (uintptr_t)&AAsset_getRemainingLength_fake},
    {"AAsset_seek", (uintptr_t)&AAsset_seek_fake},
    {"AAssetManager_openDir", (uintptr_t)&AAssetManager_openDir_fake},
    {"AAssetDir_getNextFileName", (uintptr_t)&AAssetDir_getNextFileName_fake},
    {"AAssetDir_close", (uintptr_t)&AAssetDir_close_fake},

    {"eglGetDisplay", (uintptr_t)egl_shim_GetDisplay},
    {"eglInitialize", (uintptr_t)egl_shim_Initialize},
    {"eglTerminate", (uintptr_t)egl_shim_Terminate},
    {"eglChooseConfig", (uintptr_t)egl_shim_ChooseConfig},
    {"eglCreateWindowSurface", (uintptr_t)egl_shim_CreateWindowSurface},
    {"eglCreatePbufferSurface", (uintptr_t)egl_shim_CreatePbufferSurface},
    {"eglCreateContext", (uintptr_t)egl_shim_CreateContext},
    {"eglDestroyContext", (uintptr_t)egl_shim_DestroyContext},
    {"eglDestroySurface", (uintptr_t)egl_shim_DestroySurface},
    {"eglGetConfigAttrib", (uintptr_t)egl_shim_GetConfigAttrib},
    {"eglGetError", (uintptr_t)egl_shim_GetError},
    {"eglGetProcAddress", (uintptr_t)egl_shim_GetProcAddress},
    {"eglMakeCurrent", (uintptr_t)egl_shim_MakeCurrent},
    {"eglSwapBuffers", (uintptr_t)egl_shim_SwapBuffers},
    {"eglSwapInterval", (uintptr_t)egl_shim_SwapInterval},
    {"eglBindAPI", (uintptr_t)egl_shim_BindAPI},
    {"eglQuerySurface", (uintptr_t)egl_shim_QuerySurface},
    {"eglQueryString", (uintptr_t)egl_shim_QueryString},
    {"eglGetCurrentContext", (uintptr_t)egl_shim_GetCurrentContext},
    {"eglGetCurrentSurface", (uintptr_t)egl_shim_GetCurrentSurface},
    {"eglSurfaceAttrib", (uintptr_t)egl_shim_SurfaceAttrib},

    {"ANativeWindow_fromSurface", (uintptr_t)&ret1},
    {"ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth},
    {"ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight},
    {"ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry},
    {"ALooper_removeFd", (uintptr_t)&ALooper_removeFd},
};

const int dynlib_functions_count =
    sizeof(dynlib_functions) / sizeof(dynlib_functions[0]);
