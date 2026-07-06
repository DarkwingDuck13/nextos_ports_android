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
#include <semaphore.h>
#include <sched.h>
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
#include <sys/syscall.h>
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
static char g_gl_last_asset_pvr[256];
static char g_dl_opensl;

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
  debugPrintf("__stack_chk_fail! caller=%p (libbadland+0x%lx) tls+0x28=0x%lx\n",
              ra, (unsigned long)((uintptr_t)ra - (uintptr_t)text_base),
              (unsigned long)g);
}

static int *__errno_fake(void) { return &errno; }

static int path_log_wanted(const char *path) {
  return path && (strstr(path, ".pvr") || strstr(path, "splash"));
}

static void log_path_result(const char *api, const char *path,
                            const char *resolved, long ret) {
  if (!path_log_wanted(path) && !path_log_wanted(resolved))
    return;
  if (ret < 0) {
    debugPrintf("[path] %s(\"%s\" -> \"%s\") = %ld errno=%d (%s)\n", api,
                path ? path : "(null)", resolved ? resolved : "(null)", ret,
                errno, strerror(errno));
  } else {
    debugPrintf("[path] %s(\"%s\" -> \"%s\") = %ld\n", api,
                path ? path : "(null)", resolved ? resolved : "(null)", ret);
  }
}

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
  debugPrintf("LOG [%s]: %s\n", tag, text);
  return 0;
}

void android_set_abort_message_fake(const char *msg) {
  debugPrintf("android_set_abort_message: %s\n", msg ? msg : "(null)");
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

void *__memchr_chk(const void *s, int c, size_t n, size_t s_len) {
  (void)s_len;
  return memchr(s, c, n);
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
  const char *resolved = resolve_android_path(pathname);
  FILE *f = fopen(resolved, mode);
  log_path_result("fopen", pathname, resolved, f ? 0 : -1);
  return f;
}

static int my_open(const char *pathname, int flags, ...) {
  mode_t mode = 0;
  const char *resolved = resolve_android_path(pathname);
  int fd;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
    fd = open(resolved, flags, mode);
  } else {
    fd = open(resolved, flags);
  }
  log_path_result("open", pathname, resolved, fd);
  return fd;
}

static int my_open_2(const char *pathname, int flags) {
  const char *resolved = resolve_android_path(pathname);
  int fd = open(resolved, flags);
  log_path_result("__open_2", pathname, resolved, fd);
  return fd;
}

static int access_fake(const char *pathname, int mode) {
  const char *resolved = resolve_android_path(pathname);
  int ret = access(resolved, mode);
  log_path_result("access", pathname, resolved, ret);
  return ret;
}

static int stat_fake(const char *pathname, struct stat *st) {
  const char *resolved = resolve_android_path(pathname);
  int ret = stat(resolved, st);
  log_path_result("stat", pathname, resolved, ret);
  return ret;
}

static int stat64_fake(const char *pathname, struct stat *st) {
  const char *resolved = resolve_android_path(pathname);
  int ret = stat(resolved, st);
  log_path_result("stat64", pathname, resolved, ret);
  return ret;
}

static int fstat64_fake(int fd, struct stat *st) {
  return fstat(fd, st);
}

