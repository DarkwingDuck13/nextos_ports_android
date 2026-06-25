/*
 * FINAL FANTASY VII (engine materialg / SQEX, jp.co.d4e) -> aarch64 Linux
 * so-loader para Mali-450 fbdev.
 *
 * Carrega libjni_ff7.so (STL estatica, sem aux), monta JNIEnv falso e dirige o
 * GLESJniWrapper sem ART:
 *   JNI_OnLoad -> setAssetManager -> setDataPath -> setLang
 *   -> onSurfaceCreated -> onSurfaceChanged(w,h) -> loop onDrawFrame
 * Assets do APK (Shaders/AVConfig) via AAsset shim (./assets/...).
 * Dados do jogo (OBB extraido) via setDataPath -> fopen <datapath>/ff7_1.02/...
 * Audio: OpenSLES shim. Input: SDL -> onKey/onTouch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <SDL2/SDL.h>

#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"
#include "fmv.h"

/* GL (libGLESv2) p/ a injecao de frame de FMV na textura 2D */
extern void glGenTextures(int, unsigned *);
extern void glBindTexture(unsigned, unsigned);
extern void glTexParameteri(unsigned, unsigned, int);
extern void glTexImage2D(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
extern void glTexSubImage2D(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
extern void glActiveTexture(unsigned);

typedef int jint;
typedef unsigned char jboolean;
typedef float jfloat;

#define MEMORY_MB 320
#define SO_NAME "libjni_ff7.so"

/* ---- Android keycodes (onKey usa keycodes Android) ---- */
#define AKEYCODE_DPAD_UP 19
#define AKEYCODE_DPAD_DOWN 20
#define AKEYCODE_DPAD_LEFT 21
#define AKEYCODE_DPAD_RIGHT 22
#define AKEYCODE_DPAD_CENTER 23
#define AKEYCODE_BUTTON_A 96
#define AKEYCODE_BUTTON_B 97
#define AKEYCODE_BUTTON_X 99
#define AKEYCODE_BUTTON_Y 100
#define AKEYCODE_BUTTON_L1 102
#define AKEYCODE_BUTTON_R1 103
#define AKEYCODE_BUTTON_START 108
#define AKEYCODE_BUTTON_SELECT 109
#define AKEYCODE_ENTER 66
#define AKEYCODE_BACK 4

/* ---- GLESJniWrapper (jp.co.d4e.materialg) ---- */
static jint (*JNI_OnLoad)(void *vm, void *reserved);
static void (*setAssetManager)(void *env, void *clazz, void *assetmgr);
static void (*setDataPath)(void *env, void *clazz, void *jstr);
static void (*setLang)(void *env, void *clazz, jint lang);
static void (*onSurfaceCreated)(void *env, void *clazz);
static void (*onSurfaceChanged)(void *env, void *clazz, jint w, jint h);
static void (*onDrawFrame)(void *env, void *clazz);
static void (*onResume)(void *env, void *clazz);
static void (*onPause)(void *env, void *clazz);
static void (*onKey)(void *env, void *clazz, jint keycode, jboolean pressed);
static void (*onKeyBack)(void *env, void *clazz);
static void (*onTouchBegan)(void *env, void *clazz, jfloat x, jfloat y);
static void (*onTouchMoved)(void *env, void *clazz, jfloat x, jfloat y);
static void (*onTouchEnded)(void *env, void *clazz, jfloat x, jfloat y);
static void (*callUpdateTitlemenu)(void *env, void *clazz);
static void (*g_fw_stop_movie)(void);   /* _Z13fw_stop_moviev: encerra o FMV em curso */
static void (*setBatteryLevel)(void *env, void *clazz, jint lvl);
static void (*VIDEO_update)(void);  /* render real single-thread (bypassa worker) */
/* SQEX CoreSystem master volume (DynamicValue global). SetMasterVolume e' "static"
 * (ignora this, usa global) -> chamavel como (float vol, unsigned flag). A musica
 * (AKB Ogg via SQEX CoreSource) e' multiplicada por esse master no RenderMix; se
 * ficar 0 (init default, nunca setado), a BGM/audio de batalha sai MUDA. */
static float (*g_GetMasterVolume)(void);
static void  (*g_SetMasterVolume)(float vol, unsigned flag);
/* SoundSystem_Update: tick do sistema de som mobile (SQEX). Normalmente um THREAD
 * timer (SoundSystem::CreateUpdateTimer) o chama periodicamente; no nosso ambiente
 * esse timer NAO dispara, entao o streaming de BGM (StreamingSound::Update ->
 * decode Ogg -> QueueBuffer) drena depois dos 1os buffers e a musica fica MUDA.
 * Chamamos por frame (como callUpdateTitlemenu) p/ realimentar o streaming. */
static void (*g_SoundSystem_Update)(void);

static SDL_GameController *g_gamepad = NULL;
static void *g_env = NULL;

/* ---- Trampoline hook (diag audio): chama o original e mede a saida ---- */
typedef void (*rendermix_fn)(void *thisp, void *out, unsigned size);
static rendermix_fn g_orig_rendermix = NULL;

static void *make_trampoline(uintptr_t fn, int nbytes) {
  void *tr = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr == MAP_FAILED) return NULL;
  memcpy(tr, (void *)fn, nbytes);             /* prologo original (relocavel) */
  uint32_t *p = (uint32_t *)((char *)tr + nbytes);
  p[0] = 0x58000051u;                          /* LDR X17, #8 */
  p[1] = 0xd61f0220u;                          /* BR X17 */
  *(uint64_t *)(p + 2) = fn + nbytes;          /* continua em fn+nbytes */
  __builtin___clear_cache((char *)tr, (char *)tr + 4096);
  return tr;
}

extern void ff7_music_feed(const void *pcm, unsigned bytes);
static int g_music_bypass = 0;

/* ---- FMV: injeta o frame VP8 decodificado (fmv.c) numa textura 2D e forca o
 * shader p/ enableOES=0 (sampler2D), driblando o samplerExternalOES que o Mali
 * fbdev nao alimenta. Hook em VIDEO_enableOES: so' filmes chamam com en=1. ---- */
typedef void (*enableoes_fn)(int);
static enableoes_fn g_orig_enableOES = NULL;
typedef int (*playmidi_fn)(unsigned, unsigned, unsigned);
static playmidi_fn g_orig_playmidi = NULL;
static int my_playmidi(unsigned a, unsigned b, unsigned c) {
  debugPrintf("PLAYMIDI music req a=%u b=%u c=%u\n", a, b, c);
  return g_orig_playmidi(a, b, c);
}
typedef void (*avi_open_fn)(const char *);
static avi_open_fn g_orig_avi_open = NULL;
static void my_avi_open(const char *name) {
  fmv_set_movie_by_name(name);   /* captura qual filme p/ nosso decode VP8 */
  if (g_orig_avi_open) g_orig_avi_open(name);
}
static unsigned g_fmv_tex = 0;
static int g_fmv_tex_w = 0, g_fmv_tex_h = 0;
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE0 0x84C0
#define GL_TEX_MIN 0x2801
#define GL_TEX_MAG 0x2800
#define GL_LINEAR 0x2601
#define GL_WRAP_S 0x2802
#define GL_WRAP_T 0x2803
#define GL_CLAMP_EDGE 0x812F

static int fmv_decode_and_bind(void) {
  int got = fmv_next_frame();
  if (!g_fmv_tex && !got) return 0; /* nada p/ mostrar -> deixa o engine no OES */
  int w = fmv_w(), h = fmv_h();
  if (!g_fmv_tex) {
    glGenTextures(1, &g_fmv_tex);
    glBindTexture(GL_TEXTURE_2D, g_fmv_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEX_MIN, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEX_MAG, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_WRAP_S, GL_CLAMP_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_WRAP_T, GL_CLAMP_EDGE);
  }
  glActiveTexture(GL_TEXTURE0); /* s_texture = unidade 0 */
  glBindTexture(GL_TEXTURE_2D, g_fmv_tex);
  const unsigned char *pix = fmv_rgba();
  static unsigned char *redbuf = NULL;
  if (getenv("FF7_FMVRED")) { /* DIAG: vermelho solido -> testa se a injecao chega na tela */
    if (!redbuf) { redbuf = malloc((size_t)w * h * 4); for (int k = 0; k < w * h; k++) { redbuf[k*4]=255; redbuf[k*4+1]=0; redbuf[k*4+2]=0; redbuf[k*4+3]=255; } }
    pix = redbuf; got = 1;
  }
  if (got) {
    if (w != g_fmv_tex_w || h != g_fmv_tex_h) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
      g_fmv_tex_w = w; g_fmv_tex_h = h;
    } else {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    }
  }
  return 1;
}

