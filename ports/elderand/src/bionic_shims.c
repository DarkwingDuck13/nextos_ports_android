/* bionic_shims.c — shims bionic p/ o so-loader (F1): FORTIFY _chk -> glibc unchecked,
 * __sF (stdio bionic), __system_property_get, ZSTD trace, android_set_abort_message.
 * Exportados (build c/ -rdynamic) -> o fallback dlsym(RTLD_DEFAULT) do so_resolve acha. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/select.h>

/* ---- FORTIFY _chk: ignoram o arg de tamanho, chamam a versão glibc ---- */
size_t __strlen_chk(const char *s, size_t n){ (void)n; return strlen(s); }
char  *__strchr_chk(const char *s, int c, size_t n){ (void)n; return strchr(s, c); }
char  *__strcpy_chk(char *d, const char *s, size_t n){ (void)n; return strcpy(d, s); }
char  *__strncpy_chk(char *d, const char *s, size_t n, size_t dn){ (void)dn; return strncpy(d, s, n); }
char  *__strncpy_chk2(char *d, const char *s, size_t n, size_t dn, size_t sn){ (void)dn; (void)sn; return strncpy(d, s, n); }
char  *__strcat_chk(char *d, const char *s, size_t dn){ (void)dn; return strcat(d, s); }
char  *__strncat_chk(char *d, const char *s, size_t n, size_t dn){ (void)dn; return strncat(d, s, n); }
void  *__memcpy_chk(void *d, const void *s, size_t n, size_t dn){ (void)dn; return memcpy(d, s, n); }
void  *__memmove_chk(void *d, const void *s, size_t n, size_t dn){ (void)dn; return memmove(d, s, n); }
void  *__memset_chk(void *d, int c, size_t n, size_t dn){ (void)dn; return memset(d, c, n); }
ssize_t __write_chk(int fd, const void *b, size_t n, size_t bn){ (void)bn; return write(fd, b, n); }
ssize_t __read_chk(int fd, void *b, size_t n, size_t bn){ (void)bn; return read(fd, b, n); }
ssize_t __sendto_chk(int fd, const void *b, size_t n, size_t bn, int fl, const struct sockaddr *a, socklen_t al){ (void)bn; return sendto(fd, b, n, fl, a, al); }
void __FD_SET_chk(int fd, void *s, size_t n){ (void)n; FD_SET(fd, (fd_set*)s); }
int  __FD_ISSET_chk(int fd, void *s, size_t n){ (void)n; return FD_ISSET(fd, (fd_set*)s); }
void __FD_CLR_chk(int fd, void *s, size_t n){ (void)n; FD_CLR(fd, (fd_set*)s); }
int __vsnprintf_chk(char *d, size_t n, int fl, size_t dn, const char *f, va_list ap){ (void)fl; (void)dn; return vsnprintf(d, n, f, ap); }
int __snprintf_chk(char *d, size_t n, int fl, size_t dn, const char *f, ...){ va_list ap; va_start(ap,f); int r=vsnprintf(d,n,f,ap); va_end(ap); return r; }
int __vsprintf_chk(char *d, int fl, size_t dn, const char *f, va_list ap){ (void)fl; (void)dn; return vsprintf(d, f, ap); }
int __sprintf_chk(char *d, int fl, size_t dn, const char *f, ...){ va_list ap; va_start(ap,f); int r=vsprintf(d,f,ap); va_end(ap); return r; }

/* ---- Android system properties FALSAS (pairip pode gate decriptação/anti-tamper nelas;
 * antes retornavam vazio = "ambiente não-Android" → caminho-tamper). Valores de um device
 * Android real plausível (arm64). ---- */
