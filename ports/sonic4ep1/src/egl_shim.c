#ifndef PORT_WINDOW_TITLE
#define PORT_WINDOW_TITLE "nextos_port"
#endif
/*
 * egl_shim.c -- EGL wrapper backed by SDL2 (OpenGL ES)
 *
 * Each fake EGL context gets a real SDL GL context. We keep a bootstrap
 * context around as the share root so all contexts can share resources.
 */

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "egl_shim.h"
#include "util.h"

int sonic4ep1_screen_w = 1280;
int sonic4ep1_screen_h = 720;
#define SCREEN_WIDTH sonic4ep1_screen_w
#define SCREEN_HEIGHT sonic4ep1_screen_h

static const char *ep1_env(const char *name) {
  const char *v = getenv(name);
  return (v && *v) ? v : NULL;
}

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
static int clear_test_done = 0;

static _Thread_local _egl_context *current_context = NULL;
static _Thread_local _egl_context *last_context = NULL;
static _Thread_local int has_real_gl = 0;

extern int sonic4ep1_menu_overlay_active;
extern int sonic4ep1_menu_overlay_selection;
extern int sonic4ep1_menu_overlay_screen;

SDL_Window *egl_shim_get_window(void) { return egl_window; }

void egl_shim_create_window(void) {
  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    sonic4ep1_screen_w = dm.w;
    sonic4ep1_screen_h = dm.h;
    debugPrintf("egl_shim: desktop mode %dx%d\n", dm.w, dm.h);
  }
  {
    const char *e = ep1_env("SONIC4EP1_RES");
    int w = 0, h = 0;
    if (e && sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
      sonic4ep1_screen_w = w;
      sonic4ep1_screen_h = h;
      debugPrintf("egl_shim: SONIC4EP1_RES override %dx%d\n", w, h);
    }
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  {
    const char *gles = ep1_env("SONIC4EP1_GLES");
    int major = (gles && gles[0] == '2') ? 2 : 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, major == 1 ? 1 : 0);
    debugPrintf("egl_shim: pedindo contexto ES %d.%d\n", major,
                major == 1 ? 1 : 0);
  }
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  egl_window = SDL_CreateWindow(
      PORT_WINDOW_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      SCREEN_WIDTH, SCREEN_HEIGHT,
      SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
  if (!egl_window) {
    debugPrintf("egl_shim: SDL_CreateWindow FAILED: %s\n", SDL_GetError());
    return;
  }
  debugPrintf("egl_shim: Window created %dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);

  egl_share_root = SDL_GL_CreateContext(egl_window);
  if (!egl_share_root) {
    debugPrintf("egl_shim: SDL_GL_CreateContext FAILED: %s\n", SDL_GetError());
    return;
  }
  debugPrintf("egl_shim: GL share-root context created\n");

  SDL_GL_MakeCurrent(egl_window, NULL);
  debugPrintf("egl_shim: Context released, ready for game\n");
}

void egl_shim_bind_main(void) {
  if (!egl_window || !egl_share_root)
    return;
  if (SDL_GL_MakeCurrent(egl_window, egl_share_root) == 0) {
    static int bind_log_count = 0;
    has_real_gl = 1;
    if (bind_log_count < 5 || bind_log_count % 120 == 0)
      debugPrintf("egl_shim: share-root current na thread principal\n");
    bind_log_count++;
  } else {
    has_real_gl = 0;
    debugPrintf("egl_shim: bind_main FAILED: %s\n", SDL_GetError());
  }
}

static void ep1_maybe_screenshot(void) {
  static int chk = 0;
  if (++chk % 15)
    return;
  if (access("/dev/shm/ep1_shot", F_OK) != 0)
    return;
  unlink("/dev/shm/ep1_shot");

  int w = SCREEN_WIDTH;
  int h = SCREEN_HEIGHT;
  if (w <= 0 || h <= 0)
    return;

  unsigned char *buf = (unsigned char *)malloc((size_t)w * (size_t)h * 4u);
  if (!buf)
    return;

  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
  FILE *raw = fopen("/dev/shm/ep1_shot.raw", "wb");
  if (raw) {
    fwrite(buf, 1, (size_t)w * (size_t)h * 4u, raw);
    fclose(raw);
  }
  FILE *txt = fopen("/dev/shm/ep1_shot.txt", "w");
  if (txt) {
    fprintf(txt, "%d %d\n", w, h);
    fclose(txt);
  }
  free(buf);
  debugPrintf("[shot] ep1 %dx%d salvo\n", w, h);
}

static void ep1_clear_rect(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0)
    return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > SCREEN_WIDTH)
    w = SCREEN_WIDTH - x;
  if (y + h > SCREEN_HEIGHT)
    h = SCREEN_HEIGHT - y;
  if (w <= 0 || h <= 0)
    return;
  glScissor(x, SCREEN_HEIGHT - y - h, w, h);
  glClear(GL_COLOR_BUFFER_BIT);
}