static void my_enableOES(int en) {
  if (en && getenv("FF7_NOFMV") == NULL && fmv_has_movie() && fmv_decode_and_bind()) {
    static unsigned ic = 0;
    if (ic < 6 || ic % 100 == 0) debugPrintf("FMVINJECT #%u (en=1 -> 2D %dx%d)\n", ic, fmv_w(), fmv_h());
    ic++;
    g_orig_enableOES(0); /* shader usa s_texture (nosso frame 2D), nao o OES */
    return;
  }
  g_orig_enableOES(en);
}

static void my_RenderMix(void *thisp, void *out, unsigned size) {
  g_orig_rendermix(thisp, out, size);
  /* BYPASS: o RenderMix encheu `out` com a BGM em 44100 stereo s16 (perfeita);
   * roteamos direto p/ o player dedicado, driblando o resample quebrado p/ 32000. */
  if (g_music_bypass)
    ff7_music_feed(out, size);
  if (getenv("FF7_RMDIAG")) {
    static unsigned cnt = 0;
    cnt++;
    if (cnt <= 8 || cnt % 200 == 1) {
      int pk = 0; int n = size / 2; const short *s = (const short *)out;
      for (int i = 0; i < n && i < 8192; i++) { int a = s[i] < 0 ? -s[i] : s[i]; if (a > pk) pk = a; }
      debugPrintf("RMDIAG #%u this=%p out=%p size=%u peak=%d\n", cnt, thisp, out, size, pk);
    }
  }
}

/* present single-thread: VIDEO_update chama sem_post(semB)/sem_wait(semA) no fim;
 * nossos wrappers (imports.c) chamam g_ff7_present_cb no sem_post(semB). */
extern void *g_ff7_present_semA, *g_ff7_present_semB;
extern void (*g_ff7_present_cb)(void);
extern void *text_base;  /* base de carga do .so (so_util) */
/* EGL do host (libEGL) p/ salvar os handles na struct de present do FF7 */
extern void *eglGetCurrentDisplay(void);
extern void *eglGetCurrentSurface(int readdraw);
extern void *eglGetCurrentContext(void);
extern unsigned eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx);
#include <pthread.h>

