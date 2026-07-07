/* imports.c -- libLEGO_LOTR.so import resolution (ELF32/ARM, GLES2)
 *
 * armeabi-v7a (softfp) game vs armhf (hardfp) loader: every imported function
 * that takes a float/double arg BY VALUE is routed through a pcs("aapcs")
 * softfp wrapper (softfp_shim + the three GL clear/depth entry points below).
 * Non-float GL binds to the serialized GLES2 hooks in hooks/egl.c (one real SDL
 * context handed between the render and the engine loader thread). OpenSL ES via
 * the SDL bridge; the rest to glibc / bionic shims.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <signal.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <elf.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "pthr.h"
#include "hooks.h"
#include "opensles_shim.h"
#include "softfp_shim.h"

extern int __cxa_atexit(void (*)(void *), void *, void *);
extern so_module game_mod;

// ---------------------------------------------------------------------------
// small local shims
// ---------------------------------------------------------------------------

static int *errno_fake(void) { return __errno_location(); }
static void stack_chk_fail_fake(void) { debugPrintf("!!! __stack_chk_fail reached (ignored)\n"); }
// the .so reads the guard from this global symbol (not bionic TLS on armv7)
uintptr_t __stack_chk_guard_val = 0xdeadc0de;
static void abort_log(void) { debugPrintf("!!! game called abort()\n"); abort(); }

// bionic __aeabi_atexit(obj, dtor, dso) -> register with glibc cxa_atexit
static int aeabi_atexit_fake(void *obj, void (*dtor)(void *), void *dso) {
  return __cxa_atexit(dtor, obj, dso);
}
// bionic __assert2(file, line, func, expr) -> log + abort
static void assert2_fake(const char *file, int line, const char *func, const char *expr) {
  debugPrintf("!!! __assert2 %s:%d %s: %s\n", file ? file : "?", line,
              func ? func : "?", expr ? expr : "?");
  abort();
}
// softfp float isfinite (game passes the float in an integer reg on armv7 softfp)
SF static int isfinitef_fake(float x) { return isfinite(x); }
SF static long double ceill_fake(long double x) { return ceill(x); }
SF static long double floorl_fake(long double x) { return floorl(x); }

// dl*: the game may probe optional system libs. We are a static so-loader, so
// fail cleanly (NULL) -- the engine treats a NULL handle as "feature absent".
static void *dlopen_fake(const char *name, int flags) {
  (void)flags; debugPrintf("dlopen(\"%s\") -> NULL (unsupported)\n", name ? name : "?"); return NULL;
}
static void *dlsym_fake(void *h, const char *name) {
  (void)h; debugPrintf("dlsym(\"%s\") -> NULL\n", name ? name : "?"); return NULL;
}
static int dlclose_fake(void *h) { (void)h; return 0; }

// fortified (_chk) libc variants
static void  *memcpy_chk_fake(void *d, const void *s, size_t n, size_t dl){(void)dl;return memcpy(d,s,n);}
static void  *memmove_chk_fake(void *d, const void *s, size_t n, size_t dl){(void)dl;return memmove(d,s,n);}
static void  *memset_chk_fake(void *d, int c, size_t n, size_t dl){(void)dl;return memset(d,c,n);}
static char  *strcpy_chk_fake(char *d, const char *s, size_t dl){(void)dl;return strcpy(d,s);}
static char  *strncpy_chk_fake(char *d, const char *s, size_t n, size_t dl){(void)dl;return strncpy(d,s,n);}
static char  *strcat_chk_fake(char *d, const char *s, size_t dl){(void)dl;return strcat(d,s);}
static size_t strlen_chk_fake(const char *s, size_t ml){(void)ml;return strlen(s);}
static int    vsnprintf_chk_fake(char *d, size_t n, int fl, size_t dl, const char *fmt, va_list ap){(void)fl;(void)dl;return vsnprintf(d,n,fmt,ap);}
static int    vsprintf_chk_fake(char *d, int fl, size_t dl, const char *fmt, va_list ap){(void)fl;(void)dl;return vsprintf(d,fmt,ap);}
static int    snprintf_chk_fake(char *d, size_t n, int fl, size_t dl, const char *fmt, ...){
  (void)fl;(void)dl; va_list ap; va_start(ap,fmt); int r=vsnprintf(d,n,fmt,ap); va_end(ap); return r;
}
static int    sprintf_chk_fake(char *d, int fl, size_t dl, const char *fmt, ...){
  (void)fl;(void)dl; va_list ap; va_start(ap,fmt); int r=vsprintf(d,fmt,ap); va_end(ap); return r;
}

static int android_log_print_fake(int prio, const char *tag, const char *fmt, ...) {
  (void)prio; char buf[512]; va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  debugPrintf("[alog:%s] %s\n", tag ? tag : "?", buf); return 0;
}

static int raise_fake(int sig) { debugPrintf("game raise(%d) ignored\n", sig); return 0; }

// fprintf over the fake bionic FILE* (declared in libc_shim.h; only vfprintf_fake
// is provided there).
int fprintf_fake(FILE *f, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int r = vfprintf_fake(f, fmt, ap);
  va_end(ap); return r;
}

// sleep/usleep park the single GL context while blocked so the loader thread can
// pick it up (see the PreLoadShadersDone deadlock note in the TFA port).
static void usleep_park(useconds_t usec) {
  egl_gl_ownership_park();
  struct timespec ts = { (time_t)(usec / 1000000), (long)(usec % 1000000) * 1000 };
  nanosleep(&ts, NULL);
  egl_gl_service_handover();
}
static void sleep_park(unsigned sec) {
  egl_gl_ownership_park();
  struct timespec ts = { (time_t)sec, 0 };
  nanosleep(&ts, NULL);
  egl_gl_service_handover();
}

// ---------------------------------------------------------------------------
// __gnu_Unwind_Find_exidx: the game's STATICALLY-linked C++ unwinder calls this
// to find the .ARM.exidx table of the module containing the faulting PC. Our
// module is custom-loaded (not dlopen'd), so glibc's own finder can't see it --
// a throw would fail to find its catch and hit std::terminate. Return our
// module's .ARM.exidx for PCs inside it; delegate the rest to glibc.
// ---------------------------------------------------------------------------
static uintptr_t g_text_lo = 0, g_text_hi = 0;   // loaded .text range
static uintptr_t g_exidx_addr = 0;               // loaded .ARM.exidx base
static int       g_exidx_count = 0;              // number of 8-byte entries

static void compute_exidx(void) {
  if (!game_mod.sec_hdr || !game_mod.shstrtab) return;
  uintptr_t lb = (uintptr_t)game_mod.load_base;
  for (int i = 0; i < game_mod.elf_hdr->e_shnum; i++) {
    Elf32_Shdr *sh = &game_mod.sec_hdr[i];
    const char *nm = game_mod.shstrtab + sh->sh_name;
    if (sh->sh_type == 0x70000001 /* SHT_ARM_EXIDX */ || !strcmp(nm, ".ARM.exidx")) {
      g_exidx_addr = lb + sh->sh_addr;
      g_exidx_count = (int)(sh->sh_size / 8);
    } else if (!strcmp(nm, ".text")) {
      g_text_lo = lb + sh->sh_addr;
      g_text_hi = g_text_lo + sh->sh_size;
    }
  }
  debugPrintf("exidx: text=[%p,%p) exidx=%p count=%d\n",
              (void *)g_text_lo, (void *)g_text_hi, (void *)g_exidx_addr, g_exidx_count);
}

