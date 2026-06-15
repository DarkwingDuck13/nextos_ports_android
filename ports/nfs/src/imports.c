/*
 * imports.c — shims bionic→glibc do NFS (os 18 imports que o dlsym fallback
 * não cobre: __sF/__errno/__assert2/__android_log/_ctype_/_tolower_tab_/
 * __cxa_type_match/__dso_handle/sigsetjmp/AndroidBitmap_*).
 * Exporta nfs_shims[] — main.c usa como base da tabela combinada.
 */
#define _LARGEFILE64_SOURCE
#include <ctype.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <malloc.h>
#include <dlfcn.h>
#include <string.h>

#include "so_util.h"
#include "egl_shim.h"

/* ---- bionic __sF[3] = stdin/out/err (libc++/EAStdC usam p/ cerr/cout/fflush) ----
 * 🔑 O struct __sFILE do BIONIC tem 84 bytes (0x54): __sF[0]=base+0, __sF[1]
 * (stdout)=base+0x54, __sF[2] (stderr)=base+0xA8. (A engine faz fflush(__sF+0x54).)
 * O buffer precisa do stride 0x54 senão map_sF não casa o ponteiro → fflush
 * recebe um FILE* lixo → SIGSEGV. */
#define BIONIC_FILE_SZ 0x54
static char bionic_sF[3 * BIONIC_FILE_SZ + 64];
static FILE *map_sF(void *fp) {
  if (fp == (void *)(bionic_sF + 0 * BIONIC_FILE_SZ)) return stdin;
  if (fp == (void *)(bionic_sF + 1 * BIONIC_FILE_SZ)) return stdout;
  if (fp == (void *)(bionic_sF + 2 * BIONIC_FILE_SZ)) return stderr;
  return (FILE *)fp;
}
/* 🔑 filtro de SPAM de log da engine: "EventSystem is null" (SoundManager) é
 * emitido ~1000x/frame (EventSystem do FMOD nulo) → enche o tmpfs (OOM/SIGKILL) e
 * trava a engine a ~1fps no I/O. Dropa essas linhas na fonte. NFS_NOQUIET=1 off. */
static int g_quiet = -1;
static int nfs_log_spam(const char *s, size_t n) {
  if (!s) return 0;
  if (g_quiet < 0) g_quiet = getenv("NFS_NOQUIET") ? 0 : 1;
  if (!g_quiet) return 0;
  size_t lim = n < 160 ? n : 160;
  for (size_t i = 0; i + 11 <= lim; i++)
    if (s[i] == 'E' && memcmp(s + i, "EventSystem", 11) == 0) return 1;
  return 0;
}
static int w_fprintf(void *fp, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  char buf[600]; int r = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  if (nfs_log_spam(buf, r > 0 ? (size_t)r : 0)) return r;
  return fputs(buf, map_sF(fp));
}
static int w_vfprintf(void *fp, const char *fmt, va_list ap) {
  char buf[600]; int r = vsnprintf(buf, sizeof buf, fmt, ap);
  if (nfs_log_spam(buf, r > 0 ? (size_t)r : 0)) return r;
  return fputs(buf, map_sF(fp));
}
static size_t w_fwrite(const void *p, size_t s, size_t n, void *fp) {
  if (nfs_log_spam((const char *)p, s * n)) return n;
  return fwrite(p, s, n, map_sF(fp));
}
static int w_fputs(const char *str, void *fp) {
  if (nfs_log_spam(str, str ? strlen(str) : 0)) return 0;
  return fputs(str, map_sF(fp));
}
/* hooks diretos (a engine importa printf/puts/vprintf do libc) com mesmo filtro */
static int my_puts(const char *s) { if (nfs_log_spam(s, s ? strlen(s) : 0)) return 0; return puts(s); }
static int my_printf(const char *fmt, ...) {
  char buf[512]; va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  if (nfs_log_spam(buf, r > 0 ? (size_t)r : 0)) return r;
  return fputs(buf, stdout);
}
static int my_vprintf(const char *fmt, va_list ap) {
  char buf[512]; int r = vsnprintf(buf, sizeof buf, fmt, ap);
  if (nfs_log_spam(buf, r > 0 ? (size_t)r : 0)) return r;
  return fputs(buf, stdout);
}
/* write(2) catch-all: pega QUALQUER saída da engine p/ stdout/stderr (fd 1/2)
 * independente do caminho stdio (printf/puts/EA log/etc) e dropa o spam. Outros
 * fds (arquivos) passam direto. */
