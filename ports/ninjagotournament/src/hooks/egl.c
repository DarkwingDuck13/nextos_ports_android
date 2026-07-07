/* egl.c -- wrapper-owned EGL/GL over SDL2 + single-context handover (Linux/Mali)
 *
 * TFA's libProject_Douglas_HH.so is a classic Android GLSurfaceView client: it
 * does NOT own the display/window/context/swap -- the Java GLSurfaceView did.
 * Here the wrapper owns them via SDL2 (which brings up EGL/GLES2 on the Mali
 * fbdev), and drives nativeInit/nativeRender on this (render) thread.
 *
 * The engine also creates its OWN shared loader context on an async thread.
 * The Mali driver does not honour cross-context object sharing here, so -- like
 * the reference port -- we use exactly ONE real GL context (the SDL context)
 * handed back and forth between the render thread and the engine's loader
 * thread. A thread must "own" the context (SDL_GL_MakeCurrent) before issuing
 * GL; ownership is sticky and released cooperatively at frame boundaries and
 * blocking points. This both serialises GL and keeps every object in one
 * namespace (uploads stay visible).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <SDL2/SDL.h>
#include <GLES2/gl2.h>

#include "../config.h"
#include "../util.h"
#include "../hooks.h"
#include "../fps.h"
#include "../dxt.h"

static SDL_Window   *g_window = NULL;
static SDL_GLContext g_ctx = NULL;

static unsigned cur_frame_no = 0; // updated once per frame by egl_fbo_frame_summary

volatile int egl_swap_count = 0;
volatile unsigned long long egl_last_compile_tick = 0;

static uint64_t now_tick(void) { return (uint64_t)SDL_GetPerformanceCounter(); }

// ---------------------------------------------------------------------------
// single-context ownership handover
// ---------------------------------------------------------------------------

static pthread_mutex_t ho_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ho_cond = PTHREAD_COND_INITIALIZER;
static volatile uintptr_t ctx_owner = 0;   // tid (pthread_self) that owns g_ctx; 0 = none
static volatile int       release_req = 0;

static uintptr_t cur_tid(void) { return (uintptr_t)pthread_self(); }

static void gl_acquire(void) {
  const uintptr_t me = cur_tid();
  if (__atomic_load_n(&ctx_owner, __ATOMIC_ACQUIRE) == me)
    return; // already ours
  pthread_mutex_lock(&ho_mtx);
  while (ctx_owner != 0 && ctx_owner != me) {
    release_req = 1;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 5 * 1000 * 1000; // 5ms watchdog slice
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&ho_cond, &ho_mtx, &ts);
  }
  if (ctx_owner != me) {
    SDL_GL_MakeCurrent(g_window, g_ctx);
    __atomic_store_n(&ctx_owner, me, __ATOMIC_RELEASE);
  }
  release_req = 0;
  pthread_mutex_unlock(&ho_mtx);
}

static void gl_release_if_wanted(void) {
  if (!__atomic_load_n(&release_req, __ATOMIC_ACQUIRE))
    return;
  const uintptr_t me = cur_tid();
  pthread_mutex_lock(&ho_mtx);
  if (ctx_owner == me && release_req) {
    SDL_GL_MakeCurrent(g_window, NULL);
    __atomic_store_n(&ctx_owner, 0, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&ho_cond);
  }
  pthread_mutex_unlock(&ho_mtx);
}

static inline void gl_noop(void) {}
#define GLL() gl_acquire()
#define GLU() gl_noop()

int egl_bringup(void) {
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  g_window = SDL_CreateWindow("LEGO Ninjago Tournament",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              screen_width, screen_height,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
  if (!g_window) {
    debugPrintf("EGL: SDL_CreateWindow failed: %s\n", SDL_GetError());
    return -1;
  }
  g_ctx = SDL_GL_CreateContext(g_window);
  if (!g_ctx) {
    debugPrintf("EGL: SDL_GL_CreateContext failed: %s\n", SDL_GetError());
    return -1;
  }
  SDL_GL_MakeCurrent(g_window, g_ctx);
  __atomic_store_n(&ctx_owner, cur_tid(), __ATOMIC_RELEASE);
  // TFA is a fixed-timestep 30 fps game; ask for swap interval 2 (main also
  // caps the loop to 30 fps in case the driver doesn't block two vsyncs).
  if (SDL_GL_SetSwapInterval(2) != 0)
    SDL_GL_SetSwapInterval(1);
  debugPrintf("EGL: window+context up (%dx%d) via SDL/GLES2\n", screen_width, screen_height);
  return 0;
}

void egl_present(void) {
  gl_acquire();
  if (config.show_fps)
    fps_render();
  // Amlogic fbdev OSD blends fb0 by PIXEL ALPHA: the post-processing composite
  // writes alpha=0, which scans out as "transparent" (black TV) even though the
  // colour channels hold the full image. Force the alpha plane opaque pre-swap.
  {
    GLboolean scis = glIsEnabled(GL_SCISSOR_TEST);
    GLfloat cc[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    if (scis) glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    if (scis) glEnable(GL_SCISSOR_TEST);
  }
  SDL_GL_SwapWindow(g_window);
  ++egl_swap_count;
  gl_release_if_wanted();
}

void egl_resize_surface(int w, int h) {
  (void)w; (void)h; // fullscreen fbdev: SDL owns the mode; nothing to recreate
}

EGLDisplay egl_display(void) { return (EGLDisplay)g_ctx; }

// blocking-point / exit handover hooks (called from pthr.c / libc_shim.c)
void egl_gl_ownership_park(void) {
  const uintptr_t me = cur_tid();
  if (__atomic_load_n(&ctx_owner, __ATOMIC_ACQUIRE) != me)
    return;
  pthread_mutex_lock(&ho_mtx);
  if (ctx_owner == me) {
    SDL_GL_MakeCurrent(g_window, NULL);
    __atomic_store_n(&ctx_owner, 0, __ATOMIC_RELEASE);
    release_req = 0;
    pthread_cond_broadcast(&ho_cond);
  }
  pthread_mutex_unlock(&ho_mtx);
}
void egl_gl_ownership_release(void) { egl_gl_ownership_park(); }
void egl_gl_service_handover(void) { gl_release_if_wanted(); }
void egl_gl_acquire(void) { gl_acquire(); }
int  egl_gl_thread_holds_context(void) {
  return __atomic_load_n(&ctx_owner, __ATOMIC_ACQUIRE) == cur_tid();
}

// ---------------------------------------------------------------------------
// fake pbuffer fallback for the loader thread
// ---------------------------------------------------------------------------

#define FAKE_PBUFFER_MAGIC 0x50425546u
#define MAX_FAKE_PBUFFERS 8
typedef struct { uint32_t magic; EGLint width, height; } FakePbuffer;
static FakePbuffer *fake_pbuffers[MAX_FAKE_PBUFFERS];
static int fake_pbuffer_count = 0;

static FakePbuffer *fake_from_surface(EGLSurface s) {
  for (int i = 0; i < fake_pbuffer_count; i++)
    if ((EGLSurface)fake_pbuffers[i] == s) return fake_pbuffers[i];
  return NULL;
}
static EGLint attrib_value(const EGLint *attribs, EGLint key, EGLint fb) {
  if (!attribs) return fb;
  for (int i = 0; attribs[i] != EGL_NONE; i += 2)
    if (attribs[i] == key) return attribs[i + 1];
  return fb;
}

// ---------------------------------------------------------------------------
// EGL pass-through hooks bound to the game's imports
// ---------------------------------------------------------------------------

EGLBoolean eglBindAPIHook(EGLenum api) { (void)api; return EGL_TRUE; }

EGLBoolean eglChooseConfigHook(EGLDisplay dpy, const EGLint *attrib_list,
                               EGLConfig *configs, EGLint config_size, EGLint *num_config) {
  (void)dpy; (void)attrib_list;
  if (configs && config_size > 0)
    configs[0] = (EGLConfig)(uintptr_t)0x1;
  if (num_config)
    *num_config = 1;
  return EGL_TRUE;
}

#define FAKE_CONTEXT_TOKEN ((EGLContext)0x0CC1F00D)

EGLContext eglCreateContextHook(EGLDisplay dpy, EGLConfig config,
                                EGLContext share_context, const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)share_context; (void)attrib_list;
  debugPrintf("EGL: eglCreateContext(share=%p) -> fake token (one real ctx)\n", share_context);
  return FAKE_CONTEXT_TOKEN;
}

EGLSurface eglCreatePbufferSurfaceHook(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list) {
  (void)dpy; (void)config;
  if (fake_pbuffer_count >= MAX_FAKE_PBUFFERS)
    return (EGLSurface)fake_pbuffers[0];
  FakePbuffer *f = calloc(1, sizeof(*f));
  if (!f) return EGL_NO_SURFACE;
  f->magic = FAKE_PBUFFER_MAGIC;
  f->width = attrib_value(attrib_list, EGL_WIDTH, 1);
  f->height = attrib_value(attrib_list, EGL_HEIGHT, 1);
  fake_pbuffers[fake_pbuffer_count++] = f;
  return (EGLSurface)f;
}

EGLBoolean eglMakeCurrentHook(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
  // virtual: the one real context is bound lazily by gl_acquire() on whichever
  // thread next issues GL. We just accept the engine's loader "activation".
  (void)dpy; (void)draw; (void)read; (void)ctx;
  return EGL_TRUE;
}

EGLBoolean eglQuerySurfaceHook(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value) {
  (void)dpy;
  FakePbuffer *f = fake_from_surface(surface);
  if (f) {
    if (value) *value = (attribute == EGL_WIDTH) ? f->width
                       : (attribute == EGL_HEIGHT) ? f->height : 0;
    return EGL_TRUE;
  }
  if (value) {
    if (attribute == EGL_WIDTH)  { *value = screen_width;  return EGL_TRUE; }
    if (attribute == EGL_HEIGHT) { *value = screen_height; return EGL_TRUE; }
    *value = 0;
  }
  return EGL_TRUE;
}

EGLBoolean eglSwapIntervalHook(EGLDisplay dpy, EGLint interval) {
  (void)dpy; (void)interval;
  GLL();
  SDL_GL_SetSwapInterval(2); // force 30 fps pacing
  GLU();
  return EGL_TRUE;
}

void *eglGetProcAddressHook(const char *name) {
  if (name && !strcmp(name, "glGetString"))
    return (void *)glGetStringHook;
  return (void *)SDL_GL_GetProcAddress(name);
}

// ---------------------------------------------------------------------------
// GL call hooks: every GL entry the engine imports runs only while this thread
// owns the one real context. Shader compile/link additionally logged + tick.
// ---------------------------------------------------------------------------

#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif

void glCompileShaderHook(GLuint shader) {
  GLL();
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    static char buf[1024]; GLsizei n = 0;
    glGetShaderInfoLog(shader, sizeof(buf), &n, buf);
    debugPrintf("GL: shader %u COMPILE FAILED: %s\n", shader, buf);
  }
  egl_last_compile_tick = now_tick();
  GLU();
}
void glLinkProgramHook(GLuint program) {
  GLL();
  glLinkProgram(program);
  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    static char buf[1024]; GLsizei n = 0;
    glGetProgramInfoLog(program, sizeof(buf), &n, buf);
    debugPrintf("GL: program %u LINK FAILED: %s\n", program, buf);
  }
  egl_last_compile_tick = now_tick();
  GLU();
}
GLuint glCreateShaderHook(GLenum type) { GLL(); GLuint r = glCreateShader(type); GLU(); return r; }
GLuint glCreateProgramHook(void) { GLL(); GLuint r = glCreateProgram(); GLU(); return r; }
void glShaderSourceHook(GLuint s, GLsizei c, const GLchar *const *str, const GLint *len) { GLL(); glShaderSource(s, c, str, len); GLU(); }
void glAttachShaderHook(GLuint p, GLuint s) { GLL(); glAttachShader(p, s); GLU(); }
void glBindAttribLocationHook(GLuint p, GLuint i, const GLchar *n) { GLL(); glBindAttribLocation(p, i, n); GLU(); }
void glUseProgramHook(GLuint p) { GLL(); glUseProgram(p); GLU(); }
void glDeleteShaderHook(GLuint s) { GLL(); glDeleteShader(s); GLU(); }
void glDeleteProgramHook(GLuint p) { GLL(); glDeleteProgram(p); GLU(); }
void glGetActiveAttribHook(GLuint p, GLuint i, GLsizei b, GLsizei *l, GLint *sz, GLenum *t, GLchar *n) { GLL(); glGetActiveAttrib(p, i, b, l, sz, t, n); GLU(); }
void glGetActiveUniformHook(GLuint p, GLuint i, GLsizei b, GLsizei *l, GLint *sz, GLenum *t, GLchar *n) { GLL(); glGetActiveUniform(p, i, b, l, sz, t, n); GLU(); }
GLint glGetAttribLocationHook(GLuint p, const GLchar *n) { GLL(); GLint r = glGetAttribLocation(p, n); GLU(); return r; }
GLint glGetUniformLocationHook(GLuint p, const GLchar *n) { GLL(); GLint r = glGetUniformLocation(p, n); GLU(); return r; }
void glGetProgramInfoLogHook(GLuint p, GLsizei b, GLsizei *l, GLchar *log) { GLL(); glGetProgramInfoLog(p, b, l, log); GLU(); }
void glGetProgramivHook(GLuint p, GLenum n, GLint *v) { GLL(); glGetProgramiv(p, n, v); GLU(); }
void glGetShaderInfoLogHook(GLuint s, GLsizei b, GLsizei *l, GLchar *log) { GLL(); glGetShaderInfoLog(s, b, l, log); GLU(); }
void glGetShaderivHook(GLuint s, GLenum n, GLint *v) { GLL(); glGetShaderiv(s, n, v); GLU(); }

void glUniform1fvHook(GLint l, GLsizei c, const GLfloat *v) { GLL(); glUniform1fv(l, c, v); GLU(); }
void glUniform1iHook(GLint l, GLint v0) { GLL(); glUniform1i(l, v0); GLU(); }
void glUniform2fvHook(GLint l, GLsizei c, const GLfloat *v) { GLL(); glUniform2fv(l, c, v); GLU(); }
void glUniform3fvHook(GLint l, GLsizei c, const GLfloat *v) { GLL(); glUniform3fv(l, c, v); GLU(); }
void glUniform4fvHook(GLint l, GLsizei c, const GLfloat *v) { GLL(); glUniform4fv(l, c, v); GLU(); }
void glUniformMatrix2fvHook(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { GLL(); glUniformMatrix2fv(l, c, t, v); GLU(); }
void glUniformMatrix3fvHook(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { GLL(); glUniformMatrix3fv(l, c, t, v); GLU(); }
void glUniformMatrix4fvHook(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { GLL(); glUniformMatrix4fv(l, c, t, v); GLU(); }

void glVertexAttribPointerHook(GLuint i, GLint s, GLenum t, GLboolean nm, GLsizei st, const void *p) { GLL(); glVertexAttribPointer(i, s, t, nm, st, p); GLU(); }
void glEnableVertexAttribArrayHook(GLuint i) { GLL(); glEnableVertexAttribArray(i); GLU(); }
void glDisableVertexAttribArrayHook(GLuint i) { GLL(); glDisableVertexAttribArray(i); GLU(); }

void glActiveTextureHook(GLenum t) { GLL(); glActiveTexture(t); GLU(); }
void glBindTextureHook(GLenum t, GLuint tex) { GLL(); glBindTexture(t, tex); GLU(); }
void glGenTexturesHook(GLsizei n, GLuint *t) { GLL(); glGenTextures(n, t); GLU(); }
void glDeleteTexturesHook(GLsizei n, const GLuint *t) { GLL(); glDeleteTextures(n, t); GLU(); }
// texture-upload diagnostics: a black logo/background usually means the upload
// itself failed (unsupported compressed format on Mali) or never happened. Log
// the first uploads and EVERY new (format, error) combination seen.
static int texlog_budget = 96;

void glTexImage2DHook(GLenum tg, GLint lv, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum fmt, GLenum ty, const void *px) {
  GLL();
  glTexImage2D(tg, lv, ifmt, w, h, b, fmt, ty, px);
  if (px == NULL) { // render-target allocation: formats/sizes matter on Mali-450
    GLint tex = 0;
    glGetIntegerv(0x8069 /*GL_TEXTURE_BINDING_2D*/, &tex);
    debugPrintf("RT: tex=%d %dx%d ifmt=0x%x type=0x%x err=0x%x\n",
                tex, w, h, ifmt, ty, glGetError());
  } else {
    GLenum err = glGetError();
    if (err != 0 || texlog_budget > 0) {
      if (texlog_budget > 0) texlog_budget--;
      GLint tex = 0;
      glGetIntegerv(0x8069, &tex);
      debugPrintf("TEX: tex=%d lv=%d %dx%d fmt=0x%x type=0x%x err=0x%x\n",
                  tex, lv, w, h, fmt, ty, err);
    }
  }
  GLU();
}
void glCompressedTexImage2DHook(GLenum tg, GLint lv, GLenum ifmt, GLsizei w, GLsizei h, GLint b, GLsizei sz, const void *d) {
  GLL();
  // Mali-450 nao tem S3TC: os .fib deste OBB trazem DXT1/DXT5 -> decode em CPU
  // e upload como RGB565/RGBA8888 (dxt.c). Sem isso todo upload falha com
  // GL_INVALID_ENUM e o menu fica preto (fundo/logos).
  if (dxt_upload(tg, lv, ifmt, w, h, sz, d)) { GLU(); return; }
  glCompressedTexImage2D(tg, lv, ifmt, w, h, b, sz, d);
  GLenum err = glGetError();
  if (err != 0 || texlog_budget > 0) {
    if (texlog_budget > 0) texlog_budget--;
    GLint tex = 0;
    glGetIntegerv(0x8069, &tex);
    debugPrintf("CTEX: tex=%d lv=%d %dx%d ifmt=0x%x sz=%d err=0x%x\n",
                tex, lv, w, h, ifmt, sz, err);
  }
  GLU();
}
void glTexParameterfHook(GLenum tg, GLenum p, GLfloat v) { GLL(); glTexParameterf(tg, p, v); GLU(); }
void glTexParameteriHook(GLenum tg, GLenum p, GLint v) { GLL(); glTexParameteri(tg, p, v); GLU(); }
void glBindBufferHook(GLenum t, GLuint b) { GLL(); glBindBuffer(t, b); GLU(); }
void glGenBuffersHook(GLsizei n, GLuint *b) { GLL(); glGenBuffers(n, b); GLU(); }
void glDeleteBuffersHook(GLsizei n, const GLuint *b) { GLL(); glDeleteBuffers(n, b); GLU(); }
void glBufferDataHook(GLenum t, GLsizeiptr sz, const void *d, GLenum u) { GLL(); glBufferData(t, sz, d, u); GLU(); }
void glGetBufferParameterivHook(GLenum t, GLenum p, GLint *v) { GLL(); glGetBufferParameteriv(t, p, v); GLU(); }
// FBO diagnostics: the frontend 3D scene is drawn into an offscreen buffer and
// composited over the backbuffer; a black menu background means that path dies
// somewhere between attach and composite. Log setup + per-frame draw counts.
static volatile GLuint cur_fbo = 0;
static volatile int draws_fbo0 = 0, draws_fboN = 0;
static int fbo_log_budget = 64;