static SDL_Window *g_window = NULL;
static SDL_GLContext g_glc = NULL;
static void *g_egl_dpy, *g_egl_surf, *g_egl_ctx;  /* p/ re-bind no present */
static int g_w = 0, g_h = 0;
static long g_frame = 0;
static long g_maxframes = 0;
static long g_shots_every = 0;
static int g_quit = 0;

/* CANARY BIONIC (SOTN/Bully/Dysmantle/Chrono): lib compilada p/ bionic le a
 * stack-canary de tpidr_el0+0x28 (TLS_SLOT_STACK_GUARD). Sob glibc esse offset
 * colide com TLS do Mali/SDL -> canary "muda" -> __stack_chk_fail falso. Pad
 * _Thread_local desloca o TLS estatico p/ tpidr+0x28 cair num pad nunca-escrito. */
__attribute__((used, aligned(16))) _Thread_local char g_bionic_guard_pad[256];

static SDL_GLContext gl_create_context_guarded(SDL_Window *w) {
  unsigned long tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  unsigned long g = *(unsigned long *)(tp + 0x28);
  SDL_GLContext c = SDL_GL_CreateContext(w);
  *(unsigned long *)(tp + 0x28) = g;
  return c;
}
static void gl_swap_guarded(SDL_Window *w) {
  unsigned long tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  unsigned long g = *(unsigned long *)(tp + 0x28);
  SDL_GL_SwapWindow(w);
  *(unsigned long *)(tp + 0x28) = g;
}

/* SDL controller/keyboard -> Android keycode */
static int map_btn_android(int b) {
  /* codigos INTERNOS do FF7 (tabela 0x132b260): 0=UP 1=LEFT 2=DOWN 3=RIGHT
   * 4=A(OK) 5=B(cancel) 6=X 7=Y 8=L1 9=R1 10=START 11=SELECT 12=L2 13=R2. */
  switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return 4;   /* A = OK/confirmar */
    case SDL_CONTROLLER_BUTTON_B: return 5;   /* B = cancelar */
    case SDL_CONTROLLER_BUTTON_X: return 6;
    case SDL_CONTROLLER_BUTTON_Y: return 7;
    case SDL_CONTROLLER_BUTTON_START: return 10;
    case SDL_CONTROLLER_BUTTON_BACK: return 11;  /* SELECT */
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return 8;   /* L1 */
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 9;  /* R1 */
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return 0;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 2;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 1;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 3;
    default: return -1;
  }
}
static int map_key_android(SDL_Keycode k) {
  switch (k) {
    case SDLK_UP: return 0;
    case SDLK_LEFT: return 1;
    case SDLK_DOWN: return 2;
    case SDLK_RIGHT: return 3;
    case SDLK_SPACE: case SDLK_z: case SDLK_RETURN: return 4;  /* OK */
    case SDLK_LCTRL: case SDLK_x: case SDLK_BACKSPACE: return 5; /* cancel */
    case SDLK_LSHIFT: case SDLK_a: return 6;
    case SDLK_LALT: case SDLK_s: return 7;
    case SDLK_q: return 8;
    case SDLK_w: return 9;
    case SDLK_RETURN2: return 10;
    case SDLK_TAB: return 11;
    default: return -1;
  }
}

static void send_button(int sdl_button, int pressed) {
  if (!onKey) return;
  int kc = map_btn_android(sdl_button);
  if (kc >= 0) onKey(g_env, NULL, kc, pressed);
}

static void open_gamepad(void) {
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      g_gamepad = SDL_GameControllerOpen(i);
      if (g_gamepad) { debugPrintf("Gamepad: %s\n", SDL_GameControllerName(g_gamepad)); break; }
    }
  }
}

extern void opensles_shim_pump_callbacks(void) __attribute__((weak));

/* screenshot via glReadPixels (fb0 falha durante render Mali). */
extern void glReadPixels(int x, int y, int w, int h, unsigned fmt, unsigned type, void *px);
static void ff7_dump_shot(int w, int h, int frame) {
  size_t n = (size_t)w * h * 4;
  unsigned char *buf = malloc(n);
  if (!buf) return;
  glReadPixels(0, 0, w, h, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, buf);
  const char *home = getenv("HOME"); if (!home) home = "/tmp";
  char path[256]; snprintf(path, sizeof path, "%s/ff7_shot_%04d.raw", home, frame);
  FILE *f = fopen(path, "wb");
  if (f) { fwrite(buf, 1, n, f); fclose(f); debugPrintf("SHOT %s (%dx%d)\n", path, w, h); }
  free(buf);
}

/* FF7 roda o .exe do FF7 PC sob tradutor x86->ARM. O loop principal do PC e'
 * while(PeekMessage){if WM_QUIT break} else run_frame(). fw_PeekMessageA estava
 * devolvendo WM_QUIT apos 1 frame -> jogo saia. Stubamos p/ "sem mensagem" (0):
 * o loop nunca ve WM_QUIT e roda frames pra sempre (input via DirectInput). */
static int ff7_peek_stub(void) { return 0; }
/* diagnostico: quem dispara o exit do loop Win32 do FF7 PC? */
static void ff7_exitproc_stub(unsigned code) { debugPrintf(">>> fw_ExitProcess(%u) chamado (no-op)\n", code); }
static void ff7_postquit_stub(unsigned code) { debugPrintf(">>> fw_PostQuitMessage(%u) chamado (no-op)\n", code); }
static unsigned ff7_destroywin_stub(unsigned h) { debugPrintf(">>> fw_DestroyWindow(%u) chamado (no-op)\n", h); return 1; }

