/*
 * imports.c — shims bionic→glibc do NFS (os 18 imports que o dlsym fallback
 * não cobre: __sF/__errno/__assert2/__android_log/_ctype_/_tolower_tab_/
 * __cxa_type_match/__dso_handle/sigsetjmp/AndroidBitmap_*).
 * Exporta port_shims[] — main.c usa como base da tabela combinada.
 */
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/vfs.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "so_util.h"

extern void *sonic4ep1_dlopen(const char *filename, int flags);
extern void *sonic4ep1_dlsym(void *handle, const char *symbol);
extern int sonic4ep1_dlclose(void *handle);
extern char *sonic4ep1_dlerror(void);

/* ---- bionic __sF[3] = stdin/out/err (libc++ usa p/ std::cerr/cout) ---- */
static char bionic_sF[3][512];
static FILE *map_sF(void *fp) {
  if (fp == (void *)&bionic_sF[0]) return stdin;
  if (fp == (void *)&bionic_sF[1]) return stdout;
  if (fp == (void *)&bionic_sF[2]) return stderr;
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
  (void)prio; va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[%s] ", tag ? tag : "nfs"); vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n"); va_end(ap); return 0;
}
static int b_log_write(int prio, const char *tag, const char *msg) {
  (void)prio; fprintf(stderr, "[%s] %s\n", tag ? tag : "nfs", msg ? msg : ""); return 0;
}
static void b_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
  (void)cond; va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[%s ASSERT] ", tag ? tag : "nfs"); vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n"); va_end(ap); abort();
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
static void b_google_blocking_begin(void) {}
static void b_google_blocking_end(void) {}
static int b_sigsetjmp(sigjmp_buf env, int save) { return sigsetjmp(env, save); }
static int b_atexit(void (*fn)(void)) { return atexit(fn); }

/* ---- allocator isolado para a lib Android ----
 * Marmalade antigo escreve em estruturas assumindo o layout do allocator Android.
 * Deixar malloc/free cairem direto na glibc corrompe metadados e aborta em
 * operator new durante o init de Surface/GL. A arena abaixo fica fora do heap
 * da glibc; free vira no-op e realloc copia para um bloco novo. */
#define ARENA_MAGIC 0x53344531u
#define ARENA_CHUNK_MAX 128
#define ARENA_DEFAULT_CHUNK (16u * 1024u * 1024u)

struct arena_hdr {
  uint32_t magic;
  uint32_t flags;
  size_t size;
  size_t alloc_size;
};

struct arena_chunk {
  unsigned char *base;
  size_t size;
  size_t used;
};

static struct arena_chunk g_arena_chunks[ARENA_CHUNK_MAX];
static int g_arena_chunks_n;

static int use_arena_allocator(void) {
  static int cached = -1;
  if (cached < 0)
    cached = getenv("SONIC4EP1_ARENA") ? 1 : 0;
  return cached;
}

static size_t arena_align(size_t v) {
  return (v + 15u) & ~(size_t)15u;
}

static size_t arena_chunk_size(size_t need) {
  size_t chunk = ARENA_DEFAULT_CHUNK;
  const char *e = getenv("SONIC4EP1_ARENA_CHUNK_MB");
  if (e && *e) {
    long mb = strtol(e, NULL, 10);
    if (mb >= 1 && mb <= 128)
      chunk = (size_t)mb * 1024u * 1024u;
  }
  while (chunk < need)
    chunk <<= 1;
  return arena_align(chunk);
}

static void *w_malloc(size_t size) {
  if (!use_arena_allocator())
    return malloc(size);
  if (size == 0)
    size = 1;

  size_t need = arena_align(sizeof(struct arena_hdr) + size + 64u);
  for (int i = 0; i < g_arena_chunks_n; i++) {
    struct arena_chunk *c = &g_arena_chunks[i];
    if (c->size - c->used >= need) {
      unsigned char *p = c->base + c->used;
      c->used += need;
      struct arena_hdr *h = (struct arena_hdr *)p;
      h->magic = ARENA_MAGIC;
      h->flags = 0;
      h->size = size;
      h->alloc_size = need;
      return p + sizeof(*h);
    }
  }

  if (g_arena_chunks_n >= ARENA_CHUNK_MAX)
    return NULL;

  size_t chunk_size = arena_chunk_size(need);
  void *mem = mmap(NULL, chunk_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED)
    return NULL;

  struct arena_chunk *c = &g_arena_chunks[g_arena_chunks_n++];
  c->base = (unsigned char *)mem;
  c->size = chunk_size;
  c->used = 0;
  return w_malloc(size);
}