static void *find_exidx_fake(uintptr_t pc, int *pcount) {
  if (g_exidx_addr && pc >= g_text_lo && pc < g_text_hi) {
    if (pcount) *pcount = g_exidx_count;
    return (void *)g_exidx_addr;
  }
  static void *(*real)(uintptr_t, int *) = NULL;
  static int tried = 0;
  if (!tried) { tried = 1; real = dlsym(RTLD_DEFAULT, "__gnu_Unwind_Find_exidx"); }
  if (real && real != (void *)find_exidx_fake) return real(pc, pcount);
  if (pcount) *pcount = 0;
  return NULL;
}

// ---------------------------------------------------------------------------
// GLES2 float entry points (softfp -> hooks). Only these three ES2 calls take
// floats BY VALUE; every other GL float import is a pointer (glUniform*fv etc.)
// and needs no bridge.
// ---------------------------------------------------------------------------
SF static void sf_glClearColorHook(float r, float g, float b, float a) { glClearColorHook(r, g, b, a); }
SF static void sf_glClearDepthfHook(float d) { glClearDepthfHook(d); }
SF static void sf_glDepthRangefHook(float n, float f) { glDepthRangefHook(n, f); }
SF static void sf_glTexParameterfHook(GLenum t, GLenum p, GLfloat v) { glTexParameterfHook(t, p, v); }