/* pump de input (single-thread: chamado a cada present, nao ha loop SDL). */
static void ff7_pump_input(void) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT: g_quit = 1; break;
      case SDL_KEYDOWN: case SDL_KEYUP:
        if (e.key.repeat) break;
        if (e.key.keysym.sym == SDLK_ESCAPE && e.type == SDL_KEYDOWN) { g_quit = 1; break; }
        if (onKey) { int kc = map_key_android(e.key.keysym.sym);
          if (kc >= 0) onKey(g_env, NULL, kc, e.type == SDL_KEYDOWN); }
        break;
      case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: {
        int down = (e.type == SDL_CONTROLLERBUTTONDOWN);
        /* HOTKEY PADRAO PORTMASTER: SELECT+START juntos = sair do jogo. */
        static int hk_start = 0, hk_select = 0;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_START) hk_start = down;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)  hk_select = down;
        if (hk_start && hk_select) { debugPrintf("SELECT+START -> quit\n"); g_quit = 1; break; }
        send_button(e.cbutton.button, down);
        break; }
      case SDL_CONTROLLERAXISMOTION: {
        /* gatilhos L2/R2 (codigos 12/13) e analogico esquerdo -> dpad (0/1/2/3).
         * estado anterior p/ enviar so' na borda (nao spammar onKey). */
        static int tl = 0, tr = 0, ax = 0, ay = 0;  /* -1/0/1 p/ stick */
        int a = e.caxis.axis, v = e.caxis.value;
        if (a == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
          int on = v > 8000; if (on != tl) { tl = on; if (onKey) onKey(g_env, NULL, 12, on); }
        } else if (a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
          int on = v > 8000; if (on != tr) { tr = on; if (onKey) onKey(g_env, NULL, 13, on); }
        } else if (a == SDL_CONTROLLER_AXIS_LEFTX) {
          /* deadzone ALTO (24000=~73%) p/ drift do analogico nao brigar c/ o dpad. */
          int d = v < -24000 ? -1 : v > 24000 ? 1 : 0;
          if (d != ax) {
            if (ax == -1 && onKey) onKey(g_env, NULL, 1, 0);  /* LEFT up */
            if (ax == 1 && onKey)  onKey(g_env, NULL, 3, 0);  /* RIGHT up */
            if (d == -1 && onKey) onKey(g_env, NULL, 1, 1);
            if (d == 1 && onKey)  onKey(g_env, NULL, 3, 1);
            ax = d;
          }
        } else if (a == SDL_CONTROLLER_AXIS_LEFTY) {
          int d = v < -24000 ? -1 : v > 24000 ? 1 : 0;
          if (d != ay) {
            if (ay == -1 && onKey) onKey(g_env, NULL, 0, 0);  /* UP up */
            if (ay == 1 && onKey)  onKey(g_env, NULL, 2, 0);  /* DOWN up */
            if (d == -1 && onKey) onKey(g_env, NULL, 0, 1);
            if (d == 1 && onKey)  onKey(g_env, NULL, 2, 1);
            ay = d;
          }
        }
        break;
      }
      case SDL_MOUSEBUTTONDOWN: if (onTouchBegan) onTouchBegan(g_env, NULL, e.button.x, e.button.y); break;
      case SDL_MOUSEBUTTONUP:   if (onTouchEnded) onTouchEnded(g_env, NULL, e.button.x, e.button.y); break;
      case SDL_MOUSEMOTION:
        if ((e.motion.state & SDL_BUTTON_LMASK) && onTouchMoved)
          onTouchMoved(g_env, NULL, e.motion.x, e.motion.y); break;
    }
  }
}

/* present callback: chamado pelo wrapper ff7_sem_post(semB) (frame renderizado,
 * context ja liberado pelo VIDEO_update). Faz o swap da janela + input. */
