/*
 * main_ducktales.c -- DuckTales: Remastered (WayForward, NativeActivity, armv7)
 *                     so-loader -> Mali-450 / Linux fbdev via SDL2.
 *
 * libducktales exports android_main(struct android_app*) + JNI_OnLoad and
 * imports the NDK glue funcs (ALooper/AInputQueue/AAsset/AConfiguration/
 * ANativeWindow). We provide a fake android_app (android_shim) and call
 * android_main directly -- the Syberia/RE4 model.
 *
 * fmod audio: libducktales NEEDED libfmodex.so + libfmodevent.so. We load
 * both as so_modules; libducktales' FMOD::* imports resolve against them via
 * dt_fmod_lookup(). fmod's OpenSL backend dlopen("libOpenSLES.so") is made to
 * fail so fmod falls back to org.fmod.FMODAudioDevice (jni_shim bridge->pulse).
 */
#include <signal.h>
#include <setjmp.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <ucontext.h>

#include "android_shim.h"
#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

extern void egl_shim_create_window(void);

/* --- stubs for symbols the RE4 shims expect from main_re4 (Unity-specific) --- */
void re4_signal_gameplay(int on) { (void)on; }
void my_exit(int code) { _exit(code); }
const char *re4_addr_mod(uintptr_t a) { (void)a; return "?"; }
int g_gameplay = 0, g_gameplay_frame = 0, g_re4_frame = 0;
int re4_in_unity(uintptr_t a) { (void)a; return 0; }
void re4_frame_end_present(void) {}
void *re4_gl_override(const char *procname) { (void)procname; return NULL; }

extern void hook_arm(uintptr_t addr, uintptr_t dst);
extern int egl_shim_frame_count(void);

/* ---- timestamped log line to stderr (launcher tees to logs/) ---- */
static void tslog(const char *tag, const char *msg) {
  time_t t = time(NULL);
  struct tm tmv;
  localtime_r(&t, &tmv);
  char ts[32];
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
  fprintf(stderr, "[%s] [%s] %s\n", ts, tag, msg);
  fflush(stderr);
}

/* ---- WATCHDOG: hard self-exit so the game can never wedge the device ----
   DUCK_MAXSECONDS=N force-exits after N seconds. Heartbeat every 5s reports
   elapsed time + GL frame count so a stuck run is visible in the logs. */
static volatile int g_wd_seconds = 0;
static void *watchdog_thread(void *arg) {
  (void)arg;
  int last_frames = -1, stuck = 0;
  for (int i = 1; i <= g_wd_seconds; i++) {
    sleep(1);
    if (i % 5 == 0) {
      int f = egl_shim_frame_count();
      char m[128];
      snprintf(m, sizeof(m), "alive %ds/%ds frames=%d", i, g_wd_seconds, f);
      tslog("watchdog", m);
      if (f == last_frames && f > 0) {
        if (++stuck >= 3) { tslog("watchdog", "RENDER STUCK (no new frames 15s) -> force exit"); _exit(42); }
      } else stuck = 0;
      last_frames = f;
    }
  }
  tslog("watchdog", "MAX_SECONDS reached -> force exit");
  _exit(0);
}
static void start_watchdog(void) {
  const char *s = getenv("DUCK_MAXSECONDS");
  if (!s) return;
  g_wd_seconds = atoi(s);
  if (g_wd_seconds <= 0) return;
  pthread_t th;
  pthread_create(&th, NULL, watchdog_thread, NULL);
  pthread_detach(th);
  char m[64]; snprintf(m, sizeof(m), "armed: %ds", g_wd_seconds);
  tslog("watchdog", m);
}

/* The NVIDIA gamepad helper enumerates axes/buttons via JNI reflection
   (GetObjectClass/GetMethodID/CallObjectMethod). Our fake JNIEnv returns
   garbage that gets dereferenced -> crash. Real input flows through
   AInputQueue/onInputEvent, so we bail these enum calls with 0 devices. */
static int nv_gamepad_enum_stub(void *env, void *obj, int *count) {
  (void)env; (void)obj;
  static int n = 0; if (n++ < 40) fprintf(stderr, "[NVPAD] axes enum stub (count->0) ra=%p\n",
      __builtin_return_address(0));
  if (count) *count = 0;
  return 0;
}
/* NvGetGamepadButtons enumerates the pad's button CAPABILITIES (called at init):
   the Java gamepadButtonIndices() returns the list of supported Android keycodes;
   our fake JNI can't, so we replace the whole function and return the standard
   Android gamepad keycode set. Returning count=0 (old stub) registered NO buttons
   -> the engine dropped every gamepad key event (handled=0). The array is freed by
   the caller via operator delete[] (=glibc free), so plain malloc is compatible.
   DUCK_NV0BASED=1 returns 0-based indices instead of keycodes (fallback if the
   engine expects dense indices). */
static int nv_get_buttons(void *env, void *obj, int *count) {
  (void)env; (void)obj;
  static const int kc[] = {
    19, 20, 21, 22, 23,        /* DPAD up,down,left,right,center */
    96, 97, 99, 100,           /* A,B,X,Y */
    102, 103, 104, 105,        /* L1,R1,L2,R2 */
    106, 107, 108, 109, 110    /* THUMBL,THUMBR,START,SELECT,MODE */
  };
  int n = (int)(sizeof(kc) / sizeof(kc[0]));
  int *arr = (int *)malloc((size_t)n * sizeof(int));
  if (!arr) { if (count) *count = 0; return 0; }
  int zerobased = getenv("DUCK_NV0BASED") != NULL;
  for (int i = 0; i < n; i++) arr[i] = zerobased ? i : kc[i];
  if (count) *count = n;
  static int once = 0; if (!once) { once = 1; fprintf(stderr, "[NVPAD] buttons enum -> %d caps (zerobased=%d)\n", n, zerobased); }
  return (int)(intptr_t)arr;
}

/* ---- GL texture upload logger (DUCK_GLTEXLOG=1) ----
   Discriminate the black background: a too-large image (w/h > GL_MAX_TEXTURE_SIZE
   4096 on Mali-450 -> glTexImage2D fails GL_INVALID_VALUE -> empty/black texture),
   a compressed format the Mali rejects (glCompressedTexImage2D), or a per-frame
   video (repeated glTexSubImage2D on one texture). Logs size/format/glError. */
static int g_gltexlog = 0;
extern void glTexImage2D(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
extern void glCompressedTexImage2D(unsigned, int, unsigned, int, int, int, int, const void *);
extern void glTexSubImage2D(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
extern unsigned glGetError(void);
static int upload_nonblack_pct(int w, int h, unsigned fmt, unsigned typ, const void *px) {
  if (!px || w <= 0 || h <= 0) return -1;
  /* only handle 8-bit unsigned byte rgba/rgb/luminance */
  if (typ != 0x1401 /*GL_UNSIGNED_BYTE*/) return -1;
  int bpp = (fmt == 0x1908 || fmt == 0x80E1) ? 4 : (fmt == 0x1907) ? 3 : (fmt == 0x1906 || fmt == 0x1909) ? 1 : 0;
  if (!bpp) return -1;
  const unsigned char *p = px; long nb = 0; long total = (long)w * h;
  long step = total > 100000 ? total / 100000 : 1;   /* sample */
  long sampled = 0;
  for (long i = 0; i < total; i += step) {
    const unsigned char *q = p + i * bpp; int v = 0;
    for (int c = 0; c < (bpp >= 3 ? 3 : 1); c++) v |= q[c];
    if (v) nb++; sampled++;
  }
  return sampled ? (int)(nb * 100 / sampled) : -1;
}
void drt_set_dim(int w, int h);
void drt_set_fmt(int fmt);
static void my_glTexImage2D(unsigned t, int l, int ifmt, int w, int h, int b, unsigned fmt, unsigned typ, const void *px) {
  glTexImage2D(t, l, ifmt, w, h, b, fmt, typ, px);
  if (l == 0) { drt_set_dim(w, h); drt_set_fmt(fmt); }
  if (g_gltexlog) { unsigned e = glGetError(); static int n = 0;
    if (n < 400 && (w >= 256 || h >= 256 || e)) { n++;
      int nbp = upload_nonblack_pct(w, h, fmt, typ, px);
      fprintf(stderr, "[GLTEX] %dx%d lvl=%d ifmt=0x%x fmt=0x%x type=0x%x px=%s nonblack=%d%% err=0x%x\n",
              w, h, l, ifmt, fmt, typ, px ? "data" : "NULL", nbp, e); } }
}
static void my_glCompressedTexImage2D(unsigned t, int l, unsigned ifmt, int w, int h, int b, int sz, const void *px) {
  glCompressedTexImage2D(t, l, ifmt, w, h, b, sz, px);
  if (l == 0) { drt_set_dim(w, h); drt_set_fmt(ifmt); }
  if (g_gltexlog) { unsigned e = glGetError(); static int n = 0;
    if (n < 400) { n++; fprintf(stderr, "[GLCTEX] %dx%d lvl=%d ifmt=0x%x size=%d err=0x%x\n", w, h, l, ifmt, sz, e); } }
}
static void my_glTexSubImage2D(unsigned t, int l, int x, int y, int w, int h, unsigned fmt, unsigned typ, const void *px) {
  glTexSubImage2D(t, l, x, y, w, h, fmt, typ, px);
  if (g_gltexlog) { static int n = 0; if (n < 200 && (w >= 256 || h >= 256)) { n++;
      int nbp = upload_nonblack_pct(w, h, fmt, typ, px);
      fprintf(stderr, "[GLSUB] %dx%d @%d,%d fmt=0x%x type=0x%x nonblack=%d%%\n", w, h, x, y, fmt, typ, nbp); } }
}
/* ---- FBO/render-target logger (DUCK_GLFBLOG=1) ----
   Scaleform renders the animated background to an FBO (render target) then
   composites it. If that FBO is incomplete on Mali-450, the bg is black while
   the directly-drawn UI shows. Log every framebuffer-complete check != COMPLETE
   and count binds/draws. */
static int g_glfblog = 0;
extern unsigned glCheckFramebufferStatus(unsigned);
extern void glBindFramebuffer(unsigned, unsigned);
extern void glFramebufferTexture2D(unsigned, unsigned, unsigned, unsigned, int);
static unsigned my_glCheckFramebufferStatus(unsigned target) {
  unsigned s = glCheckFramebufferStatus(target);
  if (g_glfblog && s != 0x8CD5) { static int n = 0; if (n++ < 60)
      fprintf(stderr, "[FBO] INCOMPLETE status=0x%x\n", s); }
  return s;
}
static void my_glBindFramebuffer(unsigned target, unsigned fb) {
  glBindFramebuffer(target, fb);
  if (g_glfblog) { static int n = 0; if (n++ < 40) fprintf(stderr, "[FBO] bind %u\n", fb); }
}
static void my_glFramebufferTexture2D(unsigned t, unsigned att, unsigned tt, unsigned tex, int lvl) {
  glFramebufferTexture2D(t, att, tt, tex, lvl);
  if (g_glfblog) { static int n = 0; if (n++ < 40) fprintf(stderr, "[FBO] attach tex=%u att=0x%x\n", tex, att); }
}

/* ---- shader compile/link logger (DUCK_GLSHLOG=1) ----
   The black background = textured/bitmap fills produce black while vector/text
   render. If the bitmap-fill SHADER fails to compile or link, that's the cause
   (and is deterministic / fixable independently of the heap UAF). Log every
   compile/link that FAILS, with the info-log and (for compiles) the source. */
static int g_glshlog = 0;
extern void glShaderSource(unsigned, int, const char *const *, const int *);
extern void glCompileShader(unsigned);
extern void glGetShaderiv(unsigned, unsigned, int *);
extern void glGetShaderInfoLog(unsigned, int, int *, char *);
extern void glLinkProgram(unsigned);
extern void glGetProgramiv(unsigned, unsigned, int *);
extern void glGetProgramInfoLog(unsigned, int, int *, char *);
extern void glGetShaderSource(unsigned, int, int *, char *);
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS    0x8B82
/* DUCK_SHADERFIX: A/B test — is the black bg a tint/color-path bug? Rewrite the
   textured-fill fragment shader to output the raw texture (ignore g_tint/g_add).
   If the bg/sprites then appear, the modulate color (g_tint) is wrong/zero. */
static int g_shaderfix = 0;
static void my_glShaderSource(unsigned sh, int cnt, const char *const *str, const int *len) {
  if (g_shaderfix && cnt >= 1 && str && str[0]) {
    const char *s = str[0];
    int frag = strstr(s, "gl_FragData") || strstr(s, "gl_FragColor");
    int textured = strstr(s, "g_textureSampler") != NULL;
    int istint = strstr(s, "g_tint") != NULL;   /* textured-fill variant uses g_tint */
    if (frag && textured && istint) {
      /* find the varying vec2 name (texcoord) */
      const char *vn = "xlv_TEXCOORD0";
      static char rb[512];
      snprintf(rb, sizeof(rb),
        "uniform sampler2D g_textureSampler;\n"
        "varying highp vec2 %s;\n"
        "void main(){ lowp vec4 c = texture2D(g_textureSampler, %s);\n"
        "  gl_FragData[0] = vec4(c.xyz, 1.0); }\n", vn, vn);
      const char *one = rb; int onelen = (int)strlen(rb);
      glShaderSource(sh, 1, &one, &onelen);
      static int n=0; if (n++<8) fprintf(stderr, "[SHADERFIX] rewrote textured frag shader %u to passthrough\n", sh);
      return;
    }
  }
  glShaderSource(sh, cnt, str, len);
  if (g_glshlog) {
    static int n = 0;
    if (n < 60) { n++;
      fprintf(stderr, "[SHSRC] shader=%u count=%d:\n", sh, cnt);
      for (int i = 0; i < cnt && i < 4; i++) fprintf(stderr, "----\n%.2000s\n[ENDSRC]\n", str[i] ? str[i] : "(null)");
    }
  }
}
static void my_glCompileShader(unsigned sh) {
  glCompileShader(sh);
  if (g_glshlog) {
    int ok = 1; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[1024] = {0}; glGetShaderInfoLog(sh, sizeof(log)-1, NULL, log);
      char src[2048] = {0}; glGetShaderSource(sh, sizeof(src)-1, NULL, src);
      fprintf(stderr, "[SHFAIL] shader=%u COMPILE FAILED:\n%s\n--- src ---\n%s\n", sh, log, src);
    } else { static int n=0; if (n++<20) fprintf(stderr, "[SHOK] shader=%u compiled\n", sh); }
  }
}
extern void glAttachShader(unsigned prog, unsigned sh);
static void my_glAttachShader(unsigned prog, unsigned sh) {
  glAttachShader(prog, sh);
  if (g_glshlog) { static int n=0; if (n++<60) fprintf(stderr, "[ATTACH] prog=%u shader=%u\n", prog, sh); }
}
static void my_glLinkProgram(unsigned pr) {
  glLinkProgram(pr);
  if (g_glshlog) {
    int ok = 1; glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]={0}; glGetProgramInfoLog(pr, sizeof(log)-1, NULL, log);
      fprintf(stderr, "[PRFAIL] program=%u LINK FAILED:\n%s\n", pr, log); }
    else { static int n=0; if (n++<20) fprintf(stderr, "[PROK] program=%u linked\n", pr); }
  }
}