struct eld_prop { const char *name; const char *val; };
static const struct eld_prop g_props[] = {
  {"ro.build.version.sdk", "30"},
  {"ro.build.version.release", "11"},
  {"ro.product.cpu.abi", "arm64-v8a"},
  {"ro.product.cpu.abilist", "arm64-v8a,armeabi-v7a,armeabi"},
  {"ro.product.model", "SM-G991B"},
  {"ro.product.brand", "samsung"},
  {"ro.product.manufacturer", "samsung"},
  {"ro.product.device", "o1s"},
  {"ro.product.name", "o1sxxx"},
  {"ro.build.fingerprint", "samsung/o1sxxx/o1s:11/RP1A.200720.012/G991BXXU3AUF1:user/release-keys"},
  {"ro.build.tags", "release-keys"},
  {"ro.build.type", "user"},
  {"ro.debuggable", "0"},
  {"ro.secure", "1"},
  {"ro.hardware", "exynos2100"},
  {"ro.boot.hardware", "exynos2100"},
  {"ro.kernel.qemu", "0"},
  {0, 0},
};
static const struct eld_prop *eld_prop_find(const char *name){
  if(!name) return 0;
  for(const struct eld_prop *p=g_props; p->name; p++) if(!strcmp(p->name,name)) return p;
  return 0;
}
int __system_property_get(const char *name, char *value){
  const struct eld_prop *p=eld_prop_find(name);
  if(!value) return 0;
  if(p){ size_t n=strlen(p->val); if(n>91) n=91; memcpy(value,p->val,n); value[n]=0; return (int)n; }
  value[0]=0; return 0;
}
/* find/read (API nova): find devolve handle=ponteiro p/ entry; read copia name+value. */
const void *__system_property_find(const char *name){ return (const void*)eld_prop_find(name); }
int __system_property_read(const void *pi, char *name, char *value){
  const struct eld_prop *p=(const struct eld_prop*)pi;
  if(!p){ if(name)name[0]=0; if(value)value[0]=0; return 0; }
  if(name) strcpy(name, p->name);
  if(value){ size_t n=strlen(p->val); if(n>91)n=91; memcpy(value,p->val,n); value[n]=0; return (int)n; }
  return 0;
}
/* read_callback (bionic novo): chama cb(cookie, name, value, serial) */
void __system_property_read_callback(const void *pi,
    void (*cb)(void*, const char*, const char*, unsigned), void *cookie){
  const struct eld_prop *p=(const struct eld_prop*)pi;
  if(p && cb) cb(cookie, p->name, p->val, 0);
}
void android_set_abort_message(const char *m){ fprintf(stderr, "[abort] %s\n", m?m:""); }
/* __errno (bionic) = ponteiro p/ o errno da thread. O stub devolvia 0 (NULL) → qualquer
 * deref de errno do pairip ia p/ NULL. Devolve o errno real (glibc __errno_location). */
extern int *__errno_location(void);
int *__errno(void){ return __errno_location(); }

/* ---- ZSTD trace hooks (opcionais): no-op ---- */
unsigned long long ZSTD_trace_compress_begin(void *cctx){ (void)cctx; return 0; }
void ZSTD_trace_compress_end(unsigned long long id, const void *t){ (void)id; (void)t; }
unsigned long long ZSTD_trace_decompress_begin(void *dctx){ (void)dctx; return 0; }
void ZSTD_trace_decompress_end(unsigned long long id, const void *t){ (void)id; (void)t; }

/* ---- __sF: stdio bionic. __sF[0..2] = stdin/stdout/stderr. FILE bionic arm64
 * = 152B (stride): __sF+0=stdin, +0x98=stdout, +0x130=stderr. O libunity passa
 * &__sF[i] p/ fputc/fwrite/... glibc -> map_sf traduz p/ o FILE* glibc real. ---- */