static void ff7_present_cb(void) {
  /* DIAG: estado do movie/FMV (movie_object @0x1cd8c8c, counter @0x1cd8c9c). */
  if ((g_frame < 6 || g_frame % 120 == 0) && text_base) {
    uintptr_t mo = *(uintptr_t *)((uintptr_t)text_base + 0x1cd8c8c);
    unsigned mc = *(unsigned *)((uintptr_t)text_base + 0x1cd8c9c);
    uintptr_t pmo = *(uintptr_t *)((uintptr_t)text_base + 0x1cd8c90);
    debugPrintf("MOVIEDIAG frame %ld: movie_object=0x%lx pMovie=0x%lx frame_counter=%u\n",
                g_frame, (unsigned long)mo, (unsigned long)pmo, mc);
  }
  /* FF7_FMVSKIP: encerra o FMV de abertura (longo, preto sem decode) p/ chegar no
   * campo rapido. fw_stop_movie quando o movie_frame_counter indica movie tocando
   * (>4 evita boot). Gate por frame > 5 evita lixo de boot. */
  if (getenv("FF7_FMVSKIP") && g_fw_stop_movie && text_base && g_frame > 5) {
    unsigned mc = *(unsigned *)((uintptr_t)text_base + 0x1cd8c9c);
    if (mc > 4 && mc < 0x40000000u) { g_fw_stop_movie(); }
  }
  /* FF7_FMVSTOPFRAME=N: para o FMV so' DEPOIS do frame N (testar se o FMV preto
   * fica desenhado POR CIMA do campo, escondendo o background pre-renderizado). */
  if (g_fw_stop_movie && text_base) {
    const char *sf = getenv("FF7_FMVSTOPFRAME");
    if (sf && g_frame > atol(sf)) g_fw_stop_movie();
  }
  /* VIDEO_update LIBEROU o context (eglMakeCurrent NULL) antes do sem_post(semB).
   * Re-adquirimos o context p/ o swap (e o glReadPixels do shot) funcionar — sem
   * context, SDL_GL_SwapWindow vira no-op no Mali fbdev (= tela preta). Soltamos
   * de novo no fim p/ o eglMakeCurrent(rebind) do VIDEO_update funcionar. */
  /* com keepctx (default) o context NUNCA e' solto pelo engine -> ja' esta'
   * current, so' swappa (rapido). Sem keepctx, re-adquire (modo antigo). */
  extern int g_ff7_keepctx;
  int reacq = !g_ff7_keepctx && getenv("FF7_NOREACQ") == NULL;
  if (reacq && g_egl_dpy) eglMakeCurrent(g_egl_dpy, g_egl_surf, g_egl_surf, g_egl_ctx);
  if (g_frame < 3 || (g_shots_every > 0 && g_frame % g_shots_every == 0))
    ff7_dump_shot(g_w, g_h, (int)g_frame);
  gl_swap_guarded(g_window);
  if (reacq && g_egl_dpy) eglMakeCurrent(g_egl_dpy, NULL, NULL, NULL);
  /* AUDIO STREAMING: no modo single-thread (default) o loop do jogo roda DENTRO
   * do worker_fn, entao o pump dos buffer-queue callbacks (que realimenta audio
   * em streaming: musica/BGM e SFX em loop das batalhas) NAO era chamado em lugar
   * nenhum — so' o FF7_THREADED o chamava. Sem isso, so' SFX one-shot (pegar item,
   * selecionar) tocam (enfileirados 1x), e musica/audio de batalha ficam MUDOS.
   * Chamamos por frame aqui = realimenta o ring de cada player streaming. */
  /* tick do streaming de som ANTES do pump (decode Ogg -> QueueBuffer), senao a
   * BGM/audio de batalha drena = mudo. O thread-timer do engine nao dispara aqui. */
  if (g_SoundSystem_Update && getenv("FF7_NOSNDUPD") == NULL)
    g_SoundSystem_Update();
  if (opensles_shim_pump_callbacks && getenv("FF7_NOAUDIOPUMP") == NULL)
    opensles_shim_pump_callbacks();
  /* DIAG/FIX master volume da musica (SQEX). FF7_MVOLDIAG loga o valor; FF7_FORCEMVOL
   * forca 1.0 (teste: se a BGM aparecer, o master estava 0). */
  if (g_GetMasterVolume && getenv("FF7_MVOLDIAG") && (g_frame < 3 || g_frame % 120 == 0))
    debugPrintf("MVOLDIAG frame %ld master_volume=%f\n", g_frame, g_GetMasterVolume());
  if (g_SetMasterVolume && getenv("FF7_FORCEMVOL") && g_frame % 30 == 0)
    g_SetMasterVolume(1.0f, 0);
  ff7_pump_input();
  /* o menu de titulo so' atualiza quando callUpdateTitlemenu seta o flag (a Java
   * side chamava isso por frame/touch); no single-thread chamamos nos. */
  if (callUpdateTitlemenu) callUpdateTitlemenu(g_env, NULL);
  /* FF7_AUTOSKIP=1: sequencia automatica p/ TESTE (so dev). Fase 1: BUT_A repetido
   * skipa logo/creditos ate o menu. Fase 2 (dica do NextOS): no menu o cursor cai
   * em "Continue?"; UP 1x sobe p/ NEW GAME, A entra. Threshold ajustavel via
   * FF7_SKIP_MENUFRAME (frame em que o menu ja' esta' pronto, default 520). */
  /* idle so' em FMV "de verdade" (ex opening), NAO nos logos de boot (eidoslogo/
   * sqlogo) — esses a gente deixa o autoskip passar p/ chegar no New Game. */
  const char *mn = fmv_current_name();
  int real_fmv = fmv_has_movie() && mn && !strstr(mn, "logo") && !fmv_eof() && getenv("FF7_NOFMV") == NULL;
  if (getenv("FF7_AUTOSKIP") && onKey && !real_fmv) {
    /* IMPORTANTE: para de injetar input quando o FMV (opening) comeca senao o B/A
     * do ciclo PULA o video. Assim o FMV toca inteiro p/ ver/capturar. */
    long menuf = getenv("FF7_SKIP_MENUFRAME") ? atol(getenv("FF7_SKIP_MENUFRAME")) : 400;
    if (g_frame < menuf) {
      long s = g_frame % 40;                       /* fase1: A skipa logo/creditos */
      if (s == 5)  onKey(g_env, NULL, 4, 1);
      if (s == 12) onKey(g_env, NULL, 4, 0);
    } else {
      /* fase2 AUTO-CORRETIVA: o cursor cai em "Continue?"; se A entrou na tela de
       * saves, B volta. Ciclo: B(cancel) -> UP(p/ NEW GAME) -> A(entra). Repete. */
      long c = (g_frame - menuf) % 160;
      if (c == 0)  onKey(g_env, NULL, 5, 1);   /* B press  (5=cancel) */
      if (c == 8)  onKey(g_env, NULL, 5, 0);
      if (c == 40) onKey(g_env, NULL, 0, 1);   /* UP press (0=UP -> NEW GAME) */
      if (c == 48) onKey(g_env, NULL, 0, 0);
      if (c == 80) onKey(g_env, NULL, 4, 1);   /* A press  (4=OK -> entra) */
      if (c == 88) onKey(g_env, NULL, 4, 0);
    }
  }
  if (g_frame < 5 || g_frame % 120 == 0) debugPrintf("present frame %ld\n", g_frame);
  g_frame++;
  if (g_quit || (g_maxframes && g_frame >= g_maxframes)) {
    debugPrintf("present: exit (frame %ld)\n", g_frame);
    if (onPause) onPause(g_env, NULL);
    SDL_Quit();
    _exit(0);
  }
}