static void *w_calloc(size_t nmemb, size_t size) {
  size_t total = nmemb * size;
  if (size && total / size != nmemb)
    return NULL;
  if (!use_arena_allocator())
    return calloc(nmemb, size);
  void *p = w_malloc(total);
  if (p)
    memset(p, 0, total);
  return p;
}

static void w_free(void *ptr) {
  if (!use_arena_allocator()) {
    free(ptr);
    return;
  }
  (void)ptr;
}

static void *w_realloc(void *ptr, size_t size) {
  if (!use_arena_allocator())
    return realloc(ptr, size);
  if (!ptr)
    return w_malloc(size);
  if (size == 0) {
    w_free(ptr);
    return NULL;
  }

  struct arena_hdr *h = (struct arena_hdr *)((unsigned char *)ptr - sizeof(*h));
  size_t old_size = (h->magic == ARENA_MAGIC) ? h->size : 0;
  void *n = w_malloc(size);
  if (n && old_size) {
    if (old_size > size)
      old_size = size;
    memcpy(n, ptr, old_size);
  }
  return n;
}

static char *w_strdup(const char *s) {
  if (!s)
    return NULL;
  if (!use_arena_allocator())
    return strdup(s);
  size_t len = strlen(s) + 1;
  char *p = (char *)w_malloc(len);
  if (p)
    memcpy(p, s, len);
  return p;
}

static void *w_valloc(size_t size) {
  if (!use_arena_allocator()) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    if (page < 4096)
      page = 4096;
    void *p = NULL;
    if (posix_memalign(&p, page, size) != 0)
      return NULL;
    return p;
  }
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  if (page < 4096)
    page = 4096;
  size_t need = arena_align(sizeof(struct arena_hdr) + size + page);
  void *raw = w_malloc(need);
  if (!raw)
    return NULL;
  uintptr_t aligned = ((uintptr_t)raw + page - 1u) & ~(uintptr_t)(page - 1u);
  struct arena_hdr *h = (struct arena_hdr *)(aligned - sizeof(*h));
  h->magic = ARENA_MAGIC;
  h->flags = 1;
  h->size = size;
  h->alloc_size = need;
  return (void *)aligned;
}

/* ---- pthread bionic-safe ----
 * A lib Android antiga passa pthread_mutex_t/pthread_cond_t com layout bionic
 * pequeno. Usar pthread_* da glibc direto escreve estruturas maiores na memoria
 * da engine e corrompe heap/estado. Aqui o ponteiro bionic vira apenas chave; o
 * objeto pthread real vive em side tables da glibc. */
#define TLS_MAX 64
#define PTHREAD_SIDE_MAX 256
static int g_next_tls_key = 1;
static _Thread_local void *g_tls_values[TLS_MAX];

struct mutex_side {
  void *key;
  pthread_mutex_t host;
  int inited;
};

struct cond_side {
  void *key;
  pthread_cond_t host;
  int inited;
};

static pthread_mutex_t g_pthread_side_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mutex_side g_mutex_side[PTHREAD_SIDE_MAX];
static struct cond_side g_cond_side[PTHREAD_SIDE_MAX];

static struct mutex_side *mutex_side_get(void *key, int create) {
  if (!key)
    return NULL;
  pthread_mutex_lock(&g_pthread_side_lock);
  struct mutex_side *free_slot = NULL;
  for (int i = 0; i < PTHREAD_SIDE_MAX; i++) {
    if (g_mutex_side[i].key == key) {
      pthread_mutex_unlock(&g_pthread_side_lock);
      return &g_mutex_side[i];
    }
    if (!g_mutex_side[i].key && !free_slot)
      free_slot = &g_mutex_side[i];
  }
  if (!create || !free_slot) {
    pthread_mutex_unlock(&g_pthread_side_lock);
    return NULL;
  }
  free_slot->key = key;
  pthread_mutex_init(&free_slot->host, NULL);
  free_slot->inited = 1;
  pthread_mutex_unlock(&g_pthread_side_lock);
  return free_slot;
}

