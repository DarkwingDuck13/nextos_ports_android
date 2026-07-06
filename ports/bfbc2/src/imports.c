/*
 * imports.c — shims bionic→glibc do BFBC2 (Karisma). Exporta port_shims[]:
 * a tabela que vence o dlsym em so_resolve. Cobre o que a glibc não dá igual:
 *   __sF (stdio bionic), __errno, sigaction (struct 16B arm32), fopen-redirect
 *   pros dados, __android_log_print, operator new/delete, e os 6 wrappers GL
 *   com arg float (engine SOFTFP × Mali HARDFP).
 */
#include <ctype.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>

#include "so_util.h"
#include "util.h"

/* stderr_fake definido em imports.gen.c */

/* ---------------- bionic __sF (stdin/out/err) ---------------- */
#define BIONIC_FILE_SZ 0x54
static char bionic_sF[3 * BIONIC_FILE_SZ + 64];
static FILE *map_sF(void *fp) {
  if (fp == (void *)(bionic_sF + 0 * BIONIC_FILE_SZ)) return stdin;
  if (fp == (void *)(bionic_sF + 1 * BIONIC_FILE_SZ)) return stdout;
  if (fp == (void *)(bionic_sF + 2 * BIONIC_FILE_SZ)) return stderr;
  return (FILE *)fp;
}

/* ---------------- fopen com redirect dos dados ---------------- */
static char g_data_dir[512] = "";
void imports_set_data_dir(const char *d) { snprintf(g_data_dir, sizeof g_data_dir, "%s", d); }

static int is_game_data(const char *base) {
  return base && (strncmp(base, "core.pak", 8) == 0 ||
                  strstr(base, "settings.dat") ||
                  strstr(base, ".indicate"));
}
static void *my_fopen(const char *path, const char *mode) {
  if (!path) return NULL;
  FILE *fp = fopen(path, mode);
  if (fp) return fp;
  /* redirect: <workingfolder>/core.pakdNNN -> <data_dir>/core.pakdNNN */
  const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
  if (g_data_dir[0] && is_game_data(base)) {
    char rp[1024]; snprintf(rp, sizeof rp, "%s/%s", g_data_dir, base);
    fp = fopen(rp, mode);
    if (getenv("BC2_FOPENLOG")) debugPrintf("[fopen] '%s' -> redirect '%s' %s\n", path, rp, fp ? "OK" : "FAIL");
    return fp;
  }
  if (getenv("BC2_FOPENLOG")) debugPrintf("[fopen] '%s' (%s) -> %s\n", path, mode, fp ? "OK" : "FAIL");
  return fp;
}
static size_t my_fread(void *p, size_t sz, size_t n, void *fp) { return fread(p, sz, n, map_sF(fp)); }
static size_t my_fwrite(const void *p, size_t sz, size_t n, void *fp) { return fwrite(p, sz, n, map_sF(fp)); }
static int my_fclose(void *fp) { FILE *f = map_sF(fp); if (f == stdin || f == stdout || f == stderr) return 0; return fclose(f); }
static int my_fflush(void *fp) { return fflush(fp ? map_sF(fp) : NULL); }
static int my_fseek(void *fp, long o, int w) { return fseek(map_sF(fp), o, w); }
static long my_ftell(void *fp) { return ftell(map_sF(fp)); }
static int my_fileno(void *fp) { return fileno(map_sF(fp)); }
static char *my_fgets(char *s, int n, void *fp) { return fgets(s, n, map_sF(fp)); }
static int my_fputs(const char *s, void *fp) { return fputs(s, map_sF(fp)); }
static int my_fputc(int c, void *fp) { return fputc(c, map_sF(fp)); }
static int my_fprintf(void *fp, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); int r = vfprintf(map_sF(fp), fmt, ap); va_end(ap); return r;
}

/* ---------------- errno / log ---------------- */
static volatile int *my_errno(void) { return __errno_location(); }
static int my_android_log(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
  if (!getenv("BC2_ANDROIDLOG")) return 0;
  char buf[1024]; va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt ? fmt : "", ap); va_end(ap);
  debugPrintf("[alog:%s] %s\n", tag ? tag : "?", buf);
  return 0;
}