// ---------------------------------------------------------------------------
// bionic ctype tables (the .so inlines isXXX/toXXX against these globals)
// ---------------------------------------------------------------------------
#define B_U 0x01
#define B_L 0x02
#define B_D 0x04
#define B_S 0x08
#define B_P 0x10
#define B_C 0x20
#define B_X 0x40
#define B_B 0x80
static unsigned char bionic_ctype[257];         // [c+1]
static short         bionic_tolower[257];        // [c+1]
static short         bionic_toupper[257];        // [c+1]
const unsigned char *_ctype_ptr      = bionic_ctype;
const short         *_tolower_tab_ptr = bionic_tolower;
const short         *_toupper_tab_ptr = bionic_toupper;

static void build_ctype(void) {
  bionic_ctype[0] = 0;
  bionic_tolower[0] = -1;
  bionic_toupper[0] = -1;
  for (int c = 0; c < 256; c++) {
    unsigned char f = 0;
    if (isupper(c)) f |= B_U;
    if (islower(c)) f |= B_L;
    if (isdigit(c)) f |= B_D;
    if (isspace(c)) f |= B_S;
    if (ispunct(c)) f |= B_P;
    if (iscntrl(c)) f |= B_C;
    if (isxdigit(c)) f |= B_X;
    if (c == ' ')   f |= B_B;
    bionic_ctype[c + 1]   = f;
    bionic_tolower[c + 1] = (short)tolower(c);
    bionic_toupper[c + 1] = (short)toupper(c);
  }
}

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

