/* egl_shim.c -- SDL2/Mali fbdev EGL bridge for Bully2. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static SDL_Window *g_win;
static SDL_GLContext g_ctx;
static int g_w = 1280;
static int g_h = 720;
static int g_is_kmsdrm = 0;   /* kmsdrm/wayland precisam SDL_GL_SwapWindow p/ page-flip; mali fbdev usa eglSwapBuffers cru */

int bully_is_kmsdrm(void) { return g_is_kmsdrm; }

int bully_screen_w(void) { return g_w; }
int bully_screen_h(void) { return g_h; }

/* Rejeita contexto desktop-GL: no mesa/panfrost um pedido ES pode devolver
 * "3.1 Mesa ..." (GL desktop) que cria OK mas nao roda GLSL-ES -> tela preta
 * (licao Sonic4 EP2 no ROCKNIX). */
static int ctx_is_gles(void) {
  const char *v = (const char *)glGetString(GL_VERSION);
  return v && strstr(v, "OpenGL ES") != NULL;
}

int bully_init_gl(void) {
  if (g_ctx)
    return 1;

  /* forca a lib GLES/EGL (nao desktop-GL/GLX) — inocuo onde ja e o padrao */
  if (!getenv("BULLY2_NO_FORCE_GLES")) {
    SDL_SetHint("SDL_OPENGL_ES_DRIVER", "1");
    SDL_SetHint("SDL_VIDEO_X11_FORCE_EGL", "1");
  }

  if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "[sdl] video init failed: %s\n", SDL_GetError());
    return 0;
  }

  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    g_w = dm.w;
    g_h = dm.h;
  }

  /* Escada de configs (licao do Sonic4 EP2 multi-device: driver mesa/panfrost
   * ou PowerVR pode recusar combos especificos -> tentar varias na MESMA
   * janela logica). O jogo e ES2-path sempre (GL_VERSION spoofado em
   * imports.c), entao a versao do CONTEXTO pode ser 2 ou, em ultimo caso, 3
   * (drivers que so expoem config ES3 completa). alpha=8 (mali fbdev) vs 0
   * (KMSDRM/wayland: scanout XRGB8888 -> GBM nao casa ARGB e a janela nao
   * sobe); depth 24 -> 16 fallback p/ GPUs sem D24 (PowerVR antigas). */
  static const struct {
    int es_major;
    int alpha;
    int depth;
  } cfg_try[] = {
      {2, 8, 24}, {2, 0, 24}, {2, 8, 16}, {2, 0, 16},
      {3, 8, 24}, {3, 0, 24}, {3, 0, 16},
  };
  for (unsigned i = 0; i < sizeof(cfg_try) / sizeof(cfg_try[0]) && !g_win;
       i++) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, cfg_try[i].es_major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, cfg_try[i].alpha);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, cfg_try[i].depth);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_win = SDL_CreateWindow("Bully2", SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED, g_w, g_h,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!g_win) {
      fprintf(stderr, "[sdl] window ES%d alpha=%d depth=%d falhou: %s\n",
              cfg_try[i].es_major, cfg_try[i].alpha, cfg_try[i].depth,
              SDL_GetError());
      continue;
    }
    /* o EGL_BAD_CONFIG real aparece aqui, nao na janela: tenta o contexto e,
     * se falhar, destroi a janela e segue a escada. */
    g_ctx = SDL_GL_CreateContext(g_win);
    if (g_ctx) {
      SDL_GL_MakeCurrent(g_win, g_ctx);
      if (!ctx_is_gles()) {
        fprintf(stderr,
                "[sdl] contexto ES%d alpha=%d depth=%d veio DESKTOP-GL (%s) — "
                "rejeitado\n",
                cfg_try[i].es_major, cfg_try[i].alpha, cfg_try[i].depth,
                (const char *)glGetString(GL_VERSION));
        SDL_GL_DeleteContext(g_ctx);
        g_ctx = NULL;
      }
    } else {
      fprintf(stderr, "[sdl] contexto ES%d alpha=%d depth=%d falhou: %s\n",
              cfg_try[i].es_major, cfg_try[i].alpha, cfg_try[i].depth,
              SDL_GetError());
    }
    if (!g_ctx) {
      SDL_DestroyWindow(g_win);
      g_win = NULL;
      continue;
    }
    if (i)
      fprintf(stderr, "[sdl] GL OK no fallback ES%d alpha=%d depth=%d\n",
              cfg_try[i].es_major, cfg_try[i].alpha, cfg_try[i].depth);
  }
  if (!g_win || !g_ctx) {
    fprintf(stderr, "[sdl] nenhuma config GL funcionou\n");
    return 0;
  }

  /* present path por LISTA POSITIVA: kmsdrm/wayland/x11 precisam de
   * SDL_GL_SwapWindow (page-flip no scanout); 'mali' e qualquer fbdev
   * desconhecido apresentam via eglSwapBuffers cru (backends fbdev variantes
   * de outros CFWs nao fazem page-flip pelo SDL). */
  /* comparacao case-INsensitive: o SDL do EmuELEC novo (X5M) reporta
   * "KMSDRM" em maiusculas — case-sensitive caia no present cru = tela preta. */
  { const char *drv = SDL_GetCurrentVideoDriver();
    g_is_kmsdrm = (drv && (SDL_strcasecmp(drv, "kmsdrm") == 0 ||
                           SDL_strcasecmp(drv, "wayland") == 0 ||
                           SDL_strcasecmp(drv, "x11") == 0)) ? 1 : 0;
    fprintf(stderr, "[gl] backend video='%s' kmsdrm=%d\n", drv ? drv : "?", g_is_kmsdrm); }

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