void glBindFramebufferHook(GLenum t, GLuint f) {
  GLL();
  glBindFramebuffer(t, f);
  cur_fbo = f;
  GLU();
}
void glGenFramebuffersHook(GLsizei n, GLuint *f) { GLL(); glGenFramebuffers(n, f); GLU(); }
void glDeleteFramebuffersHook(GLsizei n, const GLuint *f) { GLL(); glDeleteFramebuffers(n, f); GLU(); }
void glFramebufferTexture2DHook(GLenum tg, GLenum at, GLenum tt, GLuint tex, GLint lv) {
  GLL();
  glFramebufferTexture2D(tg, at, tt, tex, lv);
  if (fbo_log_budget > 0) {
    fbo_log_budget--;
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    debugPrintf("FBO: fbo=%u attach tex=%u at=0x%x -> status=0x%x\n", cur_fbo, tex, at, st);
  }
  GLU();
}
void glFramebufferRenderbufferHook(GLenum tg, GLenum at, GLenum rt, GLuint rb) {
  GLL();
  glFramebufferRenderbuffer(tg, at, rt, rb);
  if (fbo_log_budget > 0) {
    fbo_log_budget--;
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    debugPrintf("FBO: fbo=%u attach rb=%u at=0x%x -> status=0x%x\n", cur_fbo, rb, at, st);
  }
  GLU();
}
void glBindRenderbufferHook(GLenum t, GLuint rb) { GLL(); glBindRenderbuffer(t, rb); GLU(); }
void glGenRenderbuffersHook(GLsizei n, GLuint *rb) { GLL(); glGenRenderbuffers(n, rb); GLU(); }
void glDeleteRenderbuffersHook(GLsizei n, const GLuint *rb) { GLL(); glDeleteRenderbuffers(n, rb); GLU(); }
void glRenderbufferStorageHook(GLenum t, GLenum ifmt, GLsizei w, GLsizei h) {
  GLL();
  glRenderbufferStorage(t, ifmt, w, h);
  if (fbo_log_budget > 0)
    debugPrintf("FBO: renderbuffer storage ifmt=0x%x %dx%d err=0x%x\n", ifmt, w, h, glGetError());
  GLU();
}

