/* imports.c (DEVICE) -- SHANTAE: Risky's Revenge / Pirate's Curse
 * (WayForward "Black" engine, NativeActivity native armv7, ES2 + OpenSLES).
 * so-loader p/ NextOS armv7 + Mali-450 (Utgard, GLES2 via SDL2).
 *
 * Tabela de OVERRIDES (resolvida ANTES do fallback dlsym do so_resolve). Tudo
 * que NÃO está aqui cai no dlsym(RTLD_DEFAULT) -> libc/libm/libGLESv2/libEGL/
 * SDL2 pré-carregadas RTLD_GLOBAL.
 *   EGL          -> egl_shim_*   (contexto GLES2 via SDL2, Mali fbdev)
 *   ANativeWindow/AAsset/ALooper extras -> impls locais
 *   OpenSL ES    -> opensles_shim
 *   bionic-only (strlcpy/strlcat/__assert2/log/property) -> wrappers
 *   pthread      -> revc_pthread_table (pthread_bridge.c)
 *
 * REUSA os shims genéricos PROVADOS (Dysmantle/Crazy Taxi verdes); SEM hooks de
 * textura específicos de outro jogo.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "so_util.h"
#include "egl_shim.h"
#include "android_shim.h"

/* ---------------- liblog ---------------- */
static int b_log_print(int prio, const char *tag, const char *fmt, ...) {
  fprintf(stderr, "[ALOG:%d %s] ", prio, tag ? tag : "?");
  va_list ap; va_start(ap, fmt);
  vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap);
  return 0;
}
static int b_log_write(int prio, const char *tag, const char *text) {
  fprintf(stderr, "[ALOG:%d %s] %s\n", prio, tag ? tag : "?", text ? text : "");
  return 0;
}
static void b_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
  fprintf(stderr, "[ALOG-ASSERT %s] cond=%s ", tag ? tag : "?", cond ? cond : "?");
  if (fmt) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }
  fprintf(stderr, "\n");
}
static int b_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  fprintf(stderr, "[ALOG:%d %s] ", prio, tag ? tag : "?");
  vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
  return 0;
}

/* ---------------- bionic-only libc ---------------- */
/* glibc não tem strlcpy/strlcat (BSD/bionic). Implementação canônica. */
static size_t b_strlcpy(char *dst, const char *src, size_t dsize) {
  const char *osrc = src; size_t nleft = dsize;
  if (nleft != 0) { while (--nleft != 0) { if ((*dst++ = *src++) == '\0') break; } }
  if (nleft == 0) { if (dsize != 0) *dst = '\0'; while (*src++) ; }
  return src - osrc - 1;
}
static size_t b_strlcat(char *dst, const char *src, size_t dsize) {
  const char *odst = dst; const char *osrc = src; size_t n = dsize; size_t dlen;
  while (n-- != 0 && *dst != '\0') dst++;
  dlen = dst - odst; n = dsize - dlen;
  if (n-- == 0) return dlen + strlen(src);
  while (*src != '\0') { if (n != 0) { *dst++ = *src; n--; } src++; }
  *dst = '\0';
  return dlen + (src - osrc);
}
static void b_assert2(const char *file, int line, const char *func, const char *msg) {
  fprintf(stderr, "[bionic __assert2] %s:%d %s: %s\n",
          file ? file : "?", line, func ? func : "?", msg ? msg : "?");
  abort();
}
static int b_system_property_get(const char *name, char *value) {
  (void)name; if (value) value[0] = '\0'; return 0;
}
/* bionic usa __errno() (retorna int*); glibc usa __errno_location */
static int *b_errno(void) {
  static int *(*real)(void) = NULL;
  if (!real) real = (int *(*)(void))dlsym(RTLD_DEFAULT, "__errno_location");
  return real ? real() : NULL;
}
/* __stack_chk_fail: no-op (canary bionic tpidr+offset não casa sob glibc) */
static void my_stack_chk_fail(void) { /* no-op; ver nota canary no main */ }

/* ---------------- ANativeWindow ---------------- */
extern int dys_screen_w, dys_screen_h; /* resolucao real do egl_shim */
static ANativeWindow *aw_fromSurface(void *env, void *surface) {
  (void)env; (void)surface; return android_shim_get_window();
}
static void aw_acquire(void *w) { (void)w; }
static void aw_release(void *w) { (void)w; }
static int  aw_getWidth(void *w)  { (void)w; return dys_screen_w; }
static int  aw_getHeight(void *w) { (void)w; return dys_screen_h; }
static int  aw_getFormat(void *w) { (void)w; return 1; /* WINDOW_FORMAT_RGBA_8888 */ }
static int  aw_setBuffersGeometry(void *w, int x, int y, int f) {
  (void)w; (void)x; (void)y; (void)f; return 0;
}