/* ---- mipmap min-filter clamp (DUCK_NOMIP=1) ----
   A large textured fill that renders BLACK while small bitmaps (the logo) render
   is the classic "incomplete texture" symptom: the texture's MIN_FILTER asks for
   mipmaps (GL_*_MIPMAP_*) but no mip levels were uploaded and glGenerateMipmap
   wasn't called -> the texture is incomplete -> samples (0,0,0,0) = black on
   Mali-450. Force every mipmap min-filter down to GL_LINEAR (no mip needed). */
static int g_nomip = 0;
static int g_texparlog = 0;
extern void glTexParameteri(unsigned target, unsigned pname, int param);
extern void glGenerateMipmap(unsigned target);
static void my_glTexParameteri(unsigned target, unsigned pname, int param) {
  if (g_nomip && pname == 0x2801 /*GL_TEXTURE_MIN_FILTER*/) {
    if (param == 0x2700 || param == 0x2701 || param == 0x2702 || param == 0x2703) {
      /* mipmap filter -> plain linear */
      param = 0x2601; /*GL_LINEAR*/
    }
  }
  if (g_texparlog && pname == 0x2801) { static int n=0; if (n++<40)
      fprintf(stderr, "[TEXPAR] MIN_FILTER=0x%x\n", param); }
  glTexParameteri(target, pname, param);
}
static int g_genmip_count = 0;
static void my_glGenerateMipmap(unsigned target) {
  glGenerateMipmap(target);
  if (g_texparlog) g_genmip_count++;
}

/* ---- draw-call tracer (DUCK_DRAWLOG=1) ----
   The menu logo (bitmap) renders but the Duckburg bg never does. Find out if the
   bg's large texture is even bound+drawn: track each texture's dims (from
   glTexImage2D under the bound id) and, per draw call, count draws by texture
   size class. If a big (>=512) texture is never drawn -> the bg movie clip's
   display list is broken (load/UAF). If it IS drawn but black -> a state issue. */
static int g_drawlog = 0;
extern void glBindTexture(unsigned target, unsigned tex);
extern void glDrawElements(unsigned mode, int count, unsigned type, const void *idx);
extern void glDrawArrays(unsigned mode, int first, int count);
#define DRT_N 4096
static unsigned short g_texw[DRT_N], g_texh[DRT_N], g_texfmt[DRT_N];
static _Thread_local unsigned g_bound_tex = 0;
static unsigned g_draw_by_class[6];   /* <64,128,256,512,1024,>=2048 by max dim */
static unsigned g_draw_total = 0, g_draw_big = 0, g_bigtex_last = 0;
/* log sampler-uniform assignments (glUniform1i) to see if g_textureSamplerA is
   ever set to unit 1. If samplers stay at the default 0, the textured-fill color
   read comes from the alpha texture on unit 0 -> black. */
extern void glUniform1i(int loc, int v);
extern int glGetUniformLocation(unsigned prog, const char *name);
static int g_u1ilog = 0;
static void my_glUniform1i(int loc, int v) {
  glUniform1i(loc, v);
  if (g_u1ilog) { static int n=0; if (n++<80) fprintf(stderr, "[U1I] loc=%d val=%d\n", loc, v); }
}
static int my_glGetUniformLocation(unsigned prog, const char *name) {
  int loc = glGetUniformLocation(prog, name);
  if (g_u1ilog && name && strstr(name, "ampler")) { static int n=0; if (n++<40)
      fprintf(stderr, "[ULOC] prog=%u %s -> loc=%d\n", prog, name, loc); }
  return loc;
}
static _Thread_local unsigned g_active_unit = 0;
static _Thread_local unsigned g_unit_tex[8] = {0};
extern void glActiveTexture(unsigned unit);
static void my_glActiveTexture(unsigned unit) {
  glActiveTexture(unit);
  unsigned u = unit - 0x84C0; if (u < 8) g_active_unit = u;
}
static void my_glBindTexture(unsigned target, unsigned tex) {
  glBindTexture(target, tex);
  if (target == 0x0DE1) { g_bound_tex = tex; if (g_active_unit < 8) g_unit_tex[g_active_unit] = tex; }   /* GL_TEXTURE_2D */
}
void drt_set_dim(int w, int h) {
  unsigned t = g_bound_tex; if (t < DRT_N) { g_texw[t] = (unsigned short)w; g_texh[t] = (unsigned short)h; }
}
void drt_set_fmt(int fmt) {
  unsigned t = g_bound_tex; if (t < DRT_N) g_texfmt[t] = (unsigned short)fmt;
}
static void drt_record(void) {
  unsigned t = g_bound_tex; if (t >= DRT_N) { g_draw_total++; return; }
  int mx = g_texw[t] > g_texh[t] ? g_texw[t] : g_texh[t];
  int c = mx >= 2048 ? 5 : mx >= 1024 ? 4 : mx >= 512 ? 3 : mx >= 256 ? 2 : mx >= 128 ? 1 : 0;
  g_draw_by_class[c < 6 ? c : 5]++;
  g_draw_total++;
  if (mx >= 512) { g_draw_big++; g_bigtex_last = t; }
  if ((g_draw_total % 2000) == 0)
    fprintf(stderr, "[DRAW] total=%u big(>=512)=%u lastbig=tex%u(%dx%d)  class[<64,128,256,512,1024,2k]=%u,%u,%u,%u,%u,%u\n",
            g_draw_total, g_draw_big, g_bigtex_last, g_texw[g_bigtex_last], g_texh[g_bigtex_last],
            g_draw_by_class[0],g_draw_by_class[1],g_draw_by_class[2],g_draw_by_class[3],g_draw_by_class[4],g_draw_by_class[5]);
}
/* DUCK_DRAWSTOP=N: execute only the first N draw calls each frame (skip the
   rest) -> bisect the frame to find the draw that covers the bg with black. */
static int g_drawstop = 0;
static _Thread_local int g_frame_seen = -1;
static int g_draw_in_frame = 0;
static int drawstop_skip(void) {
  if (!g_drawstop) return 0;
  int fc = egl_shim_frame_count();
  if (fc != g_frame_seen) { g_frame_seen = fc; g_draw_in_frame = 0; }
  return (++g_draw_in_frame) > g_drawstop;
}
/* DUCK_DRAWDBG=1: on a menu frame, read the center pixel before/after each draw
   in a window and report the draw that turns it black -> the bg-covering draw. */
static int g_drawdbg = 0;
extern void glReadPixels(int,int,int,int,unsigned,unsigned,void*);
static int g_dbg_done = 0, g_dbg_started = 0, g_dbg_frame = 0;
extern unsigned char glIsEnabled(unsigned);
extern void glGetIntegerv(unsigned, int *);
/* dump details of draws 25..78 of ONE settled menu frame */
static void draw_dbg(const char *kind, int count) {
  if (!g_drawdbg || g_dbg_done) return;
  int fc = egl_shim_frame_count();
  if (fc != g_frame_seen) { g_frame_seen = fc; g_draw_in_frame = 0;
    if (g_dbg_started && fc != g_dbg_frame) { g_dbg_done = 1; return; } }
  int di = ++g_draw_in_frame;
  if (fc < 1150) return;
  if (!g_dbg_started) { g_dbg_started = 1; g_dbg_frame = fc; }
  if (di < 25 || di > 78) return;
  unsigned t = g_bound_tex;
  int blend = glIsEnabled(0x0BE2);
  int sf=0,df=0; glGetIntegerv(0x0BE1/*BLEND_DST*/,&df); glGetIntegerv(0x0BE0/*BLEND_SRC*/,&sf);
  int prog=0; glGetIntegerv(0x8B8D/*CURRENT_PROGRAM*/,&prog);
  unsigned u0=g_unit_tex[0], u1=g_unit_tex[1];
  fprintf(stderr, "[DD] frame %d draw#%d %s count=%d unit0=tex%u(%dx%d fmt=0x%x) unit1=tex%u(%dx%d fmt=0x%x) blend=%d prog=%d\n",
          fc, di, kind, count, u0, u0<DRT_N?g_texw[u0]:0, u0<DRT_N?g_texh[u0]:0, u0<DRT_N?g_texfmt[u0]:0,
          u1, u1<DRT_N?g_texw[u1]:0, u1<DRT_N?g_texh[u1]:0, u1<DRT_N?g_texfmt[u1]:0, blend, prog);
  (void)t;(void)sf;(void)df;
}
/* ---- FIX: set sampler uniforms (DUCK_FIXSAMPLERS, default ON) ----
   The engine never calls glUniform1i, so the GFx shaders' sampler uniforms stay
   at the default unit 0. The textured-fill shader reads COLOR from g_textureSampler
   and ALPHA from g_textureSamplerA; the engine binds the color texture and the
   GL_ALPHA texture to units 0/1 in EITHER order. When the alpha texture lands on
   unit 0, g_textureSampler (default 0) samples it -> RGB black (the black bg/sprites).
   Fix: before each draw, point g_textureSampler at the unit holding the non-alpha
   (color) texture and g_textureSamplerA at the GL_ALPHA unit; g_lightSampler->2. */
static int g_fixsamplers = 1, g_fixdbg = 0;
extern unsigned glGetError(void);
static int g_loc_color[DRT_N], g_loc_alpha[DRT_N], g_loc_light[DRT_N], g_loc_known[DRT_N];
static _Thread_local unsigned g_cur_prog = 0;
static unsigned g_useprog_n=0, g_fix_n=0, g_fix_nop=0, g_fix_nosamp=0;
static void my_glUseProgram(unsigned p) { extern void glUseProgram(unsigned); glUseProgram(p); g_cur_prog = p; g_useprog_n++; }
static void fix_samplers(void) {
  g_fix_n++;
  unsigned p = g_cur_prog; if (!p || p >= DRT_N) { g_fix_nop++; return; }
  if (!g_loc_known[p]) {
    g_loc_known[p] = 1;
    g_loc_color[p] = glGetUniformLocation(p, "g_textureSampler");
    g_loc_alpha[p] = glGetUniformLocation(p, "g_textureSamplerA");
    g_loc_light[p] = glGetUniformLocation(p, "g_lightSampler");
  }
  if (g_loc_color[p] < 0 && g_loc_alpha[p] < 0) { g_fix_nosamp++; return; }   /* no samplers in this prog */
  /* which of units 0/1 holds the color (non-GL_ALPHA) texture? */
  unsigned t0 = g_unit_tex[0], t1 = g_unit_tex[1];
  int a0 = (t0 < DRT_N && g_texfmt[t0] == 0x1906);
  int a1 = (t1 < DRT_N && g_texfmt[t1] == 0x1906);
  int colorUnit = 0, alphaUnit = 1;
  if (a0 && !a1) { colorUnit = 1; alphaUnit = 0; }     /* alpha on 0 -> color on 1 */
  else { colorUnit = 0; alphaUnit = 1; }
  if (g_loc_color[p] >= 0) glUniform1i(g_loc_color[p], colorUnit);
  if (g_loc_alpha[p] >= 0) glUniform1i(g_loc_alpha[p], alphaUnit);
  if (g_loc_light[p] >= 0) glUniform1i(g_loc_light[p], 2);
  if (g_fixdbg) {
    int fc2 = egl_shim_frame_count();
    if (fc2 >= 1150 && fc2 <= 1153 && g_loc_color[p] >= 0) { static int n=0; if (n++<30)
      fprintf(stderr, "[FIXU] prog=%u u0=tex%u(fmt0x%x) u1=tex%u(fmt0x%x) -> colorUnit=%d\n",
              p, t0, t0<DRT_N?g_texfmt[t0]:0, t1, t1<DRT_N?g_texfmt[t1]:0, colorUnit); }
    if (colorUnit == 1) { static int n=0; if (n++<6)
      fprintf(stderr, "[FIXSAMP] prog=%u colorUnit=1 locC=%d locA=%d frame=%d\n",
              p, g_loc_color[p], g_loc_alpha[p], egl_shim_frame_count()); }
    if ((g_fix_n % 5000)==0)
      fprintf(stderr, "[FIXSTAT] fix_calls=%u useprog=%u nop(curprog0)=%u nosamp=%u frame=%d\n",
              g_fix_n, g_useprog_n, g_fix_nop, g_fix_nosamp, egl_shim_frame_count());
  }
}
static void my_glDrawElements(unsigned mode, int count, unsigned type, const void *idx) {
  if (g_drawlog) drt_record();
  if (g_drawdbg) draw_dbg("elem", count);
  if (g_fixsamplers) fix_samplers();
  if (drawstop_skip()) return;
  glDrawElements(mode, count, type, idx);
}
static void my_glDrawArrays(unsigned mode, int first, int count) {
  if (g_drawlog) drt_record();
  if (g_drawdbg) draw_dbg("arr", count);
  if (g_fixsamplers) fix_samplers();
  if (drawstop_skip()) return;
  glDrawArrays(mode, first, count);
}

/* ---- stencil-mask A/B test (DUCK_NOSTENCIL=1) ----
   Scaleform clips movie-clip content (the animated bg) with a STENCIL mask. If
   the default framebuffer has no usable stencil on Mali (or the mask render
   fails), the masked content is clipped away -> black, while unmasked UI shows.
   Force every stencil test to ALWAYS pass and disable stencil test -> if the bg
   appears, the mask was the culprit. */
static int g_nostencil = 0;
extern void glStencilFunc(unsigned func, int ref, unsigned mask);
extern void glEnable(unsigned cap);
extern void glDisable(unsigned cap);
static void my_glStencilFunc(unsigned func, int ref, unsigned mask) {
  (void)func; (void)ref; (void)mask;
  glStencilFunc(0x0207 /*GL_ALWAYS*/, 0, 0xFF);
}
static void my_glEnable(unsigned cap) {
  if (cap == 0x0B90 /*GL_STENCIL_TEST*/) return;   /* keep it disabled */
  glEnable(cap);
}

/* wfSystem::GetCpuCount() drives both the worker-thread count and the
   barrier in wfMCP::Exec. Forcing it keeps spawn and barrier consistent so we
   can run with N workers (e.g. 1 = serial, to separate races from data bugs). */
static int g_forced_cpucount = 0;
static int forced_cpucount(void) { return g_forced_cpucount; }

/* --- Serialize wfJob::Exec across the 10 worker threads ---
   The engine's job system has data races that are latent on Android's
   scheduler but fatal here (concurrent wfJob::Exec corrupts shared parse
   state -> NULL/garbage pointers). We keep all 10 threads (fewer deadlock on
   job dependencies) but let only ONE job body run at a time via a global lock.
   Implemented as an inline hook + trampoline that replays the 2 clobbered
   prologue instructions then continues into the original function. */