void egl_fbo_frame_summary(unsigned frame) {
  cur_frame_no = frame;
  if (frame < 12 || (frame % 600) == 0)
    debugPrintf("FBO: frame %u draws fbo0=%d fboN=%d (last fbo=%u)\n",
                frame, draws_fbo0, draws_fboN, cur_fbo);
  draws_fbo0 = 0;
  draws_fboN = 0;
}

void glClearHook(GLbitfield m) { GLL(); glClear(m); GLU(); }
void glClearColorHook(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { GLL(); glClearColor(r, g, b, a); GLU(); }
void glClearDepthfHook(GLfloat d) { GLL(); glClearDepthf(d); GLU(); }
void glClearStencilHook(GLint s) { GLL(); glClearStencil(s); GLU(); }
void glDrawArraysHook(GLenum m, GLint f, GLsizei c) { GLL(); glDrawArrays(m, f, c); if (cur_fbo) draws_fboN++; else draws_fbo0++; GLU(); }
void glDrawElementsHook(GLenum m, GLsizei c, GLenum t, const void *i) { GLL(); glDrawElements(m, c, t, i); if (cur_fbo) draws_fboN++; else draws_fbo0++; GLU(); }
void glViewportHook(GLint x, GLint y, GLsizei w, GLsizei h) { GLL(); glViewport(x, y, w, h); GLU(); }
void glScissorHook(GLint x, GLint y, GLsizei w, GLsizei h) { GLL(); glScissor(x, y, w, h); GLU(); }
void glEnableHook(GLenum c) { GLL(); glEnable(c); GLU(); }
void glDisableHook(GLenum c) { GLL(); glDisable(c); GLU(); }
void glBlendEquationHook(GLenum m) { GLL(); glBlendEquation(m); GLU(); }
void glBlendFuncHook(GLenum s, GLenum d) { GLL(); glBlendFunc(s, d); GLU(); }
void glDepthFuncHook(GLenum f) { GLL(); glDepthFunc(f); GLU(); }
void glDepthMaskHook(GLboolean f) { GLL(); glDepthMask(f); GLU(); }
void glDepthRangefHook(GLfloat n, GLfloat f) { GLL(); glDepthRangef(n, f); GLU(); }
void glColorMaskHook(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { GLL(); glColorMask(r, g, b, a); GLU(); }
void glCullFaceHook(GLenum m) { GLL(); glCullFace(m); GLU(); }
void glFrontFaceHook(GLenum m) { GLL(); glFrontFace(m); GLU(); }
void glPolygonOffsetHook(GLfloat f, GLfloat u) { GLL(); glPolygonOffset(f, u); GLU(); }
void glStencilFuncHook(GLenum f, GLint r, GLuint m) { GLL(); glStencilFunc(f, r, m); GLU(); }
void glStencilMaskHook(GLuint m) { GLL(); glStencilMask(m); GLU(); }
void glStencilOpHook(GLenum f, GLenum zf, GLenum zp) { GLL(); glStencilOp(f, zf, zp); GLU(); }
void glFinishHook(void) { GLL(); glFinish(); GLU(); }
void glFlushHook(void) { GLL(); glFlush(); GLU(); }
void glGetIntegervHook(GLenum n, GLint *d) { GLL(); glGetIntegerv(n, d); GLU(); }
GLenum glGetErrorHook(void) { GLL(); GLenum r = glGetError(); GLU(); return r; }

const GLubyte *glGetStringHook(GLenum name) {
  GLL();
  const GLubyte *s = glGetString(name);
  GLU();
  if (s) {
    if (name == GL_VENDOR || name == GL_RENDERER || name == GL_VERSION)
      debugPrintf("GL: glGetString(0x%x) = %s\n", name, (const char *)s);
    return s;
  }
  switch (name) {
    case GL_VENDOR:   return (const GLubyte *)"ARM";
    case GL_RENDERER: return (const GLubyte *)"Mali-450 MP";
    case GL_VERSION:  return (const GLubyte *)"OpenGL ES 2.0";
    case 0x8B8C:      return (const GLubyte *)"OpenGL ES GLSL ES 1.00";
    case GL_EXTENSIONS:
      return (const GLubyte *)"GL_OES_compressed_ETC1_RGB8_texture "
                              "GL_OES_depth24 GL_OES_packed_depth_stencil "
                              "GL_OES_depth_texture";
    default:          return (const GLubyte *)"";
  }
}
