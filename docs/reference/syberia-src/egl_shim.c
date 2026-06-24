/*
 * egl_shim.c -- EGL wrapper backed by SDL2
 *
 * Intercepts EGL calls from libsyberia1.so and uses SDL2 to create
 * the actual OpenGL ES context with a window.
 */

#include <SDL2/SDL.h>
#include <string.h>

#include "egl_shim.h"
#include "util.h"

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720

static SDL_Window *egl_window = NULL;
static SDL_GLContext egl_gl_context = NULL;
static int egl_initialized = 0;

// Fake handles
static int fake_display = 1;
static int fake_surface = 1;
static int fake_context = 1;
static int fake_config = 1;

EGLDisplay egl_shim_GetDisplay(EGLNativeDisplayType display_id) {
  (void)display_id;
  debugPrintf("egl_shim: eglGetDisplay\n");
  return (EGLDisplay)&fake_display;
}

EGLBoolean egl_shim_Initialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  (void)dpy;
  debugPrintf("egl_shim: eglInitialize\n");

  if (!egl_initialized) {
    // Set OpenGL ES 1.x attributes (Syberia uses fixed-pipeline GL ES 1.1)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    egl_window = SDL_CreateWindow(
        "Syberia", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH, SCREEN_HEIGHT,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);

    if (!egl_window) {
      debugPrintf("egl_shim: SDL_CreateWindow failed: %s\n", SDL_GetError());
      return EGL_FALSE;
    }
    debugPrintf("egl_shim: Window created %dx%d\n", SCREEN_WIDTH,
                SCREEN_HEIGHT);

    egl_gl_context = SDL_GL_CreateContext(egl_window);
    if (!egl_gl_context) {
      debugPrintf("egl_shim: SDL_GL_CreateContext failed: %s\n",
                  SDL_GetError());
      SDL_DestroyWindow(egl_window);
      egl_window = NULL;
      return EGL_FALSE;
    }
    debugPrintf("egl_shim: GL context created\n");

    SDL_GL_MakeCurrent(egl_window, egl_gl_context);
    SDL_GL_SetSwapInterval(1); // VSync on

    egl_initialized = 1;
  }

  if (major)
    *major = 1;
  if (minor)
    *minor = 4;
  return EGL_TRUE;
}

EGLBoolean egl_shim_Terminate(EGLDisplay dpy) {
  (void)dpy;
  debugPrintf("egl_shim: eglTerminate\n");
  if (egl_gl_context) {
    SDL_GL_DeleteContext(egl_gl_context);
    egl_gl_context = NULL;
  }
  if (egl_window) {
    SDL_DestroyWindow(egl_window);
    egl_window = NULL;
  }
  egl_initialized = 0;
  return EGL_TRUE;
}

EGLBoolean egl_shim_ChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                                  EGLConfig *configs, EGLint config_size,
                                  EGLint *num_config) {
  (void)dpy;
  (void)attrib_list;
  debugPrintf("egl_shim: eglChooseConfig\n");
  if (configs && config_size > 0)
    configs[0] = (EGLConfig)&fake_config;
  if (num_config)
    *num_config = 1;
  return EGL_TRUE;
}

EGLSurface egl_shim_CreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                         EGLNativeWindowType win,
                                         const EGLint *attrib_list) {
  (void)dpy;
  (void)config;
  (void)win;
  (void)attrib_list;
  debugPrintf("egl_shim: eglCreateWindowSurface\n");
  return (EGLSurface)&fake_surface;
}

EGLContext egl_shim_CreateContext(EGLDisplay dpy, EGLConfig config,
                                  EGLContext share_context,
                                  const EGLint *attrib_list) {
  (void)dpy;
  (void)config;
  (void)share_context;
  (void)attrib_list;
  debugPrintf("egl_shim: eglCreateContext\n");
  return (EGLContext)&fake_context;
}

EGLBoolean egl_shim_MakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                 EGLSurface read, EGLContext ctx) {
  (void)dpy;
  (void)draw;
  (void)read;
  (void)ctx;
  if (egl_window && egl_gl_context) {
    SDL_GL_MakeCurrent(egl_window, egl_gl_context);
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_SwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy;
  (void)surface;
  if (egl_window)
    SDL_GL_SwapWindow(egl_window);
  return EGL_TRUE;
}

EGLBoolean egl_shim_DestroySurface(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy;
  (void)surface;
  debugPrintf("egl_shim: eglDestroySurface\n");
  return EGL_TRUE;
}

EGLBoolean egl_shim_DestroyContext(EGLDisplay dpy, EGLContext ctx) {
  (void)dpy;
  (void)ctx;
  debugPrintf("egl_shim: eglDestroyContext\n");
  return EGL_TRUE;
}

EGLBoolean egl_shim_QuerySurface(EGLDisplay dpy, EGLSurface surface,
                                  EGLint attribute, EGLint *value) {
  (void)dpy;
  (void)surface;
  // EGL_WIDTH = 0x3057, EGL_HEIGHT = 0x3056
  if (attribute == 0x3057 && value)
    *value = SCREEN_WIDTH;
  else if (attribute == 0x3056 && value)
    *value = SCREEN_HEIGHT;
  return EGL_TRUE;
}

EGLint egl_shim_GetError(void) { return EGL_SUCCESS; }

void *egl_shim_GetProcAddress(const char *procname) {
  debugPrintf("egl_shim: eglGetProcAddress(%s)\n", procname);

  // Try exact name first
  void *ptr = SDL_GL_GetProcAddress(procname);
  if (ptr)
    return ptr;

  // If name ends with "OES", try without the suffix.
  // Many GL ES 1.1 OES extensions are core in the desktop/driver GL,
  // so the driver exports e.g. glBindFramebuffer instead of glBindFramebufferOES.
  size_t len = strlen(procname);
  if (len > 3 && strcmp(procname + len - 3, "OES") == 0) {
    char stripped[256];
    if (len - 3 < sizeof(stripped)) {
      memcpy(stripped, procname, len - 3);
      stripped[len - 3] = '\0';
      ptr = SDL_GL_GetProcAddress(stripped);
      if (ptr) {
        debugPrintf("egl_shim:   -> resolved as %s\n", stripped);
        return ptr;
      }
    }
  }

  debugPrintf("egl_shim:   -> NOT FOUND\n");
  return NULL;
}