static void ep1_draw_arrow_layer(int cx, int cy, int scale, int grow) {
  int shaft_h = 7 * scale + grow * 2;
  int shaft_w = 16 * scale + grow * 2;
  int shaft_x = cx - 28 * scale - grow;
  int shaft_y = cy - shaft_h / 2;
  ep1_clear_rect(shaft_x, shaft_y, shaft_w, shaft_h);

  int tip_x = cx + 2 * scale;
  int top_y = cy - (24 * scale) / 2 - grow;
  for (int i = 0; i < 12; i++) {
    int ramp = (i < 6 ? i + 1 : 12 - i);
    int half = ramp * scale + grow;
    int y = top_y + i * 2 * scale;
    ep1_clear_rect(tip_x - 18 * scale - grow, y, 18 * scale + half,
                   2 * scale + grow * 2);
  }
}

static void ep1_draw_pretty_arrow(int cx, int cy) {
  int scale = SCREEN_HEIGHT / 300;
  if (scale < 2)
    scale = 2;
  if (scale > 3)
    scale = 3;

  /* Sombra afastada. */
  glClearColor(0.0f, 0.01f, 0.04f, 1.0f);
  ep1_draw_arrow_layer(cx + 4 * scale, cy + 4 * scale, scale, scale);

  /* Contorno escuro e borda branca, estilo cursor arcade/pixel-art. */
  glClearColor(0.02f, 0.02f, 0.04f, 1.0f);
  ep1_draw_arrow_layer(cx, cy, scale, scale);

  glClearColor(0.72f, 0.90f, 1.0f, 1.0f);
  ep1_draw_arrow_layer(cx, cy, scale, scale / 2);

  glClearColor(0.02f, 0.33f, 0.95f, 1.0f);
  ep1_draw_arrow_layer(cx, cy, scale, 0);

  /* Brilho superior para nao parecer bloco chapado. */
  glClearColor(0.25f, 0.74f, 1.0f, 1.0f);
  ep1_clear_rect(cx - 27 * scale, cy - 5 * scale, 14 * scale, 2 * scale);
  for (int i = 0; i < 5; i++) {
    int y = cy - 10 * scale + i * 2 * scale;
    ep1_clear_rect(cx - 16 * scale, y, (13 + i * 2) * scale, scale);
  }
}

