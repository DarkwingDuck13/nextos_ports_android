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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "egl_shim.h"
#include "util.h"

#define DEFAULT_SCREEN_WIDTH 1280
#define DEFAULT_SCREEN_HEIGHT 720

typedef struct {
  SDL_GLContext sdl_context;
  EGLBoolean is_pbuffer;
  int id;
  unsigned long owner_tid; /* thread dona do SDL context (SDL-mali amarra a thread criadora) */
  void *real_ctx;          /* contexto EGL REAL compartilhado (1 por eglCreateContext da Unity) */
} _egl_context;

static SDL_Window *egl_window = NULL;
static SDL_GLContext egl_share_root = NULL;
static pthread_mutex_t egl_context_create_mutex = PTHREAD_MUTEX_INITIALIZER;

/* === ABORDAGEM BULLY: usar os objetos EGL REAIS que o SDL2-mali criou + o eglMakeCurrent REAL
   (via dlsym do libEGL). O wrapper SDL_GL_MakeCurrent amarra o contexto a thread criadora ->
   EGL_BAD_ACCESS quando a Unity renderiza em outra thread. O eglMakeCurrent REAL permite handoff
   cross-thread (basta a outra thread soltar). Capturamos dpy/surf/ctx reais apos o SDL criar. === */
#include <dlfcn.h>
#define EGL_DRAW_ATTR 0x3059
static void *(*r_getCurDisplay)(void);
static void *(*r_getCurSurface)(int);
static void *(*r_getCurContext)(void);
static unsigned (*r_makeCurrent)(void*,void*,void*,void*);
static unsigned (*r_swapBuffers)(void*,void*);
static int (*r_getError)(void);
static void *(*r_createContext)(void*,void*,void*,const int*);
static unsigned (*r_chooseConfig)(void*,const int*,void*,int,int*);
static void *(*r_createPbuffer)(void*,void*,const int*);
static void *g_real_dpy=NULL, *g_real_surf=NULL, *g_real_ctx=NULL, *g_real_cfg=NULL, *g_real_pbuf=NULL;
static int g_use_real_egl=0;
static unsigned long g_creator_tid=0; /* thread que cria os contextos (setup) -> usa PBUFFER;
                                         a thread de RENDER (outra) usa a surface do WINDOW. */
/* DuckTales renderiza NA PROPRIA thread criadora (nao ha render-thread separada),
   entao a criadora precisa usar a WINDOW (senao renderiza num pbuffer e o swap e
   pulado -> frames=0). DUCK_EGL_CREATOR_WIN=1 faz a criadora usar a window. */
static int g_creator_win = 0;
static int frame_count = 0;
int egl_shim_frame_count(void) { return frame_count; }
static int next_context_id = 1;
static int cached_width = 0;
static int cached_height = 0;

static _Thread_local _egl_context *current_context = NULL;
static _Thread_local _egl_context *last_context = NULL;
static _Thread_local int has_real_gl = 0;

static int real_current_is_window_surface(void) {
  if (!g_use_real_egl || !r_getCurSurface || !g_real_surf)
    return 0;
  return r_getCurSurface(EGL_DRAW_ATTR) == g_real_surf;
}

SDL_Window *egl_shim_get_window(void) { return egl_window; }

/* ---- on-GL-thread screenshot (DUCK_SHOT=1) ----
   /dev/fb0 reads return 0 bytes while the Mali fbdev is owned by our GL client,
   so capture with glReadPixels on the rendering thread instead. Every
   DUCK_SHOTEVERY frames (default 30) dump the current framebuffer to
   /tmp/duck_shot.ppm (overwrite). Also computes a non-black % so the run log
   tells us if the background actually rendered without needing the image. */
