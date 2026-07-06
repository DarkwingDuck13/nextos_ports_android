/*
 * egl_shim.c -- EGL wrapper backed by SDL2 (OpenGL ES 2.0)
 *
 * Each fake EGL context gets a real SDL GL context. We keep a bootstrap
 * context around as the share root so all contexts can share resources.
 */

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "egl_shim.h"
#include "util.h"

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720

const GLubyte *glGetString_wrap(GLenum name);
void glCompressedTexImage2D_wrap(GLenum target, GLint level,
                                 GLenum internalformat, GLsizei width,
                                 GLsizei height, GLint border,
                                 GLsizei imageSize, const void *data);
void glTexImage2D_wrap(GLenum target, GLint level, GLint internalformat,
                       GLsizei width, GLsizei height, GLint border,
                       GLenum format, GLenum type, const void *pixels);
void glTexParameteri_wrap(GLenum target, GLenum pname, GLint param);
void glTexSubImage2D_wrap(GLenum target, GLint level, GLint xoffset,
                          GLint yoffset, GLsizei width, GLsizei height,
                          GLenum format, GLenum type, const void *pixels);
void glPixelStorei_wrap(GLenum pname, GLint param);
void glCopyTexImage2D_wrap(GLenum target, GLint level, GLenum internalformat,
                           GLint x, GLint y, GLsizei width, GLsizei height,
                           GLint border);
void glCopyTexSubImage2D_wrap(GLenum target, GLint level, GLint xoffset,
                              GLint yoffset, GLint x, GLint y, GLsizei width,
                              GLsizei height);

typedef struct {
  SDL_GLContext sdl_context;
  EGLBoolean is_pbuffer;
  int id;
} _egl_context;

static SDL_Window *egl_window = NULL;
static SDL_GLContext egl_share_root = NULL;
static pthread_mutex_t egl_context_create_mutex = PTHREAD_MUTEX_INITIALIZER;
static int frame_count = 0;
static int next_context_id = 1;

static _Thread_local _egl_context *current_context = NULL;
static _Thread_local _egl_context *last_context = NULL;
static _Thread_local int has_real_gl = 0;

SDL_Window *egl_shim_get_window(void) { return egl_window; }

static uintptr_t tls_stack_guard_read(void) {
#if defined(__aarch64__)
  uintptr_t tls = 0;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
  if (tls)
    return *(uintptr_t *)(tls + 0x28);
#endif
  return 0;
}

static void tls_stack_guard_write(uintptr_t guard) {
#if defined(__aarch64__)
  uintptr_t tls = 0;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
  if (tls)
    *(uintptr_t *)(tls + 0x28) = guard;
#else
  (void)guard;
#endif
}

static SDL_GLContext guarded_create_context(SDL_Window *window) {
  uintptr_t guard = tls_stack_guard_read();
  SDL_GLContext context = SDL_GL_CreateContext(window);
  tls_stack_guard_write(guard);
  return context;
}

static int guarded_make_current(SDL_Window *window, SDL_GLContext context) {
  uintptr_t guard = tls_stack_guard_read();
  int ret = SDL_GL_MakeCurrent(window, context);
  tls_stack_guard_write(guard);
  return ret;
}

static void guarded_swap_window(SDL_Window *window) {
  uintptr_t guard = tls_stack_guard_read();
  SDL_GL_SwapWindow(window);
  tls_stack_guard_write(guard);
}

static void guarded_delete_context(SDL_GLContext context) {
  uintptr_t guard = tls_stack_guard_read();
  SDL_GL_DeleteContext(context);
  tls_stack_guard_write(guard);
}

