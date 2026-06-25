/* egl_shim.c (DEVICE) -- contexto GL via SDL2 (driver "mali" do device faz o
 * EGL fbdev CERTO -> render visível na TV; o raw eglCreateWindowSurface(fbdev_
 * window) dava BAD_ALLOC 0x3003). Expomos os objetos EGL que o SDL2-mali criou
 * (eglGetCurrent*) p/ o libGame (que importa egl* direto) usar o MESMO contexto.
 * API bully_* idêntica -> jni_shim não muda. */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

static SDL_Window *g_win = NULL;
static SDL_GLContext g_ctx = NULL;
static int g_w = 1280, g_h = 720;
static int g_is_kmsdrm = 0;
int bully_is_kmsdrm(void) { return g_is_kmsdrm; }

int bully_screen_w(void) { return g_w; }
int bully_screen_h(void) { return g_h; }

int bully_init_gl(void) {
  if (g_ctx) return 1;
  if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "[sdl] InitVideo: %s\n", SDL_GetError());
    /* o SDL2 do device pode nao ter o backend pedido (ex: Trimui sem kmsdrm)
     * -> solta o SDL_VIDEODRIVER e deixa o SDL escolher o que existe */
    if (getenv("SDL_VIDEODRIVER")) {
      unsetenv("SDL_VIDEODRIVER");
      if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
        fprintf(stderr, "[sdl] InitVideo (auto): %s\n", SDL_GetError());
      else
        fprintf(stderr, "[sdl] InitVideo OK driver auto='%s'\n",
                SDL_GetCurrentVideoDriver());
    }
  }

  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    g_w = dm.w; g_h = dm.h;
  }

  /* Backend-agnostico: tenta alpha=8 (mali fbdev) e, se falhar, alpha=0
   * (KMSDRM/wayland: scanout primario e XRGB8888 sem alpha -> GBM nao casa ARGB).
   * BULLY_MSAA=N (so setado no launcher p/ KMSDRM): pede multisample Nx; se o
   * driver recusar, re-tenta sem (Mali-450/fbdev nunca pede). */
  int msaa = 0;
  { const char *e = getenv("BULLY_MSAA"); if (e) msaa = atoi(e); }
  static const int alpha_try[] = {8, 0};
  int msaa_try[2] = {0, 0}, nmsaa = 1;
  if (msaa > 0) { msaa_try[0] = msaa; msaa_try[1] = 0; nmsaa = 2; }
  int got_msaa = 0;
  for (int j = 0; j < nmsaa && !g_win; j++)
   for (int i = 0; i < 2 && !g_win; i++) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, alpha_try[i]);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, msaa_try[j] ? 1 : 0);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, msaa_try[j]);
    g_win = SDL_CreateWindow("Bully", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                             g_w, g_h, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!g_win)
      fprintf(stderr, "[sdl] CreateWindow alpha=%d msaa=%d: %s\n",
              alpha_try[i], msaa_try[j], SDL_GetError());
    else {
      got_msaa = msaa_try[j];
      if (i) fprintf(stderr, "[sdl] CreateWindow OK com alpha=0 (KMSDRM/XRGB)\n");
    }
  }
  if (!g_win) return 0;
  if (msaa > 0)
    fprintf(stderr, "[gl] MSAA pedido=%dx, conseguido=%dx\n", msaa, got_msaa);
  { const char *drv = SDL_GetCurrentVideoDriver();
    g_is_kmsdrm = (drv && SDL_strcmp(drv, "mali") != 0) ? 1 : 0;   /* kmsdrm/wayland precisam SDL_GL_SwapWindow p/ page-flip */
    fprintf(stderr, "[gl] backend video='%s' kmsdrm=%d\n", drv?drv:"?", g_is_kmsdrm); }
  g_ctx = SDL_GL_CreateContext(g_win);
  if (!g_ctx) { fprintf(stderr, "[sdl] GL_CreateContext: %s\n", SDL_GetError()); return 0; }
  SDL_GL_MakeCurrent(g_win, g_ctx);

  const GLubyte *r = glGetString(GL_RENDERER), *v = glGetString(GL_VERSION);
  fprintf(stderr, "[gl] SDL2 GLES2 %dx%d | EGL dpy=%p surf=%p ctx=%p | %s / %s\n",
          g_w, g_h, (void*)eglGetCurrentDisplay(), (void*)eglGetCurrentSurface(EGL_DRAW),
          (void*)eglGetCurrentContext(), r ? (const char*)r : "?", v ? (const char*)v : "?");
  /* caps p/ decidir melhorias de qualidade (aniso, tamanho max de textura) */
  { const GLubyte *ext = glGetString(GL_EXTENSIONS);
    GLint maxtex = 0, maxaniso_i = 0; float maxaniso = 0;
    glGetIntegerv(0x0D33, &maxtex);                 /* GL_MAX_TEXTURE_SIZE */
    int has_aniso = ext && strstr((const char*)ext, "texture_filter_anisotropic") != NULL;
    if (has_aniso) { glGetIntegerv(0x84FF, &maxaniso_i);  /* GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT */
      glGetFloatv(0x84FF, &maxaniso); }
    fprintf(stderr, "[glcaps] MAX_TEXTURE_SIZE=%d aniso=%s(max=%g/%d)\n",
            maxtex, has_aniso ? "SIM" : "nao", maxaniso, maxaniso_i);
    fprintf(stderr, "[glext] %s\n", ext ? (const char*)ext : "?");
  }
  return 1;
}

