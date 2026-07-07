/* egl.c -- wrapper-owned EGL/GLES1 over SDL2 (Linux/Mali-450)
 *
 * LEGO Harry Potter (libLEGOHarry.so) is a classic GLSurfaceView client using
 * OpenGL ES 1.x (fixed-function). The wrapper owns the display/window/context
 * via SDL2 (GLES1 profile) and drives nativeInit/nativeRender on the render
 * thread. GL entry points bind straight to the system libGLESv1_CM (no shader
 * hooks), so this file only provides the EGL surface hooks, the swap, and the
 * single-context handover stubs the pthread bridge references.
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
#include <GLES/gl.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GLES/glext.h>

#include "../config.h"
#include "../util.h"

static SDL_Window   *g_window = NULL;
static SDL_GLContext g_ctx = NULL;

volatile int egl_swap_count = 0;
volatile unsigned long long egl_last_compile_tick = 0;

static pthread_t g_render_thread;
static volatile int g_render_thread_set = 0;

void egl_mark_render_thread(void) { g_render_thread = pthread_self(); g_render_thread_set = 1; }
int  egl_gl_thread_holds_context(void) {
  return g_render_thread_set && pthread_equal(pthread_self(), g_render_thread);
}
void egl_gl_ownership_park(void) {}
void egl_gl_ownership_release(void) {}
void egl_gl_service_handover(void) {}
void egl_gl_acquire(void) {}

int egl_bringup(void) {
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  g_window = SDL_CreateWindow("LEGO Harry Potter",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              screen_width, screen_height,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
  if (!g_window) { debugPrintf("EGL: SDL_CreateWindow failed: %s\n", SDL_GetError()); return -1; }
  g_ctx = SDL_GL_CreateContext(g_window);
  if (!g_ctx) { debugPrintf("EGL: SDL_GL_CreateContext failed: %s\n", SDL_GetError()); return -1; }
  SDL_GL_MakeCurrent(g_window, g_ctx);
  egl_mark_render_thread();
  if (SDL_GL_SetSwapInterval(1) != 0)
    SDL_GL_SetSwapInterval(0);
  debugPrintf("EGL: window+context up (%dx%d) via SDL/GLES1\n", screen_width, screen_height);
  return 0;
}

// on-screen cursor (right-stick pointer): drawn last, over the frame
static float g_cursor_x = -1.f, g_cursor_y = -1.f;
static int g_cursor_show = 0;
void egl_set_cursor(float x, float y, int show) { g_cursor_x = x; g_cursor_y = y; g_cursor_show = show; }

static void egl_draw_cursor(void) {
  GLboolean tex = glIsEnabled(GL_TEXTURE_2D), blend = glIsEnabled(GL_BLEND);
  GLboolean depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE);
  GLboolean varr = glIsEnabled(GL_VERTEX_ARRAY), carr = glIsEnabled(GL_COLOR_ARRAY);
  GLboolean tarr = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
  GLint vp[4];
  glGetIntegerv(GL_VIEWPORT, vp);

  glViewport(0, 0, screen_width, screen_height);
  glDisable(GL_TEXTURE_2D); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  if (carr) glDisableClientState(GL_COLOR_ARRAY);
  if (tarr) glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  if (!varr) glEnableClientState(GL_VERTEX_ARRAY);

  glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
  glOrthof(0.f, (float)screen_width, (float)screen_height, 0.f, -1.f, 1.f);
  glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();

  const float x = g_cursor_x, y = g_cursor_y;
  // black outline cross, then white cross on top (two bars each)
  const float oh[8] = { x-14,y-4, x+14,y-4, x-14,y+4, x+14,y+4 };
  const float ov[8] = { x-4,y-14, x+4,y-14, x-4,y+14, x+4,y+14 };
  const float wh[8] = { x-12,y-2, x+12,y-2, x-12,y+2, x+12,y+2 };
  const float wv[8] = { x-2,y-12, x+2,y-12, x-2,y+12, x+2,y+12 };
  glColor4f(0.f, 0.f, 0.f, 1.f);
  glVertexPointer(2, GL_FLOAT, 0, oh); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glVertexPointer(2, GL_FLOAT, 0, ov); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glColor4f(1.f, 1.f, 1.f, 1.f);
  glVertexPointer(2, GL_FLOAT, 0, wh); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glVertexPointer(2, GL_FLOAT, 0, wv); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glColor4f(1.f, 1.f, 1.f, 1.f);

  glMatrixMode(GL_PROJECTION); glPopMatrix();
  glMatrixMode(GL_MODELVIEW); glPopMatrix();
  if (!varr) glDisableClientState(GL_VERTEX_ARRAY);
  if (carr) glEnableClientState(GL_COLOR_ARRAY);
  if (tarr) glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  if (tex) glEnable(GL_TEXTURE_2D);
  if (blend) glEnable(GL_BLEND);
  if (depth) glEnable(GL_DEPTH_TEST);
  if (cull) glEnable(GL_CULL_FACE);
  glViewport(vp[0], vp[1], vp[2], vp[3]);
}

void egl_present(void) {
  {
    GLboolean scis = glIsEnabled(GL_SCISSOR_TEST);
    GLfloat cc[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    if (scis) glDisable(GL_SCISSOR_TEST);
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, 0);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    if (g_cursor_show) egl_draw_cursor();
    if (scis) glEnable(GL_SCISSOR_TEST);
  }
  SDL_GL_SwapWindow(g_window);
  ++egl_swap_count;
}

void egl_resize_surface(int w, int h) { (void)w; (void)h; }
void egl_fbo_frame_summary(unsigned frame) { (void)frame; }
void *egl_display(void) { return g_ctx; }

// ---------------------------------------------------------------------------
// EGL hooks (virtual: one real SDL context)
// ---------------------------------------------------------------------------

typedef unsigned EGLBoolean;
typedef int EGLint;
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
#define EGL_TRUE_ 1

EGLBoolean eglBindAPIHook(unsigned api) { (void)api; return 1; }
EGLContext eglCreateContextHook(EGLDisplay d, EGLConfig c, EGLContext s, const EGLint *a) {
  (void)d; (void)c; (void)s; (void)a; return (EGLContext)0x0CC1F00D;
}
EGLSurface eglCreatePbufferSurfaceHook(EGLDisplay d, EGLConfig c, const EGLint *a) {
  (void)d; (void)c; (void)a; return (EGLSurface)0x50425546u;
}
EGLBoolean eglMakeCurrentHook(EGLDisplay d, EGLSurface dr, EGLSurface re, EGLContext c) {
  (void)d; (void)dr; (void)re; (void)c; return 1;
}
EGLBoolean eglSwapBuffersHook(EGLDisplay d, EGLSurface s) { (void)d; (void)s; egl_present(); return 1; }
EGLBoolean eglSwapIntervalHook(EGLDisplay d, EGLint i) { (void)d; (void)i; return 1; }
EGLBoolean eglGetConfigAttribHook(EGLDisplay d, EGLConfig c, EGLint attr, EGLint *val) {
  (void)d; (void)c; if (val) *val = (attr == 0x3025) ? 0 : 1; return 1;
}

const GLubyte *glGetStringHook(GLenum name) {
  const GLubyte *s = glGetString(name);
  if (s) return s;
  switch (name) {
    case GL_VENDOR:   return (const GLubyte *)"ARM";
    case GL_RENDERER: return (const GLubyte *)"Mali-450 MP";
    case GL_VERSION:  return (const GLubyte *)"OpenGL ES 1.1";
    case GL_EXTENSIONS:
      return (const GLubyte *)"GL_OES_framebuffer_object GL_OES_matrix_palette "
                              "GL_OES_compressed_ETC1_RGB8_texture GL_OES_draw_texture";
    default: return (const GLubyte *)"";
  }
}