static pthread_mutex_t g_jobexec_lock;
static void (*g_wfjob_tramp)(void *) = NULL;
static int g_serial_after = 0;   /* DUCK_SERIAL_AFTER=N: serialize jobs once frames>=N */
static void my_wfjob_exec(void *job) {
  /* The INITIAL asset load needs concurrent jobs (the coordinator waits on all
     10 -> serializing deadlocks). But the MENU load (~frame 750) has an engine
     data race in independent JPEG/Scaleform jobs that corrupts the heap free-list.
     So serialize jobs ONLY after the initial load (frames >= g_serial_after). */
  if (g_serial_after && egl_shim_frame_count() >= g_serial_after) {
    pthread_mutex_lock(&g_jobexec_lock);
    g_wfjob_tramp(job);
    pthread_mutex_unlock(&g_jobexec_lock);
  } else {
    g_wfjob_tramp(job);
  }
}
static void install_jobexec_serializer(void) {
  const char *sa = getenv("DUCK_SERIAL_AFTER");
  if (sa) g_serial_after = atoi(sa);
  if (!getenv("DUCK_SERIAL_JOBS") && !g_serial_after) return;
  { pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_jobexec_lock, &a);
    pthread_mutexattr_destroy(&a); }
  uintptr_t a = so_find_addr_safe("_ZN5wfJob4ExecEP11wfJobThread");
  if (!a) { debugPrintf("[serial] wfJob::Exec not found\n"); return; }
  uint32_t *orig = (uint32_t *)a;
  uint32_t i0 = orig[0], i1 = orig[1];
  uint32_t *tr = (uint32_t *)mmap(NULL, 32, PROT_READ | PROT_WRITE | PROT_EXEC,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr == MAP_FAILED) return;
  tr[0] = i0; tr[1] = i1; tr[2] = 0xe51ff004u; tr[3] = (uint32_t)(a + 8);
  __builtin___clear_cache((char *)tr, (char *)tr + 16);
  g_wfjob_tramp = (void (*)(void *))tr;
  hook_arm(a, (uintptr_t)my_wfjob_exec);
  debugPrintf("[serial] wfJob::Exec serialized\n");
}

/* --- Lock the engine's internal (dlmalloc-style) pool allocator ---
   At file offsets 0x9392a4 (malloc) / 0x939e04 (free) there is a bundled
   size-classed free-list allocator with NO internal locking. Under our 10
   worker threads its doubly-linked free lists corrupt (unlink writes through
   garbage prev/next -> SIGSEGV). glibc malloc is already thread-safe; this
   private pool is not. We wrap malloc+free with one recursive global lock.
   Locking an allocator can't deadlock (it never waits on other jobs). */
/* 8-arg signature: these internal allocator functions read stack-passed args
   (e.g. ldr r4,[sp,#76] = 6th arg). Forwarding 8 args keeps the stack layout
   identical for the trampoline so its [sp,#NN] arg reads stay valid. */
typedef void *(*alloc_fn8)(void *, void *, void *, void *, void *, void *, void *, void *);
static alloc_fn8 g_pool_malloc_tramp = NULL;
static alloc_fn8 g_pool_free_tramp = NULL;
/* recursive spinlock — async-signal-safe (the allocator may be reached from a
   signal handler; pthread_mutex there would deadlock). Allocator ops are short. */
static uintptr_t g_main_text, g_main_text_sz;   /* load base of libducktales; set in main() */
static int g_heaplock = 0;             /* serialize the whole GFx allocator (low+high) */
static pthread_mutex_t g_heap_mtx;     /* recursive; init'd when heaplock on */
static __thread int g_alock_depth = 0; /* this thread's recursion count on g_heap_mtx */
static volatile long g_pool_owner = 0;
static volatile int g_pool_depth = 0;
/* alock/aunlock track depth so the crash handler can fully release the recursive
   mutex before siglongjmp'ing out of a corrupted free (the free-guard). */
static void alock(void) { pthread_mutex_lock(&g_heap_mtx); g_alock_depth++; }
static void aunlock(void) { g_alock_depth--; pthread_mutex_unlock(&g_heap_mtx); }
static void pool_lock(void) {
  if (g_heaplock) { alock(); return; }  /* unified lock w/ high-level */
  long me = (long)pthread_self();
  if (g_pool_owner == me) { g_pool_depth++; return; }
  while (__sync_val_compare_and_swap(&g_pool_owner, 0, me) != 0) { /* spin */ }
  g_pool_depth = 1;
}
static void pool_unlock(void) {
  if (g_heaplock) { aunlock(); return; }
  if (--g_pool_depth == 0) { __sync_synchronize(); g_pool_owner = 0; }
}
static void df_alloced(void *p);
static int g_df_on = 0;
static int g_nofree = 0;   /* DUCK_NOFREE: skip the bucket free entirely (leak) */

/* ---- ASAN-lite allocation tracker (DUCK_ATRACK=1) ----
   Records every GFx-heap allocation (chunk ptr + a shallow engine backtrace).
   At the crash, at_dump_near(addr) finds the allocation physically just-below a
   corrupted address = the buffer that overflowed into the free-list, and prints
   its allocation backtrace -> the engine code (asset parser) that overran. */
static int g_atrack = 0;
#define AT_N (1u << 20)                      /* 1M entries, open-addressing */
typedef struct { void *ptr; uint32_t bt[6]; } at_ent;
static at_ent g_at[AT_N];
/* ---- alloc HISTORY (DUCK_HIST=1) ----
   Keyed by chunk addr (direct-mapped), NOT cleared on free. Records the last
   allocation backtrace for each TLSF chunk address so that when a freed block's
   free-list link is found zeroed (the UAF), we can print WHO ALLOCATED THAT
   EXACT BLOCK = the object whose stale pointer later zeroes it. Captured at the
   TLSF malloc level (pool_malloc_wrap) so the key == the free-list node addr. */
static int g_hist = 0;
#define HIST_N (1u << 20)
static at_ent g_histtab[HIST_N];
static void at_capture_bt(uint32_t *bt) {
  uintptr_t sp; __asm__ volatile("mov %0, sp" : "=r"(sp));
  uintptr_t *s = (uintptr_t *)(sp & ~3u);
  int n = 0;
  for (int i = 0; i < 384 && n < 6; i++) {
    uintptr_t v = s[i];
    if (v >= g_main_text && v < g_main_text + g_main_text_sz) bt[n++] = (uint32_t)(v - g_main_text);
  }
  for (; n < 6; n++) bt[n] = 0;
}
static void at_insert(void *p) {
  if (!p) return;
  unsigned idx = ((uintptr_t)p >> 4) & (AT_N - 1);
  for (int i = 0; i < 256; i++) {
    unsigned j = (idx + i) & (AT_N - 1);
    if (g_at[j].ptr == NULL || g_at[j].ptr == p) { g_at[j].ptr = p; at_capture_bt(g_at[j].bt); return; }
  }
}
static void hist_put(void *p) {
  if (!p) return;
  unsigned i = ((uintptr_t)p >> 4) & (HIST_N - 1);
  g_histtab[i].ptr = p; at_capture_bt(g_histtab[i].bt);
}
/* print the last-known allocation backtrace for chunk addr `p` (the UAF victim) */
static void hist_dump(const char *tag, void *p) {
  /* the free-list node is the chunk header; the recorded alloc key is the USER
     pointer (chunk + header). Probe a few offsets to find the owning alloc. */
  int offs[] = {0, 8, 16, 4, 12, -8, -16, 24, 32};
  for (unsigned o = 0; o < sizeof(offs)/sizeof(offs[0]); o++) {
    void *q = (char *)p + offs[o];
    unsigned i = ((uintptr_t)q >> 4) & (HIST_N - 1);
    if (g_histtab[i].ptr == q && g_histtab[i].bt[0]) {
      fprintf(stderr, "[HIST] %s node=%p (user=%p, +%d) allocated by:\n", tag, p, q, offs[o]);
      for (int k = 0; k < 6 && g_histtab[i].bt[k]; k++)
        fprintf(stderr, "         duck+0x%lx\n", (unsigned long)g_histtab[i].bt[k]);
      return;
    }
  }
  fprintf(stderr, "[HIST] %s node=%p no alloc record at any offset\n", tag, p);
}
static void at_remove(void *p) {
  if (!p) return;
  unsigned idx = ((uintptr_t)p >> 4) & (AT_N - 1);
  for (int i = 0; i < 256; i++) {
    unsigned j = (idx + i) & (AT_N - 1);
    if (g_at[j].ptr == p) { g_at[j].ptr = NULL; return; }
  }
}
/* find the live allocation with the greatest ptr <= addr (owner/just-below) */
static void at_dump_near(const char *tag, uintptr_t addr) {
  void *best = NULL; uint32_t *bestbt = NULL;
  for (unsigned j = 0; j < AT_N; j++) {
    void *p = g_at[j].ptr;
    if (!p) continue;
    if ((uintptr_t)p <= addr && (best == NULL || (uintptr_t)p > (uintptr_t)best)) {
      best = p; bestbt = g_at[j].bt;
    }
  }
  if (best) {
    fprintf(stderr, "[ATRACK] %s=0x%lx -> nearest alloc below = %p (delta=%ld)\n",
            tag, (unsigned long)addr, best, (long)(addr - (uintptr_t)best));
    fprintf(stderr, "         alloc backtrace:\n");
    for (int k = 0; k < 6 && bestbt[k]; k++) fprintf(stderr, "           duck+0x%lx\n", (unsigned long)bestbt[k]);
  } else {
    fprintf(stderr, "[ATRACK] %s=0x%lx -> no alloc below\n", tag, (unsigned long)addr);
  }
}

static void *pool_malloc_wrap(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  pool_lock();
  void *r = g_pool_malloc_tramp(a, b, c, d, e, f, g, h);
  if (g_df_on) df_alloced(r);
  if (g_hist) hist_put(r);
  pool_unlock();
  return r;
}
/* double-free detector: direct-mapped "currently-freed" table. free() marks the
   slot; if it's already the same chunk -> freed twice with no malloc between =
   real double-free. malloc() clears the slot (chunk live again). */
#define DF_SET 262144
static void *g_df_freed[DF_SET];
static int g_df_skip = 0;   /* DUCK_DFSKIP: drop a free() of an already-freed chunk */
/* scan the stack for return addresses inside libducktales -> engine call chain */
static void df_backtrace(void) {
  uintptr_t sp;
  __asm__ volatile("mov %0, sp" : "=r"(sp));
  uintptr_t *s = (uintptr_t *)(sp & ~3u);
  int shown = 0;
  for (int i = 0; i < 2048 && shown < 16; i++) {
    uintptr_t v = s[i];
    if (v >= g_main_text && v < g_main_text + g_main_text_sz) {
      fprintf(stderr, "    duck+0x%lx\n", (unsigned long)(v - g_main_text)); shown++;
    }
  }
}
/* returns 1 if this free is a double-free (chunk already marked freed) */
static int df_freed(void *p, void *caller) {
  if (!p) return 0;
  unsigned idx = ((uintptr_t)p >> 4) & (DF_SET - 1);
  int dup = (g_df_freed[idx] == p);
  if (dup) {
    static int n = 0;
    if (n++ < 12) {
      uintptr_t off = caller ? (uintptr_t)caller - g_main_text : 0;
      fprintf(stderr, "[DOUBLEFREE] chunk=%p caller=duck+0x%lx\n", p, (unsigned long)off);
      df_backtrace();
    }
  }
  g_df_freed[idx] = p;
  return dup;
}
static void df_alloced(void *p) {
  if (!p) return;
  unsigned idx = ((uintptr_t)p >> 4) & (DF_SET - 1);
  if (g_df_freed[idx] == p) g_df_freed[idx] = NULL;
}
/* ---- BUCKET-FREE QUARANTINE (ROOT FIX for the menu-load crash) ----
   ROOT CAUSE (found via gdb): the engine USE-AFTER-FREEs a bucket block — it frees
   a block (which goes onto the allocator's circular free-list, links stored IN the
   block's first 8 bytes) and then memset(0)/writes the stale pointer, ZEROING the
   free-list next ptr. A later free's unlink walks the circular list, hits the NULL
   next, and writes through it -> SIGSEGV (0x939dd8 str r12,[NULL+4]).
   FIX: delay the real bucket free by QBF entries. While a just-freed block waits in
   our quarantine it is NOT on the free-list, so the engine's stale write hits a
   block that no list traverses (harmless). We replay the real free only after QBF
   later frees, by which point the UAF write has happened. DEFAULT ON. */
static int g_bq = 0;                 /* DUCK_NO_BQ disables */
#define QBF (1u << 14)               /* 16384 deferred bucket frees */
static struct { void *a,*b,*c,*d,*e,*f,*g,*h; int used; } g_bq_ring[QBF];
static unsigned g_bq_head = 0;
static void *pool_free_wrap(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  pool_lock();
  if (g_nofree) { pool_unlock(); return NULL; }
  if (g_df_on) {
    int dup = df_freed(b, __builtin_return_address(0));
    if (dup && g_df_skip) { pool_unlock(); return NULL; }
  }
  if (g_bq) {
    unsigned i = g_bq_head; g_bq_head = (g_bq_head + 1) & (QBF - 1);
    void *ra=NULL,*rb=NULL,*rc=NULL,*rd=NULL,*re=NULL,*rf=NULL,*rg=NULL,*rh=NULL;
    int replay = g_bq_ring[i].used;
    if (replay) { ra=g_bq_ring[i].a; rb=g_bq_ring[i].b; rc=g_bq_ring[i].c; rd=g_bq_ring[i].d;
                  re=g_bq_ring[i].e; rf=g_bq_ring[i].f; rg=g_bq_ring[i].g; rh=g_bq_ring[i].h; }
    g_bq_ring[i].a=a; g_bq_ring[i].b=b; g_bq_ring[i].c=c; g_bq_ring[i].d=d;
    g_bq_ring[i].e=e; g_bq_ring[i].f=f; g_bq_ring[i].g=g; g_bq_ring[i].h=h; g_bq_ring[i].used=1;
    void *r = replay ? g_pool_free_tramp(ra, rb, rc, rd, re, rf, rg, rh) : NULL;
    pool_unlock();
    return r;
  }
  void *r = g_pool_free_tramp(a, b, c, d, e, f, g, h);
  pool_unlock();
  return r;
}
/* ATRACK hooks at the HIGH-level GMemoryHeap interface so BOTH the small-bucket
   (<2048) and large-block allocators are tracked. HeapAlloc=0x82bd78 (ret=r0),
   HeapFree=0x82bb84 (ptr=r1). The corrupted region is the large-block heap, only
   visible here. */
static alloc_fn8 g_at_alloc_tramp = NULL, g_at_free_tramp = NULL;
/* g_heaplock / g_heap_mtx declared near the top */
/* ---- free-guard ---- the residual menu-load heap overflow makes a free trip
   over a corrupted free-list (GFx raises, or a wild store faults). The guard
   wraps a free in sigsetjmp; on a fault/raise INSIDE the free, the crash handler
   releases the recursive lock and siglongjmp's back here -> abandon the corrupted
   free (block leaks) and continue. DEFAULT ON; DUCK_NO_FREEGUARD disables. */
static int g_free_guard = 0;
static __thread sigjmp_buf g_fg_jb;
static __thread volatile int g_fg_active = 0;
/* HIGH-LEVEL FREE DEDUP: the menu-load has an ENGINE data race -> two worker
   jobs free the SAME GFx object (logical double-free). The allocator lock
   serializes the two frees but can't stop the double-free, which smashes the
   free-list. Track currently-freed pointers at HeapFree; drop a repeat free of
   a not-yet-reallocated pointer. HeapAlloc clears the slot on reuse. DEFAULT ON. */