static int g_shot = -1, g_shot_every = 30;
static void egl_shim_maybe_shot(int w, int h) {
  if (g_shot < 0) {
    g_shot = getenv("DUCK_SHOT") ? 1 : 0;
    const char *e = getenv("DUCK_SHOTEVERY"); if (e) g_shot_every = atoi(e);
    if (g_shot_every < 1) g_shot_every = 30;
  }
  if (!g_shot) return;
  if (frame_count % g_shot_every != 0) return;
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) return;
  static unsigned char *buf = NULL; static int cap = 0;
  int need = w * h * 4;
  if (need > cap) { free(buf); buf = malloc(need); cap = need; }
  if (!buf) return;
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
  /* non-black ratio + flat-color dominance (intro logos are one flat color;
     the real menu has varied content). Bucket colors into 4x4x4 = 64 bins. */
  long nb = 0; int bins[64]; for (int i=0;i<64;i++) bins[i]=0; long binned=0;
  for (int i = 0; i < w * h; i += 7) {
    unsigned char *p = buf + i*4;
    if (p[0]|p[1]|p[2]) nb++;
    int b = (p[0]>>6)*16 + (p[1]>>6)*4 + (p[2]>>6); bins[b]++; binned++;
  }
  /* scale nb (sampled every 7) to full */
  long sampled_total = (w*h + 6)/7;
  int pct = sampled_total ? (int)(nb * 100 / sampled_total) : 0;
  int topbin = 0; for (int i=0;i<64;i++) if (bins[i]>topbin) topbin = bins[i];
  int flatpct = binned ? (int)((long)topbin * 100 / binned) : 100;  /* % in dominant color bin */
  int varied = flatpct < 80;   /* menu content is varied; flat logos are not */
  /* write PPM (flip vertically: GL origin bottom-left). Track the best frame of
     the MENU phase (frame > 600) separately so we can tell if the menu bg
     rendered this run, independent of the fullscreen intro logos. */
  static int best = -1, menubest = -1;
  const char *path = "/tmp/duck_shot.ppm";
  /* a "menu with bg" frame = varied content (not a flat logo) and substantial
     coverage. The intro logos are flat single colors -> excluded by `varied`. */
  int is_menu = frame_count > 600 && varied;
  if (pct > best) { best = pct; path = "/tmp/duck_best.ppm"; }
  if (is_menu && pct > menubest) { menubest = pct; path = "/tmp/duck_menubest.ppm"; }
  FILE *f = fopen(path, "wb");
  if (f) {
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--) {
      unsigned char *row = buf + y * w * 4;
      for (int x = 0; x < w; x++) { fputc(row[x*4], f); fputc(row[x*4+1], f); fputc(row[x*4+2], f); }
    }
    fclose(f);
  }
  debugPrintf("[SHOT] frame %d non-black=%d%% flat=%d%% varied=%d (best=%d menubest=%d)\n",
              frame_count, pct, flatpct, varied, best, menubest);
}

static int read_env_int(const char *name, int fallback, int min_value, int max_value) {
  const char *value = getenv(name);
  char *end = NULL;
  long parsed;
  if (!value || !value[0]) return fallback;
  parsed = strtol(value, &end, 10);
  if (!end || *end) return fallback;
  if (parsed < min_value) parsed = min_value;
  if (parsed > max_value) parsed = max_value;
  return (int)parsed;
}

static int screen_width(void) {
  if (!cached_width) cached_width = read_env_int("RE4_WIDTH", DEFAULT_SCREEN_WIDTH, 320, 1920);
  return cached_width;
}

static int screen_height(void) {
  if (!cached_height) cached_height = read_env_int("RE4_HEIGHT", DEFAULT_SCREEN_HEIGHT, 240, 1080);
  return cached_height;
}