static struct cond_side *cond_side_get(void *key, int create) {
  if (!key)
    return NULL;
  pthread_mutex_lock(&g_pthread_side_lock);
  struct cond_side *free_slot = NULL;
  for (int i = 0; i < PTHREAD_SIDE_MAX; i++) {
    if (g_cond_side[i].key == key) {
      pthread_mutex_unlock(&g_pthread_side_lock);
      return &g_cond_side[i];
    }
    if (!g_cond_side[i].key && !free_slot)
      free_slot = &g_cond_side[i];
  }
  if (!create || !free_slot) {
    pthread_mutex_unlock(&g_pthread_side_lock);
    return NULL;
  }
  free_slot->key = key;
  pthread_cond_init(&free_slot->host, NULL);
  free_slot->inited = 1;
  pthread_mutex_unlock(&g_pthread_side_lock);
  return free_slot;
}

static void mutex_side_destroy(void *key) {
  if (!key)
    return;
  pthread_mutex_lock(&g_pthread_side_lock);
  for (int i = 0; i < PTHREAD_SIDE_MAX; i++) {
    if (g_mutex_side[i].key == key) {
      if (g_mutex_side[i].inited)
        pthread_mutex_destroy(&g_mutex_side[i].host);
      memset(&g_mutex_side[i], 0, sizeof(g_mutex_side[i]));
      break;
    }
  }
  pthread_mutex_unlock(&g_pthread_side_lock);
}

static void cond_side_destroy(void *key) {
  if (!key)
    return;
  pthread_mutex_lock(&g_pthread_side_lock);
  for (int i = 0; i < PTHREAD_SIDE_MAX; i++) {
    if (g_cond_side[i].key == key) {
      if (g_cond_side[i].inited)
        pthread_cond_destroy(&g_cond_side[i].host);
      memset(&g_cond_side[i], 0, sizeof(g_cond_side[i]));
      break;
    }
  }
  pthread_mutex_unlock(&g_pthread_side_lock);
}

