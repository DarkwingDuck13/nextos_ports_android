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
  /* FMV: detecta o filme atual quando o engine abre um .webm -> carrega o .ivf
   * correspondente p/ nosso decode VP8 (driblando o samplerExternalOES). */
  if (pathname) {
    size_t n = strlen(pathname);
    if (n > 5 && strcasecmp(pathname + n - 5, ".webm") == 0) {
      extern void fmv_set_movie_from_webm(const char *);
      fmv_set_movie_from_webm(resolve_android_path(pathname));
    }
  }
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
/* idem destroy/setstacksize/setdetachstate: o glibc destroy faria free() de um
 * ponteiro lixo lido do attr bionic -> SIGSEGV. pthread_create_fake ignora o
 * attr, entao no-op e' seguro. */
static int my_pthread_attr_destroy_noop(void *attr) { (void)attr; return 0; }
static int my_pthread_attr_setstacksize_noop(void *attr, size_t s) { (void)attr; (void)s; return 0; }
static int my_pthread_attr_setdetachstate_noop(void *attr, int s) { (void)attr; (void)s; return 0; }

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
    /* FF7: dados do jogo (ff7_1.02/...) ficam em $FF7_DATA, nao em ./assets */
    const char *dp = getenv("FF7_DATA");
    if (dp) {
      snprintf(path, sizeof(path), "%s/%s", dp, relative_path);
      f = fopen(path, "rb");
    }
  }
  if (!f) {
    f = fopen(resolve_android_path(filename), "rb");
  }

  debugPrintf("AAssetManager_open(\"%s\" -> \"%s\") = %p\n", filename, path, f);

  /* FMV: filmes podem abrir via AssetManager -> detecta o .webm p/ nosso decode. */
  if (f && filename) {
    size_t fn = strlen(filename);
    if (fn > 5 && strcasecmp(filename + fn - 5, ".webm") == 0) {
      extern void fmv_set_movie_from_webm(const char *);
      fmv_set_movie_from_webm(path);
    }
  }

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

/* FF7 usa AAsset_openFileDescriptor64 p/ ler assets grandes via fd.
 * (AAsset*, off64_t* outStart, off64_t* outLen) -> fd (dup do FILE). */
int AAsset_openFileDescriptor64_fake(void *asset, long long *outStart, long long *outLen) {
  if (!asset) return -1;
  FakeAsset *a = (FakeAsset *)asset;
  int fd = fileno(a->f);
  if (fd < 0) return -1;
  int dupfd = dup(fd);
  if (outStart) *outStart = ftell(a->f);
  if (outLen) *outLen = a->size;
  debugPrintf("AAsset_openFileDescriptor64 -> fd=%d start=%lld len=%lld\n",
              dupfd, outStart ? *outStart : -1, outLen ? *outLen : -1);
  return dupfd;
}

/* OpenSLES interface IDs que o FF7 referencia mas o opensles_shim nao mapeia.
 * Sao tokens-de-identidade; o shim ignora os desconhecidos. Dummies nao-nulos
 * p/ nenhum SL_IID_* resolver a NULL (engine compara/passa adiante sem deref). */
static const int ff7_sl_iid_dummies[48];
#define SLD(i) ((uintptr_t)&ff7_sl_iid_dummies[i])

size_t __ctype_get_mb_cur_max_fake(void) { return 4; }

int dl_iterate_phdr_fake(void *callback, void *data) { return 0; }
/* PERF: o VIDEO_update solta/re-bind o EGL context a cada frame (handshake
 * threaded). Em single-thread isso e' so' overhead (4 eglMakeCurrent/frame no
 * Mali = lento, ~8fps). Override: ignora os RELEASE (ctx==NULL) depois do init
 * -> context fica sempre current -> swap nao precisa re-adquirir -> rapido.
 * FF7_RELEASECTX desliga (volta ao modo re-aquisicao). */