int main(int argc, char *argv[]) {
  { volatile char c = g_bionic_guard_pad[0]; (void)c; }
  {
    unsigned long tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    debugPrintf("TLSDIAG tp=0x%lx pad=%p tp+0x28=0x%lx pad_in_range=%d\n",
                tp, (void *)g_bionic_guard_pad, tp + 0x28,
                ((uintptr_t)g_bionic_guard_pad <= tp + 0x28 &&
                 tp + 0x28 < (uintptr_t)g_bionic_guard_pad + 256));
  }
  debugPrintf("=== FINAL FANTASY VII (materialg) AARCH64 so-loader ===\n");

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    fatal_error("SDL_Init: %s", SDL_GetError());
  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) != 0)
    fatal_error("SDL_GetDesktopDisplayMode: %s", SDL_GetError());

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  SDL_Window *window = SDL_CreateWindow("FINAL FANTASY VII",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, dm.w, dm.h,
      SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN);
  if (!window) fatal_error("SDL_CreateWindow: %s", SDL_GetError());
  SDL_GLContext glc = gl_create_context_guarded(window);
  if (!glc) fatal_error("SDL_GL_CreateContext: %s", SDL_GetError());
  int w, h; SDL_GL_GetDrawableSize(window, &w, &h);
  debugPrintf("Window %dx%d\n", w, h);

  open_gamepad();

  /* ---- libjni_ff7.so (engine + jogo). STL estatica -> sem modulo aux. ---- */
  size_t heap_size = (size_t)MEMORY_MB * 1024 * 1024;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) fatal_error("mmap heap %d MB", MEMORY_MB);

  if (so_load(SO_NAME, heap, heap_size) < 0) fatal_error("so_load %s", SO_NAME);
  debugPrintf("Loaded %s: text=%p+%zu data=%p+%zu\n", SO_NAME, text_base, text_size, data_base, data_size);
  if (so_relocate() < 0) fatal_error("so_relocate");
  if (so_resolve(dynlib_functions, dynlib_functions_count, 0) < 0) fatal_error("so_resolve");
  so_make_text_writable();
  so_flush_caches();
  so_execute_init_array();

  JNI_OnLoad       = (void *)so_find_addr("JNI_OnLoad");
  setAssetManager  = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_setAssetManager");
  setDataPath      = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_setDataPath");
  setLang          = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_setLang");
  onSurfaceCreated = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onSurfaceCreated");
  onSurfaceChanged = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onSurfaceChanged");
  onDrawFrame      = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onDrawFrame");
  onResume         = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onResume");
  onPause          = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onPause");
  onKey            = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onKey");
  onKeyBack        = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onKeyBack");
  onTouchBegan     = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onTouchBegan");
  onTouchMoved     = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onTouchMoved");
  onTouchEnded     = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onTouchEnded");
  callUpdateTitlemenu = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_callUpdateTitlemenu");
  g_fw_stop_movie = (void *)so_find_addr_safe("_Z13fw_stop_moviev");
  g_SoundSystem_Update = (void *)so_find_addr_safe("SoundSystem_Update");
  g_GetMasterVolume = (void *)so_find_addr_safe("_ZN4SQEX10CoreSystem15GetMasterVolumeEv");
  g_SetMasterVolume = (void *)so_find_addr_safe("_ZN4SQEX10CoreSystem15SetMasterVolumeEfj");
  /* Hook VIDEO_enableOES p/ injetar o FMV decodificado (default ON; FF7_NOFMV desliga). */
  if (getenv("FF7_NOFMV") == NULL) {
    uintptr_t ve = so_find_addr_safe("VIDEO_enableOES");
    if (ve) {
      g_orig_enableOES = (enableoes_fn)make_trampoline(ve, 16);
      if (g_orig_enableOES) { hook_arm64(ve, (uintptr_t)&my_enableOES);
        debugPrintf("FMV: hooked VIDEO_enableOES @%p tramp=%p\n", (void*)ve, (void*)g_orig_enableOES); }
    } else debugPrintf("FMV: VIDEO_enableOES nao achado\n");
    uintptr_t ao = so_find_addr_safe("_Z8AVI_openPKc");
    if (ao) {
      g_orig_avi_open = (avi_open_fn)make_trampoline(ao, 16);
      if (g_orig_avi_open) { hook_arm64(ao, (uintptr_t)&my_avi_open);
        debugPrintf("FMV: hooked AVI_open @%p\n", (void*)ao); }
    } else debugPrintf("FMV: AVI_open nao achado\n");
  }
  if (getenv("FF7_MIDILOG")) {
    uintptr_t pm = so_find_addr_safe("_Z12fw_play_midijjj");
    if (pm) { g_orig_playmidi = (playmidi_fn)make_trampoline(pm, 16);
      if (g_orig_playmidi) { hook_arm64(pm, (uintptr_t)&my_playmidi);
        debugPrintf("MIDILOG: hooked fw_play_midi @%p\n", (void*)pm); } }
  }

  /* TESTE de decode (sem GL): decodifica N frames do opening e grava PPM -> prova
   * o pipeline VP8->RGBA isolado. FF7_FMVDUMP=N (default 6). */
  if (getenv("FF7_FMVDUMP")) {
    int nf = atoi(getenv("FF7_FMVDUMP")); if (nf <= 0) nf = 6;
    fmv_set_movie_from_webm("/roms/ports/ff7/gamedata/ff7_1.02/data/movies/opening.webm");
    const char *home = getenv("HOME"); if (!home) home = "/tmp";
    for (int i = 0; i < nf; i++) {
      if (!fmv_next_frame()) { debugPrintf("FMVDUMP: sem frame %d\n", i); break; }
      int w = fmv_w(), h = fmv_h(); const unsigned char *r = fmv_rgba();
      char p[256]; snprintf(p, sizeof p, "%s/fmvdump_%02d.ppm", home, i);
      FILE *f = fopen(p, "wb");
      if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int k = 0; k < w * h; k++) fwrite(r + (size_t)k * 4, 1, 3, f);
        fclose(f); debugPrintf("FMVDUMP wrote %s (%dx%d)\n", p, w, h); }
    }
    _exit(0);
  }

  /* Hook do RenderMix p/ o BYPASS de BGM (default ON; FF7_NOMUSICBYPASS desliga). */
  g_music_bypass = (getenv("FF7_NOMUSICBYPASS") == NULL);
  if (g_music_bypass || getenv("FF7_RMDIAG")) {
    uintptr_t rm = so_find_addr_safe("_ZN4SQEX10CoreSource9RenderMixEPvm");
    if (rm) {
      g_orig_rendermix = (rendermix_fn)make_trampoline(rm, 16);
      if (g_orig_rendermix) { hook_arm64(rm, (uintptr_t)&my_RenderMix);
        debugPrintf("BGM bypass: hooked RenderMix @%p tramp=%p bypass=%d\n",
                    (void*)rm, (void*)g_orig_rendermix, g_music_bypass); }
      else g_music_bypass = 0;
    } else g_music_bypass = 0;
  }
  setBatteryLevel  = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_setBatteryLevel");
  VIDEO_update     = (void *)so_find_addr_safe("VIDEO_update");

  /* stub fw_PeekMessageA -> 0 (senao o loop Win32 do FF7 PC ve WM_QUIT e sai). */
  {
    uintptr_t peek = so_find_addr_safe("_Z15fw_PeekMessageAjjjjj");
    if (peek) { hook_arm64(peek, (uintptr_t)&ff7_peek_stub);
      debugPrintf("hooked fw_PeekMessageA -> 0\n"); }
    else debugPrintf("WARN: fw_PeekMessageA nao achado\n");
    uintptr_t ep = so_find_addr_safe("_Z14fw_ExitProcessj");
    if (ep) hook_arm64(ep, (uintptr_t)&ff7_exitproc_stub);
    uintptr_t pq = so_find_addr_safe("_Z18fw_PostQuitMessagej");
    if (pq) hook_arm64(pq, (uintptr_t)&ff7_postquit_stub);
    uintptr_t dw = so_find_addr_safe("_Z16fw_DestroyWindowj");
    if (dw) hook_arm64(dw, (uintptr_t)&ff7_destroywin_stub);
    debugPrintf("exit-diag hooks: ExitProcess=%p PostQuit=%p DestroyWin=%p\n",
                (void*)ep, (void*)pq, (void*)dw);
  }

  if (!onDrawFrame || !onSurfaceChanged)
    fatal_error("missing GLESJniWrapper onDrawFrame/onSurfaceChanged");

  void *fake_vm = NULL, *fake_env = NULL;
  jni_shim_init(&fake_vm, &fake_env);
  g_env = fake_env;

  debugPrintf("JNI_OnLoad...\n");
  if (JNI_OnLoad) JNI_OnLoad(fake_vm, NULL);

  void *dummy = (void *)0x1337;
  /* setAssetManager: AAssetManager_fromJava devolve fake; AAsset shim le ./assets/ */
  if (setAssetManager) { debugPrintf("setAssetManager\n"); setAssetManager(fake_env, NULL, dummy); }

  /* setDataPath: dir contendo ff7_1.02/. OBB extraido (sem prefixo assets/). */
  const char *datapath = getenv("FF7_DATA");
  if (!datapath) datapath = "/storage/roms/ports/ff7/data";
  void *jpath = jni_make_string(datapath);
  if (setDataPath) { debugPrintf("setDataPath(%s)\n", datapath); setDataPath(fake_env, NULL, jpath); }

  /* JAMAIS JAPONES: forcar ingles. setLang: 0=EN 1=FR 2=DE 3/4=ES/JP (verif. por
   * screenshot 2026-06-24). FF7_LANG default 0 = INGLES. */
  int lang = getenv("FF7_LANG") ? atoi(getenv("FF7_LANG")) : 0;
  if (setLang) { debugPrintf("setLang(%d)\n", lang); setLang(fake_env, NULL, lang); }

  if (setBatteryLevel) setBatteryLevel(fake_env, NULL, 100);

  debugPrintf("onSurfaceCreated...\n");
  if (onSurfaceCreated) onSurfaceCreated(fake_env, NULL);
  debugPrintf("onSurfaceChanged(%d,%d)...\n", w, h);
  onSurfaceChanged(fake_env, NULL, w, h);
  if (onResume) onResume(fake_env, NULL);

  g_window = window; g_w = w; g_h = h;
  g_maxframes = getenv("FF7_MAXFRAMES") ? atol(getenv("FF7_MAXFRAMES")) : 0;
  g_shots_every = getenv("FF7_SHOTS") ? atol(getenv("FF7_SHOTS")) : 0;

  /* ==== SINGLE-THREAD (default) ====
   * O FF7 roda o jogo (main_ff7) num WORKER thread com o EGL context migrado
   * via eglMakeCurrent — o que TRAVA no Mali fbdev (surface presa a' thread que
   * a criou). Em vez disso rodamos main_ff7 na PROPRIA main thread (dona do
   * context). A funcao do worker (libjni+0x150658) faz JNI_attachMe +
   * eglMakeCurrent(rebind, mesma thread=OK) + main_ff7(loop do jogo).
   * VIDEO_update, a cada frame, faz sem_post(semB)/sem_wait(semA) p/ entregar o
   * present a' "UI thread"; nossos wrappers (imports.c) fazem o swap no
   * sem_post(semB) via g_ff7_present_cb e nao bloqueiam no sem_wait(semA).
   * struct de present @ vaddr 0x1cce0b0: +0 semA, +0x10 semB, +32 dpy, +40/48
   * surf, +56 ctx, +64 tid-dona. Populamos os handles EGL e tid = main. */
  if (getenv("FF7_THREADED")) {
    /* fallback: deixa ff7_DrawFrame fazer o threaded (so renderiza o bezel). */
    debugPrintf("render mode: ff7_DrawFrame (threaded, fallback)\n");
    while (!g_quit) {
      ff7_pump_input();
      if (callUpdateTitlemenu) callUpdateTitlemenu(g_env, NULL);
      if (opensles_shim_pump_callbacks) opensles_shim_pump_callbacks();
      onDrawFrame(g_env, NULL);
      gl_swap_guarded(window);
      if (g_shots_every>0 && g_frame>0 && g_frame%g_shots_every==0) ff7_dump_shot(w,h,(int)g_frame);
      g_frame++;
      if (g_maxframes && g_frame >= g_maxframes) break;
    }
  } else {
    uintptr_t base = (uintptr_t)text_base;
    uintptr_t st = base + 0x1cce0b0;            /* struct de present */
    *(void **)(st + 32) = eglGetCurrentDisplay();
    *(void **)(st + 40) = eglGetCurrentSurface(0x3059 /*EGL_DRAW*/);
    *(void **)(st + 48) = eglGetCurrentSurface(0x305a /*EGL_READ*/);
    *(void **)(st + 56) = eglGetCurrentContext();
    g_egl_dpy = *(void **)(st + 32); g_egl_surf = *(void **)(st + 40);
    g_egl_ctx = *(void **)(st + 56);  /* p/ re-bind no present cb */
    *(void **)(st + 64) = (void *)pthread_self();  /* tid dona do present = main */
    g_ff7_present_semA = (void *)(st + 0);
    g_ff7_present_semB = (void *)(st + 0x10);
    g_ff7_present_cb = ff7_present_cb;
    /* PERF: manter o context sempre current (engine nao solta) -> swap rapido. */
    extern int g_ff7_keepctx;
    if (!getenv("FF7_RELEASECTX")) g_ff7_keepctx = 1;
    debugPrintf("present struct: dpy=%p surf=%p ctx=%p tid=%p semA=%p semB=%p\n",
                *(void **)(st+32), *(void **)(st+40), *(void **)(st+56),
                *(void **)(st+64), g_ff7_present_semA, g_ff7_present_semB);

    /* arg do worker: [+0]=global(0x133a000+2408), [+8]=JNIEnv */
    void *(*JNI_getEnv)(void) = (void *)so_find_addr_safe("JNI_getEnv");
    void *worker_arg[4];
    worker_arg[0] = *(void **)(base + 0x133a000 + 2408);
    worker_arg[1] = JNI_getEnv ? JNI_getEnv() : g_env;
    worker_arg[2] = NULL; worker_arg[3] = NULL;

    void (*worker_fn)(void *) = (void *)(base + 0x150658);
    int loop_drive = getenv("FF7_LOOPDRIVE") != NULL;
    debugPrintf("render mode: single-thread%s; worker_fn=%p\n",
                loop_drive ? " (loop-drive)" : "", (void *)worker_fn);
    if (loop_drive) {
      /* main_ff7 faz 1 frame por chamada (design mobile: driver externo).
       * Chamamos em loop -> anima. (init guardado na 1a chamada.) */
      int n = 0;
      while (!g_quit && (!g_maxframes || g_frame < g_maxframes)) {
        worker_fn(worker_arg);
        if (++n <= 3 || n % 60 == 0) debugPrintf("worker_fn iter %d (frame %ld)\n", n, g_frame);
      }
    } else {
      worker_fn(worker_arg);   /* uma vez (main_ff7 deveria ser o loop) */
    }
    debugPrintf("worker_fn returned (game exited)\n");
  }

  debugPrintf("Exiting...\n");
  if (onPause) onPause(g_env, NULL);
  if (g_gamepad) SDL_GameControllerClose(g_gamepad);
  SDL_Quit();
  return 0;
}