static int b_pthread_attr_init(void *attr) {
  if (attr)
    *(uint32_t *)attr = 0;
  return 0;
}
static int b_pthread_attr_setstack(void *attr, void *stackaddr,
                                   size_t stacksize) {
  (void)attr; (void)stackaddr; (void)stacksize; return 0;
}
static int b_pthread_attr_setstacksize(void *attr, size_t stacksize) {
  (void)attr; (void)stacksize; return 0;
}
static int b_pthread_mutex_init(void *mutex, const void *attr) {
  (void)attr;
  if (mutex)
    *(uint32_t *)mutex = 0;
  return mutex_side_get(mutex, 1) ? 0 : -1;
}
static int b_pthread_mutex_destroy(void *mutex) {
  mutex_side_destroy(mutex);
  return 0;
}
static int b_pthread_mutex_lock(void *mutex) {
  struct mutex_side *m = mutex_side_get(mutex, 1);
  return m ? pthread_mutex_lock(&m->host) : -1;
}
static int b_pthread_mutex_trylock(void *mutex) {
  struct mutex_side *m = mutex_side_get(mutex, 1);
  return m ? pthread_mutex_trylock(&m->host) : -1;
}
static int b_pthread_mutex_unlock(void *mutex) {
  struct mutex_side *m = mutex_side_get(mutex, 1);
  return m ? pthread_mutex_unlock(&m->host) : -1;
}
static int b_pthread_cond_init(void *cond, const void *attr) {
  (void)attr;
  if (cond)
    *(uint32_t *)cond = 0;
  return cond_side_get(cond, 1) ? 0 : -1;
}
static int b_pthread_cond_destroy(void *cond) {
  cond_side_destroy(cond);
  return 0;
}
static int b_pthread_cond_wait(void *cond, void *mutex) {
  struct cond_side *c = cond_side_get(cond, 1);
  struct mutex_side *m = mutex_side_get(mutex, 1);
  return (c && m) ? pthread_cond_wait(&c->host, &m->host) : -1;
}
static int b_pthread_cond_timedwait(void *cond, void *mutex,
                                    const struct timespec *abstime) {
  struct cond_side *c = cond_side_get(cond, 1);
  struct mutex_side *m = mutex_side_get(mutex, 1);
  if (!c || !m)
    return -1;

  struct timespec ts;
  if (abstime) {
    const int32_t *bionic_ts = (const int32_t *)abstime;
    ts.tv_sec = (time_t)bionic_ts[0];
    ts.tv_nsec = (long)bionic_ts[1];
  } else {
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 10000000L;
  }
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
  }
  return pthread_cond_timedwait(&c->host, &m->host, &ts);
}
static int b_pthread_cond_broadcast(void *cond) {
  struct cond_side *c = cond_side_get(cond, 1);
  return c ? pthread_cond_broadcast(&c->host) : -1;
}
static int b_pthread_cond_signal(void *cond) {
  struct cond_side *c = cond_side_get(cond, 1);
  return c ? pthread_cond_signal(&c->host) : -1;
}
static int b_pthread_create(uint32_t *thread, const void *attr,
                            void *(*start_routine)(void *), void *arg) {
  (void)attr;
  pthread_t t;
  int r = pthread_create(&t, NULL, start_routine, arg);
  if (!r && thread)
    *thread = (uint32_t)(uintptr_t)t;
  return r;
}
static int b_pthread_join(uint32_t thread, void **retval) {
  return pthread_join((pthread_t)(uintptr_t)thread, retval);
}
static void b_pthread_exit(void *retval) {
  pthread_exit(retval);
}
static uint32_t b_pthread_self(void) {
  return (uint32_t)(uintptr_t)pthread_self();
}
static int b_pthread_equal(uint32_t a, uint32_t b) {
  return a == b;
}
static int b_pthread_key_create(uint32_t *key, void (*destructor)(void *)) {
  (void)destructor;
  if (!key || g_next_tls_key >= TLS_MAX)
    return -1;
  *key = (uint32_t)g_next_tls_key++;
  return 0;
}
static int b_pthread_key_delete(uint32_t key) {
  if (key < TLS_MAX)
    g_tls_values[key] = NULL;
  return 0;
}
static int b_pthread_setspecific(uint32_t key, const void *value) {
  if (key >= TLS_MAX)
    return -1;
  g_tls_values[key] = (void *)value;
  return 0;
}
static void *b_pthread_getspecific(uint32_t key) {
  if (key >= TLS_MAX)
    return NULL;
  return g_tls_values[key];
}
static int b_pthread_once(uint32_t *once_control, void (*init_routine)(void)) {
  if (!once_control)
    return 0;
  if (*once_control)
    return 0;
  *once_control = 1;
  if (init_routine)
    init_routine();
  return 0;
}

static int trace_io(void) {
  static int cached = -1;
  if (cached < 0)
    cached = getenv("SONIC4EP1_TRACE_IO") ? 1 : 0;
  return cached;
}

struct traced_file {
  FILE *fp;
  char path[192];
  size_t total_read;
  int reads_logged;
};

static struct traced_file g_traced_files[64];

static struct traced_file *trace_file_slot(FILE *fp, int create,
                                           const char *path) {
  if (!fp)
    return NULL;
  for (size_t i = 0; i < sizeof(g_traced_files) / sizeof(g_traced_files[0]);
       i++) {
    if (g_traced_files[i].fp == fp)
      return &g_traced_files[i];
  }
  if (!create)
    return NULL;
  for (size_t i = 0; i < sizeof(g_traced_files) / sizeof(g_traced_files[0]);
       i++) {
    if (!g_traced_files[i].fp) {
      g_traced_files[i].fp = fp;
      g_traced_files[i].total_read = 0;
      g_traced_files[i].reads_logged = 0;
      snprintf(g_traced_files[i].path, sizeof(g_traced_files[i].path), "%s",
               path ? path : "(null)");
      return &g_traced_files[i];
    }
  }
  return NULL;
}