void egl_shim_create_window(void) {
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  egl_window = SDL_CreateWindow(
      "Legend of Mana", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      SCREEN_WIDTH, SCREEN_HEIGHT,
      SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
  if (!egl_window) {
    debugPrintf("egl_shim: SDL_CreateWindow FAILED: %s\n", SDL_GetError());
    return;
  }
  debugPrintf("egl_shim: Window created %dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);

  egl_share_root = guarded_create_context(egl_window);
  if (!egl_share_root) {
    debugPrintf("egl_shim: SDL_GL_CreateContext FAILED: %s\n", SDL_GetError());
    return;
  }
  debugPrintf("egl_shim: GL share-root context created\n");

  guarded_make_current(egl_window, NULL);
  debugPrintf("egl_shim: Context released, ready for game\n");
}

/* --- Mutex hooks (called from imports.c pthread wrappers) --- */

void egl_shim_on_mutex_post_lock(void *mutex_id) {
  (void)mutex_id;
}

void egl_shim_on_mutex_pre_unlock(void *mutex_id) {
  (void)mutex_id;
}

int egl_shim_ensure_current(void) {
  if (has_real_gl)
    return 1;
  _egl_context *ctx = current_context ? current_context : last_context;
  if (!egl_window || !ctx || !ctx->sdl_context)
    return 0;

  int ret = guarded_make_current(egl_window, ctx->sdl_context);
  if (ret == 0) {
    has_real_gl = 1;
    current_context = ctx;
    debugPrintf("egl_shim: restored current context [tid=%lx] [ctx_id=%d]\n",
                (unsigned long)pthread_self(), ctx->id);
    return 1;
  }

  debugPrintf("egl_shim: failed to restore current context [tid=%lx] [ctx_id=%d]: %s\n",
              (unsigned long)pthread_self(), ctx->id, SDL_GetError());
  return 0;
}

/* --- EGL API --- */

EGLDisplay egl_shim_GetDisplay(EGLNativeDisplayType display_id) {
  (void)display_id;
  debugPrintf("egl_shim: eglGetDisplay()\n");
  return (EGLDisplay)strdup("display");
}

EGLBoolean egl_shim_Initialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  (void)dpy;
  if (major) *major = 1;
  if (minor) *minor = 4;
  debugPrintf("egl_shim: eglInitialize() -> 1.4\n");
  return EGL_TRUE;
}

EGLBoolean egl_shim_Terminate(EGLDisplay dpy) {
  (void)dpy;
  debugPrintf("egl_shim: eglTerminate()\n");
  if (egl_share_root) {
    guarded_delete_context(egl_share_root);
    egl_share_root = NULL;
  }
  if (egl_window) {
    SDL_DestroyWindow(egl_window);
    egl_window = NULL;
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_ChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                                  EGLConfig *configs, EGLint config_size,
                                  EGLint *num_config) {
  (void)dpy; (void)attrib_list;
  debugPrintf("egl_shim: eglChooseConfig()\n");
  if (configs && config_size > 0)
    configs[0] = (EGLConfig)strdup("config");
  if (num_config)
    *num_config = 1;
  return EGL_TRUE;
}

EGLBoolean egl_shim_GetConfigs(EGLDisplay dpy, EGLConfig *configs,
                               EGLint config_size, EGLint *num_config) {
  (void)dpy;
  if (configs && config_size > 0)
    configs[0] = (EGLConfig)strdup("config");
  if (num_config)
    *num_config = 1;
  return EGL_TRUE;
}

EGLSurface egl_shim_CreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                         EGLNativeWindowType win,
                                         const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)win; (void)attrib_list;
  EGLSurface s = (EGLSurface)strdup("window");
  debugPrintf("egl_shim: eglCreateWindowSurface() -> %p\n", s);
  return s;
}

EGLSurface egl_shim_CreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                          const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)attrib_list;
  EGLSurface s = (EGLSurface)strdup("pbuffer");
  debugPrintf("egl_shim: eglCreatePbufferSurface() -> %p\n", s);
  return s;
}

EGLContext egl_shim_CreateContext(EGLDisplay dpy, EGLConfig config,
                                  EGLContext share_context,
                                  const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)share_context; (void)attrib_list;
  _egl_context *c = (_egl_context *)calloc(1, sizeof(_egl_context));
  if (!c)
    return EGL_NO_CONTEXT;

  pthread_mutex_lock(&egl_context_create_mutex);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  if (egl_share_root)
    guarded_make_current(egl_window, egl_share_root);
  c->sdl_context = guarded_create_context(egl_window);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  guarded_make_current(egl_window, NULL);
  pthread_mutex_unlock(&egl_context_create_mutex);

  if (!c->sdl_context) {
    debugPrintf("egl_shim: eglCreateContext(share=%p) FAILED: %s\n",
                share_context, SDL_GetError());
    free(c);
    return EGL_NO_CONTEXT;
  }

  c->id = next_context_id++;
  debugPrintf("egl_shim: eglCreateContext(share=%p) -> %p [ctx_id=%d]\n",
              share_context, c, c->id);
  return (EGLContext)c;
}

