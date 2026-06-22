// imports.gen.c — GERADO por new-port.sh para 'nier' (libUE4.so)
// 704 simbolos. Resolva os UNKNOWN no fim do arquivo.
#define _GNU_SOURCE
#include "imports.h"
#include "so_util.h"
#include "egl_shim.h"
#include "opensles_shim.h"
#include "android_shim.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <wchar.h>
#include <setjmp.h>
#include <fcntl.h>
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <locale.h>
#include <termios.h>
#include <zlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <sys/sysinfo.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <pthread.h>
#include <GLES2/gl2.h>
#include <stdarg.h>

// no-op stub p/ Oculus VR API (ovrp_*): nao fazemos VR -> retorna 0.
long nier_ret0(void) { return 0; }

// FAndroidWindow::GetHardwareWindow_EventThread: a engine espera uma janela nativa NAO-NULL
// p/ inicializar o RHI (criar EGL surface). x20 do evento e' null -> SetHardwareWindow(null) ->
// GetHardwareWindow()==null -> RHI trava esperando. Forçamos um ponteiro fake nao-null (o
// egl_shim ignora a janela e usa SDL2; nossos ANW stubs ignoram o ptr).
static unsigned char g_fake_hwwin[256];
void *nier_GetHardwareWindow(void) { return g_fake_hwwin; }

// FAndroidPlatformInput::GetKeyMap retorna contagem de teclas (lixo no nosso env) -> o loop da
// AndroidMain estoura o array e corrompe um TSet. Retorna 0 (sem keymap; loop pulado via cbz). */
int nier_GetKeyMap(void *keys, void *names, unsigned max) { (void)keys;(void)names;(void)max; return 0; }

// ANativeWindow: o EGL real e' bypassado (egl_shim usa SDL2), mas a engine LE estas p/
// bookkeeping/janela. acquire->devolve a janela (WindowIn nao-null), get*->dims sanas.
void *nier_ANW_acquire(void *w) { return w; }
void nier_ANW_release(void *w) { (void)w; }
int  nier_ANW_getWidth(void *w) { (void)w; return 1280; }
int  nier_ANW_getHeight(void *w) { (void)w; return 720; }
int  nier_ANW_setBuffersGeometry(void *w, int wd, int h, int fmt) { (void)w;(void)wd;(void)h;(void)fmt; return 0; }

// liblog (Android) -> stderr. Nao existe no glibc.
int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[UE4/%s] ", tag ? tag : "?");
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  return 0;
}
int __android_log_write(int prio, const char *tag, const char *text) {
  (void)prio;
  fprintf(stderr, "[UE4/%s] %s\n", tag ? tag : "?", text ? text : "");
  return 0;
}

// ---- diagnostico: hook mmap p/ ver o que FMallocBinned2/BinnedAllocFromOS pede ----
#include <sys/mman.h>
static int g_mmap_calls = 0;
void *nier_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
  void *r = mmap(addr, len, prot, flags, fd, off);
  if (g_mmap_calls < 80) {
    fprintf(stderr, "[mmap #%d] addr=%p len=%zu (%.1f MB) prot=%d flags=%d fd=%d -> %p%s\n",
            g_mmap_calls, addr, len, len/1048576.0, prot, flags, fd, r,
            r == MAP_FAILED ? " FAILED" : "");
    fflush(stderr);
  }
  g_mmap_calls++;
  return r;
}

// ---- munmap tolerante: FMallocBinned2::BinnedAllocFromOS (AndroidPlatformMemory.cpp:378)
// faz mmap(size+align) e depois munmap da CAUDA p/ alinhar. No nosso ambiente o mmap volta
// JA-alinhado -> cauda de tamanho 0 -> munmap(addr,0)=EINVAL -> Fatal "munmap (for tail align)
// failed" -> abort no PreInit. munmap aqui e' best-effort (so' devolve overhead de alinhamento);
// reportar sucesso mesmo em falha so' vaza um pouco de address-space, sem risco de correcao. ----
int nier_munmap(void *addr, size_t len) {
  if (len == 0) return 0;
  munmap(addr, len);  /* tenta; ignora resultado (trim de alinhamento e' best-effort) */
  return 0;
}

// ---- redirect do filesystem virtual do UE4 + log das buscas (pak/ini/uproject) ----
// UE4 Android monta paths como "/UE4Game/<Projeto>/..." (raiz nao-gravavel). Redirecionamos
// p/ NIER_GAMEFS (default /storage/roms/ports/nier/gamefs) onde colocamos o pak. Tambem pega
// escritas (saves/config/log) -> tudo no lugar gravavel. ----
#include <stdarg.h>
static int g_open_logs = 0;
static const char *gamefs_base(void) {
  const char *b = getenv("NIER_GAMEFS");
  return b ? b : "/storage/roms/ports/nier/gamefs";
}
static const char *redir(const char *path, char *buf, size_t bufsz) {
  if (path && path[0] == '/' && strncmp(path, "/UE4Game/", 9) == 0) {
    snprintf(buf, bufsz, "%s%s", gamefs_base(), path);
    return buf;
  }
  return path;
}
static void log_open(const char *fn, const char *path, int rc) {
  if (g_open_logs < 600) {
    fprintf(stderr, "[%s] '%s' -> %d%s\n", fn, path ? path : "(null)", rc, rc < 0 ? " FAIL" : "");
    fflush(stderr); g_open_logs++;
  }
}
int nier_open(const char *path, int flags, ...) {
  char b[1024]; const char *p = redir(path, b, sizeof(b));
  mode_t mode = 0;
  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }
  int fd = open(p, flags, mode);
  log_open("open", p, fd);
  return fd;
}
int nier_openat(int dfd, const char *path, int flags, ...) {
  char b[1024]; const char *p = redir(path, b, sizeof(b));
  mode_t mode = 0;
  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }
  int fd = openat(dfd, p, flags, mode);
  log_open("openat", p, fd);
  return fd;
}
FILE *nier_fopen(const char *path, const char *mode) {
  char b[1024]; const char *p = redir(path, b, sizeof(b));
  FILE *f = fopen(p, mode);
  log_open("fopen", p, f ? 0 : -1);
  return f;
}
int nier_access(const char *path, int amode) {
  char b[1024]; const char *p = redir(path, b, sizeof(b));
  int r = access(p, amode);
  log_open("access", p, r);
  return r;
}
#include <dirent.h>
#include <sys/stat.h>
DIR *nier_opendir(const char *path) {
  char b[1024]; const char *p = redir(path, b, sizeof(b));
  DIR *d = opendir(p);
  log_open("opendir", p, d ? 0 : -1);
  return d;
}
int nier_stat(const char *path, struct stat *st) {
  char b[1024]; const char *p = redir(path, b, sizeof(b));
  int r = stat(p, st);
  log_open("stat", p, r);
  return r;
}
int nier_lstat(const char *path, struct stat *st) {
  char b[1024]; const char *p = redir(path, b, sizeof(b));
  int r = lstat(p, st);
  return r;
}
int nier_xstat(int ver, const char *path, struct stat *st) {
  char b[1024]; const char *p = redir(path, b, sizeof(b));
  extern int __xstat(int, const char *, struct stat *);
  int r = __xstat(ver, p, st);
  log_open("__xstat", p, r);
  return r;
}
long nier_sysconf(int name) {
  long r = sysconf(name);
  fprintf(stderr, "[sysconf %d] -> %ld\n", name, r); fflush(stderr);
  return r;
}

// ---- diagnostico: hook pthread_create p/ ver QUAIS threads o UE4 cria no init ----
extern void *text_base; extern size_t text_size;
static int (*real_pthread_create)(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
int nier_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void*), void *arg) {
  if (!real_pthread_create) real_pthread_create = dlsym(RTLD_NEXT, "pthread_create");
  uintptr_t f = (uintptr_t)fn, g = (uintptr_t)arg, tb = (uintptr_t)text_base;
  fprintf(stderr, "[pthread_create] routine=");
  if (f >= tb && f < tb + text_size) fprintf(stderr, "libUE4+0x%lx", (unsigned long)(f - tb));
  else fprintf(stderr, "%p", fn);
  fprintf(stderr, " arg=");
  if (g >= tb && g < tb + text_size) fprintf(stderr, "libUE4+0x%lx", (unsigned long)(g - tb));
  else if (g) { /* arg pode ser objeto: tenta ler vtable[0] e mapear */
    uintptr_t vt = *(uintptr_t *)g;
    if (vt >= tb && vt < tb + text_size) fprintf(stderr, "obj(vtbl=libUE4+0x%lx)", (unsigned long)(vt - tb));
    else fprintf(stderr, "%p", arg);
  } else fprintf(stderr, "null");
  fprintf(stderr, "\n"); fflush(stderr);
  return real_pthread_create(t, a, fn, arg);
}

// ---- diagnostico: hook syscall p/ logar futex WAIT/WAKE no FEvent que a main espera ----
static long (*real_syscall)(long,long,long,long,long,long,long);
long nier_syscall(long n, long a, long b, long c, long d, long e, long f) {
  if (!real_syscall) real_syscall = dlsym(RTLD_NEXT, "syscall");
  if (n == 98) { /* SYS_futex */
    uintptr_t tb = (uintptr_t)text_base, ua = (uintptr_t)a;
    int op = (int)(b & 0x7f);
    if (ua >= tb && ua < tb + text_size && (op == 0 || op == 1))
      fprintf(stderr, "[futex] uaddr=libUE4+0x%lx op=%s val=%ld\n",
              (unsigned long)(ua - tb), op == 0 ? "WAIT" : "WAKE", c);
  }
  return real_syscall(n, a, b, c, d, e, f);
}

// ---- FIX bootstrap do alocador: FMemory::Malloc recursa (Malloc->GCreateMalloc->
// FMallocBinned2::ctor->Malloc) com GMalloc==NULL -> null-deref. Enquanto GMalloc==NULL
// devolvemos memalign (igual bionic), quebrando a recursão; ctor completa, GMalloc é setado. ----
#include <malloc.h>
static void **g_gmalloc_var;  /* = &GMalloc (resolvido no hook) */
void nier_set_gmalloc_var(void **v) { g_gmalloc_var = v; }

/* registro de ponteiros alocados no BOOTSTRAP (GMalloc==NULL) p/ rotear seu free/realloc
 * sempre p/ glibc, mesmo depois que GMalloc existe (binned2 não reconhece ptr externo). */
#define NBOOT 8192
static void *g_boot[NBOOT]; static int g_boot_n;
static int boot_find(void *p) { for (int i = 0; i < g_boot_n; i++) if (g_boot[i] == p) return i; return -1; }
static void boot_add(void *p) { if (g_boot_n < NBOOT) g_boot[g_boot_n++] = p; }
static void boot_del(int i) { if (i >= 0) g_boot[i] = g_boot[--g_boot_n]; }
static inline void *gmalloc(void) { return g_gmalloc_var ? *g_gmalloc_var : (void *)0; }
void nier_FMemory_Free(void *ptr); /* fwd: usado por Realloc(ptr,0)==free antes da definicao */

/* 🧪 TESTE/FIX: rotear TODO FMemory pro glibc (malloc/realloc/free), ignorando o
 * FMallocBinned2. O diagnostico do crash da camada 16 mostrou o buffer de hash de um
 * TSet sendo SOBRESCRITO (bucket=lixo que e' low32 de um ponteiro vizinho) => alocacoes
 * sobrepostas = bug do binned2 (ou da interacao bootstrap<->binned2). glibc malloc e'
 * solido e trata realloc/free uniformemente. Gate: NIER_ALLOC_GLIBC. */
static int g_glibc_alloc = -1;
static inline int use_glibc(void) {
  /* DEFAULT ON: glibc resolveu o crash sistemico de containers (camada 16). Reverter p/
   * o FMallocBinned2 do jogo so' com NIER_BINNED2=1 (p/ comparacao/diagnostico). */
  if (g_glibc_alloc < 0) g_glibc_alloc = getenv("NIER_BINNED2") ? 0 : 1;
  return g_glibc_alloc;
}