/* ---------------- AAsset / AAssetManager / AAssetDir ----------------
 * O jogo lê assets/*.vol via AAssetManager. Servimos de um diretório real no
 * device (extraído do APK). Base por env SHANTAE_ASSETS, default "assets". */
typedef struct { FILE *fp; long len; char path[640]; } ShAsset;
typedef struct { DIR *d; char base[640]; struct dirent *cur; } ShAssetDir;

static const char *assets_base(void) {
  const char *b = getenv("SHANTAE_ASSETS");
  return (b && *b) ? b : "assets";
}
static void *aam_fromJava(void *env, void *obj) { (void)env; (void)obj; return (void *)1; }
static void *aam_open(void *mgr, const char *fn, int mode) {
  (void)mgr; (void)mode;
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", assets_base(), fn);
  FILE *fp = fopen(path, "rb");
  if (!fp) { fprintf(stderr, "[AAsset] MISS %s\n", path); return NULL; }
  ShAsset *a = calloc(1, sizeof(ShAsset));
  a->fp = fp; fseek(fp, 0, SEEK_END); a->len = ftell(fp); fseek(fp, 0, SEEK_SET);
  snprintf(a->path, sizeof(a->path), "%s", path);
  return a;
}
static int  aa_read(void *h, void *buf, size_t n) {
  ShAsset *a = h; if (!a) return -1; return (int)fread(buf, 1, n, a->fp);
}
static long aa_seek(void *h, long off, int wh) {
  ShAsset *a = h; if (!a) return -1; fseek(a->fp, off, wh); return ftell(a->fp);
}
static long aa_seek64(void *h, long off, int wh) { return aa_seek(h, off, wh); }
static long aa_getLength(void *h)   { ShAsset *a = h; return a ? a->len : 0; }
static long aa_getRemaining(void *h){ ShAsset *a = h; return a ? a->len - ftell(a->fp) : 0; }
static void aa_close(void *h)       { ShAsset *a = h; if (a) { fclose(a->fp); free(a); } }
static int  aa_openFd(void *h, long *start, long *len) {
  ShAsset *a = h; if (!a) return -1;
  if (start) *start = 0; if (len) *len = a->len;
  fflush(a->fp); return dup(fileno(a->fp));
}
static void *aam_openDir(void *mgr, const char *dirn) {
  (void)mgr;
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", assets_base(), dirn ? dirn : "");
  DIR *d = opendir(path);
  if (!d) return NULL;
  ShAssetDir *ad = calloc(1, sizeof(ShAssetDir));
  ad->d = d; snprintf(ad->base, sizeof(ad->base), "%s", path);
  return ad;
}
static const char *aad_getNext(void *h) {
  ShAssetDir *ad = h; if (!ad) return NULL;
  struct dirent *e;
  while ((e = readdir(ad->d))) {
    if (e->d_name[0] == '.') continue;
    return e->d_name;
  }
  return NULL;
}
static void aad_close(void *h) { ShAssetDir *ad = h; if (ad) { closedir(ad->d); free(ad); } }

/* ---------------- ALooper extras ---------------- */
static int  al_pollOnce(int t, int *fd, int *ev, void **data) { return ALooper_pollAll(t, fd, ev, data); }
static void *al_forThread(void) { return (void *)1; }
static void al_acquire(void *l) { (void)l; }
static void al_release(void *l) { (void)l; }
static void al_removeFd(void *l, int fd) { (void)l; (void)fd; }
static void al_wake(void *l) { (void)l; }

/* ---------------- AInput extras ---------------- */
static int  aie_getDeviceId(void *e) { (void)e; return 0; }
static int  ame_getButtonState(void *e) { (void)e; return 0; }

/* ---------------- pthread attr (stack size grande p/ bionic 1MB+) ---------------- */
static int my_attr_setstacksize(void *attr, size_t sz) {
  static int (*real)(void *, size_t) = NULL;
  if (!real) real = (int (*)(void *, size_t))dlsym(RTLD_DEFAULT, "pthread_attr_setstacksize");
  if (sz < 512 * 1024) sz = 512 * 1024;
  return real ? real(attr, sz) : 0;
}

/* ---------------- OpenSL ES interface IDs ---------------- */
extern uint32_t slCreateEngine_shim(void **, uint32_t, const void *, uint32_t,
                                    const void *, const void *);
