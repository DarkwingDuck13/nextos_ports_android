/* egl_shim.c -- SDL2/Mali fbdev EGL bridge for Bully2. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window *g_win;
static SDL_GLContext g_ctx;
static int g_w = 1280;
static int g_h = 720;

int bully_screen_w(void) { return g_w; }
int bully_screen_h(void) { return g_h; }

int bully_init_gl(void) {
  if (g_ctx)
    return 1;

  if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "[sdl] video init failed: %s\n", SDL_GetError());
    return 0;
  }

  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    g_w = dm.w;
    g_h = dm.h;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  g_win = SDL_CreateWindow("Bully2", SDL_WINDOWPOS_UNDEFINED,
                           SDL_WINDOWPOS_UNDEFINED, g_w, g_h,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
  if (!g_win) {
    fprintf(stderr, "[sdl] create window failed: %s\n", SDL_GetError());
    return 0;
  }

  g_ctx = SDL_GL_CreateContext(g_win);
  if (!g_ctx) {
    fprintf(stderr, "[sdl] GL context failed: %s\n", SDL_GetError());
    return 0;
  }

  SDL_GL_MakeCurrent(g_win, g_ctx);
  const GLubyte *renderer = glGetString(GL_RENDERER);
  const GLubyte *version = glGetString(GL_VERSION);
  fprintf(stderr, "[gl] %dx%d driver=%s | EGL d=%p s=%p c=%p | %s / %s\n",
          g_w, g_h, SDL_GetCurrentVideoDriver(),
          (void *)eglGetCurrentDisplay(), (void *)eglGetCurrentSurface(EGL_DRAW),
          (void *)eglGetCurrentContext(), renderer ? (const char *)renderer : "?",
          version ? (const char *)version : "?");
  return 1;
}

void bully_egl_objects(uintptr_t *d, uintptr_t *s, uintptr_t *c) {
  if (d)
    *d = (uintptr_t)eglGetCurrentDisplay();
  if (s)
    *s = (uintptr_t)eglGetCurrentSurface(EGL_DRAW);
  if (c)
    *c = (uintptr_t)eglGetCurrentContext();
}

int bully_make_current(void) {
  return SDL_GL_MakeCurrent(g_win, g_ctx) == 0 ? 1 : 0;
}

void bully_release_current(void) {
  SDL_GL_MakeCurrent(g_win, NULL);
}

void bully_swap_buffers(void) {
  static unsigned (*raw_swap)(void *, void *) = NULL;
  if (!g_win)
    return;
  if (!raw_swap)
    raw_swap = (unsigned (*)(void *, void *))dlsym(RTLD_DEFAULT, "eglSwapBuffers");
  EGLDisplay d = eglGetCurrentDisplay();
  EGLSurface s = eglGetCurrentSurface(EGL_DRAW);
  if (raw_swap && d != EGL_NO_DISPLAY && s != EGL_NO_SURFACE) {
    raw_swap(d, s);
    return;
  }
  SDL_GL_SwapWindow(g_win);
}