static int g_dedup = 0;            /* DUCK_NO_DEDUP disables */
#define HF_N (1u << 19)
static void *g_hf_freed[HF_N];
/* DUCK_HEAPSLACK=N: inflate every GMemoryHeap::Alloc (0x82bd78) request by N
   bytes. The menu-load corruption lands a single 4-byte write on a NEIGHBOR
   TLSF chunk's free-list next pointer (gdb: only [node+0] zeroed). Both the
   over-writing object and the victim free-list node are TLSF chunks in the
   SAME engine arena, so padding each chunk shifts neighbors apart -> the stray
   write lands in our pad (harmless). r1 (=arg b) is the plain byte size here
   (verified by disasm); free derives size from the chunk tag, so inflation is
   transparent. This is the HIGH-level entry (clean size), unlike the failed
   AllocWrap(0x9166bc) attempt whose arg was not a plain size. */
static size_t g_heapslack = 0;
static void *at_alloc_wrap(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  if (g_heaplock) alock();
  if (g_heapslack && b && (size_t)b < 0x40000000) b = (void *)(((size_t)b + g_heapslack));
  void *r = g_at_alloc_tramp(a, b, c, d, e, f, g, h);
  if (g_dedup && r) { unsigned i = ((uintptr_t)r >> 4) & (HF_N - 1); if (g_hf_freed[i] == r) g_hf_freed[i] = NULL; }
  if (g_atrack) { pool_lock(); at_insert(r); pool_unlock(); }
  if (g_heaplock) aunlock();
  return r;
}
static void *at_free_body(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  if (g_heaplock) alock();
  if (g_dedup && b) {
    unsigned i = ((uintptr_t)b >> 4) & (HF_N - 1);
    if (g_hf_freed[i] == b) {       /* freed again with no realloc -> double-free, DROP */
      static int n = 0; if (n++ < 20) fprintf(stderr, "[DEDUP] dropped double HeapFree %p\n", b);
      if (g_heaplock) aunlock();
      return NULL;
    }
    g_hf_freed[i] = b;
  }
  if (g_atrack) { pool_lock(); at_remove(b); pool_unlock(); }
  void *r = g_at_free_tramp(a, b, c, d, e, f, g, h);
  if (g_heaplock) aunlock();
  return r;
}
static void *at_free_wrap(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  if (g_free_guard && g_heaplock && !g_fg_active) {
    g_fg_active = 1;
    if (sigsetjmp(g_fg_jb, 1) != 0) { g_fg_active = 0; return NULL; }  /* recovered */
    void *r = at_free_body(a, b, c, d, e, f, g, h);
    g_fg_active = 0;
    return r;
  }
  return at_free_body(a, b, c, d, e, f, g, h);
}
/* FreeWrap (0x915d64) and AllocWrap (0x9166bc) manipulate the heap's boundary
   tags OUTSIDE the 0x9392a4/0x938bec calls we already lock -> those tag writes
   race between worker threads. Lock the WHOLE wrapper body. */
static alloc_fn8 g_fw_tramp = NULL, g_aw_tramp = NULL;
static void *fw_body(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  if (g_heaplock) alock();
  void *r = g_fw_tramp(a, b, c, d, e, f, g, h);
  if (g_heaplock) aunlock();
  return r;
}
static void *fw_wrap(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  if (g_free_guard && g_heaplock && !g_fg_active) {
    g_fg_active = 1;
    if (sigsetjmp(g_fg_jb, 1) != 0) { g_fg_active = 0; return NULL; }  /* recovered */
    void *r = fw_body(a, b, c, d, e, f, g, h);
    g_fg_active = 0;
    return r;
  }
  return fw_body(a, b, c, d, e, f, g, h);
}
/* AllocWrap(0x9166bc): r3(=d) is the requested size; it rounds up to 16 and the
   bucket is stored in the chunk's boundary tag (the free reads the SAME tag, so
   inflating here stays consistent). Add a 32-byte tail red-zone to every bucket
   block to absorb the engine's small overruns that smash the next chunk's tag.
   Skip near the large-block threshold (0x800) so we don't change the path. */
static int g_bslack = 0;   /* bucket-size inflation: broke the allocator, left off */
static void *aw_wrap(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  size_t sz = (size_t)d;
  if (g_bslack && sz > 0 && sz + g_bslack < 0x800) d = (void *)(sz + g_bslack);
  if (g_heaplock) pthread_mutex_lock(&g_heap_mtx);
  void *r = g_aw_tramp(a, b, c, d, e, f, g, h);
  if (g_heaplock) pthread_mutex_unlock(&g_heap_mtx);
  return r;
}
static void *make_tramp(uintptr_t addr) {
  uint32_t *o = (uint32_t *)addr;
  uint32_t *tr = (uint32_t *)mmap(NULL, 32, PROT_READ | PROT_WRITE | PROT_EXEC,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr == MAP_FAILED) return NULL;
  tr[0] = o[0]; tr[1] = o[1]; tr[2] = 0xe51ff004u; tr[3] = (uint32_t)(addr + 8);
  __builtin___clear_cache((char *)tr, (char *)tr + 16);
  return tr;
}
static void install_pool_alloc_lock(void) {
  /* HEAPLOCK is ON by default (required: serializes GFx GMemoryHeap -> the
     intro/title render without heap corruption). DUCK_NO_HEAPLOCK disables. */
  int want_heaplock = !getenv("DUCK_NO_HEAPLOCK");
  if (!getenv("DUCK_POOLLOCK") && !getenv("DUCK_DFDETECT") && !getenv("DUCK_DFSKIP")
      && !getenv("DUCK_ATRACK") && !want_heaplock && !getenv("DUCK_NOFREE")) return;
  if (getenv("DUCK_NOFREE")) { g_nofree = 1; debugPrintf("[nofree] bucket free disabled (leak)\n"); }
  if (getenv("DUCK_DFDETECT") || getenv("DUCK_DFSKIP")) g_df_on = 1;
  if (getenv("DUCK_DFSKIP")) { g_df_skip = 1; debugPrintf("[df] double-free DROP enabled\n"); }
  if (getenv("DUCK_ATRACK")) { g_atrack = 1; debugPrintf("[atrack] allocation tracker enabled\n"); }
  if (want_heaplock) {
    g_heaplock = 1;
    pthread_mutexattr_t at; pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_heap_mtx, &at); pthread_mutexattr_destroy(&at);
    debugPrintf("[heaplock] GMemoryHeap Alloc/Free serialized\n");
  }
  uintptr_t base = (uintptr_t)text_base;
  /* free target = 0x938bec = the TLSF deallocate ENTRY (r1=chunk). Hooking here
     lets DFSKIP drop a double-free BEFORE the free-list bitmap ops run; hooking
     the later coalesce (0x939e04) was too late (bitmap already corrupted). */
  uintptr_t m = base + 0x9392a4, f = base + 0x938bec;
  const char *fo = getenv("DUCK_FREE_OFF"); if (fo) f = base + strtoul(fo, NULL, 0);
  g_pool_malloc_tramp = (alloc_fn8)make_tramp(m);
  g_pool_free_tramp = (alloc_fn8)make_tramp(f);
  if (g_pool_malloc_tramp) hook_arm(m, (uintptr_t)pool_malloc_wrap);
  if (g_pool_free_tramp) hook_arm(f, (uintptr_t)pool_free_wrap);
  debugPrintf("[poollock] internal allocator malloc/free serialized\n");
  if (g_atrack || g_heaplock) {
    /* high-level GMemoryHeap interface: HeapAlloc=0x82bd78, HeapFree=0x82bb84.
       Tracks BOTH small-bucket and large-block allocations. */
    uintptr_t ha = base + 0x82bd78, hf = base + 0x82bb84;
    g_at_alloc_tramp = (alloc_fn8)make_tramp(ha);
    g_at_free_tramp = (alloc_fn8)make_tramp(hf);
    if (g_at_alloc_tramp) hook_arm(ha, (uintptr_t)at_alloc_wrap);
    if (g_at_free_tramp) hook_arm(hf, (uintptr_t)at_free_wrap);
    debugPrintf("[atrack] HeapAlloc/HeapFree (high-level) tracked\n");
  }
  if (g_heaplock) {
    /* also lock the WRAPPER bodies (boundary-tag writes outside malloc/free) */
    uintptr_t fw = base + 0x915d64, aw = base + 0x9166bc;
    g_fw_tramp = (alloc_fn8)make_tramp(fw);
    g_aw_tramp = (alloc_fn8)make_tramp(aw);
    if (g_fw_tramp) hook_arm(fw, (uintptr_t)fw_wrap);
    if (g_aw_tramp) hook_arm(aw, (uintptr_t)aw_wrap);
    debugPrintf("[heaplock] FreeWrap/AllocWrap bodies serialized\n");
  }
}
/* ---- 16-byte malloc alignment shim (bionic parity) ----
   bionic malloc always returns 16-byte-aligned blocks; glibc on 32-bit ARM only
   guarantees 8 (MALLOC_ALIGNMENT = 2*SIZE_SZ = 8). The engine's internal TLSF
   allocator carves its arena (obtained via malloc) in 16-byte units and assumes
   16-byte base alignment -> on glibc the 8-off arena makes every chunk misaligned
   -> free-list math corrupts (misaligned chunk ptrs, double-free, crashes). Force
   16-byte alignment to match bionic. free() handles memalign'd ptrs in glibc. */
#include <malloc.h>
/* ---- LOW-address arena for large allocations (DUCK_LOWHEAP, default ON) ----
   GFx's GMemoryHeap maps addresses to metadata using the low 31 bits (mask
   0x7fffffff seen in the allocator). bionic places heaps < 0x80000000 so it
   works; glibc malloc serves large (mmap-backed) blocks at HIGH addresses
   (0xd0000000+), so two heaps at 0x60000000 and 0xe0000000 alias -> metadata
   corruption. Serve large allocations from a reserved sub-0x80000000 region. */
static int g_lowheap = 1;
static char *g_low_base = NULL, *g_low_ptr = NULL, *g_low_end = NULL;
static pthread_mutex_t g_low_mtx = PTHREAD_MUTEX_INITIALIZER;
#define LOWHEAP_SZ (640u * 1024 * 1024)
#define LOWHEAP_THRESH 4096           /* allocations >= this go to the low arena */
static void low_init(void) {
  /* reserve a big region below 0x80000000 (hint at 0x30000000) */
  void *p = mmap((void *)0x30000000, LOWHEAP_SZ, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED || (uintptr_t)p >= 0x80000000) {
    if (p != MAP_FAILED) munmap(p, LOWHEAP_SZ);
    g_lowheap = 0; debugPrintf("[lowheap] reserve failed -> disabled\n"); return;
  }
  g_low_base = (char *)p; g_low_ptr = (char *)p; g_low_end = (char *)p + LOWHEAP_SZ;
  debugPrintf("[lowheap] reserved %p..%p\n", g_low_base, g_low_end);
}
/* each low allocation stores its size in the 16 bytes preceding the user ptr */
static int in_low(void *p) { return g_low_base && (char *)p >= g_low_base && (char *)p < g_low_end; }
static size_t low_size(void *p) { return *(size_t *)((char *)p - 16); }
static void *low_alloc_aligned(size_t n, size_t align) {
  if (align < 16) align = 16;
  size_t need = ((n + 15) & ~(size_t)15) + align + 16;
  pthread_mutex_lock(&g_low_mtx);
  char *raw = g_low_ptr;
  if (raw + need > g_low_end) { pthread_mutex_unlock(&g_low_mtx); return NULL; }  /* fall back */
  g_low_ptr += need;
  pthread_mutex_unlock(&g_low_mtx);
  char *user = (char *)(((uintptr_t)raw + 16 + (align - 1)) & ~(uintptr_t)(align - 1));
  *(size_t *)(user - 16) = n;     /* user-16 >= raw, holds size */
  return user;
}
static void *low_alloc(size_t n) { return low_alloc_aligned(n, 16); }
/* SLACK = tail red-zone added to EVERY allocation. The engine has small buffer
   overruns (a few bytes past the end) that smash the next chunk's free-list
   metadata under glibc/bionic alike; bionic's heap layout happened to tolerate
   them, glibc's doesn't. Padding each block absorbs the overrun harmlessly.
   DUCK_SLACK overrides the byte count (default 256). */
static size_t g_slack = 256;
static void *m16_malloc(size_t n) {
  size_t t = n + g_slack;
  if (g_lowheap && t >= LOWHEAP_THRESH) { void *r = low_alloc(t); if (r) return r; }
  return memalign(16, t ? t : 16);
}
static void *m16_memalign(size_t align, size_t n) {
  size_t t = n + g_slack;
  if (g_lowheap && t >= LOWHEAP_THRESH) { void *r = low_alloc_aligned(t, align); if (r) return r; }
  return memalign(align < 16 ? 16 : align, t ? t : 16);
}
static int m16_posix_memalign(void **out, size_t align, size_t n) {
  void *p = m16_memalign(align, n); if (!p) return 12; *out = p; return 0;
}
/* QUARANTINE: delay reuse of freed blocks (bionic's scudo/jemalloc do this; glibc
   tcache reuses immediately). The engine has a use-after-free: it writes to a block
   AFTER freeing it, smashing glibc's free-list next-ptr -> the next malloc that
   reuses it returns a corrupted pointer -> crash. Delaying reuse makes the stale
   write land on a still-quarantined (not-yet-reused) block = harmless. */
#define QN (1u << 14)               /* 16384 freed blocks held back */
static void *g_qring[QN];
static unsigned g_qhead = 0;
static pthread_mutex_t g_q_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_quarantine = 0;        /* DUCK_NO_QUARANTINE disables */
static void m16_free(void *p) {
  if (in_low(p)) return;          /* low bump arena never frees (leak) */
  if (!p) return;
  if (!g_quarantine) { free(p); return; }
  pthread_mutex_lock(&g_q_mtx);
  unsigned i = g_qhead; g_qhead = (g_qhead + 1) & (QN - 1);
  void *evict = g_qring[i];       /* the block freed QN allocations ago */
  g_qring[i] = p;
  pthread_mutex_unlock(&g_q_mtx);
  if (evict) free(evict);         /* now safe to actually release */
}
static void *m16_calloc(size_t nm, size_t sz) {
  size_t t = nm * sz;
  if (sz && t / sz != nm) return NULL;            /* overflow */
  void *p = m16_malloc(t ? t : 16);
  if (p) memset(p, 0, t);
  return p;
}
static void *m16_realloc(void *p, size_t n) {
  if (!p) return m16_malloc(n ? n : 16);
  if (n == 0) { m16_free(p); return NULL; }
  void *np = m16_malloc(n);
  if (!np) return NULL;
  size_t old = in_low(p) ? low_size(p) : malloc_usable_size(p);
  memcpy(np, p, old < n ? old : n);
  m16_free(p);
  return np;
}

/* ---- diagnostic memcpy/memmove guard ----
   The recurring crash is a libc memcpy(dest=NULL, src=garbage, n=11). Intercept
   the engine's memcpy/memmove import: when dest or src is unusable, log the
   ENGINE caller (reliable, unlike the stack scan) + the src bytes, then skip.
   DUCK_MEMGUARD=1 enables. Lets us pinpoint the bad caller and maybe limp on. */