extern const void *sl_IID_ENGINE, *sl_IID_PLAY, *sl_IID_VOLUME,
    *sl_IID_BUFFERQUEUE;
static const void *SL_IID_ENGINE_v, *SL_IID_PLAY_v, *SL_IID_RECORD_v,
    *SL_IID_VOLUME_v, *SL_IID_BUFFERQUEUE_v, *SL_IID_ANDROIDSIMPLEBUFFERQUEUE_v,
    *SL_IID_ANDROIDCONFIGURATION_v = "ACFG";
__attribute__((constructor)) static void sl_iid_init(void) {
  SL_IID_ENGINE_v = sl_IID_ENGINE;
  SL_IID_PLAY_v = sl_IID_PLAY;
  SL_IID_VOLUME_v = sl_IID_VOLUME;
  SL_IID_RECORD_v = "RECORD";
  SL_IID_BUFFERQUEUE_v = sl_IID_BUFFERQUEUE;
  SL_IID_ANDROIDSIMPLEBUFFERQUEUE_v = sl_IID_BUFFERQUEUE;
}

/* ponte stdio (stdio_shim.c) */
extern size_t b_fwrite(const void *, size_t, size_t, void *);
extern size_t b_fread(void *, size_t, size_t, void *);
extern int b_fputs(const char *, void *);
extern int b_fputc(int, void *);
extern int b_fgetc(void *);
extern int b_fseek(void *, long, int);
extern long b_ftell(void *);
extern int b_fflush(void *);
extern int b_fclose(void *);

/* memcpy/memmove com guarda de diagnóstico: loga e protege dest/src nulos.
 * (O jogo crasha cedo em memcpy(dest=0,...,11) numa op de std::string na init de
 * idioma; queremos ver a origem e sobreviver p/ avaliar.) */
extern int dysmantle_record_vbsize_dummy; /* nada */
static void *my_memcpy(void *d, const void *s, size_t n) {
  if ((uintptr_t)d < 0x1000 || (uintptr_t)s < 0x1000) {
    static int z = 0;
    if (z < 40) {
      fprintf(stderr, "[MEMCPY-NULL] dest=%p src=%p n=%zu ret=%p\n",
              d, s, n, __builtin_return_address(0));
      z++;
    }
    return d; /* sobrevive (não copia) */
  }
  return memcpy(d, s, n);
}
static void *my_memmove(void *d, const void *s, size_t n) {
  if ((uintptr_t)d < 0x1000 || (uintptr_t)s < 0x1000) return d;
  return memmove(d, s, n);
}
static void *my_memcpy_chk(void *d, const void *s, size_t n, size_t dn) {
  (void)dn; return my_memcpy(d, s, n);
}
/* __aeabi_memcpy: void(void*,const void*,size_t) — mesmos args do memcpy */
static void my_aeabi_memcpy(void *d, const void *s, size_t n) { my_memcpy(d, s, n); }
static void my_aeabi_memmove(void *d, const void *s, size_t n) { my_memmove(d, s, n); }
/* __aeabi_memset: void(void*, size_t n, int c) — ORDEM DIFERENTE do memset */
static void my_aeabi_memset(void *d, size_t n, int c) {
  if ((uintptr_t)d < 0x1000) return;
  memset(d, c, n);
}

static unsigned egl_releasethread_stub(void) { return 1u; }

/* ---- GL logging (diagnóstico tela preta) — gated por SHANTAE_GLLOG ---- */
static int g_gllog = -1;
static int gllog_on(void) { if (g_gllog<0) g_gllog = getenv("SHANTAE_GLLOG")?1:0; return g_gllog; }
static void *rgl(const char *n) { return dlsym(RTLD_DEFAULT, n); }
static void my_glBindFramebuffer(unsigned t, unsigned fb) {
  static void(*r)(unsigned,unsigned); if(!r)r=rgl("glBindFramebuffer");
  if (gllog_on()) { static int z=0; if(z<60){fprintf(stderr,"[GL] BindFramebuffer(0x%x, fbo=%u)\n",t,fb);z++;} }
  r(t,fb);
}
static void my_glClearColor(float a,float b,float c,float d){
  static void(*r)(float,float,float,float); if(!r)r=rgl("glClearColor");
  if (gllog_on()) { static int z=0; if(z<30){fprintf(stderr,"[GL] ClearColor(%.2f,%.2f,%.2f,%.2f)\n",a,b,c,d);z++;} }
  r(a,b,c,d);
}
static void my_glClear(unsigned m){
  static void(*r)(unsigned); if(!r)r=rgl("glClear");
  if (gllog_on()) { static unsigned long n=0; if((n++ % 120)==0)fprintf(stderr,"[GL] Clear mask=0x%x (#%lu)\n",m,n); }
  r(m);
}
static void my_glDrawElements(unsigned md,int c,unsigned t,const void*i){
  static void(*r)(unsigned,int,unsigned,const void*); if(!r)r=rgl("glDrawElements");
  if (gllog_on()) { static unsigned long n=0; if((n++ % 500)==0)fprintf(stderr,"[GL] DrawElements #%lu count=%d\n",n,c); }
  r(md,c,t,i);
}
static void my_glDrawArrays(unsigned md,int f,int c){
  static void(*r)(unsigned,int,int); if(!r)r=rgl("glDrawArrays");
  if (gllog_on()) { static unsigned long n=0; if((n++ % 500)==0)fprintf(stderr,"[GL] DrawArrays #%lu count=%d\n",n,c); }
  r(md,f,c);
}

