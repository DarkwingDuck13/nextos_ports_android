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
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "egl_shim.h"
#include "util.h"

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720

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

void egl_shim_create_window(void) {
  if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
    debugPrintf("egl_shim: SDL_Init falhou: %s\n", SDL_GetError());
  { const char *drv = SDL_GetCurrentVideoDriver();
    FILE *vf = fopen("/tmp/nfs_video.txt", "w");
    if (vf) { fprintf(vf, "video_driver=%s\n", drv ? drv : "(NULL)"); fclose(vf); } }
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
      SCREEN_WIDTH, SCREEN_HEIGHT,
      SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
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
  { int rd=0,gd=0,bd=0,ad=0,dd=0,sd=0;
    SDL_GL_GetAttribute(SDL_GL_RED_SIZE,&rd); SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE,&gd);
    SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE,&bd); SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE,&ad);
    SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE,&dd); SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE,&sd);
    fprintf(stderr,"[EGLCFG] R%d G%d B%d A%d DEPTH=%d STENCIL=%d\n",rd,gd,bd,ad,dd,sd); }

  /* MANTÉM o share_root current desde já (a engine NÃO cria contexto próprio nem
   * faz MakeCurrent — assume o GLSurfaceView; sem isso glGetString=NULL no init do
   * renderer e tudo vai pro vazio). has_real_gl=1 p/ o SwapBuffers apresentar. */
  if (SDL_GL_MakeCurrent(egl_window, egl_share_root) == 0) has_real_gl = 1;
  debugPrintf("egl_shim: share_root MANTIDO current (has_real_gl=%d), ready for game\n", has_real_gl);
}

/* 🔑 No Android o GLSurfaceView cria o contexto GL e o torna CURRENT na render
 * thread; a engine nativa só USA o contexto corrente. No nosso port ninguém faz
 * isso → nenhum contexto current → toda chamada GL falha (glGetString=NULL,
 * texturas/shaders no vazio) → fb0 preto. Esta fn torna o share_root current na
 * thread chamadora (nossa render thread) e o MANTÉM. Chamada antes do render loop. */