static int g_memguard = 0;
static int addr_in_main(uintptr_t a) { return a >= g_main_text && a < g_main_text + g_main_text_sz; }
static void *my_memcpy(void *d, const void *s, size_t n) {
  if (g_memguard && (d == NULL || (uintptr_t)s < 0x10000)) {
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    static int k = 0;
    if (k++ < 30) {
      fprintf(stderr, "[MEMGUARD] memcpy(d=%p s=%p n=%zu) caller=%s+0x%lx\n",
              d, s, n, addr_in_main(ra) ? "duck" : "?",
              (unsigned long)(addr_in_main(ra) ? ra - g_main_text : ra));
    }
    return d;
  }
  return memcpy(d, s, n);
}
static void *my_memmove(void *d, const void *s, size_t n) {
  if (g_memguard && (d == NULL || (uintptr_t)s < 0x10000)) {
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    static int k = 0;
    if (k++ < 30) fprintf(stderr, "[MEMGUARD] memmove(d=%p s=%p n=%zu) caller=duck+0x%lx\n",
                          d, s, n, (unsigned long)(addr_in_main(ra) ? ra - g_main_text : ra));
    return d;
  }
  return memmove(d, s, n);
}
/* ---- suppress GFx fatal asserts (DUCK_NORAISE=1) ----
   GFx's heap integrity check, on detecting the corrupted free-list, calls
   raise(SIGSEGV)/abort to die. Suppressing lets execution continue past the
   assert; combined with allocator-store RECOVER it may limp to a frame. */
static int g_noraise = 0;
static int my_raise(int sig) {
  if (g_noraise && (sig == 11 || sig == 6 || sig == 4 || sig == 7)) {
    static int n = 0; if (n++ < 10) fprintf(stderr, "[NORAISE] suppressed raise(%d)\n", sig);
    return 0;
  }
  return raise(sig);
}
static void my_abort(void) {
  static int n = 0; if (n++ < 10) fprintf(stderr, "[NORAISE] suppressed abort()\n");
  if (!g_noraise) _exit(134);
  /* return to caller — UB but lets the band-aid run continue */
}
/* The GFx assert self-raises SIGSEGV via syscall(__NR_tgkill, pid, tid, sig)
   directly (not the raise import). Intercept tgkill/tkill/kill with a fatal
   signal and drop it; forward everything else unchanged. */
#include <sys/syscall.h>
#ifndef SYS_mmap2
#define SYS_mmap2 192
#endif
static volatile uintptr_t g_low_mmap_hint = 0x60000000;   /* engine mmap2 hint, sub-0x80000000 */
static pthread_mutex_t g_lowmm_mtx = PTHREAD_MUTEX_INITIALIZER;
static long my_syscall(long n, long a0, long a1, long a2, long a3, long a4, long a5) {
  if (g_noraise) {
    int sig = -1;
    if (n == SYS_tgkill) sig = (int)a2;        /* tgkill(tgid, tid, sig) */
    else if (n == SYS_tkill) sig = (int)a1;    /* tkill(tid, sig) */
    else if (n == SYS_kill) sig = (int)a1;     /* kill(pid, sig) */
    if (sig == 11 || sig == 6 || sig == 4 || sig == 7) {
      static int k = 0; if (k++ < 10) fprintf(stderr, "[NORAISE] suppressed syscall(%ld) sig=%d\n", n, sig);
      return 0;
    }
  }
  /* Force engine mmap2(addr=0) allocations to LOW addresses (< 0x80000000) so
     GFx's 31-bit address->metadata mapping doesn't alias. */
  if (g_lowheap && n == SYS_mmap2 && a0 == 0 && a1 > 0) {
    pthread_mutex_lock(&g_lowmm_mtx);
    uintptr_t hint = g_low_mmap_hint;
    g_low_mmap_hint += ((size_t)a1 + 0xffff) & ~(size_t)0xffff;
    if (g_low_mmap_hint >= 0x7f000000) g_low_mmap_hint = 0x60000000;  /* wrap (best effort) */
    pthread_mutex_unlock(&g_lowmm_mtx);
    long r = syscall(n, (long)hint, a1, a2, a3, a4, a5);
    if (r != -1 && (uintptr_t)r < 0x80000000) {
      static int k = 0; if (k++ < 8) fprintf(stderr, "[LOWMM] mmap2 len=%ld -> %p\n", a1, (void *)r);
      return r;
    }
    if (r != -1) { static int k = 0; if (k++ < 8) fprintf(stderr, "[LOWMM] mmap2 still high %p\n", (void *)r); }
    return r;  /* hint ignored; return whatever (best effort) */
  }
  return syscall(n, a0, a1, a2, a3, a4, a5);
}
/* ---- ISOLATE image-decode memory (libjpeg/libpng/zlib) ----
   The menu-load crash is in libjpeg's free tripping over a smashed GFx free-list:
   the JPEG/PNG decode overruns a buffer into the SHARED GFx heap. Route the image
   codecs' allocations to a DEDICATED region so an overrun corrupts only image
   memory (harmless), not the GFx allocator -> no crash + the decode completes so
   the menu background actually renders. DEFAULT ON; DUCK_NO_IMGISO disables. */
static int g_imgiso = 1;
static char *g_img_base = NULL, *g_img_ptr = NULL, *g_img_end = NULL;
static pthread_mutex_t g_img_mtx = PTHREAD_MUTEX_INITIALIZER;
#define IMGHEAP_SZ (256u * 1024 * 1024)
#define IMG_GUARD 256                 /* tail red-zone per block */
static void img_init(void) {
  void *p = mmap((void *)0x58000000, IMGHEAP_SZ, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) { p = mmap(NULL, IMGHEAP_SZ, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0); }
  if (p == MAP_FAILED) { g_imgiso = 0; debugPrintf("[imgiso] reserve failed\n"); return; }
  g_img_base = (char *)p; g_img_ptr = (char *)p; g_img_end = (char *)p + IMGHEAP_SZ;
  debugPrintf("[imgiso] image heap %p..%p\n", g_img_base, g_img_end);
}
static int in_img(void *p) { return g_img_base && (char *)p >= g_img_base && (char *)p < g_img_end; }
static void *img_alloc(size_t n) {
  size_t a = ((n + 15) & ~(size_t)15) + IMG_GUARD;
  pthread_mutex_lock(&g_img_mtx);
  char *r = g_img_ptr;
  if (r + a > g_img_end) { g_img_ptr = g_img_base; r = g_img_ptr; }  /* wrap (best effort) */
  g_img_ptr += a;
  pthread_mutex_unlock(&g_img_mtx);
  return r;
}
/* hooked codec entry points (overwrite whole body -> no trampoline needed) */
static void *my_jpeg_get(void *cinfo, size_t size) { (void)cinfo; return img_alloc(size ? size : 16); }
static void  my_jpeg_free(void *cinfo, void *p, size_t size) { (void)cinfo; (void)p; (void)size; }
static void *my_zcalloc(void *opq, unsigned items, unsigned size) { (void)opq; return img_alloc((size_t)items * size); }
static void  my_zcfree(void *opq, void *p) { (void)opq; (void)p; }
static void *my_png_malloc(void *png, size_t size) { (void)png; return img_alloc(size ? size : 16); }
static void  my_png_free(void *png, void *p) { (void)png; (void)p; }
static void install_image_isolation(void) {
  if (!g_imgiso) return;
  img_init();
  if (!g_imgiso) return;
  uintptr_t base = (uintptr_t)text_base;
  struct { uintptr_t off; void *fn; } H[] = {
    {0x96a8c4, my_jpeg_get},  {0x96a8e4, my_jpeg_get},   /* get_small, get_large */
    {0x96a8d4, my_jpeg_free}, {0x96a8f4, my_jpeg_free},  /* free_small, free_large */
    {0x9b2f10, my_zcalloc},   {0x9b2f18, my_zcfree},     /* zlib */
    {0x94b330, my_png_malloc},{0x94b3d4, my_png_free},   /* png (non-default) */
    {0x94b30c, my_png_malloc},{0x94b3b8, my_png_free},   /* png_malloc_default/free_default */
  };
  for (size_t i = 0; i < sizeof(H)/sizeof(H[0]); i++)
    hook_arm(base + H[i].off, (uintptr_t)H[i].fn);
  debugPrintf("[imgiso] jpeg/png/zlib allocators isolated\n");
}

/* ---- NULL-SAFE free-list unlink (ROOT-CAUSE crash fix) ----
   gdb pinned the menu-load crash to the allocator's circular-free-list unlink at
   0x939d84: the engine USE-AFTER-FREEs a bucket block, zeroing its in-block next/
   prev pointers; a later unlink does next->prev = prev with next == NULL -> SIGSEGV.
   Replace the unlink with a faithful reimplementation that GUARDS stores against
   NULL/out-of-arena links, so a corrupted node drops from the bin without crashing.
   Layout (from the disasm): node[0xc]=size-class; binidx=clamp(sc-1,0..0x1f);
   binslot=mstate+binidx*4; head=binslot[1]; node[0]=next, node[1]=prev; bitmap=mstate[0]. */
static int g_safeunlink = 1;          /* DUCK_NO_SAFEUNLINK disables */
static int g_su_hits = 0;
static inline int su_ok(void *p) { return p != NULL && (uintptr_t)p > 0x10000 && (uintptr_t)p < 0xf0000000; }
static void my_unlink(void *r0, void *r1) {
  void **mstate = (void **)r0;
  void **node = (void **)r1;
  /* match the real clamp exactly: ldrb sc; sc-1; if (unsigned)>=31 -> 31 (so 0->31) */
  unsigned scb = ((unsigned char *)node)[0xc];
  int binidx = (int)(scb - 1u); if ((unsigned)binidx > 0x1f) binidx = 0x1f;
  void **binslot = (void **)((char *)r0 + binidx * 4);
  void *head = binslot[1];
  void *next = node[0];
  void *prev = node[1];
  /* ---- corruption containment (the ROOT crash fix) ----
     A UAF zeroes a free block's next link. The faithful unlink would then write
     that garbage (0) THROUGH into a valid neighbor (prev->next = 0), spreading
     the corruption node-by-node until the engine's own free-list integrity
     check fires raise(SIGSEGV). Instead: when EITHER link is bad, DO NOT touch
     any neighbor with a bad value. Collapse the whole bin to a single valid
     survivor (made self-circular) — or empty it — so every bin stays internally
     consistent (the engine's walk sees valid circular lists -> no abort). Any
     other elements of a >2 bin leak (bounded, menu-load only). We also reset the
     departing node's own links to a benign self-loop so the real coalesce code
     can't deref a zero next inline after we return. */
  if (!su_ok(next) || !su_ok(prev)) {
    if (g_hist) { static int hn = 0; if (hn++ < 12) {
        fprintf(stderr, "[UAF] corrupt node=%p next=%p prev=%p sc=%u tid=%ld\n", (void *)node, next, prev, scb, (long)syscall(SYS_gettid));
        hist_dump("victim", (void *)node);
        if (g_atrack) at_dump_near("victim-owner", (uintptr_t)node);
        df_backtrace(); } }
    else if (g_su_hits < 30) { g_su_hits++;
      fprintf(stderr, "[SAFEUNLINK] contained corrupt node=%p next=%p prev=%p\n", (void *)node, next, prev); }
    void *surv = NULL;
    if (su_ok(prev) && prev != (void *)node) surv = prev;
    else if (su_ok(next) && next != (void *)node) surv = next;
    if (surv) {
      ((void **)surv)[0] = surv; ((void **)surv)[1] = surv;   /* self-circular */
      binslot[1] = surv;
      mstate[0] = (void *)((uintptr_t)mstate[0] | ((uintptr_t)1 << binidx));
    } else {
      binslot[1] = NULL;
      mstate[0] = (void *)((uintptr_t)mstate[0] & ~((uintptr_t)1 << binidx));
    }
    node[0] = node; node[1] = node;             /* benign self-loop on the corpse */
    return;
  }
  if (r1 != head) {
    ((void **)next)[1] = prev;
    ((void **)prev)[0] = next;
  } else if (r1 == prev) {                       /* single element */
    binslot[1] = NULL;
    mstate[0] = (void *)((uintptr_t)mstate[0] & ~((uintptr_t)1 << binidx));
  } else {
    binslot[1] = prev;
    ((void **)next)[1] = prev;
    ((void **)prev)[0] = next;
  }
}
/* INSERT (0x939d20): add node to its size-class bin (circular list). Crashes at
   `tail->next = node` when the head's prev (tail) link was zeroed by the UAF.
   Reimplemented NULL-safe. args: r0=mstate, r1=node, r3=size-class. */
static void my_insert(void *r0, void *r1, void *r2u, int sc) {
  (void)r2u;
  void **mstate = (void **)r0;
  void **node = (void **)r1;
  /* match the real clamp: sc-1; if (unsigned)>30 -> 31 (so sc==0 -> 31) */
  int binidx = (int)((unsigned)sc - 1u); if ((unsigned)binidx > 0x1e) binidx = 0x1f;
  uintptr_t bit = (uintptr_t)1 << binidx;
  void **binslot = (void **)((char *)r0 + binidx * 4);
  void *head = binslot[1];
  if (head == NULL || !su_ok(head)) {
    node[0] = node; node[1] = node;            /* empty/corrupt bin: self-circular */
  } else {
    void *tail = ((void **)head)[1];           /* head->prev */
    if (!su_ok(tail)) tail = head;             /* corrupt tail: treat head as 1-elem */
    node[0] = head; node[1] = tail;
    ((void **)head)[1] = node;
    if (su_ok(tail)) ((void **)tail)[0] = node;
  }
  mstate[0] = (void *)((uintptr_t)mstate[0] | bit);
  binslot[1] = node;
}
static void install_safe_unlink(void) {
  if (!g_safeunlink) return;
  hook_arm((uintptr_t)text_base + 0x939d84, (uintptr_t)my_unlink);
  hook_arm((uintptr_t)text_base + 0x939d20, (uintptr_t)my_insert);
  debugPrintf("[safeunlink] free-list unlink+insert -> NULL-safe\n");
}
/* ---- SERIALIZE zlib inflate (image-decode race fix) ----
   zlib inflate() lazily builds STATIC fixed-Huffman tables on first use (NOT
   thread-safe). The menu decodes several PNGs concurrently on workers; their
   first inflate calls race on those statics -> garbage output (black textures) +
   wrong sizes overrunning the GFx heap (the crash). Serialize inflate. DEFAULT ON. */
static int g_inflatelock = 1;
static pthread_mutex_t g_inflate_mtx;
typedef int (*inflate_fn)(void *, int);
static inflate_fn g_inflate_tramp = NULL;
static int my_inflate(void *strm, int flush) {
  pthread_mutex_lock(&g_inflate_mtx);
  int r = g_inflate_tramp(strm, flush);
  pthread_mutex_unlock(&g_inflate_mtx);
  return r;
}
/* ---- SERIALIZE the texture loader (THE image-race root fix) ----
   wfPNGTexture::Init (0x59f160) starts with a CACHE lookup; two workers loading
   the SAME texture concurrently both miss, both decode, and one is freed as a
   duplicate WHILE the other is still decoding into it -> use-after-free that
   zeroes the GFx free-list links (the gdb-found crash) AND yields a corrupt/black
   texture. Serialize the whole texture load with one lock -> no duplicate-decode
   race -> no crash + the background actually decodes. DEFAULT ON. */