/* __gnu_Unwind_Find_exidx: o unwinder ESTÁTICO do jogo chama isto p/ achar a
 * tabela .ARM.exidx do módulo que contém o PC. O módulo é custom-loaded (não
 * dlopen) -> o __gnu_Unwind_Find_exidx do glibc (via dl_iterate_phdr) não o
 * conhece -> throw não acha o catch -> std::terminate. Aqui retornamos o
 * .ARM.exidx do nosso módulo p/ PCs dentro dele, e delegamos o resto. */
extern volatile uintptr_t g_load_base;
#define SH_EXIDX_VADDR 0x2e1198u
#define SH_EXIDX_SIZE  0xc0b8u
#define SH_TEXT_SIZE   0x340c58u
static void *my_find_exidx(uintptr_t pc, int *pcount) {
  uintptr_t lb = g_load_base;
  if (getenv("SHANTAE_EXIDXLOG") && lb && pc >= lb && pc < lb + SH_TEXT_SIZE)
    fprintf(stderr, "[EXIDX] frame off=0x%lx\n", (unsigned long)(pc - lb));
  if (lb && pc >= lb && pc < lb + SH_TEXT_SIZE) {
    if (pcount) *pcount = (int)(SH_EXIDX_SIZE / 8);
    return (void *)(lb + SH_EXIDX_VADDR);
  }
  static void *(*real)(uintptr_t, int *) = NULL;
  static int tried = 0;
  if (!tried) { tried = 1; real = dlsym(RTLD_DEFAULT, "__gnu_Unwind_Find_exidx"); }
  if (real && real != (void *)my_find_exidx) return real(pc, pcount);
  if (pcount) *pcount = 0;
  return NULL;
}

/* egl_shim chama isto p/ permitir override de glGetProcAddress; sem override
 * (Shantae usa GLES2 do device direto) -> NULL = usa a função real. */
void *dysmantle_gl_proc_override(const char *name) { (void)name; return NULL; }