static const char *ci_path(const char *path, char *buf, size_t bufsz);

/* Marmalade s3e VFS scheme "raw://" = caminho real do SO. Alguns paths chegam
 * com o scheme LITERAL nos wrappers de libc do exec -> ENOENT. Removemos o
 * prefixo p/ o arquivo ser achado (ex.: package.dz do OBB). "raw://" = 6 chars. */
static const char *strip_scheme(const char *path) {
  if (path && strncmp(path, "raw://", 6) == 0) {
    if (getenv("PES_TRACE_RAW"))
      fprintf(stderr, "[raw] \"%s\" -> \"%s\"\n", path, path + 6);
    return path + 6;
  }
  return path;
}

static FILE *w_fopen(const char *path, const char *mode) {
  char buf[1024];
  path = strip_scheme(path);
  if (mode && (mode[0] == 'r'))
    path = ci_path(path, buf, sizeof(buf));
  FILE *r = fopen(path, mode);
  if (trace_io()) {
    trace_file_slot(r, 1, path);
    fprintf(stderr, "[io] fopen(\"%s\", \"%s\") -> %p errno=%d\n",
            path ? path : "(null)", mode ? mode : "(null)", (void *)r, errno);
  }
  return r;
}

static FILE *w_fopen64(const char *path, const char *mode) {
  return w_fopen(path, mode);
}

/* Resolve `in` case-insensitively p/ um caminho existente em `out`. O jogo pede
 * assets com case misto (menuAssetLoader.group.bin) mas os arquivos são
 * minúsculos -> access()/open() case-sensitive falham. Anda componente a
 * componente casando ci via readdir. Retorna 1 se resolveu p/ arquivo existente. */
static int ci_resolve(const char *in, char *out, size_t outsz) {
  if (!in || !out || !outsz)
    return 0;
  if (access(in, F_OK) == 0) {
    snprintf(out, outsz, "%s", in);
    return 1;
  }
  size_t pos = 0;
  out[0] = 0;
  const char *p = in;
  if (*p == '/') {
    out[pos++] = '/';
    out[pos] = 0;
    p++;
  }
  while (*p) {
    const char *slash = strchr(p, '/');
    size_t clen = slash ? (size_t)(slash - p) : strlen(p);
    char comp[256];
    if (clen == 0) { p = slash + 1; continue; }
    if (clen >= sizeof(comp))
      return 0;
    memcpy(comp, p, clen);
    comp[clen] = 0;
    char cur[1024];
    snprintf(cur, sizeof(cur), "%s%s", out, comp);
    if (access(cur, F_OK) != 0) {
      const char *dir = out[0] ? out : ".";
      DIR *d = opendir(dir);
      int found = 0;
      if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
          if (strcasecmp(e->d_name, comp) == 0) {
            snprintf(cur, sizeof(cur), "%s%s", out, e->d_name);
            found = 1;
            break;
          }
        }
        closedir(d);
      }
      if (!found)
        return 0;
    }
    pos = (size_t)snprintf(out, outsz, "%s", cur);
    if (slash) {
      if (pos + 1 < outsz) {
        out[pos++] = '/';
        out[pos] = 0;
      }
      p = slash + 1;
    } else {
      break;
    }
  }
  return access(out, F_OK) == 0;
}

/* devolve o path resolvido ci ou o original */
static const char *ci_path(const char *path, char *buf, size_t bufsz) {
  if (path && ci_resolve(path, buf, bufsz))
    return buf;
  return path;
}

static int w_open(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  char buf[1024];
  path = strip_scheme(path);
  if (!(flags & O_CREAT))
    path = ci_path(path, buf, sizeof(buf));
  int r = open(path, flags, mode);
  if (trace_io())
    fprintf(stderr, "[io] open(\"%s\", 0x%x) -> %d errno=%d\n",
            path ? path : "(null)", flags, r, errno);
  return r;
}