static int g_texlock = 1;
static pthread_mutex_t g_tex_mtx;
static alloc_fn8 g_pnginit_tramp = NULL, g_decompress_tramp = NULL;
static int g_texlog = 0;   /* DUCK_TEXLOG=1: log every wfPNGTexture::Init(name,w,h,fmt) */
static void *pnginit_wrap(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  pthread_mutex_lock(&g_tex_mtx);
  void *r = g_pnginit_tramp(a, b, c, d, e, f, g, h);
  if (g_texlog) {
    const char *nm = (const char *)b;
    int ok = 1; for (const char *p = nm; nm && p < nm + 128; p++) { if ((uintptr_t)p < 0x1000) { ok = 0; break; } if (!*p) break; }
    fprintf(stderr, "[TEX] Init name=%s args=(%d,%d,%d) ret=%p\n",
            (nm && ok) ? nm : "?", (int)(intptr_t)c, (int)(intptr_t)d, (int)(intptr_t)e, r);
  }
  pthread_mutex_unlock(&g_tex_mtx);
  return r;
}
static void *decompress_wrap(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  pthread_mutex_lock(&g_tex_mtx);
  void *r = g_decompress_tramp(a, b, c, d, e, f, g, h);
  pthread_mutex_unlock(&g_tex_mtx);
  return r;
}
static void install_tex_serial(void) {
  if (!g_texlock) return;
  pthread_mutexattr_t at; pthread_mutexattr_init(&at);
  pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&g_tex_mtx, &at); pthread_mutexattr_destroy(&at);
  uintptr_t pi = (uintptr_t)text_base + 0x59f160;
  g_pnginit_tramp = (alloc_fn8)make_tramp(pi);
  if (g_pnginit_tramp) { hook_arm(pi, (uintptr_t)pnginit_wrap);
    debugPrintf("[texlock] wfPNGTexture::Init serialized\n"); }
  /* also serialize the whole texture DECODE (wfTexture::DecompressUpdate 0x50fe8c)
     so concurrent menu image decodes can't race/corrupt. Same recursive lock. */
  uintptr_t du = (uintptr_t)text_base + 0x50fe8c;
  g_decompress_tramp = (alloc_fn8)make_tramp(du);
  if (g_decompress_tramp) { hook_arm(du, (uintptr_t)decompress_wrap);
    debugPrintf("[texlock] wfTexture::DecompressUpdate serialized\n"); }
}
static void install_inflate_serial(void) {
  if (!g_inflatelock) return;
  pthread_mutexattr_t a; pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&g_inflate_mtx, &a); pthread_mutexattr_destroy(&a);
  uintptr_t inf = (uintptr_t)text_base + 0x9addf0;
  g_inflate_tramp = (inflate_fn)make_tramp(inf);
  if (g_inflate_tramp) { hook_arm(inf, (uintptr_t)my_inflate);
    debugPrintf("[inflatelock] zlib inflate serialized\n"); }
}

static void install_ducktales_hooks(void) {
  so_make_text_writable();
  uintptr_t a;
  a = so_find_addr_safe("_Z16NvGetGamepadAxesP7_JNIEnvP8_jobjectRi");
  if (a) { hook_arm(a, (uintptr_t)nv_gamepad_enum_stub); debugPrintf("[hook] NvGetGamepadAxes->stub\n"); }
  a = so_find_addr_safe("_Z19NvGetGamepadButtonsP7_JNIEnvP8_jobjectRi");
  if (a) { hook_arm(a, (uintptr_t)nv_get_buttons); debugPrintf("[hook] NvGetGamepadButtons->real caps\n"); }

  /* Force wfSystem::GetCpuCount() (spawn count + barrier in one shot). */
  const char *cc = getenv("DUCK_CPUCOUNT");
  if (cc) {
    g_forced_cpucount = atoi(cc);
    uintptr_t g = so_find_addr_safe("_ZN8wfSystem11GetCpuCountEv");
    if (g) { hook_arm(g, (uintptr_t)forced_cpucount); debugPrintf("[hook] GetCpuCount -> %d\n", g_forced_cpucount); }
  }

  /* Optional: clamp wfLogicalCore job-thread count (default 10) for debugging
     concurrency. The ctor loop ends at `cmp r6, #10` (e356000a) at
     wfLogicalCore::wfLogicalCore+0x9c. Patch the immediate to N. */
  const char *jt = getenv("DUCK_JOBTHREADS");
  if (jt) {
    int n = atoi(jt); if (n < 1) n = 1; if (n > 10) n = 10;
    uintptr_t c = so_find_addr_safe("_ZN13wfLogicalCoreC1Ei");
    if (!c) c = so_find_addr_safe("_ZN13wfLogicalCoreC2Ei");
    if (c) {
      uint32_t *ins = (uint32_t *)(c + 0x9c);
      if ((*ins & 0xfffff000) == 0xe3560000) {  /* cmp r6, #imm */
        *ins = 0xe3560000 | (n & 0xff);
        debugPrintf("[hook] wfLogicalCore threads -> %d\n", n);
      } else debugPrintf("[hook] jobthread cmp not found (%08x)\n", *ins);
    }
  }
  install_jobexec_serializer();
  install_pool_alloc_lock();
  if (getenv("DUCK_NO_IMGISO")) g_imgiso = 0;
  install_image_isolation();
  if (getenv("DUCK_NO_SAFEUNLINK")) g_safeunlink = 0;
  install_safe_unlink();
  if (getenv("DUCK_NO_INFLATELOCK")) g_inflatelock = 0;
  install_inflate_serial();
  if (getenv("DUCK_NO_TEXLOCK")) g_texlock = 0;
  install_tex_serial();
  so_make_text_executable();
  so_flush_caches();
}

/* ---- loaded fmod modules (for cross-lib symbol resolution) ---- */
static so_module *g_m_fmodex = NULL;
static so_module *g_m_fmodevent = NULL;
static so_module *g_m_main = NULL;
/* g_main_text / g_main_text_sz declared near the top (used by df_backtrace) */
static uintptr_t g_fx_text = 0, g_fx_sz = 0, g_fe_text = 0, g_fe_sz = 0;

/* Install bionic<->glibc pthread shims (mutex/cond/sem/attr/create) by
   overriding the import table BEFORE resolution. Bionic pthread_mutex_t is 4
   bytes vs glibc 24 -> without this the game's small mutex buffers overflow
   into the heap (malloc: invalid size). */
extern void recon_wire_pthread(void (*)(const char *, void *));
static void dt_set_import(const char *name, void *fn) {
  for (size_t i = 0; i < dynlib_numfunctions; i++)
    if (!strcmp(dynlib_functions[i].symbol, name)) {
      dynlib_functions[i].func = (uintptr_t)fn;
      return;
    }
}

void *dt_fmod_lookup(const char *nm) {
  so_module *mods[2] = {g_m_fmodex, g_m_fmodevent};
  for (int i = 0; i < 2; i++) {
    if (!mods[i]) continue;
    so_module *cur = so_save();
    so_use(mods[i]);
    uintptr_t a = so_find_addr_safe(nm);
    so_use(cur);
    free(cur);
    if (a) return (void *)a;
  }
  return NULL;
}

/* ---- allocator self-heal ----
   The engine's TLSF allocator faults when it walks a free-list whose boundary
   tags were corrupted (deterministic overflow upstream): it computes a garbage
   chunk pointer and stores a tag through it (str/strh/strb to an out-of-arena
   addr). DUCK_RECOVER=1: when a SIGSEGV's pc is inside the allocator code and
   the faulting insn is a store, skip it (pc+=4) and resume — the bookkeeping
   write is lost (block leaks) but the crash is avoided. Bounded to avoid loops. */
static int g_recover = 0, g_recover_n = 0;
/* allocator code range (text-relative): free 0x938bec .. malloc end ~0x939f40 */
static int pc_in_allocator(uintptr_t pc) {
  uintptr_t o = pc - g_main_text;
  return o >= 0x938b00 && o < 0x93a000;
}
static int insn_is_store(uintptr_t pc) {
  uint32_t w = *(uint32_t *)pc;
  /* ARM str/strh/strb/strd (cond any): bits[27:26]=01 & L=0 (store) for str/strb;
     strh/strd are 000 with specific encodings. Accept the common single-reg
     stores: 0bxxxx 01x x x0x0 ... (L bit = bit20 == 0). */
  if ((w & 0x0c100000) == 0x04000000) return 1;     /* LDR/STR imm/reg, L(bit20)=0 -> STR */
  if ((w & 0x0e1000b0) == 0x000000b0) return 1;     /* STRH (reg), L=0 */
  if ((w & 0x0e1000f0) == 0x000000b0) return 1;     /* STRH (imm) */
  return 0;
}

/* ALLOC-RECOVER: the engine's UAF/overflow leaves garbage in the allocator's
   free-list links; an alloc/free op then loads/stores through a wild pointer.
   When a fault's pc is in the allocator's free-list code, skip the faulting insn
   in place (load -> zero the dest reg; store -> drop) and resume. Combined with
   the NULL-safe unlink/insert this makes the allocator survive corrupted links
   without crashing or leaking locks (in-place resume, no siglongjmp). DEFAULT ON. */
static int g_allocrec = 1, g_ar_n = 0;
static void set_reg(ucontext_t *uc, int rd, unsigned long v) {
  switch (rd) {
    case 0: uc->uc_mcontext.arm_r0=v; break; case 1: uc->uc_mcontext.arm_r1=v; break;
    case 2: uc->uc_mcontext.arm_r2=v; break; case 3: uc->uc_mcontext.arm_r3=v; break;
    case 4: uc->uc_mcontext.arm_r4=v; break; case 5: uc->uc_mcontext.arm_r5=v; break;
    case 6: uc->uc_mcontext.arm_r6=v; break; case 7: uc->uc_mcontext.arm_r7=v; break;
    case 8: uc->uc_mcontext.arm_r8=v; break; case 9: uc->uc_mcontext.arm_r9=v; break;
    case 10: uc->uc_mcontext.arm_r10=v; break; case 11: uc->uc_mcontext.arm_fp=v; break;
    case 12: uc->uc_mcontext.arm_ip=v; break; default: break;
  }
}
static unsigned long get_reg(ucontext_t *uc, int r) {
  switch (r) {
    case 0: return uc->uc_mcontext.arm_r0; case 1: return uc->uc_mcontext.arm_r1;
    case 2: return uc->uc_mcontext.arm_r2; case 3: return uc->uc_mcontext.arm_r3;
    case 4: return uc->uc_mcontext.arm_r4; case 5: return uc->uc_mcontext.arm_r5;
    case 6: return uc->uc_mcontext.arm_r6; case 7: return uc->uc_mcontext.arm_r7;
    case 8: return uc->uc_mcontext.arm_r8; case 9: return uc->uc_mcontext.arm_r9;
    case 10: return uc->uc_mcontext.arm_r10; case 11: return uc->uc_mcontext.arm_fp;
    case 12: return uc->uc_mcontext.arm_ip; case 13: return uc->uc_mcontext.arm_sp;
    case 14: return uc->uc_mcontext.arm_lr; case 15: return uc->uc_mcontext.arm_pc + 8;
    default: return 0;
  }
}
/* ---- mprotect software watchpoint (DUCK_WP=0xADDR DUCK_WPFRAME=N) ----
   HW watchpoints aren't exposed by this 3.14 kernel and gdb software watchpoints
   single-step everything (too slow to reach menu-load). Instead protect the PAGE
   of the deterministic UAF victim (ASLR off -> stable addr) read-only just before
   the menu-load, and in SIGSEGV emulate every store to the page (so the program
   keeps running with the page protected) while LOGGING the exact pc that writes
   the watched word -> the UAF writer. */
static uintptr_t g_wp_addr = 0;       /* watched word (e.g. 0x3186fe40) */
static uintptr_t g_wp_page = 0;       /* its page base */
static volatile int g_wp_armed = 0;
static volatile int g_wp_need_rearm = 0;
static int g_wp_frame = 0;
static int g_wp_hits = 0;
static int g_procmem = -1;
static void wp_protect(int ro) {
  if (!g_wp_page) return;
  mprotect((void *)g_wp_page, 4096, ro ? PROT_READ : (PROT_READ | PROT_WRITE));
}
static void *wp_arm_thread(void *a) {
  (void)a;
  while (egl_shim_frame_count() < g_wp_frame) usleep(2000);
  g_procmem = open("/proc/self/mem", O_RDWR);
  if (g_procmem < 0) fprintf(stderr, "[WP] WARN: /proc/self/mem open failed (%s)\n", strerror(errno));
  g_wp_page = g_wp_addr & ~0xfffUL;
  /* commit the page first (touch it) so /proc/self/mem writes have a backing page */
  (void)*(volatile unsigned *)g_wp_addr;
  wp_protect(1);
  g_wp_armed = 1;
  fprintf(stderr, "[WP] armed: page=0x%lx watching=0x%lx at frame %d\n",
          (unsigned long)g_wp_page, (unsigned long)g_wp_addr, egl_shim_frame_count());
  unsigned last = *(volatile unsigned *)g_wp_addr;
  while (g_wp_armed) {
    if (g_wp_need_rearm) { g_wp_need_rearm = 0; wp_protect(1); }
    /* also poll the value as a backstop: if it ever becomes 0 we at least know */
    unsigned v = *(volatile unsigned *)g_wp_addr;
    if (v != last && v == 0) fprintf(stderr, "[WP-POLL] watched word became 0\n");
    last = v;
    usleep(30);
  }
  return NULL;
}
/* emulate a single ARM store to *EA (=si_addr) using ucontext regs; keep page RO.
   returns 1 if handled (pc advanced), 0 if can't decode. */
/* write `n` bytes to the watched (RO) page by briefly dropping protection.
   A spinlock serialises concurrent fault handlers; the tiny window (2 mprotect
   syscalls) can rarely let another thread's store slip unseen, so a run may need
   a retry (the victim addr is deterministic with ASLR off). */
static volatile int g_wp_lock = 0;
static void wp_write(uintptr_t ea, const void *src, int n) {
  while (__sync_lock_test_and_set(&g_wp_lock, 1)) { }
  mprotect((void *)g_wp_page, 4096, PROT_READ | PROT_WRITE);
  for (int i = 0; i < n; i++) ((volatile unsigned char *)ea)[i] = ((const unsigned char *)src)[i];
  mprotect((void *)g_wp_page, 4096, PROT_READ);
  __sync_lock_release(&g_wp_lock);
}
/* locate the VFP register file in the signal ucontext (magic VFP_MAGIC) */
static unsigned long long *wp_vfp_regs(ucontext_t *uc) {
  unsigned long *p = (unsigned long *)&uc->uc_mcontext;
  for (int i = 0; i < 320; i++) {
    if (p[i] == 0x56465001u) return (unsigned long long *)&p[i + 2]; /* skip magic,size */
  }
  return NULL;
}
/* full ARM store emulator: keeps the watched page protected, replays the store
   through /proc/self/mem. Returns 1 if handled. Covers the store forms seen on
   this heap page (STR/STRB/STRH/STRD, STM, STREX*, VSTR/VSTM). */