/* ---------------- sigaction bionic(16B)→glibc ---------------- */
struct bionic_sigaction { void *handler; unsigned long mask; int flags; void *restorer; };
static int (*real_sigaction)(int, const struct sigaction *, struct sigaction *);
static int my_sigaction(int sig, const void *actp, void *oldp) {
  /* 🔑 NÃO deixar a engine sequestrar sinais fatais: o crash-reporter do Karisma
   * re-entra e faz LOOP INFINITO (enche o tmpfs → D-state → asfixia o device).
   * Nosso crash_handler (main.c) já dá backtrace + _exit(). */
  if (sig == SIGSEGV || sig == SIGBUS || sig == SIGABRT || sig == SIGILL || sig == SIGFPE) {
    if (getenv("BC2_FOPENLOG")) debugPrintf("[sigaction] recusado p/ sig=%d (mantem nosso handler)\n", sig);
    return 0;
  }
  if (!real_sigaction) real_sigaction = (void *)dlsym(RTLD_DEFAULT, "sigaction");
  struct sigaction ga; memset(&ga, 0, sizeof ga);
  const struct bionic_sigaction *ba = actp;
  if (ba) {
    ga.sa_handler = (void (*)(int))ba->handler;
    ga.sa_flags = ba->flags;
    sigemptyset(&ga.sa_mask);
    if (ba->mask) *(unsigned long *)&ga.sa_mask = ba->mask;
  }
  return real_sigaction(sig, ba ? &ga : NULL, NULL);
  (void)oldp;
}

/* ---------------- C++ operators (bionic mangling) ---------------- */
static void *my_new(size_t n) { return malloc(n ? n : 1); }
static void my_delete(void *p) { free(p); }

/* ---------------- GL float wrappers (SOFTFP) ---------------- */
#define AAPCS __attribute__((pcs("aapcs")))
static void *rgl(const char *n) { static void *h; (void)h; return dlsym(RTLD_DEFAULT, n); }

AAPCS static void my_glClearColor(float r, float g, float b, float a) {
  static void (*f)(float,float,float,float); if (!f) f = rgl("glClearColor"); if (f) f(r,g,b,a);
}
AAPCS static void my_glClearDepthf(float d) {
  static void (*f)(float); if (!f) f = rgl("glClearDepthf"); if (f) f(d);
}
AAPCS static void my_glDepthRangef(float n, float fa) {
  static void (*f)(float,float); if (!f) f = rgl("glDepthRangef"); if (f) f(n,fa);
}
AAPCS static void my_glAlphaFunc(unsigned int func, float ref) {
  static void (*f)(unsigned int,float); if (!f) f = rgl("glAlphaFunc"); if (f) f(func,ref);
}
AAPCS static void my_glFogf(unsigned int pname, float param) {
  static void (*f)(unsigned int,float); if (!f) f = rgl("glFogf"); if (f) f(pname,param);
}
AAPCS static void my_glTexEnvf(unsigned int target, unsigned int pname, float param) {
  static void (*f)(unsigned int,unsigned int,float); if (!f) f = rgl("glTexEnvf"); if (f) f(target,pname,param);
}

/* ---------------- tabela ---------------- */
static void *dso_handle_storage[1];
DynLibFunction port_shims[] = {
  { "__sF", (uintptr_t)bionic_sF },
  { "__dso_handle", (uintptr_t)dso_handle_storage },
  { "__errno", (uintptr_t)my_errno },
  { "__android_log_print", (uintptr_t)my_android_log },
  { "sigaction", (uintptr_t)my_sigaction },

  { "fopen", (uintptr_t)my_fopen },
  { "fread", (uintptr_t)my_fread },
  { "fwrite", (uintptr_t)my_fwrite },
  { "fclose", (uintptr_t)my_fclose },
  { "fflush", (uintptr_t)my_fflush },
  { "fseek", (uintptr_t)my_fseek },
  { "ftell", (uintptr_t)my_ftell },
  { "fileno", (uintptr_t)my_fileno },
  { "fgets", (uintptr_t)my_fgets },
  { "fputs", (uintptr_t)my_fputs },
  { "fputc", (uintptr_t)my_fputc },
  { "fprintf", (uintptr_t)my_fprintf },

  { "_Znwj", (uintptr_t)my_new },
  { "_Znaj", (uintptr_t)my_new },
  { "_ZdlPv", (uintptr_t)my_delete },
  { "_ZdaPv", (uintptr_t)my_delete },

  { "glClearColor", (uintptr_t)my_glClearColor },
  { "glClearDepthf", (uintptr_t)my_glClearDepthf },
  { "glDepthRangef", (uintptr_t)my_glDepthRangef },
  { "glAlphaFunc", (uintptr_t)my_glAlphaFunc },
  { "glFogf", (uintptr_t)my_glFogf },
  { "glTexEnvf", (uintptr_t)my_glTexEnvf },
};
int port_shims_count = (int)(sizeof(port_shims) / sizeof(port_shims[0]));