static int w_open64(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  path = strip_scheme(path);
  int r = open(path, flags, mode);
  if (trace_io())
    fprintf(stderr, "[io] open64(\"%s\", 0x%x) -> %d errno=%d\n",
            path ? path : "(null)", flags, r, errno);
  return r;
}

static int w_access(const char *path, int mode) {
  char buf[1024];
  path = strip_scheme(path);
  const char *rp = ci_path(path, buf, sizeof(buf));
  int r = access(rp, mode);
  if (trace_io())
    fprintf(stderr, "[io] access(\"%s\"->\"%s\", 0x%x) -> %d errno=%d\n",
            path ? path : "(null)", rp, mode, r, errno);
  return r;
}

static int w_stat(const char *path, struct stat *st) {
  char buf[1024];
  path = ci_path(path, buf, sizeof(buf));
  int r = stat(path, st);
  if (trace_io())
    fprintf(stderr, "[io] stat(\"%s\") -> %d errno=%d\n",
            path ? path : "(null)", r, errno);
  return r;
}

static int w_stat64(const char *path, struct stat *st) {
  return w_stat(path, st);
}

/* O jogo checa espaço livre (statfs) antes de "instalar" o OBB (180MB). O vfat
 * de 120GB pode dar valores errados/overflow no struct 32-bit -> "Not enough
 * space" mesmo com GBs livres. Forçamos ~2GB livres. */
static int w_statfs(const char *path, struct statfs *buf) {
  int r = statfs(path, buf);
  if (r != 0)
    memset(buf, 0, sizeof(*buf));
  /* ~512MB livre: > 180MB exigido, MAS < 2GB p/ NÃO estourar o cálculo 32-bit
   * signed do jogo (2.7GB reais estouram -> negativo -> "not enough space"). */
  buf->f_bsize = 4096;
  buf->f_frsize = 4096;
  buf->f_blocks = 262144; /* 1GB total */
  buf->f_bfree = 131072;  /* 512MB livre */
  buf->f_bavail = 131072; /* 512MB livre */
  if (trace_io())
    fprintf(stderr, "[io] statfs(\"%s\") -> forçando 512MB livre\n",
            path ? path : "(null)");
  return 0;
}
static int w_statfs64(const char *path, struct statfs *buf) {
  return w_statfs(path, buf);
}
static int w_fstatfs(int fd, struct statfs *buf) {
  int r = fstatfs(fd, buf);
  if (r != 0)
    memset(buf, 0, sizeof(*buf));
  buf->f_bsize = 4096;
  buf->f_frsize = 4096;
  buf->f_blocks = 262144;
  buf->f_bfree = 131072; /* 512MB */
  buf->f_bavail = 131072;
  return 0;
}
static int w_fstatfs64(int fd, size_t sz, struct statfs *buf) {
  (void)sz;
  return w_fstatfs(fd, buf);
}

static size_t w_fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
  size_t r = fread(ptr, size, nmemb, fp);
  if (trace_io()) {
    struct traced_file *tf = trace_file_slot(fp, 0, NULL);
    size_t bytes = r * size;
    if (tf) {
      tf->total_read += bytes;
      if (tf->reads_logged < 32 || bytes == 0 || (tf->total_read % (1024 * 1024)) < bytes) {
        fprintf(stderr,
                "[io] fread(%p \"%s\", %zu*%zu) -> %zu bytes=%zu total=%zu errno=%d\n",
                (void *)fp, tf->path, size, nmemb, r, bytes, tf->total_read,
                errno);
        tf->reads_logged++;
      }
    } else {
      fprintf(stderr, "[io] fread(%p, %zu*%zu) -> %zu bytes=%zu errno=%d\n",
              (void *)fp, size, nmemb, r, bytes, errno);
    }
  }
  return r;
}

static int w_fseek(FILE *fp, long offset, int whence) {
  int r = fseek(fp, offset, whence);
  if (trace_io()) {
    struct traced_file *tf = trace_file_slot(fp, 0, NULL);
    fprintf(stderr, "[io] fseek(%p \"%s\", %ld, %d) -> %d errno=%d\n",
            (void *)fp, tf ? tf->path : "?", offset, whence, r, errno);
  }
  return r;
}