int g_ff7_keepctx = 0;  /* setado pela main apos init */
static unsigned (*real_eglMakeCurrent)(void *, void *, void *, void *);
unsigned ff7_eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx) {
  if (g_ff7_keepctx && ctx == NULL) return 1; /* EGL_TRUE: nao solta o context */
  if (!real_eglMakeCurrent)
    real_eglMakeCurrent = (void *)dlsym(RTLD_DEFAULT, "eglMakeCurrent");
  return real_eglMakeCurrent ? real_eglMakeCurrent(dpy, draw, read, ctx) : 1;
}

/* DIAG render-target: loga glBindFramebuffer/glViewport p/ ver onde o jogo
 * renderiza (FBO offscreen vs FB 0 da tela). FF7_GLDIAG=1 ativa. */
static void (*real_glBindFramebuffer)(unsigned, unsigned);
void ff7_glBindFramebuffer(unsigned target, unsigned fb) {
  if (!real_glBindFramebuffer) real_glBindFramebuffer = (void *)dlsym(RTLD_DEFAULT, "glBindFramebuffer");
  real_glBindFramebuffer(target, fb);
  /* FF7_GLDIAG: ao bindar FBO offscreen (fb!=0) checa se esta' COMPLETO. status
   * != 0x8CD5 (FRAMEBUFFER_COMPLETE) => render-target quebrado = bg preto. */
  if (getenv("FF7_GLDIAG") && fb != 0) {
    static unsigned (*chk)(unsigned) = 0;
    if (!chk) chk = (void *)dlsym(RTLD_DEFAULT, "glCheckFramebufferStatus");
    unsigned st = chk ? chk(target) : 0;
    static int n = 0;
    if (st != 0x8CD5u || n++ < 8)
      debugPrintf("FBO bind fb=%u status=0x%x %s\n", fb, st,
                  st == 0x8CD5u ? "COMPLETE" : "INCOMPLETE!");
  }
}

/* hooks de profiling do Google (Play/anr): no-op. GOT NULL -> chamada crasha. */
void __google_potentially_blocking_region_begin_stub(void) {}
void __google_potentially_blocking_region_end_stub(void) {}

/* ---- present single-thread (bypassa o renderer threaded do FF7) ----
 * VIDEO_update, no fim, faz: eglMakeCurrent(release) -> sem_post(semB) ->
 * sem_wait(semA) -> eglMakeCurrent(rebind). Na thread dona do present
 * (struct[+64]==pthread_self). Rodando o jogo numa thread so, ninguem do "outro
 * lado" posta/consome -> sem_wait(semA) travaria. Estes wrappers detectam os
 * dois sems do present pelo endereco: no sem_post(semB) fazemos o swap+input
 * (o context ja esta liberado, igual a UI thread fazia), e sem_wait(semA)
 * retorna na hora. Qualquer outro sem usa o glibc real. */