/* screenshot sob demanda: `touch /dev/shm/bully_shot` -> salva RGBA cru do
 * backbuffer (antes do flip) em /dev/shm/bully_shot.raw + .txt com WxH.
 * Roda na thread de render (contexto GL correto). Permite validar o render
 * sem depender de /dev/fb0 (que no KMSDRM fica estatico). */
void bully_maybe_screenshot(void) {
  static int chk = 0;
  if (++chk % 15)
    return;
  if (access("/dev/shm/bully_shot", F_OK) != 0)
    return;
  unlink("/dev/shm/bully_shot");
  int vp[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_VIEWPORT, vp);
  int w = vp[2], h = vp[3];
  if (w <= 0 || h <= 0) {
    w = g_w;
    h = g_h;
  }
  if (w <= 0 || h <= 0)
    return;
  unsigned char *buf = malloc((size_t)w * h * 4);
  if (!buf)
    return;
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
  FILE *o = fopen("/dev/shm/bully_shot.raw", "wb");
  if (o) {
    fwrite(buf, 1, (size_t)w * h * 4, o);
    fclose(o);
  }
  FILE *t = fopen("/dev/shm/bully_shot.txt", "w");
  if (t) {
    fprintf(t, "%d %d\n", w, h);
    fclose(t);
  }
  free(buf);
  fprintf(stderr, "[shot] %dx%d salvo em /dev/shm/bully_shot.raw\n", w, h);
}

/* BULLY2_FPS_LIMIT=30 (ou 15..120): pacing absoluto no present. O CTimer do
 * engine mede a duracao real do frame, entao a logica adapta sozinha. Se um
 * frame atrasar alem do slot, ressincroniza sem acumular debito. Default off. */
static void bully_fps_limit_pace(void) {
  static long frame_ns = -1;
  if (frame_ns == -1) {
    const char *e = getenv("BULLY2_FPS_LIMIT");
    if (!e || !*e)
      e = getenv("BULLY_FPS_LIMIT");
    int fps = e ? atoi(e) : 0;
    frame_ns = (fps >= 15 && fps <= 120) ? 1000000000L / fps : 0;
    if (frame_ns)
      fprintf(stderr, "[gl] fps limit=%d\n", fps);
  }
  if (!frame_ns)
    return;
  static struct timespec next;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (next.tv_sec == 0 && next.tv_nsec == 0)
    next = now;
  next.tv_nsec += frame_ns;
  while (next.tv_nsec >= 1000000000L) {
    next.tv_nsec -= 1000000000L;
    next.tv_sec++;
  }
  if (now.tv_sec > next.tv_sec ||
      (now.tv_sec == next.tv_sec && now.tv_nsec >= next.tv_nsec)) {
    next = now;
    return;
  }
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
    ;
}

void bully_swap_buffers(void) {
  if (!g_win)
    return;
  bully_maybe_screenshot();
  bully_fps_limit_pace();
  /* fbdev/mali (NAO kmsdrm): presenta via eglSwapBuffers CRU no surface atual --
   * mesmo caminho que o jogo usa e que CHEGA no /dev/fb0. KMSDRM/wayland precisam
   * do page-flip do SDL (drmModePageFlip), senao o buffer GBM nunca vai pro scanout
   * -> tela preta (render + audio OK, mas nada no painel). */
  if (!g_is_kmsdrm) {
    static unsigned (*raw_swap)(void *, void *) = NULL;
    if (!raw_swap)
      raw_swap = (unsigned (*)(void *, void *))dlsym(RTLD_DEFAULT, "eglSwapBuffers");
    EGLDisplay d = eglGetCurrentDisplay();
    EGLSurface s = eglGetCurrentSurface(EGL_DRAW);
    if (raw_swap && d != EGL_NO_DISPLAY && s != EGL_NO_SURFACE) {
      raw_swap(d, s);
      return;
    }
  }
  SDL_GL_SwapWindow(g_win);
}