static long w_ftell(FILE *fp) {
  long r = ftell(fp);
  if (trace_io()) {
    struct traced_file *tf = trace_file_slot(fp, 0, NULL);
    fprintf(stderr, "[io] ftell(%p \"%s\") -> %ld errno=%d\n", (void *)fp,
            tf ? tf->path : "?", r, errno);
  }
  return r;
}

static int w_fclose(FILE *fp) {
  if (trace_io()) {
    struct traced_file *tf = trace_file_slot(fp, 0, NULL);
    fprintf(stderr, "[io] fclose(%p \"%s\") total=%zu errno=%d\n", (void *)fp,
            tf ? tf->path : "?", tf ? tf->total_read : 0, errno);
    if (tf)
      memset(tf, 0, sizeof(*tf));
  }
  return fclose(fp);
}

/* ---- __cxa_guard estilo bionic/gnustl ----
 * O guard da libstdc++ do host pode entrar em futex/deadlock com static local
 * vindo de binario Android antigo. byte0=done, byte1=in-progress. */
static int b_cxa_guard_acquire(unsigned char *g) {
  if (!g || g[0])
    return 0;
  if (g[1])
    return 0;
  g[1] = 1;
  return 1;
}
static void b_cxa_guard_release(unsigned char *g) {
  if (!g)
    return;
  g[0] = 1;
  g[1] = 0;
}
static void b_cxa_guard_abort(unsigned char *g) {
  if (g)
    g[1] = 0;
}

/* AndroidBitmap (jnigraphics) — stub: sinaliza erro p/ a engine cair no fallback */
static int abm_getInfo(void *env, void *bmp, void *info) { (void)env; (void)bmp; (void)info; return -1; }
static int abm_lock(void *env, void *bmp, void **pix) { (void)env; (void)bmp; if (pix) *pix = 0; return -1; }
static int abm_unlock(void *env, void *bmp) { (void)env; (void)bmp; return 0; }