static off_t lseek64_fake(int fd, off_t offset, int whence) {
  return lseek(fd, offset, whence);
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

static HostMutexEntry *g_mutex_entries = NULL;
static HostCondEntry *g_cond_entries = NULL;
static pthread_mutex_t g_mutex_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_cond_registry_lock = PTHREAD_MUTEX_INITIALIZER;

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

typedef struct {
  void *(*entry)(void *);
  void *arg;
} ThreadWrapper;
static void *thread_wrapper_func(void *data) {
  ThreadWrapper *w = (ThreadWrapper *)data;
  void *(*entry)(void *) = w->entry;
  void *arg = w->arg;
  free(w);
  return entry(arg);
}
int pthread_create_fake(pthread_t *thread, const void *attr, void *entry,
                        void *arg) {
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

static void strip_hd_suffix(char *path) {
  char *dot = strrchr(path, '.');
  if (dot && dot - path >= 3 && memcmp(dot - 3, "-hd", 3) == 0)
    memmove(dot - 3, dot, strlen(dot) + 1);
}

static FILE *try_asset_path(char *out, size_t out_size, const char *relative) {
  if (snprintf(out, out_size, "./assets/%s", relative) >= (int)out_size)
    return NULL;
  FILE *f = fopen(out, "rb");
  if (f)
    return f;

  char no_hd[1024];
  snprintf(no_hd, sizeof(no_hd), "%s", out);
  strip_hd_suffix(no_hd);
  if (strcmp(no_hd, out) != 0) {
    f = fopen(no_hd, "rb");
    if (f) {
      snprintf(out, out_size, "%s", no_hd);
      return f;
    }
  }
  return NULL;
}

static FILE *open_graphics_alias(char *out, size_t out_size,
                                 const char *relative) {
  const char *suffix = NULL;
  const char *bases[2] = {NULL, NULL};

  if (strncmp(relative, "graphics/sd_etc2/", 17) == 0) {
    suffix = relative + 17;
    bases[0] = "graphics/sd_common";
    bases[1] = "graphics/sd";
  } else if (strncmp(relative, "graphics/hd_etc2/", 17) == 0) {
    suffix = relative + 17;
    bases[0] = "graphics/sd_common";
    bases[1] = "graphics/sd";
  } else if (strncmp(relative, "graphics/hd_common/", 19) == 0) {
    suffix = relative + 19;
    bases[0] = "graphics/sd_common";
  } else if (strncmp(relative, "graphics/hd/", 12) == 0) {
    suffix = relative + 12;
    bases[0] = "graphics/sd";
  }

  if (!suffix)
    return NULL;

  for (int i = 0; i < 2 && bases[i]; i++) {
    char candidate[1024];
    snprintf(candidate, sizeof(candidate), "%s/%s", bases[i], suffix);
    FILE *f = try_asset_path(out, out_size, candidate);
    if (f) {
      if (getenv("BADLAND_ASSETLOG"))
        debugPrintf("AAsset alias: %s -> %s\n", relative, out);
      return f;
    }
  }
  return NULL;
}

void *AAssetManager_open_fake(void *mgr, const char *filename, int mode) {
  (void)mgr;
  (void)mode;
  char path[1024];
  path[0] = '\0';

  const char *relative_path = filename;
  if (strncmp(filename, "assets/", 7) == 0) {
    relative_path = filename + 7;
  } else if (strncmp(filename, "./assets/", 9) == 0) {
    relative_path = filename + 9;
  }

  FILE *f = try_asset_path(path, sizeof(path), relative_path);
  if (!f)
    f = open_graphics_alias(path, sizeof(path), relative_path);
  if (!f) {
    const char *resolved = resolve_android_path(filename);
    snprintf(path, sizeof(path), "%s", resolved ? resolved : "");
    f = fopen(resolved, "rb");
  }
  if (filename && strstr(filename, ".pvr"))
    snprintf(g_gl_last_asset_pvr, sizeof(g_gl_last_asset_pvr), "%s", filename);

  /* NAO logar por padrao: o jogo abre resources.bin centenas de vezes/frame e
     debugPrintf faz fopen+fclose de debug.log a cada chamada -> stall brutal de
     I/O no main loop -> decode de audio morre de fome (gagueira). BADLAND_ASSETLOG=1
     reativa p/ debug. */
  if (getenv("BADLAND_ASSETLOG") || path_log_wanted(filename) ||
      path_log_wanted(path))
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
static void __assert2_stub(const char *file, int line, const char *func,
                           const char *expr) {
  debugPrintf("__assert2: %s:%d %s: %s\n", file ? file : "?", line,
              func ? func : "?", expr ? expr : "?");
}
static pid_t gettid_stub(void) { return (pid_t)syscall(SYS_gettid); }
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
static int tolower_l_stub(int c, void *loc) {
  (void)loc;
  return tolower(c);
}
static int toupper_l_stub(int c, void *loc) {
  (void)loc;
  return toupper(c);
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
static int tcgetattr_stub(int fd, void *termios_p) { return 0; }
static int tcsetattr_stub(int fd, int optional_actions, const void *termios_p) {
  return 0;
}
static int mlock_stub(const void *addr, size_t len) { return 0; }
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

static int pthread_kill_fake(pthread_t thread, int sig) {
  static int logged = 0;
  void *ra = __builtin_return_address(0);
  if (logged < 32) {
    debugPrintf("pthread_kill_fake(thread=%p sig=%d caller=%p libbadland+0x%lx)\n",
                (void *)thread, sig, ra,
                (unsigned long)((uintptr_t)ra - (uintptr_t)text_base));
    logged++;
  }
  return 0;
}

static void *signal_fake(int signum, void *handler) {
  static int logged = 0;
  void *ra = __builtin_return_address(0);
  if (logged < 32) {
    debugPrintf("signal_fake(signum=%d handler=%p caller=%p libbadland+0x%lx)\n",
                signum, handler, ra,
                (unsigned long)((uintptr_t)ra - (uintptr_t)text_base));
    logged++;
  }
  return NULL;
}

static void abort_fake(void) {
  void *ra = __builtin_return_address(0);
  debugPrintf("abort_fake caller=%p libbadland+0x%lx\n", ra,
              (unsigned long)((uintptr_t)ra - (uintptr_t)text_base));
}

extern unsigned char *etc2_decode_rgba(unsigned fmt, int w, int h,
                                       const void *data, int size);
static unsigned long g_gl_draw_arrays_total;
static unsigned long g_gl_draw_elements_total;
static unsigned long g_gl_clear_total;
static unsigned long g_gl_bind_texture_total;
static GLuint g_gl_current_texture;
static GLuint g_gl_current_program;
static GLenum g_gl_current_blend_src;
static GLenum g_gl_current_blend_dst;
static char g_gl_upload_label[256];

typedef struct TextureLabel {
  GLuint id;
  int w;
  int h;
  char label[192];
} TextureLabel;

static TextureLabel g_texture_labels[512];

void badland_gl_set_upload_label(const char *label) {
  if (!label)
    label = "";
  snprintf(g_gl_upload_label, sizeof(g_gl_upload_label), "%s", label);
}

static TextureLabel *find_texture_label(GLuint id, int create) {
  if (!id)
    return NULL;
  TextureLabel *empty = NULL;
  for (size_t i = 0; i < sizeof(g_texture_labels) / sizeof(g_texture_labels[0]);
       i++) {
    if (g_texture_labels[i].id == id)
      return &g_texture_labels[i];
    if (!g_texture_labels[i].id && !empty)
      empty = &g_texture_labels[i];
  }
  if (create && empty) {
    empty->id = id;
    return empty;
  }
  return NULL;
}

static void note_bound_texture_upload(int w, int h) {
  GLint id = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
  if (id <= 0)
    return;
  TextureLabel *entry = find_texture_label((GLuint)id, 1);
  if (!entry)
    return;
  entry->w = w;
  entry->h = h;
  const char *label = g_gl_upload_label[0] ? g_gl_upload_label
                                           : g_gl_last_asset_pvr;
  if (label && label[0])
    snprintf(entry->label, sizeof(entry->label), "%s", label);
}

const char *badland_gl_current_texture_label(void) {
  TextureLabel *entry = find_texture_label(g_gl_current_texture, 0);
  return (entry && entry->label[0]) ? entry->label : "";
}

unsigned badland_gl_current_texture_id(void) { return g_gl_current_texture; }

unsigned badland_gl_current_program_id(void) { return g_gl_current_program; }

unsigned badland_gl_current_blend_src(void) { return g_gl_current_blend_src; }

unsigned badland_gl_current_blend_dst(void) { return g_gl_current_blend_dst; }

unsigned long badland_gl_draw_calls_total(void) {
  return g_gl_draw_arrays_total + g_gl_draw_elements_total;
}

static void glClearColor_badland(GLfloat red, GLfloat green, GLfloat blue,
                                 GLfloat alpha) {
  static int n = 0;
  if (n < 24) {
    debugPrintf("[GLSTATE] glClearColor %.3f %.3f %.3f %.3f\n", red, green,
                blue, alpha);
    n++;
  }
  glClearColor(red, green, blue, alpha);
}

static void glClear_badland(GLbitfield mask) {
  g_gl_clear_total++;
  if (g_gl_clear_total <= 24 || (g_gl_clear_total % 600) == 0)
    debugPrintf("[GLSTATE] glClear #%lu mask=0x%x\n", g_gl_clear_total,
                (unsigned)mask);
  glClear(mask);
}

static void glBlendFunc_badland(GLenum sfactor, GLenum dfactor) {
  g_gl_current_blend_src = sfactor;
  g_gl_current_blend_dst = dfactor;
  static GLenum last_s = 0, last_d = 0;
  static int n = 0;
  if (n < 64 && (sfactor != last_s || dfactor != last_d)) {
    debugPrintf("[GLSTATE] glBlendFunc 0x%x 0x%x\n", (unsigned)sfactor,
                (unsigned)dfactor);
    last_s = sfactor;
    last_d = dfactor;
    n++;
  }
  glBlendFunc(sfactor, dfactor);
}

static void glViewport_badland(GLint x, GLint y, GLsizei width,
                               GLsizei height) {
  if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
    SDL_Window *win = SDL_GL_GetCurrentWindow();
    int dw = 0, dh = 0;
    if (win)
      SDL_GL_GetDrawableSize(win, &dw, &dh);
    if (dw <= 0)
      dw = 1280;
    if (dh <= 0)
      dh = 720;
    debugPrintf("[GLFIX] glViewport invalid %d,%d %dx%d -> 0,0 %dx%d\n", x,
                y, width, height, dw, dh);
    x = 0;
    y = 0;
    width = dw;
    height = dh;
  }
  static GLint last_x = -99999, last_y = -99999;
  static GLsizei last_w = -1, last_h = -1;
  static int n = 0;
  if (n < 32 &&
      (x != last_x || y != last_y || width != last_w || height != last_h)) {
    debugPrintf("[GLSTATE] glViewport %d,%d %dx%d\n", x, y, width, height);
    last_x = x;
    last_y = y;
    last_w = width;
    last_h = height;
    n++;
  }
  glViewport(x, y, width, height);
}

static void glScissor_badland(GLint x, GLint y, GLsizei width,
                              GLsizei height) {
  if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
    SDL_Window *win = SDL_GL_GetCurrentWindow();
    int dw = 0, dh = 0;
    if (win)
      SDL_GL_GetDrawableSize(win, &dw, &dh);
    if (dw <= 0)
      dw = 1280;
    if (dh <= 0)
      dh = 720;
    debugPrintf("[GLFIX] glScissor invalid %d,%d %dx%d -> 0,0 %dx%d\n", x, y,
                width, height, dw, dh);
    x = 0;
    y = 0;
    width = dw;
    height = dh;
  }
  static int n = 0;
  if (n < 32) {
    debugPrintf("[GLSTATE] glScissor %d,%d %dx%d\n", x, y, width, height);
    n++;
  }
  glScissor(x, y, width, height);
}

static void glBindTexture_badland(GLenum target, GLuint texture) {
  if (target == GL_TEXTURE_2D)
    g_gl_current_texture = texture;
  g_gl_bind_texture_total++;
  if (g_gl_bind_texture_total <= 64 ||
      (g_gl_bind_texture_total % 2000) == 0)
    debugPrintf("[GLSTATE] glBindTexture #%lu tgt=0x%x tex=%u\n",
                g_gl_bind_texture_total, (unsigned)target, texture);
  glBindTexture(target, texture);
}

static void glUseProgram_badland(GLuint program) {
  g_gl_current_program = program;
  static GLuint last_program = 0xffffffffu;
  static int n = 0;
  if (n < 64 && program != last_program) {
    debugPrintf("[GLSTATE] glUseProgram %u\n", program);
    last_program = program;
    n++;
  }
  glUseProgram(program);
}

static void glDrawArrays_badland(GLenum mode, GLint first, GLsizei count) {
  g_gl_draw_arrays_total++;
  unsigned long total = badland_gl_draw_calls_total();
  if (total <= 80 || (total % 2000) == 0)
    debugPrintf("[GLDRAW] #%lu glDrawArrays mode=0x%x first=%d count=%d\n",
                total, (unsigned)mode, first, count);
  glDrawArrays(mode, first, count);
}

static void glDrawElements_badland(GLenum mode, GLsizei count, GLenum type,
                                   const GLvoid *indices) {
  g_gl_draw_elements_total++;
  unsigned long total = badland_gl_draw_calls_total();
  if (total <= 80 || (total % 2000) == 0)
    debugPrintf("[GLDRAW] #%lu glDrawElements mode=0x%x count=%d type=0x%x "
                "idx=%p\n",
                total, (unsigned)mode, count, (unsigned)type, indices);
  glDrawElements(mode, count, type, indices);
}

static void glTexImage2D_badland(GLenum target, GLint level,
                                 GLint internalformat, GLsizei width,
                                 GLsizei height, GLint border, GLenum format,
                                 GLenum type, const GLvoid *pixels);

static void glCompressedTexImage2D_badland(GLenum target, GLint level,
                                           GLenum internalformat, GLsizei width,
                                           GLsizei height, GLint border,
                                           GLsizei imageSize,
                                           const GLvoid *data) {
  if (data && internalformat >= 0x9274 && internalformat <= 0x9279) {
    unsigned char *rgba = etc2_decode_rgba((unsigned)internalformat, width,
                                           height, data, imageSize);
    if (rgba) {
      glTexImage2D_badland(target, level, GL_RGBA, width, height, border,
                           GL_RGBA, GL_UNSIGNED_BYTE, rgba);
      free(rgba);
      static int n = 0;
      if (n < 16) {
        debugPrintf("[ETC2] 0x%x %dx%d lvl=%d -> RGBA8888\n",
                    (unsigned)internalformat, width, height, level);
        n++;
      }
      return;
    }
    debugPrintf("[ETC2] decode falhou fmt=0x%x %dx%d size=%d\n",
                (unsigned)internalformat, width, height, imageSize);
  }
  glCompressedTexImage2D(target, level, internalformat, width, height, border,
                         imageSize, data);
}

static unsigned char *bgra_to_rgba(const unsigned char *src, int w, int h) {
  if (!src || w <= 0 || h <= 0)
    return NULL;
  size_t pixels = (size_t)w * (size_t)h;
  if (pixels > (SIZE_MAX / 4))
    return NULL;
  unsigned char *dst = malloc(pixels * 4);
  if (!dst)
    return NULL;
  for (size_t i = 0; i < pixels; i++) {
    dst[i * 4 + 0] = src[i * 4 + 2];
    dst[i * 4 + 1] = src[i * 4 + 1];
    dst[i * 4 + 2] = src[i * 4 + 0];
    dst[i * 4 + 3] = src[i * 4 + 3];
  }
  return dst;
}

static void glTexImage2D_badland(GLenum target, GLint level,
                                 GLint internalformat, GLsizei width,
                                 GLsizei height, GLint border, GLenum format,
                                 GLenum type, const GLvoid *pixels) {
  GLint orig_internal = internalformat;
  GLenum orig_format = format;
  const GLvoid *upload_pixels = pixels;
  unsigned char *converted = NULL;

  switch ((GLenum)internalformat) {
  case 0x8058: /* GL_RGBA8 */
  case 0x8C43: /* GL_SRGB8_ALPHA8 */
  case 0x881A: /* GL_RGBA16F */
  case 0x8814: /* GL_RGBA32F */
    internalformat = GL_RGBA;
    break;
  case 0x8051: /* GL_RGB8 */
  case 0x8C41: /* GL_SRGB8 */
  case 0x881B: /* GL_RGB16F */
  case 0x8815: /* GL_RGB32F */
    internalformat = GL_RGB;
    break;
  case 0x8229: /* GL_R8 */
  case 0x822E: /* GL_R32F */
    internalformat = GL_LUMINANCE;
    break;
  case 0x822B: /* GL_RG8 */
    internalformat = GL_LUMINANCE_ALPHA;
    break;
  default:
    break;
  }

  if (format == 0x80E1 && type == GL_UNSIGNED_BYTE && pixels) { /* GL_BGRA_EXT */
    converted = bgra_to_rgba((const unsigned char *)pixels, width, height);
    if (converted) {
      upload_pixels = converted;
      format = GL_RGBA;
      if ((GLenum)internalformat == 0x80E1)
        internalformat = GL_RGBA;
    }
  }

  static int n = 0;
  if (n < 32) {
    debugPrintf("[GL] glTexImage2D tgt=0x%x lvl=%d ifmt=0x%x->0x%x %dx%d "
                "fmt=0x%x->0x%x type=0x%x pixels=%p%s\n",
                (unsigned)target, level, (unsigned)orig_internal,
                (unsigned)internalformat, width, height,
                (unsigned)orig_format, (unsigned)format, (unsigned)type,
                pixels, converted ? " bgra->rgba" : "");
    n++;
  }

  note_bound_texture_upload(width, height);
  glTexImage2D(target, level, internalformat, width, height, border, format,
               type, upload_pixels);
  free(converted);
}

static GLint sanitize_tex_param(GLenum target, GLenum pname, GLint param,
                                const char *api) {
  if (target != GL_TEXTURE_2D)
    return param;
  GLint orig = param;
  if (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T) {
    param = GL_CLAMP_TO_EDGE;
  } else if (pname == GL_TEXTURE_MIN_FILTER) {
    if (param == GL_NEAREST_MIPMAP_NEAREST || param == GL_LINEAR_MIPMAP_NEAREST ||
        param == GL_NEAREST_MIPMAP_LINEAR || param == GL_LINEAR_MIPMAP_LINEAR)
      param = GL_LINEAR;
  }
  static int n = 0;
  if (param != orig && n < 48) {
    debugPrintf("[GLFIX] %s tex=%u pname=0x%x 0x%x -> 0x%x\n", api,
                g_gl_current_texture, (unsigned)pname, (unsigned)orig,
                (unsigned)param);
    n++;
  }
  return param;
}

static void glTexParameteri_badland(GLenum target, GLenum pname, GLint param) {
  if (pname == 0x813D || pname == 0x84FE)
    return; /* GL_TEXTURE_MAX_LEVEL / GL_TEXTURE_MAX_ANISOTROPY_EXT */
  param = sanitize_tex_param(target, pname, param, "glTexParameteri");
  glTexParameteri(target, pname, param);
}

static void glTexParameterf_badland(GLenum target, GLenum pname,
                                    GLfloat param) {
  if (pname == 0x813D || pname == 0x84FE)
    return;
  GLint iparam = (GLint)param;
  GLint fixed = sanitize_tex_param(target, pname, iparam, "glTexParameterf");
  glTexParameterf(target, pname, (GLfloat)fixed);
}

static void *my_dlopen(const char *filename, int flags) {
  if (filename && strstr(filename, "OpenSLES")) {
    debugPrintf("[dlopen] %s -> opensles_shim\n", filename);
    return &g_dl_opensl;
  }
  return dlopen(filename, flags);
}

static void *generic_sl_iid(const char *name) {
  static struct {
    char name[48];
    void *id;
  } ids[24];
  static char slots[24];

  for (int i = 0; i < 24; i++) {
    if (ids[i].name[0] && strcmp(ids[i].name, name) == 0)
      return &ids[i].id;
    if (!ids[i].name[0]) {
      snprintf(ids[i].name, sizeof(ids[i].name), "%s", name);
      ids[i].id = &slots[i];
      debugPrintf("[dlsym:SL] %s -> generic iid\n", name);
      return &ids[i].id;
    }
  }
  return NULL;
}

static void *my_dlsym(void *handle, const char *symbol) {
  if (handle == &g_dl_opensl) {
    debugPrintf("[dlsym:SL] %s\n", symbol ? symbol : "(null)");
    if (!symbol)
      return NULL;
    if (strcmp(symbol, "slCreateEngine") == 0)
      return (void *)slCreateEngine_shim;
    if (strcmp(symbol, "SL_IID_ENGINE") == 0)
      return (void *)&sl_IID_ENGINE;
    if (strcmp(symbol, "SL_IID_PLAY") == 0)
      return (void *)&sl_IID_PLAY;
    if (strcmp(symbol, "SL_IID_VOLUME") == 0)
      return (void *)&sl_IID_VOLUME;
    if (strcmp(symbol, "SL_IID_BUFFERQUEUE") == 0 ||
        strcmp(symbol, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE") == 0)
      return (void *)&sl_IID_BUFFERQUEUE;
    if (strcmp(symbol, "SL_IID_EFFECTSEND") == 0)
      return (void *)&sl_IID_EFFECTSEND;
    if (strcmp(symbol, "SL_IID_ENGINECAPABILITIES") == 0)
      return (void *)&sl_IID_ENGINECAPABILITIES;
    if (strcmp(symbol, "SL_IID_ENVIRONMENTALREVERB") == 0)
      return (void *)&sl_IID_ENVIRONMENTALREVERB;
    if (strncmp(symbol, "SL_IID_", 7) == 0)
      return generic_sl_iid(symbol);
    return NULL;
  }
  return dlsym(handle, symbol);
}

/* ---- stubs bionic-only (glibc nao tem; senao slot vira PLT0 -> crash) ---- */
static int my___system_property_get(const char *name, char *value) {
  const char *ret = "";
  if (name && strcmp(name, "ro.build.version.sdk") == 0)
    ret = "25";
  else if (name && strcmp(name, "ro.product.manufacturer") == 0)
    ret = "NextOS";
  else if (name && strcmp(name, "ro.product.model") == 0)
    ret = "NextOS";
  if (value)
    strcpy(value, ret);
  return (int)strlen(ret);
}
static void my___android_log_assert(const char *cond, const char *tag,
                                    const char *fmt, ...) {
  (void)cond; (void)tag; (void)fmt;
  debugPrintf("[android_log_assert] cond=%s tag=%s\n", cond ? cond : "?",
              tag ? tag : "?");
  /* nao aborta: deixa o engine seguir (boot). */
}

static unsigned char bionic_ctype[1 + 256];
static unsigned char *bionic_ctype_ptr = bionic_ctype;
__attribute__((constructor)) static void init_bionic_ctype(void) {
  bionic_ctype[0] = 0;
  for (int c = 0; c < 256; c++) {
    unsigned char f = 0;
    if (isupper(c)) f |= 0x01;
    if (islower(c)) f |= 0x02;
    if (isdigit(c)) f |= 0x04;
    if (isspace(c)) f |= 0x08;
    if (ispunct(c)) f |= 0x10;
    if (iscntrl(c)) f |= 0x20;
    if (isxdigit(c)) f |= 0x40;
    if (c == ' ') f |= 0x80;
    bionic_ctype[1 + c] = f;
  }
}

DynLibFunction dynlib_functions[] = {
    {"__system_property_get", (uintptr_t)&my___system_property_get},
    {"__assert2", (uintptr_t)&__assert2_stub},
    {"__android_log_assert", (uintptr_t)&my___android_log_assert},
    {"_ctype_", (uintptr_t)&bionic_ctype_ptr},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&sl_IID_BUFFERQUEUE},
    {"abort", (uintptr_t)&abort_fake},
    {"accept", (uintptr_t)&accept},
    {"access", (uintptr_t)&access_fake},
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
    {"dlclose", (uintptr_t)&dlclose},
    {"dlerror", (uintptr_t)&dlerror},
    {"dl_iterate_phdr", (uintptr_t)&dl_iterate_phdr_fake},
    {"dlopen", (uintptr_t)&my_dlopen},
    {"dlsym", (uintptr_t)&my_dlsym},
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
    {"fstat64", (uintptr_t)&fstat64_fake},
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
    {"gettid", (uintptr_t)&gettid_stub},
    {"getuid", (uintptr_t)&getuid},

    {"glActiveTexture", (uintptr_t)&glActiveTexture},
    {"glAttachShader", (uintptr_t)&glAttachShader},
    {"glBindAttribLocation", (uintptr_t)&glBindAttribLocation},
    {"glBindBuffer", (uintptr_t)&glBindBuffer},
    {"glBindFramebuffer", (uintptr_t)&glBindFramebuffer},
    {"glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer},
    {"glBindTexture", (uintptr_t)&glBindTexture_badland},
    {"glBlendColor", (uintptr_t)&glBlendColor},
    {"glBlendEquation", (uintptr_t)&glBlendEquation},
    {"glBlendFunc", (uintptr_t)&glBlendFunc_badland},
    {"glBufferData", (uintptr_t)&glBufferData},
    {"glBufferSubData", (uintptr_t)&glBufferSubData},
    {"glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus},
    {"glClear", (uintptr_t)&glClear_badland},
    {"glClearColor", (uintptr_t)&glClearColor_badland},
    {"glClearDepthf", (uintptr_t)&glClearDepthf},
    {"glClearStencil", (uintptr_t)&glClearStencil},
    {"glColorMask", (uintptr_t)&glColorMask},
    {"glCompileShader", (uintptr_t)&glCompileShader},
    {"glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D_badland},
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
    {"glDrawArrays", (uintptr_t)&glDrawArrays_badland},
    {"glDrawElements", (uintptr_t)&glDrawElements_badland},
    {"glEnable", (uintptr_t)&glEnable},
    {"glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray},
    {"glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer},
    {"glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D},
    {"glGenBuffers", (uintptr_t)&glGenBuffers},
    {"glGenerateMipmap", (uintptr_t)&glGenerateMipmap},
    {"glGenFramebuffers", (uintptr_t)&glGenFramebuffers},
    {"glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers},
    {"glGenTextures", (uintptr_t)&glGenTextures},
    {"glGetFramebufferAttachmentParameteriv",
     (uintptr_t)&glGetFramebufferAttachmentParameteriv},
    {"glGetBooleanv", (uintptr_t)&glGetBooleanv},
    {"glGetError", (uintptr_t)&glGetError},
    {"glGetFloatv", (uintptr_t)&glGetFloatv},
    {"glGetIntegerv", (uintptr_t)&glGetIntegerv},
    {"glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog},
    {"glGetProgramiv", (uintptr_t)&glGetProgramiv},
    {"glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog},
    {"glGetShaderiv", (uintptr_t)&glGetShaderiv},
    {"glGetShaderSource", (uintptr_t)&glGetShaderSource},
    {"glGetString", (uintptr_t)&glGetString},
    {"glGetUniformLocation", (uintptr_t)&glGetUniformLocation},
    {"glLinkProgram", (uintptr_t)&glLinkProgram},
    {"glPixelStorei", (uintptr_t)&glPixelStorei},
    {"glReadPixels", (uintptr_t)&glReadPixels},
    {"glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage},
    {"glScissor", (uintptr_t)&glScissor_badland},
    {"glShaderSource", (uintptr_t)&glShaderSource},
    {"glStencilFunc", (uintptr_t)&glStencilFunc},
    {"glStencilMask", (uintptr_t)&glStencilMask},
    {"glStencilOp", (uintptr_t)&glStencilOp},
    {"glTexImage2D", (uintptr_t)&glTexImage2D_badland},
    {"glTexParameterf", (uintptr_t)&glTexParameterf_badland},
    {"glTexParameteri", (uintptr_t)&glTexParameteri_badland},
    {"glTexSubImage2D", (uintptr_t)&glTexSubImage2D},
    {"glUniform1f", (uintptr_t)&glUniform1f},
    {"glUniform1fv", (uintptr_t)&glUniform1fv},
    {"glUniform1i", (uintptr_t)&glUniform1i},
    {"glUniform2f", (uintptr_t)&glUniform2f},
    {"glUniform2fv", (uintptr_t)&glUniform2fv},
    {"glUniform2i", (uintptr_t)&glUniform2i},
    {"glUniform2iv", (uintptr_t)&glUniform2iv},
    {"glUniform3f", (uintptr_t)&glUniform3f},
    {"glUniform3fv", (uintptr_t)&glUniform3fv},
    {"glUniform3i", (uintptr_t)&glUniform3i},
    {"glUniform3iv", (uintptr_t)&glUniform3iv},
    {"glUniform4f", (uintptr_t)&glUniform4f},
    {"glUniform4fv", (uintptr_t)&glUniform4fv},
    {"glUniform4i", (uintptr_t)&glUniform4i},
    {"glUniform4iv", (uintptr_t)&glUniform4iv},
    {"glUniformMatrix2fv", (uintptr_t)&glUniformMatrix2fv},
    {"glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv},
    {"glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv},
    {"glUseProgram", (uintptr_t)&glUseProgram_badland},
    {"glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer},
    {"glViewport", (uintptr_t)&glViewport_badland},

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
    {"kill", (uintptr_t)&kill},
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
    {"lseek64", (uintptr_t)&lseek64_fake},
    {"madvise", (uintptr_t)&madvise},
    {"malloc", (uintptr_t)&malloc},
    {"mbrlen", (uintptr_t)&mbrlen},
    {"mbrtowc", (uintptr_t)&mbrtowc},
    {"mbsnrtowcs", (uintptr_t)&mbsnrtowcs},
    {"mbsrtowcs", (uintptr_t)&mbsrtowcs},
    {"mbstowcs", (uintptr_t)&mbstowcs},
    {"mbtowc", (uintptr_t)&mbtowc},
    {"memchr", (uintptr_t)&memchr},
    {"__memchr_chk", (uintptr_t)&__memchr_chk},
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
    {"pthread_attr_destroy", (uintptr_t)&ret0},
    {"pthread_attr_setdetachstate", (uintptr_t)&ret0},
    {"pthread_attr_setstacksize", (uintptr_t)&ret0},
    {"pthread_attr_setschedparam", (uintptr_t)&my_pthread_attr_setschedparam_noop},
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
    {"sem_destroy", (uintptr_t)&sem_destroy},
    {"sem_init", (uintptr_t)&sem_init},
    {"sem_post", (uintptr_t)&sem_post},
    {"sem_wait", (uintptr_t)&sem_wait},
    {"send", (uintptr_t)&send},
    {"setpriority", (uintptr_t)&setpriority},
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
    {"signal", (uintptr_t)&signal_fake},
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
    {"stat", (uintptr_t)&stat_fake},
    {"stat64", (uintptr_t)&stat64_fake},
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
    {"syscall", (uintptr_t)&syscall},
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
    {"tolower_l", (uintptr_t)&tolower_l_stub},
    {"toupper", (uintptr_t)&toupper},
    {"toupper_l", (uintptr_t)&toupper_l_stub},
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

    {"ANativeWindow_fromSurface", (uintptr_t)&ret1},
    {"ANativeWindow_getWidth", (uintptr_t)&ret1},
    {"ANativeWindow_getHeight", (uintptr_t)&ret1},
    {"ANativeWindow_setBuffersGeometry", (uintptr_t)&ret1},
};

const int dynlib_functions_count =
    sizeof(dynlib_functions) / sizeof(dynlib_functions[0]);