EGLBoolean egl_shim_MakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                 EGLSurface read, EGLContext ctx) {
  (void)dpy; (void)read;

  _egl_context *context = (_egl_context *)ctx;
  static _Thread_local int mc_count = 0;
  int mc = ++mc_count;

  /* === UNBIND === */
  if (context == NULL || draw == NULL) {
    current_context = NULL;
    if (egl_window) {
      guarded_make_current(egl_window, NULL);
      /* debugPrintf("egl_shim: GL released [tid=%lx] reason=eglMakeCurrent(NULL)\n",
                    (unsigned long)pthread_self()); */
    }
    has_real_gl = 0;
    return EGL_TRUE;
  }

  int is_window = (((char *)draw)[0] == 'w');
  context->is_pbuffer = is_window ? EGL_FALSE : EGL_TRUE;
  current_context = context;
  last_context = context;

  if (!egl_window || !context->sdl_context)
    return EGL_TRUE;

  int ret = guarded_make_current(egl_window, context->sdl_context);
  if (ret == 0) {
    has_real_gl = 1;
    static _Thread_local int acq_log = 0;
    if (acq_log < 20 || mc % 500 == 0) {
      //debugPrintf("egl_shim: MakeCurrent #%d %s [tid=%lx] ACQUIRED [ctx_id=%d]\n",
      //            mc, is_window ? "WINDOW" : "PBUFFER",
      //            (unsigned long)pthread_self(), context->id);
      acq_log++;
    }
  } else {
    has_real_gl = 0;
    debugPrintf("egl_shim: MakeCurrent #%d %s [tid=%lx] SDL FAILED [ctx_id=%d]: %s\n",
                mc, is_window ? "WINDOW" : "PBUFFER",
                (unsigned long)pthread_self(), context->id, SDL_GetError());
  }

  return EGL_TRUE;
}