DynLibFunction dynlib_functions[] = {
  // runtime / fortify
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__aeabi_atexit", (uintptr_t)&aeabi_atexit_fake },
  { "__errno", (uintptr_t)&errno_fake },
  { "__sF", (uintptr_t)&fake_sF },
  { "__assert2", (uintptr_t)&assert2_fake },
  { "__stack_chk_fail", (uintptr_t)&stack_chk_fail_fake },
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_val },
  { "__gnu_Unwind_Find_exidx", (uintptr_t)&find_exidx_fake },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "raise", (uintptr_t)&raise_fake },
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlsym", (uintptr_t)&dlsym_fake },
  { "dlclose", (uintptr_t)&dlclose_fake },

  // bionic ctype tables
  { "_ctype_", (uintptr_t)&_ctype_ptr },
  { "_tolower_tab_", (uintptr_t)&_tolower_tab_ptr },
  { "_toupper_tab_", (uintptr_t)&_toupper_tab_ptr },

  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake },

  // math -- ALL float/double args go through softfp wrappers
  { "acos", (uintptr_t)&sf_acos }, { "asin", (uintptr_t)&sf_asin }, { "atan", (uintptr_t)&sf_atan },
  { "cos", (uintptr_t)&sf_cos }, { "sin", (uintptr_t)&sf_sin }, { "tan", (uintptr_t)&sf_tan },
  { "exp", (uintptr_t)&sf_exp }, { "log", (uintptr_t)&sf_log }, { "log10", (uintptr_t)&sf_log10 },
  { "sqrt", (uintptr_t)&sf_sqrt }, { "ceil", (uintptr_t)&sf_ceil }, { "floor", (uintptr_t)&sf_floor },
  { "atan2", (uintptr_t)&sf_atan2 }, { "fmod", (uintptr_t)&sf_fmod }, { "pow", (uintptr_t)&sf_pow },
  { "ceill", (uintptr_t)&ceill_fake }, { "floorl", (uintptr_t)&floorl_fake },
  { "acosf", (uintptr_t)&sf_acosf }, { "asinf", (uintptr_t)&sf_asinf }, { "atanf", (uintptr_t)&sf_atanf },
  { "cosf", (uintptr_t)&sf_cosf }, { "sinf", (uintptr_t)&sf_sinf }, { "tanf", (uintptr_t)&sf_tanf },
  { "expf", (uintptr_t)&sf_expf }, { "logf", (uintptr_t)&sf_logf }, { "log10f", (uintptr_t)&sf_log10f },
  { "sqrtf", (uintptr_t)&sf_sqrtf }, { "fabsf", (uintptr_t)&sf_fabsf },
  { "ceilf", (uintptr_t)&sf_ceilf }, { "floorf", (uintptr_t)&sf_floorf },
  { "atan2f", (uintptr_t)&sf_atan2f }, { "fmodf", (uintptr_t)&sf_fmodf }, { "powf", (uintptr_t)&sf_powf },
  { "sincosf", (uintptr_t)&sf_sincosf },
  { "__isfinitef", (uintptr_t)&isfinitef_fake },

  // memory / stdlib
  { "abort", (uintptr_t)&abort_log }, { "atof", (uintptr_t)&atof },
  { "atoi", (uintptr_t)&atoi }, { "atol", (uintptr_t)&atol }, { "bsearch", (uintptr_t)&bsearch },
  { "lrand48", (uintptr_t)&lrand48 },
  { "free", (uintptr_t)&free }, { "malloc", (uintptr_t)&malloc },
  { "calloc", (uintptr_t)&calloc }, { "realloc", (uintptr_t)&realloc },
  { "memcmp", (uintptr_t)&memcmp }, { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove }, { "memset", (uintptr_t)&memset }, { "memchr", (uintptr_t)&memchr },
  { "qsort", (uintptr_t)&qsort }, { "posix_memalign", (uintptr_t)&posix_memalign_fake },
  { "strtol", (uintptr_t)&strtol }, { "strtoll", (uintptr_t)&strtoll },
  { "strtoul", (uintptr_t)&strtoul }, { "strtod", (uintptr_t)&strtod }, { "strtof", (uintptr_t)&strtof },
  { "mmap", (uintptr_t)&mmap }, { "munmap", (uintptr_t)&munmap },

  // strings
  { "strcasecmp", (uintptr_t)&strcasecmp }, { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr }, { "strcmp", (uintptr_t)&strcmp },
  { "strcpy", (uintptr_t)&strcpy }, { "strerror", (uintptr_t)&strerror },
  { "strlen", (uintptr_t)&strlen }, { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncat", (uintptr_t)&strncat }, { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy }, { "strrchr", (uintptr_t)&strrchr },
  { "strstr", (uintptr_t)&strstr }, { "strtok", (uintptr_t)&strtok },
  { "strerror_r", (uintptr_t)&strerror_r_fake },

  // ctype (also imported as functions)
  { "isalnum", (uintptr_t)&isalnum }, { "isalpha", (uintptr_t)&isalpha },
  { "iscntrl", (uintptr_t)&iscntrl }, { "isgraph", (uintptr_t)&isgraph },
  { "islower", (uintptr_t)&islower }, { "isprint", (uintptr_t)&isprint },
  { "ispunct", (uintptr_t)&ispunct }, { "isspace", (uintptr_t)&isspace },
  { "isupper", (uintptr_t)&isupper }, { "isxdigit", (uintptr_t)&isxdigit },
  { "isdigit", (uintptr_t)&isdigit },
  { "tolower", (uintptr_t)&tolower }, { "toupper", (uintptr_t)&toupper },

  // stdio
  { "fclose", (uintptr_t)&fclose_fake }, { "ferror", (uintptr_t)&ferror_fake },
  { "fflush", (uintptr_t)&fflush_fake }, { "fgetc", (uintptr_t)&fgetc },
  { "fileno", (uintptr_t)&fileno_fake }, { "fopen", (uintptr_t)&fopen_fake },
  { "fputc", (uintptr_t)&fputc_fake }, { "fputs", (uintptr_t)&fputs_fake },
  { "fread", (uintptr_t)&fread_fake }, { "fseek", (uintptr_t)&fseek_fake },
  { "ftell", (uintptr_t)&ftell }, { "fwrite", (uintptr_t)&fwrite_fake },
  { "rewind", (uintptr_t)&rewind }, { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf }, { "sscanf", (uintptr_t)&sscanf },
  { "ungetc", (uintptr_t)&ungetc_fake }, { "vasprintf", (uintptr_t)&vasprintf },
  { "vfprintf", (uintptr_t)&vfprintf_fake }, { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },
  { "fprintf", (uintptr_t)&fprintf_fake }, { "printf", (uintptr_t)&printf },
  { "fdopen", (uintptr_t)&fdopen }, { "puts", (uintptr_t)&puts },

  // filesystem
  { "close", (uintptr_t)&close }, { "mkdir", (uintptr_t)&mkdir_fake },
  { "open", (uintptr_t)&open_fake }, { "__open_2", (uintptr_t)&open2_fake },
  { "remove", (uintptr_t)&remove_fake }, { "stat", (uintptr_t)&stat_fake },
  { "read", (uintptr_t)&read }, { "write", (uintptr_t)&write }, { "lseek", (uintptr_t)&lseek },
  { "access", (uintptr_t)&access },

  // syslog (no-op)
  { "closelog", (uintptr_t)&ret0 }, { "openlog", (uintptr_t)&ret0 }, { "syslog", (uintptr_t)&ret0 },

  // time / sched / sleep
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "time", (uintptr_t)&time }, { "localtime", (uintptr_t)&localtime },
  { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max_fake },
  { "sched_get_priority_min", (uintptr_t)&sched_get_priority_min_fake },
  { "sched_yield", (uintptr_t)&sched_yield },
  { "sleep", (uintptr_t)&sleep_park }, { "usleep", (uintptr_t)&usleep_park },
  { "nanosleep", (uintptr_t)&nanosleep }, { "strftime", (uintptr_t)&strftime },
  { "sysconf", (uintptr_t)&sysconf_fake }, { "syscall", (uintptr_t)&syscall_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "__android_log_print", (uintptr_t)&android_log_print_fake },

  // EGL (wrapper owns display/ctx/surface; these query + tweak)
  { "eglBindAPI", (uintptr_t)&eglBindAPIHook },
  { "eglChooseConfig", (uintptr_t)&eglChooseConfigHook },
  { "eglCreateContext", (uintptr_t)&eglCreateContextHook },
  { "eglCreatePbufferSurface", (uintptr_t)&eglCreatePbufferSurfaceHook },
  { "eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext },
  { "eglGetCurrentDisplay", (uintptr_t)&eglGetCurrentDisplay },
  { "eglGetCurrentSurface", (uintptr_t)&eglGetCurrentSurface },
  { "eglGetError", (uintptr_t)&eglGetError },
  { "eglGetProcAddress", (uintptr_t)&eglGetProcAddressHook },
  { "eglMakeCurrent", (uintptr_t)&eglMakeCurrentHook },
  { "eglSwapInterval", (uintptr_t)&eglSwapIntervalHook },

  // GLES2 (serialized through hooks/egl.c)
  { "glActiveTexture", (uintptr_t)&glActiveTextureHook },
  { "glAttachShader", (uintptr_t)&glAttachShaderHook },
  { "glBindBuffer", (uintptr_t)&glBindBufferHook },
  { "glBindFramebuffer", (uintptr_t)&glBindFramebufferHook },
  { "glBindRenderbuffer", (uintptr_t)&glBindRenderbufferHook },
  { "glBindTexture", (uintptr_t)&glBindTextureHook },
  { "glBlendEquation", (uintptr_t)&glBlendEquationHook },
  { "glBlendFunc", (uintptr_t)&glBlendFuncHook },
  { "glBufferData", (uintptr_t)&glBufferDataHook },
  { "glClear", (uintptr_t)&glClearHook },
  { "glClearColor", (uintptr_t)&sf_glClearColorHook },
  { "glClearDepthf", (uintptr_t)&sf_glClearDepthfHook },
  { "glClearStencil", (uintptr_t)&glClearStencilHook },
  { "glCompileShader", (uintptr_t)&glCompileShaderHook },
  { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2DHook },
  { "glCreateProgram", (uintptr_t)&glCreateProgramHook },
  { "glCreateShader", (uintptr_t)&glCreateShaderHook },
  { "glDeleteBuffers", (uintptr_t)&glDeleteBuffersHook },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffersHook },
  { "glDeleteProgram", (uintptr_t)&glDeleteProgramHook },
  { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffersHook },
  { "glDeleteShader", (uintptr_t)&glDeleteShaderHook },
  { "glDeleteTextures", (uintptr_t)&glDeleteTexturesHook },
  { "glDepthFunc", (uintptr_t)&glDepthFuncHook },
  { "glDepthMask", (uintptr_t)&glDepthMaskHook },
  { "glDepthRangef", (uintptr_t)&sf_glDepthRangefHook },
  { "glDisable", (uintptr_t)&glDisableHook },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArrayHook },
  { "glDrawArrays", (uintptr_t)&glDrawArraysHook },
  { "glDrawElements", (uintptr_t)&glDrawElementsHook },
  { "glEnable", (uintptr_t)&glEnableHook },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArrayHook },
  { "glFinish", (uintptr_t)&glFinishHook },
  { "glFlush", (uintptr_t)&glFlushHook },
  { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbufferHook },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2DHook },
  { "glFrontFace", (uintptr_t)&glFrontFaceHook },
  { "glGenBuffers", (uintptr_t)&glGenBuffersHook },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffersHook },
  { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffersHook },
  { "glGenTextures", (uintptr_t)&glGenTexturesHook },
  { "glGetActiveAttrib", (uintptr_t)&glGetActiveAttribHook },
  { "glGetActiveUniform", (uintptr_t)&glGetActiveUniformHook },
  { "glGetAttribLocation", (uintptr_t)&glGetAttribLocationHook },
  { "glGetBufferParameteriv", (uintptr_t)&glGetBufferParameterivHook },
  { "glGetError", (uintptr_t)&glGetErrorHook },
  { "glGetIntegerv", (uintptr_t)&glGetIntegervHook },
  { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLogHook },
  { "glGetProgramiv", (uintptr_t)&glGetProgramivHook },
  { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLogHook },
  { "glGetShaderiv", (uintptr_t)&glGetShaderivHook },
  { "glGetString", (uintptr_t)&glGetStringHook },
  { "glGetUniformLocation", (uintptr_t)&glGetUniformLocationHook },
  { "glLinkProgram", (uintptr_t)&glLinkProgramHook },
  { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorageHook },
  { "glScissor", (uintptr_t)&glScissorHook },
  { "glShaderSource", (uintptr_t)&glShaderSourceHook },
  { "glStencilFunc", (uintptr_t)&glStencilFuncHook },
  { "glStencilMask", (uintptr_t)&glStencilMaskHook },
  { "glStencilOp", (uintptr_t)&glStencilOpHook },
  { "glTexImage2D", (uintptr_t)&glTexImage2DHook },
  { "glTexParameterf", (uintptr_t)&sf_glTexParameterfHook },
  { "glTexParameteri", (uintptr_t)&glTexParameteriHook },
  { "glUniform1fv", (uintptr_t)&glUniform1fvHook },
  { "glUniform1i", (uintptr_t)&glUniform1iHook },
  { "glUniform2fv", (uintptr_t)&glUniform2fvHook },
  { "glUniform3fv", (uintptr_t)&glUniform3fvHook },
  { "glUniform4fv", (uintptr_t)&glUniform4fvHook },
  { "glUniformMatrix2fv", (uintptr_t)&glUniformMatrix2fvHook },
  { "glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fvHook },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fvHook },
  { "glUseProgram", (uintptr_t)&glUseProgramHook },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointerHook },
  { "glViewport", (uintptr_t)&glViewportHook },

  // OpenSL ES (SDL-backed bridge)
  { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&sl_IID_BUFFERQUEUE },
  { "SL_IID_ENGINE", (uintptr_t)&sl_IID_ENGINE },
  { "SL_IID_PLAY", (uintptr_t)&sl_IID_PLAY },
  { "SL_IID_PLAYBACKRATE", (uintptr_t)&sl_IID_PLAYBACKRATE },
  { "SL_IID_SEEK", (uintptr_t)&sl_IID_SEEK },
  { "SL_IID_VOLUME", (uintptr_t)&sl_IID_VOLUME },
  { "slCreateEngine", (uintptr_t)&slCreateEngine_shim },

  // pthread (pthr.c soloader wrappers)
  { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader },
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init_soloader },
  { "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate_soloader },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_soloader },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_soloader },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_soloader },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_soloader },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_soloader },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_soloader },
  { "pthread_create", (uintptr_t)&pthread_create_soloader },
  { "pthread_detach", (uintptr_t)&pthread_detach_soloader },
  { "pthread_equal", (uintptr_t)&pthread_equal_soloader },
  { "pthread_getschedparam", (uintptr_t)&pthread_getschedparam_soloader },
  { "pthread_join", (uintptr_t)&pthread_join_soloader },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_soloader },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_soloader },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_soloader },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_soloader },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_soloader },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_soloader },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_soloader },
  { "pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy_soloader },
  { "pthread_once", (uintptr_t)&pthread_once_soloader },
  { "pthread_self", (uintptr_t)&pthread_self_soloader },
  { "pthread_setname_np", (uintptr_t)&pthread_setname_np_fake },
  { "pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },

  // semaphores
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },

  // fortified (_chk) libc variants
  { "__memcpy_chk", (uintptr_t)&memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&memmove_chk_fake },
  { "__memset_chk", (uintptr_t)&memset_chk_fake },
  { "__strcpy_chk", (uintptr_t)&strcpy_chk_fake },
  { "__strncpy_chk", (uintptr_t)&strncpy_chk_fake },
  { "__strcat_chk", (uintptr_t)&strcat_chk_fake },
  { "__strlen_chk", (uintptr_t)&strlen_chk_fake },
  { "__vsnprintf_chk", (uintptr_t)&vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&vsprintf_chk_fake },
  { "__snprintf_chk", (uintptr_t)&snprintf_chk_fake },
  { "__sprintf_chk", (uintptr_t)&sprintf_chk_fake },
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void update_imports(void) { build_ctype(); compute_exidx(); }