void egl_shim_create_window(void) {
  /* DuckTales renders on the creator thread -> it must use the WINDOW surface
     (default ON). DUCK_NO_EGL_CREATOR_WIN reverts to the pbuffer (RE4-style). */
  if (!getenv("DUCK_NO_EGL_CREATOR_WIN")) g_creator_win = 1;
  /* RESOLUCAO AUTOMATICA/PORTATIL: se RE4_WIDTH/HEIGHT nao foram fixados, usa a resolucao
     NATIVA do display (SDL_GetDesktopDisplayMode) -> 480p/720p/1080p sem hardcode. setenv ANTES
     de qualquer screen_width()/re4_screen_width() -> egl_shim e main_re4 concordam. O fix do
     GL_MAX_TEXTURE_SIZE garante que o render target na resolucao nativa nao encolha (era o cap
     1024 que encolhia o RT da cena sem ajustar o viewport -> ZOOM em 1280+). */
  if (!getenv("RE4_WIDTH") || !getenv("RE4_HEIGHT")) {
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
      char b[16];
      snprintf(b, sizeof b, "%d", dm.w); setenv("RE4_WIDTH", b, 1);
      snprintf(b, sizeof b, "%d", dm.h); setenv("RE4_HEIGHT", b, 1);
      debugPrintf("egl_shim: [AUTO] native display %dx%d -> RE4_WIDTH/HEIGHT\n", dm.w, dm.h);
    }
  }
  int width = screen_width();
  int height = screen_height();
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  egl_window = SDL_CreateWindow(
      PORT_WINDOW_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      width, height,
      SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
  if (!egl_window) {
    debugPrintf("egl_shim: SDL_CreateWindow FAILED: %s\n", SDL_GetError());
    return;
  }
  debugPrintf("egl_shim: Window created %dx%d\n", width, height);

  egl_share_root = SDL_GL_CreateContext(egl_window);
  if (!egl_share_root) {
    debugPrintf("egl_shim: SDL_GL_CreateContext FAILED: %s\n", SDL_GetError());
    return;
  }
  debugPrintf("egl_shim: GL share-root context created\n");

  /* captura os objetos EGL REAIS criados pelo SDL2-mali (contexto current agora=share_root) */
  r_getCurDisplay=dlsym(RTLD_DEFAULT,"eglGetCurrentDisplay");
  r_getCurSurface=dlsym(RTLD_DEFAULT,"eglGetCurrentSurface");
  r_getCurContext=dlsym(RTLD_DEFAULT,"eglGetCurrentContext");
  r_makeCurrent  =dlsym(RTLD_DEFAULT,"eglMakeCurrent");
  r_swapBuffers  =dlsym(RTLD_DEFAULT,"eglSwapBuffers");
  r_getError     =dlsym(RTLD_DEFAULT,"eglGetError");
  r_createContext=dlsym(RTLD_DEFAULT,"eglCreateContext");
  r_chooseConfig =dlsym(RTLD_DEFAULT,"eglChooseConfig");
  unsigned (*r_querySurface)(void*,void*,int,int*)=dlsym(RTLD_DEFAULT,"eglQuerySurface");
  if(r_getCurDisplay&&r_getCurSurface&&r_getCurContext&&r_makeCurrent&&r_createContext&&r_chooseConfig&&r_querySurface){
    g_real_dpy=r_getCurDisplay(); g_real_surf=r_getCurSurface(EGL_DRAW_ATTR); g_real_ctx=r_getCurContext();
    /* DIAG: tamanho REAL da surface mali-fbdev (decisivo p/ o bug de zoom/fullscreen) */
    { int rw=-1,rh=-1; r_querySurface(g_real_dpy,g_real_surf,0x3057/*WIDTH*/,&rw);
      r_querySurface(g_real_dpy,g_real_surf,0x3056/*HEIGHT*/,&rh);
      int dw=0,dh=0; SDL_GL_GetDrawableSize(egl_window,&dw,&dh);
      int mts=0,mrb=0,mvp[2]={0,0};
      void (*r_getiv)(unsigned,int*)=dlsym(RTLD_DEFAULT,"glGetIntegerv");
      int sbits=-1,dbits=-1,rbits=-1,abits=-1;
      if(r_getiv){ r_getiv(0x0D33,&mts); r_getiv(0x84E8,&mrb); r_getiv(0x0D3A,mvp);
        r_getiv(0x0D57/*GL_STENCIL_BITS*/,&sbits); r_getiv(0x0D56/*GL_DEPTH_BITS*/,&dbits);
        r_getiv(0x0D52/*GL_RED_BITS*/,&rbits); r_getiv(0x0D55/*GL_ALPHA_BITS*/,&abits); }
      debugPrintf("egl_shim: [DIAG] REAL surface=%dx%d  SDL drawable=%dx%d  requested=%dx%d  GL_MAX_TEX=%d MAX_RB=%d MAX_VP=%dx%d\n",
                  rw,rh,dw,dh,width,height,mts,mrb,mvp[0],mvp[1]);
      debugPrintf("egl_shim: [DIAG] framebuffer bits: stencil=%d depth=%d red=%d alpha=%d\n", sbits,dbits,rbits,abits); }
    /* config EXATO da surface do SDL (via CONFIG_ID) -> os contextos compartilhados batem com a
       surface (senao eglMakeCurrent = EGL_BAD_MATCH 0x3009). */
    int cfgid=0, n=0; r_querySurface(g_real_dpy, g_real_surf, 0x3028 /*EGL_CONFIG_ID*/, &cfgid);
    int cfgattr[]={0x3028 /*EGL_CONFIG_ID*/, cfgid, 0x3038 /*EGL_NONE*/};
    r_chooseConfig(g_real_dpy, cfgattr, &g_real_cfg, 1, &n);
    debugPrintf("egl_shim: surface CONFIG_ID=%d cfg=%p n=%d\n", cfgid, g_real_cfg, n);
    /* PBUFFER real p/ a thread de setup (pra nao segurar a surface do window) */
    r_createPbuffer=dlsym(RTLD_DEFAULT,"eglCreatePbufferSurface");
    if(r_createPbuffer && g_real_cfg){ int pb[]={0x3057/*WIDTH*/,16, 0x3056/*HEIGHT*/,16, 0x3038};
      g_real_pbuf=r_createPbuffer(g_real_dpy, g_real_cfg, pb);
      debugPrintf("egl_shim: pbuffer=%p (err=0x%x)\n", g_real_pbuf, r_getError?r_getError():0); }
    if(g_real_dpy&&g_real_surf&&g_real_ctx&&g_real_cfg&&n>0){ g_use_real_egl=1;
      debugPrintf("egl_shim: REAL EGL dpy=%p surf=%p ctx=%p cfg=%p (Bully-style, 1 ctx/thread)\n",
                  g_real_dpy,g_real_surf,g_real_ctx,g_real_cfg); }
    /* EGL_BUFFER_PRESERVED: no Mali fbdev double-buffer (page0/page1), o back buffer e DESCARTADO
       no swap -> o composite-draw da Unity num buffer reciclado nao "sobrevive" (tile nao resolve)
       -> tela preta apos o menu. Preservar o back buffer no swap faz o conteudo do composite chegar
       ao scanout. (clear sobrevive, draw nao -> preservar resolve a diferenca.) */
    if(!getenv("RE4_NO_PRESERVE")){
      unsigned (*r_surfAttrib)(void*,void*,int,int)=dlsym(RTLD_DEFAULT,"eglSurfaceAttrib");
      if(r_surfAttrib){
        unsigned ok=r_surfAttrib(g_real_dpy,g_real_surf,0x3093 /*EGL_SWAP_BEHAVIOR*/,0x3094 /*EGL_BUFFER_PRESERVED*/);
        debugPrintf("egl_shim: eglSurfaceAttrib(SWAP_BEHAVIOR=PRESERVED) -> %u err=0x%x\n",ok,r_getError?r_getError():0);
      }
    }
  }
  if(!g_use_real_egl) debugPrintf("egl_shim: REAL EGL indisponivel -> fallback SDL\n");

  SDL_GL_MakeCurrent(egl_window, NULL); /* solta -> qualquer thread pode dar eglMakeCurrent REAL */
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
  debugPrintf("egl_shim: eglGetDisplay() [tid=%lx]\n",(unsigned long)pthread_self());
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
  /* Unity 2018 valida o config por MATCH EXATO de cor (R/G/B/A/LUM) contra o pedido (de
     PlayerSettings, NAO no attrib_list). Em vez de adivinhar, devolvemos VARIOS configs com
     formatos comuns; o ponteiro codifica o indice (0xC0F00+i) e GetConfigAttrib devolve a cor
     daquele formato -> Unity acha o que casa. depth/stencil sao ">=" (24/8 cobrem). */
  int total = 6;
  if (num_config) *num_config = total;
  if (configs && config_size > 0) {
    int n = config_size < total ? config_size : total;
    for (int i = 0; i < n; i++) configs[i] = (EGLConfig)(uintptr_t)(0xC0F00 + i);
    if (num_config) *num_config = n;
  }
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
  /* NAO cria o SDL/EGL context aqui: o Unity cria contextos na MAIN thread mas RENDERIZA numa
     worker thread -> contexto preso a main -> eglMakeCurrent na worker = EGL_BAD_ACCESS. Adiamos
     a criacao do SDL context p/ o 1o MakeCurrent, na PROPRIA thread de render (sdl_context=NULL). */
  c->sdl_context = NULL;
  c->id = next_context_id++;
  /* cria um contexto EGL REAL compartilhado (com o share-root g_real_ctx). NAO o torna current
     -> a thread que fizer MakeCurrent o ativa. Main e gfx-thread cada um seu contexto, compartilham. */
  if (!g_creator_tid) g_creator_tid = (unsigned long)pthread_self(); /* 1a CreateContext = setup thread */
  if (g_use_real_egl && r_createContext) {
    int ctxattr[] = {0x3098 /*EGL_CONTEXT_CLIENT_VERSION*/, 2, 0x3038 /*EGL_NONE*/};
    c->real_ctx = r_createContext(g_real_dpy, g_real_cfg, g_real_ctx /*share*/, ctxattr);
    debugPrintf("egl_shim: eglCreateContext -> ctx_id=%d real=%p [tid=%lx]\n",
                c->id, c->real_ctx, (unsigned long)pthread_self());
    if (!c->real_ctx) c->real_ctx = g_real_ctx; /* fallback: usa o share-root */
  }
  return (EGLContext)c;
}