EGLBoolean egl_shim_SwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy; (void)surface;
  if (!egl_window) return EGL_TRUE;

  if (has_real_gl && current_context && !current_context->is_pbuffer) {
    guarded_swap_window(egl_window);
    int fc = ++frame_count;
    if (fc <= 10 || fc % 60 == 0) {
      debugPrintf("egl_shim: SwapBuffers #%d [tid=%lx]\n",
                  fc, (unsigned long)pthread_self());
    }
  } else {
    static int noswap_log = 0;
    if (noswap_log < 3) {
      debugPrintf("egl_shim: SwapBuffers SKIPPED (no real GL) [tid=%lx]\n",
                  (unsigned long)pthread_self());
      noswap_log++;
    }
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_DestroySurface(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy;
  free(surface);
  return EGL_TRUE;
}

EGLBoolean egl_shim_DestroyContext(EGLDisplay dpy, EGLContext ctx) {
  (void)dpy;
  _egl_context *context = (_egl_context *)ctx;
  if (context) {
    if (context->sdl_context)
      guarded_delete_context(context->sdl_context);
    free(context);
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_QuerySurface(EGLDisplay dpy, EGLSurface surface,
                                  EGLint attribute, EGLint *value) {
  (void)dpy; (void)surface;
  if (attribute == 0x3057 && value) *value = SCREEN_WIDTH;
  else if (attribute == 0x3056 && value) *value = SCREEN_HEIGHT;
  return EGL_TRUE;
}

EGLBoolean egl_shim_GetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                                     EGLint attribute, EGLint *value) {
  (void)dpy; (void)config;
  if (!value) return EGL_TRUE;
  switch (attribute) {
  case 0x3020: *value = 32; break;      /* EGL_BUFFER_SIZE */
  case 0x3021: *value = 8; break;       /* EGL_ALPHA_SIZE */
  case 0x3022: *value = 8; break;       /* EGL_BLUE_SIZE */
  case 0x3023: *value = 8; break;       /* EGL_GREEN_SIZE */
  case 0x3024: *value = 8; break;       /* EGL_RED_SIZE */
  case 0x3025: *value = 24; break;      /* EGL_DEPTH_SIZE */
  case 0x3026: *value = 0; break;       /* EGL_STENCIL_SIZE */
  case 0x3027: *value = 0x3038; break;  /* EGL_CONFIG_CAVEAT = DONT_CARE */
  case 0x3028: *value = 1; break;       /* EGL_CONFIG_ID */
  case 0x3029: *value = 0; break;       /* EGL_LEVEL */
  case 0x302d: *value = 1; break;       /* EGL_NATIVE_RENDERABLE */
  case 0x302e: *value = 1; break;       /* EGL_NATIVE_VISUAL_ID: RGBA_8888 */
  case 0x3031: *value = 0; break;       /* EGL_SAMPLES */
  case 0x3032: *value = 0; break;       /* EGL_SAMPLE_BUFFERS */
  case 0x3033: *value = 0x0005; break;  /* EGL_WINDOW_BIT | EGL_PBUFFER_BIT */
  case 0x3040: *value = 0x0004; break;  /* EGL_OPENGL_ES2_BIT */
  case 0x3042: *value = 0x0004; break;  /* EGL_CONFORMANT */
  default: *value = 0; break;
  }
  return EGL_TRUE;
}

EGLint egl_shim_GetError(void) { return EGL_SUCCESS; }

static void *egl_shim_proc_override(const char *procname) {
  if (strcmp(procname, "glGetString") == 0 ||
      strcmp(procname, "glGetStringOES") == 0 ||
      strcmp(procname, "glGetStringEXT") == 0)
    return glGetString_wrap;
  if (strcmp(procname, "glCompressedTexImage2D") == 0 ||
      strcmp(procname, "glCompressedTexImage2DOES") == 0 ||
      strcmp(procname, "glCompressedTexImage2DEXT") == 0)
    return glCompressedTexImage2D_wrap;
  if (strcmp(procname, "glTexImage2D") == 0 ||
      strcmp(procname, "glTexImage2DOES") == 0 ||
      strcmp(procname, "glTexImage2DEXT") == 0)
    return glTexImage2D_wrap;
  if (strcmp(procname, "glTexParameteri") == 0 ||
      strcmp(procname, "glTexParameteriOES") == 0 ||
      strcmp(procname, "glTexParameteriEXT") == 0)
    return glTexParameteri_wrap;
  if (strcmp(procname, "glTexSubImage2D") == 0 ||
      strcmp(procname, "glTexSubImage2DOES") == 0 ||
      strcmp(procname, "glTexSubImage2DEXT") == 0)
    return glTexSubImage2D_wrap;
  if (strcmp(procname, "glPixelStorei") == 0 ||
      strcmp(procname, "glPixelStoreiOES") == 0 ||
      strcmp(procname, "glPixelStoreiEXT") == 0)
    return glPixelStorei_wrap;
  if (strcmp(procname, "glCopyTexImage2D") == 0)
    return glCopyTexImage2D_wrap;
  if (strcmp(procname, "glCopyTexSubImage2D") == 0)
    return glCopyTexSubImage2D_wrap;
  return NULL;
}

void *egl_shim_GetProcAddress(const char *procname) {
  if (!procname)
    return NULL;

  void *ptr = egl_shim_proc_override(procname);
  if (ptr)
    return ptr;

  ptr = SDL_GL_GetProcAddress(procname);
  if (ptr) return ptr;

  size_t len = strlen(procname);
  if (len > 3 && strcmp(procname + len - 3, "OES") == 0) {
    char stripped[256];
    if (len - 3 < sizeof(stripped)) {
      memcpy(stripped, procname, len - 3);
      stripped[len - 3] = '\0';
      ptr = egl_shim_proc_override(stripped);
      if (ptr) return ptr;
      ptr = SDL_GL_GetProcAddress(stripped);
      if (ptr) return ptr;
    }
  }

  debugPrintf("egl_shim: eglGetProcAddress(%s) -> NOT FOUND\n", procname);
  return NULL;
}

EGLBoolean egl_shim_BindAPI(unsigned int api) {
  (void)api;
  return EGL_TRUE;
}