static int wp_emulate_store(ucontext_t *uc, uintptr_t fa) {
  uintptr_t pc = uc->uc_mcontext.arm_pc;
  uint32_t insn = *(uint32_t *)pc;
  int rt = (insn >> 12) & 0xf;
  /* STR/STRB (single data transfer), L=0 */
  if ((insn & 0x0c100000) == 0x04000000) {
    unsigned long v = get_reg(uc, rt);
    int B = (insn >> 22) & 1;
    wp_write(fa, &v, B ? 1 : 4);
    uc->uc_mcontext.arm_pc = pc + 4; return 1;
  }
  /* extra load/store: STRH (op=01), STRD (op=11), L=0, bit7=bit4=1 */
  if ((insn & 0x0e000090) == 0x00000090 && ((insn >> 4) & 0xb) == 0xb /*bit7,bit4*/) {
    int op = (insn >> 5) & 3, L = (insn >> 20) & 1;
    if (!L && op == 1) { unsigned long v = get_reg(uc, rt); wp_write(fa, &v, 2);
      uc->uc_mcontext.arm_pc = pc + 4; return 1; }            /* STRH */
    if (!L && op == 3) { unsigned long a = get_reg(uc, rt), b = get_reg(uc, rt + 1);
      wp_write(fa, &a, 4); wp_write(fa + 4, &b, 4);
      uc->uc_mcontext.arm_pc = pc + 4; return 1; }            /* STRD */
  }
  /* STM (block store), L=0: store reg list starting near base */
  if ((insn & 0x0e100000) == 0x08000000) {
    int rn = (insn >> 16) & 0xf, P = (insn >> 24) & 1, U = (insn >> 23) & 1;
    unsigned list = insn & 0xffff;
    int cnt = __builtin_popcount(list);
    uintptr_t base = get_reg(uc, rn);
    uintptr_t addr = U ? (base + (P ? 4 : 0)) : (base - 4 * cnt + (P ? 0 : 4));
    for (int r = 0; r < 16; r++) if (list & (1u << r)) {
      unsigned long v = get_reg(uc, r); wp_write(addr, &v, 4); addr += 4;
    }
    uc->uc_mcontext.arm_pc = pc + 4; return 1;
  }
  /* STREX/STREXB/STREXH/STREXD: store R[rt(=bits3:0)] to [rn], set R[rd]=0 (ok) */
  if ((insn & 0x0f900ff0) == 0x01800f90) {
    int rd = (insn >> 12) & 0xf, rtt = insn & 0xf;
    int sub = (insn >> 21) & 3;   /* 0=word,1=D,2=byte,3=half (per bits[22:21]) */
    unsigned long v = get_reg(uc, rtt);
    if (sub == 2) wp_write(fa, &v, 1);
    else if (sub == 3) wp_write(fa, &v, 2);
    else if (sub == 1) { unsigned long w = get_reg(uc, rtt + 1); wp_write(fa, &v, 4); wp_write(fa + 4, &w, 4); }
    else wp_write(fa, &v, 4);
    set_reg(uc, rd, 0);   /* exclusive store "succeeded" */
    uc->uc_mcontext.arm_pc = pc + 4; return 1;
  }
  /* VSTR (single FP reg), coproc 1010(S)/1011(D), L=0 */
  if ((insn & 0x0f300e00) == 0x0d000a00 && !((insn >> 20) & 1)) {
    unsigned long long *vfp = wp_vfp_regs(uc);
    if (vfp) {
      int dbl = (insn >> 8) & 1;     /* 1011=double */
      int Vd = (insn >> 12) & 0xf, D = (insn >> 22) & 1;
      if (dbl) { int dn = (D << 4) | Vd; wp_write(fa, &vfp[dn], 8); }
      else { int sn = (Vd << 1) | D; unsigned w = (sn & 1) ? (unsigned)(vfp[sn>>1] >> 32) : (unsigned)vfp[sn>>1];
             wp_write(fa, &w, 4); }
      uc->uc_mcontext.arm_pc = pc + 4; return 1;
    }
  }
  /* VSTM (FP reg list), coproc 1010/1011, L=0, bit23(U) addressing */
  if ((insn & 0x0e100e00) == 0x0c000a00 && !((insn >> 20) & 1)) {
    unsigned long long *vfp = wp_vfp_regs(uc);
    if (vfp) {
      int dbl = (insn >> 8) & 1, Vd = (insn >> 12) & 0xf, D = (insn >> 22) & 1;
      int imm8 = insn & 0xff;
      uintptr_t addr = fa;
      if (dbl) { int dn = (D << 4) | Vd, n = imm8 / 2;
        for (int i = 0; i < n; i++) { wp_write(addr, &vfp[dn + i], 8); addr += 8; } }
      else { int sn = (Vd << 1) | D;
        for (int i = 0; i < imm8; i++) { int s = sn + i; unsigned w = (s&1)?(unsigned)(vfp[s>>1]>>32):(unsigned)vfp[s>>1];
          wp_write(addr, &w, 4); addr += 4; } }
      uc->uc_mcontext.arm_pc = pc + 4; return 1;
    }
  }
  return 0;   /* unhandled */
}
/* ---- ARM32 crash handler (debug) ---- */
static void crash_handler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  uintptr_t pc = uc->uc_mcontext.arm_pc;
  uintptr_t lr = uc->uc_mcontext.arm_lr;
  /* ---- mprotect watchpoint: fault inside the protected page ---- */
  if (g_wp_armed && (sig == SIGSEGV || sig == SIGBUS)) {
    uintptr_t fa = (uintptr_t)info->si_addr;
    if (fa >= g_wp_page && fa < g_wp_page + 4096) {
      uintptr_t off = pc - g_main_text;
      int is_target = (fa == g_wp_addr);
      int thumb = (uc->uc_mcontext.arm_cpsr >> 5) & 1;
      /* emulate the store (writes via /proc/self/mem; page stays RO) */
      if (!thumb && wp_emulate_store(uc, fa)) {
        if (is_target) {
          unsigned after = *(volatile unsigned *)g_wp_addr;
          /* the UAF is the write that ZEROES the next link; allocator relinks
             write valid pointers. Flag the zeroing write loudly. */
          if (after == 0) {
            fprintf(stderr, "\n[WP-WRITER] *** 0x%lx ZEROED by pc=duck+0x%lx lr=duck+0x%lx ***\n",
                    (unsigned long)g_wp_addr, (unsigned long)off, (unsigned long)(lr - g_main_text));
            uintptr_t sp = uc->uc_mcontext.arm_sp; uintptr_t *s = (uintptr_t *)(sp & ~3u);
            int shown = 0;
            for (int i = 0; i < 4096 && shown < 28; i++) { uintptr_t v = s[i];
              if (v >= g_main_text && v < g_main_text + g_main_text_sz) {
                fprintf(stderr, "    duck+0x%lx\n", (unsigned long)(v - g_main_text)); shown++; } }
            g_wp_hits++;
          } else if (g_wp_hits < 1) {
            static int wn=0; if (wn++ < 20)
              fprintf(stderr, "[WP] write to target pc=duck+0x%lx val=0x%x (not zero)\n",
                      (unsigned long)off, after);
          }
        }
        return;
      }
      /* unhandled store form -> dump it, then brief unprotect + poll-rearm */
      static int undn = 0;
      if (undn < 16) { undn++;
        uint32_t w = *(uint32_t *)pc;
        fprintf(stderr, "[WP] UNHANDLED store pc=duck+0x%lx thumb=%d insn=0x%08x fault=0x%lx%s\n",
                (unsigned long)off, thumb, w, (unsigned long)fa, is_target ? "  <<< TARGET" : ""); }
      g_wp_need_rearm = 1; wp_protect(0);
      return;
    }
  }
  /* alloc free-list code range (text-relative): 0x938800 .. 0x93a000 */
  if (g_allocrec && (sig == SIGSEGV || sig == SIGBUS)) {
    uintptr_t off = pc - g_main_text;
    if (off >= 0x938800 && off < 0x93a000) {
      uint32_t insn = *(uint32_t *)pc;
      int is_ldr = (insn & 0x0c100000) == 0x04100000;       /* LDR/LDRB imm/reg (L=1) */
      int is_ldrh = (insn & 0x0e1000f0) == 0x001000b0;      /* LDRH imm */
      int rd = (insn >> 12) & 0xf;
      if (is_ldr || is_ldrh) set_reg(uc, rd, 0);             /* load -> dest = 0 */
      if (g_ar_n < 25) { g_ar_n++; fprintf(stderr, "[ALLOCREC] skip @duck+0x%lx fault=%p (%s)\n",
            (unsigned long)off, info->si_addr, (is_ldr||is_ldrh)?"load":"store"); }
      uc->uc_mcontext.arm_pc = pc + 4;
      return;
    }
  }
  /* FREE-GUARD: a SIGSEGV occurred while this thread was inside a guarded free
     (corrupted free-list: GFx raised, or a wild store faulted). Release the
     recursive heap lock we still hold and siglongjmp back to the free wrapper,
     which abandons the free (block leaks) and returns -> game survives. */
  if (g_free_guard && g_fg_active && (sig == SIGSEGV || sig == SIGBUS)) {
    static int n = 0; if (n++ < 30) fprintf(stderr, "[FREEGUARD] aborted corrupted free (sig=%d si=%d)\n", sig, info->si_code);
    int d = g_alock_depth; g_alock_depth = 0;
    while (d-- > 0) pthread_mutex_unlock(&g_heap_mtx);
    g_fg_active = 0;
    siglongjmp(g_fg_jb, 1);
  }
  /* NORAISE: a SIGSEGV with si_code <= 0 was sent by tgkill/raise/pthread_kill
     (user-generated), i.e. the GFx integrity assert deliberately killing itself.
     Resume the interrupted context (return) so execution continues past it. */
  if (g_noraise && sig == SIGSEGV && info->si_code <= 0) {
    static int n = 0; if (n++ < 12) fprintf(stderr, "[NORAISE] resumed deliberate raise (si_code=%d)\n", info->si_code);
    return;
  }
  /* RECOVER: any store fault to a wild low pointer (< 0x100000) from libducktales
     code = the corrupted GFx free-list writing through garbage next/prev. Skip the
     store and resume (the bookkeeping write is lost; block leaks). Covers small AND
     large allocators (wider than the fixed pc_in_allocator range). */
  int pc_in_duck = pc >= g_main_text && pc < g_main_text + g_main_text_sz;
  int wild = (uintptr_t)info->si_addr < 0x100000;
  if (g_recover && sig == SIGSEGV && pc_in_duck && wild && insn_is_store(pc)) {
    if (g_recover_n < 20) {
      fprintf(stderr, "[RECOVER] store fault @duck+0x%lx fault=%p (#%d)\n",
              (unsigned long)(pc - g_main_text), info->si_addr, g_recover_n);
      /* FIRST allocator fault = closest to the overflow. Dump the alloc owning
         each arena-pointer register -> the buffer that overran the free-list. */
      if (g_atrack && g_recover_n < 4) {
        uintptr_t cand[5] = { uc->uc_mcontext.arm_r4, uc->uc_mcontext.arm_r5,
                              uc->uc_mcontext.arm_r6, uc->uc_mcontext.arm_r7,
                              uc->uc_mcontext.arm_r8 };
        const char *nm[5] = {"r4","r5","r6","r7","r8"};
        for (int i = 0; i < 5; i++)
          if (cand[i] > 0x10000000 && cand[i] < 0xf0000000) at_dump_near(nm[i], cand[i]);
      }
    }
    g_recover_n++;
    if (g_recover_n < 100000) { uc->uc_mcontext.arm_pc = pc + 4; return; }
  }
  uintptr_t sp = uc->uc_mcontext.arm_sp;
  fprintf(stderr, "\n=== CRASH sig=%d fault=%p ===\n", sig, info->si_addr);
  fprintf(stderr, "pc=%p lr=%p sp=%p\n", (void *)pc, (void *)lr, (void *)sp);
#define INMOD(a,b,s,nm) do{ if((a)>=(b)&&(a)<(b)+(s)) fprintf(stderr,"   %s+0x%lx\n",nm,(unsigned long)((a)-(b))); }while(0)
  INMOD(pc, g_main_text, g_main_text_sz, "duck.pc"); INMOD(lr, g_main_text, g_main_text_sz, "duck.lr");
  INMOD(pc, g_fx_text, g_fx_sz, "fmodex.pc"); INMOD(lr, g_fx_text, g_fx_sz, "fmodex.lr");
  fprintf(stderr, "r0=%lx r1=%lx r2=%lx r3=%lx r4=%lx r5=%lx\n",
          (unsigned long)uc->uc_mcontext.arm_r0, (unsigned long)uc->uc_mcontext.arm_r1,
          (unsigned long)uc->uc_mcontext.arm_r2, (unsigned long)uc->uc_mcontext.arm_r3,
          (unsigned long)uc->uc_mcontext.arm_r4, (unsigned long)uc->uc_mcontext.arm_r5);
  fprintf(stderr, "r6=%lx r7=%lx r8=%lx r10=%lx fp=%lx ip=%lx\n",
          (unsigned long)uc->uc_mcontext.arm_r6, (unsigned long)uc->uc_mcontext.arm_r7,
          (unsigned long)uc->uc_mcontext.arm_r8, (unsigned long)uc->uc_mcontext.arm_r10,
          (unsigned long)uc->uc_mcontext.arm_fp, (unsigned long)uc->uc_mcontext.arm_ip);
  /* ASAN-lite: identify the buffer that overflowed into the corrupted free-list.
     The corrupted chunk address is in one of the arena-pointer registers; print
     the allocation physically just-below each, with its allocation backtrace. */
  if (g_atrack) {
    uintptr_t cand[7] = { uc->uc_mcontext.arm_r4, uc->uc_mcontext.arm_r5,
                          uc->uc_mcontext.arm_r6, uc->uc_mcontext.arm_r7,
                          uc->uc_mcontext.arm_r8, uc->uc_mcontext.arm_r10,
                          (uintptr_t)info->si_addr };
    const char *nm[7] = {"r4","r5","r6","r7","r8","r10","fault"};
    for (int i = 0; i < 7; i++)
      if (cand[i] > 0x02000000 && cand[i] < 0x60000000) at_dump_near(nm[i], cand[i]);
    /* deliberate-raise crashes have no useful regs: scan the stack for heap-range
       pointers (the corrupted chunk the GFx free was processing) and dump them. */
    uintptr_t *ss = (uintptr_t *)(sp & ~3u);
    int dumped = 0;
    for (int i = 0; i < 256 && dumped < 6; i++) {
      uintptr_t v = ss[i];
      if (v > 0x04000000 && v < 0x58000000) {  /* brk heap + low arena range */
        char tg[24]; snprintf(tg, sizeof(tg), "stk[%d]", i);
        at_dump_near(tg, v); dumped++;
      }
    }
  }
  /* scan stack for return addresses inside loaded modules */
  fprintf(stderr, "backtrace (stack scan):\n");
  uintptr_t *s = (uintptr_t *)(sp & ~3u);
  int shown = 0;
  for (int i = 0; i < 4096 && shown < 24; i++) {
    uintptr_t v = s[i];
    if (v >= g_main_text && v < g_main_text + g_main_text_sz) {
      fprintf(stderr, "  duck+0x%lx\n", (unsigned long)(v - g_main_text)); shown++;
    } else if (g_fx_text && v >= g_fx_text && v < g_fx_text + g_fx_sz) {
      fprintf(stderr, "  fmodex+0x%lx\n", (unsigned long)(v - g_fx_text)); shown++;
    }
  }
  /* dump maps line covering pc */
  FILE *m = fopen("/proc/self/maps", "r");
  if (m) { char ln[512]; while (fgets(ln, sizeof(ln), m)) { unsigned long s, e;
    if (sscanf(ln, "%lx-%lx", &s, &e) == 2 && pc >= s && pc < e) fprintf(stderr, ">>> %s", ln); }
    fclose(m); }
  fprintf(stderr, "=== END CRASH ===\n");
  fflush(stderr);
  _exit(128 + sig);
}
static void install_crash_handler(void) {
  struct sigaction sa; memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL); sigaction(SIGILL, &sa, NULL);
}