void *nier_FMemory_Malloc(unsigned long long count, unsigned int align) {
  if (use_glibc()) { if (align < 16) align = 16; return memalign(align, count ? count : 1); }
  void *gm = gmalloc();
  if (!gm) {                       /* bootstrap */
    if (align < 16) align = 16;
    void *p = memalign(align, count ? count : 1); boot_add(p); return p;
  }
  void **vt = *(void ***)gm;
  void *(*fn)(void *, unsigned long long, unsigned int) = (void *)vt[3]; /* +24 Malloc */
  return fn(gm, count, align);
}
void *nier_FMemory_Realloc(void *ptr, unsigned long long count, unsigned int align) {
  /* 🔑 RAIZ DO MURO SISTEMICO DE CONTAINERS (TSet/TMap): realloc(NULL,n) == malloc(n).
   * O FMallocBinned2::Realloc(Ptr==NULL) entra num fast-path que depende do TLS-cache
   * per-thread do binned (pthread_getspecific @0x4ae9700); no nosso ambiente esse cache
   * pode nao existir p/ a thread -> o caminho devolve NULL. Quando o TSet::Rehash cresce
   * o hash (>=4 elementos: HashSize 1->16) ele chama FMemory::Realloc(NULL, HashSize*4, 0);
   * se vier NULL, o Rehash grava HashSize mas deixa o heap-ptr NULL -> o csel inline-vs-heap
   * cai no inline storage (pequeno) indexado com mask grande -> overflow -> EmplaceImpl
   * le lixo -> crash. Roteamos o caso NULL p/ Malloc (caminho COMPROVADO: a engine alocou
   * o init_array inteiro por ele). Isso destrava TSet/TMap em TODO o engine de uma vez. */
  if (!ptr) return nier_FMemory_Malloc(count, align);
  if (count == 0) { nier_FMemory_Free(ptr); return (void *)0; } /* realloc(ptr,0) == free */
  if (use_glibc()) {
    if (align < 16) align = 16;
    void *np = realloc(ptr, count);
    if (np && ((unsigned long)np & (align - 1))) { /* glibc realloc nao garante >16-align */
      void *a = memalign(align, count);
      if (a) { unsigned long us = malloc_usable_size(np); memcpy(a, np, us < count ? us : count); free(np); return a; }
    }
    return np;
  }
  int bi = boot_find(ptr);
  void *gm = gmalloc();
  if (bi >= 0) {                   /* ptr veio do bootstrap -> glibc realloc */
    if (align < 16) align = 16;
    void *np = realloc(ptr, count); /* nota: realloc nao garante align, mas bootstrap usa default */
    boot_del(bi); if (np) boot_add(np); return np;
  }
  if (!gm) { if (align < 16) align = 16; void *p = memalign(align, count); boot_add(p); return p; }
  void **vt = *(void ***)gm;
  void *(*fn)(void *, void *, unsigned long long, unsigned int) = (void *)vt[4]; /* +32 Realloc */
  return fn(gm, ptr, count, align);
}
/* FMemory::GetAllocSize/QuantizeSize tambem roteiam pro GMalloc (FMallocBinned2) que NAO
 * reconhece ponteiro do glibc -> CanaryFail (heap corrompido) fatal. Com use_glibc, GetAllocSize
 * = malloc_usable_size; QuantizeSize = o proprio tamanho pedido (alinhado). */
unsigned long long nier_FMemory_GetAllocSize(void *ptr) {
  if (use_glibc()) return ptr ? (unsigned long long)malloc_usable_size(ptr) : 0;
  void *gm = gmalloc(); if (!gm || !ptr) return 0;
  void **vt = *(void ***)gm;
  unsigned long long (*fn)(void *, void *) = (void *)vt[7]; /* GetAllocationSizeExternal +56 */
  return fn(gm, ptr);
}
unsigned long long nier_FMemory_QuantizeSize(unsigned long long count, unsigned int align) {
  if (use_glibc()) { if (align < 16) align = 16; return (count + align - 1) & ~(unsigned long long)(align - 1); }
  void *gm = gmalloc(); if (!gm) return count;
  void **vt = *(void ***)gm;
  unsigned long long (*fn)(void *, unsigned long long, unsigned int) = (void *)vt[6]; /* QuantizeSize +48 */
  return fn(gm, count, align);
}

void nier_FMemory_Free(void *ptr) {
  if (!ptr) return;
  if (use_glibc()) { free(ptr); return; }
  int bi = boot_find(ptr);
  if (bi >= 0) { boot_del(bi); free(ptr); return; } /* bootstrap ptr -> glibc free */
  void *gm = gmalloc();
  if (!gm) { free(ptr); return; }
  void **vt = *(void ***)gm;
  void (*fn)(void *, void *) = (void *)vt[5]; /* +40 Free */
  fn(gm, ptr);
}

// ---- FAndroidApplicationMisc::ComputePhysicalScreenDensity(int&): parseia uma string de
// device (DPI/tela) vinda do JNI que vem VAZIA nos nossos stubs -> ParseIntoArray array vazio
// -> array[0] = NULL+8 crash (no ctor do FSlateApplication). Stub: DPI fixo razoavel. ----
int nier_ComputeScreenDensity(int *out_density) {
  if (out_density) *out_density = 320;  /* DPI tipico de telefone */
  return 320;
}

// ---- Stats: FStatGroupEnableManager::GetHighPerformanceEnableForStat faz sondagem de
// hash num mapa VAZIO (HashCount=0 -> and com -1 -> OOB -> loop infinito) no init[267].
// no-op: retorna um TStatIdData* estatico ZERADO (Name=0 => stat invalido/ignorado). ----
static unsigned char g_dummy_stat[64];
void *nier_GetHighPerfStat(void) { return g_dummy_stat; }

// ---- diagnostico: captura a mensagem do check() do UE4 e TENTA pular (retorna) ----
static int g_check_fails;
int nier_CheckVerifyFailed(const char *expr, const char *file, int line, const unsigned short *fmt) {
  if (g_check_fails < 60) {
    fprintf(stderr, "[UE4 CHECK FAIL] expr='%s' file='%s' line=%d msg='",
            expr ? expr : "?", file ? file : "?", line);
    if (fmt) for (int i = 0; i < 160 && fmt[i]; i++) fputc(fmt[i] < 128 ? (char)fmt[i] : '?', stderr);
    fprintf(stderr, "'\n"); fflush(stderr);
  }
  g_check_fails++;
  return 0;  /* false = NÃO quebrar (caller faz if(...)PLATFORM_BREAK) -> tenta seguir */
}

// ---- __cxa_guard ESTILO-BIONIC: o gnustl/glibc BLOQUEIA (futex) na init estatica
// re-entrante da MESMA thread (= o deadlock do GMalloc: FMallocBinned2::ctor->Malloc->
// GCreateMalloc->FMallocBinned2::ctor). O bionic é leniente: re-entrância retorna "já
// feito" e segue com o objeto meio-construído (que o UE4 trata). byte0=done, byte1=in-progress. */
int nier_cxa_guard_acquire(unsigned char *g) {
  if (g[0]) return 0;   /* já inicializado */
  if (g[1]) return 0;   /* em progresso (re-entrância) -> NÃO re-inicializa (anti-deadlock+anti-dupla) */
  g[1] = 1;             /* marca em progresso */
  return 1;             /* faz a init */
}
void nier_cxa_guard_release(unsigned char *g) { g[0] = 1; g[1] = 0; }
void nier_cxa_guard_abort(unsigned char *g)   { g[1] = 0; }

// ---- bloqueia a UE4 (FAndroidMisc) de instalar signal handler p/ sinais de crash:
// assim NOSSO handler ve o fault REAL (a UE4 captura+re-levanta via tgkill, mascarando). ----
#include <signal.h>
static int (*real_sigaction)(int, const void *, void *);
int nier_sigaction(int sig, const void *act, void *old) {
  if (!real_sigaction) real_sigaction = dlsym(RTLD_NEXT, "sigaction");
  if (!getenv("NIER_LETUE4SIG") &&
      (sig == SIGSEGV || sig == SIGBUS || sig == SIGABRT || sig == SIGILL ||
       sig == SIGFPE || sig == SIGTRAP)) {
    fprintf(stderr, "[sigaction] UE4 quis capturar sig=%d -> RECUSADO\n", sig);
    return 0;
  }
  return real_sigaction(sig, act, old);
}

// ---- bionic libc internos (nao existem no glibc) ----
extern int *__errno_location(void);
int *nier_errno(void) { return __errno_location(); }
int nier_isfinitef(float x) { return isfinite(x); }
static char nier_sF[3*256];            // fake bionic __sF[3] (stdin/stdout/stderr)
static unsigned char nier_ctype[384];  // fake _ctype_ (zerado)
void nier_gpbr_begin(void) {}
void nier_gpbr_end(void) {}
void nier_assert2(const char *file, int line, const char *fn, const char *msg) {
  fprintf(stderr, "[UE4 assert] %s:%d %s: %s\n", file?file:"", line, fn?fn:"", msg?msg:"");
  abort();
}
int nier_sysprop_get(const char *name, char *val) { (void)name; if (val) val[0]=0; return 0; }