static void ep1_draw_menu_overlay(void) {
  const char *overlay = ep1_env("SONIC4EP1_MENU_OVERLAY");
  if ((overlay && strcmp(overlay, "0") == 0) ||
      !sonic4ep1_menu_overlay_active || !has_real_gl)
    return;

  int sel = sonic4ep1_menu_overlay_selection;

  int cx = 0;
  int cy = 0;
  if (sonic4ep1_menu_overlay_screen == 1) {
    if (sel < 0)
      sel = 0;
    if (sel > 6)
      sel = 6;
    switch (sel) {
    case 0:
      cx = (int)((float)SCREEN_WIDTH * 0.07f);
      cy = (int)((float)SCREEN_HEIGHT * 0.35f);
      break;
    case 1:
      cx = (int)((float)SCREEN_WIDTH * 0.49f);
      cy = (int)((float)SCREEN_HEIGHT * 0.35f);
      break;
    case 2:
      cx = (int)((float)SCREEN_WIDTH * 0.07f);
      cy = (int)((float)SCREEN_HEIGHT * 0.65f);
      break;
    case 3:
      cx = (int)((float)SCREEN_WIDTH * 0.49f);
      cy = (int)((float)SCREEN_HEIGHT * 0.65f);
      break;
    case 4:
      cx = (int)((float)SCREEN_WIDTH * 0.07f);
      cy = (int)((float)SCREEN_HEIGHT * 0.85f);
      break;
    case 5:
      cx = (int)((float)SCREEN_WIDTH * 0.07f);
      cy = (int)((float)SCREEN_HEIGHT * 0.94f);
      break;
    default:
      cx = (int)((float)SCREEN_WIDTH * 0.70f);
      cy = (int)((float)SCREEN_HEIGHT * 0.94f);
      break;
    }
  } else if (sonic4ep1_menu_overlay_screen == 2) {
    if (sel < 0)
      sel = 0;
    if (sel > 6)
      sel = 6;
    switch (sel) {
    case 0:
      cx = (int)((float)SCREEN_WIDTH * 0.49f);
      cy = (int)((float)SCREEN_HEIGHT * 0.31f);
      break;
    case 1:
      cx = (int)((float)SCREEN_WIDTH * 0.78f);
      cy = (int)((float)SCREEN_HEIGHT * 0.31f);
      break;
    case 2:
      cx = (int)((float)SCREEN_WIDTH * 0.49f);
      cy = (int)((float)SCREEN_HEIGHT * 0.49f);
      break;
    case 3:
      cx = (int)((float)SCREEN_WIDTH * 0.78f);
      cy = (int)((float)SCREEN_HEIGHT * 0.49f);
      break;
    case 4:
      cx = (int)((float)SCREEN_WIDTH * 0.35f);
      cy = (int)((float)SCREEN_HEIGHT * 0.71f);
      break;
    case 5:
      cx = (int)((float)SCREEN_WIDTH * 0.62f);
      cy = (int)((float)SCREEN_HEIGHT * 0.71f);
      break;
    default:
      cx = (int)((float)SCREEN_WIDTH * 0.70f);
      cy = (int)((float)SCREEN_HEIGHT * 0.94f);
      break;
    }
  } else if (sonic4ep1_menu_overlay_screen == 3) {
    cx = (int)((float)SCREEN_WIDTH * 0.70f);
    cy = (int)((float)SCREEN_HEIGHT * 0.94f);
  } else if (sonic4ep1_menu_overlay_screen == 4) {
    if (sel < 0)
      sel = 0;
    if (sel > 2)
      sel = 2;
    if (sel == 0) {
      cx = (int)((float)SCREEN_WIDTH * 0.14f);
      cy = (int)((float)SCREEN_HEIGHT * 0.37f);
    } else if (sel == 1) {
      cx = (int)((float)SCREEN_WIDTH * 0.50f);
      cy = (int)((float)SCREEN_HEIGHT * 0.37f);
    } else {
      cx = (int)((float)SCREEN_WIDTH * 0.14f);
      cy = (int)((float)SCREEN_HEIGHT * 0.64f);
    }
  } else if (sonic4ep1_menu_overlay_screen == 5) {
    if (sel < 0)
      sel = 0;
    if (sel > 1)
      sel = 1;
    if (sel == 0) {
      cx = (int)((float)SCREEN_WIDTH * 0.31f);
      cy = (int)((float)SCREEN_HEIGHT * 0.62f);
    } else {
      cx = (int)((float)SCREEN_WIDTH * 0.50f);
      cy = (int)((float)SCREEN_HEIGHT * 0.62f);
    }
  } else {
    if (sel < 0)
      sel = 0;
    if (sel > 2)
      sel = 2;

    cx = (int)((float)SCREEN_WIDTH * 0.13f);
    cy = (int)((float)SCREEN_HEIGHT * 0.31f);
    if (sel == 1) {
      cy = (int)((float)SCREEN_HEIGHT * 0.56f);
    } else if (sel == 2) {
      cx = (int)((float)SCREEN_WIDTH * 0.70f);
      cy = (int)((float)SCREEN_HEIGHT * 0.94f);
    }
  }

  GLboolean scissor_was = glIsEnabled(GL_SCISSOR_TEST);
  GLint old_scissor[4] = {0, 0, 0, 0};
  GLfloat old_clear[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_SCISSOR_BOX, old_scissor);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear);

  glEnable(GL_SCISSOR_TEST);
  ep1_draw_pretty_arrow(cx, cy);

  glClearColor(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
  glScissor(old_scissor[0], old_scissor[1], old_scissor[2], old_scissor[3]);
  if (!scissor_was)
    glDisable(GL_SCISSOR_TEST);
}