extern const unsigned char *glGetString(unsigned int name);
int egl_shim_make_root_current(void) {
  if (!egl_window || !egl_share_root) {
    fprintf(stderr, "[egl] make_root_current: window=%p root=%p (FALHOU)\n",
            (void *)egl_window, (void *)egl_share_root);
    return 0;
  }
  int r = SDL_GL_MakeCurrent(egl_window, egl_share_root);
  has_real_gl = (r == 0);
  fprintf(stderr, "[egl] make_root_current: r=%d err=%s | RENDERER=%s\n", r,
          r ? SDL_GetError() : "ok", (const char *)glGetString(0x1F01)/*GL_RENDERER*/);
  return r == 0;
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

/* present FORÇADO chamado pelo nosso render loop após cada tick. No Android quem
 * faz eglSwapBuffers é o GLSurfaceView (framework Java) — a engine nativa NÃO
 * chama swap. Sem framework, NÓS apresentamos. Não gateia em has_real_gl
 * (thread-local; o tick roda na nossa thread c/ o contexto da engine current). */
int g_disc_frame = 0; /* 🔬 contador de frames apresentados (p/ gatear logs no disclaimer) */
void egl_shim_force_present(void) {
  g_disc_frame++;
  { static int hb = 0; hb++;
    if (hb == 1 || hb % 30 == 0) { FILE *h = fopen("/tmp/present.txt", "w");
      if (h) { fprintf(h, "present_calls=%d window=%p\n", hb, (void *)egl_window); fclose(h); } } }
  if (!egl_window) { static int w=0; if(!w){w=1;fprintf(stderr,"[present] egl_window NULL!\n");} return; }
  static int fc = 0; fc++;
  /* 🔑 GARANTE contexto current TODO frame antes de ler/apresentar. A engine assume
   * o modelo GLSurfaceView: ela desenha mas NÃO mantém o contexto current na nossa
   * thread entre ticks (eglMakeCurrent(NULL) no fim do tick e/ou um worker rouba o
   * current — contexto SDL/GL só pode estar current em 1 thread). Sem re-bind aqui,
   * o glReadPixels e o SDL_GL_SwapWindow operam SEM contexto → backbuffer/tela PRETOS.
   * (Diagnóstico: NFS_TESTRED, que já fazia esse re-bind, mostrava conteúdo real;
   * o caminho normal, que só rebindava nos 3 primeiros frames, dava preto.) */
  {
    void *cur = SDL_GL_GetCurrentContext();
    if (cur != egl_share_root) {
      SDL_GL_MakeCurrent(egl_window, egl_share_root);
      if (fc <= 3) fprintf(stderr, "[present] #%d rebind share-root (was curctx=%p) err=%s tid=%lx\n",
                           fc, cur, SDL_GetError(), (unsigned long)pthread_self());
    }
  }
  /* TESTE do pipe de present: NFS_TESTRED=1 limpa VERMELHO antes do swap.
   * fb0 vermelho => present OK (engine não desenha). fb0 preto => contexto/thread errado. */
  static int testred = -1;
  if (testred < 0) testred = getenv("NFS_TESTRED") ? 1 : 0;
  if (testred) {
    void *cur = SDL_GL_GetCurrentContext();
    if (!cur) SDL_GL_MakeCurrent(egl_window, egl_share_root);
    extern void glClearColor(float,float,float,float); extern void glClear(unsigned);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(0x4000 /*GL_COLOR_BUFFER_BIT*/);
  }
  /* 📸 screenshot AUTOMÁTICO via glReadPixels (lê o BACKBUFFER GL real — o que a
   * engine desenhou, independente de /dev/fb0 que não reflete o Mali/EGL). Salva
   * a cada 100 frames (overwrite) → o último frame antes do crash fica capturado.
   * Bully usa o mesmo método. NFS_NOAUTOSHOT=1 desliga. */
  {
    static int sc = 0, off = -1, seq = -1;
    /* 🔑 DEFAULT-OFF: escrever auto.raw (3.6MB) a cada 30 frames no vfat MARTELAVA o
     * FAT32 → erro → remount READ-ONLY → 2º lançamento não entrava no gameplay (e
     * crashes ao escrever no fs ro, ex. ao usar nitro). Só liga com NFS_AUTOSHOT=1. */
    if (off < 0) off = getenv("NFS_AUTOSHOT") ? 0 : 1;
    if (seq < 0) seq = getenv("NFS_SEQSHOT") ? 1 : 0;
    ++sc;
    if (!off && (sc % 30 == 0 || sc == 5)) {
      static unsigned char *shot;
      int W = SCREEN_WIDTH, H = SCREEN_HEIGHT;
      if (!shot) shot = malloc(W * H * 4);
      extern void glReadPixels(int, int, int, int, unsigned, unsigned, void *);
      extern void glGetIntegerv(unsigned, int *);
      int fbo = -1; glGetIntegerv(0x8CA6 /*GL_FRAMEBUFFER_BINDING*/, &fbo);
      glReadPixels(0, 0, W, H, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, shot);
      /* conta pixels não-pretos p/ saber rápido se TEVE imagem (sem puxar o .raw) */
      long nb = 0; for (long p = 0; p < (long)W*H; p++)
        if (shot[p*4] | shot[p*4+1] | shot[p*4+2]) nb++;
      char nm[64];
      if (seq) snprintf(nm, sizeof nm, "seq_%04d.raw", sc); else snprintf(nm, sizeof nm, "auto.raw");
      FILE *sf = fopen(nm, "wb"); if (sf) { fwrite(shot, 1, (long)W * H * 4, sf); fclose(sf); }
      FILE *st = fopen("shotstats.txt", seq ? "a" : "w");
      if (st) { fprintf(st, "frame=%d fbo_bound=%d nonblack=%ld\n", sc, fbo, nb); fclose(st); }
    }
  }
  { extern void egl_shim_gltrace_dump(int); static int gf=0; if(++gf%30==0) egl_shim_gltrace_dump(gf); }
  SDL_GL_SwapWindow(egl_window);
}

EGLBoolean egl_shim_SwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy; (void)surface;
  if (!egl_window) return EGL_TRUE;

  if (has_real_gl && current_context && !current_context->is_pbuffer) {
    SDL_GL_SwapWindow(egl_window);
    int fc = ++frame_count;
    if (fc <= 5 || fc % 120 == 0) {
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

/* === GL TRACE (NFS_GLTRACE=1): descobre EM QUAL FBO a engine desenha por frame === */
static int g_gltrace = -1;
static unsigned g_cur_fbo = 0;
static unsigned g_bind0 = 0, g_bindN = 0, g_draw_fbo0 = 0, g_draw_fboN = 0, g_clears = 0;
static unsigned g_last_clear_mask = 0;
typedef void (*pfn_glBindFramebuffer)(unsigned, unsigned);
typedef void (*pfn_glDrawArrays)(unsigned, int, int);
typedef void (*pfn_glDrawElements)(unsigned, int, unsigned, const void *);
typedef void (*pfn_glClear)(unsigned);
static pfn_glBindFramebuffer real_glBindFramebuffer;
static pfn_glDrawArrays real_glDrawArrays;
static pfn_glDrawElements real_glDrawElements;
static pfn_glClear real_glClear;
void my_glBindFramebuffer(unsigned target, unsigned fb) {
  if (!real_glBindFramebuffer) real_glBindFramebuffer = (pfn_glBindFramebuffer)SDL_GL_GetProcAddress("glBindFramebuffer");
  g_cur_fbo = fb;
  if (fb == 0) g_bind0++; else g_bindN++;
  real_glBindFramebuffer(target, fb);
}
/* estado p/ diagnóstico do draw (NFS_DRAWLOG): textura/programa/blend ligados */
static unsigned g_cur_prog = 0; static int g_blend = 0;
static unsigned g_unit_tex[8]; static int g_active_unit = 0; /* textura por unidade */
typedef void (*pfn_glBindTexture)(unsigned, unsigned);
typedef void (*pfn_glActiveTexture)(unsigned);
typedef void (*pfn_glUseProgram)(unsigned);
typedef void (*pfn_glEnable)(unsigned);
static pfn_glBindTexture real_glBindTexture;
static pfn_glActiveTexture real_glActiveTexture;
static pfn_glUseProgram real_glUseProgram;
static pfn_glEnable real_glEnable, real_glDisable;
static int g_drawlog = -1, g_drawn = 0;
/* 🔑 textura INCOMPLETA = preto no GLES2: se min-filter pede mipmap mas só o
 * nível 0 foi enviado (sem glGenerateMipmap), a amostragem vira PRETO. O .sba do
 * splash pede LINEAR_MIPMAP_* mas a engine não gera mips → logos pretos. Força
 * min-filter p/ GL_LINEAR (NFS_NOMIPFIX desliga). */
typedef void (*pfn_glTexParameteri)(unsigned,unsigned,int);
static pfn_glTexParameteri real_glTexParameteri;
void my_glTexParameteri(unsigned tgt, unsigned pname, int param){
  if(!real_glTexParameteri) real_glTexParameteri=(pfn_glTexParameteri)SDL_GL_GetProcAddress("glTexParameteri");
  if(pname==0x813D) return; /* GL_TEXTURE_MAX_LEVEL: não existe em GLES2 (gera erro) */
  static int off=-1; if(off<0) off=getenv("NFS_NOMIPFIX")?1:0;
  if(!off && (pname==0x2801/*MIN*/||pname==0x2800/*MAG*/) &&
     (param==0x2700||param==0x2701||param==0x2702||param==0x2703)) /* *_MIPMAP_* */
    param = (param==0x2700||param==0x2701) ? 0x2600 /*NEAREST*/ : 0x2601 /*LINEAR*/;
  real_glTexParameteri(tgt,pname,param);
}
typedef void (*pfn_glUniform4f)(int,float,float,float,float);
typedef void (*pfn_glUniform1i)(int,int);
typedef void (*pfn_glVertexAttrib4f)(unsigned,float,float,float,float);
static pfn_glUniform4f real_glUniform4f;
static pfn_glUniform1i real_glUniform1i;
static pfn_glVertexAttrib4f real_glVertexAttrib4f;
static int g_unilog=-1, g_unin=0;
/* 🎨🔑 A engine é SOFTFP (float args em r0-r3), nosso loader HARDFP (floats em VFP).
 * Sem pcs("aapcs"), my_glUniform4f/glVertexAttrib4f liam os floats do VFP = LIXO →
 * cores de material/vértice garbage → cenário magenta/verde saturado por objeto.
 * pcs("aapcs") faz a função ler os floats dos registradores CORE (softfp). */
__attribute__((pcs("aapcs")))
void my_glUniform4f(int loc,float a,float b,float c,float d){
  if(!real_glUniform4f) real_glUniform4f=(pfn_glUniform4f)SDL_GL_GetProcAddress("glUniform4f");
  if(g_unilog<0) g_unilog=getenv("NFS_UNILOG")?1:0;
  if(g_unilog&&g_unin<5000){ fprintf(stderr,"[uni4f loc=%d] %.2f %.2f %.2f %.2f prog=%u\n",loc,a,b,c,d,g_cur_prog); g_unin++; }
  real_glUniform4f(loc,a,b,c,d);
}
typedef void (*pfn_glUniform4fv)(int,int,const float*);
static pfn_glUniform4fv real_glUniform4fv;
void my_glUniform4fv(int loc,int cnt,const float*v){
  if(!real_glUniform4fv) real_glUniform4fv=(pfn_glUniform4fv)SDL_GL_GetProcAddress("glUniform4fv");
  if(g_unilog<0) g_unilog=getenv("NFS_UNILOG")?1:0;
  if(g_unilog&&g_unin<5000&&v&&cnt>=1){ fprintf(stderr,"[uni4fv loc=%d cnt=%d] %.2f %.2f %.2f %.2f prog=%u\n",loc,cnt,v[0],v[1],v[2],v[3],g_cur_prog); g_unin++; }
  real_glUniform4fv(loc,cnt,v);
}
void my_glUniform1i(int loc,int v){
  if(!real_glUniform1i) real_glUniform1i=(pfn_glUniform1i)SDL_GL_GetProcAddress("glUniform1i");
  if(g_unilog<0) g_unilog=getenv("NFS_UNILOG")?1:0;
  if(g_unilog&&g_unin<5000){ fprintf(stderr,"[uni1i loc=%d]=%d (sampler) prog=%u\n",loc,v,g_cur_prog); g_unin++; }
  real_glUniform1i(loc,v);
}
__attribute__((pcs("aapcs")))
void my_glVertexAttrib4f(unsigned idx,float a,float b,float c,float d){
  if(!real_glVertexAttrib4f) real_glVertexAttrib4f=(pfn_glVertexAttrib4f)SDL_GL_GetProcAddress("glVertexAttrib4f");
  if(g_unilog<0) g_unilog=getenv("NFS_UNILOG")?1:0;
  if(g_unilog&&g_unin<5000){ fprintf(stderr,"[vattr%u] %.2f %.2f %.2f %.2f prog=%u\n",idx,a,b,c,d,g_cur_prog); g_unin++; }
  real_glVertexAttrib4f(idx,a,b,c,d);
}
/* 🎨🔑 TODAS as funções GL com float args precisam de pcs("aapcs") (engine SOFTFP,
 * Mali HARDFP). As não-wrapped iam DIRETO pro Mali hardfp lendo VFP=lixo. As *fv e
 * Matrix*fv usam ponteiro (sem float arg) → não precisam. SDL_GL_GetProcAddress
 * retorna a função hardfp real; o compilador converte softfp→hardfp na chamada. */
#define PCS __attribute__((pcs("aapcs")))
#define GLP(nm) SDL_GL_GetProcAddress(nm)
PCS void my_glUniform1f(int l,float a){ static void(*r)(int,float); if(!r)r=(void(*)(int,float))GLP("glUniform1f"); r(l,a);}
PCS void my_glUniform2f(int l,float a,float b){ static void(*r)(int,float,float); if(!r)r=(void(*)(int,float,float))GLP("glUniform2f"); r(l,a,b);}
PCS void my_glUniform3f(int l,float a,float b,float c){ static void(*r)(int,float,float,float); if(!r)r=(void(*)(int,float,float,float))GLP("glUniform3f"); r(l,a,b,c);}
PCS void my_glVertexAttrib1f(unsigned i,float a){ static void(*r)(unsigned,float); if(!r)r=(void(*)(unsigned,float))GLP("glVertexAttrib1f"); r(i,a);}
PCS void my_glVertexAttrib2f(unsigned i,float a,float b){ static void(*r)(unsigned,float,float); if(!r)r=(void(*)(unsigned,float,float))GLP("glVertexAttrib2f"); r(i,a,b);}
PCS void my_glVertexAttrib3f(unsigned i,float a,float b,float c){ static void(*r)(unsigned,float,float,float); if(!r)r=(void(*)(unsigned,float,float,float))GLP("glVertexAttrib3f"); r(i,a,b,c);}
PCS void my_glClearColor(float a,float b,float c,float d){ static void(*r)(float,float,float,float); if(!r)r=(void(*)(float,float,float,float))GLP("glClearColor"); r(a,b,c,d);}
PCS void my_glBlendColor(float a,float b,float c,float d){ static void(*r)(float,float,float,float); if(!r)r=(void(*)(float,float,float,float))GLP("glBlendColor"); r(a,b,c,d);}
PCS void my_glClearDepthf(float a){ static void(*r)(float); if(!r)r=(void(*)(float))GLP("glClearDepthf"); r(a);}
PCS void my_glDepthRangef(float a,float b){ static void(*r)(float,float); if(!r)r=(void(*)(float,float))GLP("glDepthRangef"); r(a,b);}
PCS void my_glLineWidth(float a){ static void(*r)(float); if(!r)r=(void(*)(float))GLP("glLineWidth"); r(a);}
PCS void my_glPolygonOffset(float a,float b){ static void(*r)(float,float); if(!r)r=(void(*)(float,float))GLP("glPolygonOffset"); r(a,b);}
PCS void my_glSampleCoverage(float a,unsigned char b){ static void(*r)(float,unsigned char); if(!r)r=(void(*)(float,unsigned char))GLP("glSampleCoverage"); r(a,b);}
PCS void my_glTexParameterf(unsigned t,unsigned p,float a){ static void(*r)(unsigned,unsigned,float); if(!r)r=(void(*)(unsigned,unsigned,float))GLP("glTexParameterf"); r(t,p,a);}
/* checa compile/link de shaders — falha = sprite preto (shadergen por-material) */
typedef void (*pfn_glCompileShader)(unsigned);
typedef void (*pfn_glGetShaderiv)(unsigned,unsigned,int*);
typedef void (*pfn_glGetShaderInfoLog)(unsigned,int,int*,char*);
typedef void (*pfn_glLinkProgram)(unsigned);
typedef void (*pfn_glGetProgramiv)(unsigned,unsigned,int*);
typedef void (*pfn_glGetProgramInfoLog)(unsigned,int,int*,char*);
static pfn_glCompileShader real_glCompileShader;
static pfn_glGetShaderiv real_glGetShaderiv;
static pfn_glGetShaderInfoLog real_glGetShaderInfoLog;
static pfn_glLinkProgram real_glLinkProgram;
static pfn_glGetProgramiv real_glGetProgramiv;
static pfn_glGetProgramInfoLog real_glGetProgramInfoLog;
void my_glCompileShader(unsigned sh){
  if(!real_glCompileShader){ real_glCompileShader=(pfn_glCompileShader)SDL_GL_GetProcAddress("glCompileShader");
    real_glGetShaderiv=(pfn_glGetShaderiv)SDL_GL_GetProcAddress("glGetShaderiv");
    real_glGetShaderInfoLog=(pfn_glGetShaderInfoLog)SDL_GL_GetProcAddress("glGetShaderInfoLog"); }
  real_glCompileShader(sh);
  if(getenv("NFS_SHADERDUMP")){ static int dn=0; if(dn<400){
    pfn_glGetShaderInfoLog gss=(pfn_glGetShaderInfoLog)SDL_GL_GetProcAddress("glGetShaderSource");
    if(gss){ char *src=malloc(16384); int n=0; src[0]=0; gss(sh,16384,&n,src);
      fprintf(stderr,"===SHADER sh=%u (%s)===\n%s\n===END===\n",sh, strstr(src,"gl_Position")?"VERT":"FRAG", src); free(src); dn++; } } }
  if(getenv("NFS_SHADERLOG")){ int ok=1; real_glGetShaderiv(sh,0x8B81,&ok);
    if(!ok){ char log[1024]={0}; int n=0; real_glGetShaderInfoLog(sh,1024,&n,log);
      fprintf(stderr,"[SHADER COMPILE FAIL sh=%u] %s\n",sh,log);
      char src[4096]={0}; pfn_glGetShaderInfoLog gss=(pfn_glGetShaderInfoLog)SDL_GL_GetProcAddress("glGetShaderSource");
      if(gss){ gss(sh,4096,&n,src); fprintf(stderr,"  SRC: %.1500s\n",src); } } }
}
void my_glLinkProgram(unsigned p){
  if(!real_glLinkProgram){ real_glLinkProgram=(pfn_glLinkProgram)SDL_GL_GetProcAddress("glLinkProgram");
    real_glGetProgramiv=(pfn_glGetProgramiv)SDL_GL_GetProcAddress("glGetProgramiv");
    real_glGetProgramInfoLog=(pfn_glGetProgramInfoLog)SDL_GL_GetProcAddress("glGetProgramInfoLog"); }
  real_glLinkProgram(p);
  if(getenv("NFS_SHADERLOG")){ int ok=1; real_glGetProgramiv(p,0x8B82,&ok);
    if(!ok){ char log[1024]={0}; int n=0; real_glGetProgramInfoLog(p,1024,&n,log);
      fprintf(stderr,"[PROGRAM LINK FAIL p=%u] %s\n",p,log); } }
}
/* === Receitas Mali-450 Utgard (do port do Bully) ===
 * highp→mediump no FRAGMENT (a PP do Utgard não tem highp → resultados errados/
 * pretos; o VERTEX mantém highp pois a GP é FP32 e o skinning precisa). */
static char *str_replace_all(const char *src, const char *find, const char *repl){
  size_t fl=strlen(find), rl=strlen(repl), n=0;
  for(const char*p=src;(p=strstr(p,find));p+=fl) n++;
  char *out=malloc(strlen(src)+n*(rl>fl?rl-fl:0)+1); char*o=out; const char*p=src,*q;
  while((q=strstr(p,find))){ memcpy(o,p,q-p); o+=q-p; memcpy(o,repl,rl); o+=rl; p=q+fl; }
  strcpy(o,p); return out;
}
typedef void (*pfn_glShaderSource)(unsigned,int,const char*const*,const int*);
static pfn_glShaderSource real_glShaderSource;
void my_glShaderSource(unsigned sh,int count,const char*const*str,const int*len){
  (void)len;
  if(!real_glShaderSource) real_glShaderSource=(pfn_glShaderSource)SDL_GL_GetProcAddress("glShaderSource");
  size_t total=1; for(int i=0;i<count;i++) if(str&&str[i]) total+=strlen(str[i]);
  char *cat=malloc(total); cat[0]=0;
  for(int i=0;i<count;i++) if(str&&str[i]) strcat(cat,str[i]);
  int is_vertex = strstr(cat,"gl_Position")!=NULL;
  static int off=-1; if(off<0) off=getenv("NFS_NOPREC")?1:0;
  char *s = (is_vertex||off) ? cat : str_replace_all(cat,"highp","mediump");
  if(s!=cat) free(cat);
  /* 🎨 DIAG NFS_NOLIGHT: força a luz do mundo = branca (ambient+diffuse=1) e mata o
   * termo hemisphere → mostra a textura crua. Se o magenta SUMIR, a cor vem do
   * lighting (ambient/hemisphere magenta), não da textura. */
  if(!is_vertex && getenv("NFS_NOLIGHT")){
    char *a=str_replace_all(s,"g_AmbientColor.rgb + g_DiffuseColor.rgb","vec3(1.0,1.0,1.0)"); free(s); s=a;
  }
  /* 🎨 DIAG NFS_NOVCOL: força a cor de vértice (v_1) = branca no fragment → se o
   * céu/cena corrigir, o magenta vem da COR DE VÉRTICE (a_Color0, verde perdido). */
  if(!is_vertex && getenv("NFS_NOVCOL")){
    char *a=str_replace_all(s,"* v_1)","* vec4(1.0))"); free(s);
    char *b=str_replace_all(a,"= v_1;","= vec4(1.0);"); free(a); s=b;
  }
  /* 🎨 DIAG NFS_NOHEMI: #define mix→1º arg (mata o termo hemisphere) → mostra a
   * base (textura*luz) crua. Se o magenta sumir, vem do hemisphere/mix. */
  if(!is_vertex && getenv("NFS_NOHEMI")){
    size_t L=strlen(s); char *a=malloc(L+64); strcpy(a,"#define mix(a,b,c) (a)\n"); strcat(a,s); free(s); s=a;
  }
  /* DIAGNÓSTICO NFS_FORCETEX: ignora a cor (varColor/constantColor) no multiply
   * → se os logos aparecerem, a cor do sprite vinha PRETA. */
  if(!is_vertex && getenv("NFS_FORCETEX")){
    char *a=str_replace_all(s,"varColor*texture2D","texture2D"); free(s);
    char *b=str_replace_all(a,"constantColor*texture2D","texture2D"); free(a); s=b;
  }
  const char *one=s;
  if(real_glShaderSource) real_glShaderSource(sh,1,&one,NULL);
  free(s);
}
unsigned egl_cur_tex0(void){ return g_unit_tex[g_active_unit&7]; }
typedef void (*pfn_glDeleteTextures)(int,const unsigned*);
static pfn_glDeleteTextures real_glDeleteTextures;
static void atlas_forget(unsigned id); /* 🔑 esquece refs de atlas ao deletar a textura */
void my_glDeleteTextures(int n,const unsigned*t){
  if(!real_glDeleteTextures) real_glDeleteTextures=(pfn_glDeleteTextures)SDL_GL_GetProcAddress("glDeleteTextures");
  if(getenv("NFS_DELLOG")&&t){ static int c=0; for(int i=0;i<n&&c<60;i++){ fprintf(stderr,"[glDeleteTextures] id=%u\n",t[i]); c++; } }
  /* 🔑 ao deletar uma textura, esquece toda ref de atlas a ela — senão o atlas-
   * rebind binda um id DELETADO/REUSADO (ao sair e voltar a um menu, a fonte/
   * spinner pegava lixo → quebrava/sumia). */
  if(t) for(int i=0;i<n;i++) atlas_forget(t[i]);
  real_glDeleteTextures(n,t);
}
void my_glActiveTexture(unsigned u){
  if(!real_glActiveTexture) real_glActiveTexture=(pfn_glActiveTexture)SDL_GL_GetProcAddress("glActiveTexture");
  int idx=(int)(u-0x84C0); if(idx>=0&&idx<8) g_active_unit=idx; real_glActiveTexture(u);
}
void my_glBindTexture(unsigned tgt, unsigned tex){
  if(!real_glBindTexture) real_glBindTexture=(pfn_glBindTexture)SDL_GL_GetProcAddress("glBindTexture");
  if(tgt==0x0DE1 && g_active_unit>=0 && g_active_unit<8) g_unit_tex[g_active_unit]=tex;
  /* 🔬 NFS_BINDLOG: no disclaimer (frames ~285-375), loga cada bind a unit 0 com o
   * return-address (achar a fn da engine que liga 0/a textura do sprite). */
  static int bindlog=-1; if(bindlog<0) bindlog=getenv("NFS_BINDLOG")?1:0;
  if(bindlog){ extern int g_disc_frame;
    if(g_disc_frame>=285 && g_disc_frame<=375 && tgt==0x0DE1 && g_active_unit==0){
      static int bn=0; if(bn<400){ fprintf(stderr,"[bind] f=%d unit0 tex=%u ra=%p\n",
        g_disc_frame,tex,__builtin_return_address(0)); bn++; } } }
  real_glBindTexture(tgt,tex);
}
#define g_cur_tex g_unit_tex[0]
void my_glUseProgram(unsigned p){
  if(!real_glUseProgram) real_glUseProgram=(pfn_glUseProgram)SDL_GL_GetProcAddress("glUseProgram");
  g_cur_prog=p; real_glUseProgram(p);
}
void my_glEnable(unsigned c){
  if(!real_glEnable) real_glEnable=(pfn_glEnable)SDL_GL_GetProcAddress("glEnable");
  if(c==0x0BE2) g_blend=1; real_glEnable(c);
}
void my_glDisable(unsigned c){
  if(!real_glDisable) real_glDisable=(pfn_glEnable)SDL_GL_GetProcAddress("glDisable");
  if(c==0x0BE2) g_blend=0; real_glDisable(c);
}
/* 🎨 glColorMask: o céu/cenário sai com VERDE=0 (magenta). A engine usa glColorMask
 * p/ um pass que (sem FBO offscreen no nosso port) mascara o verde no FBO 0 (tela).
 * NFS_CMASKLOG loga máscaras parciais; NFS_FORCEGREEN força o verde sempre ligado. */
typedef void (*pfn_glColorMask)(unsigned char,unsigned char,unsigned char,unsigned char);
static pfn_glColorMask real_glColorMask;
void my_glColorMask(unsigned char r,unsigned char g,unsigned char b,unsigned char a){
  if(!real_glColorMask) real_glColorMask=(pfn_glColorMask)SDL_GL_GetProcAddress("glColorMask");
  if(getenv("NFS_CMASKLOG")){ static int n=0; if(n<300 && !(r&&g&&b&&a)){ fprintf(stderr,"[colorMask] r=%d g=%d b=%d a=%d\n",r,g,b,a); n++; } }
  /* só liga o verde quando É um write de cor (R ou B ligados) — NÃO mexe no pass
   * depth-only (0,0,0,0) p/ não quebrar o loading. */
  if(getenv("NFS_FORCEGREEN") && (r||b) && !g){ g=1; static int fn=0; if(fn<20){fprintf(stderr,"[FORCEGREEN] verde religado (era r=%d g=0 b=%d)\n",r,b);fn++;} }
  real_glColorMask(r,g,b,a);
}
static int g_bigdraw_max=0, g_bigdraw_n=0;
static void drawlog(const char*k,int n){
  if(g_drawlog<0) g_drawlog=getenv("NFS_DRAWLOG")?1:0;
  if(g_drawlog&&g_drawn<90){ fprintf(stderr,"[draw %s n=%d] fbo=%u u0=%u u1=%u au=%d prog=%u blend=%d\n",k,n,g_cur_fbo,g_unit_tex[0],g_unit_tex[1],g_active_unit,g_cur_prog,g_blend); g_drawn++; }
  /* 🔎 geometria 3D = draws GRANDES. Loga os maiores vistos (NFS_DRAWLOG). */
  if(g_drawlog){ if(n>g_bigdraw_max) g_bigdraw_max=n;
    if(n>=64 && g_bigdraw_n<60){ fprintf(stderr,"[BIGDRAW %s n=%d] fbo=%u u0=%u prog=%u blend=%d (max=%d)\n",k,n,g_cur_fbo,g_unit_tex[0],g_cur_prog,g_blend,g_bigdraw_max); g_bigdraw_n++; } }
}
static void atlas_rebind(void);
static void atlas_record(void);
void my_glDrawArrays(unsigned m, int f, int c) {
  if (!real_glDrawArrays) real_glDrawArrays = (pfn_glDrawArrays)SDL_GL_GetProcAddress("glDrawArrays");
  if (g_cur_fbo == 0) g_draw_fbo0++; else g_draw_fboN++;
  drawlog("arr",c);
  atlas_record(); /* lembra o atlas deste shader (draws com textura real) */
  if (c < 64) atlas_rebind(); /* só UI (draws pequenos); o atlas-hack estraga o 3D */
  real_glDrawArrays(m, f, c);
}
/* ATLASHACK: religa o atlas quando um draw texturizado tem tex=0 (link sprite->
 * atlas null no .sba). Só afeta shaders que amostram (solid usa shader sem
 * texture2D, não importa). */
unsigned g_nfs_atlas_tex = 0;
static int g_atlashack = -1;
/* 🎨 PER-PROGRAMA (atlas-only): cada shader lembra o último ATLAS (textura grande
 * não-quadrada) usado com ele. O texto (glyph pequeno/quadrado) NÃO entra em
 * g_is_atlas → não polui (foi o que quebrou o per-programa antigo). Nos draws com
 * tex=0, religa o atlas DAQUELE shader → corrige decorações do disclaimer (spinner/
 * formas) que pegavam o atlas errado. Fallback p/ o último atlas global. */
#define ATLAS_MAP_SZ 2048
static unsigned char g_is_atlas[ATLAS_MAP_SZ];
static unsigned g_prog_atlas[ATLAS_MAP_SZ];
static int g_progatlas = -1;
/* 🔑 PER-PROGRAMA: cada shader lembra a última textura REAL usada com ele. A
 * engine liga a textura no 1º draw do batch e desenha os seguintes com tex=0
 * (link sprite->atlas null no .sba) → religamos a textura DAQUELE shader. Tela-
 * agnóstico: corrige logos após MOST WANTED, spinner de loading, etc. (o "último
 * atlas global" errava quando novos packs carregavam). */
/* 🔬 textura 1x1 BRANCA: fallback p/ draws tex=0 (objeto de textura do sprite é
 * null → engine liga 0). Branco = quad mostra a COR DE VÉRTICE (fundo branco do
 * disclaimer fica branco; sprites sem textura viram cor sólida em vez de lixo de
 * atlas). NFS_WHITEHACK liga este modo em vez do atlas-rebind. */
static unsigned g_white_tex = 0;
static unsigned white_tex(void){
  if(g_white_tex) return g_white_tex;
  extern void glGenTextures(int,unsigned*); extern void glTexParameteri(unsigned,unsigned,int);
  extern void glTexImage2D(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
  if(!real_glBindTexture) real_glBindTexture=(pfn_glBindTexture)SDL_GL_GetProcAddress("glBindTexture");
  if(!real_glActiveTexture) real_glActiveTexture=(pfn_glActiveTexture)SDL_GL_GetProcAddress("glActiveTexture");
  unsigned id=0; glGenTextures(1,&id); if(!id) return 0;
  unsigned char px[4]={255,255,255,255};
  real_glActiveTexture(0x84C0); real_glBindTexture(0x0DE1,id);
  glTexImage2D(0x0DE1,0,0x1908,1,1,0,0x1908,0x1401,px);
  glTexParameteri(0x0DE1,0x2801,0x2601/*MIN_FILTER LINEAR*/);
  glTexParameteri(0x0DE1,0x2800,0x2601/*MAG_FILTER LINEAR*/);
  g_white_tex=id; return id;
}
static void atlas_rebind(void){
  if(g_atlashack<0) g_atlashack=getenv("NFS_NOATLASHACK")?0:1; /* PADRÃO ligado */
  if(g_progatlas<0) g_progatlas=getenv("NFS_NOPROGATLAS")?0:1; /* per-prog PADRÃO ligado */
  if(!g_atlashack || g_unit_tex[0]!=0) return;
  /* 🔑 FALLBACK PADRÃO = textura 1x1 BRANCA p/ draws tex=0 (objeto de textura do
   * sprite é null → engine liga 0). O quad passa a mostrar a COR DE VÉRTICE:
   *  - fundo do disclaimer (cor branca) → BRANCO (era preto sem hack / atlas-lixo);
   *  - sprites/decorações sem textura → cor sólida limpa (em vez do lixo de atlas
   *    que dava caixa-preta + formas-fantasma "fora de ordem").
   * Seguro game-wide: conteúdo COM textura (tex!=0, ex. logos/menu) não passa aqui.
   * NFS_ATLASHACK=1 restaura o modo atlas-rebind antigo (diagnóstico). */
  static int atlasmode=-1; if(atlasmode<0) atlasmode=getenv("NFS_ATLASHACK")?1:0;
  if(!atlasmode){ unsigned w=white_tex(); if(w){
    if(!real_glActiveTexture) real_glActiveTexture=(pfn_glActiveTexture)SDL_GL_GetProcAddress("glActiveTexture");
    if(!real_glBindTexture) real_glBindTexture=(pfn_glBindTexture)SDL_GL_GetProcAddress("glBindTexture");
    real_glActiveTexture(0x84C0); real_glBindTexture(0x0DE1,w); } return; }
  /* atlas deste shader (per-programa, atlas-only) → fallback p/ o global */
  unsigned pa = (g_cur_prog<ATLAS_MAP_SZ) ? g_prog_atlas[g_cur_prog] : 0;
  unsigned a = (g_progatlas && pa) ? pa : g_nfs_atlas_tex;
  /* 🔬 NFS_REBINDLOG: loga cada draw tex=0 que religamos — programa, atlas que
   * bindamos (a), atlas per-prog (pa) e o global. Pra achar o atlas CERTO do
   * spinner/decorações do disclaimer (que pegam atlas errado = fora de ordem). */
  if(getenv("NFS_REBINDLOG")){ static int rn=0; if(rn<4000){
    fprintf(stderr,"[rebind] prog=%u bind=%u progatlas=%u global=%u\n",g_cur_prog,a,pa,g_nfs_atlas_tex); rn++; } }
  if(a){
    if(!real_glBindTexture) real_glBindTexture=(pfn_glBindTexture)SDL_GL_GetProcAddress("glBindTexture");
    if(!real_glActiveTexture) real_glActiveTexture=(pfn_glActiveTexture)SDL_GL_GetProcAddress("glActiveTexture");
    real_glActiveTexture(0x84C0); real_glBindTexture(0x0DE1,a);
  }
}
/* registra o atlas usado por este shader (chamado nos draws com textura real) */
static void atlas_record(void){
  unsigned t0=g_unit_tex[0];
  if(t0 && t0<ATLAS_MAP_SZ && g_is_atlas[t0] && g_cur_prog<ATLAS_MAP_SZ)
    g_prog_atlas[g_cur_prog]=t0;
}
/* esquece todas as refs de atlas a uma textura deletada (anti-bind de id stale) */
static void atlas_forget(unsigned id){
  if(id==g_nfs_atlas_tex) g_nfs_atlas_tex=0;
  if(id<ATLAS_MAP_SZ && g_is_atlas[id]){
    g_is_atlas[id]=0;
    for(unsigned j=0;j<ATLAS_MAP_SZ;j++) if(g_prog_atlas[j]==id) g_prog_atlas[j]=0;
  }
}
/* 🔬 TESTE 3D-preto: desabilita depth/cull nos draws GRANDES (geometria 3D) p/
 * ver se a geometria aparece (depth buffer ausente/quebrado no Mali-450 rejeita
 * tudo; HUD desenha sem depth). NFS_NODEPTH/NFS_NOCULL. */
static void big3d_state(int n){
  static int nd=-1,nc=-1;
  if(nd<0){ nd=getenv("NFS_NODEPTH")?1:0; nc=getenv("NFS_NOCULL")?1:0; }
  if(n<64) return;
  if(nd){ if(!real_glDisable) real_glDisable=(pfn_glEnable)SDL_GL_GetProcAddress("glDisable");
    if(real_glDisable) real_glDisable(0x0B71/*GL_DEPTH_TEST*/); }
  if(nc){ if(!real_glDisable) real_glDisable=(pfn_glEnable)SDL_GL_GetProcAddress("glDisable");
    if(real_glDisable) real_glDisable(0x0B44/*GL_CULL_FACE*/); }
}
void my_glDrawElements(unsigned m, int c, unsigned t, const void *i) {
  if (!real_glDrawElements) real_glDrawElements = (pfn_glDrawElements)SDL_GL_GetProcAddress("glDrawElements");
  if (g_cur_fbo == 0) g_draw_fbo0++; else g_draw_fboN++;
  drawlog("elt",c);
  static int draw0log=-1; if(draw0log<0) draw0log=getenv("NFS_BINDLOG")?1:0;
  if(draw0log){ extern int g_disc_frame;
    if(g_disc_frame>=285 && g_disc_frame<=375 && g_unit_tex[0]==0 && c<64){
      static int dn=0; if(dn<400){ fprintf(stderr,"[draw0] f=%d prog=%u c=%d ra=%p\n",
        g_disc_frame,g_cur_prog,c,__builtin_return_address(0)); dn++; } } }
  atlas_record(); /* lembra o atlas deste shader (draws com textura real) */
  if (c < 64) atlas_rebind(); /* só UI (draws pequenos); o atlas-hack estraga o 3D */
  big3d_state(c);
  real_glDrawElements(m, c, t, i);
}
/* 🔑 FIX 3D-preto: o depth-test rejeitava TODA a geometria 3D (mundo preto, só
 * HUD que desenha sem depth). Causa: o clear de depth no Mali-450 fica errado
 * (buffer ~0 em vez de 1.0=far) → fragmentos (z>buffer) falham GL_LESS. Forçamos
 * glClearDepthf(1.0)+glDepthMask(1) ANTES de cada clear de depth → buffer=far →
 * geometria passa. Preserva a ordenação (≠ desabilitar depth). NFS_NODEPTHFIX desliga. */
typedef void (*pfn_glClearDepthf)(float);
typedef void (*pfn_glDepthMask)(unsigned char);
static pfn_glClearDepthf real_glClearDepthf;
static pfn_glDepthMask real_glDepthMask;
void my_glClear(unsigned mask) {
  if (!real_glClear) real_glClear = (pfn_glClear)SDL_GL_GetProcAddress("glClear");
  static int fix=-1; if(fix<0) fix=getenv("NFS_NODEPTHFIX")?0:1;
  if(fix && (mask & 0x100)){ /* GL_DEPTH_BUFFER_BIT */
    if(!real_glClearDepthf) real_glClearDepthf=(pfn_glClearDepthf)SDL_GL_GetProcAddress("glClearDepthf");
    if(!real_glDepthMask) real_glDepthMask=(pfn_glDepthMask)SDL_GL_GetProcAddress("glDepthMask");
    if(real_glDepthMask) real_glDepthMask(1);
    if(real_glClearDepthf) real_glClearDepthf(1.0f);
  }
  g_clears++; g_last_clear_mask = mask;
  real_glClear(mask);
}
typedef void (*pfn_glTexImage2D)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
typedef void (*pfn_glCompressedTexImage2D)(unsigned,int,unsigned,int,int,int,int,const void*);
static pfn_glTexImage2D real_glTexImage2D;
static pfn_glCompressedTexImage2D real_glCompressedTexImage2D;
static int g_teximg_log = -1, g_teximg_n = 0;
void my_glTexImage2D(unsigned t,int l,int ifmt,int w,int h,int b,unsigned fmt,unsigned ty,const void*px){
  if (!real_glTexImage2D) real_glTexImage2D=(pfn_glTexImage2D)SDL_GL_GetProcAddress("glTexImage2D");
  if (g_teximg_log<0) g_teximg_log=getenv("NFS_TEXLOG")?1:0;
  if (g_teximg_log&&l==0&&g_teximg_n<60){ extern unsigned egl_cur_tex0(void); fprintf(stderr,"[texImage2D tex=%u] %dx%d ifmt=0x%x fmt=0x%x type=0x%x px=%p\n",egl_cur_tex0(),w,h,ifmt,fmt,ty,px); g_teximg_n++; }
  /* rastreia o ÚLTIMO atlas de sprite (RGBA grande, NÃO-quadrado = não é
   * glyph/RT) p/ o ATLASHACK religar nos draws texturizados sem textura. */
  { extern unsigned g_nfs_atlas_tex; extern unsigned egl_cur_tex0(void);
    if (l==0) {
      unsigned cur = egl_cur_tex0();
      int is_atlas = (w>=256 && h>=256 && w!=h && (ifmt==0x1908||fmt==0x1908));
      if (cur < ATLAS_MAP_SZ) g_is_atlas[cur] = is_atlas ? 1 : 0; /* marca/limpa (id reusado) */
      if (is_atlas) {
        g_nfs_atlas_tex = cur;
        if(getenv("NFS_TEXLOG")) fprintf(stderr,"[ATLAS candidate tex=%u %dx%d]\n",cur,w,h);
      } } }
  /* NFS_TEXDUMP=1: salva as 1as texturas grandes (atlas) p/ inspeção do que a
   * engine SOBE de verdade (preto vs colorido). */
  if (getenv("NFS_TEXDUMP") && l==0 && w>=256 && h>=256 && px && ty==0x1401) {
    static int dn=0;
    if (dn<8){ char nm[64]; snprintf(nm,sizeof nm,"tex_%d_%dx%d.raw",dn,w,h);
      FILE*f=fopen(nm,"wb"); if(f){ fwrite(px,1,(size_t)w*h*4,f); fclose(f);} dn++; }
  }
  /* 🎨 TESTE NFS_SWAPRB: troca R↔B em texturas não-comprimidas (RGBA/RGB,
   * UNSIGNED_BYTE) → diagnostica se o magenta do cenário é canal trocado (azul→
   * magenta). Cópia em scratch p/ não mexer no buffer da engine. */
  if (px && getenv("NFS_SWAPRB") && ty==0x1401 && (fmt==0x1908||fmt==0x1907)) {
    int nc = (fmt==0x1908)?4:3; long npx=(long)w*h;
    unsigned char *sb=malloc(npx*nc); /* thread-safe: por chamada */
    if (sb){ memcpy(sb,px,npx*nc);
      for(long i=0;i<npx;i++){ unsigned char tmp=sb[i*nc]; sb[i*nc]=sb[i*nc+2]; sb[i*nc+2]=tmp; }
      real_glTexImage2D(t,l,ifmt,w,h,b,fmt,ty,sb); free(sb); return; }
  }
  /* 🗜️ NFS_TEXSCALE=1.3: reduz texturas RGBA/RGB grandes por um fator (box-filter)
   * → menos memória de GPU (Mali usa RAM compartilhada do device, 832MB) + perf.
   * Só nível 0, não-comprimidas, w/h>=128. ETC1 (comprimido) não dá p/ reduzir aqui. */
  { static float texscale=0; if(texscale==0){ const char*e=getenv("NFS_TEXSCALE"); texscale=e?(float)atof(e):1.0f; if(texscale<1.0f)texscale=1.0f; }
    /* 🗜️ reduz TODOS os níveis de mip de forma CONSISTENTE: deriva o tamanho-alvo
     * do nível 0 reconstruído (W0=w<<l) e desce por >>l → level k = level0>>k
     * (regra de mip válida; reduzir só o nível 0 quebrava a cadeia → travava o Mali). */
    if(texscale>1.01f && px && ty==0x1401 && (fmt==0x1908||fmt==0x1907)){
      int W0=w<<l, H0=h<<l;
      if(W0>=512 && H0>=512){  /* só texturas grandes */
        int nc=(fmt==0x1908)?4:3;
        /* 🔑 Mali-450 (Utgard/GLES2) NÃO suporta NPOT com mipmap → snap o tamanho-
         * alvo do nível 0 pra POTÊNCIA-DE-2 ≤ W0/scale (mantém POT, mip válido). */
        int tW=(int)(W0/texscale), tH=(int)(H0/texscale);
        int nW0=1; while(nW0*2<=tW) nW0*=2;
        int nH0=1; while(nH0*2<=tH) nH0*=2;
        int nw=nW0>>l; if(nw<1)nw=1;
        int nh=nH0>>l; if(nh<1)nh=1;
        if(nw<w || nh<h){
          unsigned char*dst=malloc((size_t)nw*nh*nc);
          if(dst){ const unsigned char*src=px;
            for(int y=0;y<nh;y++){ int sy0=y*h/nh, sy1=(y+1)*h/nh; if(sy1<=sy0)sy1=sy0+1;
              for(int x=0;x<nw;x++){ int sx0=x*w/nw, sx1=(x+1)*w/nw; if(sx1<=sx0)sx1=sx0+1;
                for(int c=0;c<nc;c++){ unsigned acc=0,cnt=0;
                  for(int yy=sy0;yy<sy1;yy++) for(int xx=sx0;xx<sx1;xx++){ acc+=src[((size_t)yy*w+xx)*nc+c]; cnt++; }
                  dst[((size_t)y*nw+x)*nc+c]=(unsigned char)(cnt?acc/cnt:0); } } }
            real_glTexImage2D(t,l,ifmt,nw,nh,b,fmt,ty,dst); free(dst); return;
          }
        }
      }
    } }
  real_glTexImage2D(t,l,ifmt,w,h,b,fmt,ty,px);
}
typedef void (*pfn_glViewport)(int,int,int,int);
typedef unsigned (*pfn_glCreateProgram)(void);
static pfn_glViewport real_glViewport;
static int g_vp_log=-1, g_vp_n=0;
void my_glViewport(int x,int y,int w,int h){
  if(!real_glViewport) real_glViewport=(pfn_glViewport)SDL_GL_GetProcAddress("glViewport");
  if(g_vp_log<0) g_vp_log=getenv("NFS_VPLOG")?1:0;
  if(g_vp_log&&g_vp_n<40){ fprintf(stderr,"[viewport] %d,%d %dx%d\n",x,y,w,h); g_vp_n++; }
  real_glViewport(x,y,w,h);
}
/* páginas dinâmicas: a engine cria textura 512x512 vazia e preenche via SubImage */
typedef void (*pfn_glTexSubImage2D)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
static pfn_glTexSubImage2D real_glTexSubImage2D;
void my_glTexSubImage2D(unsigned t,int l,int xo,int yo,int w,int h,unsigned fmt,unsigned ty,const void*px){
  if(!real_glTexSubImage2D) real_glTexSubImage2D=(pfn_glTexSubImage2D)SDL_GL_GetProcAddress("glTexSubImage2D");
  if(getenv("NFS_TEXLOG")){ static int n=0; if(n<50){ extern unsigned egl_cur_tex0(void);
    long nb=0; const unsigned char*p=px; if(p) for(long i=0;i<(long)w*h;i++) if(p[i*4]|p[i*4+1]|p[i*4+2]) nb++;
    fprintf(stderr,"[texSub tex=%u] +%d,%d %dx%d fmt=0x%x nonblack=%ld\n",egl_cur_tex0(),xo,yo,w,h,fmt,nb); n++; } }
  real_glTexSubImage2D(t,l,xo,yo,w,h,fmt,ty,px);
}
void my_glCompressedTexImage2D(unsigned t,int l,unsigned ifmt,int w,int h,int b,int sz,const void*px){
  if (!real_glCompressedTexImage2D) real_glCompressedTexImage2D=(pfn_glCompressedTexImage2D)SDL_GL_GetProcAddress("glCompressedTexImage2D");
  if (g_teximg_log<0) g_teximg_log=getenv("NFS_TEXLOG")?1:0;
  if (g_teximg_log&&l==0&&g_teximg_n<40){ fprintf(stderr,"[compTexImage2D] %dx%d ifmt=0x%x size=%d px=%p\n",w,h,ifmt,sz,px); g_teximg_n++; }
  /* 🎨 NFS_ETCDUMP: salva o ETC1 cru (p/ decodificar no PC e ver se já é magenta). */
  if(getenv("NFS_ETCDUMP") && l==0 && px && w>=512){ static int en=0;
    if(en<40){ char nm[64]; snprintf(nm,sizeof nm,"etc_%d_%dx%d_%x.raw",en,w,h,ifmt);
      FILE*f=fopen(nm,"wb"); if(f){fwrite(px,1,sz,f); fclose(f);} en++; } }
  real_glCompressedTexImage2D(t,l,ifmt,w,h,b,sz,px);
}
void egl_shim_gltrace_dump(int frame) {
  if (g_gltrace < 0) g_gltrace = getenv("NFS_GLTRACE") ? 1 : 0;
  if (g_gltrace != 1) return;
  FILE *f = fopen("gltrace.txt", "w");
  if (f) {
    fprintf(f, "frame=%d cur_fbo=%u bind0=%u bindN=%u draw_fbo0=%u draw_fboN=%u clears=%u last_clear_mask=0x%x\n",
            frame, g_cur_fbo, g_bind0, g_bindN, g_draw_fbo0, g_draw_fboN, g_clears, g_last_clear_mask);
    fclose(f);
  }
  g_bind0 = g_bindN = g_draw_fbo0 = g_draw_fboN = g_clears = 0;
}

void *egl_shim_GetProcAddress(const char *procname) {
  if (g_gltrace < 0) g_gltrace = getenv("NFS_GLTRACE") ? 1 : 0;
  if (g_gltrace == 1) {
    if (!strcmp(procname, "glBindFramebuffer")) {
      if (!real_glBindFramebuffer) real_glBindFramebuffer = (pfn_glBindFramebuffer)SDL_GL_GetProcAddress("glBindFramebuffer");
      if (real_glBindFramebuffer) return (void *)my_glBindFramebuffer;
    } else if (!strcmp(procname, "glDrawArrays")) {
      if (!real_glDrawArrays) real_glDrawArrays = (pfn_glDrawArrays)SDL_GL_GetProcAddress("glDrawArrays");
      if (real_glDrawArrays) return (void *)my_glDrawArrays;
    } else if (!strcmp(procname, "glDrawElements")) {
      if (!real_glDrawElements) real_glDrawElements = (pfn_glDrawElements)SDL_GL_GetProcAddress("glDrawElements");
      if (real_glDrawElements) return (void *)my_glDrawElements;
    } else if (!strcmp(procname, "glClear")) {
      if (!real_glClear) real_glClear = (pfn_glClear)SDL_GL_GetProcAddress("glClear");
      if (real_glClear) return (void *)my_glClear;
    }
  }
  /* 🔑 EGL via eglGetProcAddress: a engine importa SÓ eglGetProcAddress e pega
   * eglCreateContext/eglMakeCurrent/eglSwapBuffers/... por aqui. DEVE retornar
   * NOSSOS shims (SDL2-backed/Mali fbdev) — SDL_GL_GetProcAddress devolveria o
   * libEGL REAL, que precisa de ANativeWindow (inexistente) → surface/contexto
   * falham → "Renderer: NULL" → fb0 preto. Roteando p/ os shims, o pipeline EGL
   * inteiro passa pelo SDL2 → renderiza no fb0. */
  if (procname[0] == 'e' && procname[1] == 'g' && procname[2] == 'l') {
    static const struct { const char *n; void *f; } egltab[] = {
      {"eglGetDisplay", egl_shim_GetDisplay}, {"eglInitialize", egl_shim_Initialize},
      {"eglTerminate", egl_shim_Terminate}, {"eglChooseConfig", egl_shim_ChooseConfig},
      {"eglGetConfigAttrib", egl_shim_GetConfigAttrib},
      {"eglCreateWindowSurface", egl_shim_CreateWindowSurface},
      {"eglCreatePbufferSurface", egl_shim_CreatePbufferSurface},
      {"eglDestroySurface", egl_shim_DestroySurface}, {"eglCreateContext", egl_shim_CreateContext},
      {"eglDestroyContext", egl_shim_DestroyContext}, {"eglMakeCurrent", egl_shim_MakeCurrent},
      {"eglSwapBuffers", egl_shim_SwapBuffers}, {"eglSwapInterval", egl_shim_SwapInterval},
      {"eglGetCurrentContext", egl_shim_GetCurrentContext},
      {"eglGetCurrentSurface", egl_shim_GetCurrentSurface}, {"eglGetError", egl_shim_GetError},
      {"eglQueryString", egl_shim_QueryString}, {"eglQuerySurface", egl_shim_QuerySurface},
      {"eglBindAPI", egl_shim_BindAPI}, {"eglSurfaceAttrib", egl_shim_SurfaceAttrib},
      {"eglGetProcAddress", egl_shim_GetProcAddress}, {0, 0}
    };
    for (int i = 0; egltab[i].n; i++)
      if (strcmp(procname, egltab[i].n) == 0) {
        debugPrintf("egl_shim: eglGetProcAddress(%s) -> shim %p\n", procname, egltab[i].f);
        return egltab[i].f;
      }
  }
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