/* ---- load one .so into its own mmap region ---- */
static so_module *load_lib(const char *path, size_t mb) {
  size_t sz = mb * 1024 * 1024;
  void *region = mmap(NULL, sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED) { debugPrintf("mmap fail %s\n", path); return NULL; }
  if (so_load(path, region, sz) < 0) { debugPrintf("so_load fail %s\n", path); return NULL; }
  if (so_relocate() < 0) { debugPrintf("reloc fail %s\n", path); return NULL; }
  so_resolve(dynlib_functions, dynlib_numfunctions, 0);
  so_finalize();
  so_execute_init_array();
  debugPrintf("loaded %s text=%p+%zu\n", path, text_base, text_size);
  return so_save();
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  /* GFx maps allocation addresses via the low 31 bits; glibc's per-thread malloc
     arenas (mmap'd ABOVE 0x80000000) and large-block mmaps alias under that mask
     -> heap-metadata corruption. Force a SINGLE arena (low brk heap) and disable
     glibc mmap so every glibc allocation stays < 0x80000000 (bionic parity). */
  /* Single glibc arena + no mmap = ALL allocations from the low brk heap (which
     frees properly, no leak). Decoupled from the bump arena. DUCK_NO_ARENA1 off. */
  if (!getenv("DUCK_NO_ARENA1")) {
    mallopt(M_ARENA_MAX, 1);
    mallopt(M_MMAP_MAX, 0);
    mallopt(M_TRIM_THRESHOLD, -1);
  }
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  /* no core dumps (keep the small system partition safe) */
  struct rlimit rl = {0, 0};
  setrlimit(RLIMIT_CORE, &rl);
  if (getenv("DUCK_RECOVER")) g_recover = 1;
  { const char *wp = getenv("DUCK_WP");
    if (wp && wp[0]) {
      g_wp_addr = (uintptr_t)strtoul(wp, NULL, 0);
      g_wp_frame = getenv("DUCK_WPFRAME") ? atoi(getenv("DUCK_WPFRAME")) : 600;
      pthread_t th; pthread_create(&th, NULL, wp_arm_thread, NULL); pthread_detach(th);
      fprintf(stderr, "[WP] watch 0x%lx armed-at-frame %d\n", (unsigned long)g_wp_addr, g_wp_frame);
    } }
  if (getenv("DUCK_NO_FREEGUARD")) g_free_guard = 0;
  if (getenv("DUCK_NO_QUARANTINE")) g_quarantine = 0;
  if (getenv("DUCK_NO_ALLOCREC")) g_allocrec = 0;
  if (getenv("DUCK_NO_DEDUP")) g_dedup = 0;
  if (getenv("DUCK_NO_BQ")) g_bq = 0;
  { const char *hs = getenv("DUCK_HEAPSLACK");
    if (hs && hs[0]) { long v = atol(hs); if (v < 0) v = 0; g_heapslack = (size_t)((v + 15) & ~15L);
      debugPrintf("[heapslack] GMemoryHeap::Alloc padded by %zu bytes\n", g_heapslack); } }
  if (getenv("DUCK_HIST")) { g_hist = 1; debugPrintf("[hist] alloc-history UAF identifier enabled\n"); }
  if (getenv("DUCK_TEXLOG")) g_texlog = 1;
  if (getenv("DUCK_GLTEXLOG")) {
    g_gltexlog = 1;
    dt_set_import("glTexImage2D", (void *)my_glTexImage2D);
    dt_set_import("glCompressedTexImage2D", (void *)my_glCompressedTexImage2D);
    dt_set_import("glTexSubImage2D", (void *)my_glTexSubImage2D);
    tslog("init", "GLTEXLOG: glTexImage2D/Compressed/Sub logged");
  }
  if (getenv("DUCK_GLFBLOG")) {
    g_glfblog = 1;
    dt_set_import("glCheckFramebufferStatus", (void *)my_glCheckFramebufferStatus);
    dt_set_import("glBindFramebuffer", (void *)my_glBindFramebuffer);
    dt_set_import("glFramebufferTexture2D", (void *)my_glFramebufferTexture2D);
    tslog("init", "GLFBLOG: framebuffer ops logged");
  }
  if (getenv("DUCK_GLSHLOG")) {
    g_glshlog = 1;
    dt_set_import("glShaderSource", (void *)my_glShaderSource);
    dt_set_import("glCompileShader", (void *)my_glCompileShader);
    dt_set_import("glLinkProgram", (void *)my_glLinkProgram);
    dt_set_import("glAttachShader", (void *)my_glAttachShader);
    tslog("init", "GLSHLOG: shader compile/link logged");
  }
  if (getenv("DUCK_SHADERFIX")) {
    g_shaderfix = 1;
    dt_set_import("glShaderSource", (void *)my_glShaderSource);
    tslog("init", "SHADERFIX: textured frag -> passthrough (A/B test)");
  }
  if (getenv("DUCK_U1ILOG")) {
    g_u1ilog = 1;
    dt_set_import("glUniform1i", (void *)my_glUniform1i);
    dt_set_import("glGetUniformLocation", (void *)my_glGetUniformLocation);
    tslog("init", "U1ILOG: sampler uniform assignments logged");
  }
  if (getenv("DUCK_NOSTENCIL")) {
    g_nostencil = 1;
    dt_set_import("glStencilFunc", (void *)my_glStencilFunc);
    dt_set_import("glEnable", (void *)my_glEnable);
    tslog("init", "NOSTENCIL: stencil test forced always-pass (A/B test)");
  }
  if (getenv("DUCK_NO_FIXSAMPLERS")) g_fixsamplers = 0;
  if (getenv("DUCK_FIXDBG")) g_fixdbg = 1;
  if (g_fixsamplers || getenv("DUCK_DRAWLOG") || getenv("DUCK_DRAWSTOP") || getenv("DUCK_DRAWDBG")) {
    if (getenv("DUCK_DRAWLOG")) g_drawlog = 1;
    if (getenv("DUCK_DRAWSTOP")) g_drawstop = atoi(getenv("DUCK_DRAWSTOP"));
    if (getenv("DUCK_DRAWDBG")) g_drawdbg = 1;
    dt_set_import("glTexImage2D", (void *)my_glTexImage2D);   /* record dims */
    dt_set_import("glCompressedTexImage2D", (void *)my_glCompressedTexImage2D);
    dt_set_import("glBindTexture", (void *)my_glBindTexture);
    dt_set_import("glActiveTexture", (void *)my_glActiveTexture);
    dt_set_import("glUseProgram", (void *)my_glUseProgram);
    dt_set_import("glDrawElements", (void *)my_glDrawElements);
    dt_set_import("glDrawArrays", (void *)my_glDrawArrays);
    tslog("init", g_fixsamplers ? "FIXSAMPLERS: sampler units corrected (default ON)" : "DRAWLOG/DRAWSTOP tracer");
  }
  if (getenv("DUCK_NOMIP") || getenv("DUCK_TEXPARLOG")) {
    if (getenv("DUCK_NOMIP")) g_nomip = 1;
    if (getenv("DUCK_TEXPARLOG")) g_texparlog = 1;
    dt_set_import("glTexParameteri", (void *)my_glTexParameteri);
    dt_set_import("glGenerateMipmap", (void *)my_glGenerateMipmap);
    tslog("init", "NOMIP/TEXPARLOG: texture min-filter clamp + mipmap log");
  }
  install_crash_handler();
  tslog("init", "=== DuckTales Remastered -> Mali-450 (armv7 so-loader) ===");

  /* wire pthread shims into the import table before any resolution */
  recon_wire_pthread(dt_set_import);
  /* bionic-layout stat/fstat: glibc fstat writes st_size at the wrong offset
     for NDK-built code -> wrong file sizes -> truncated buffers. */
  { extern int bionic_fstat(int, void *), bionic_stat(const char *, void *),
        bionic_lstat(const char *, void *), bionic_fstatat(int, const char *, void *, int);
    dt_set_import("fstat", (void *)bionic_fstat);
    dt_set_import("stat", (void *)bionic_stat);
    dt_set_import("lstat", (void *)bionic_lstat);
    dt_set_import("fstatat", (void *)bionic_fstatat); }
  /* setjmp/longjmp: bionic arm jmp_buf = long[64] = 256B. glibc setjmp() =
     __sigsetjmp(env,1) ALSO saves the signal mask -> writes ~132B PAST the
     bionic-sized inline jmp_buf -> deterministic heap/free-list corruption
     during asset-parser error setup. _setjmp/_longjmp (savemask=0) write only
     the 256B __jmpbuf -> fit the bionic buffer. Engine parsers don't rely on
     sigmask save/restore, so the non-local jump semantics are preserved. */
  if (!getenv("DUCK_NO_SETJMP_FIX")) {
    /* _setjmp/_longjmp come from <setjmp.h>; cast to the import slot type */
    dt_set_import("setjmp", (void *)_setjmp);
    dt_set_import("longjmp", (void *)_longjmp);
    dt_set_import("siglongjmp", (void *)_longjmp);
  }
  if (getenv("DUCK_MEMGUARD")) {
    g_memguard = 1;
    dt_set_import("memcpy", (void *)my_memcpy);
    dt_set_import("memmove", (void *)my_memmove);
    tslog("init", "MEMGUARD: memcpy/memmove intercepted");
  }
  if (getenv("DUCK_NORAISE")) g_noraise = 1;   /* opt-in: tolerate the GFx integrity assert */
  if (getenv("DUCK_NO_NORAISE")) g_noraise = 0;
  if (g_noraise) {     /* tolerate the GFx integrity assert (deliberate raise, si_code<=0) */
    dt_set_import("raise", (void *)my_raise);
    dt_set_import("abort", (void *)my_abort);
    dt_set_import("syscall", (void *)my_syscall);
    tslog("init", "NORAISE: raise/abort/syscall(tgkill) suppressed");
  }
  /* 16-byte malloc alignment (bionic parity) + LOW-address arena for large
     allocations (GFx maps addresses via low 31 bits; high glibc-mmap heaps alias
     and corrupt). DUCK_NO_ALIGN16=1 disables; DUCK_NO_LOWHEAP=1 disables low arena. */
  if (getenv("DUCK_NO_LOWHEAP")) g_lowheap = 0;
  if (!getenv("DUCK_NO_ALIGN16")) {
    if (g_lowheap) low_init();
    dt_set_import("malloc", (void *)m16_malloc);
    dt_set_import("calloc", (void *)m16_calloc);
    dt_set_import("realloc", (void *)m16_realloc);
    dt_set_import("free", (void *)m16_free);
    dt_set_import("memalign", (void *)m16_memalign);
    dt_set_import("posix_memalign", (void *)m16_posix_memalign);
    dt_set_import("aligned_alloc", (void *)m16_memalign);  /* same (align,size) order as memalign */
    if (g_lowheap) dt_set_import("syscall", (void *)my_syscall);  /* force mmap2 low */
    tslog("init", "malloc/calloc/realloc/free/memalign -> 16-byte aligned + low arena");
  }
  tslog("init", "pthread + bionic-stat + setjmp shims wired");

  const char *libdir = getenv("DUCK_LIBDIR");
  if (!libdir) libdir = "./lib";
  char p[1024];

  /* 1. fmodex (no fmod deps), 2. fmodevent (needs fmodex), 3. ducktales */
  snprintf(p, sizeof(p), "%s/libfmodex.so", libdir);
  g_m_fmodex = load_lib(p, 16);
  if (!g_m_fmodex) debugPrintf("WARN: libfmodex failed (audio off)\n");
  else { g_fx_text = (uintptr_t)text_base; g_fx_sz = text_size; }

  snprintf(p, sizeof(p), "%s/libfmodevent.so", libdir);
  g_m_fmodevent = load_lib(p, 8);
  if (g_m_fmodevent) { g_fe_text = (uintptr_t)text_base; g_fe_sz = text_size; }
  if (!g_m_fmodevent) debugPrintf("WARN: libfmodevent failed\n");

  snprintf(p, sizeof(p), "%s/libducktales.so", libdir);
  size_t hs = 160 * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) fatal_error("heap mmap failed");
  if (so_load(p, heap, hs) < 0) fatal_error("load libducktales failed");
  if (so_relocate() < 0) fatal_error("relocate libducktales failed");
  debugPrintf("Resolving %zu imports...\n", dynlib_numfunctions);
  so_resolve(dynlib_functions, dynlib_numfunctions, 0);
  so_finalize();
  so_flush_caches();
  so_execute_init_array();
  g_main_text = (uintptr_t)text_base; g_main_text_sz = text_size;
  install_ducktales_hooks();
  g_m_main = so_save();

  uintptr_t android_main_addr = so_find_addr("android_main");
  if (!android_main_addr) fatal_error("android_main not found");
  debugPrintf("android_main @ %p\n", (void *)android_main_addr);

  /* JNI_OnLoad (registers natives, inits FMODAudioDevice bridge) */
  uintptr_t onload = so_find_addr_safe("JNI_OnLoad");
  if (onload) {
    void *fake_vm = NULL, *fake_env = NULL;
    jni_shim_init(&fake_vm, &fake_env);
    typedef int (*onload_t)(void *, void *);
    int ver = ((onload_t)onload)(fake_vm, NULL);
    debugPrintf("JNI_OnLoad -> 0x%x\n", ver);
  }

  /* SDL window + GL context up front (main thread) */
  egl_shim_create_window();

  /* fake android environment */
  struct android_app *app = android_shim_init();
  if (!app) fatal_error("android_shim_init failed");

  android_shim_send_cmd(app, APP_CMD_INIT_WINDOW);
  android_shim_send_cmd(app, APP_CMD_GAINED_FOCUS);

  start_watchdog();
  tslog("init", "calling android_main");
  void (*amain)(struct android_app *) = (void (*)(struct android_app *))android_main_addr;
  amain(app);
  tslog("init", "android_main returned");
  _exit(0);
}