// === passthrough/pthread/shim: ligados automaticamente ===
DynLibFunction dynlib_functions[] = {
  // TODO {"accept", (uintptr_t)&stub_accept},  // <<< IMPLEMENTAR
  // TODO {"acosf", (uintptr_t)&stub_acosf},  // <<< IMPLEMENTAR
  // TODO {"__addtf3", (uintptr_t)&stub___addtf3},  // <<< IMPLEMENTAR
  {"__android_log_print", (uintptr_t)&__android_log_print},  // liblog
  {"__android_log_write", (uintptr_t)&__android_log_write},  // liblog
  // TODO {"asinf", (uintptr_t)&stub_asinf},  // <<< IMPLEMENTAR
  // TODO {"atanf", (uintptr_t)&stub_atanf},  // <<< IMPLEMENTAR
  // TODO {"basename", (uintptr_t)&stub_basename},  // <<< IMPLEMENTAR
  // TODO {"bind", (uintptr_t)&stub_bind},  // <<< IMPLEMENTAR
  // TODO {"chmod", (uintptr_t)&stub_chmod},  // <<< IMPLEMENTAR
  // TODO {"clock_nanosleep", (uintptr_t)&stub_clock_nanosleep},  // <<< IMPLEMENTAR
  // TODO {"compressBound", (uintptr_t)&stub_compressBound},  // <<< IMPLEMENTAR
  // TODO {"connect", (uintptr_t)&stub_connect},  // <<< IMPLEMENTAR
  // TODO {"deflateReset", (uintptr_t)&stub_deflateReset},  // <<< IMPLEMENTAR
  // TODO {"div", (uintptr_t)&stub_div},  // <<< IMPLEMENTAR
  // TODO {"dladdr", (uintptr_t)&stub_dladdr},  // <<< IMPLEMENTAR
  // TODO {"dlclose", (uintptr_t)&stub_dlclose},  // <<< IMPLEMENTAR
  // TODO {"dlerror", (uintptr_t)&stub_dlerror},  // <<< IMPLEMENTAR
  // TODO {"dlopen", (uintptr_t)&stub_dlopen},  // <<< IMPLEMENTAR
  // TODO {"dlsym", (uintptr_t)&stub_dlsym},  // <<< IMPLEMENTAR
  // TODO {"__dynamic_cast", (uintptr_t)&stub___dynamic_cast},  // <<< IMPLEMENTAR
  {"eglBindAPI", (uintptr_t)&egl_shim_BindAPI},  // egl_shim
  {"eglChooseConfig", (uintptr_t)&egl_shim_ChooseConfig},  // egl_shim
  {"eglCreateContext", (uintptr_t)&egl_shim_CreateContext},  // egl_shim
  {"eglCreatePbufferSurface", (uintptr_t)&egl_shim_CreatePbufferSurface},  // egl_shim
  {"eglCreateWindowSurface", (uintptr_t)&egl_shim_CreateWindowSurface},  // egl_shim
  {"eglDestroyContext", (uintptr_t)&egl_shim_DestroyContext},  // egl_shim
  {"eglDestroySurface", (uintptr_t)&egl_shim_DestroySurface},  // egl_shim
  {"eglGetConfigAttrib", (uintptr_t)&egl_shim_GetConfigAttrib},  // egl_shim
  {"eglGetCurrentContext", (uintptr_t)&egl_shim_GetCurrentContext},  // egl_shim
  {"eglGetDisplay", (uintptr_t)&egl_shim_GetDisplay},  // egl_shim
  {"eglGetError", (uintptr_t)&egl_shim_GetError},  // egl_shim
  {"eglGetProcAddress", (uintptr_t)&egl_shim_GetProcAddress},  // egl_shim
  {"eglInitialize", (uintptr_t)&egl_shim_Initialize},  // egl_shim
  {"eglMakeCurrent", (uintptr_t)&egl_shim_MakeCurrent},  // egl_shim
  {"eglQueryString", (uintptr_t)&egl_shim_QueryString},  // egl_shim
  {"eglQuerySurface", (uintptr_t)&egl_shim_QuerySurface},  // egl_shim
  {"eglSurfaceAttrib", (uintptr_t)&egl_shim_SurfaceAttrib},  // egl_shim
  {"eglSwapBuffers", (uintptr_t)&egl_shim_SwapBuffers},  // egl_shim
  {"eglSwapInterval", (uintptr_t)&egl_shim_SwapInterval},  // egl_shim
  {"eglTerminate", (uintptr_t)&egl_shim_Terminate},  // egl_shim
  // TODO {"execl", (uintptr_t)&stub_execl},  // <<< IMPLEMENTAR
  // TODO {"exp2f", (uintptr_t)&stub_exp2f},  // <<< IMPLEMENTAR
  // TODO {"fdatasync", (uintptr_t)&stub_fdatasync},  // <<< IMPLEMENTAR
  // TODO {"feof", (uintptr_t)&stub_feof},  // <<< IMPLEMENTAR
  // TODO {"ferror", (uintptr_t)&stub_ferror},  // <<< IMPLEMENTAR
  // TODO {"fesetround", (uintptr_t)&stub_fesetround},  // <<< IMPLEMENTAR
  // TODO {"fork", (uintptr_t)&stub_fork},  // <<< IMPLEMENTAR
  // TODO {"freeaddrinfo", (uintptr_t)&stub_freeaddrinfo},  // <<< IMPLEMENTAR
  // TODO {"fsync", (uintptr_t)&stub_fsync},  // <<< IMPLEMENTAR
  // TODO {"ftruncate", (uintptr_t)&stub_ftruncate},  // <<< IMPLEMENTAR
  // TODO {"getaddrinfo", (uintptr_t)&stub_getaddrinfo},  // <<< IMPLEMENTAR
  // TODO {"getegid", (uintptr_t)&stub_getegid},  // <<< IMPLEMENTAR
  // TODO {"geteuid", (uintptr_t)&stub_geteuid},  // <<< IMPLEMENTAR
  // TODO {"getgid", (uintptr_t)&stub_getgid},  // <<< IMPLEMENTAR
  // TODO {"gethostbyaddr", (uintptr_t)&stub_gethostbyaddr},  // <<< IMPLEMENTAR
  // TODO {"gethostbyname", (uintptr_t)&stub_gethostbyname},  // <<< IMPLEMENTAR
  // TODO {"gethostname", (uintptr_t)&stub_gethostname},  // <<< IMPLEMENTAR
  // TODO {"getnameinfo", (uintptr_t)&stub_getnameinfo},  // <<< IMPLEMENTAR
  // TODO {"getpeername", (uintptr_t)&stub_getpeername},  // <<< IMPLEMENTAR
  // TODO {"getpwuid", (uintptr_t)&stub_getpwuid},  // <<< IMPLEMENTAR
  // TODO {"getrlimit", (uintptr_t)&stub_getrlimit},  // <<< IMPLEMENTAR
  // TODO {"getsockname", (uintptr_t)&stub_getsockname},  // <<< IMPLEMENTAR
  // TODO {"getsockopt", (uintptr_t)&stub_getsockopt},  // <<< IMPLEMENTAR
  // TODO {"getuid", (uintptr_t)&stub_getuid},  // <<< IMPLEMENTAR
  {"glActiveTexture", (uintptr_t)&glActiveTexture},  // gles
  {"glAttachShader", (uintptr_t)&glAttachShader},  // gles
  {"glBindAttribLocation", (uintptr_t)&glBindAttribLocation},  // gles
  {"glBindBuffer", (uintptr_t)&glBindBuffer},  // gles
  {"glBindFramebuffer", (uintptr_t)&glBindFramebuffer},  // gles
  {"glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer},  // gles
  {"glBindTexture", (uintptr_t)&glBindTexture},  // gles
  {"glBlendEquation", (uintptr_t)&glBlendEquation},  // gles
  {"glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate},  // gles
  {"glBlendFunc", (uintptr_t)&glBlendFunc},  // gles
  {"glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate},  // gles
  {"glBufferData", (uintptr_t)&glBufferData},  // gles
  {"glBufferSubData", (uintptr_t)&glBufferSubData},  // gles
  {"glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus},  // gles
  {"glClear", (uintptr_t)&glClear},  // gles
  {"glClearColor", (uintptr_t)&glClearColor},  // gles
  {"glClearDepthf", (uintptr_t)&glClearDepthf},  // gles
  {"glClearStencil", (uintptr_t)&glClearStencil},  // gles
  {"glColorMask", (uintptr_t)&glColorMask},  // gles
  {"glCompileShader", (uintptr_t)&glCompileShader},  // gles
  {"glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D},  // gles
  {"glCompressedTexSubImage2D", (uintptr_t)&glCompressedTexSubImage2D},  // gles
  {"glCopyTexSubImage2D", (uintptr_t)&glCopyTexSubImage2D},  // gles
  {"glCreateProgram", (uintptr_t)&glCreateProgram},  // gles
  {"glCreateShader", (uintptr_t)&glCreateShader},  // gles
  {"glCullFace", (uintptr_t)&glCullFace},  // gles
  {"glDeleteBuffers", (uintptr_t)&glDeleteBuffers},  // gles
  {"glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers},  // gles
  {"glDeleteProgram", (uintptr_t)&glDeleteProgram},  // gles
  {"glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers},  // gles
  {"glDeleteShader", (uintptr_t)&glDeleteShader},  // gles
  {"glDeleteTextures", (uintptr_t)&glDeleteTextures},  // gles
  {"glDepthFunc", (uintptr_t)&glDepthFunc},  // gles
  {"glDepthMask", (uintptr_t)&glDepthMask},  // gles
  {"glDepthRangef", (uintptr_t)&glDepthRangef},  // gles
  {"glDisable", (uintptr_t)&glDisable},  // gles
  {"glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray},  // gles
  {"glDrawArrays", (uintptr_t)&glDrawArrays},  // gles
  {"glDrawElements", (uintptr_t)&glDrawElements},  // gles
  {"glEnable", (uintptr_t)&glEnable},  // gles
  {"glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray},  // gles
  {"glFlush", (uintptr_t)&glFlush},  // gles
  {"glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer},  // gles
  {"glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D},  // gles
  {"glGenBuffers", (uintptr_t)&glGenBuffers},  // gles
  {"glGenFramebuffers", (uintptr_t)&glGenFramebuffers},  // gles
  {"glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers},  // gles
  {"glGenTextures", (uintptr_t)&glGenTextures},  // gles
  {"glGetBooleanv", (uintptr_t)&glGetBooleanv},  // gles
  {"glGetError", (uintptr_t)&glGetError},  // gles
  {"glGetIntegerv", (uintptr_t)&glGetIntegerv},  // gles
  {"glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog},  // gles
  {"glGetProgramiv", (uintptr_t)&glGetProgramiv},  // gles
  {"glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog},  // gles
  {"glGetShaderiv", (uintptr_t)&glGetShaderiv},  // gles
  {"glGetShaderPrecisionFormat", (uintptr_t)&glGetShaderPrecisionFormat},  // gles
  {"glGetString", (uintptr_t)&glGetString},  // gles
  {"glGetUniformLocation", (uintptr_t)&glGetUniformLocation},  // gles
  {"glIsEnabled", (uintptr_t)&glIsEnabled},  // gles
  {"glLinkProgram", (uintptr_t)&glLinkProgram},  // gles
  {"glPixelStorei", (uintptr_t)&glPixelStorei},  // gles
  {"glPolygonOffset", (uintptr_t)&glPolygonOffset},  // gles
  {"glReadPixels", (uintptr_t)&glReadPixels},  // gles
  {"glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage},  // gles
  {"glScissor", (uintptr_t)&glScissor},  // gles
  {"glShaderSource", (uintptr_t)&glShaderSource},  // gles
  {"glStencilFunc", (uintptr_t)&glStencilFunc},  // gles
  {"glStencilFuncSeparate", (uintptr_t)&glStencilFuncSeparate},  // gles
  {"glStencilMask", (uintptr_t)&glStencilMask},  // gles
  {"glStencilOp", (uintptr_t)&glStencilOp},  // gles
  {"glStencilOpSeparate", (uintptr_t)&glStencilOpSeparate},  // gles
  {"glTexImage2D", (uintptr_t)&glTexImage2D},  // gles
  {"glTexParameteri", (uintptr_t)&glTexParameteri},  // gles
  {"glTexSubImage2D", (uintptr_t)&glTexSubImage2D},  // gles
  {"glUniform1i", (uintptr_t)&glUniform1i},  // gles
  {"glUniform4fv", (uintptr_t)&glUniform4fv},  // gles
  {"glUniform4iv", (uintptr_t)&glUniform4iv},  // gles
  {"glUseProgram", (uintptr_t)&glUseProgram},  // gles
  {"glVertexAttrib4fv", (uintptr_t)&glVertexAttrib4fv},  // gles
  {"glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer},  // gles
  {"glViewport", (uintptr_t)&glViewport},  // gles
  // TODO {"gmtime_r", (uintptr_t)&stub_gmtime_r},  // <<< IMPLEMENTAR
  // TODO {"if_nametoindex", (uintptr_t)&stub_if_nametoindex},  // <<< IMPLEMENTAR
  // TODO {"inet_addr", (uintptr_t)&stub_inet_addr},  // <<< IMPLEMENTAR
  // TODO {"inet_ntop", (uintptr_t)&stub_inet_ntop},  // <<< IMPLEMENTAR
  // TODO {"inet_pton", (uintptr_t)&stub_inet_pton},  // <<< IMPLEMENTAR
  // TODO {"inflateReset", (uintptr_t)&stub_inflateReset},  // <<< IMPLEMENTAR
  // TODO {"__isfinitef", (uintptr_t)&stub___isfinitef},  // <<< IMPLEMENTAR
  // TODO {"isgraph", (uintptr_t)&stub_isgraph},  // <<< IMPLEMENTAR
  // TODO {"isprint", (uintptr_t)&stub_isprint},  // <<< IMPLEMENTAR
  // TODO {"iswalnum", (uintptr_t)&stub_iswalnum},  // <<< IMPLEMENTAR
  // TODO {"iswalpha", (uintptr_t)&stub_iswalpha},  // <<< IMPLEMENTAR
  // TODO {"iswdigit", (uintptr_t)&stub_iswdigit},  // <<< IMPLEMENTAR
  // TODO {"iswlower", (uintptr_t)&stub_iswlower},  // <<< IMPLEMENTAR
  // TODO {"iswpunct", (uintptr_t)&stub_iswpunct},  // <<< IMPLEMENTAR
  // TODO {"iswupper", (uintptr_t)&stub_iswupper},  // <<< IMPLEMENTAR
  // TODO {"iswxdigit", (uintptr_t)&stub_iswxdigit},  // <<< IMPLEMENTAR
  // TODO {"isxdigit", (uintptr_t)&stub_isxdigit},  // <<< IMPLEMENTAR
  // TODO {"kill", (uintptr_t)&stub_kill},  // <<< IMPLEMENTAR
  // TODO {"ldexpf", (uintptr_t)&stub_ldexpf},  // <<< IMPLEMENTAR
  // TODO {"listen", (uintptr_t)&stub_listen},  // <<< IMPLEMENTAR
  // TODO {"localtime_r", (uintptr_t)&stub_localtime_r},  // <<< IMPLEMENTAR
  // TODO {"log10f", (uintptr_t)&stub_log10f},  // <<< IMPLEMENTAR
  // TODO {"longjmp", (uintptr_t)&stub_longjmp},  // <<< IMPLEMENTAR
  // TODO {"lrint", (uintptr_t)&stub_lrint},  // <<< IMPLEMENTAR
  // TODO {"lrintf", (uintptr_t)&stub_lrintf},  // <<< IMPLEMENTAR
  // TODO {"lseek64", (uintptr_t)&stub_lseek64},  // <<< IMPLEMENTAR
  // TODO {"memrchr", (uintptr_t)&stub_memrchr},  // <<< IMPLEMENTAR
  // TODO {"modff", (uintptr_t)&stub_modff},  // <<< IMPLEMENTAR
  // TODO {"__multf3", (uintptr_t)&stub___multf3},  // <<< IMPLEMENTAR
  // TODO {"ovrp_BeginFrame4", (uintptr_t)&stub_ovrp_BeginFrame4},  // <<< IMPLEMENTAR
  // TODO {"ovrp_CalculateEyeLayerDesc2", (uintptr_t)&stub_ovrp_CalculateEyeLayerDesc2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_CalculateEyeViewportRect", (uintptr_t)&stub_ovrp_CalculateEyeViewportRect},  // <<< IMPLEMENTAR
  // TODO {"ovrp_CalculateLayerDesc", (uintptr_t)&stub_ovrp_CalculateLayerDesc},  // <<< IMPLEMENTAR
  // TODO {"ovrp_CloseCameraDevice", (uintptr_t)&stub_ovrp_CloseCameraDevice},  // <<< IMPLEMENTAR
  // TODO {"ovrp_DestroyDistortionWindow2", (uintptr_t)&stub_ovrp_DestroyDistortionWindow2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_DestroyLayer", (uintptr_t)&stub_ovrp_DestroyLayer},  // <<< IMPLEMENTAR
  // TODO {"ovrp_DestroyMirrorTexture2", (uintptr_t)&stub_ovrp_DestroyMirrorTexture2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_DoesCameraDeviceSupportDepth", (uintptr_t)&stub_ovrp_DoesCameraDeviceSupportDepth},  // <<< IMPLEMENTAR
  // TODO {"ovrp_EndFrame4", (uintptr_t)&stub_ovrp_EndFrame4},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetAdaptiveGpuPerformanceScale2", (uintptr_t)&stub_ovrp_GetAdaptiveGpuPerformanceScale2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetAppFramerate2", (uintptr_t)&stub_ovrp_GetAppFramerate2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetAppHasInputFocus", (uintptr_t)&stub_ovrp_GetAppHasInputFocus},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetAppHasVrFocus2", (uintptr_t)&stub_ovrp_GetAppHasVrFocus2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetAppLatencyTimings2", (uintptr_t)&stub_ovrp_GetAppLatencyTimings2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetAppShouldQuit2", (uintptr_t)&stub_ovrp_GetAppShouldQuit2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetAppShouldRecenter2", (uintptr_t)&stub_ovrp_GetAppShouldRecenter2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetAppShouldRecreateDistortionWindow2", (uintptr_t)&stub_ovrp_GetAppShouldRecreateDistortionWindow2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetBoundaryConfigured2", (uintptr_t)&stub_ovrp_GetBoundaryConfigured2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetBoundaryDimensions2", (uintptr_t)&stub_ovrp_GetBoundaryDimensions2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetBoundaryGeometry3", (uintptr_t)&stub_ovrp_GetBoundaryGeometry3},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetBoundaryVisible2", (uintptr_t)&stub_ovrp_GetBoundaryVisible2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetCameraDeviceColorFrameBgraPixels", (uintptr_t)&stub_ovrp_GetCameraDeviceColorFrameBgraPixels},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetCameraDeviceColorFrameSize", (uintptr_t)&stub_ovrp_GetCameraDeviceColorFrameSize},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetCameraDeviceDepthFramePixels", (uintptr_t)&stub_ovrp_GetCameraDeviceDepthFramePixels},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetCameraDeviceDepthFrameSize", (uintptr_t)&stub_ovrp_GetCameraDeviceDepthFrameSize},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetControllerHapticsDesc2", (uintptr_t)&stub_ovrp_GetControllerHapticsDesc2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetControllerHapticsState2", (uintptr_t)&stub_ovrp_GetControllerHapticsState2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetControllerState4", (uintptr_t)&stub_ovrp_GetControllerState4},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetDeviceExtensionsVk", (uintptr_t)&stub_ovrp_GetDeviceExtensionsVk},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetExternalCameraCalibrationRawPose", (uintptr_t)&stub_ovrp_GetExternalCameraCalibrationRawPose},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetExternalCameraCount", (uintptr_t)&stub_ovrp_GetExternalCameraCount},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetExternalCameraExtrinsics", (uintptr_t)&stub_ovrp_GetExternalCameraExtrinsics},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetExternalCameraIntrinsics", (uintptr_t)&stub_ovrp_GetExternalCameraIntrinsics},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetExternalCameraName", (uintptr_t)&stub_ovrp_GetExternalCameraName},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetGPUFrameTime", (uintptr_t)&stub_ovrp_GetGPUFrameTime},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetGPUUtilLevel", (uintptr_t)&stub_ovrp_GetGPUUtilLevel},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetGPUUtilSupported", (uintptr_t)&stub_ovrp_GetGPUUtilSupported},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetInitialized", (uintptr_t)&stub_ovrp_GetInitialized},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetInstanceExtensionsVk", (uintptr_t)&stub_ovrp_GetInstanceExtensionsVk},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetLayerTexture2", (uintptr_t)&stub_ovrp_GetLayerTexture2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetLayerTextureFoveation", (uintptr_t)&stub_ovrp_GetLayerTextureFoveation},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetLayerTextureStageCount", (uintptr_t)&stub_ovrp_GetLayerTextureStageCount},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetMixedRealityInitialized", (uintptr_t)&stub_ovrp_GetMixedRealityInitialized},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetNodeFrustum2", (uintptr_t)&stub_ovrp_GetNodeFrustum2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetNodeOrientationTracked2", (uintptr_t)&stub_ovrp_GetNodeOrientationTracked2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetNodeOrientationValid", (uintptr_t)&stub_ovrp_GetNodeOrientationValid},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetNodePoseState3", (uintptr_t)&stub_ovrp_GetNodePoseState3},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetNodePositionTracked2", (uintptr_t)&stub_ovrp_GetNodePositionTracked2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetNodePositionValid", (uintptr_t)&stub_ovrp_GetNodePositionValid},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetNodePresent2", (uintptr_t)&stub_ovrp_GetNodePresent2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetPerfMetricsFloat", (uintptr_t)&stub_ovrp_GetPerfMetricsFloat},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetPerfMetricsInt", (uintptr_t)&stub_ovrp_GetPerfMetricsInt},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetSystemDisplayAvailableFrequencies", (uintptr_t)&stub_ovrp_GetSystemDisplayAvailableFrequencies},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetSystemDisplayFrequency2", (uintptr_t)&stub_ovrp_GetSystemDisplayFrequency2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetSystemHeadsetType2", (uintptr_t)&stub_ovrp_GetSystemHeadsetType2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetSystemMultiViewSupported2", (uintptr_t)&stub_ovrp_GetSystemMultiViewSupported2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetSystemProductName2", (uintptr_t)&stub_ovrp_GetSystemProductName2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetSystemRecommendedMSAALevel2", (uintptr_t)&stub_ovrp_GetSystemRecommendedMSAALevel2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetTiledMultiResLevel", (uintptr_t)&stub_ovrp_GetTiledMultiResLevel},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetTrackingOriginType2", (uintptr_t)&stub_ovrp_GetTrackingOriginType2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetTrackingPositionSupported2", (uintptr_t)&stub_ovrp_GetTrackingPositionSupported2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetTrackingTransformRawPose", (uintptr_t)&stub_ovrp_GetTrackingTransformRawPose},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetUserEyeHeight2", (uintptr_t)&stub_ovrp_GetUserEyeHeight2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetUserIPD2", (uintptr_t)&stub_ovrp_GetUserIPD2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetUserNeckEyeDistance2", (uintptr_t)&stub_ovrp_GetUserNeckEyeDistance2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetUserPresent2", (uintptr_t)&stub_ovrp_GetUserPresent2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetVersion2", (uintptr_t)&stub_ovrp_GetVersion2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_GetViewportStencil", (uintptr_t)&stub_ovrp_GetViewportStencil},  // <<< IMPLEMENTAR
  // TODO {"ovrp_Initialize5", (uintptr_t)&stub_ovrp_Initialize5},  // <<< IMPLEMENTAR
  // TODO {"ovrp_InitializeMixedReality", (uintptr_t)&stub_ovrp_InitializeMixedReality},  // <<< IMPLEMENTAR
  // TODO {"ovrp_IsCameraDeviceColorFrameAvailable2", (uintptr_t)&stub_ovrp_IsCameraDeviceColorFrameAvailable2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_IsCameraDeviceDepthFrameAvailable", (uintptr_t)&stub_ovrp_IsCameraDeviceDepthFrameAvailable},  // <<< IMPLEMENTAR
  // TODO {"ovrp_IsPerfMetricsSupported", (uintptr_t)&stub_ovrp_IsPerfMetricsSupported},  // <<< IMPLEMENTAR
  // TODO {"ovrp_Media_EncodeMrcFrameWithDualTextures", (uintptr_t)&stub_ovrp_Media_EncodeMrcFrameWithDualTextures},  // <<< IMPLEMENTAR
  // TODO {"ovrp_Media_GetInitialized", (uintptr_t)&stub_ovrp_Media_GetInitialized},  // <<< IMPLEMENTAR
  // TODO {"ovrp_Media_GetMrcFrameSize", (uintptr_t)&stub_ovrp_Media_GetMrcFrameSize},  // <<< IMPLEMENTAR
  // TODO {"ovrp_Media_Initialize", (uintptr_t)&stub_ovrp_Media_Initialize},  // <<< IMPLEMENTAR
  // TODO {"ovrp_Media_IsMrcActivated", (uintptr_t)&stub_ovrp_Media_IsMrcActivated},  // <<< IMPLEMENTAR
  // TODO {"ovrp_Media_IsMrcEnabled", (uintptr_t)&stub_ovrp_Media_IsMrcEnabled},  // <<< IMPLEMENTAR
  // TODO {"ovrp_Media_SetMrcActivationMode", (uintptr_t)&stub_ovrp_Media_SetMrcActivationMode},  // <<< IMPLEMENTAR
  // TODO {"ovrp_Media_SetMrcAudioSampleRate", (uintptr_t)&stub_ovrp_Media_SetMrcAudioSampleRate},  // <<< IMPLEMENTAR
  // TODO {"ovrp_Media_SetMrcFrameImageFlipped", (uintptr_t)&stub_ovrp_Media_SetMrcFrameImageFlipped},  // <<< IMPLEMENTAR
  // TODO {"ovrp_Media_SetMrcFrameInverseAlpha", (uintptr_t)&stub_ovrp_Media_SetMrcFrameInverseAlpha},  // <<< IMPLEMENTAR
  // TODO {"ovrp_Media_SetMrcInputVideoBufferType", (uintptr_t)&stub_ovrp_Media_SetMrcInputVideoBufferType},  // <<< IMPLEMENTAR
  // TODO {"ovrp_Media_Shutdown", (uintptr_t)&stub_ovrp_Media_Shutdown},  // <<< IMPLEMENTAR
  // TODO {"ovrp_Media_SyncMrcFrame", (uintptr_t)&stub_ovrp_Media_SyncMrcFrame},  // <<< IMPLEMENTAR
  // TODO {"ovrp_Media_Update", (uintptr_t)&stub_ovrp_Media_Update},  // <<< IMPLEMENTAR
  // TODO {"ovrp_PreInitialize3", (uintptr_t)&stub_ovrp_PreInitialize3},  // <<< IMPLEMENTAR
  // TODO {"ovrp_RecenterTrackingOrigin2", (uintptr_t)&stub_ovrp_RecenterTrackingOrigin2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetAppCPUPriority2", (uintptr_t)&stub_ovrp_SetAppCPUPriority2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetAppEngineInfo2", (uintptr_t)&stub_ovrp_SetAppEngineInfo2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetBoundaryVisible2", (uintptr_t)&stub_ovrp_SetBoundaryVisible2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetControllerHaptics2", (uintptr_t)&stub_ovrp_SetControllerHaptics2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetControllerVibration2", (uintptr_t)&stub_ovrp_SetControllerVibration2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetReorientHMDOnControllerRecenter", (uintptr_t)&stub_ovrp_SetReorientHMDOnControllerRecenter},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetSystemCpuLevel2", (uintptr_t)&stub_ovrp_SetSystemCpuLevel2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetSystemDisplayFrequency", (uintptr_t)&stub_ovrp_SetSystemDisplayFrequency},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetSystemGpuLevel2", (uintptr_t)&stub_ovrp_SetSystemGpuLevel2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetTiledMultiResLevel", (uintptr_t)&stub_ovrp_SetTiledMultiResLevel},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetTrackingOrientationEnabled2", (uintptr_t)&stub_ovrp_SetTrackingOrientationEnabled2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetTrackingOriginType2", (uintptr_t)&stub_ovrp_SetTrackingOriginType2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetTrackingPositionEnabled2", (uintptr_t)&stub_ovrp_SetTrackingPositionEnabled2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetupDisplayObjects2", (uintptr_t)&stub_ovrp_SetupDisplayObjects2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetupDistortionWindow3", (uintptr_t)&stub_ovrp_SetupDistortionWindow3},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetupLayer", (uintptr_t)&stub_ovrp_SetupLayer},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetupMirrorTexture2", (uintptr_t)&stub_ovrp_SetupMirrorTexture2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_SetUserIPD2", (uintptr_t)&stub_ovrp_SetUserIPD2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_ShowSystemUI2", (uintptr_t)&stub_ovrp_ShowSystemUI2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_Shutdown2", (uintptr_t)&stub_ovrp_Shutdown2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_ShutdownMixedReality", (uintptr_t)&stub_ovrp_ShutdownMixedReality},  // <<< IMPLEMENTAR
  // TODO {"ovrp_TestBoundaryNode2", (uintptr_t)&stub_ovrp_TestBoundaryNode2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_TestBoundaryPoint2", (uintptr_t)&stub_ovrp_TestBoundaryPoint2},  // <<< IMPLEMENTAR
  // TODO {"ovrp_Update3", (uintptr_t)&stub_ovrp_Update3},  // <<< IMPLEMENTAR
  // TODO {"ovrp_UpdateCameraDevices", (uintptr_t)&stub_ovrp_UpdateCameraDevices},  // <<< IMPLEMENTAR
  // TODO {"ovrp_UpdateExternalCamera", (uintptr_t)&stub_ovrp_UpdateExternalCamera},  // <<< IMPLEMENTAR
  // TODO {"ovrp_WaitToBeginFrame", (uintptr_t)&stub_ovrp_WaitToBeginFrame},  // <<< IMPLEMENTAR
  // TODO {"pause", (uintptr_t)&stub_pause},  // <<< IMPLEMENTAR
  // TODO {"poll", (uintptr_t)&stub_poll},  // <<< IMPLEMENTAR
  // TODO {"pread64", (uintptr_t)&stub_pread64},  // <<< IMPLEMENTAR
  // TODO {"putchar", (uintptr_t)&stub_putchar},  // <<< IMPLEMENTAR
  // TODO {"pwrite64", (uintptr_t)&stub_pwrite64},  // <<< IMPLEMENTAR
  // TODO {"recv", (uintptr_t)&stub_recv},  // <<< IMPLEMENTAR
  // TODO {"recvfrom", (uintptr_t)&stub_recvfrom},  // <<< IMPLEMENTAR
  // TODO {"sched_get_priority_max", (uintptr_t)&stub_sched_get_priority_max},  // <<< IMPLEMENTAR
  // TODO {"sched_get_priority_min", (uintptr_t)&stub_sched_get_priority_min},  // <<< IMPLEMENTAR
  // TODO {"sched_getscheduler", (uintptr_t)&stub_sched_getscheduler},  // <<< IMPLEMENTAR
  // TODO {"select", (uintptr_t)&stub_select},  // <<< IMPLEMENTAR
  // TODO {"send", (uintptr_t)&stub_send},  // <<< IMPLEMENTAR
  // TODO {"sendto", (uintptr_t)&stub_sendto},  // <<< IMPLEMENTAR
  // TODO {"setjmp", (uintptr_t)&stub_setjmp},  // <<< IMPLEMENTAR
  // TODO {"setrlimit", (uintptr_t)&stub_setrlimit},  // <<< IMPLEMENTAR
  // TODO {"setsockopt", (uintptr_t)&stub_setsockopt},  // <<< IMPLEMENTAR
  // TODO {"__sF", (uintptr_t)&stub___sF},  // <<< IMPLEMENTAR
  // TODO {"__sfp_handle_exceptions", (uintptr_t)&stub___sfp_handle_exceptions},  // <<< IMPLEMENTAR
  // TODO {"shutdown", (uintptr_t)&stub_shutdown},  // <<< IMPLEMENTAR
  // TODO {"sigaction", (uintptr_t)&stub_sigaction},  // <<< IMPLEMENTAR
  // TODO {"sigemptyset", (uintptr_t)&stub_sigemptyset},  // <<< IMPLEMENTAR
  // TODO {"signal", (uintptr_t)&stub_signal},  // <<< IMPLEMENTAR
  // TODO {"sinhf", (uintptr_t)&stub_sinhf},  // <<< IMPLEMENTAR
  {"slCreateEngine", (uintptr_t)&slCreateEngine_shim},  // opensles_shim
  // TODO {"socket", (uintptr_t)&stub_socket},  // <<< IMPLEMENTAR
  // TODO {"socketpair", (uintptr_t)&stub_socketpair},  // <<< IMPLEMENTAR
  // TODO {"statfs", (uintptr_t)&stub_statfs},  // <<< IMPLEMENTAR
  // TODO {"strerror_r", (uintptr_t)&stub_strerror_r},  // <<< IMPLEMENTAR
  // TODO {"strpbrk", (uintptr_t)&stub_strpbrk},  // <<< IMPLEMENTAR
  // TODO {"strtok_r", (uintptr_t)&stub_strtok_r},  // <<< IMPLEMENTAR
  // TODO {"strtoll", (uintptr_t)&stub_strtoll},  // <<< IMPLEMENTAR
  // TODO {"strtoull", (uintptr_t)&stub_strtoull},  // <<< IMPLEMENTAR
  // TODO {"syscall", (uintptr_t)&stub_syscall},  // <<< IMPLEMENTAR
  // TODO {"sysinfo", (uintptr_t)&stub_sysinfo},  // <<< IMPLEMENTAR
  // TODO {"__system_property_get", (uintptr_t)&stub___system_property_get},  // <<< IMPLEMENTAR
  // TODO {"tcgetattr", (uintptr_t)&stub_tcgetattr},  // <<< IMPLEMENTAR
  // TODO {"tcsetattr", (uintptr_t)&stub_tcsetattr},  // <<< IMPLEMENTAR
  // TODO {"timezone", (uintptr_t)&stub_timezone},  // <<< IMPLEMENTAR
  // TODO {"towlower", (uintptr_t)&stub_towlower},  // <<< IMPLEMENTAR
  // TODO {"towupper", (uintptr_t)&stub_towupper},  // <<< IMPLEMENTAR
  // TODO {"__trunctfdf2", (uintptr_t)&stub___trunctfdf2},  // <<< IMPLEMENTAR
  // TODO {"tzname", (uintptr_t)&stub_tzname},  // <<< IMPLEMENTAR
  // TODO {"tzset", (uintptr_t)&stub_tzset},  // <<< IMPLEMENTAR
  // TODO {"_Unwind_Backtrace", (uintptr_t)&stub__Unwind_Backtrace},  // <<< IMPLEMENTAR
  // TODO {"_Unwind_GetIP", (uintptr_t)&stub__Unwind_GetIP},  // <<< IMPLEMENTAR
  // TODO {"_Unwind_Resume", (uintptr_t)&stub__Unwind_Resume},  // <<< IMPLEMENTAR
  // TODO {"vasprintf", (uintptr_t)&stub_vasprintf},  // <<< IMPLEMENTAR
  // TODO {"vfprintf", (uintptr_t)&stub_vfprintf},  // <<< IMPLEMENTAR
  // TODO {"waitpid", (uintptr_t)&stub_waitpid},  // <<< IMPLEMENTAR
  // TODO {"wcschr", (uintptr_t)&stub_wcschr},  // <<< IMPLEMENTAR
  // TODO {"zlibVersion", (uintptr_t)&stub_zlibVersion},  // <<< IMPLEMENTAR
  // TODO {"_ZNKSs13find_first_ofEPKcm", (uintptr_t)&stub__ZNKSs13find_first_ofEPKcm},  // <<< IMPLEMENTAR
  // TODO {"_ZNKSs17find_first_not_ofEPKcm", (uintptr_t)&stub__ZNKSs17find_first_not_ofEPKcm},  // <<< IMPLEMENTAR
  // TODO {"_ZNKSs3endEv", (uintptr_t)&stub__ZNKSs3endEv},  // <<< IMPLEMENTAR
  // TODO {"_ZNKSs4findEcm", (uintptr_t)&stub__ZNKSs4findEcm},  // <<< IMPLEMENTAR
  // TODO {"_ZNKSs4findEPKcmm", (uintptr_t)&stub__ZNKSs4findEPKcmm},  // <<< IMPLEMENTAR
  // TODO {"_ZNKSs4findERKSsm", (uintptr_t)&stub__ZNKSs4findERKSsm},  // <<< IMPLEMENTAR
  // TODO {"_ZNKSs6substrEmm", (uintptr_t)&stub__ZNKSs6substrEmm},  // <<< IMPLEMENTAR
  // TODO {"_ZNKSs7compareEPKc", (uintptr_t)&stub__ZNKSs7compareEPKc},  // <<< IMPLEMENTAR
  // TODO {"_ZNKSs7compareERKSs", (uintptr_t)&stub__ZNKSs7compareERKSs},  // <<< IMPLEMENTAR
  // TODO {"_ZNKSs7_M_iendEv", (uintptr_t)&stub__ZNKSs7_M_iendEv},  // <<< IMPLEMENTAR
  // TODO {"_ZNKSt15basic_stringbufIcSt11char_traitsIcESaIcEE3strEv", (uintptr_t)&stub__ZNKSt15basic_stringbufIcSt11char_traitsIcESaIcEE3strEv},  // <<< IMPLEMENTAR
  // TODO {"_ZNKSt5ctypeIcE13_M_widen_initEv", (uintptr_t)&stub__ZNKSt5ctypeIcE13_M_widen_initEv},  // <<< IMPLEMENTAR
  // TODO {"_ZNKSt8__detail20_Prime_rehash_policy11_M_next_bktEm", (uintptr_t)&stub__ZNKSt8__detail20_Prime_rehash_policy11_M_next_bktEm},  // <<< IMPLEMENTAR
  // TODO {"_ZNKSt8__detail20_Prime_rehash_policy14_M_need_rehashEmmm", (uintptr_t)&stub__ZNKSt8__detail20_Prime_rehash_policy14_M_need_rehashEmmm},  // <<< IMPLEMENTAR
  // TODO {"_ZNKSt9basic_iosIcSt11char_traitsIcEE4fillEv", (uintptr_t)&stub__ZNKSt9basic_iosIcSt11char_traitsIcEE4fillEv},  // <<< IMPLEMENTAR
  // TODO {"_ZNSi4readEPcl", (uintptr_t)&stub__ZNSi4readEPcl},  // <<< IMPLEMENTAR
  // TODO {"_ZNSo3putEc", (uintptr_t)&stub__ZNSo3putEc},  // <<< IMPLEMENTAR
  // TODO {"_ZNSo5flushEv", (uintptr_t)&stub__ZNSo5flushEv},  // <<< IMPLEMENTAR
  // TODO {"_ZNSo5writeEPKcl", (uintptr_t)&stub__ZNSo5writeEPKcl},  // <<< IMPLEMENTAR
  // TODO {"_ZNSo9_M_insertIbEERSoT_", (uintptr_t)&stub__ZNSo9_M_insertIbEERSoT_},  // <<< IMPLEMENTAR
  // TODO {"_ZNSo9_M_insertIdEERSoT_", (uintptr_t)&stub__ZNSo9_M_insertIdEERSoT_},  // <<< IMPLEMENTAR
  // TODO {"_ZNSo9_M_insertIlEERSoT_", (uintptr_t)&stub__ZNSo9_M_insertIlEERSoT_},  // <<< IMPLEMENTAR
  // TODO {"_ZNSo9_M_insertImEERSoT_", (uintptr_t)&stub__ZNSo9_M_insertImEERSoT_},  // <<< IMPLEMENTAR
  // TODO {"_ZNSo9_M_insertIPKvEERSoT_", (uintptr_t)&stub__ZNSo9_M_insertIPKvEERSoT_},  // <<< IMPLEMENTAR
  // TODO {"_ZNSolsEi", (uintptr_t)&stub__ZNSolsEi},  // <<< IMPLEMENTAR
  // TODO {"_ZNSolsEm", (uintptr_t)&stub__ZNSolsEm},  // <<< IMPLEMENTAR
  // TODO {"_ZNSolsEPFRSt8ios_baseS0_E", (uintptr_t)&stub__ZNSolsEPFRSt8ios_baseS0_E},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs13_S_copy_charsEPcS_S_", (uintptr_t)&stub__ZNSs13_S_copy_charsEPcS_S_},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs2atEm", (uintptr_t)&stub__ZNSs2atEm},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs3endEv", (uintptr_t)&stub__ZNSs3endEv},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs4_Rep10_M_destroyERKSaIcE", (uintptr_t)&stub__ZNSs4_Rep10_M_destroyERKSaIcE},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs4_Rep20_S_empty_rep_storageE", (uintptr_t)&stub__ZNSs4_Rep20_S_empty_rep_storageE},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs4_Rep26_M_set_length_and_sharableEm", (uintptr_t)&stub__ZNSs4_Rep26_M_set_length_and_sharableEm},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs4_Rep9_S_createEmmRKSaIcE", (uintptr_t)&stub__ZNSs4_Rep9_S_createEmmRKSaIcE},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs4swapERSs", (uintptr_t)&stub__ZNSs4swapERSs},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs5beginEv", (uintptr_t)&stub__ZNSs5beginEv},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs5clearEv", (uintptr_t)&stub__ZNSs5clearEv},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs5eraseEmm", (uintptr_t)&stub__ZNSs5eraseEmm},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs6appendEmc", (uintptr_t)&stub__ZNSs6appendEmc},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs6appendEPKc", (uintptr_t)&stub__ZNSs6appendEPKc},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs6appendEPKcm", (uintptr_t)&stub__ZNSs6appendEPKcm},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs6appendERKSs", (uintptr_t)&stub__ZNSs6appendERKSs},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs6appendERKSsmm", (uintptr_t)&stub__ZNSs6appendERKSsmm},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs6assignEPKcm", (uintptr_t)&stub__ZNSs6assignEPKcm},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs6assignERKSs", (uintptr_t)&stub__ZNSs6assignERKSs},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs6insertEmmc", (uintptr_t)&stub__ZNSs6insertEmmc},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs6insertEmPKc", (uintptr_t)&stub__ZNSs6insertEmPKc},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs6resizeEm", (uintptr_t)&stub__ZNSs6resizeEm},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs7_M_leakEv", (uintptr_t)&stub__ZNSs7_M_leakEv},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs7replaceEN9__gnu_cxx17__normal_iteratorIPcSsEES2_NS0_IPKcSsEES5_", (uintptr_t)&stub__ZNSs7replaceEN9__gnu_cxx17__normal_iteratorIPcSsEES2_NS0_IPKcSsEES5_},  // <<< IMPLEMENTAR
  // TODO {"_ZNSs7reserveEm", (uintptr_t)&stub__ZNSs7reserveEm},  // <<< IMPLEMENTAR
  // TODO {"_ZNSsaSEPKc", (uintptr_t)&stub__ZNSsaSEPKc},  // <<< IMPLEMENTAR
  // TODO {"_ZNSsaSERKSs", (uintptr_t)&stub__ZNSsaSERKSs},  // <<< IMPLEMENTAR
  // TODO {"_ZNSsC1EPKcmRKSaIcE", (uintptr_t)&stub__ZNSsC1EPKcmRKSaIcE},  // <<< IMPLEMENTAR
  // TODO {"_ZNSsC1EPKcRKSaIcE", (uintptr_t)&stub__ZNSsC1EPKcRKSaIcE},  // <<< IMPLEMENTAR
  // TODO {"_ZNSsC1ERKSs", (uintptr_t)&stub__ZNSsC1ERKSs},  // <<< IMPLEMENTAR
  // TODO {"_ZNSsD1Ev", (uintptr_t)&stub__ZNSsD1Ev},  // <<< IMPLEMENTAR
  // TODO {"_ZNSsD2Ev", (uintptr_t)&stub__ZNSsD2Ev},  // <<< IMPLEMENTAR
  // TODO {"_ZNSspLEPKc", (uintptr_t)&stub__ZNSspLEPKc},  // <<< IMPLEMENTAR
  // TODO {"_ZNSspLERKSs", (uintptr_t)&stub__ZNSspLERKSs},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt12__basic_fileIcED1Ev", (uintptr_t)&stub__ZNSt12__basic_fileIcED1Ev},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt13basic_filebufIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode", (uintptr_t)&stub__ZNSt13basic_filebufIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt13basic_filebufIcSt11char_traitsIcEE5closeEv", (uintptr_t)&stub__ZNSt13basic_filebufIcSt11char_traitsIcEE5closeEv},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt13basic_filebufIcSt11char_traitsIcEEC1Ev", (uintptr_t)&stub__ZNSt13basic_filebufIcSt11char_traitsIcEEC1Ev},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode", (uintptr_t)&stub__ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEED1Ev", (uintptr_t)&stub__ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEED1Ev},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt18condition_variable10notify_allEv", (uintptr_t)&stub__ZNSt18condition_variable10notify_allEv},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt18condition_variable10notify_oneEv", (uintptr_t)&stub__ZNSt18condition_variable10notify_oneEv},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt18condition_variable4waitERSt11unique_lockISt5mutexE", (uintptr_t)&stub__ZNSt18condition_variable4waitERSt11unique_lockISt5mutexE},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt18condition_variableC1Ev", (uintptr_t)&stub__ZNSt18condition_variableC1Ev},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt18condition_variableC2Ev", (uintptr_t)&stub__ZNSt18condition_variableC2Ev},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt18condition_variableD1Ev", (uintptr_t)&stub__ZNSt18condition_variableD1Ev},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt18condition_variableD2Ev", (uintptr_t)&stub__ZNSt18condition_variableD2Ev},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode", (uintptr_t)&stub__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev", (uintptr_t)&stub__ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt6chrono3_V212steady_clock3nowEv", (uintptr_t)&stub__ZNSt6chrono3_V212steady_clock3nowEv},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt6chrono3_V212system_clock3nowEv", (uintptr_t)&stub__ZNSt6chrono3_V212system_clock3nowEv},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt6localeC1Ev", (uintptr_t)&stub__ZNSt6localeC1Ev},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt6localeD1Ev", (uintptr_t)&stub__ZNSt6localeD1Ev},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt6thread15_M_start_threadESt10shared_ptrINS_10_Impl_baseEE", (uintptr_t)&stub__ZNSt6thread15_M_start_threadESt10shared_ptrINS_10_Impl_baseEE},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt6thread6detachEv", (uintptr_t)&stub__ZNSt6thread6detachEv},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt8__detail15_List_node_base7_M_hookEPS0_", (uintptr_t)&stub__ZNSt8__detail15_List_node_base7_M_hookEPS0_},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt8__detail15_List_node_base9_M_unhookEv", (uintptr_t)&stub__ZNSt8__detail15_List_node_base9_M_unhookEv},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt8ios_base4InitC1Ev", (uintptr_t)&stub__ZNSt8ios_base4InitC1Ev},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt8ios_base4InitD1Ev", (uintptr_t)&stub__ZNSt8ios_base4InitD1Ev},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt8ios_baseC2Ev", (uintptr_t)&stub__ZNSt8ios_baseC2Ev},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt8ios_baseD2Ev", (uintptr_t)&stub__ZNSt8ios_baseD2Ev},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E", (uintptr_t)&stub__ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E},  // <<< IMPLEMENTAR
  // TODO {"_ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate", (uintptr_t)&stub__ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate},  // <<< IMPLEMENTAR
  // TODO {"_ZSt16__ostream_insertIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_PKS3_l", (uintptr_t)&stub__ZSt16__ostream_insertIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_PKS3_l},  // <<< IMPLEMENTAR
  // TODO {"_ZSt16__throw_bad_castv", (uintptr_t)&stub__ZSt16__throw_bad_castv},  // <<< IMPLEMENTAR
  // TODO {"_ZSt17__throw_bad_allocv", (uintptr_t)&stub__ZSt17__throw_bad_allocv},  // <<< IMPLEMENTAR
  // TODO {"_ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base", (uintptr_t)&stub__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base},  // <<< IMPLEMENTAR
  // TODO {"_ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base", (uintptr_t)&stub__ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base},  // <<< IMPLEMENTAR
  // TODO {"_ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base", (uintptr_t)&stub__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base},  // <<< IMPLEMENTAR
  // TODO {"_ZSt18uncaught_exceptionv", (uintptr_t)&stub__ZSt18uncaught_exceptionv},  // <<< IMPLEMENTAR
  // TODO {"_ZSt19__throw_logic_errorPKc", (uintptr_t)&stub__ZSt19__throw_logic_errorPKc},  // <<< IMPLEMENTAR
  // TODO {"_ZSt20__throw_length_errorPKc", (uintptr_t)&stub__ZSt20__throw_length_errorPKc},  // <<< IMPLEMENTAR
  // TODO {"_ZSt20__throw_system_errori", (uintptr_t)&stub__ZSt20__throw_system_errori},  // <<< IMPLEMENTAR
  // TODO {"_ZSt24__throw_out_of_range_fmtPKcz", (uintptr_t)&stub__ZSt24__throw_out_of_range_fmtPKcz},  // <<< IMPLEMENTAR
  // TODO {"_ZSt25__throw_bad_function_callv", (uintptr_t)&stub__ZSt25__throw_bad_function_callv},  // <<< IMPLEMENTAR
  // TODO {"_ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_", (uintptr_t)&stub__ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_},  // <<< IMPLEMENTAR
  // TODO {"_ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_", (uintptr_t)&stub__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_},  // <<< IMPLEMENTAR
  // TODO {"_ZSt4cerr", (uintptr_t)&stub__ZSt4cerr},  // <<< IMPLEMENTAR
  // TODO {"_ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_", (uintptr_t)&stub__ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_},  // <<< IMPLEMENTAR
  // TODO {"_ZSt9terminatev", (uintptr_t)&stub__ZSt9terminatev},  // <<< IMPLEMENTAR
  // TODO {"_ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St5_Setw", (uintptr_t)&stub__ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St5_Setw},  // <<< IMPLEMENTAR
  // TODO {"_ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St8_SetfillIS3_E", (uintptr_t)&stub__ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St8_SetfillIS3_E},  // <<< IMPLEMENTAR
  // TODO {"_ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKSbIS4_S5_T1_E", (uintptr_t)&stub__ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKSbIS4_S5_T1_E},  // <<< IMPLEMENTAR
  // TODO {"_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc", (uintptr_t)&stub__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc},  // <<< IMPLEMENTAR
  // TODO {"_ZTHN5Trace7Private12GWriteBufferE", (uintptr_t)&stub__ZTHN5Trace7Private12GWriteBufferE},  // <<< IMPLEMENTAR
  // TODO {"_ZTIN10__cxxabiv115__forced_unwindE", (uintptr_t)&stub__ZTIN10__cxxabiv115__forced_unwindE},  // <<< IMPLEMENTAR
  // TODO {"_ZTTSt14basic_ofstreamIcSt11char_traitsIcEE", (uintptr_t)&stub__ZTTSt14basic_ofstreamIcSt11char_traitsIcEE},  // <<< IMPLEMENTAR
  // TODO {"_ZTTSt18basic_stringstreamIcSt11char_traitsIcESaIcEE", (uintptr_t)&stub__ZTTSt18basic_stringstreamIcSt11char_traitsIcESaIcEE},  // <<< IMPLEMENTAR
  // TODO {"_ZTVN10__cxxabiv117__class_type_infoE", (uintptr_t)&stub__ZTVN10__cxxabiv117__class_type_infoE},  // <<< IMPLEMENTAR
  // TODO {"_ZTVN10__cxxabiv119__pointer_type_infoE", (uintptr_t)&stub__ZTVN10__cxxabiv119__pointer_type_infoE},  // <<< IMPLEMENTAR
  // TODO {"_ZTVN10__cxxabiv120__function_type_infoE", (uintptr_t)&stub__ZTVN10__cxxabiv120__function_type_infoE},  // <<< IMPLEMENTAR
  // TODO {"_ZTVN10__cxxabiv120__si_class_type_infoE", (uintptr_t)&stub__ZTVN10__cxxabiv120__si_class_type_infoE},  // <<< IMPLEMENTAR
  // TODO {"_ZTVN10__cxxabiv121__vmi_class_type_infoE", (uintptr_t)&stub__ZTVN10__cxxabiv121__vmi_class_type_infoE},  // <<< IMPLEMENTAR
  // TODO {"_ZTVSt13basic_filebufIcSt11char_traitsIcEE", (uintptr_t)&stub__ZTVSt13basic_filebufIcSt11char_traitsIcEE},  // <<< IMPLEMENTAR
  // TODO {"_ZTVSt14basic_ofstreamIcSt11char_traitsIcEE", (uintptr_t)&stub__ZTVSt14basic_ofstreamIcSt11char_traitsIcEE},  // <<< IMPLEMENTAR
  // TODO {"_ZTVSt15basic_streambufIcSt11char_traitsIcEE", (uintptr_t)&stub__ZTVSt15basic_streambufIcSt11char_traitsIcEE},  // <<< IMPLEMENTAR
  // TODO {"_ZTVSt15basic_stringbufIcSt11char_traitsIcESaIcEE", (uintptr_t)&stub__ZTVSt15basic_stringbufIcSt11char_traitsIcESaIcEE},  // <<< IMPLEMENTAR
  // TODO {"_ZTVSt18basic_stringstreamIcSt11char_traitsIcESaIcEE", (uintptr_t)&stub__ZTVSt18basic_stringstreamIcSt11char_traitsIcESaIcEE},  // <<< IMPLEMENTAR
  // TODO {"_ZTVSt9basic_iosIcSt11char_traitsIcEE", (uintptr_t)&stub__ZTVSt9basic_iosIcSt11char_traitsIcEE},  // <<< IMPLEMENTAR
  // ==== Oculus VR API stubs (no-op) ====
  {"ovrp_BeginFrame4", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_CalculateEyeLayerDesc2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_CalculateEyeViewportRect", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_CalculateLayerDesc", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_CloseCameraDevice", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_DestroyDistortionWindow2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_DestroyLayer", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_DestroyMirrorTexture2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_DoesCameraDeviceSupportDepth", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_EndFrame4", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetAdaptiveGpuPerformanceScale2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetAppFramerate2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetAppHasInputFocus", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetAppHasVrFocus2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetAppLatencyTimings2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetAppShouldQuit2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetAppShouldRecenter2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetAppShouldRecreateDistortionWindow2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetBoundaryConfigured2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetBoundaryDimensions2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetBoundaryGeometry3", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetBoundaryVisible2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetCameraDeviceColorFrameBgraPixels", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetCameraDeviceColorFrameSize", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetCameraDeviceDepthFramePixels", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetCameraDeviceDepthFrameSize", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetControllerHapticsDesc2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetControllerHapticsState2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetControllerState4", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetDeviceExtensionsVk", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetExternalCameraCalibrationRawPose", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetExternalCameraCount", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetExternalCameraExtrinsics", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetExternalCameraIntrinsics", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetExternalCameraName", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetGPUFrameTime", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetGPUUtilLevel", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetGPUUtilSupported", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetInitialized", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetInstanceExtensionsVk", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetLayerTexture2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetLayerTextureFoveation", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetLayerTextureStageCount", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetMixedRealityInitialized", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetNodeFrustum2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetNodeOrientationTracked2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetNodeOrientationValid", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetNodePoseState3", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetNodePositionTracked2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetNodePositionValid", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetNodePresent2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetPerfMetricsFloat", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetPerfMetricsInt", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetSystemDisplayAvailableFrequencies", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetSystemDisplayFrequency2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetSystemHeadsetType2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetSystemMultiViewSupported2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetSystemProductName2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetSystemRecommendedMSAALevel2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetTiledMultiResLevel", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetTrackingOriginType2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetTrackingPositionSupported2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetTrackingTransformRawPose", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetUserEyeHeight2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetUserIPD2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetUserNeckEyeDistance2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetUserPresent2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetVersion2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_GetViewportStencil", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_Initialize5", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_InitializeMixedReality", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_IsCameraDeviceColorFrameAvailable2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_IsCameraDeviceDepthFrameAvailable", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_IsPerfMetricsSupported", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_Media_EncodeMrcFrameWithDualTextures", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_Media_GetInitialized", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_Media_GetMrcFrameSize", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_Media_Initialize", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_Media_IsMrcActivated", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_Media_IsMrcEnabled", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_Media_SetMrcActivationMode", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_Media_SetMrcAudioSampleRate", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_Media_SetMrcFrameImageFlipped", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_Media_SetMrcFrameInverseAlpha", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_Media_SetMrcInputVideoBufferType", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_Media_Shutdown", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_Media_SyncMrcFrame", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_Media_Update", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_PreInitialize3", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_RecenterTrackingOrigin2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetAppCPUPriority2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetAppEngineInfo2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetBoundaryVisible2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetControllerHaptics2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetControllerVibration2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetReorientHMDOnControllerRecenter", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetSystemCpuLevel2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetSystemDisplayFrequency", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetSystemGpuLevel2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetTiledMultiResLevel", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetTrackingOrientationEnabled2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetTrackingOriginType2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetTrackingPositionEnabled2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetUserIPD2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetupDisplayObjects2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetupDistortionWindow3", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetupLayer", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_SetupMirrorTexture2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_ShowSystemUI2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_Shutdown2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_ShutdownMixedReality", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_TestBoundaryNode2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_TestBoundaryPoint2", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_Update3", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_UpdateCameraDevices", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_UpdateExternalCamera", (uintptr_t)&nier_ret0},  // ovrp stub
  {"ovrp_WaitToBeginFrame", (uintptr_t)&nier_ret0},  // ovrp stub
  // ==== Android NDK (android_shim) ====
  {"AConfiguration_delete", (uintptr_t)&AConfiguration_delete},  // android_shim
  {"AConfiguration_fromAssetManager", (uintptr_t)&AConfiguration_fromAssetManager},  // android_shim
  {"AConfiguration_getCountry", (uintptr_t)&AConfiguration_getCountry},  // android_shim
  {"AConfiguration_getLanguage", (uintptr_t)&AConfiguration_getLanguage},  // android_shim
  {"AConfiguration_new", (uintptr_t)&AConfiguration_new},  // android_shim
  {"AInputEvent_getSource", (uintptr_t)&AInputEvent_getSource},  // android_shim
  {"AInputEvent_getType", (uintptr_t)&AInputEvent_getType},  // android_shim
  {"AInputQueue_attachLooper", (uintptr_t)&AInputQueue_attachLooper},  // android_shim
  {"AInputQueue_detachLooper", (uintptr_t)&AInputQueue_detachLooper},  // android_shim
  {"AInputQueue_finishEvent", (uintptr_t)&AInputQueue_finishEvent},  // android_shim
  {"AInputQueue_getEvent", (uintptr_t)&AInputQueue_getEvent},  // android_shim
  {"AInputQueue_preDispatchEvent", (uintptr_t)&AInputQueue_preDispatchEvent},  // android_shim
  {"AKeyEvent_getAction", (uintptr_t)&AKeyEvent_getAction},  // android_shim
  {"AKeyEvent_getKeyCode", (uintptr_t)&AKeyEvent_getKeyCode},  // android_shim
  {"ALooper_addFd", (uintptr_t)&ALooper_addFd},  // android_shim
  {"ALooper_pollAll", (uintptr_t)&ALooper_pollAll},  // android_shim
  {"ALooper_prepare", (uintptr_t)&ALooper_prepare},  // android_shim
  {"AMotionEvent_getAction", (uintptr_t)&AMotionEvent_getAction},  // android_shim
  {"AMotionEvent_getPointerCount", (uintptr_t)&AMotionEvent_getPointerCount},  // android_shim
  {"AMotionEvent_getPointerId", (uintptr_t)&AMotionEvent_getPointerId},  // android_shim
  {"AMotionEvent_getX", (uintptr_t)&AMotionEvent_getX},  // android_shim
  {"AMotionEvent_getY", (uintptr_t)&AMotionEvent_getY},  // android_shim
  {"AAsset_close", (uintptr_t)&nier_ret0},  // android stub
  {"AAssetDir_close", (uintptr_t)&nier_ret0},  // android stub
  {"AAssetDir_getNextFileName", (uintptr_t)&nier_ret0},  // android stub
  {"AAsset_getBuffer", (uintptr_t)&nier_ret0},  // android stub
  {"AAsset_getLength", (uintptr_t)&nier_ret0},  // android stub
  {"AAssetManager_fromJava", (uintptr_t)&nier_ret0},  // android stub
  {"AAssetManager_open", (uintptr_t)&nier_ret0},  // android stub
  {"AAssetManager_openDir", (uintptr_t)&nier_ret0},  // android stub
  {"AAsset_openFileDescriptor", (uintptr_t)&nier_ret0},  // android stub
  {"AInputEvent_getDeviceId", (uintptr_t)&nier_ret0},  // android stub
  {"AKeyEvent_getFlags", (uintptr_t)&nier_ret0},  // android stub
  {"AKeyEvent_getMetaState", (uintptr_t)&nier_ret0},  // android stub
  {"AMotionEvent_getButtonState", (uintptr_t)&nier_ret0},  // android stub
  {"ANativeActivity_setWindowFormat", (uintptr_t)&nier_ret0},  // android stub
  {"ANativeWindow_acquire", (uintptr_t)&nier_ANW_acquire},  // android stub
  {"ANativeWindow_getHeight", (uintptr_t)&nier_ANW_getHeight},  // android stub
  {"ANativeWindow_getWidth", (uintptr_t)&nier_ANW_getWidth},  // android stub
  {"ANativeWindow_release", (uintptr_t)&nier_ANW_release},  // android stub
  {"ANativeWindow_setBuffersGeometry", (uintptr_t)&nier_ANW_setBuffersGeometry},  // android stub
  // ---- bionic internos + TLS ----
  {"mmap", (uintptr_t)&nier_mmap},
  {"munmap", (uintptr_t)&nier_munmap},
  {"open", (uintptr_t)&nier_open},
  {"openat", (uintptr_t)&nier_openat},
  {"fopen", (uintptr_t)&nier_fopen},
  {"access", (uintptr_t)&nier_access},
  {"opendir", (uintptr_t)&nier_opendir},
  {"stat", (uintptr_t)&nier_stat},
  {"lstat", (uintptr_t)&nier_lstat},
  {"__xstat", (uintptr_t)&nier_xstat},
  {"sysconf", (uintptr_t)&nier_sysconf},
  {"pthread_create", (uintptr_t)&nier_pthread_create},
  {"syscall", (uintptr_t)&nier_syscall},
  {"sigaction", (uintptr_t)&nier_sigaction},
  {"__cxa_guard_acquire", (uintptr_t)&nier_cxa_guard_acquire},
  {"__cxa_guard_release", (uintptr_t)&nier_cxa_guard_release},
  {"__cxa_guard_abort", (uintptr_t)&nier_cxa_guard_abort},
  {"__errno", (uintptr_t)&nier_errno},
  {"__isfinitef", (uintptr_t)&nier_isfinitef},
  {"__sF", (uintptr_t)&nier_sF},
  {"_ctype_", (uintptr_t)&nier_ctype},
  {"__google_potentially_blocking_region_begin", (uintptr_t)&nier_gpbr_begin},
  {"__google_potentially_blocking_region_end", (uintptr_t)&nier_gpbr_end},
  {"__assert2", (uintptr_t)&nier_assert2},
  {"__system_property_get", (uintptr_t)&nier_sysprop_get},
  {"_ZTHN5Trace7Private12GWriteBufferE", (uintptr_t)&nier_ret0},
};
const int dynlib_functions_count = sizeof(dynlib_functions)/sizeof(dynlib_functions[0]);

// ===================== SIMBOLOS A IMPLEMENTAR =====================
//   accept
//   acosf
//   __addtf3
//   asinf
//   atanf
//   basename
//   bind
//   chmod
//   clock_nanosleep
//   compressBound
//   connect
//   deflateReset
//   div
//   dladdr
//   dlclose
//   dlerror
//   dlopen
//   dlsym
//   __dynamic_cast
//   execl
//   exp2f
//   fdatasync
//   feof
//   ferror
//   fesetround
//   fork
//   freeaddrinfo
//   fsync
//   ftruncate
//   getaddrinfo
//   getegid
//   geteuid
//   getgid
//   gethostbyaddr
//   gethostbyname
//   gethostname
//   getnameinfo
//   getpeername
//   getpwuid
//   getrlimit
//   getsockname
//   getsockopt
//   getuid
//   gmtime_r
//   if_nametoindex
//   inet_addr
//   inet_ntop
//   inet_pton
//   inflateReset
//   __isfinitef
//   isgraph
//   isprint
//   iswalnum
//   iswalpha
//   iswdigit
//   iswlower
//   iswpunct
//   iswupper
//   iswxdigit
//   isxdigit
//   kill
//   ldexpf
//   listen
//   localtime_r
//   log10f
//   longjmp
//   lrint
//   lrintf
//   lseek64
//   memrchr
//   modff
//   __multf3
//   ovrp_BeginFrame4
//   ovrp_CalculateEyeLayerDesc2
//   ovrp_CalculateEyeViewportRect
//   ovrp_CalculateLayerDesc
//   ovrp_CloseCameraDevice
//   ovrp_DestroyDistortionWindow2
//   ovrp_DestroyLayer
//   ovrp_DestroyMirrorTexture2
//   ovrp_DoesCameraDeviceSupportDepth
//   ovrp_EndFrame4
//   ovrp_GetAdaptiveGpuPerformanceScale2
//   ovrp_GetAppFramerate2
//   ovrp_GetAppHasInputFocus
//   ovrp_GetAppHasVrFocus2
//   ovrp_GetAppLatencyTimings2
//   ovrp_GetAppShouldQuit2
//   ovrp_GetAppShouldRecenter2
//   ovrp_GetAppShouldRecreateDistortionWindow2
//   ovrp_GetBoundaryConfigured2
//   ovrp_GetBoundaryDimensions2
//   ovrp_GetBoundaryGeometry3
//   ovrp_GetBoundaryVisible2
//   ovrp_GetCameraDeviceColorFrameBgraPixels
//   ovrp_GetCameraDeviceColorFrameSize
//   ovrp_GetCameraDeviceDepthFramePixels
//   ovrp_GetCameraDeviceDepthFrameSize
//   ovrp_GetControllerHapticsDesc2
//   ovrp_GetControllerHapticsState2
//   ovrp_GetControllerState4
//   ovrp_GetDeviceExtensionsVk
//   ovrp_GetExternalCameraCalibrationRawPose
//   ovrp_GetExternalCameraCount
//   ovrp_GetExternalCameraExtrinsics
//   ovrp_GetExternalCameraIntrinsics
//   ovrp_GetExternalCameraName
//   ovrp_GetGPUFrameTime
//   ovrp_GetGPUUtilLevel
//   ovrp_GetGPUUtilSupported
//   ovrp_GetInitialized
//   ovrp_GetInstanceExtensionsVk
//   ovrp_GetLayerTexture2
//   ovrp_GetLayerTextureFoveation
//   ovrp_GetLayerTextureStageCount
//   ovrp_GetMixedRealityInitialized
//   ovrp_GetNodeFrustum2
//   ovrp_GetNodeOrientationTracked2
//   ovrp_GetNodeOrientationValid
//   ovrp_GetNodePoseState3
//   ovrp_GetNodePositionTracked2
//   ovrp_GetNodePositionValid
//   ovrp_GetNodePresent2
//   ovrp_GetPerfMetricsFloat
//   ovrp_GetPerfMetricsInt
//   ovrp_GetSystemDisplayAvailableFrequencies
//   ovrp_GetSystemDisplayFrequency2
//   ovrp_GetSystemHeadsetType2
//   ovrp_GetSystemMultiViewSupported2
//   ovrp_GetSystemProductName2
//   ovrp_GetSystemRecommendedMSAALevel2
//   ovrp_GetTiledMultiResLevel
//   ovrp_GetTrackingOriginType2
//   ovrp_GetTrackingPositionSupported2
//   ovrp_GetTrackingTransformRawPose
//   ovrp_GetUserEyeHeight2
//   ovrp_GetUserIPD2
//   ovrp_GetUserNeckEyeDistance2
//   ovrp_GetUserPresent2
//   ovrp_GetVersion2
//   ovrp_GetViewportStencil
//   ovrp_Initialize5
//   ovrp_InitializeMixedReality
//   ovrp_IsCameraDeviceColorFrameAvailable2
//   ovrp_IsCameraDeviceDepthFrameAvailable
//   ovrp_IsPerfMetricsSupported
//   ovrp_Media_EncodeMrcFrameWithDualTextures
//   ovrp_Media_GetInitialized
//   ovrp_Media_GetMrcFrameSize
//   ovrp_Media_Initialize
//   ovrp_Media_IsMrcActivated
//   ovrp_Media_IsMrcEnabled
//   ovrp_Media_SetMrcActivationMode
//   ovrp_Media_SetMrcAudioSampleRate
//   ovrp_Media_SetMrcFrameImageFlipped
//   ovrp_Media_SetMrcFrameInverseAlpha
//   ovrp_Media_SetMrcInputVideoBufferType
//   ovrp_Media_Shutdown
//   ovrp_Media_SyncMrcFrame
//   ovrp_Media_Update
//   ovrp_PreInitialize3
//   ovrp_RecenterTrackingOrigin2
//   ovrp_SetAppCPUPriority2
//   ovrp_SetAppEngineInfo2
//   ovrp_SetBoundaryVisible2
//   ovrp_SetControllerHaptics2
//   ovrp_SetControllerVibration2
//   ovrp_SetReorientHMDOnControllerRecenter
//   ovrp_SetSystemCpuLevel2
//   ovrp_SetSystemDisplayFrequency
//   ovrp_SetSystemGpuLevel2
//   ovrp_SetTiledMultiResLevel
//   ovrp_SetTrackingOrientationEnabled2
//   ovrp_SetTrackingOriginType2
//   ovrp_SetTrackingPositionEnabled2
//   ovrp_SetupDisplayObjects2
//   ovrp_SetupDistortionWindow3
//   ovrp_SetupLayer
//   ovrp_SetupMirrorTexture2
//   ovrp_SetUserIPD2
//   ovrp_ShowSystemUI2
//   ovrp_Shutdown2
//   ovrp_ShutdownMixedReality
//   ovrp_TestBoundaryNode2
//   ovrp_TestBoundaryPoint2
//   ovrp_Update3
//   ovrp_UpdateCameraDevices
//   ovrp_UpdateExternalCamera
//   ovrp_WaitToBeginFrame
//   pause
//   poll
//   pread64
//   putchar
//   pwrite64
//   recv
//   recvfrom
//   sched_get_priority_max
//   sched_get_priority_min
//   sched_getscheduler
//   select
//   send
//   sendto
//   setjmp
//   setrlimit
//   setsockopt
//   __sF
//   __sfp_handle_exceptions
//   shutdown
//   sigaction
//   sigemptyset
//   signal
//   sinhf
//   socket
//   socketpair
//   statfs
//   strerror_r
//   strpbrk
//   strtok_r
//   strtoll
//   strtoull
//   syscall
//   sysinfo
//   __system_property_get
//   tcgetattr
//   tcsetattr
//   timezone
//   towlower
//   towupper
//   __trunctfdf2
//   tzname
//   tzset
//   _Unwind_Backtrace
//   _Unwind_GetIP
//   _Unwind_Resume
//   vasprintf
//   vfprintf
//   waitpid
//   wcschr
//   zlibVersion
//   _ZNKSs13find_first_ofEPKcm
//   _ZNKSs17find_first_not_ofEPKcm
//   _ZNKSs3endEv
//   _ZNKSs4findEcm
//   _ZNKSs4findEPKcmm
//   _ZNKSs4findERKSsm
//   _ZNKSs6substrEmm
//   _ZNKSs7compareEPKc
//   _ZNKSs7compareERKSs
//   _ZNKSs7_M_iendEv
//   _ZNKSt15basic_stringbufIcSt11char_traitsIcESaIcEE3strEv
//   _ZNKSt5ctypeIcE13_M_widen_initEv
//   _ZNKSt8__detail20_Prime_rehash_policy11_M_next_bktEm
//   _ZNKSt8__detail20_Prime_rehash_policy14_M_need_rehashEmmm
//   _ZNKSt9basic_iosIcSt11char_traitsIcEE4fillEv
//   _ZNSi4readEPcl
//   _ZNSo3putEc
//   _ZNSo5flushEv
//   _ZNSo5writeEPKcl
//   _ZNSo9_M_insertIbEERSoT_
//   _ZNSo9_M_insertIdEERSoT_
//   _ZNSo9_M_insertIlEERSoT_
//   _ZNSo9_M_insertImEERSoT_
//   _ZNSo9_M_insertIPKvEERSoT_
//   _ZNSolsEi
//   _ZNSolsEm
//   _ZNSolsEPFRSt8ios_baseS0_E
//   _ZNSs13_S_copy_charsEPcS_S_
//   _ZNSs2atEm
//   _ZNSs3endEv
//   _ZNSs4_Rep10_M_destroyERKSaIcE
//   _ZNSs4_Rep20_S_empty_rep_storageE
//   _ZNSs4_Rep26_M_set_length_and_sharableEm
//   _ZNSs4_Rep9_S_createEmmRKSaIcE
//   _ZNSs4swapERSs
//   _ZNSs5beginEv
//   _ZNSs5clearEv
//   _ZNSs5eraseEmm
//   _ZNSs6appendEmc
//   _ZNSs6appendEPKc
//   _ZNSs6appendEPKcm
//   _ZNSs6appendERKSs
//   _ZNSs6appendERKSsmm
//   _ZNSs6assignEPKcm
//   _ZNSs6assignERKSs
//   _ZNSs6insertEmmc
//   _ZNSs6insertEmPKc
//   _ZNSs6resizeEm
//   _ZNSs7_M_leakEv
//   _ZNSs7replaceEN9__gnu_cxx17__normal_iteratorIPcSsEES2_NS0_IPKcSsEES5_
//   _ZNSs7reserveEm
//   _ZNSsaSEPKc
//   _ZNSsaSERKSs
//   _ZNSsC1EPKcmRKSaIcE
//   _ZNSsC1EPKcRKSaIcE
//   _ZNSsC1ERKSs
//   _ZNSsD1Ev
//   _ZNSsD2Ev
//   _ZNSspLEPKc
//   _ZNSspLERKSs
//   _ZNSt12__basic_fileIcED1Ev
//   _ZNSt13basic_filebufIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode
//   _ZNSt13basic_filebufIcSt11char_traitsIcEE5closeEv
//   _ZNSt13basic_filebufIcSt11char_traitsIcEEC1Ev
//   _ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode
//   _ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEED1Ev
//   _ZNSt18condition_variable10notify_allEv
//   _ZNSt18condition_variable10notify_oneEv
//   _ZNSt18condition_variable4waitERSt11unique_lockISt5mutexE
//   _ZNSt18condition_variableC1Ev
//   _ZNSt18condition_variableC2Ev
//   _ZNSt18condition_variableD1Ev
//   _ZNSt18condition_variableD2Ev
//   _ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode
//   _ZNSt19basic_ostringstreamIcSt11char_traitsIcESaIcEED1Ev
//   _ZNSt6chrono3_V212steady_clock3nowEv
//   _ZNSt6chrono3_V212system_clock3nowEv
//   _ZNSt6localeC1Ev
//   _ZNSt6localeD1Ev
//   _ZNSt6thread15_M_start_threadESt10shared_ptrINS_10_Impl_baseEE
//   _ZNSt6thread6detachEv
//   _ZNSt8__detail15_List_node_base7_M_hookEPS0_
//   _ZNSt8__detail15_List_node_base9_M_unhookEv
//   _ZNSt8ios_base4InitC1Ev
//   _ZNSt8ios_base4InitD1Ev
//   _ZNSt8ios_baseC2Ev
//   _ZNSt8ios_baseD2Ev
//   _ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E
//   _ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate
//   _ZSt16__ostream_insertIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_PKS3_l
//   _ZSt16__throw_bad_castv
//   _ZSt17__throw_bad_allocv
//   _ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base
//   _ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base
//   _ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base
//   _ZSt18uncaught_exceptionv
//   _ZSt19__throw_logic_errorPKc
//   _ZSt20__throw_length_errorPKc
//   _ZSt20__throw_system_errori
//   _ZSt24__throw_out_of_range_fmtPKcz
//   _ZSt25__throw_bad_function_callv
//   _ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_
//   _ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_
//   _ZSt4cerr
//   _ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_
//   _ZSt9terminatev
//   _ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St5_Setw
//   _ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_St8_SetfillIS3_E
//   _ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKSbIS4_S5_T1_E
//   _ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc
//   _ZTHN5Trace7Private12GWriteBufferE
//   _ZTIN10__cxxabiv115__forced_unwindE
//   _ZTTSt14basic_ofstreamIcSt11char_traitsIcEE
//   _ZTTSt18basic_stringstreamIcSt11char_traitsIcESaIcEE
//   _ZTVN10__cxxabiv117__class_type_infoE
//   _ZTVN10__cxxabiv119__pointer_type_infoE
//   _ZTVN10__cxxabiv120__function_type_infoE
//   _ZTVN10__cxxabiv120__si_class_type_infoE
//   _ZTVN10__cxxabiv121__vmi_class_type_infoE
//   _ZTVSt13basic_filebufIcSt11char_traitsIcEE
//   _ZTVSt14basic_ofstreamIcSt11char_traitsIcEE
//   _ZTVSt15basic_streambufIcSt11char_traitsIcEE
//   _ZTVSt15basic_stringbufIcSt11char_traitsIcESaIcEE
//   _ZTVSt18basic_stringstreamIcSt11char_traitsIcESaIcEE
//   _ZTVSt9basic_iosIcSt11char_traitsIcEE

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(dynlib_functions[0]);
