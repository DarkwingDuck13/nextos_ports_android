#ifndef PORT_WINDOW_TITLE
#define PORT_WINDOW_TITLE "nextos_port"
#endif
/*
 * egl_shim.c -- EGL wrapper backed by SDL2 (OpenGL ES 2.0)
 *
 * Each fake EGL context gets a real SDL GL context. We keep a bootstrap
 * context around as the share root so all contexts can share resources.
 */

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "egl_shim.h"
#include "util.h"

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720

/* --- libEGL REAL (Valhall) p/ as queries de config do gles-api-check do Cuphead ---
 * O shim fakeia a maior parte do EGL (a janela/contexto vêm do SDL3/kmsdrm), mas o
 * Cuphead ENUMERA configs (eglChooseConfig + eglGetConfigAttrib) p/ validar GLES3.
 * Dados falsos -> ele não acha config boa, retenta e ABORTA (SIGTRAP). Solução:
 * pegar o EGLDisplay REAL que o SDL já inicializou (eglGetCurrentDisplay após o
 * contexto current) e delegar Choose/GetConfigAttrib pro libEGL real -> Unity vê
 * uma config ES3 de verdade. */
static void *(*r_eglGetCurrentDisplay)(void);
static unsigned (*r_eglChooseConfig)(void *, const int *, void **, int, int *);
static unsigned (*r_eglGetConfigAttrib)(void *, void *, int, int *);
static void *(*r_eglGetCurrentSurface)(int);
static void *(*r_eglCreatePbufferSurface)(void *, void *, const int *);
static unsigned (*r_eglMakeCurrent)(void *, void *, void *, void *);
static void *(*r_eglCreateContext)(void *, void *, void *, const int *);
/* dono da WINDOW é por-THREAD (a thread de render do Unity); as demais threads usam
 * um contexto real próprio (compartilhado com o share-root) num pbuffer. */
static pthread_t g_owner_thread;
static int g_have_owner = 0;
static pthread_mutex_t g_owner_mtx2 = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local void *tls_real_ctx = NULL;   /* contexto real próprio da thread (pbuffer) */
static _Thread_local int tls_is_window = 0;       /* esta thread renderiza na window? */
static void *g_real_dpy = NULL;   /* EGLDisplay real do SDL */
static void *g_real_cfg = NULL;   /* EGLConfig real ES3 */
static void *g_win_surf = NULL;   /* EGLSurface REAL da window do SDL */
static void *g_pbuf = NULL;       /* pbuffer REAL p/ contextos worker (uploads) */
/* Dono da surface da window: o 1º contexto que pede MakeCurrent(window) fica com a
 * window; os demais (worker do Unity) recebem o pbuffer -> sem EGL_BAD_ACCESS
 * (2 contextos NÃO podem compartilhar a mesma surface). Compartilham recursos, então
 * o upload do worker vale pro render. */
static SDL_GLContext g_window_owner = NULL;

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
/* Versão de contexto GL ES que de fato pegou (compartilhada entre o root e os
 * contextos do jogo). kmsdrm/Valhall: tenta ES3 (Cuphead exige GLES3); cai p/ ES2.
 * Env CUP_GLES_MAJOR força (2 ou 3). */
static int g_es_major = 0;
static int g_es_minor = 0;

static _Thread_local _egl_context *current_context = NULL;
static _Thread_local _egl_context *last_context = NULL;
static _Thread_local int has_real_gl = 0;

SDL_Window *egl_shim_get_window(void) { return egl_window; }