/* os objetos EGL REAIS que o SDL2-mali criou (o jogo seeda nos OS_EGL globals e
 * chama eglMakeCurrent direto neles) */
void bully_egl_objects(uintptr_t *d, uintptr_t *s, uintptr_t *c) {
  if (d) *d = (uintptr_t)eglGetCurrentDisplay();
  if (s) *s = (uintptr_t)eglGetCurrentSurface(EGL_DRAW);
  if (c) *c = (uintptr_t)eglGetCurrentContext();
}
int  bully_make_current(void) { return SDL_GL_MakeCurrent(g_win, g_ctx) == 0 ? 1 : 0; }
void bully_release_current(void) { SDL_GL_MakeCurrent(g_win, NULL); }
/* screenshot sob demanda: `touch /dev/shm/bully_shot` -> salva RGBA cru do
 * backbuffer (antes do flip) em /dev/shm/bully_shot.raw + .txt com WxH.
 * Roda na thread de render (contexto GL correto). */
void bully_maybe_screenshot(void) {
  static int chk = 0;
  if (++chk % 15) return;
  if (access("/dev/shm/bully_shot", F_OK) != 0) return;
  unlink("/dev/shm/bully_shot");
  int vp[4] = {0,0,0,0};
  glGetIntegerv(GL_VIEWPORT, vp);
  int w = vp[2], h = vp[3];
  if (w <= 0 || h <= 0) return;
  unsigned char *buf = malloc((size_t)w * h * 4);
  if (!buf) return;
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
  FILE *o = fopen("/dev/shm/bully_shot.raw", "wb");
  if (o) { fwrite(buf, 1, (size_t)w * h * 4, o); fclose(o); }
  FILE *t = fopen("/dev/shm/bully_shot.txt", "w");
  if (t) { fprintf(t, "%d %d\n", w, h); fclose(t); }
  free(buf);
  fprintf(stderr, "[shot] %dx%d salvo em /dev/shm/bully_shot.raw\n", w, h);
}
void bully_swap_buffers(void) {
  if (!g_win) return;
  { extern int bully_bake_active, bully_bake_cur, bully_bake_total; extern void bully_bake_ui(int, int);
    if (bully_bake_active) { static int n=0; if(n<3){fprintf(stderr,"[swap] via SDL_GL_SwapWindow (bake)\n");n++;} bully_bake_ui(bully_bake_cur, bully_bake_total); } }
  bully_maybe_screenshot();
  /* fbdev/mali (NAO kmsdrm): PRESENTA via eglSwapBuffers CRU no surface atual -- mesmo
   * caminho que o jogo usa e que CHEGA no /dev/fb0. O SDL_GL_SwapWindow no modo splash
   * standalone NAO estava presentando no fb0 (tela preta na extracao). KMSDRM precisa
   * do page-flip do SDL, entao la mantemos SDL_GL_SwapWindow. */
  if (!g_is_kmsdrm) {
    static unsigned (*raw_swap)(void*, void*) = NULL;
    if (!raw_swap) raw_swap = (unsigned(*)(void*,void*))dlsym(RTLD_DEFAULT, "eglSwapBuffers");
    EGLDisplay d = eglGetCurrentDisplay(); EGLSurface s = eglGetCurrentSurface(EGL_DRAW);
    if (raw_swap && d != EGL_NO_DISPLAY && s != EGL_NO_SURFACE) { raw_swap(d, s); return; }
  }
  SDL_GL_SwapWindow(g_win);
}