static ssize_t (*real_write)(int, const void *, size_t);
static ssize_t my_write(int fd, const void *buf, size_t n) {
  if (!real_write) real_write = (ssize_t (*)(int, const void *, size_t))dlsym(RTLD_DEFAULT, "write");
  if ((fd == 1 || fd == 2) && nfs_log_spam((const char *)buf, n)) return (ssize_t)n;
  return real_write(fd, buf, n);
}
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
  char buf[600]; int r = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  if (nfs_log_spam(buf, r > 0 ? (size_t)r : 0)) return 0;
  if (tag && nfs_log_spam(tag, strlen(tag))) return 0;
  fprintf(stderr, "[%s] %s\n", tag ? tag : "nfs", buf);
  return 0;
}
static int b_log_write(int prio, const char *tag, const char *msg) {
  (void)prio;
  if (msg && nfs_log_spam(msg, strlen(msg))) return 0;
  fprintf(stderr, "[%s] %s\n", tag ? tag : "nfs", msg ? msg : ""); return 0;
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

/* ---- malloc com PADDING (+64B) ----
 * A engine NFS faz pequenos overflows que o malloc do bionic tolera (layout de
 * chunk diferente) mas o glibc detecta ("malloc(): invalid size"). Over-alocar
 * absorve o overflow na folga, sem corromper a metadata do glibc. free/realloc
 * usam o mesmo ponteiro base → consistentes. */
static int nfs_pad_bytes(void) {
  static int v = -1;
  if (v < 0) { const char *e = getenv("NFS_PAD"); v = e ? atoi(e) : 64; if (v < 0) v = 0; }
  return v;
}
static void *pad_malloc(size_t n) { return malloc(n + nfs_pad_bytes()); }
static void *pad_calloc(size_t a, size_t b) {
  size_t t = a * b, pad = nfs_pad_bytes(); void *p = malloc(t + pad); if (p) memset(p, 0, t + pad); return p;
}
static void *pad_realloc(void *p, size_t n) { return realloc(p, n + nfs_pad_bytes()); }
static void *pad_memalign(size_t al, size_t n) { return memalign(al, n + nfs_pad_bytes()); }

/* ---- flags cacheados (NÃO chamar getenv em hot path) ----
 * 🔑 A engine importa setenv/unsetenv e roda worker threads; glibc setenv realoca
 * o array __environ e LIBERA o antigo. Um getenv concorrente caminhando o array
 * velho (já liberado) lê memória morta → SIGSEGV em getenv (libc+0x367ac). O
 * my_dynamic_cast chamava getenv 4x/cast num loop RTTI apertado → colidia com o
 * setenv da engine. Solução: ler os flags UMA vez no início (env estável, sem
 * threads ainda) e usar os globais. */
int g_nfs_dcastlog, g_nfs_rclog, g_nfs_wildlog, g_nfs_tichain;
int g_nfs_nodcastrec, g_nfs_norecover, g_nfs_noassertignore;
int g_nfs_dcwalk;  /* NFS_DCWALK=1 usa nosso walker; default=libcxxabi nativo (relocs corretas) */
int g_nfs_frames = 600; /* NFS_FRAMES cacheado cedo (a engine corrompe environ depois) */
void nfs_cache_flags(void) {
  { const char *f = getenv("NFS_FRAMES"); if (f && *f) g_nfs_frames = atoi(f); }
  g_nfs_dcwalk = getenv("NFS_DCWALK") != NULL;
  g_nfs_dcastlog = getenv("NFS_DCASTLOG") != NULL;
  g_nfs_rclog = getenv("NFS_RCLOG") != NULL;
  g_nfs_wildlog = getenv("NFS_WILDLOG") != NULL;
  g_nfs_tichain = getenv("NFS_TICHAIN") != NULL;
  g_nfs_nodcastrec = getenv("NFS_NODCASTREC") != NULL;
  g_nfs_norecover = getenv("NFS_NORECOVER") != NULL;
  g_nfs_noassertignore = getenv("NFS_NOASSERTIGNORE") != NULL;
  (void)nfs_pad_bytes(); /* fixa o PAD cedo também */
}

/* ---- pthread_create hook: loga a função de entrada de cada thread (p/
 * identificar a worker thread que crasha no init). NFS_PTLOG=1 liga. ---- */
static int (*real_pthread_create)(void *, const void *, void *(*)(void *), void *);
static int my_pthread_create(void *th, const void *attr, void *(*fn)(void *), void *arg) {
  if (!real_pthread_create) {
    real_pthread_create = (int (*)(void *, const void *, void *(*)(void *), void *))
        dlsym(RTLD_DEFAULT, "pthread_create");
  }
  if (getenv("NFS_PTLOG")) {
    static int n = 0;
    if (n < 40) { fprintf(stderr, "[pthread_create] #%d fn=%p arg=%p attr=%p\n", n, (void *)fn, arg, (void *)attr); n++; }
  }
  /* IGNORA o attr bionic (layout ≠ glibc → glibc lê lixo de stack-size). Passa
   * NULL = thread glibc default (8MB). NFS_PT_KEEPATTR=1 mantém o attr original. */
  if (!getenv("NFS_PT_KEEPATTR")) attr = NULL;
  return real_pthread_create(th, attr, fn, arg);
}

/* ---- fopen/open hook: loga acessos a arquivo (ver se a engine abre o OBB) ---- */
static void *(*real_fopen)(const char *, const char *);
void *g_obb_fp;  /* fp do OBB, p/ diagnóstico de leitura do índice ZIP */
static void *my_fopen(const char *path, const char *mode) {
  if (!real_fopen) real_fopen = (void *(*)(const char *, const char *))dlsym(RTLD_DEFAULT, "fopen");
  void *fp = real_fopen(path, mode);
  if (path && strstr(path, ".obb")) g_obb_fp = fp;
  if (getenv("NFS_FOPENLOG")) {
    static int n = 0;
    int tex = path && (strstr(path, "texturepack") || strstr(path, ".sba") || strstr(path, ".tif") || strstr(path, "splash"));
    if (tex || !fp || n < 60) {
      fprintf(stderr, "[fopen] '%s' (%s) -> %s\n", path ? path : "?", mode ? mode : "?", fp ? "OK" : "MISS"); n++; }
  }
  return fp;
}
static size_t (*real_fread)(void *, size_t, size_t, void *);
static size_t my_fread(void *p, size_t sz, size_t n, void *fp) {
  if (!real_fread) real_fread = (size_t(*)(void*,size_t,size_t,void*))dlsym(RTLD_DEFAULT, "fread");
  long pos_before = (fp == g_obb_fp && getenv("NFS_OBBDUMP")) ? ftell((FILE *)fp) : -1;
  size_t r = real_fread(p, sz, n, fp);
  { extern long nfs_io_read_bytes; nfs_io_read_bytes += (long)(r * sz); }
  /* 🔎 dump de blocos GRANDES lidos do OBB (índice ZIP) — ver se são entradas
   * válidas (PK\1\2 = central dir, PK\3\4 = local header). NFS_OBBDUMP=1. */
  if (pos_before >= 0 && r * sz >= 8) {
    const unsigned char *b = (const unsigned char *)p;
    /* só loga nomes de entrada que NÃO são texturepack (= base/databases) p/ ver
     * se o índice inclui published/data, published/fonts, etc. */
    char nm[80]; int L2 = (int)(r * sz); if (L2 > 79) L2 = 79;
    int printable = (b[0] >= 32 && b[0] < 127);
    if (printable) { memcpy(nm, b, L2); nm[L2] = 0;
      if (!strstr(nm, "texturepack")) {
        static int c = 0;
        if (c < 6000) { fprintf(stderr, "[obb-name] off=%ld len=%zu '%s'\n", pos_before, r * sz, nm); c++; }
      }
    }
  }
  if (getenv("NFS_FOPENLOG")) { static int c = 0; static size_t tot = 0; tot += r * sz;
    if (c < 30) { fprintf(stderr, "[fread] %zu*%zu -> %zu (fp=%p, total=%zu)\n", sz, n, r, fp, tot); c++; } }
  return r;
}
/* ---- stat/fstat/statfs: o struct stat do bionic ARM32 = kernel stat64 =
 * glibc stat64 (st_size 64-bit @ offset 48). O stat/fstat DEFAULT da glibc é o
 * layout ANTIGO (st_size 32-bit @ 44) → a engine lia st_blksize/st_blocks como
 * tamanho do OBB (lixo) → seek/parse do índice garbage. Rotear p/ as *64. ---- */
static int b_stat(const char *p, void *st) {
  int rc = stat64(p, (struct stat64 *)st);
  if (getenv("NFS_SEEKLOG")) {
    static int n = 0;
    if (n < 30) { fprintf(stderr, "[stat] '%s' -> rc=%d size=%lld\n", p ? p : "?", rc, rc == 0 ? (long long)((struct stat64 *)st)->st_size : -1LL); n++; }
  }
  return rc;
}
static int b_fstat(int fd, void *st) {
  int rc = fstat64(fd, (struct stat64 *)st);
  if (getenv("NFS_SEEKLOG")) {
    static int n = 0;
    if (n < 30) { fprintf(stderr, "[fstat] fd=%d -> rc=%d size=%lld\n", fd, rc, rc == 0 ? (long long)((struct stat64 *)st)->st_size : -1LL); n++; }
  }
  return rc;
}
static int b_statfs(const char *p, void *st) { return statfs64(p, (struct statfs64 *)st); }

/* ---- seek hooks (NFS_SEEKLOG=1): ver o padrão de leitura do índice do OBB ----
 * Estado de I/O em globals p/ o crash handler imprimir onde a caminhada parou. */
long nfs_io_last_seek = -1;
long nfs_io_seeks = 0;
long nfs_io_read_bytes = 0;
static int my_fseek(void *fp, long off, int wh) {
  int rc = fseek((FILE *)fp, off, wh);
  nfs_io_last_seek = off; nfs_io_seeks++;
  if (getenv("NFS_SEEKLOG")) {
    static int n = 0;
    if (n < 60) { fprintf(stderr, "[fseek] fp=%p off=%ld wh=%d -> %d (pos=%ld)\n", fp, off, wh, rc, ftell((FILE *)fp)); n++; }
  }
  return rc;
}
static long my_ftell(void *fp) {
  long r = ftell((FILE *)fp);
  if (getenv("NFS_SEEKLOG")) {
    static int n = 0;
    if (n < 60) { fprintf(stderr, "[ftell] fp=%p -> %ld\n", fp, r); n++; }
  }
  return r;
}
static off_t my_lseek(int fd, off_t off, int wh) {
  off_t r = lseek(fd, off, wh);
  if (getenv("NFS_SEEKLOG")) {
    static int n = 0;
    if (n < 60) { fprintf(stderr, "[lseek] fd=%d off=%ld wh=%d -> %ld\n", fd, (long)off, wh, (long)r); n++; }
  }
  return r;
}
static long long my_lseek64(int fd, long long off, int wh) {
  long long r = lseek64(fd, off, wh);
  if (getenv("NFS_SEEKLOG")) {
    static int n = 0;
    if (n < 60) { fprintf(stderr, "[lseek64] fd=%d off=%lld wh=%d -> %lld\n", fd, off, wh, r); n++; }
  }
  return r;
}

static int (*real_open)(const char *, int, ...);
static int my_open(const char *path, int flags, ...) {
  if (!real_open) real_open = (int (*)(const char *, int, ...))dlsym(RTLD_DEFAULT, "open");
  int fd = real_open(path, flags, 0666);
  if (getenv("NFS_FOPENLOG")) {
    static int n = 0;
    if (n < 60) { fprintf(stderr, "[open] '%s' -> %s\n", path ? path : "?", fd >= 0 ? "OK" : "MISS"); n++; }
  }
  return fd;
}

/* ---- dlsym hook: a engine pode dlsym funções math em runtime e chamá-las
 * (softfp). Retornamos o wrapper softfp_resolve ANTES do dlsym real (senão
 * pega a versão HARDFP do glibc → crash). NFS_DLSYMLOG=1 loga. ---- */
extern void *softfp_resolve(const char *);
/* OpenSL ES shim (opensles_shim.c) p/ o FMOD: ele dlopen("libOpenSLES.so") +
 * dlsym(slCreateEngine, SL_IID_*). O device não tem libOpenSLES → FMOD falha →
 * EventSystem::init falha → jogo trava no spam. Roteamos p/ o nosso shim SDL2. */
extern int slCreateEngine_shim();          /* SLresult slCreateEngine_shim(void**,...) */
extern const void *sl_IID_ENGINE, *sl_IID_PLAY, *sl_IID_VOLUME, *sl_IID_BUFFERQUEUE,
                   *sl_IID_EFFECTSEND;
#define NFS_OPENSLES_SENTINEL ((void *)0x05105105)
static void *my_opensles_dlsym(const char *name) {
  if (!name) return NULL;
  if (strcmp(name, "slCreateEngine") == 0) return (void *)slCreateEngine_shim;
  if (strcmp(name, "SL_IID_ENGINE") == 0) return (void *)&sl_IID_ENGINE;
  if (strcmp(name, "SL_IID_PLAY") == 0) return (void *)&sl_IID_PLAY;
  if (strcmp(name, "SL_IID_VOLUME") == 0) return (void *)&sl_IID_VOLUME;
  if (strcmp(name, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE") == 0 || strcmp(name, "SL_IID_BUFFERQUEUE") == 0)
    return (void *)&sl_IID_BUFFERQUEUE;
  /* ANDROIDCONFIGURATION/RECORD/etc: IID dummy (GetInterface do shim aceita qualquer) */
  if (strncmp(name, "SL_IID_", 7) == 0) return (void *)&sl_IID_EFFECTSEND;
  return NULL;
}
static void *(*real_dlsym)(void *, const char *);
static void *my_dlsym(void *handle, const char *name) {
  if (!real_dlsym) real_dlsym = (void *(*)(void *, const char *))dlsym(RTLD_DEFAULT, "dlsym");
  if (handle == NFS_OPENSLES_SENTINEL) { void *s = my_opensles_dlsym(name); if (s) return s; }
  void *p = softfp_resolve(name);
  if (!p) p = real_dlsym(handle, name);
  if (getenv("NFS_DLSYMLOG")) {
    static int n = 0;
    if (n < 80) { fprintf(stderr, "[dlsym] '%s' -> %p%s\n", name ? name : "?", p, softfp_resolve(name) ? " (softfp)" : ""); n++; }
  }
  return p;
}

/* ---- opendir/closedir: rastrear o leak de fds (engine reabre files/ em loop) ---- */
static void *(*real_opendir)(const char *);
static int (*real_closedir)(void *);
static int g_opendir_n, g_closedir_n;
static int g_dir_depth_max;
static void *my_opendir(const char *path) {
  if (!real_opendir) real_opendir = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "opendir");
  int depth = g_opendir_n - g_closedir_n; /* fds atualmente abertos = profundidade da recursão */
  /* NFS_DIRCAP=N: quando a recursão passa de N níveis abertos, falha o opendir
   * (retorna NULL) p/ QUEBRAR a recursão infinita na raiz. Diagnóstico+workaround:
   * se a engine prossegue, o walk era não-essencial / bug de self-recursão. */
  static int cap = -2;
  if (cap == -2) { const char *c = getenv("NFS_DIRCAP"); cap = c ? atoi(c) : -1; }
  if (cap > 0 && depth >= cap) {
    static int capped;
    if (capped < 8) {
      fprintf(stderr, "[opendir CAP] depth=%d >= %d, falhando '%s' caller=%p\n",
              depth, cap, path ? path : "?", __builtin_return_address(0));
      capped++;
    }
    errno = EMFILE;
    return NULL;
  }
  void *d = real_opendir(path);
  g_opendir_n++;
  if (depth + 1 > g_dir_depth_max) g_dir_depth_max = depth + 1;
  if (getenv("NFS_DIRLOG")) {
    /* loga as 40 primeiras + toda vez que a profundidade bate novo recorde
     * (captura o crescimento da recursão sem storm de log) */
    if (g_opendir_n < 40 || (depth + 1) == g_dir_depth_max)
      fprintf(stderr, "[opendir #%d depth=%d] '%s' -> %p caller=%p\n", g_opendir_n,
              depth + 1, path ? path : "?", d, __builtin_return_address(0));
  }
  return d;
}
static int my_closedir(void *d) {
  if (!real_closedir) real_closedir = (int (*)(void *))dlsym(RTLD_DEFAULT, "closedir");
  g_closedir_n++;
  return real_closedir(d);
}

/* ---- readdir → readdir64 (FIX do busy-loop / fd-leak da PARTE 7) ----
 * A engine (bionic) lê dirent->d_name no OFFSET 19 (visto no disassembly de
 * enumerate@0x582c24: `ldrb r1,[r9,#19]!`). Layout bionic 32-bit:
 *   d_ino(8) d_off(8) d_reclen(2) d_type(1) d_name@19.
 * Mas o `readdir` da glibc (sem _FILE_OFFSET_BITS=64) usa `struct dirent` com
 * ino/off de 4 bytes → d_name@11. A engine lendo @19 pega 8 bytes adiante →
 * nomes ≤8 chars (data/fonts/flow/sounds…) viram VAZIO → o walk recursivo
 * desce com basename "" → child==parent → re-scan infinito da raiz "files"
 * (1011 fds abertos = profundidade da recursão). O `struct dirent64` da glibc
 * tem EXATAMENTE o layout bionic (ino/off 8B, reclen@16, type@18, d_name@19),
 * então basta rotear readdir→readdir64. Mesma classe do fix stat→stat64. */
static void *(*real_readdir64)(void *);
static void *my_readdir(void *dirp) {
  if (!real_readdir64) real_readdir64 = (void *(*)(void *))dlsym(RTLD_DEFAULT, "readdir64");
  return real_readdir64(dirp);
}
/* idem p/ readdir_r (a engine tb o importa): readdir64_r preenche dirent64
 * (d_name@19) no buffer do chamador, que é dimensionado p/ dirent bionic. */
static int (*real_readdir64_r)(void *, void *, void **);
static int my_readdir_r(void *dirp, void *entry, void **result) {
  if (!real_readdir64_r)
    real_readdir64_r = (int (*)(void *, void *, void **))dlsym(RTLD_DEFAULT, "readdir64_r");
  return real_readdir64_r(dirp, entry, result);
}

/* stub do FMOD EventSystem::init → FMOD_OK (0). Faz o EventSystem ficar não-nulo
 * no SoundManager → mata o spam "EventSystem is null" + destrava o jogo (sem som). */
static int fmod_es_init_stub(void *self, int maxchannels, unsigned int flags,
                             void *extradriverdata, unsigned int eventflags) {
  (void)self; (void)maxchannels; (void)flags; (void)extradriverdata; (void)eventflags;
  static int once = 0;
  if (!once) { fprintf(stderr, "[FMOD] EventSystem::init STUBADO -> FMOD_OK (sem som)\n"); once = 1; }
  return 0; /* FMOD_OK */
}

/* ---- dlopen: loga o que a engine tenta carregar (rastrear o módulo anon 12K) ---- */
static void *(*real_dlopen)(const char *, int);
static void *my_dlopen(const char *path, int flag) {
  if (!real_dlopen) real_dlopen = (void *(*)(const char *, int))dlsym(RTLD_DEFAULT, "dlopen");
  /* FMOD dlopen("libOpenSLES.so"): rotear p/ o opensles_shim quebrava o boot
   * (FMOD usava a sentinela e crashava ANTES da janela). NFS_OPENSLES=1 reativa
   * p/ experimentar; default = deixa falhar (FMOD trata e segue, render OK). */
  if (path && strstr(path, "libOpenSLES")) {
    static int en = -1; if (en < 0) en = getenv("NFS_OPENSLES") ? 1 : 0;
    if (en) { fprintf(stderr, "[dlopen] '%s' -> OpenSLES SHIM sentinel\n", path);
      return NFS_OPENSLES_SENTINEL; }
  }
  void *h = real_dlopen(path, flag);
  fprintf(stderr, "[dlopen] '%s' flag=0x%x -> %p\n", path ? path : "(NULL)", flag, h);
  return h;
}

/* ---- mmap: loga mapeamentos EXECUTÁVEIS (engine faz codegen/carrega .so da memória?) ---- */
#include <sys/mman.h>
static void *my_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
  void *p = mmap(addr, len, prot, flags, fd, off);
  if (prot & PROT_EXEC) {
    static int n = 0;
    if (n < 40) { fprintf(stderr, "[mmap-EXEC] addr=%p len=%zu(%zuK) prot=0x%x flags=0x%x fd=%d off=0x%lx -> %p\n",
                          addr, len, len / 1024, prot, flags, fd, off, p); n++; }
  }
  return p;
}

static int my_mprotect(void *addr, size_t len, int prot) {
  int r = mprotect(addr, len, prot);
  if (prot & PROT_EXEC) {
    static int n = 0;
    if (n < 40) { fprintf(stderr, "[mprotect-EXEC] addr=%p len=%zu(%zuK) prot=0x%x -> %d\n",
                          addr, len, len / 1024, prot, r); n++; }
  }
  return r;
}

/* ---- __dynamic_cast SEGURO ----
 * A engine, no parse de asset, chama dynamic_cast em objetos cujo ponteiro de
 * vtable está corrompido/deslocado (vtable[-1]=typeinfo aponta p/ CÓDIGO em vez
 * de um __class_type_info). O __dynamic_cast da libc++ desreferencia esse
 * typeinfo lixo → blx num ponteiro inválido → SIGSEGV. Validamos a cadeia
 * sub→vtable→typeinfo→typeinfo_vtable[6] ANTES de delegar; se algo não for
 * legível, devolvemos NULL (= "cast falhou", resultado C++ válido). */
void *g_real_dynamic_cast;  /* setado em main.c c/ o __dynamic_cast da libc++ */

/* probe de legibilidade à prova de fault: arma g_probe_jmp, lê o 1º e o último
 * byte de [p,p+n); se faultar, o crash_handler dá siglongjmp de volta → 0. */
static int mem_readable(const void *p, size_t n) {
  if (!p || n == 0) return 0;
  if ((uintptr_t)p < 0x1000) return 0;
  volatile char sink;
  if (sigsetjmp(g_probe_jmp, 1) != 0) { g_probe_armed = 0; return 0; }
  g_probe_armed = 1;
  sink = ((const volatile char *)p)[0];
  sink = ((const volatile char *)p)[n - 1];
  (void)sink;
  g_probe_armed = 0;
  return 1;
}
/* faixas executáveis do processo (cache do /proc/self/maps) p/ validar que um
 * ponteiro de função é código real e não dados de asset (ex: ASCII "HT=1"). */
static struct { uintptr_t s, e; } g_xr[64];
static int g_xr_n = -1;
static void load_exec_ranges(void) {
  g_xr_n = 0;
  FILE *mf = fopen("/proc/self/maps", "r");
  if (!mf) return;
  char ln[512];
  while (fgets(ln, sizeof ln, mf) && g_xr_n < 64) {
    unsigned long s, e; char perm[8];
    if (sscanf(ln, "%lx-%lx %7s", &s, &e, perm) == 3 && perm[2] == 'x') {
      g_xr[g_xr_n].s = s; g_xr[g_xr_n].e = e; g_xr_n++;
    }
  }
  fclose(mf);
}
static int ptr_executable(uintptr_t p) {
  if (g_xr_n < 0) load_exec_ranges();
  for (int i = 0; i < g_xr_n; i++) if (p >= g_xr[i].s && p < g_xr[i].e) return 1;
  /* miss: regiões podem ter mudado (mmap novo) → recarrega 1x e retenta */
  load_exec_ranges();
  for (int i = 0; i < g_xr_n; i++) if (p >= g_xr[i].s && p < g_xr[i].e) return 1;
  return 0;
}

uintptr_t g_dyncast_tramp;  /* trampolim p/ rodar o __dynamic_cast original */
static void *my_dynamic_cast(const void *, const void *, const void *, long);

/* caminha a cadeia de type_info (Itanium ABI) p/ achar a base CORROMPIDA — a que
 * tem a vtable do type_info fora de qq região executável. Heurística __si vs __vmi:
 *   __class: vt,name              __si: vt,name,base@8
 *   __vmi:   vt,name,flags@8,count@12,base_info[]@16 (cada 8B: base@0, offflags@4)
 * Retorna 1 se achou corrupção. NFS_TICHAIN=1 liga o dump. */
static const char *ti_name(const void *ti) {
  if (!mem_readable((const char *)ti + 4, 4)) return "?";
  const char *n = *(const char *const *)((const char *)ti + 4);
  return mem_readable(n, 1) ? n : "?";
}
/* type_info válido: tem vtable (em .data.rel.ro, pode NÃO ser exec) cujo handler
 * vt[6] (offset 0x18, o que o dcast faz blx) É código executável. */
static int ti_ok(const void *ti) {
  if (!mem_readable(ti, 12)) return 0;
  uintptr_t vt = *(const uintptr_t *)ti;
  if (!mem_readable((const void *)(vt + 0x18), 4)) return 0;
  uintptr_t handler = *(const uintptr_t *)(vt + 0x18);
  return ptr_executable(handler & ~1u);
}
static int ti_walk(const void *ti, int depth, int dump) {
  if (depth > 10 || !mem_readable(ti, 16)) {
    if (dump) fprintf(stderr, "    %*sti=%p UNREADABLE\n", depth * 2, "", ti);
    return 1;
  }
  uintptr_t vt = *(const uintptr_t *)ti;
  int ok = ti_ok(ti);
  uintptr_t handler = (mem_readable((const void *)(vt + 0x18), 4)) ? *(const uintptr_t *)(vt + 0x18) : 0;
  if (dump)
    fprintf(stderr, "    %*sti=%p vt=%p handler=%p ok=%d name=%.48s\n", depth * 2, "",
            ti, (void *)vt, (void *)handler, ok, ok ? ti_name(ti) : "<<CORROMPIDO>>");
  if (!ok) return 1;                       /* handler do type_info é lixo */
  /* tenta __si: base@8 que pareça type_info válido */
  uintptr_t b8 = *(const uintptr_t *)((const char *)ti + 8);
  if (mem_readable((const void *)b8, 12) && ti_ok((const void *)b8))
    return ti_walk((const void *)b8, depth + 1, dump);
  /* tenta __vmi: count@12, bases@16 (cada base_info: type@0, offflags@4) */
  if (mem_readable((const char *)ti + 16, 4)) {
    unsigned cnt = *(const unsigned *)((const char *)ti + 12);
    if (cnt > 0 && cnt < 32) {
      int bad = 0;
      for (unsigned i = 0; i < cnt; i++) {
        uintptr_t bi = *(const uintptr_t *)((const char *)ti + 16 + i * 8);
        bad |= ti_walk((const void *)bi, depth + 1, dump);
      }
      return bad;
    }
  }
  return 0;
}

/* instala inline-hook na ENTRADA do __dynamic_cast da libc++ p/ que TODA chamada
 * (inclusive a recursão interna da libc++, que não passa pela GOT da libapp)
 * valide o typeinfo antes. Constrói trampolim com os 8 bytes originais. */
/* ---- hook de SKU::GetFileSystemPath (libapp+0x3fba3c, ARM) ----
 * Loga o path de ENTRADA (r2 = {char* begin, char* end}) e SAÍDA (r0 = std::string
 * libc++) p/ ver a chave que a engine procura no OBB. NFS_FSPATHLOG=1. */
static uintptr_t g_fsp_tramp;
static void log_range(const char *tag, const char *b, const char *e) {
  if (!b || !e || e < b || (e - b) > 300) { fprintf(stderr, "  %s=<inval %p..%p>\n", tag, b, e); return; }
  fprintf(stderr, "  %s(%ld)=\"", tag, (long)(e - b));
  for (const char *p = b; p < e; p++) fputc((*p >= 32 && *p < 127) ? *p : '.', stderr);
  fprintf(stderr, "\"\n");
}
static void log_path_arg(const char *tag, void *r1) {
  /* r1 pode ser: char* (null-term), ou ptr p/ {char* begin, char* end}, ou std::string */
  if (!r1 || (uintptr_t)r1 < 0x10000) { fprintf(stderr, "  %s=<%p>\n", tag, r1); return; }
  char **w = (char **)r1;
  /* tenta {begin,end} */
  if ((uintptr_t)w[0] > 0x10000 && (uintptr_t)w[1] > (uintptr_t)w[0] && (uintptr_t)w[1] - (uintptr_t)w[0] < 300)
    { log_range(tag, w[0], w[1]); return; }
  /* tenta char* direto */
  const char *c = (const char *)r1; int ok = 1;
  for (int i = 0; i < 200; i++) { if (c[i] == 0) break; if (c[i] < 9 || (unsigned char)c[i] > 126) { ok = 0; break; } }
  if (ok && c[0] >= 32) { fprintf(stderr, "  %s(cstr)=\"%.200s\"\n", tag, c); return; }
  fprintf(stderr, "  %s=<raw %08x %08x %08x>\n", tag, ((unsigned*)r1)[0], ((unsigned*)r1)[1], ((unsigned*)r1)[2]);
}
/* hook do mount-lookup 0x410230(r0=context, r1=key) -> mount ptr ou NULL */
static uintptr_t g_mntlk_tramp;
static void *my_mntlookup(void *ctx, void *key, void *r2, void *r3) {
  void *(*real)(void *, void *, void *, void *) = (void *(*)(void *, void *, void *, void *))g_mntlk_tramp;
  void *ret = real(ctx, key, r2, r3);
  static int n = 0;
  if (n < 60) { fprintf(stderr, "[mntlookup #%d] ctx=%p ret=%p\n", n, ctx, ret); log_path_arg("key", key);
    /* se mount achado, computa o método FS open (vtable[6] de mount[0x24][8]) como offset */
    if (ret && (uintptr_t)ret > 0x10000) { extern void *text_base; uintptr_t tb = (uintptr_t)text_base;
      uintptr_t r10 = *(uintptr_t *)((char *)ret + 0x24);
      fprintf(stderr, "  mount[0x24]=%p", (void *)r10);
      if (r10 > 0x10000) { uintptr_t fs = *(uintptr_t *)((char *)r10 + 8);
        fprintf(stderr, " fsobj=%p", (void *)fs);
        if (fs > 0x10000) { uintptr_t vt = *(uintptr_t *)fs; uintptr_t open6 = *(uintptr_t *)(vt + 0x18);
          fprintf(stderr, " vt=%p open6=%p (libapp+0x%lx)", (void *)vt, (void *)open6,
                  (open6 > tb && open6 < tb + 0xa00000) ? (unsigned long)(open6 - tb) : 0); } }
      fprintf(stderr, "\n"); }
    n++; }
  return ret;
}

static void *my_getfspath(void *out, void *path_r1, void *r2, void *r3) {
  static int n = 0;
  int log = (n < 80);
  /* 🔧 EXPERIMENTO: o OBB tem entradas "published/..." mas a engine busca
   * "/published/..." (barra inicial). Remove a '/' inicial passando {begin+1,end}.
   * NFS_STRIPSLASH=1. */
  char *newrange[2];
  if (getenv("NFS_STRIPSLASH") && path_r1 && (uintptr_t)path_r1 > 0x10000) {
    char **w = (char **)path_r1;
    if ((uintptr_t)w[0] > 0x10000 && (uintptr_t)w[1] > (uintptr_t)w[0] && w[0][0] == '/') {
      newrange[0] = w[0] + 1; newrange[1] = w[1];
      path_r1 = newrange;
    }
  }
  if (log) { fprintf(stderr, "[dbopen #%d] this=%p r2=%p\n", n, out, r2); log_path_arg("path", path_r1); n++; }
  /* 🔎 one-shot: pega o singleton VFS (getter 0x40e8e8) e loga vtable[2]/[6] como
   * offsets de arquivo (- text_base) p/ desmontar os métodos open. */
  { static int once = 0; extern void *text_base;
    if (!once) { once = 1;
      void *(*getter)(void) = (void *(*)(void))((uintptr_t)text_base + 0x40e8e8);
      void *sing = getter();
      fprintf(stderr, "[VFS] singleton=%p\n", sing);
      if (sing && (uintptr_t)sing > 0x10000) {
        uintptr_t *vt = *(uintptr_t **)sing;
        uintptr_t tb = (uintptr_t)text_base;
        for (int i = 0; i < 12; i++) {
          uintptr_t m = vt[i];
          fprintf(stderr, "  vtable[%d]=%p (libapp+0x%lx)\n", i, (void *)m,
                  (m > tb && m < tb + 0xa00000) ? (unsigned long)(m - tb) : 0);
        }
      }
    }
  }
  void *(*real)(void *, void *, void *, void *) =
      (void *(*)(void *, void *, void *, void *))g_fsp_tramp;
  return real(out, path_r1, r2, r3);
}
/* hook do FS open (0x582ac0, disk fs): loga *param_3 = path de DISCO (stat/open) */
static uintptr_t g_fsopen_tramp;
static void *my_fsopen(void *out, void *p2, void **p3, void *r3) {
  static int n = 0;
  if (n < 80 && p3 && (uintptr_t)p3 > 0x10000) {
    char *path = (char *)*p3;
    fprintf(stderr, "[fsopen #%d] disk-path=\"%.160s\"\n", n, (path && (uintptr_t)path > 0x10000) ? path : "?"); n++;
  }
  void *(*real)(void *, void *, void **, void *) = (void *(*)(void *, void *, void **, void *))g_fsopen_tramp;
  return real(out, p2, p3, r3);
}

void nfs_install_getfspath_hook(void) {
  extern void hook_arm(uintptr_t, uintptr_t);
  extern void *text_base;
  uintptr_t fn = (uintptr_t)text_base + 0x4f0138;  /* database-open (ARM); r1=path */
  uint8_t *tr = mmap(NULL, 64, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr == MAP_FAILED) return;
  memcpy(tr, (void *)fn, 8);                       /* push{..}; add fp,sp,#0x1c (ARM, PIC-safe) */
  *(uint32_t *)(tr + 8) = 0xe51ff004u;             /* ldr pc,[pc,#-4] */
  *(uint32_t *)(tr + 12) = (uint32_t)(fn + 8);     /* continua em +8 */
  __builtin___clear_cache((char *)tr, (char *)tr + 16);
  g_fsp_tramp = (uintptr_t)tr;
  long pg = sysconf(_SC_PAGESIZE);
  uintptr_t pbase = fn & ~((uintptr_t)pg - 1);
  mprotect((void *)pbase, pg * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
  hook_arm(fn, (uintptr_t)my_getfspath);           /* ARM (addr par) */
  __builtin___clear_cache((char *)fn, (char *)fn + 8);
  mprotect((void *)pbase, pg * 2, PROT_READ | PROT_EXEC);
  fprintf(stderr, "[GetFSPath-hook] @%p hooked (tramp=%p)\n", (void *)fn, (void *)g_fsp_tramp);

  /* hook 2: mount-lookup 0x410230 */
  uintptr_t fn2 = (uintptr_t)text_base + 0x410230;
  uint8_t *tr2 = mmap(NULL, 64, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr2 != MAP_FAILED) {
    memcpy(tr2, (void *)fn2, 8);
    *(uint32_t *)(tr2 + 8) = 0xe51ff004u;
    *(uint32_t *)(tr2 + 12) = (uint32_t)(fn2 + 8);
    __builtin___clear_cache((char *)tr2, (char *)tr2 + 16);
    g_mntlk_tramp = (uintptr_t)tr2;
    uintptr_t pb2 = fn2 & ~((uintptr_t)pg - 1);
    mprotect((void *)pb2, pg * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
    hook_arm(fn2, (uintptr_t)my_mntlookup);
    __builtin___clear_cache((char *)fn2, (char *)fn2 + 8);
    mprotect((void *)pb2, pg * 2, PROT_READ | PROT_EXEC);
    fprintf(stderr, "[mntlookup-hook] @%p hooked\n", (void *)fn2);
  }

  /* hook 3: FS open (disk) 0x582ac0 */
  uintptr_t fn3 = (uintptr_t)text_base + 0x582ac0;
  uint8_t *tr3 = mmap(NULL, 64, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr3 != MAP_FAILED) {
    memcpy(tr3, (void *)fn3, 8);
    *(uint32_t *)(tr3 + 8) = 0xe51ff004u;
    *(uint32_t *)(tr3 + 12) = (uint32_t)(fn3 + 8);
    __builtin___clear_cache((char *)tr3, (char *)tr3 + 16);
    g_fsopen_tramp = (uintptr_t)tr3;
    uintptr_t pb3 = fn3 & ~((uintptr_t)pg - 1);
    mprotect((void *)pb3, pg * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
    hook_arm(fn3, (uintptr_t)my_fsopen);
    __builtin___clear_cache((char *)fn3, (char *)fn3 + 8);
    mprotect((void *)pb3, pg * 2, PROT_READ | PROT_EXEC);
    fprintf(stderr, "[fsopen-hook] @%p hooked\n", (void *)fn3);
  }
}

void nfs_install_dyncast_hook(void) {
  extern void hook_arm(uintptr_t, uintptr_t);
  if (!g_real_dynamic_cast) return;
  uintptr_t even = ((uintptr_t)g_real_dynamic_cast) & ~1u;
  uint8_t *tr = mmap(NULL, 64, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr == MAP_FAILED) return;
  memcpy(tr, (void *)even, 8);                  /* push{..};add r7;str.w r8 (PIC) */
  tr[8] = 0xDF; tr[9] = 0xF8; tr[10] = 0x00; tr[11] = 0xF0;  /* ldr.w pc,[pc,#0] */
  *(uint32_t *)(tr + 12) = (uint32_t)(even + 8) | 1u;        /* continua em +8 (Thumb) */
  __builtin___clear_cache((char *)tr, (char *)tr + 16);
  g_dyncast_tramp = (uintptr_t)tr | 1u;
  long pg = sysconf(_SC_PAGESIZE);
  uintptr_t pbase = even & ~((uintptr_t)pg - 1);
  mprotect((void *)pbase, pg * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
  hook_arm(even | 1u, (uintptr_t)my_dynamic_cast);          /* entrada → nosso shim */
  __builtin___clear_cache((char *)even, (char *)even + 8);
  mprotect((void *)pbase, pg * 2, PROT_READ | PROT_EXEC);
  fprintf(stderr, "[dcast-hook] __dynamic_cast @%p hooked (tramp=%p)\n",
          (void *)even, (void *)g_dyncast_tramp);
}

/* última cadeia de dcast vista — o crash_handler imprime p/ identificar o objeto
 * selvagem (cujo handler interno deu blx em lixo). */
const void *g_last_dcast_sub, *g_last_dcast_vt, *g_last_dcast_ti, *g_last_dcast_caller;
/* ring dos últimos N dcast (sub, vtable, dst, caller) p/ o crash_handler imprimir */
#define DCRING 12
struct dcrec { const void *sub, *vt, *dst, *caller; };
struct dcrec g_dcring[DCRING];
int g_dcring_i;

/* ---- WALKER de dynamic_cast PRÓPRIO ----
 * 🔑 O __dynamic_cast da libc++ bundled crasha caminhando a hierarquia de
 * type_info sob nosso ambiente (re-entra p/ classificar type_infos e lê um
 * ponteiro deslocado → blx em lixo). As ESTRUTURAS estáticas de type_info do
 * libapp estão CORRETAS (verificado: vtable→libc++, name, base_type bem
 * relocados). Então fazemos o walk nós mesmos, comparando type_infos por nome
 * (cross-módulo seguro), evitando o walk interno. Cobre __class (sem base),
 * __si (herança simples) e __vmi (múltipla/virtual). */
uintptr_t g_ti_vt_class, g_ti_vt_si, g_ti_vt_vmi;  /* setados em main.c (vtable+8) */

/* layout Itanium: ti[0]=vtable(+8), ti[1]=name(char*).
 * __si: ti[2]=__base_type. __vmi: ti[2]=flags, ti[3]=base_count, ti[4..]=__base_class_info[]
 * (__base_class_info = {base_type, offset_flags}; offset_flags>>8 = offset, bit0=virtual). */
static int dc_same_type(const void *a, const void *b) {
  if (a == b) return 1;
  if (!a || !b || !mem_readable(a, 8) || !mem_readable(b, 8)) return 0;
  const char *na = ((const char *const *)a)[1], *nb = ((const char *const *)b)[1];
  if (na == nb) return 1;
  if (!mem_readable(na, 1) || !mem_readable(nb, 1)) return 0;
  return strcmp(na, nb) == 0;  /* nomes mangled: cross-módulo seguro */
}
/* procura dst na hierarquia de base de 'dyn' a partir do objeto em obj_ptr.
 * retorna o ponteiro ajustado p/ o subobjeto dst, ou NULL. depth p/ não loopar. */
static const void *dc_walk(const void *obj_ptr, const void *dyn, const void *dst, int depth) {
  if (depth > 32 || !dyn || !mem_readable(dyn, 12)) return 0;
  if (dc_same_type(dyn, dst)) return obj_ptr;
  uintptr_t vt = ((const uintptr_t *)dyn)[0];
  if (vt == g_ti_vt_si) {                       /* herança simples: base em offset 0 */
    const void *base = ((const void *const *)dyn)[2];
    return dc_walk(obj_ptr, base, dst, depth + 1);
  }
  if (vt == g_ti_vt_vmi) {                       /* herança múltipla/virtual */
    if (!mem_readable(dyn, 16)) return 0;
    unsigned base_count = ((const unsigned *)dyn)[3];
    if (base_count > 64) return 0;
    const uintptr_t *bi = (const uintptr_t *)dyn + 4; /* base_class_info[] */
    for (unsigned i = 0; i < base_count; i++) {
      if (!mem_readable(bi + 2 * i, 8)) break;
      const void *base = (const void *)bi[2 * i];
      long offflags = (long)bi[2 * i + 1];
      const char *adj;
      if (offflags & 1) {                        /* base VIRTUAL: offset via vtable */
        if (!mem_readable(obj_ptr, 4)) continue;
        const char *vtbl = *(const char *const *)obj_ptr;
        long voff = (offflags >> 8);
        if (!mem_readable(vtbl + voff, 4)) continue;
        adj = (const char *)obj_ptr + *(const long *)(vtbl + voff);
      } else {
        adj = (const char *)obj_ptr + (offflags >> 8);
      }
      const void *r = dc_walk(adj, base, dst, depth + 1);
      if (r) return r;
    }
  }
  /* __class_type_info (g_ti_vt_class) ou vtable desconhecida: sem bases */
  return 0;
}

static void *my_dynamic_cast(const void *sub, const void *src, const void *dst, long s2d) {
  int dbg = g_nfs_dcastlog;
  static int dn = 0;
  g_last_dcast_sub = sub;
  g_last_dcast_caller = __builtin_return_address(0);
  g_last_dcast_vt = mem_readable(sub, 4) ? *(const void *const *)sub : 0;
  g_last_dcast_ti = (g_last_dcast_vt && mem_readable((const char *)g_last_dcast_vt - 4, 4))
                        ? *(const void *const *)((const char *)g_last_dcast_vt - 4) : 0;
  { struct dcrec *r = &g_dcring[g_dcring_i++ % DCRING];
    r->sub = sub; r->vt = g_last_dcast_vt; r->dst = dst; r->caller = g_last_dcast_caller; }
  /* teste UAF: loga o refcount [sub+4] e validade da vtable dos nós castados.
   * Se um nó tem vtable inválida E refcount 0/lixo = use-after-free. NFS_RCLOG=1. */
  if (g_nfs_rclog && sub) {
    extern void *text_base, *data_base; extern size_t text_size, data_size;
    uintptr_t vt = (uintptr_t)g_last_dcast_vt;
    int in_img = ((vt >= (uintptr_t)text_base && vt < (uintptr_t)text_base + text_size) ||
                  (vt >= (uintptr_t)data_base && vt < (uintptr_t)data_base + data_size));
    static int rn = 0;
    unsigned rc = mem_readable((const char *)sub + 4, 4) ? *(const unsigned *)((const char *)sub + 4) : 0xBAD;
    if (rn < 40) {
      fprintf(stderr, "[RC] #%d sub=%p vt=%p img=%d refcount=%u%s\n", rn, sub, (void *)vt,
              in_img, rc, in_img ? "" : "  <<< VTABLE FORA DA IMAGEM");
      rn++;
    }
  }
  /* detecta o NÓ SELVAGEM: a vtable do objeto (*sub) tem que estar na IMAGEM do
   * libapp (.rodata/.data.rel.ro). Se cair fora, sub aponta p/ heap/asset = nó
   * inválido. Loga o 1º selvagem + contexto. NFS_WILDLOG=1. */
  if (g_nfs_wildlog && sub && g_last_dcast_vt) {
    extern void *text_base, *data_base; extern size_t text_size, data_size;
    uintptr_t vt = (uintptr_t)g_last_dcast_vt;
    int in_img = ((vt >= (uintptr_t)text_base && vt < (uintptr_t)text_base + text_size) ||
                  (vt >= (uintptr_t)data_base && vt < (uintptr_t)data_base + data_size));
    static int wn = 0, total = 0; total++;
    if (!in_img && wn < 8) {
      const unsigned char *b = (const unsigned char *)sub;
      fprintf(stderr, "[WILD-NODE] #%d apos %d casts: sub=%p vt=%p (FORA da imagem libapp) caller=%p\n",
              wn, total, sub, (void *)vt, g_last_dcast_caller);
      fprintf(stderr, "  obj bytes:");
      if (mem_readable(sub, 32)) { for (int q = 0; q < 8; q++) fprintf(stderr, " %08x", ((const uint32_t *)sub)[q]); }
      fprintf(stderr, "\n  obj ascii: ");
      if (mem_readable(sub, 32)) for (int q = 0; q < 32; q++) fprintf(stderr, "%c", (b[q] >= 32 && b[q] < 127) ? b[q] : '.');
      fprintf(stderr, "\n");
      wn++;
    }
  }
  /* dump da cadeia de type_info do objeto p/ achar a base corrompida */
  if (g_nfs_tichain && g_last_dcast_vt) {
    static int tn = 0;
    const void *obj_ti = (mem_readable((const char *)g_last_dcast_vt - 4, 4))
                             ? *(const void *const *)((const char *)g_last_dcast_vt - 4) : 0;
    if (tn < 16 && obj_ti) {
      int bad = ti_walk(obj_ti, 0, 0);     /* 1ª passada silenciosa: só os ruins */
      if (bad) {
        fprintf(stderr, "[ti-chain] sub=%p obj_ti=%p caller=%p CADEIA CORROMPIDA:\n",
                sub, obj_ti, g_last_dcast_caller);
        ti_walk(obj_ti, 0, 1);             /* 2ª passada: dump completo */
        tn++;
      }
    }
  }
  /* ---- 🔑 NOSSO walker: se o type_info dinâmico é de um kind reconhecido
   * (__class/__si/__vmi da libc++), caminhamos a hierarquia nós mesmos e
   * NÃO delegamos ao libcxxabi (cujo walk crasha aqui). Autoritativo. ---- */
  if (g_nfs_dcwalk && g_ti_vt_si && mem_readable(sub, 4)) {
    const uintptr_t *vtbl = *(const uintptr_t *const *)sub;
    if (vtbl && mem_readable(vtbl - 2, 8)) {
      const void *dyn_ti = (const void *)vtbl[-1];
      long off2top = (long)vtbl[-2];
      if (mem_readable(dyn_ti, 8)) {
        uintptr_t kind = ((const uintptr_t *)dyn_ti)[0];
        if (kind == g_ti_vt_class || kind == g_ti_vt_si || kind == g_ti_vt_vmi) {
          const void *r = dc_walk((const char *)sub + off2top, dyn_ti, dst, 0);
          if (dbg && dn < 20) { fprintf(stderr, "[dcast-walk] sub=%p dyn_ti=%p dst=%p -> %p\n", sub, dyn_ti, dst, r); dn++; }
          return (void *)r;
        }
      }
    }
  }
  /* fallback p/ type_info de kind desconhecido (vtables não capturadas): valida
   * a cadeia e delega ao libcxxabi real (caminho legado). */
  if (!mem_readable(sub, 4)) goto fail;
  { const char *vt = *(const char *const *)sub;
    if (!mem_readable(vt - 8, 8)) goto fail;
    const void *ti = *(const void *const *)(vt - 4);
    if (!mem_readable(ti, 4)) goto fail;
    const char *ti_vt = *(const char *const *)ti;       /* vtable do próprio typeinfo */
    if (!mem_readable(ti_vt, 28)) goto fail;            /* dynamic_cast lê ti_vt[6] (+24) */
    uintptr_t handler = *(const uintptr_t *)(ti_vt + 0x18);
    if (!ptr_executable(handler & ~1u)) {
      if (dbg && dn < 20) { fprintf(stderr, "[dcast] sub=%p ti=%p handler=%p NÃO-EXEC -> NULL\n", sub, ti, (void *)handler); dn++; }
      goto fail;
    }
  }
  if (g_dyncast_tramp) {
    void *(*f)(const void *, const void *, const void *, long) =
        (void *(*)(const void *, const void *, const void *, long))g_dyncast_tramp;
    return f(sub, src, dst, s2d);
  }
  if (g_real_dynamic_cast) {
    void *(*f)(const void *, const void *, const void *, long) =
        (void *(*)(const void *, const void *, const void *, long))g_real_dynamic_cast;
    return f(sub, src, dst, s2d);
  }
fail:
  /* 🔬 DUMP do nó corrompido: o crash do shadergen é um nó cujo vtable aponta p/
   * lixo. Dump one-shot (4x) do conteúdo p/ identificar a estrutura — dado de
   * asset? chunk liberado (UAF)? type_info no lugar do objeto? */
  { extern void *text_base, *data_base; extern size_t text_size, data_size;
    static int nd = 0;
    if (nd < 4 && mem_readable(sub, 32)) {
      const uint32_t *w = (const uint32_t *)sub;
      const unsigned char *b = (const unsigned char *)sub;
      uintptr_t vt = w[0], tb = (uintptr_t)text_base, db = (uintptr_t)data_base;
      int vt_text = (vt >= tb && vt < tb + text_size);
      int vt_data = (vt >= db && vt < db + data_size);
      fprintf(stderr, "[BADNODE] #%d sub=%p caller=%p(libapp+0x%lx) dst=%p\n", nd, sub,
              g_last_dcast_caller,
              (unsigned long)((uintptr_t)g_last_dcast_caller - tb), dst);
      fprintf(stderr, "  vtable=w[0]=%08lx %s%s refcount=w[1]=%08x\n", (unsigned long)vt,
              vt_text ? "(.text!) " : "", vt_data ? "(.data/rodata) " : (vt_text ? "" : "(HEAP/lixo) "),
              w[1]);
      fprintf(stderr, "  obj[0..7]:");
      for (int q = 0; q < 8; q++) fprintf(stderr, " %08x", w[q]);
      fprintf(stderr, "\n  ascii: ");
      for (int q = 0; q < 32; q++) fprintf(stderr, "%c", (b[q] >= 32 && b[q] < 127) ? b[q] : '.');
      /* se vtable aponta na imagem, o que tem em vtable[-1] (typeinfo) e seu nome? */
      if ((vt_text || vt_data) && mem_readable((void *)(vt - 4), 4)) {
        uintptr_t ti = *(uintptr_t *)(vt - 4);
        fprintf(stderr, "\n  vtable[-1]=ti=%08lx", (unsigned long)ti);
        if (mem_readable((void *)ti, 8)) {
          uintptr_t nm = *(uintptr_t *)(ti + 4);
          if (mem_readable((void *)nm, 4)) fprintf(stderr, " name=\"%.40s\"", (char *)nm);
        }
      }
      fprintf(stderr, "\n");
      nd++;
    }
  }
  if (g_nfs_dcastlog) {
    static int n = 0;
    if (n < 20) { fprintf(stderr, "[dynamic_cast] sub=%p -> NULL (cadeia typeinfo inválida)\n", sub); n++; }
  }
  return 0;
}

/* ---- sigaction/sigprocmask: ABI bionic→glibc ----
 * O struct sigaction do bionic (arm32) tem 16B: handler@0, mask@4 (4B), flags@8,
 * restorer@12. O da glibc tem sa_mask de 128B → flags@132, restorer@136. O ctor
 * 186 (detecção de CPU via probes que dão SIGILL) preenche o struct BIONIC e chama
 * a sigaction da GLIBC → glibc lê flags/restorer LIXO (offset errado) → instala
 * handler com SA_RESTORER+restorer lixo → no retorno do handler de SIGILL salta p/
 * 0xba. Traduzimos o struct. NFS_NOSIGSHIM=1 desliga. */
struct bionic_sigaction { void *handler; unsigned long mask; int flags; void *restorer; };
static int (*real_sigaction)(int, const struct sigaction *, struct sigaction *);
static void mask32_to_set(unsigned long m, sigset_t *s) {
  sigemptyset(s);
  for (int sg = 1; sg <= 32; sg++) if (m & (1u << (sg - 1))) sigaddset(s, sg);
}
static unsigned long set_to_mask32(const sigset_t *s) {
  unsigned long m = 0;
  for (int sg = 1; sg <= 32; sg++) if (sigismember(s, sg)) m |= (1u << (sg - 1));
  return m;
}
static int my_sigaction(int sig, const void *actp, void *oldp) {
  if (getenv("NFS_NOSIGSHIM")) {
    if (!real_sigaction) real_sigaction = (void *)dlsym(RTLD_DEFAULT, "sigaction");
    return real_sigaction(sig, actp, oldp);
  }
  if (!real_sigaction) real_sigaction = (void *)dlsym(RTLD_DEFAULT, "sigaction");
  struct sigaction ga, go;
  const struct bionic_sigaction *ba = actp;
  if (ba) {
    memset(&ga, 0, sizeof ga);
    ga.sa_handler = (void (*)(int))ba->handler;            /* union @0 */
    /* mantém só flags portáveis; DROPA SA_RESTORER(0x04000000)+SA_THIRTYTWO
     * (0x02000000) → glibc fornece o próprio restorer correto p/ ARM. */
    ga.sa_flags = ba->flags & (SA_NOCLDSTOP | SA_NOCLDWAIT | SA_SIGINFO |
                               SA_ONSTACK | SA_RESTART | SA_NODEFER | SA_RESETHAND);
    mask32_to_set(ba->mask, &ga.sa_mask);
  }
  int r = real_sigaction(sig, ba ? &ga : NULL, oldp ? &go : NULL);
  if (oldp) {
    struct bionic_sigaction *bo = oldp;
    bo->handler = (void *)go.sa_handler;
    bo->mask = set_to_mask32(&go.sa_mask);
    bo->flags = go.sa_flags;
    bo->restorer = 0;
  }
  return r;
}
static int (*real_sigprocmask)(int, const sigset_t *, sigset_t *);
static int my_sigprocmask(int how, const void *setp, void *oldp) {
  if (!real_sigprocmask) real_sigprocmask = (void *)dlsym(RTLD_DEFAULT, "sigprocmask");
  if (getenv("NFS_NOSIGSHIM")) return real_sigprocmask(how, setp, oldp);
  sigset_t gs, go;
  if (setp) mask32_to_set(*(const unsigned long *)setp, &gs);
  int r = real_sigprocmask(how, setp ? &gs : NULL, oldp ? &go : NULL);
  if (oldp) *(unsigned long *)oldp = set_to_mask32(&go);
  return r;
}

/* ---- getauxval: o ctor 186 (detecção de CPU) usa AT_HWCAP bit 0x1000 como gate
 * "ler features via getauxval" vs "PROBAR instrução (SIGILL)". O probe SIGILL não
 * sobrevive sob o loader → ctor crasha (por isso era pulado). Forçando bit 0x1000
 * no AT_HWCAP, o ctor lê features do AT_HWCAP2 (reais) e NÃO proba → completa.
 * NFS_NOAUXHACK=1 desliga. */
static unsigned long (*real_getauxval)(unsigned long);
static unsigned long my_getauxval(unsigned long t) {
  if (!real_getauxval) real_getauxval = (unsigned long (*)(unsigned long))dlsym(RTLD_DEFAULT, "getauxval");
  unsigned long v = real_getauxval ? real_getauxval(t) : 0;
  if (t == 16 /*AT_HWCAP*/ && !getenv("NFS_NOAUXHACK")) v |= 0x1000;
  return v;
}

/* ---- stubs ---- */
static int b_dso_handle;                       /* __dso_handle = endereço dummy */
static void *b_cxa_type_match(void *a, void *b, char c) { (void)a; (void)b; (void)c; return (void *)0; }
/* __sigsetjmp: declarado por <setjmp.h> (via so_util.h); bionic sigsetjmp == isso */
/* AndroidBitmap (jnigraphics): a engine usa getBitmap()->Bitmap p/ renderizar
 * texto/fonte (no Android o framework desenha; aqui não há framework). Antes os
 * stubs sinalizavam erro (-1, pixels NULL) esperando fallback, mas a engine NÃO
 * tem fallback gracioso — fazia memcpy dos pixels NULL → SIGSEGV (PARTE 8). Agora
 * retornamos SUCESSO com um buffer REAL zerado: o texto/glifo fica em branco mas
 * a engine prossegue e RENDERIZA. */
#define ABM_W 1024
#define ABM_H 1024
#define ABM_STRIDE (ABM_W * 4)
static unsigned char *abm_buf(void) {
  static unsigned char *buf;
  if (!buf) buf = (unsigned char *)calloc(1, 16 * 1024 * 1024); /* 16MB: folga contra over-read */
  return buf;
}
static int abm_getInfo(void *env, void *bmp, void *info) {
  (void)env; (void)bmp;
  if (info) {
    unsigned int *p = (unsigned int *)info; /* AndroidBitmapInfo: w,h,stride,format,flags */
    p[0] = ABM_W; p[1] = ABM_H; p[2] = ABM_STRIDE;
    p[3] = 1; /* ANDROID_BITMAP_FORMAT_RGBA_8888 */
    p[4] = 0;
  }
  if (getenv("NFS_BMPLOG")) fprintf(stderr, "[abm_getInfo] bmp=%p -> %dx%d stride=%d\n", bmp, ABM_W, ABM_H, ABM_STRIDE);
  return 0; /* ANDROID_BITMAP_RESULT_SUCCESS */
}
static int abm_lock(void *env, void *bmp, void **pix) {
  (void)env; (void)bmp;
  if (pix) *pix = abm_buf();
  if (getenv("NFS_BMPLOG")) fprintf(stderr, "[abm_lock] bmp=%p -> pix=%p\n", bmp, pix ? *pix : 0);
  return 0;
}
static int abm_unlock(void *env, void *bmp) { (void)env; (void)bmp; return 0; }

/* expõe o bitmap p/ o font_shim rasterizar texto (via jni_shim drawString) */
unsigned char *nfs_abm_buf(void) { return abm_buf(); }
int nfs_abm_w(void) { return ABM_W; }
int nfs_abm_h(void) { return ABM_H; }
int nfs_abm_stride(void) { return ABM_STRIDE; }

/* GL trace wrappers (egl_shim.c): contam draws/binds/clears por frame p/ achar
 * EM QUAL FBO a engine desenha. Resolvidos pela TABELA (vencem o dlsym). */
extern void my_glBindFramebuffer(unsigned, unsigned);
extern void my_glDrawArrays(unsigned, int, int);
extern void my_glDrawElements(unsigned, int, unsigned, const void *);
extern void my_glClear(unsigned);
extern void my_glTexImage2D(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
extern void my_glCompressedTexImage2D(unsigned,int,unsigned,int,int,int,int,const void*);
extern void my_glViewport(int,int,int,int);
extern void my_glBindTexture(unsigned,unsigned);
extern void my_glActiveTexture(unsigned);
extern void my_glTexParameteri(unsigned,unsigned,int);
extern void my_glUniform4f(int,float,float,float,float);
extern void my_glUniform1i(int,int);
extern void my_glUniform4fv(int,int,const float*);
extern void my_glShaderSource(unsigned,int,const char*const*,const int*);
extern void my_glTexSubImage2D(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
extern void my_glDeleteTextures(int,const unsigned*);
extern void my_glCompileShader(unsigned);
extern void my_glLinkProgram(unsigned);
extern void my_glVertexAttrib4f(unsigned,float,float,float,float);
extern void my_glUseProgram(unsigned);
extern void my_glEnable(unsigned);
extern void my_glDisable(unsigned);

DynLibFunction nfs_shims[] = {
    {"glBindFramebuffer", (uintptr_t)my_glBindFramebuffer},
    {"glDrawArrays", (uintptr_t)my_glDrawArrays},
    {"glDrawElements", (uintptr_t)my_glDrawElements},
    {"glClear", (uintptr_t)my_glClear},
    {"glTexImage2D", (uintptr_t)my_glTexImage2D},
    {"glCompressedTexImage2D", (uintptr_t)my_glCompressedTexImage2D},
    {"glViewport", (uintptr_t)my_glViewport},
    {"glBindTexture", (uintptr_t)my_glBindTexture},
    {"glActiveTexture", (uintptr_t)my_glActiveTexture},
    {"glTexParameteri", (uintptr_t)my_glTexParameteri},
    {"glUniform4f", (uintptr_t)my_glUniform4f},
    {"glUniform1i", (uintptr_t)my_glUniform1i},
    {"glUniform4fv", (uintptr_t)my_glUniform4fv},
    {"glShaderSource", (uintptr_t)my_glShaderSource},
    {"glTexSubImage2D", (uintptr_t)my_glTexSubImage2D},
    {"glDeleteTextures", (uintptr_t)my_glDeleteTextures},
    {"glCompileShader", (uintptr_t)my_glCompileShader},
    {"glLinkProgram", (uintptr_t)my_glLinkProgram},
    {"glVertexAttrib4f", (uintptr_t)my_glVertexAttrib4f},
    {"glUseProgram", (uintptr_t)my_glUseProgram},
    {"glEnable", (uintptr_t)my_glEnable},
    {"glDisable", (uintptr_t)my_glDisable},
    /* EGL → SDL2 (Mali fbdev): a engine cria contexto/surface via egl_shim */
    {"eglGetDisplay", (uintptr_t)egl_shim_GetDisplay},
    {"eglInitialize", (uintptr_t)egl_shim_Initialize},
    {"eglTerminate", (uintptr_t)egl_shim_Terminate},
    {"eglChooseConfig", (uintptr_t)egl_shim_ChooseConfig},
    {"eglGetConfigAttrib", (uintptr_t)egl_shim_GetConfigAttrib},
    {"eglCreateWindowSurface", (uintptr_t)egl_shim_CreateWindowSurface},
    {"eglCreatePbufferSurface", (uintptr_t)egl_shim_CreatePbufferSurface},
    {"eglDestroySurface", (uintptr_t)egl_shim_DestroySurface},
    {"eglCreateContext", (uintptr_t)egl_shim_CreateContext},
    {"eglDestroyContext", (uintptr_t)egl_shim_DestroyContext},
    {"eglMakeCurrent", (uintptr_t)egl_shim_MakeCurrent},
    {"eglSwapBuffers", (uintptr_t)egl_shim_SwapBuffers},
    {"eglSwapInterval", (uintptr_t)egl_shim_SwapInterval},
    {"eglGetCurrentContext", (uintptr_t)egl_shim_GetCurrentContext},
    {"eglGetCurrentSurface", (uintptr_t)egl_shim_GetCurrentSurface},
    {"eglGetError", (uintptr_t)egl_shim_GetError},
    {"eglQueryString", (uintptr_t)egl_shim_QueryString},
    {"eglQuerySurface", (uintptr_t)egl_shim_QuerySurface},
    {"eglBindAPI", (uintptr_t)egl_shim_BindAPI},
    {"eglSurfaceAttrib", (uintptr_t)egl_shim_SurfaceAttrib},
    {"eglGetProcAddress", (uintptr_t)egl_shim_GetProcAddress},
    {"pthread_create", (uintptr_t)my_pthread_create},
    {"dlsym", (uintptr_t)my_dlsym},
    {"dlopen", (uintptr_t)my_dlopen},
    /* FMOD EventSystem::init falha (sem áudio no device) → EventSystem nulo →
     * SoundManager spama SetVolume/SetMute (syscall direta, não filtrável) +
     * thrasha o device. NFS_FMODSTUB=1 força init=FMOD_OK p/ o EventSystem ficar
     * não-nulo (sem som, mas para o spam e destrava o jogo). */
    {"_ZN4FMOD11EventSystem4initEijPvj", (uintptr_t)fmod_es_init_stub},
    {"opendir", (uintptr_t)my_opendir},
    {"closedir", (uintptr_t)my_closedir},
    {"readdir", (uintptr_t)my_readdir},
    {"readdir_r", (uintptr_t)my_readdir_r},
    {"fopen", (uintptr_t)my_fopen},
    {"fread", (uintptr_t)my_fread},
    {"open", (uintptr_t)my_open},
    {"stat", (uintptr_t)b_stat},
    {"fstat", (uintptr_t)b_fstat},
    {"statfs", (uintptr_t)b_statfs},
    {"fseek", (uintptr_t)my_fseek},
    {"ftell", (uintptr_t)my_ftell},
    {"lseek", (uintptr_t)my_lseek},
    {"lseek64", (uintptr_t)my_lseek64},
    {"malloc", (uintptr_t)pad_malloc},
    {"calloc", (uintptr_t)pad_calloc},
    {"realloc", (uintptr_t)pad_realloc},
    {"memalign", (uintptr_t)pad_memalign},
    {"__sF", (uintptr_t)bionic_sF},
    {"fprintf", (uintptr_t)w_fprintf}, {"vfprintf", (uintptr_t)w_vfprintf},
    {"fwrite", (uintptr_t)w_fwrite}, {"fputs", (uintptr_t)w_fputs},
    {"write", (uintptr_t)my_write}, {"puts", (uintptr_t)my_puts}, {"printf", (uintptr_t)my_printf},
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
    {"sigsetjmp", (uintptr_t)__sigsetjmp},
    {"__dynamic_cast", (uintptr_t)my_dynamic_cast},
    {"getauxval", (uintptr_t)my_getauxval},
    {"sigaction", (uintptr_t)my_sigaction},
    {"sigprocmask", (uintptr_t)my_sigprocmask},
    {"AndroidBitmap_getInfo", (uintptr_t)abm_getInfo},
    {"AndroidBitmap_lockPixels", (uintptr_t)abm_lock},
    {"AndroidBitmap_unlockPixels", (uintptr_t)abm_unlock},
};
int nfs_shims_count = sizeof(nfs_shims) / sizeof(nfs_shims[0]);