DynLibFunction port_shims[] = {
    {"__sF", (uintptr_t)bionic_sF},
    {"fprintf", (uintptr_t)w_fprintf}, {"vfprintf", (uintptr_t)w_vfprintf},
    {"fwrite", (uintptr_t)w_fwrite}, {"fputs", (uintptr_t)w_fputs},
    {"fputc", (uintptr_t)w_fputc}, {"fflush", (uintptr_t)w_fflush},
    {"__errno", (uintptr_t)b_errno},
    {"__assert2", (uintptr_t)b_assert2},
    {"__android_log_print", (uintptr_t)b_log_print},
    {"__android_log_write", (uintptr_t)b_log_write},
    {"__android_log_assert", (uintptr_t)b_log_assert},
    {"_ctype_", (uintptr_t)b_ctype},
    {"_tolower_tab_", (uintptr_t)b_tolower},
    {"_toupper_tab_", (uintptr_t)b_toupper},
    {"__dso_handle", (uintptr_t)&b_dso_handle},
    {"__cxa_type_match", (uintptr_t)b_cxa_type_match},
    {"__cxa_guard_acquire", (uintptr_t)b_cxa_guard_acquire},
    {"__cxa_guard_release", (uintptr_t)b_cxa_guard_release},
    {"__cxa_guard_abort", (uintptr_t)b_cxa_guard_abort},
    {"__google_potentially_blocking_region_begin", (uintptr_t)b_google_blocking_begin},
    {"__google_potentially_blocking_region_end", (uintptr_t)b_google_blocking_end},
    {"sigsetjmp", (uintptr_t)b_sigsetjmp},
    {"malloc", (uintptr_t)w_malloc},
    {"calloc", (uintptr_t)w_calloc},
    {"realloc", (uintptr_t)w_realloc},
    {"free", (uintptr_t)w_free},
    /* C++ operators (libstdc++ nao linkado; PES os importa e ficavam
     * UNRESOLVED -> operator new[] saltava pro nada e crashava no parser de
     * config). ABI-compat com w_malloc/w_free (size_t/void* em 32-bit). */
    {"_Znwj", (uintptr_t)w_malloc},   /* operator new(unsigned) */
    {"_Znaj", (uintptr_t)w_malloc},   /* operator new[](unsigned) */
    {"_ZdlPv", (uintptr_t)w_free},    /* operator delete(void*) */
    {"_ZdaPv", (uintptr_t)w_free},    /* operator delete[](void*) */
    {"atexit", (uintptr_t)b_atexit},
    {"strdup", (uintptr_t)w_strdup},
    {"valloc", (uintptr_t)w_valloc},
    {"pthread_attr_init", (uintptr_t)b_pthread_attr_init},
    {"pthread_attr_setstack", (uintptr_t)b_pthread_attr_setstack},
    {"pthread_attr_setstacksize", (uintptr_t)b_pthread_attr_setstacksize},
    {"pthread_mutex_init", (uintptr_t)b_pthread_mutex_init},
    {"pthread_mutex_destroy", (uintptr_t)b_pthread_mutex_destroy},
    {"pthread_mutex_lock", (uintptr_t)b_pthread_mutex_lock},
    {"pthread_mutex_trylock", (uintptr_t)b_pthread_mutex_trylock},
    {"pthread_mutex_unlock", (uintptr_t)b_pthread_mutex_unlock},
    {"pthread_cond_init", (uintptr_t)b_pthread_cond_init},
    {"pthread_cond_destroy", (uintptr_t)b_pthread_cond_destroy},
    {"pthread_cond_wait", (uintptr_t)b_pthread_cond_wait},
    {"pthread_cond_timedwait", (uintptr_t)b_pthread_cond_timedwait},
    {"pthread_cond_broadcast", (uintptr_t)b_pthread_cond_broadcast},
    {"pthread_cond_signal", (uintptr_t)b_pthread_cond_signal},
    {"pthread_create", (uintptr_t)b_pthread_create},
    {"pthread_join", (uintptr_t)b_pthread_join},
    {"pthread_exit", (uintptr_t)b_pthread_exit},
    {"pthread_self", (uintptr_t)b_pthread_self},
    {"pthread_equal", (uintptr_t)b_pthread_equal},
    {"pthread_key_create", (uintptr_t)b_pthread_key_create},
    {"pthread_key_delete", (uintptr_t)b_pthread_key_delete},
    {"pthread_setspecific", (uintptr_t)b_pthread_setspecific},
    {"pthread_getspecific", (uintptr_t)b_pthread_getspecific},
    {"pthread_once", (uintptr_t)b_pthread_once},
    {"fopen", (uintptr_t)w_fopen},
    {"fopen64", (uintptr_t)w_fopen64},
    {"fread", (uintptr_t)w_fread},
    {"fseek", (uintptr_t)w_fseek},
    {"ftell", (uintptr_t)w_ftell},
    {"fclose", (uintptr_t)w_fclose},
    {"dlopen", (uintptr_t)sonic4ep1_dlopen},
    {"dlsym", (uintptr_t)sonic4ep1_dlsym},
    {"dlclose", (uintptr_t)sonic4ep1_dlclose},
    {"dlerror", (uintptr_t)sonic4ep1_dlerror},
    {"open", (uintptr_t)w_open},
    {"open64", (uintptr_t)w_open64},
    {"access", (uintptr_t)w_access},
    {"stat", (uintptr_t)w_stat},
    {"statfs", (uintptr_t)w_statfs},
    {"statfs64", (uintptr_t)w_statfs64},
    {"fstatfs", (uintptr_t)w_fstatfs},
    {"fstatfs64", (uintptr_t)w_fstatfs64},
    {"stat64", (uintptr_t)w_stat64},
    {"AndroidBitmap_getInfo", (uintptr_t)abm_getInfo},
    {"AndroidBitmap_lockPixels", (uintptr_t)abm_lock},
    {"AndroidBitmap_unlockPixels", (uintptr_t)abm_unlock},
};
int port_shims_count = sizeof(port_shims) / sizeof(port_shims[0]);