void egl_shim_swap_main(void) {
  if (!egl_window)
    return;
  if (!clear_test_done && ep1_env("SONIC4EP1_CLEAR_TEST")) {
    clear_test_done = 1;
    egl_shim_bind_main();
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    debugPrintf("egl_shim: clear-test vermelho antes do swap\n");
  }
  if (has_real_gl) {
    ep1_draw_menu_overlay();
    ep1_maybe_screenshot();
  }
  SDL_GL_SwapWindow(egl_window);
  int fc = ++frame_count;
  if (fc <= 10 || fc % 60 == 0)
    debugPrintf("egl_shim: Java glSwapBuffers #%d\n", fc);
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

  int ret = SDL_GL_MakeCurrent(egl_window, ctx->sdl_context);
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
    SDL_GL_DeleteContext(egl_share_root);
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
    SDL_GL_MakeCurrent(egl_window, egl_share_root);
  c->sdl_context = SDL_GL_CreateContext(egl_window);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  SDL_GL_MakeCurrent(egl_window, NULL);
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
      SDL_GL_MakeCurrent(egl_window, NULL);
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

  int ret = SDL_GL_MakeCurrent(egl_window, context->sdl_context);
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
    SDL_GL_SwapWindow(egl_window);
    int fc = ++frame_count;
    if (fc <= 10 || fc % 60 == 0) {
      //debugPrintf("egl_shim: SwapBuffers #%d [tid=%lx]\n",
      //            fc, (unsigned long)pthread_self());
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
      SDL_GL_DeleteContext(context->sdl_context);
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
  case 0x3020: *value = 8; break;
  case 0x3021: *value = 8; break;
  case 0x3022: *value = 8; break;
  case 0x3023: *value = 0; break;
  case 0x3025: *value = 24; break;
  case 0x3026: *value = 8; break;
  default: *value = 0; break;
  }
  return EGL_TRUE;
}

EGLint egl_shim_GetError(void) { return EGL_SUCCESS; }

void *egl_shim_GetProcAddress(const char *procname) {
  void *ptr = SDL_GL_GetProcAddress(procname);
  if (ptr) return ptr;

  size_t len = strlen(procname);
  if (len > 3 && strcmp(procname + len - 3, "OES") == 0) {
    char stripped[256];
    if (len - 3 < sizeof(stripped)) {
      memcpy(stripped, procname, len - 3);
      stripped[len - 3] = '\0';
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

const char *egl_shim_QueryString(EGLDisplay dpy, EGLint name) {
  (void)dpy;
  switch (name) {
  case 0x3053: return "NextOS";      /* EGL_VENDOR */
  case 0x3054: return "1.4 NextOS";  /* EGL_VERSION */
  case 0x3055: return "";            /* EGL_EXTENSIONS */
  case 0x308D: return "OpenGL_ES";   /* EGL_CLIENT_APIS */
  default: return "";
  }
}

EGLBoolean egl_shim_SwapInterval(EGLDisplay dpy, EGLint interval) {
  (void)dpy;
  SDL_GL_SetSwapInterval(interval);
  return EGL_TRUE;
}

EGLContext egl_shim_GetCurrentContext(void) {
  return (EGLContext)current_context;
}

EGLSurface egl_shim_GetCurrentSurface(EGLint readdraw) {
  (void)readdraw;
  return (EGLSurface)"window";
}

EGLBoolean egl_shim_SurfaceAttrib(EGLDisplay dpy, EGLSurface s, EGLint a,
                                  EGLint v) {
  (void)dpy; (void)s; (void)a; (void)v;
  return EGL_TRUE;
}