EGLBoolean egl_shim_MakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                 EGLSurface read, EGLContext ctx) {
  (void)dpy; (void)read;

  _egl_context *context = (_egl_context *)ctx;
  static _Thread_local int mc_count = 0;
  int mc = ++mc_count;

  /* === BULLY-STYLE: eglMakeCurrent REAL (cross-thread OK). bind: (dpy,surf,surf,ctx); unbind: 0s. */
  if (g_use_real_egl) {
    unsigned r;
    if (context == NULL || draw == NULL) {
      /* FRAME-END: a render thread esta SOLTANDO a window -> e o fim do frame dela, o FBO0 (window)
         tem o frame FINAL do Unity. Apresenta AGORA (snapshot+blit+swap) enquanto a window e current
         -> captura a cena renderizada por ultimo (o composite mid-frame era cedo/plano). */
      if (real_current_is_window_surface()) {
        extern void re4_frame_end_present(void);
        re4_frame_end_present();
      }
      r = r_makeCurrent(g_real_dpy, NULL, NULL, NULL);
      current_context = NULL; has_real_gl = 0;
    } else {
      current_context = context; last_context = context;
      unsigned long me=(unsigned long)pthread_self();
      /* UM contexto real compartilhado POR THREAD (a Unity passa o MESMO handle p/ varias threads;
         compartilhar 1 contexto entre threads = BAD_ACCESS). Cada thread cria o seu (share=root)
         na 1a vez. A thread de SETUP usa PBUFFER, a de RENDER usa o WINDOW. */
      static _Thread_local void *tl_ctx=NULL;
      if (!tl_ctx) {
        int ctxattr[]={0x3098,2,0x3038};
        tl_ctx = r_createContext(g_real_dpy, g_real_cfg, g_real_ctx, ctxattr);
        if(!tl_ctx) tl_ctx = (context->real_ctx?context->real_ctx:g_real_ctx);
      }
      void *surf = (me==g_creator_tid && g_real_pbuf && !g_creator_win) ? g_real_pbuf : g_real_surf;
      r = r_makeCurrent(g_real_dpy, surf, surf, tl_ctx);
      if (r == 0 && surf!=g_real_surf) { r = r_makeCurrent(g_real_dpy, g_real_surf, g_real_surf, tl_ctx); surf=g_real_surf; }
      has_real_gl = (r != 0);
      static _Thread_local int lg=0;
      if (lg < 10) { debugPrintf("egl_shim: REAL MakeCurrent %s [tid=%lx] %s tl_ctx=%p err=0x%x\n",
            r?"OK":"FALHOU", me, surf==g_real_pbuf?"PBUF":"WIN", tl_ctx, r?0:(r_getError?r_getError():0)); lg++; }
    }
    return EGL_TRUE;
  }

  /* === UNBIND (fallback SDL) === */
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

  if (!egl_window)
    return EGL_TRUE;

  /* SDL-mali amarra o SDL context a thread que o criou. O Unity cria o contexto na MAIN e
     RENDERIZA numa thread de gfx -> precisa MIGRAR: se o contexto nao existe OU pertence a outra
     thread, (re)cria nesta thread. A thread de render sustentado acaba dona dele. */
  unsigned long me = (unsigned long)pthread_self();
  if (!context->sdl_context || context->owner_tid != me) {
    pthread_mutex_lock(&egl_context_create_mutex);
    if (context->sdl_context) { SDL_GL_DeleteContext(context->sdl_context); context->sdl_context = NULL; }
    SDL_GL_MakeCurrent(egl_window, NULL);
    context->sdl_context = SDL_GL_CreateContext(egl_window);
    context->owner_tid = me;
    pthread_mutex_unlock(&egl_context_create_mutex);
    debugPrintf("egl_shim: (re)criou SDL context p/ ctx_id=%d [tid=%lx] -> %p\n",
                context->id, me, context->sdl_context);
    if (!context->sdl_context) { debugPrintf("egl_shim: SDL_GL_CreateContext FALHOU: %s\n", SDL_GetError()); return EGL_TRUE; }
  }

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
  if (g_use_real_egl) {
    if (r_swapBuffers && g_real_dpy && g_real_surf && real_current_is_window_surface()) {
      egl_shim_maybe_shot(screen_width(), screen_height());
      r_swapBuffers(g_real_dpy, g_real_surf);
      int fc = ++frame_count; static int sl=0;
      if (sl < 8) { debugPrintf("egl_shim: REAL SwapBuffers #%d [tid=%lx]\n", fc, (unsigned long)pthread_self()); sl++; }
    } else {
      static _Thread_local int skip_log = 0;
      if (skip_log < 8) {
        debugPrintf("egl_shim: REAL SwapBuffers SKIPPED [tid=%lx] cur=%p want=%p\n",
                    (unsigned long)pthread_self(),
                    r_getCurSurface ? r_getCurSurface(EGL_DRAW_ATTR) : NULL, g_real_surf);
        skip_log++;
      }
    }
    return EGL_TRUE;
  }
  if (!egl_window) return EGL_TRUE;

  if (has_real_gl && current_context && !current_context->is_pbuffer) {
    egl_shim_maybe_shot(screen_width(), screen_height());
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

void egl_shim_force_present(const char *reason) {
  if (g_use_real_egl) {
    if (r_swapBuffers && g_real_dpy && g_real_surf && real_current_is_window_surface()) {
      r_swapBuffers(g_real_dpy, g_real_surf);
      int fc = ++frame_count;
      static _Thread_local int sl=0;
      if (sl < 12) {
        debugPrintf("egl_shim: FORCE SwapBuffers #%d [tid=%lx] %s\n",
                    fc, (unsigned long)pthread_self(), reason ? reason : "?");
        sl++;
      }
    } else {
      static _Thread_local int skip_log = 0;
      if (skip_log < 12) {
        debugPrintf("egl_shim: FORCE SwapBuffers SKIPPED [tid=%lx] %s cur=%p want=%p\n",
                    (unsigned long)pthread_self(), reason ? reason : "?",
                    r_getCurSurface ? r_getCurSurface(EGL_DRAW_ATTR) : NULL, g_real_surf);
        skip_log++;
      }
    }
    return;
  }

  if (egl_window && has_real_gl && current_context && !current_context->is_pbuffer) {
    SDL_GL_SwapWindow(egl_window);
    int fc = ++frame_count;
    static _Thread_local int sl=0;
    if (sl < 12) {
      debugPrintf("egl_shim: FORCE SDL swap #%d [tid=%lx] %s\n",
                  fc, (unsigned long)pthread_self(), reason ? reason : "?");
      sl++;
    }
  }
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
  if (attribute == 0x3057 && value) *value = screen_width();
  else if (attribute == 0x3056 && value) *value = screen_height();
  return EGL_TRUE;
}

EGLBoolean egl_shim_GetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                                     EGLint attribute, EGLint *value) {
  (void)dpy;
  if (!value) return EGL_TRUE;
  int _logn=0; { static int n=0; _logn=(n++<120); }
  /* cor por-config: o ponteiro codifica o indice (0xC0F00+i). r/g/b/a do formato i. */
  static const int FMT[6][4] = {
    {8,8,8,8}, {8,8,8,0}, {5,6,5,0}, {4,4,4,4}, {5,5,5,1}, {8,8,8,0},
  };
  int idx = ((uintptr_t)config >= 0xC0F00 && (uintptr_t)config < 0xC0F00+6) ? (int)((uintptr_t)config - 0xC0F00) : 0;
  switch (attribute) {
  case 0x3020: *value = FMT[idx][0]; break; /* EGL_RED_SIZE */
  case 0x3021: *value = FMT[idx][1]; break; /* EGL_GREEN_SIZE */
  case 0x3022: *value = FMT[idx][2]; break; /* EGL_BLUE_SIZE */
  case 0x3023: *value = FMT[idx][3]; break; /* EGL_ALPHA_SIZE */
  case 0x3025: *value = 24; break;         /* EGL_DEPTH_SIZE */
  case 0x3026: *value = 8; break;          /* EGL_STENCIL_SIZE */
  case 0x3024: *value = 8; break;          /* EGL_LUMINANCE_SIZE -- Unity pede match exato
                                              (LUM,ALPHA,BLUE,GREEN)=(8,8,8,8); idx0=RGBA8888 +
                                              este LUM=8 fecha o match -> config aceito */
  case 0x3027: *value = 0x3038; break;     /* EGL_CONFIG_CAVEAT = EGL_NONE */
  case 0x3028: *value = idx + 1; break;    /* EGL_CONFIG_ID (unico por config) */
  case 0x3031: *value = 0; break;          /* EGL_SAMPLE_BUFFERS */
  case 0x3032: *value = 0; break;          /* EGL_SAMPLES */
  case 0x3033: *value = 0x0004; break;     /* EGL_SURFACE_TYPE = EGL_WINDOW_BIT */
  case 0x3040: *value = 0x0004; break;     /* EGL_RENDERABLE_TYPE = ES2 SO (4). Unity tenta
                                              ES3.2/3.1/3.0/ES2 em sequencia; anunciar so ES2 faz
                                              ele REJEITAR as ES3 e ACEITAR a ES2 -> usa contexto
                                              ES2 (Mali-450 e ES2-only; ES3 crashava). */
  case 0x3042: *value = 1; break;          /* EGL_BIND_TO_TEXTURE_RGB */
  case 0x3039: *value = 0x0004; break;     /* EGL_CONFORMANT = ES2 */
  case 0x302E: *value = 0; break;          /* EGL_NATIVE_VISUAL_ID */
  case 0x3034: *value = 0; break;          /* EGL_TRANSPARENT_TYPE = EGL_NONE */
  case 0x30e2: *value = 0x30e3; break;     /* Unity 2018 config-match: aceita se 0x30e2==0x30e3 */
  case 0x30e1: *value = 0; break;
  default: *value = 0; break;
  }
  if(_logn) debugPrintf("egl_shim: GetConfigAttrib(0x%x) = %d\n", attribute, *value);
  return EGL_TRUE;
}

EGLint egl_shim_GetError(void) { return EGL_SUCCESS; }
extern void *re4_gl_override(const char *procname);

static void *egl_shim_lookup_egl_proc(const char *procname) {
  if (!procname || procname[0] != 'e' || procname[1] != 'g' || procname[2] != 'l') return NULL;
  if (!strcmp(procname, "eglGetDisplay")) return (void *)egl_shim_GetDisplay;
  if (!strcmp(procname, "eglInitialize")) return (void *)egl_shim_Initialize;
  if (!strcmp(procname, "eglTerminate")) return (void *)egl_shim_Terminate;
  if (!strcmp(procname, "eglChooseConfig")) return (void *)egl_shim_ChooseConfig;
  if (!strcmp(procname, "eglGetConfigAttrib")) return (void *)egl_shim_GetConfigAttrib;
  if (!strcmp(procname, "eglCreateWindowSurface")) return (void *)egl_shim_CreateWindowSurface;
  if (!strcmp(procname, "eglCreatePbufferSurface")) return (void *)egl_shim_CreatePbufferSurface;
  if (!strcmp(procname, "eglDestroySurface")) return (void *)egl_shim_DestroySurface;
  if (!strcmp(procname, "eglCreateContext")) return (void *)egl_shim_CreateContext;
  if (!strcmp(procname, "eglDestroyContext")) return (void *)egl_shim_DestroyContext;
  if (!strcmp(procname, "eglMakeCurrent")) return (void *)egl_shim_MakeCurrent;
  if (!strcmp(procname, "eglSwapBuffers")) return (void *)egl_shim_SwapBuffers;
  if (!strcmp(procname, "eglSwapInterval")) return (void *)egl_shim_SwapInterval;
  if (!strcmp(procname, "eglGetCurrentContext")) return (void *)egl_shim_GetCurrentContext;
  if (!strcmp(procname, "eglGetCurrentSurface")) return (void *)egl_shim_GetCurrentSurface;
  if (!strcmp(procname, "eglGetError")) return (void *)egl_shim_GetError;
  if (!strcmp(procname, "eglBindAPI")) return (void *)egl_shim_BindAPI;
  if (!strcmp(procname, "eglQueryString")) return (void *)egl_shim_QueryString;
  if (!strcmp(procname, "eglQuerySurface")) return (void *)egl_shim_QuerySurface;
  return NULL;
}

void *egl_shim_GetProcAddress(const char *procname) {
  void *ptr = egl_shim_lookup_egl_proc(procname);
  if (ptr) {
    debugPrintf("egl_shim: eglGetProcAddress(%s) -> shim\n", procname);
    return ptr;
  }

  ptr = re4_gl_override(procname);
  if (ptr) {
    debugPrintf("egl_shim: eglGetProcAddress(%s) -> gl-override\n", procname);
    return ptr;
  }

  ptr = SDL_GL_GetProcAddress(procname);
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