char __sF[3 * 512];
#define SF_STRIDE 152
static FILE *map_sf(void *p) {
  uintptr_t d = (uintptr_t)p - (uintptr_t)__sF;
  if (d < sizeof(__sF)) {
    unsigned idx = (unsigned)(d / SF_STRIDE);
    return idx == 0 ? stdin : idx == 1 ? stdout : stderr;
  }
  return (FILE *)p;
}
/* wrappers stdio: mapeiam o FILE* (pode ser &__sF[i] bionic) p/ glibc real. */
int   sf_fputc(int c, void *f){ return fputc(c, map_sf(f)); }
int   sf_putc(int c, void *f){ return fputc(c, map_sf(f)); }
int   sf_fputs(const char *s, void *f){ return fputs(s, map_sf(f)); }
int   sf_puts(const char *s){ return puts(s); }
size_t sf_fwrite(const void *p, size_t a, size_t b, void *f){ return fwrite(p, a, b, map_sf(f)); }
size_t sf_fread(void *p, size_t a, size_t b, void *f){ return fread(p, a, b, map_sf(f)); }
int   sf_fflush(void *f){ return fflush(f ? map_sf(f) : NULL); }
int   sf_vfprintf(void *f, const char *fmt, va_list ap){ return vfprintf(map_sf(f), fmt, ap); }
int   sf_fprintf(void *f, const char *fmt, ...){ va_list ap; va_start(ap,fmt); int r=vfprintf(map_sf(f),fmt,ap); va_end(ap); return r; }
int   sf_fileno(void *f){ return fileno(map_sf(f)); }
int   sf_ferror(void *f){ return ferror(map_sf(f)); }
int   sf_feof(void *f){ return feof(map_sf(f)); }
void  sf_clearerr(void *f){ clearerr(map_sf(f)); }
int   sf_setvbuf(void *f, char *b, int m, size_t s){ return setvbuf(map_sf(f), b, m, s); }
int   sf_fseek(void *f, long o, int w){ return fseek(map_sf(f), o, w); }
long  sf_ftell(void *f){ return ftell(map_sf(f)); }
int   sf_getc(void *f){ return getc(map_sf(f)); }
int   sf_fgetc(void *f){ return fgetc(map_sf(f)); }
char *sf_fgets(char *s, int n, void *f){ return fgets(s, n, map_sf(f)); }
int   sf_vfwprintf(void *f, const wchar_t *fmt, va_list ap){ return vfwprintf(map_sf(f), fmt, ap); }
int   sf_fwide(void *f, int m){ return fwide(map_sf(f), m); }

/* ====== sigaction/sigprocmask: ABI bionic x glibc (arm64) ======
 * struct sigaction bionic arm64 = { int sa_flags; void* sa_handler; unsigned long sa_mask; void* sa_restorer; } = 32B.
 * glibc = 152B (sigset_t=128B). Se chamar a glibc direto no oldact (buffer bionic de 32B) -> estoura a stack.
 * Campos bsa_* p/ não colidir com as MACROS sa_handler/sa_sigaction da glibc. */
#include <signal.h>
#include <stdlib.h>
#include <string.h>
struct bionic_sigaction { int bsa_flags; void *bsa_handler; unsigned long bsa_mask; void *bsa_restorer; };
int my_sigaction(int sig, const struct bionic_sigaction *act, struct bionic_sigaction *oldact) {
  struct sigaction ga, go; struct sigaction *pga = NULL, *pgo = NULL;
  /* CUP_NOSIGH: NÃO deixa o engine instalar handler de sinais de crash -> nosso
   * handler pega o fault ORIGINAL (em vez do re-raise do crash handler do Unity). */
  if (getenv("CUP_NOSIGH") && (sig==4||sig==5||sig==6||sig==7||sig==8||sig==11)) { (void)oldact; return 0; }
  if (sig==10) { (void)oldact; return 0; }  /* SIGUSR1 = nosso diag_handler; jogo NÃO sobrescreve */
  /* CUP_GCSIG: não deixa o engine/GC sobrescrever nossos handlers de SIGPWR(30)/
     SIGXCPU(24) — nossas threads usam o protocolo de suspensão que NÃO mata. */
  if (getenv("CUP_GCSIG") && (sig==30||sig==24)) { (void)oldact; return 0; }
  if (act) {
    memset(&ga, 0, sizeof ga); ga.sa_flags = act->bsa_flags;
    if (act->bsa_flags & SA_SIGINFO) ga.sa_sigaction = (void (*)(int, siginfo_t *, void *))act->bsa_handler;
    else ga.sa_handler = (void (*)(int))act->bsa_handler;
    sigemptyset(&ga.sa_mask);
    for (int s = 1; s < 64; s++) if (act->bsa_mask & (1UL << (s - 1))) sigaddset(&ga.sa_mask, s);
    pga = &ga;
  }
  if (oldact) { memset(&go, 0, sizeof go); pgo = &go; }
  int r = sigaction(sig, pga, pgo);
  if (oldact) {
    oldact->bsa_flags = go.sa_flags;
    oldact->bsa_handler = (go.sa_flags & SA_SIGINFO) ? (void *)go.sa_sigaction : (void *)go.sa_handler;
    unsigned long m = 0; for (int s = 1; s < 64; s++) if (sigismember(&go.sa_mask, s)) m |= (1UL << (s - 1));
    oldact->bsa_mask = m; oldact->bsa_restorer = NULL;
  }
  return r;
}