void *g_ff7_present_semA = NULL;  /* VIDEO_update sem_wait (base+0)  */
void *g_ff7_present_semB = NULL;  /* VIDEO_update sem_post (base+0x10) */
void (*g_ff7_present_cb)(void) = NULL;  /* swap janela + pump input (main.c) */
static int (*real_sem_wait)(void *) = NULL;
static int (*real_sem_post)(void *) = NULL;
int ff7_sem_wait(void *s) {
  if (s && s == g_ff7_present_semA) return 0;  /* nao bloqueia: outro lado e' nos */
  if (!real_sem_wait) real_sem_wait = (void *)dlsym(RTLD_DEFAULT, "sem_wait");
  return real_sem_wait ? real_sem_wait(s) : 0;
}
int ff7_sem_post(void *s) {
  if (s && s == g_ff7_present_semB) {           /* frame pronto -> apresenta */
    if (g_ff7_present_cb) g_ff7_present_cb();
    return 0;
  }
  if (!real_sem_post) real_sem_post = (void *)dlsym(RTLD_DEFAULT, "sem_post");
  return real_sem_post ? real_sem_post(s) : 0;
}
static unsigned long getauxval_stub(unsigned long type) { return 0; }
static void sincos_stub(double x, double *s, double *c) {
  *s = sin(x);
  *c = cos(x);
}
static void sincosf_stub(float x, float *s, float *c) {
  *s = sinf(x);
  *c = cosf(x);
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

/* DIAG fundo do campo: FF7_CLEARRED forca o clear-color p/ vermelho. Se o fundo
 * da cena virar VERMELHO = o engine nao desenha o background (vemos o clear). Se
 * continuar PRETO = ha um quad preto desenhado por cima do clear. */
extern void glClearColor(float r, float g, float b, float a);
extern void glClear(unsigned mask);
static int g_clearred = -1, g_drawlog = -1;
static long g_de_count = 0;
static void my_glClearColor(float r, float g, float b, float a) {
  static int once = 0; if (!once++) debugPrintf("HOOKCHK my_glClearColor CALLED\n");
  if (g_clearred < 0) g_clearred = getenv("FF7_CLEARRED") != NULL;
  if (g_clearred) glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
  else glClearColor(r, g, b, a);
}
extern void glDrawElements(unsigned mode, int count, unsigned type, const void *indices);
static void my_glDrawElements(unsigned mode, int count, unsigned type, const void *indices) {
  if (g_drawlog < 0) g_drawlog = getenv("FF7_DRAWLOG") != NULL;
  g_de_count++;
  glDrawElements(mode, count, type, indices);
}
static int g_texlog = -1;
static long g_tex_total = 0, g_tex_zero = 0, g_tex_null = 0;
static int data_nonzero(const void *p, int n) {
  if (!p) return -1;  /* ptr NULL */
  const unsigned char *b = p; int nz = 0;
  for (int i = 0; i < n; i++) if (b[i]) nz++;
  return nz;
}
/* conta TODO upload de textura (qualquer tamanho) + resumo periodico. nz<0=ptr NULL,
 * nz==0=dados zerados (preto). Distingue tile-com-dados vs zerado vs sem-textura. */
static void tex_account(const char *fn, int w, int h, const void *px) {
  if (g_texlog < 0) g_texlog = getenv("FF7_TEXLOG") != NULL;
  if (!g_texlog) return;
  int nz = data_nonzero(px, 64);
  g_tex_total++;
  if (nz < 0) g_tex_null++; else if (nz == 0) g_tex_zero++;
  if (g_tex_total <= 6 || g_tex_total % 256 == 0 || (w >= 200 || h >= 200))
    debugPrintf("TEXLOG[%ld] %s %dx%d nz/64=%d  (total=%ld zero=%ld null=%ld)\n",
                g_tex_total, fn, w, h, nz, g_tex_total, g_tex_zero, g_tex_null);
}
extern void glTexImage2D(unsigned t, int lvl, int ifmt, int w, int h, int b,
                         unsigned fmt, unsigned ty, const void *px);
static void my_glTexImage2D(unsigned t, int lvl, int ifmt, int w, int h, int b,
                            unsigned fmt, unsigned ty, const void *px) {
  /* captura o CALLER (func do FF7) dos uploads 256x256 ZERADOS = tiles do bg pretos */
  if (getenv("FF7_TEXLOG") && w == 256 && h == 256 && data_nonzero(px, 64) == 0) {
    static int n = 0;
    if (n++ < 6) {
      void *ra = __builtin_return_address(0);
      debugPrintf("TEXCALLER 256x256 ZERO ret=%p (tb-rel=0x%lx) ifmt=0x%x fmt=0x%x type=0x%x\n",
                  ra, text_base ? (unsigned long)((uintptr_t)ra - (uintptr_t)text_base) : 0,
                  ifmt, fmt, ty);
    }
  }
  tex_account("glTexImage2D", w, h, px);
  /* FF7_TILEFILL: troca tiles 256x256 ZERADOS (bg do campo) por AZUL solido p/
   * provar que esses tiles SAO o background visivel (e o pipeline downstream OK). */
  if (getenv("FF7_TILEFILL") && w == 256 && h == 256 && fmt == 0x1908 /*RGBA*/
      && ty == 0x1401 && data_nonzero(px, 64) == 0) {
    static unsigned char *fill = 0;
    if (!fill) { fill = malloc(256*256*4);
      for (int i = 0; i < 256*256; i++) { fill[i*4]=0; fill[i*4+1]=64; fill[i*4+2]=200; fill[i*4+3]=255; } }
    glTexImage2D(t, lvl, ifmt, w, h, b, fmt, ty, fill);
    return;
  }
  glTexImage2D(t, lvl, ifmt, w, h, b, fmt, ty, px);
}
extern void glCompressedTexImage2D(unsigned t, int lvl, unsigned ifmt, int w, int h,
                                   int b, int sz, const void *data);
static void my_glCompressedTexImage2D(unsigned t, int lvl, unsigned ifmt, int w, int h,
                                      int b, int sz, const void *data) {
  tex_account("glCompressedTexImage2D", w, h, data);
  glCompressedTexImage2D(t, lvl, ifmt, w, h, b, sz, data);
}
extern void glTexSubImage2D(unsigned t, int lvl, int xo, int yo, int w, int h,
                            unsigned fmt, unsigned ty, const void *px);
static void my_glTexSubImage2D(unsigned t, int lvl, int xo, int yo, int w, int h,
                               unsigned fmt, unsigned ty, const void *px) {
  tex_account("glTexSubImage2D", w, h, px);
  glTexSubImage2D(t, lvl, xo, yo, w, h, fmt, ty, px);
}
void ff7_glBindFramebuffer(unsigned target, unsigned fb);  /* def. mais abaixo */
/* hook glFramebufferTexture2D: loga o attachment do FBO (qual textura, level) +
 * checa status logo apos -> identifica o FBO incompleto e seu attachment. */
extern void glFramebufferTexture2D(unsigned target, unsigned att, unsigned textgt, unsigned tex, int lvl);
static void my_glFramebufferTexture2D(unsigned target, unsigned att, unsigned textgt, unsigned tex, int lvl) {
  glFramebufferTexture2D(target, att, textgt, tex, lvl);
  if (getenv("FF7_GLDIAG")) {
    static unsigned (*chk)(unsigned) = 0;
    if (!chk) chk = (void *)dlsym(RTLD_DEFAULT, "glCheckFramebufferStatus");
    unsigned st = chk ? chk(target) : 0;
    static int n = 0;
    if (st != 0x8CD5u || n++ < 10)
      debugPrintf("FBTEX att=0x%x textgt=0x%x tex=%u lvl=%d -> status=0x%x %s\n",
                  att, textgt, tex, lvl, st, st == 0x8CD5u ? "OK" : "INCOMPLETE!");
  }
}
/* eglGetProcAddress hook: o jogo resolve MUITAS funcs GL por aqui (FBO, texturas),
 * bypassando a tabela de imports (por isso glClearColor/import pegou mas TexImage nao).
 * Devolvemos NOSSOS wrappers p/ as funcs instrumentadas. */
static void *(*real_eglGetProcAddress)(const char *) = 0;
static void *my_eglGetProcAddress(const char *name) {
  { static int once = 0; if (!once++) debugPrintf("HOOKCHK my_eglGetProcAddress CALLED first=%s\n", name?name:"?"); }
  if (name) {
    if (!strcmp(name, "glClearColor"))           return (void *)&my_glClearColor;
    if (!strcmp(name, "glTexImage2D"))           return (void *)&my_glTexImage2D;
    if (!strcmp(name, "glTexSubImage2D"))        return (void *)&my_glTexSubImage2D;
    if (!strcmp(name, "glCompressedTexImage2D")) return (void *)&my_glCompressedTexImage2D;
    if (!strcmp(name, "glBindFramebuffer"))      return (void *)&ff7_glBindFramebuffer;
    if (!strcmp(name, "glFramebufferTexture2D")) return (void *)&my_glFramebufferTexture2D;
  }
  if (!real_eglGetProcAddress)
    real_eglGetProcAddress = (void *)dlsym(RTLD_DEFAULT, "eglGetProcAddress");
  void *r = real_eglGetProcAddress ? real_eglGetProcAddress(name) : 0;
  if (!r && name) r = dlsym(RTLD_DEFAULT, name);
  return r;
}

DynLibFunction dynlib_functions[] = {
    {"__system_property_get", (uintptr_t)&my___system_property_get},
    {"__android_log_assert", (uintptr_t)&my___android_log_assert},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&sl_IID_BUFFERQUEUE},
    {"abort", (uintptr_t)&abort},
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
    {"dlclose", (uintptr_t)&dlclose},
    {"dlerror", (uintptr_t)&dlerror},
    {"dl_iterate_phdr", (uintptr_t)&dl_iterate_phdr_fake},
    {"dlopen", (uintptr_t)&dlopen},
    {"dlsym", (uintptr_t)&dlsym},
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
    {"glBufferData", (uintptr_t)&glBufferData},
    {"glBufferSubData", (uintptr_t)&glBufferSubData},
    {"glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus},
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
    {"glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer},
    {"glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D},
    {"glGenBuffers", (uintptr_t)&glGenBuffers},
    {"glGenerateMipmap", (uintptr_t)&glGenerateMipmap},
    {"glGenFramebuffers", (uintptr_t)&glGenFramebuffers},
    {"glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers},
    {"glGenTextures", (uintptr_t)&glGenTextures},
    {"glGetFramebufferAttachmentParameteriv",
     (uintptr_t)&glGetFramebufferAttachmentParameteriv},
    {"glGetIntegerv", (uintptr_t)&glGetIntegerv},
    {"glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog},
    {"glGetProgramiv", (uintptr_t)&glGetProgramiv},
    {"glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog},
    {"glGetShaderiv", (uintptr_t)&glGetShaderiv},
    {"glGetString", (uintptr_t)&glGetString},
    {"glGetUniformLocation", (uintptr_t)&glGetUniformLocation},
    {"glLinkProgram", (uintptr_t)&glLinkProgram},
    {"glReadPixels", (uintptr_t)&glReadPixels},
    {"glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage},
    {"glScissor", (uintptr_t)&glScissor},
    {"glShaderSource", (uintptr_t)&glShaderSource},
    {"glStencilFunc", (uintptr_t)&glStencilFunc},
    {"glStencilMask", (uintptr_t)&glStencilMask},
    {"glStencilOp", (uintptr_t)&glStencilOp},
    {"glTexImage2D", (uintptr_t)&glTexImage2D},
    {"glTexParameteri", (uintptr_t)&glTexParameteri},
    {"glTexSubImage2D", (uintptr_t)&glTexSubImage2D},
    {"glUniform1fv", (uintptr_t)&glUniform1fv},
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
    {"pthread_attr_destroy", (uintptr_t)&my_pthread_attr_destroy_noop},
    {"pthread_attr_setstacksize", (uintptr_t)&my_pthread_attr_setstacksize_noop},
    {"pthread_attr_setdetachstate", (uintptr_t)&my_pthread_attr_setdetachstate_noop},
    {"pthread_detach", (uintptr_t)&pthread_detach},
    {"pthread_equal", (uintptr_t)&pthread_equal},
    {"pthread_exit", (uintptr_t)&pthread_exit},
    {"pthread_getspecific", (uintptr_t)&pthread_getspecific_fake},
    {"pthread_join", (uintptr_t)&pthread_join},
    {"pthread_key_create", (uintptr_t)&pthread_key_create},
    {"pthread_key_delete", (uintptr_t)&pthread_key_delete},
    {"pthread_kill", (uintptr_t)&pthread_kill},
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
    {"AAsset_openFileDescriptor64", (uintptr_t)&AAsset_openFileDescriptor64_fake},
    {"__google_potentially_blocking_region_begin", (uintptr_t)&__google_potentially_blocking_region_begin_stub},
    {"__google_potentially_blocking_region_end", (uintptr_t)&__google_potentially_blocking_region_end_stub},
    {"glBindFramebuffer", (uintptr_t)&glBindFramebuffer},
    {"eglMakeCurrent", (uintptr_t)&ff7_eglMakeCurrent},
    {"sem_wait", (uintptr_t)&ff7_sem_wait},
    {"sem_post", (uintptr_t)&ff7_sem_post},
    /* --- FF7: SL_IID_* extras (dummies nao-nulos) --- */
    {"SL_IID_NULL", SLD(0)}, {"SL_IID_OBJECT", SLD(1)}, {"SL_IID_OUTPUTMIX", SLD(2)},
    {"SL_IID_SEEK", SLD(3)}, {"SL_IID_PREFETCHSTATUS", SLD(4)}, {"SL_IID_PLAYBACKRATE", SLD(5)},
    {"SL_IID_PITCH", SLD(6)}, {"SL_IID_RATEPITCH", SLD(7)}, {"SL_IID_MUTESOLO", SLD(8)},
    {"SL_IID_METADATAEXTRACTION", SLD(9)}, {"SL_IID_METADATATRAVERSAL", SLD(10)},
    {"SL_IID_DYNAMICSOURCE", SLD(11)}, {"SL_IID_EFFECTSEND", SLD(12)},
    {"SL_IID_DEVICEVOLUME", SLD(13)}, {"SL_IID_RECORD", SLD(14)}, {"SL_IID_EQUALIZER", SLD(15)},
    {"SL_IID_LED", SLD(16)}, {"SL_IID_VIBRA", SLD(17)}, {"SL_IID_PRESETREVERB", SLD(18)},
    {"SL_IID_BASSBOOST", SLD(19)}, {"SL_IID_VIRTUALIZER", SLD(20)}, {"SL_IID_VISUALIZATION", SLD(21)},
    {"SL_IID_ENGINECAPABILITIES", SLD(22)}, {"SL_IID_THREADSYNC", SLD(23)},
    {"SL_IID_AUDIOIODEVICECAPABILITIES", SLD(24)}, {"SL_IID_AUDIODECODERCAPABILITIES", SLD(25)},
    {"SL_IID_AUDIOENCODER", SLD(26)}, {"SL_IID_AUDIOENCODERCAPABILITIES", SLD(27)},
    {"SL_IID_3DGROUPING", SLD(28)}, {"SL_IID_3DCOMMIT", SLD(29)}, {"SL_IID_3DLOCATION", SLD(30)},
    {"SL_IID_3DDOPPLER", SLD(31)}, {"SL_IID_3DSOURCE", SLD(32)}, {"SL_IID_3DMACROSCOPIC", SLD(33)},
    {"SL_IID_MIDIMESSAGE", SLD(34)}, {"SL_IID_MIDIMUTESOLO", SLD(35)}, {"SL_IID_MIDITEMPO", SLD(36)},
    {"SL_IID_MIDITIME", SLD(37)}, {"SL_IID_DYNAMICINTERFACEMANAGEMENT", SLD(38)},
    {"SL_IID_ANDROIDCONFIGURATION", SLD(39)}, {"SL_IID_ANDROIDEFFECT", SLD(40)},
    {"SL_IID_ANDROIDEFFECTSEND", SLD(41)}, {"SL_IID_ANDROIDEFFECTCAPABILITIES", SLD(42)},

    {"ANativeWindow_fromSurface", (uintptr_t)&ret1},
    {"ANativeWindow_getWidth", (uintptr_t)&ret1},
    {"ANativeWindow_getHeight", (uintptr_t)&ret1},
    {"ANativeWindow_setBuffersGeometry", (uintptr_t)&ret1},
};

const int dynlib_functions_count =
    sizeof(dynlib_functions) / sizeof(dynlib_functions[0]);