static void egl_set_ctx_attrs(int major, int minor) {
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

void egl_shim_create_window(void) {
  /* Cuphead (build Android) exige GLES3; o Valhall do X5M oferece. Tenta ES3.0,
     cai p/ ES2.0 (Mali-450 etc). CUP_GLES_MAJOR força a versão. */
  int want_major = 3, want_minor = 0;
  const char *envm = getenv("CUP_GLES_MAJOR");
  if (envm) { want_major = envm[0] - '0'; if (want_major < 2) want_major = 2; want_minor = 0; }

  egl_set_ctx_attrs(want_major, want_minor);
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
  if (!egl_share_root && want_major >= 3) {
    debugPrintf("egl_shim: ES%d.%d ctx FAILED (%s); fallback ES2.0\n",
                want_major, want_minor, SDL_GetError());
    want_major = 2; want_minor = 0;
    egl_set_ctx_attrs(want_major, want_minor);
    egl_share_root = SDL_GL_CreateContext(egl_window);
  }
  if (!egl_share_root) {
    debugPrintf("egl_shim: SDL_GL_CreateContext FAILED: %s\n", SDL_GetError());
    return;
  }
  g_es_major = want_major; g_es_minor = want_minor;
  debugPrintf("egl_shim: GL share-root context created (ES%d.%d)\n", g_es_major, g_es_minor);

  /* captura o EGLDisplay REAL do SDL (contexto está current agora) + escolhe uma
     EGLConfig real ES3 p/ servir às queries do gles-api-check do Cuphead. */
  r_eglGetCurrentDisplay = (void *(*)(void))dlsym(RTLD_DEFAULT, "eglGetCurrentDisplay");
  r_eglChooseConfig = (unsigned (*)(void *, const int *, void **, int, int *))dlsym(RTLD_DEFAULT, "eglChooseConfig");
  r_eglGetConfigAttrib = (unsigned (*)(void *, void *, int, int *))dlsym(RTLD_DEFAULT, "eglGetConfigAttrib");
  r_eglGetCurrentSurface = (void *(*)(int))dlsym(RTLD_DEFAULT, "eglGetCurrentSurface");
  r_eglCreatePbufferSurface = (void *(*)(void *, void *, const int *))dlsym(RTLD_DEFAULT, "eglCreatePbufferSurface");
  r_eglMakeCurrent = (unsigned (*)(void *, void *, void *, void *))dlsym(RTLD_DEFAULT, "eglMakeCurrent");
  r_eglCreateContext = (void *(*)(void *, void *, void *, const int *))dlsym(RTLD_DEFAULT, "eglCreateContext");
  if (r_eglGetCurrentDisplay) g_real_dpy = r_eglGetCurrentDisplay();
  if (r_eglGetCurrentSurface) g_win_surf = r_eglGetCurrentSurface(0x3059 /*EGL_DRAW*/);
  if (g_real_dpy && r_eglChooseConfig) {
    /* EGL_RENDERABLE_TYPE=ES3 | SURFACE=WINDOW | RGB888 | D24 S8 */
    static const int attrs[] = {
      0x3040, 0x40,   /* EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT */
      0x3033, 0x04,   /* EGL_SURFACE_TYPE,    EGL_WINDOW_BIT */
      0x3024, 8,      /* EGL_RED_SIZE   */
      0x3023, 8,      /* EGL_GREEN_SIZE */
      0x3022, 8,      /* EGL_BLUE_SIZE  */
      0x3025, 16,     /* EGL_DEPTH_SIZE (>=16) */
      0x3026, 8,      /* EGL_STENCIL_SIZE */
      0x3038          /* EGL_NONE */
    };
    void *cfgs[8]; int n = 0;
    if (r_eglChooseConfig(g_real_dpy, attrs, cfgs, 8, &n) && n > 0) {
      g_real_cfg = cfgs[0];
      debugPrintf("egl_shim: EGLConfig real ES3 capturada (dpy=%p cfg=%p n=%d win_surf=%p)\n",
                  g_real_dpy, g_real_cfg, n, g_win_surf);
      /* pbuffer real p/ os contextos worker do Unity (uploads compartilhados) */
      if (r_eglCreatePbufferSurface) {
        static const int pb[] = { 0x3057, 16, 0x3056, 16, 0x3038 }; /* WIDTH,HEIGHT,NONE */
        g_pbuf = r_eglCreatePbufferSurface(g_real_dpy, g_real_cfg, pb);
        debugPrintf("egl_shim: pbuffer worker real = %p\n", g_pbuf);
      }
    } else {
      debugPrintf("egl_shim: eglChooseConfig real falhou (n=%d) — usa attribs hardcoded\n", n);
    }
  } else {
    debugPrintf("egl_shim: sem EGLDisplay real (dpy=%p) — usa attribs hardcoded\n", g_real_dpy);
  }

  SDL_GL_MakeCurrent(egl_window, NULL);
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

  int ret;
  if (g_real_dpy && r_eglMakeCurrent && g_win_surf) {
    int is_main = (syscall(SYS_gettid) == getpid()) && !getenv("CUP_MAINGL");
    int am_owner = !is_main && g_have_owner && pthread_equal(g_owner_thread, pthread_self());
    void *use_ctx = (void *)ctx->sdl_context, *surf;
    if (am_owner) { surf = g_win_surf; tls_is_window = 1; }
    else {
      if (!tls_real_ctx && r_eglCreateContext) {
        int ca[] = { 0x3098, g_es_major, 0x3038 };
        tls_real_ctx = r_eglCreateContext(g_real_dpy, g_real_cfg, egl_share_root, ca);
      }
      if (tls_real_ctx) use_ctx = tls_real_ctx;
      surf = g_pbuf ? g_pbuf : g_win_surf; tls_is_window = 0;
    }
    ret = r_eglMakeCurrent(g_real_dpy, surf, surf, use_ctx) ? 0 : -1;
  } else {
    ret = SDL_GL_MakeCurrent(egl_window, ctx->sdl_context);
  }
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
  if (configs && config_size > 0)
    configs[0] = g_real_cfg ? (EGLConfig)g_real_cfg : (EGLConfig)strdup("config");
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
  /* mesma versão ES que o root (ES3 no Valhall, ES2 fallback) */
  if (g_es_major) egl_set_ctx_attrs(g_es_major, g_es_minor);
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
    if (g_real_dpy && r_eglMakeCurrent && g_win_surf)
      r_eglMakeCurrent(g_real_dpy, NULL, NULL, NULL);  /* release real */
    else if (egl_window)
      SDL_GL_MakeCurrent(egl_window, NULL);
    has_real_gl = 0;
    return EGL_TRUE;
  }

  int is_window = (((char *)draw)[0] == 'w');
  context->is_pbuffer = is_window ? EGL_FALSE : EGL_TRUE;
  current_context = context;
  last_context = context;

  if (!egl_window || !context->sdl_context)
    return EGL_TRUE;

  /* Caminho EGL REAL (multi-contexto do Unity): o 1º contexto a pedir a WINDOW vira
     dono e usa a surface real da window (renderiza/swap); os outros recebem o pbuffer
     (worker de upload, recursos compartilhados) -> elimina o EGL_BAD_ACCESS de 2
     contextos na mesma surface. Usado quando temos display+surfaces reais. */
  if (g_real_dpy && r_eglMakeCurrent && g_win_surf) {
    /* dono da window = a RENDER THREAD do Unity (GfxDeviceWorker), que faz o GL+present.
       A main thread (tid==pid) só submete comandos → recebe pbuffer (contexto próprio,
       recursos compartilhados). Sem isto a main pegava a window e o worker o pbuffer →
       worker não apresentava → livelock no nativeRender. CUP_MAINGL força a main na window. */
    int is_main = (syscall(SYS_gettid) == getpid()) && !getenv("CUP_MAINGL");
    pthread_mutex_lock(&g_owner_mtx2);
    if (is_window && !is_main && !g_have_owner) { g_have_owner = 1; g_owner_thread = pthread_self(); }
    int am_owner = !is_main && g_have_owner && pthread_equal(g_owner_thread, pthread_self());
    pthread_mutex_unlock(&g_owner_mtx2);

    void *use_ctx, *surf;
    if (am_owner) {
      use_ctx = (void *)context->sdl_context;   /* contexto real do jogo na window */
      surf = g_win_surf;
      tls_is_window = 1;
    } else {
      /* contexto real PRÓPRIO desta thread (compartilha recursos c/ o share-root) */
      if (!tls_real_ctx && r_eglCreateContext) {
        int ca[] = { 0x3098 /*EGL_CONTEXT_CLIENT_VERSION*/, g_es_major, 0x3038 };
        tls_real_ctx = r_eglCreateContext(g_real_dpy, g_real_cfg, egl_share_root, ca);
      }
      use_ctx = tls_real_ctx ? tls_real_ctx : (void *)context->sdl_context;
      surf = g_pbuf ? g_pbuf : g_win_surf;
      tls_is_window = 0;
    }
    unsigned ok = r_eglMakeCurrent(g_real_dpy, surf, surf, use_ctx);
    if (ok) {
      has_real_gl = 1;
      static _Thread_local int acq_log = 0;
      if (acq_log < 6) {
        debugPrintf("egl_shim: MakeCurrent #%d [tid=%lx] REAL OK [ctx_id=%d] %s\n",
                    mc, (unsigned long)pthread_self(), context->id,
                    am_owner ? "WIN(owner)" : "PBUF(worker)");
        acq_log++;
      }
      return EGL_TRUE;
    }
    has_real_gl = 0;
    debugPrintf("egl_shim: MakeCurrent #%d [tid=%lx] REAL FAILED [ctx_id=%d] %s\n",
                mc, (unsigned long)pthread_self(), context->id,
                am_owner ? "WIN" : "PBUF");
    return EGL_TRUE;
  }

  int ret = SDL_GL_MakeCurrent(egl_window, context->sdl_context);
  if (ret == 0) {
    has_real_gl = 1;
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

  if (has_real_gl && (tls_is_window || (current_context && !current_context->is_pbuffer))) {
    { extern void ter_shot_hook(void); ter_shot_hook(); }  /* captura na thread DONA da window (antes do swap) */
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
  /* delega pro EGL real (config ES3 verdadeira) — o gles-api-check do Cuphead lê
     visual-type/samples/renderable-type reais e ACEITA a config. */
  if (g_real_dpy && g_real_cfg && r_eglGetConfigAttrib &&
      r_eglGetConfigAttrib(g_real_dpy, g_real_cfg, attribute, value))
    return EGL_TRUE;
  switch (attribute) {
  case 0x3020: *value = 8; break;   /* EGL_BUFFER_SIZE? (RED) */
  case 0x3021: *value = 8; break;   /* EGL_GREEN_SIZE */
  case 0x3022: *value = 8; break;   /* EGL_BLUE_SIZE */
  case 0x3023: *value = 0; break;   /* EGL_ALPHA_SIZE */
  case 0x3025: *value = 24; break;  /* EGL_DEPTH_SIZE */
  case 0x3026: *value = 8; break;   /* EGL_STENCIL_SIZE */
  case 0x3024: *value = 24; break;  /* EGL_BUFFER_SIZE (RGB) */
  case 0x3027: *value = 0x3038; break; /* EGL_CONFIG_CAVEAT = EGL_NONE */
  case 0x3028: *value = 1; break;   /* EGL_CONFIG_ID */
  case 0x3033: *value = 0x0005; break; /* EGL_SURFACE_TYPE = WINDOW|PBUFFER */
  /* EGL_RENDERABLE_TYPE: anuncia ES2|ES3 — o gles-api-check do Cuphead exige o
     bit ES3 (0x40); sem isto o jogo conclui "sem GLES3", retenta e ABORTA (SIGTRAP). */
  case 0x3040: *value = 0x0044; break; /* EGL_OPENGL_ES2_BIT(4)|EGL_OPENGL_ES3_BIT(0x40) */
  case 0x3042: *value = 0x0044; break; /* EGL_CONFORMANT = idem */
  case 0x3039: *value = 0x308E; break; /* EGL_COLOR_BUFFER_TYPE = EGL_RGB_BUFFER */
  case 0x3032: *value = 0; break;   /* EGL_NATIVE_RENDERABLE */
  default:
    debugPrintf("egl_shim: GetConfigAttrib(0x%x) -> 0 (default)\n", attribute);
    *value = 0; break;
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