/* ---------------- tabela de overrides ---------------- */
DynLibFunction shantae_overrides[] = {
    /* liblog */
    {"__android_log_print", (uintptr_t)b_log_print},
    {"__android_log_write", (uintptr_t)b_log_write},
    {"__android_log_assert", (uintptr_t)b_log_assert},
    {"__android_log_vprint", (uintptr_t)b_log_vprint},
    /* bionic-only libc */
    {"strlcpy", (uintptr_t)b_strlcpy},
    {"strlcat", (uintptr_t)b_strlcat},
    {"__assert2", (uintptr_t)b_assert2},
    {"__system_property_get", (uintptr_t)b_system_property_get},
    {"__errno", (uintptr_t)b_errno},
    {"__stack_chk_fail", (uintptr_t)my_stack_chk_fail},
    {"__gnu_Unwind_Find_exidx", (uintptr_t)my_find_exidx},
    /* pthread_attr_* e pthread_create -> revc_pthread_table (bridge) */
    /* stdio bridge bionic->glibc (stdio_shim.c) — __sF/_ctype_/_toupper_tab_
     * resolvem por dlsym (símbolos exportados); funções abaixo precisam OVERRIDE
     * (a glibc nativa crasharia com FILE* do array __sF). */
    {"fwrite", (uintptr_t)b_fwrite},
    {"fread", (uintptr_t)b_fread},
    {"fputs", (uintptr_t)b_fputs},
    {"fputc", (uintptr_t)b_fputc},
    {"fgetc", (uintptr_t)b_fgetc},
    {"fseek", (uintptr_t)b_fseek},
    {"ftell", (uintptr_t)b_ftell},
    {"fflush", (uintptr_t)b_fflush},
    {"fclose", (uintptr_t)b_fclose},
    {"memcpy", (uintptr_t)my_memcpy},
    {"memmove", (uintptr_t)my_memmove},
    {"__memcpy_chk", (uintptr_t)my_memcpy_chk},
    {"__aeabi_memcpy", (uintptr_t)my_aeabi_memcpy},
    {"__aeabi_memcpy4", (uintptr_t)my_aeabi_memcpy},
    {"__aeabi_memcpy8", (uintptr_t)my_aeabi_memcpy},
    {"__aeabi_memmove", (uintptr_t)my_aeabi_memmove},
    {"__aeabi_memmove4", (uintptr_t)my_aeabi_memmove},
    {"__aeabi_memmove8", (uintptr_t)my_aeabi_memmove},
    {"__aeabi_memset", (uintptr_t)my_aeabi_memset},
    {"__aeabi_memset4", (uintptr_t)my_aeabi_memset},
    {"__aeabi_memset8", (uintptr_t)my_aeabi_memset},
    /* EGL -> egl_shim (GLES2 via SDL2, Mali fbdev) */
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
    {"eglReleaseThread", (uintptr_t)egl_releasethread_stub},
    /* ANativeWindow */
    {"ANativeWindow_fromSurface", (uintptr_t)aw_fromSurface},
    {"ANativeWindow_acquire", (uintptr_t)aw_acquire},
    {"ANativeWindow_release", (uintptr_t)aw_release},
    {"ANativeWindow_getWidth", (uintptr_t)aw_getWidth},
    {"ANativeWindow_getHeight", (uintptr_t)aw_getHeight},
    {"ANativeWindow_getFormat", (uintptr_t)aw_getFormat},
    {"ANativeWindow_setBuffersGeometry", (uintptr_t)aw_setBuffersGeometry},
    /* AAsset */
    {"AAssetManager_fromJava", (uintptr_t)aam_fromJava},
    {"AAssetManager_open", (uintptr_t)aam_open},
    {"AAssetManager_openDir", (uintptr_t)aam_openDir},
    {"AAsset_read", (uintptr_t)aa_read},
    {"AAsset_seek", (uintptr_t)aa_seek},
    {"AAsset_seek64", (uintptr_t)aa_seek64},
    {"AAsset_getLength", (uintptr_t)aa_getLength},
    {"AAsset_getLength64", (uintptr_t)aa_getLength},
    {"AAsset_getRemainingLength", (uintptr_t)aa_getRemaining},
    {"AAsset_getRemainingLength64", (uintptr_t)aa_getRemaining},
    {"AAsset_close", (uintptr_t)aa_close},
    {"AAsset_openFileDescriptor", (uintptr_t)aa_openFd},
    {"AAssetDir_getNextFileName", (uintptr_t)aad_getNext},
    {"AAssetDir_close", (uintptr_t)aad_close},
    /* ALooper extras */
    {"ALooper_pollOnce", (uintptr_t)al_pollOnce},
    {"ALooper_forThread", (uintptr_t)al_forThread},
    {"ALooper_acquire", (uintptr_t)al_acquire},
    {"ALooper_release", (uintptr_t)al_release},
    {"ALooper_removeFd", (uintptr_t)al_removeFd},
    {"ALooper_wake", (uintptr_t)al_wake},
    /* AInput extras */
    {"AInputEvent_getDeviceId", (uintptr_t)aie_getDeviceId},
    {"AMotionEvent_getButtonState", (uintptr_t)ame_getButtonState},
    /* OpenSL ES */
    /* GL logging (diag tela preta). glClearColor NÃO aqui -> cai no
     * softfp_resolve (sf_glClearColor) por causa do ABI float softfp/hardfp. */
    {"glBindFramebuffer", (uintptr_t)my_glBindFramebuffer},
    {"glClear", (uintptr_t)my_glClear},
    {"glDrawElements", (uintptr_t)my_glDrawElements},
    {"glDrawArrays", (uintptr_t)my_glDrawArrays},
    {"slCreateEngine", (uintptr_t)slCreateEngine_shim},
    {"SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE_v},
    {"SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY_v},
    {"SL_IID_RECORD", (uintptr_t)&SL_IID_RECORD_v},
    {"SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME_v},
    {"SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE_v},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE_v},
    {"SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&SL_IID_ANDROIDCONFIGURATION_v},
};
const int shantae_overrides_count =
    sizeof(shantae_overrides) / sizeof(shantae_overrides[0]);
