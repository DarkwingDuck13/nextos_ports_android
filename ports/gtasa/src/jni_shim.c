/* jni_shim.c -- fake JNI 64-bit p/ Bully (porta do bully_vita/jni_patch.c).
 * Offsets do JNINativeInterface = indice_spec * 8 (64-bit) = offset_vita * 2.
 * Input via SDL_GameController. */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "so_util_x64.h"
#include "jni_shim.h"
#include "util.h"   /* ret0 */
#include "zip_fs.h"

extern Module mod_game;
/* TUNE STREAMING (opcao 1 anti-stutter/RAM): reduz a distancia de streaming dos objetos do mundo
 * (Loading::IplStreamingDist) + LOD de ped/veiculo por BULLY_STREAM_MULT (<1 = menos mundo residente
 * -> menos RAM -> menos file-fault/OOM). Motor GTA/RenderWare. Idempotente (guarda os originais). */
static void bully_tune_stream(void) {
  static int resolved = 0; static float mult = -1;
  static float *p_ipl = 0, *p_ped = 0, *p_veh = 0; static float o_ipl = 0, o_ped = 0, o_veh = 0;
  if (mult < 0) { const char *e = getenv("BULLY_STREAM_MULT"); mult = e ? (float)atof(e) : 1.0f; if (mult <= 0.05f) mult = 1.0f; }
  if (mult == 1.0f) return;
  if (!resolved) {
    p_ipl = (float *)so_symbol(&mod_game, "_ZN7Loading16IplStreamingDistE");
    p_ped = (float *)so_symbol(&mod_game, "_ZN18CVisibilityPlugins13ms_pedLodDistE");
    p_veh = (float *)so_symbol(&mod_game, "_ZN18CVisibilityPlugins18ms_vehicleLod0DistE");
    fprintf(stderr, "[stream] mult=%.2f symbols ipl=%p ped=%p veh=%p\n", mult, (void *)p_ipl, (void *)p_ped, (void *)p_veh);
    resolved = 1;
  }
  /* captura o ORIGINAL lazy (so quando o motor ja inicializou: valor > 1) */
  if (p_ipl && o_ipl <= 1 && *p_ipl > 1) { o_ipl = *p_ipl; fprintf(stderr, "[stream] ipl orig=%.1f -> %.1f\n", o_ipl, o_ipl * mult); }
  if (p_ped && o_ped <= 1 && *p_ped > 1) { o_ped = *p_ped; fprintf(stderr, "[stream] ped orig=%.1f -> %.1f\n", o_ped, o_ped * mult); }
  if (p_veh && o_veh <= 1 && *p_veh > 1) o_veh = *p_veh;
  if (p_ipl && o_ipl > 1) *p_ipl = o_ipl * mult;
  if (p_ped && o_ped > 1) *p_ped = o_ped * mult;
  if (p_veh && o_veh > 1) *p_veh = o_veh * mult;
}
extern void bully_swap_buffers(void);  /* egl_shim */
extern int bully_screen_w(void);
extern int bully_screen_h(void);
extern int  bully_init_gl(void);       /* egl_shim */
extern int  bully_make_current(void);
extern void bully_release_current(void);
extern void bully_egl_objects(uintptr_t *d, uintptr_t *s, uintptr_t *c);

#define DATA_PATH "."   /* STORAGE_ROOT (ajustar p/ dir dos assets/OBB) */

enum {
  UNKNOWN = 0, INIT_EGL_AND_GLES2, SWAP_BUFFERS, MAKE_CURRENT, UN_MAKE_CURRENT,
  SHARE_TEXT, SHARE_IMAGE,
  HAS_APP_LOCAL_VALUE, GET_APP_LOCAL_VALUE, SET_APP_LOCAL_VALUE, GET_PARAMETER,
  FILE_GET_ARCHIVE_NAME, DELETE_FILE,
  GET_DEVICE_INFO, GET_DEVICE_TYPE, GET_DEVICE_LOCALE,
  GET_GAMEPAD_TYPE, GET_GAMEPAD_BUTTONS, GET_GAMEPAD_AXIS,
  ROCKSTAR_SHOW_INITIAL, ROCKSTAR_SHOW_GATE,
};
static struct { const char *name; int id; } method_ids[] = {
  {"rockstarShowInitial", ROCKSTAR_SHOW_INITIAL}, {"rockstarShowGate", ROCKSTAR_SHOW_GATE},
  {"InitEGLAndGLES2", INIT_EGL_AND_GLES2}, {"swapBuffers", SWAP_BUFFERS},
  {"makeCurrent", MAKE_CURRENT}, {"unMakeCurrent", UN_MAKE_CURRENT},
  {"ShareText", SHARE_TEXT}, {"ShareImage", SHARE_IMAGE},
  {"hasAppLocalValue", HAS_APP_LOCAL_VALUE}, {"getAppLocalValue", GET_APP_LOCAL_VALUE},
  {"setAppLocalValue", SET_APP_LOCAL_VALUE}, {"getParameter", GET_PARAMETER},
  {"FileGetArchiveName", FILE_GET_ARCHIVE_NAME}, {"DeleteFile", DELETE_FILE},
  {"GetDeviceInfo", GET_DEVICE_INFO}, {"GetDeviceType", GET_DEVICE_TYPE},
  {"GetDeviceLocale", GET_DEVICE_LOCALE},
  {"GetGamepadType", GET_GAMEPAD_TYPE}, {"GetGamepadButtons", GET_GAMEPAD_BUTTONS},
  {"GetGamepadAxis", GET_GAMEPAD_AXIS},
};

static char fake_vm[0x1000];
static char fake_env[0x1000];
static void *natives;
static SDL_GameController *g_pad;

/* ---- métodos "Java" que o jogo chama de volta ---- */
static int GetDeviceType(void) { return (2048 << 6) | (3 << 2) | 0x1; } /* mem|tegra3|phone */
static int swapBuffers(void) {
  static unsigned long n = 0;
  if (n < 10 || n % 60 == 0) fprintf(stderr, "[gtasa] swapBuffers #%lu (RENDER!)\n", n);
  n++;
  bully_swap_buffers();
  return 1;
}
static int InitEGLAndGLES2(void) { return bully_init_gl(); }
static char *getAppLocalValue(char *key) {
  if (key && strcmp(key, "STORAGE_ROOT") == 0) return (char *)DATA_PATH;
  return NULL;
}
static int hasAppLocalValue(char *key) { return (key && strcmp(key, "STORAGE_ROOT") == 0) ? 1 : 0; }
static void setAppLocalValue(char *k, char *v) { fprintf(stderr, "[jni] setAppLocalValue %s=%s\n", k?k:"?", v?v:"?"); }
static char *getParameter(char *key) { return NULL; }
static char *FileGetArchiveName(int type) {
  if (type == 1) return (char *)"main.obb";
  if (type == 2) return (char *)"patch.obb";
  return NULL;
}
/* Tipo do pad define o MAPA DE ACOES interno (BBI->botao) do engine:
 * 0/5/6=XBOX360 4=MogaPocket 7=MogaPro 8=PS3 9=iOSExtended 10=iOSSimple.
 * BULLY_PAD_TYPE p/ experimentar (default 8=PS3, historico do port). */
static int GetGamepadType(int port) {
  if (port != 0) return -1;
  static int t = -99;
  if (t == -99) { const char *e = getenv("BULLY_PAD_TYPE"); t = e ? atoi(e) : 8;
    fprintf(stderr, "[pad] GetGamepadType=%d\n", t); }
  return t;
}
/* Hotkey universal de SAIR (SELECT+START) — funciona em qualquer device, sem
 * depender de gptokeyb/set_kill (que varia por CFW). Chamado do pump_gamepad
 * (todo frame; o jogo usa eventos, nao polling) E do GetGamepadButtons (poll).
 * _exit imediato evita o deadlock do blob Mali (Valhall/Utgard) ao liberar o
 * contexto GL no encerramento. */
static void check_exit_hotkey(void) {
  if (g_pad &&
      SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_BACK) &&
      SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_START)) {
    fprintf(stderr, "[pad] SELECT+START -> saindo do jogo\n");
    _exit(0);
  }
}
/* ---- estado do modo gptokeyb (usado nos DOIS caminhos: eventos E poll) ---- */
static int g_gptk = -1;
static int gptk_mode(void) {
  if (g_gptk < 0) {
    const char *e = getenv("BULLY_INPUT");
    g_gptk = (e && strcmp(e, "gptk") == 0) ? 1 : 0;
    if (g_gptk) fprintf(stderr, "[pad] modo GPTOKEYB (teclado/mouse, layout PS2 via bully.gptk)\n");
  }
  return g_gptk;
}
static unsigned char g_kb[SDL_NUM_SCANCODES];
static int g_mxrel, g_myrel;

/* INJEÇÃO de botão headless: `echo <hexmask> > /dev/shm/gtasa_btn` -> segura a
 * máscara ~8 frames (p/ dirigir o menu sem controle físico via ssh). Máscara
 * layout Vita/NX: 0x1=Cruz(confirmar) 0x2=Circulo(voltar) 0x10=START 0x100=cima
 * 0x200=baixo 0x400=esq 0x800=dir. Ex: `echo 10 >` = START, `echo 1 >` = Cruz. */
static int gtasa_inject_mask(void) {
  static int hold = 0, mask = 0, fc = 0;
  if (hold > 0) { if (--hold == 0) mask = 0; return mask; }
  if (++fc % 3 == 0) {
    FILE *f = fopen("/dev/shm/gtasa_btn", "r");
    if (f) { int m = 0; if (fscanf(f, "%x", &m) == 1 && m) { mask = m; hold = 8;
             fprintf(stderr, "[inject] mask=0x%x\n", m); } fclose(f); unlink("/dev/shm/gtasa_btn"); }
  }
  return hold > 0 ? mask : 0;
}
static int GetGamepadButtons(int port) {
  if (port != 0) return 0;
  if (gptk_mode()) {
    /* POLL normalizado: mascara (layout Vita/NX) derivada do TECLADO do
     * gptokeyb — fonte UNICA de botoes; o pad fisico nao entra mais aqui.
     * NAV up/down (zoom) ficam FORA da mascara, igual bully-NX. */
    int m = 0;
    if (g_kb[SDL_SCANCODE_X])      m |= 0x1;    /* Cruz */
    if (g_kb[SDL_SCANCODE_C])      m |= 0x2;    /* Circulo */
    if (g_kb[SDL_SCANCODE_Q])      m |= 0x4;    /* Quadrado */
    if (g_kb[SDL_SCANCODE_T])      m |= 0x8;    /* Triangulo */
    if (g_kb[SDL_SCANCODE_RETURN]) m |= 0x10;   /* START */
    if (g_kb[SDL_SCANCODE_ESCAPE]) m |= 0x20;   /* SELECT */
    if (g_kb[SDL_SCANCODE_H])      m |= 0x40;   /* L1 */
    if (g_kb[SDL_SCANCODE_J])      m |= 0x80;   /* R1 */
    if (g_kb[SDL_SCANCODE_LEFT])   m |= 0x400;
    if (g_kb[SDL_SCANCODE_RIGHT])  m |= 0x800;
    if (g_kb[SDL_SCANCODE_N])      m |= 0x1000; /* L3 */
    if (g_kb[SDL_SCANCODE_M])      m |= 0x2000; /* R3 */
    static int plog = 0, lastm = -1;
    if (plog < 5) { fprintf(stderr, "[poll] GetGamepadButtons consultado m=0x%x\n", m); plog++; }
    if (m != lastm) { fprintf(stderr, "[poll] mask=0x%x\n", m); lastm = m; }
    return m;
  }
  if (!g_pad) return gtasa_inject_mask();  /* sem pad: ainda aceita injeção via /dev/shm */
  SDL_GameControllerUpdate();
  check_exit_hotkey();
  int m = 0;
  struct { int b; int mask; } map[] = {
    {SDL_CONTROLLER_BUTTON_A,0x1},{SDL_CONTROLLER_BUTTON_B,0x2},
    {SDL_CONTROLLER_BUTTON_X,0x4},{SDL_CONTROLLER_BUTTON_Y,0x8},
    {SDL_CONTROLLER_BUTTON_START,0x10},{SDL_CONTROLLER_BUTTON_BACK,0x20},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,0x40},{SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,0x80},
    {SDL_CONTROLLER_BUTTON_DPAD_UP,0x100},{SDL_CONTROLLER_BUTTON_DPAD_DOWN,0x200},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT,0x400},{SDL_CONTROLLER_BUTTON_DPAD_RIGHT,0x800},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK,0x1000},{SDL_CONTROLLER_BUTTON_RIGHTSTICK,0x2000},
  };
  for (unsigned i = 0; i < sizeof(map)/sizeof(map[0]); i++)
    if (SDL_GameControllerGetButton(g_pad, map[i].b)) m |= map[i].mask;
  static int last_m = 0, calls = 0;
  if (calls < 3) { fprintf(stderr, "[pad] GetGamepadButtons CHAMADO (poll #%d) m=0x%x\n", calls, m); calls++; }
  if (m != last_m) { fprintf(stderr, "[pad] buttons=0x%x\n", m); last_m = m; }
  return m | gtasa_inject_mask();  /* injeção headless (/dev/shm/gtasa_btn) OR pad físico */
}
static float GetGamepadAxis(int port, int axis) {
  if (port != 0) return 0.0f;
  if (gptk_mode()) {
    /* AIM/FIRE moram nos EIXOS 4/5 (LT/RT analogicos, estilo Rockstar
     * mobile/360) — comprovado: a mira so funciona com o eixo em 1.0.
     * k=mira(eixo4) l=tiro(eixo5). Sticks: pad fisico se visivel. */
    static int alog = 0;
    if (alog < 10 && (axis == 4 || axis == 5)) { fprintf(stderr, "[poll] GetGamepadAxis(%d) consultado\n", axis); alog++; }
    if (axis == 4) return g_kb[SDL_SCANCODE_K] ? 1.0f : 0.0f;
    if (axis == 5) return g_kb[SDL_SCANCODE_L] ? 1.0f : 0.0f;
    if (!g_pad) {
      switch (axis) {
        case 0: return (g_kb[SDL_SCANCODE_D]?1.0f:0.0f) - (g_kb[SDL_SCANCODE_A]?1.0f:0.0f);
        case 1: return (g_kb[SDL_SCANCODE_S]?1.0f:0.0f) - (g_kb[SDL_SCANCODE_W]?1.0f:0.0f);
      }
      return 0.0f;
    }
  }
  if (!g_pad) return 0.0f;
  SDL_GameControllerAxis ax[] = {SDL_CONTROLLER_AXIS_LEFTX,SDL_CONTROLLER_AXIS_LEFTY,
    SDL_CONTROLLER_AXIS_RIGHTX,SDL_CONTROLLER_AXIS_RIGHTY,
    SDL_CONTROLLER_AXIS_TRIGGERLEFT,SDL_CONTROLLER_AXIS_TRIGGERRIGHT};
  if (axis < 0 || axis > 5) return 0.0f;
  float v = SDL_GameControllerGetAxis(g_pad, ax[axis]) / 32768.0f;
  return fabsf(v) > 0.25f ? v : 0.0f;
}

/* ==== modo GPTOKEYB (BULLY_INPUT=gptk, setado pelo launcher) ===============
 * Padrao PortMaster: o gptokeyb de CADA CFW le o controle fisico (normalizado
 * pelo control.txt do device) e emite TECLADO/MOUSE via uinput, conforme o
 * bully.gptk (layout PS2). O binario le essas teclas e entrega pro jogo os
 * MESMOS eventos JNI de sempre — o mapeamento sai do binario e vai pro .gptk.
 *
 *   tecla -> botao:  x=Cruz c=Circulo q=Quadrado t=Triangulo enter=START
 *                    esc=SELECT h=L1 j=R1 k=L2 l=R2 n=L3 m=R3 setas=dpad
 *   sticks: se o pad estiver visivel pro SDL (gptokeyb nao da grab), os EIXOS
 *           continuam ANALOGICOS direto do pad (gradiente andar/correr).
 *           Senao, fallback digital: wasd=stick esq, mouse rel=stick dir.
 *   sair:   SELECT+START (esc+enter) — mesmo combo de todo device.        */
void gptk_event(void *ev) { /* chamado do loop de eventos do main */
  SDL_Event *e = (SDL_Event *)ev;
  if (e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) {
    int sc = e->key.keysym.scancode;
    if (sc >= 0 && sc < SDL_NUM_SCANCODES) g_kb[sc] = (e->type == SDL_KEYDOWN);
    static int klog = 0;
    if (klog < 120) { fprintf(stderr, "[kbd] %s sc=%d (%s)\n",
        e->type == SDL_KEYDOWN ? "DOWN" : "UP  ", sc, SDL_GetScancodeName(sc)); klog++; }
  } else if (e->type == SDL_MOUSEMOTION) {
    g_mxrel += e->motion.xrel; g_myrel += e->motion.yrel;
  }
}
static void pump_gptk(void) {
  static void (*down)(void*,void*,int,int) = NULL;
  static void (*up)(void*,void*,int,int) = NULL;
  static void (*axesfn)(void*,void*,int,float,float,float,float,float,float) = NULL;
  static void (*countfn)(void*,void*,int) = NULL;
  static int inited = 0, last[20] = {0};
  static float la[6] = {0}, cam_x = 0, cam_y = 0, sens = 0;
  if (!inited) {
#define GP(n) (void*)so_symbol(&mod_game, "Java_com_rockstargames_oswrapper_GameNative_" n)
    down = GP("implOnGamepadButtonDown"); up = GP("implOnGamepadButtonUp");
    axesfn = GP("implOnGamepadAxesChanged"); countfn = GP("implOnGamepadCountChanged");
#undef GP
    if (countfn) countfn(fake_env, NULL, 1); /* avisa: 1 controle conectado */
    inited = 1;
  }
  /* SAIR: SELECT+START (esc+enter vindos do gptokeyb) */
  if (g_kb[SDL_SCANCODE_ESCAPE] && g_kb[SDL_SCANCODE_RETURN]) {
    fprintf(stderr, "[pad] SELECT+START (gptk) -> saindo do jogo\n");
    _exit(0);
  }
  /* TAP sintetico (AND_TouchEvent): calibracao via `echo "x y" >
   * /dev/shm/bully_tap` OU teclas f/g -> toque nos botoes touch de troca de
   * item do HUD (coords BULLY_TAP_PREV/BULLY_TAP_NEXT="x,y"). E o unico jeito
   * de trocar item no build mobile: a acao BBI_Next/PrevWeapon so existe no
   * touch (nenhum dos 20 eventos de gamepad cicla itens — sondados todos). */
  {
    static void (*touchfn)(int,int,int,int) = NULL;
    static int t_init = 0, tap_hold = -1, tap_x, tap_y, tframes = 0;
    static int px = -1, py = -1, nx = -1, ny = -1, lastf = 0, lastg = 0;
    if (!t_init) {
      touchfn = (void*)so_symbol(&mod_game, "_Z14AND_TouchEventiiii");
      /* slot de arma do HUD touch (calibrado por screenshot no X5M 1080p):
       * zona sup-esq = item ANTERIOR, zona inf-dir = PROXIMO. Coordenadas
       * RELATIVAS a resolucao (1288,923)/(1320,958) @1920x1080. */
      int w = bully_screen_w(), h = bully_screen_h();
      px = w * 1288 / 1920; py = h * 923 / 1080;
      nx = w * 1320 / 1920; ny = h * 958 / 1080;
      const char *e;
      if ((e = getenv("BULLY_TAP_PREV"))) sscanf(e, "%d,%d", &px, &py);
      if ((e = getenv("BULLY_TAP_NEXT"))) sscanf(e, "%d,%d", &nx, &ny);
      fprintf(stderr, "[tap] AND_TouchEvent=%p prev=%d,%d next=%d,%d\n", (void*)touchfn, px, py, nx, ny);
      t_init = 1;
    }
    if (touchfn) {
      if (tap_hold > 0) {
        if (--tap_hold == 0) { touchfn(1, 0, tap_x, tap_y); tap_hold = -1; }
      } else {
        int f = g_kb[SDL_SCANCODE_F] ? 1 : 0, g = g_kb[SDL_SCANCODE_G] ? 1 : 0;
        if (f && !lastf && px >= 0) { tap_x = px; tap_y = py; touchfn(2, 0, px, py); tap_hold = 8; }
        else if (g && !lastg && nx >= 0) { tap_x = nx; tap_y = ny; touchfn(2, 0, nx, ny); tap_hold = 8; }
        else if (++tframes % 10 == 0) {
          FILE *tf = fopen("/dev/shm/bully_tap", "r");
          if (tf) {
            int x = -1, y = -1;
            if (fscanf(tf, "%d %d", &x, &y) == 2 && x >= 0) {
              tap_x = x; tap_y = y; touchfn(2, 0, x, y); tap_hold = 8;
              fprintf(stderr, "[tap] %d,%d\n", x, y);
            }
            fclose(tf); unlink("/dev/shm/bully_tap");
          }
        }
        lastf = f; lastg = g;
      }
    }
  }
  /* SONDA de enums (mapeamento empirico do GamepadButton do libGame):
   * `echo N > /dev/shm/bully_btn` via ssh -> dispara down/up do enum N
   * (segura ~0.5s). Sem o arquivo, custo = 1 fopen a cada 15 frames. */
  {
    static int pframes = 0, phold = -1, pbtn = -1;
    if (phold >= 0) {
      if (--phold == 0) { if (up) up(fake_env, NULL, 0, pbtn); fprintf(stderr, "[probe] enum %d UP\n", pbtn); phold = -1; }
    } else if (++pframes % 15 == 0) {
      FILE *pf = fopen("/dev/shm/bully_btn", "r");
      if (pf) {
        int b = -1; if (fscanf(pf, "%d", &b) != 1) b = -1; fclose(pf);
        unlink("/dev/shm/bully_btn");
        if (b >= 0 && b < 20) { pbtn = b; if (down) down(fake_env, NULL, 0, b); fprintf(stderr, "[probe] enum %d DOWN\n", b); phold = 30; }
      }
    }
  }
  /* INJECAO DE MOVIMENTO SUSTENTADO (teste autonomo, sem pad fisico): escrever as teclas a
   * SEGURAR neste frame em /dev/shm/bully_hold -> ex `echo wd > /dev/shm/bully_hold` anda+vira.
   * Arquivo PRESENTE = autoritativo sobre as teclas injetaveis (ausente = controle manual).
   * Parar = `echo > bully_hold` (esvazia) ou `rm`. Cobre roaming p/ stress de streaming. */
  {
    static int hframes = 0;
    if (++hframes % 2 == 0) {
      static const struct { char ch; int sc; } hmap[] = {
        {'w',SDL_SCANCODE_W},{'s',SDL_SCANCODE_S},{'a',SDL_SCANCODE_A},{'d',SDL_SCANCODE_D},
        {'x',SDL_SCANCODE_X},{'c',SDL_SCANCODE_C},{'r',SDL_SCANCODE_RETURN},{'e',SDL_SCANCODE_ESCAPE},
        {'1',SDL_SCANCODE_UP},{'2',SDL_SCANCODE_DOWN},{'3',SDL_SCANCODE_LEFT},{'4',SDL_SCANCODE_RIGHT},
      };
      FILE *hf = fopen("/dev/shm/bully_hold", "r");
      if (hf) {
        char buf[32]; size_t n = fread(buf, 1, sizeof buf - 1, hf); buf[n] = '\0'; fclose(hf);
        for (unsigned i = 0; i < sizeof(hmap)/sizeof(hmap[0]); i++)
          g_kb[hmap[i].sc] = strchr(buf, hmap[i].ch) ? 1 : 0;
      }
    }
  }
  /* botoes: SEMPRE do teclado (e o que o gptokeyb padroniza por device).
   * Enum REAL do libGame (fonte: bully-NX): 0-3=face 4=START 5=BACK
   * 6/7=L3/R3 "novos" (NAO usar; 6 dispara lock-on), 8-11=NAV(zoom/tasks),
   * 12-15=DPAD legado do gameplay -> 12=olhar p/ tras 13=AGACHAR (manual
   * PS2: L3/R3), 16=L1 17=L2 18=R1 19=R2. */
  /* TODAS as 20 acoes do engine tem tecla fixa -> remapear = so editar o
   * bully.gptk (sem rebuild). Teclas: x c q t (face) enter esc (start/sel)
   * u i (6/7 L3/R3 "novos") setas (NAV 8-11) n m f g (DPAD legado 12-15:
   * olhar-tras/agachar/item-esq/item-dir) h k j l (16-19 LB/LT/RB/RT). */
  static const struct { int sc; int game; } kmap[] = {
    {SDL_SCANCODE_X,0},{SDL_SCANCODE_C,1},{SDL_SCANCODE_Q,2},{SDL_SCANCODE_T,3},
    {SDL_SCANCODE_RETURN,4},{SDL_SCANCODE_ESCAPE,5},
    {SDL_SCANCODE_U,6},{SDL_SCANCODE_I,7},
    {SDL_SCANCODE_UP,8},{SDL_SCANCODE_DOWN,9},{SDL_SCANCODE_LEFT,10},{SDL_SCANCODE_RIGHT,11},
    {SDL_SCANCODE_N,12},{SDL_SCANCODE_M,13},{SDL_SCANCODE_F,14},{SDL_SCANCODE_G,15},
    {SDL_SCANCODE_H,16},{SDL_SCANCODE_K,17},{SDL_SCANCODE_J,18},{SDL_SCANCODE_L,19},
  };
  for (unsigned i = 0; i < sizeof(kmap)/sizeof(kmap[0]); i++) {
    int g = kmap[i].game, p = g_kb[kmap[i].sc] ? 1 : 0;
    if (p != last[g]) {
      static int elog = 0;
      if (elog < 80) { fprintf(stderr, "[evt] %s enum %d\n", p ? "DOWN" : "UP  ", g); elog++; }
      if (p) { if (down) down(fake_env, NULL, 0, g); }
      else   { if (up)   up(fake_env, NULL, 0, g); }
      last[g] = p;
    }
  }
  /* eixos */
  float a[6];
  if (g_pad) {
    /* pad visivel: sticks ANALOGICOS direto (gptokeyb nao deu grab) */
    SDL_GameControllerUpdate();
    a[0] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX)/32768.0f;
    a[1] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY)/32768.0f;
    a[2] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX)/32768.0f;
    a[3] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY)/32768.0f;
    g_mxrel = g_myrel = 0;
  } else {
    /* pad invisivel (grab do gptokeyb): wasd digital + mouse rel = camera */
    a[0] = (g_kb[SDL_SCANCODE_D] ? 1.0f : 0.0f) - (g_kb[SDL_SCANCODE_A] ? 1.0f : 0.0f);
    a[1] = (g_kb[SDL_SCANCODE_S] ? 1.0f : 0.0f) - (g_kb[SDL_SCANCODE_W] ? 1.0f : 0.0f);
    if (sens == 0) { const char *e = getenv("BULLY_MOUSE_SENS"); sens = e ? atof(e) : 0.09f; if (sens <= 0) sens = 0.09f; }
    float tx = g_mxrel * sens, ty = g_myrel * sens;
    g_mxrel = g_myrel = 0;
    if (tx > 1) tx = 1; if (tx < -1) tx = -1;
    if (ty > 1) ty = 1; if (ty < -1) ty = -1;
    cam_x = cam_x * 0.5f + tx * 0.5f; cam_y = cam_y * 0.5f + ty * 0.5f; /* suaviza + decai */
    if (fabsf(cam_x) < 0.02f) cam_x = 0;
    if (fabsf(cam_y) < 0.02f) cam_y = 0;
    a[2] = cam_x; a[3] = cam_y;
  }
  a[4] = g_kb[SDL_SCANCODE_K] ? 1.0f : 0.0f; /* MIRA = eixo 4 (LT) em 1.0 */
  a[5] = g_kb[SDL_SCANCODE_L] ? 1.0f : 0.0f; /* TIRO = eixo 5 (RT) em 1.0 */
  int ch = 0;
  for (int i = 0; i < 6; i++) if (fabsf(a[i] - la[i]) > 0.02f) { ch = 1; break; }
  if (ch && axesfn) { axesfn(fake_env, NULL, 0, a[0],a[1],a[2],a[3],a[4],a[5]); for (int i = 0; i < 6; i++) la[i] = a[i]; }
}

/* ---- pump de eventos de controle (o jogo NÃO faz polling; usa eventos JNI,
 * igual bully-NX). GamepadButton enum do libGame: 0=A 1=B 2=X 3=Y 4=START
 * 5=BACK 6=L3 7=R3 8-11=NAV(menu) 12-15=DPAD 16=LB 17=LT 18=RB 19=RT. ---- */
static const struct { int sdl; int game; } g_btnmap[] = {
  {SDL_CONTROLLER_BUTTON_A,0},{SDL_CONTROLLER_BUTTON_B,1},
  {SDL_CONTROLLER_BUTTON_X,2},{SDL_CONTROLLER_BUTTON_Y,3},
  {SDL_CONTROLLER_BUTTON_START,4},{SDL_CONTROLLER_BUTTON_BACK,5},
  {SDL_CONTROLLER_BUTTON_LEFTSTICK,6},{SDL_CONTROLLER_BUTTON_RIGHTSTICK,7},
  {SDL_CONTROLLER_BUTTON_DPAD_UP,8},{SDL_CONTROLLER_BUTTON_DPAD_DOWN,9},
  {SDL_CONTROLLER_BUTTON_DPAD_LEFT,10},{SDL_CONTROLLER_BUTTON_DPAD_RIGHT,11},
  {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,16},{SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,18},
};
static void pump_gamepad(void) {
  static void (*down)(void*,void*,int,int) = NULL;
  static void (*up)(void*,void*,int,int) = NULL;
  static void (*axesfn)(void*,void*,int,float,float,float,float,float,float) = NULL;
  static void (*countfn)(void*,void*,int) = NULL;
  static int inited = 0, last[20] = {0};
  static float la[6] = {0};
  if (!g_pad) return;
  if (!inited) {
#define GP(n) (void*)so_symbol(&mod_game, "Java_com_rockstargames_oswrapper_GameNative_" n)
    down = GP("implOnGamepadButtonDown"); up = GP("implOnGamepadButtonUp");
    axesfn = GP("implOnGamepadAxesChanged"); countfn = GP("implOnGamepadCountChanged");
#undef GP
    if (countfn) countfn(fake_env, NULL, 1); /* avisa: 1 controle conectado */
    inited = 1;
  }
  SDL_GameControllerUpdate();
  check_exit_hotkey();
  /* botões */
  for (unsigned i = 0; i < sizeof(g_btnmap)/sizeof(g_btnmap[0]); i++) {
    int g = g_btnmap[i].game;
    int p = SDL_GameControllerGetButton(g_pad, g_btnmap[i].sdl) ? 1 : 0;
    if (p != last[g]) {
      if (p) { if (down) down(fake_env, NULL, 0, g); }
      else   { if (up)   up(fake_env, NULL, 0, g); }
      last[g] = p;
    }
  }
  /* gatilhos como botões 17(LT)/19(RT) */
  int lt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 12000 ? 1 : 0;
  int rt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000 ? 1 : 0;
  if (lt != last[17]) { if (lt) { if(down) down(fake_env,NULL,0,17);} else if(up) up(fake_env,NULL,0,17); last[17]=lt; }
  if (rt != last[19]) { if (rt) { if(down) down(fake_env,NULL,0,19);} else if(up) up(fake_env,NULL,0,19); last[19]=rt; }
  /* eixos (sticks) */
  float a[6];
  a[0] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX)/32768.0f;
  a[1] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY)/32768.0f;
  a[2] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX)/32768.0f;
  a[3] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY)/32768.0f;
  a[4] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)/32768.0f;
  a[5] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)/32768.0f;
  int ch = 0;
  for (int i = 0; i < 6; i++) if (fabsf(a[i]-la[i]) > 0.02f) { ch = 1; break; }
  if (ch && axesfn) { axesfn(fake_env, NULL, 0, a[0],a[1],a[2],a[3],a[4],a[5]); for(int i=0;i<6;i++) la[i]=a[i]; }
}

/* ---- dispatchers JNI ---- */
static int GetMethodID(void *e, void *c, const char *name, const char *sig) {
  for (unsigned i = 0; i < sizeof(method_ids)/sizeof(method_ids[0]); i++)
    if (strcmp(name, method_ids[i].name) == 0) return method_ids[i].id;
  /* GTA SA (Vita): método desconhecido -> UNKNOWN(0). O jogo faz `if(methodID)`
     e simplesmente pula a chamada (os 13 métodos que ele usa estão todos na
     tabela). NÃO usar o hack 0x7777 do Bully aqui. */
  return 0;
}
static int CallBooleanMethodV(void *e, void *o, int id, va_list a) {
  switch (id) {
    case INIT_EGL_AND_GLES2: return InitEGLAndGLES2();
    case SWAP_BUFFERS: return swapBuffers();
    case MAKE_CURRENT: return bully_make_current();
    case UN_MAKE_CURRENT: bully_release_current(); return 1;
    case HAS_APP_LOCAL_VALUE: return hasAppLocalValue(va_arg(a, char *));
    case DELETE_FILE: return 0;
  }
  return 0;
}
static float CallFloatMethodV(void *e, void *o, int id, va_list a) {
  if (id == GET_GAMEPAD_AXIS) { int p = va_arg(a, int); int ax = va_arg(a, int); return GetGamepadAxis(p, ax); }
  return 0.0f;
}
static int CallIntMethodV(void *e, void *o, int id, va_list a) {
  switch (id) {
    case GET_GAMEPAD_TYPE: return GetGamepadType(va_arg(a, int));
    case GET_GAMEPAD_BUTTONS: return GetGamepadButtons(va_arg(a, int));
    case GET_DEVICE_TYPE: return GetDeviceType();
    case GET_DEVICE_INFO: case GET_DEVICE_LOCALE: return 0;
  }
  return 0;
}
static void *CallObjectMethodV(void *e, void *o, int id, va_list a) {
  switch (id) {
    case GET_APP_LOCAL_VALUE: { char *r = getAppLocalValue(va_arg(a, char *)); return r ? r : (void*)""; }
    case GET_PARAMETER: { char *r = getParameter(va_arg(a, char *)); return r ? r : (void*)""; }
    case FILE_GET_ARCHIVE_NAME: { char *r = FileGetArchiveName(va_arg(a, int)); return r ? r : (void*)""; }
  }
  return (void*)"";  /* string vazia em vez de NULL: evita strlen(NULL) no jogo */
}
volatile int g_rk_pending_initial = 0, g_rk_pending_gate = 0, g_rk_pending_gate_type = 0;
static void CallVoidMethodV(void *e, void *o, int id, va_list a) {
  if (id == SET_APP_LOCAL_VALUE) { char *k = va_arg(a, char *); char *v = va_arg(a, char *); setAppLocalValue(k, v); }
  else if (id == ROCKSTAR_SHOW_INITIAL) { g_rk_pending_initial = 1; fprintf(stderr, "[jni] rockstarShowInitial -> pending\n"); }
  else if (id == ROCKSTAR_SHOW_GATE) { g_rk_pending_gate_type = va_arg(a, int); g_rk_pending_gate = 1; fprintf(stderr, "[jni] rockstarShowGate -> pending\n"); }
}
static void *FindClass(void *e, const char *n) { return (void *)0x41414141; }
static void *NewGlobalRef(void *e, void *o) { return o ? o : (void *)0x42424242; }
static char *NewStringUTF(void *e, char *b) { return b ? b : (char *)""; }
static char *GetStringUTFChars(void *e, char *s, int *c) { if (c) *c = 0; return s ? s : (char *)""; }
/* CÓPIA ESTÁVEL da tabela de natives: o jogo (GTA SA) passa um array de
 * JNINativeMethod que vive na STACK do JNI_OnLoad (`memcpy(sp, tabela, 312);
 * RegisterNatives(env,cls,sp,13)`). Depois que JNI_OnLoad RETORNA, essa stack é
 * reciclada (o próximo fprintf a sobrescreve) -> ler natives[0].fn dava lixo ->
 * crash. Copiamos os n*24 bytes AGORA (enquanto válido) p/ um buffer estático. */
static char natives_buf[128 * 24]; /* 24B/entrada (aarch64: name@0,sig@8,fn@16) */
static void RegisterNatives(void *e, void *cls, void *methods, int n) {
  int cn = n; if (cn < 0) cn = 0; if (cn > 128) cn = 128;
  memcpy(natives_buf, methods, (size_t)cn * 24);
  natives = natives_buf;
  fprintf(stderr, "[jni] RegisterNatives: %d metodos (copiados p/ buffer estavel)\n", n);
  struct JNM { const char *name; const char *sig; void *fn; } *m = (void *)natives_buf;
  for (int i = 0; i < cn && i < 8; i++)
    fprintf(stderr, "   [%d] %s %s -> %p\n", i, m[i].name, m[i].sig, m[i].fn);
}
void *NVThreadGetCurrentJNIEnv(void) { return fake_env; }

/* variantes varargs (...Method) — o jogo usa AMBAS; delegam pras ...MethodV */
static void *CallObjectMethod(void *e, void *o, int id, ...) { va_list a; va_start(a, id); void *r = CallObjectMethodV(e, o, id, a); va_end(a); return r; }
static int CallBooleanMethod(void *e, void *o, int id, ...) { va_list a; va_start(a, id); int r = CallBooleanMethodV(e, o, id, a); va_end(a); return r; }
static int CallIntMethod(void *e, void *o, int id, ...) { va_list a; va_start(a, id); int r = CallIntMethodV(e, o, id, a); va_end(a); return r; }
static float CallFloatMethod(void *e, void *o, int id, ...) { va_list a; va_start(a, id); float r = CallFloatMethodV(e, o, id, a); va_end(a); return r; }
static void CallVoidMethod(void *e, void *o, int id, ...) { va_list a; va_start(a, id); CallVoidMethodV(e, o, id, a); va_end(a); }

static int GetEnv(void *vm, void **env, int v) { *env = fake_env; return 0; }
static int AttachCurrentThread(void *vm, void **env, void *args) { *env = fake_env; return 0; }

#define SET(off, fn) *(uintptr_t *)(fake_env + (off)) = (uintptr_t)(fn)
static void build_env(void) {
  /* preenche TUDO com ret0 (qualquer slot JNI nao-tratado retorna 0, sem crash) */
  for (unsigned i = 0; i < sizeof(fake_env)/sizeof(uintptr_t); i++)
    ((uintptr_t *)fake_env)[i] = (uintptr_t)ret0;
  *(uintptr_t *)(fake_env + 0x00) = (uintptr_t)fake_env;
  SET(0x30, FindClass);            /* idx 6 */
  SET(0x88, ret0);                 /* idx 17 ExceptionClear */
  SET(0xA8, NewGlobalRef);         /* idx 21 */
  SET(0xB0, ret0);                 /* idx 22 DeleteGlobalRef */
  SET(0xB8, ret0);                 /* idx 23 DeleteLocalRef */
  SET(0x108, GetMethodID);         /* idx 33 */
  SET(0x110, CallObjectMethod);    /* idx 34 (varargs) */
  SET(0x118, CallObjectMethodV);   /* idx 35 */
  SET(0x128, CallBooleanMethod);   /* idx 37 (varargs) */
  SET(0x130, CallBooleanMethodV);  /* idx 38 */
  SET(0x188, CallIntMethod);       /* idx 49 (varargs) */
  SET(0x190, CallIntMethodV);      /* idx 50 */
  SET(0x1B8, CallFloatMethod);     /* idx 55 (varargs) */
  SET(0x1C0, CallFloatMethodV);    /* idx 56 */
  SET(0x1E8, CallVoidMethod);      /* idx 61 (varargs) */
  SET(0x1F0, CallVoidMethodV);     /* idx 62 */
  SET(0x538, NewStringUTF);        /* idx 167 */
  SET(0x548, GetStringUTFChars);   /* idx 169 */
  SET(0x550, ret0);                /* idx 170 ReleaseStringUTFChars */
  SET(0x6B8, RegisterNatives);     /* idx 215 */
}

void jni_init_input(void) {
  int n = SDL_NumJoysticks();
  fprintf(stderr, "[pad] SDL_NumJoysticks=%d\n", n);
  for (int i = 0; i < n; i++) {
    fprintf(stderr, "[pad]  js%d: \"%s\" isGameController=%d\n",
            i, SDL_JoystickNameForIndex(i), SDL_IsGameController(i));
    if (SDL_IsGameController(i) && !g_pad) {
      g_pad = SDL_GameControllerOpen(i);
      fprintf(stderr, "[pad]  -> abriu como GameController: %s\n", g_pad ? "OK" : SDL_GetError());
    }
  }
  /* fallback: se nenhum tem mapeamento, abre o 1º joystick como pad genérico */
  if (!g_pad && n > 0) {
    SDL_GameControllerAddMapping(
      "03000000000000000000000000000000,USB Gamepad,"
      "a:b2,b:b1,x:b3,y:b0,start:b9,back:b8,"
      "leftshoulder:b4,rightshoulder:b5,"
      "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
      "leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,");
    g_pad = SDL_GameControllerOpen(0);
    fprintf(stderr, "[pad]  fallback genérico: %s\n", g_pad ? "OK" : SDL_GetError());
  }
}

/* ---- NvAPK hooks -> asset_archive (le dos data_*.zip reais) ---- */
extern int   asset_archive_init(void);
extern void *asset_open(const char *path);
extern void  asset_close(void *h);
extern size_t asset_read(void *buf, size_t s, size_t n, void *h);
extern int   asset_seek(void *h, long off, int wh);
extern long  asset_tell(void *h);
extern long  asset_size(void *h);
extern int   asset_eof(void *h);
extern int   asset_getc(void *h);
extern char *asset_gets(char *b, int m, void *h);

static int   nv_init(void *a, void *b, void *c) { asset_archive_init(); return 0; }
/* TESTE escola: stuba sons de aluno (BULLY_NO_CROWD_SND) e/ou os mapas de
 * DETALHE _n/_s (BULLY_TEX_LIGHT) -> corta memória de textura da GPU (o Mali
 * Utgard trava quando a escola cheia de NPCs estoura o limite de textura). */
static int g_no_crowd_snd = -1;
static int g_tex_light = -1;
static int ends_with(const char *s, const char *suf) {
  size_t ls = strlen(s), lf = strlen(suf);
  return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}
static int g_no_nvapk = -1;
static void *nv_open(const char *p) {
  /* GTASA_NO_NVAPK=1: NvAPKOpen retorna NULL -> o NvFOpen do jogo cai no fopen
   * NATIVO (FILE* real em handle+8) -> OS_FileRead/GetPosition/Seek funcionam
   * pelo glibc (ftell/fread/fseek). Sem isso, ReadLine->OS_FileGetPosition faz
   * ftell num handle NvAPK nosso (não-FILE*) -> SIGSEGV nos .met/.dat. Requer os
   * assets acessíveis por symlink relativo ao CWD (o launcher cria). */
  if (g_no_nvapk < 0) g_no_nvapk = getenv("GTASA_NO_NVAPK") ? 1 : 0;
  if (g_no_nvapk) {
    if (p && ends_with(p, ".tex")) { extern void bully_set_tex_path(const char *); bully_set_tex_path(p); }
    return NULL;
  }
  if (g_no_crowd_snd < 0) g_no_crowd_snd = getenv("BULLY_NO_CROWD_SND") ? 1 : 0;
  if (g_tex_light < 0) g_tex_light = getenv("BULLY_TEX_LIGHT") ? 1 : 0;
  if (g_no_crowd_snd && p &&
      (strstr(p, "speech") || strstr(p, "ambs_") || strstr(p, "_chatter") || strstr(p, "crowd"))) {
    return NULL;
  }
  if (g_tex_light && p && (ends_with(p, "_n.tex") || ends_with(p, "_s.tex"))) {
    static int n = 0;
    if (n < 6) { fprintf(stderr, "[nvapk] SKIP detalhe \"%s\" (TEX_LIGHT)\n", p); n++; }
    return NULL;
  }
  /* registra o .tex corrente p/ a sidetable ETC1 (chave = caminho do asset). */
  if (p && ends_with(p, ".tex")) { extern void bully_set_tex_path(const char *); bully_set_tex_path(p); }
  void *h = asset_open(p);
  if (!h) fprintf(stderr, "[nvapk] MISS \"%s\"\n", p ? p : "(null)");
  return h;
}
static int g_nvdbg = 0; /* loga as primeiras N chamadas p/ ver o padrão do loop */
static size_t nv_read(void *buf, size_t s, size_t n, void *h) {
  size_t r = h ? asset_read(buf, s, n, h) : 0;
  if (g_nvdbg < 90) { fprintf(stderr, "[nv] read h=%p s=%zu n=%zu -> %zu\n", h, s, n, r); g_nvdbg++; }
  return r;
}
static int   nv_seek(void *h, long o, int w) {
  int r = h ? asset_seek(h, o, w) : -1;
  if (g_nvdbg < 90) { fprintf(stderr, "[nv] seek h=%p o=%ld w=%d -> %d\n", h, o, w, r); g_nvdbg++; }
  return r;
}
static void  nv_close(void *h) { asset_close(h); }
static long  nv_tell(void *h) {
  long r = h ? asset_tell(h) : -1;
  if (g_nvdbg < 90) { fprintf(stderr, "[nv] tell h=%p -> %ld\n", h, r); g_nvdbg++; }
  return r;
}
static long  nv_size(void *h) {
  long r = h ? asset_size(h) : 0;
  if (g_nvdbg < 90) { fprintf(stderr, "[nv] size h=%p -> %ld\n", h, r); g_nvdbg++; }
  return r;
}
static int   nv_eof(void *h) { return h ? asset_eof(h) : 1; }
static int   nv_getc(void *h) { return h ? asset_getc(h) : -1; }
static char *nv_gets(char *b, int m, void *h) { return h ? asset_gets(b, m, h) : NULL; }

/* EGL surface lifecycle: nos gerenciamos o pbuffer; neutraliza o create/destroy do jogo
 * (no PC o pbuffer nao pode ser destruido/recriado como window surface -> abortava) */
static void and_create_egl(void) { bully_make_current(); }
static void and_destroy_egl(void) { /* no-op */ }
/* rate-limit: este par dispara MILHARES de vezes no gameplay (streaming de
 * textura) e afogava o log -> o fim (freeze) nunca aparecia. Loga só as 1as 30. */
static int g_mc_log = 0;
static void os_thread_makecurrent(void) {
  int ok = bully_make_current();
  if (g_mc_log < 30) { fprintf(stderr, "[gl] OS_ThreadMakeCurrent tid=%lu -> eglMakeCurrent ok=%d\n",
          (unsigned long)pthread_self(), ok); g_mc_log++; }
}
/* SOLTA o NOSSO contexto na thread chamadora — pareia com makecurrent. Sem isso
 * o GameMain segura o ctx (EGL é single-thread) e a render thread falha (ok=0). */
static void os_thread_unmakecurrent(void) {
  bully_release_current();
  if (g_mc_log < 30) { fprintf(stderr, "[gl] OS_ThreadUnmakeCurrent tid=%lu -> released\n",
          (unsigned long)pthread_self()); g_mc_log++; }
}

/* ===== FORCE-RENDER (bake completo): captura a RenderScene ativa ===== */
/* Texturas so decodificam+sobem quando DESENHADAS. Pra bakear TODAS sem jogar, eu
 * desenho cada uma via RenderScene::CreateSpriteComponent. Preciso do ponteiro da
 * cena ativa -> capturo no 1o AddToRenderList (hook que se auto-cura e chama o real). */
void *g_render_scene = NULL;
/* detour PERMANENTE (sem reescrever text em runtime = sem race com a render thread):
 * trampoline = [1a instr original (sub sp,sp,#N, relocavel)] + LDR X17/BR X17 -> addr+4.
 * my_fn captura a cena 1x e chama o original VIA trampoline. */
static void *make_callthrough(uintptr_t addr) {
  unsigned int *t = mmap(NULL, 32, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (t == MAP_FAILED) return NULL;
  t[0] = *(unsigned int *)addr;     /* sub sp,sp,#N (a 1a instr, PC-independente) */
  t[1] = 0x58000051u;               /* LDR X17, #8  -> le o literal em t[3] (PC+8) */
  t[2] = 0xd61f0220u;               /* BR X17 */
  *(unsigned long long *)(t + 3) = (unsigned long long)(addr + 4); /* literal: resto da func */
  __builtin___clear_cache((char *)t, (char *)t + 32);
  return t;
}
/* UM passo do bake-all, RODANDO NA RENDER THREAD (chamado de dentro do Synchronize).
 * Cria o sprite de uma textura -> entra na render-list -> desenhada -> meu hook bakeia
 * o ETC1. Pipeline de 3 frames + DeleteGL pra nao estourar a MMU. */
/* UM sprite reusado: cada passo troca a textura dele (SpriteComponent::Setup) -> ela e
 * desenhada -> meu hook bakeia o ETC1. SEM criar/deletar 14k sprites (era o que crashava
 * por use-after-free no DeleteComponent). Resume via .bake_next. */
static void bakeall_step(void *scene) {
  static int inited = 0, bi = 0, total = 0, donef = 0;
  static void *(*TexRead)(const char *, const char *) = NULL;
  static void *(*CreateSprite)(void *, void *, float, unsigned int) = NULL;
  static void (*Setup)(void *, void *, float, unsigned int) = NULL;
  static void *g_sprite = NULL;
  static char progpath[300];
  extern int bully_texname_count(void); extern const char *bully_texname(int);
  if (!inited) {
    inited = 1;
    TexRead = (void *)so_symbol(&mod_game, "_Z18MadNoRwTextureReadPKcS0_");
    CreateSprite = (void *)so_symbol(&mod_game, "_ZN11RenderScene21CreateSpriteComponentEP9Texture2Df5color");
    Setup = (void *)so_symbol(&mod_game, "_ZN15SpriteComponent5SetupEP9Texture2Df5color");
    total = bully_texname_count();
    const char *cd = getenv("BULLY_ETC1CACHE"); snprintf(progpath, sizeof(progpath), "%s/.bake_next", cd ? cd : ".");
    FILE *pf = fopen(progpath, "r"); if (pf) { if (fscanf(pf, "%d", &bi) != 1) bi = 0; fclose(pf); }
    if (bi < 0) bi = 0;
    fprintf(stderr, "[bakeall] START %d texturas tid=%lu resume@%d Setup=%p\n",
            total, (unsigned long)pthread_self(), bi, (void *)Setup);
  }
  extern int bully_bake_active, bully_bake_cur, bully_bake_total;
  static int runc = 0, chunk = -1, batch = -1; /* FRAÇÃO: texturas/run (sai+resume); LOTE: texturas/frame (rapidez) */
  if (chunk < 0) { const char *e = getenv("BULLY_BAKE_CHUNK"); chunk = e ? atoi(e) : 400; }
  if (batch < 0) { const char *e = getenv("BULLY_BAKE_BATCH"); batch = e ? atoi(e) : 24; }
  if (bi < total && TexRead && CreateSprite && Setup) {
    bully_bake_active = 1;
    for (int k = 0; k < batch && bi < total; k++) {  /* LOTE por frame: o despejo (em glTexSubImage2D) bounda a GPU */
      bully_bake_cur = bi; bully_bake_total = total;
      FILE *pf = fopen(progpath, "w"); if (pf) { fprintf(pf, "%d\n", bi + 1); fclose(pf); } /* resume pula o ruim */
      const char *nm = bully_texname(bi++);
      void *t = nm ? TexRead(nm, NULL) : NULL;
      if (t) {
        if (!g_sprite) g_sprite = CreateSprite(scene, t, 1.0f, 0xFFFFFFFFu); /* cria 1 sprite */
        else Setup(g_sprite, t, 1.0f, 0xFFFFFFFFu);                          /* REUSA: troca a textura */
      }
      if (getenv("BULLY_BAKEDIAG")) {
        static int tok = 0, tnull = 0; if (t) tok++; else tnull++;
        if ((tok + tnull) % 200 == 0) fprintf(stderr, "[bakediag] TexRead ok=%d null=%d (de %d)\n", tok, tnull, tok + tnull);
      }
      if (bi % 250 == 0) fprintf(stderr, "[bakeall] %d/%d\n", bi, total);
      if (chunk > 0 && ++runc >= chunk) {   /* FRAÇÃO concluída -> SAI limpo (libera GPU/RAM); o loop resume @bi */
        fprintf(stderr, "[bakeall] fracao %d texturas (bi=%d/%d) -> SAINDO p/ resume\n", runc, bi, total);
        fflush(NULL); sync(); _exit(0);
      }
    }
  } else if (!donef && bi >= total && total > 0) {
    donef = 1;
    bully_bake_active = 0;
    char dp[300]; const char *cd = getenv("BULLY_ETC1CACHE");
    snprintf(dp, sizeof(dp), "%s/.bake_done", cd ? cd : "."); FILE *df = fopen(dp, "w"); if (df) fclose(df);
    /* CRITICO: o processo de BAKE tem que SAIR aqui. Se ele continua rodando, fica PRETO
     * (nao e um jogo jogavel, era so o force-render) e o launcher fica preso no
     * `timeout 1500 ./bully` por 25 min sem nunca abrir o jogo de verdade ("acaba tudo
     * e fica preto"). Saindo: o timeout retorna -> o loop ve .bake_done -> abre o jogo
     * NORMAL (linha 238 do Bully.sh), que renderiza. [fix 2026-06-20] */
    fprintf(stderr, "[bakeall] FIM (%d texturas) -> SAINDO p/ o launcher abrir o jogo\n", total);
    fflush(NULL); sync();
    _exit(0);
  }
}
/* dirige o bake POR-FRAME (chamado do present, na GL thread) -> varre as texturas mesmo no MENU
 * (sem cena 3D contínua / sem precisar de input). Reusa a g_render_scene que o Synchronize setou 1x. */
void bully_bakeall_tick(void) {
  static int on = -1; if (on < 0) on = getenv("BULLY_BAKEALL") ? 1 : 0;
  if (on && g_render_scene) bakeall_step(g_render_scene);
}
static void (*tramp_sync)(void *) = NULL;
static void my_Synchronize(void *scene) {
  if (!g_render_scene && scene) { g_render_scene = scene; fprintf(stderr, "[render] scene=%p tid=%lu (Synchronize)\n", scene, (unsigned long)pthread_self()); }
  tramp_sync(scene);                                       /* handoff original (na render thread) */
  if (scene && getenv("BULLY_BAKEALL")) bakeall_step(scene); /* bake na MESMA thread = seguro */
}
static void hook_render(void) {
  uintptr_t addr = (uintptr_t)so_symbol(&mod_game, "_ZN11RenderScene11SynchronizeEv");
  if (!addr) { fprintf(stderr, "[render] Synchronize NAO achado\n"); return; }
  tramp_sync = (void (*)(void *))make_callthrough(addr);
  hook_x64(addr, (uintptr_t)my_Synchronize);
  fprintf(stderr, "[render] hook Synchronize @%p tramp=%p\n", (void *)addr, (void *)tramp_sync);
}

/* ===== EXPERIMENTO: religar o DESPEJO de streaming (igual GTA SA) =====
 * Descoberta (estudo 2026-06-21): Bully = mesmo motor GTA/RenderWare, mas o despejo de
 * textura esta STUBADO (CStreaming::MakeSpaceFor com 0 call-sites, GetTotalGraphicsMemory
 * = 512MB fixo, 0 ref-count de TXD). Por isso acumula textura e estoura no 1GB, enquanto o
 * GTA SA despeja por area e segura no full. Aqui hookamos LoadScene (chamado na troca de
 * area) e chamamos RemoveUnusedModelsInLoadedList() (despejo GENTIL: so o que nao esta em
 * uso -> seguro contra "mundo preto"). Mede a memoria de textura antes/depois.
 * Gate: BULLY_EVICT=1 (nao afeta o build estavel). */
extern long long g_texbytes_live;
extern long g_tex_gen, g_tex_del;     /* contadores de glGen/glDelete (imports.c) */
static void (*tramp_loadscene)(void *) = NULL;
static void (*g_RemoveUnused)(void) = NULL;
static void (*g_MakeSpaceFor)(int)  = NULL;
static void (*g_RemoveIslands)(int) = NULL;
static int  (*g_GetTexMemUsed)(void) = NULL;
static void my_LoadScene(void *vec) {
  long long before = g_texbytes_live; long del0 = g_tex_del;
  tramp_loadscene(vec);                 /* LoadScene original (carrega a area nova) */
  if (g_RemoveUnused)  g_RemoveUnused();        /* (1) despejo gentil */
  if (g_RemoveIslands) g_RemoveIslands(0);      /* (2) remove ilhas/setores nao usados */
  if (g_MakeSpaceFor)  g_MakeSpaceFor(0x4000000); /* (3) FORCA liberar ~64MB do buffer de streaming */
  fprintf(stderr, "[evict] LoadScene: tex %lld->%lld MB | glDelete +%ld (total gen=%ld del=%ld)\n",
          before/(1024*1024), g_texbytes_live/(1024*1024),
          g_tex_del - del0, g_tex_gen, g_tex_del);
}
static void hook_evict(void) {
  uintptr_t ls = (uintptr_t)so_symbol(&mod_game, "_ZN10CStreaming9LoadSceneERK7CVector");
  g_RemoveUnused  = (void(*)(void))so_symbol(&mod_game, "_ZN10CStreaming30RemoveUnusedModelsInLoadedListEv");
  g_MakeSpaceFor  = (void(*)(int)) so_symbol(&mod_game, "_ZN10CStreaming12MakeSpaceForEi");
  g_RemoveIslands = (void(*)(int)) so_symbol(&mod_game, "_ZN10CStreaming20RemoveIslandsNotUsedEi");
  g_GetTexMemUsed = (int(*)(void)) so_symbol(&mod_game, "_ZN17TextureHeapHelper27GetCurrentTextureMemoryUsedEv");
  if (!ls) { fprintf(stderr, "[evict] LoadScene NAO achado\n"); return; }
  tramp_loadscene = (void(*)(void*))make_callthrough(ls);
  hook_x64(ls, (uintptr_t)my_LoadScene);
  fprintf(stderr, "[evict] hook LoadScene @%p -> RemoveUnused+RemoveIslands+MakeSpaceFor(64MB) | rm=%p island=%p msf=%p\n",
          (void*)ls, (void*)g_RemoveUnused, (void*)g_RemoveIslands, (void*)g_MakeSpaceFor);
}

static void hook_egl(void) {
  hook_x64(so_symbol(&mod_game, "_Z20AND_CreateEglSurfacev"), (uintptr_t)and_create_egl);
  hook_x64(so_symbol(&mod_game, "_Z21AND_DestroyEglSurfacev"), (uintptr_t)and_destroy_egl);
  hook_x64(so_symbol(&mod_game, "_Z20OS_ThreadMakeCurrentv"), (uintptr_t)os_thread_makecurrent);
  hook_x64(so_symbol(&mod_game, "_Z22OS_ThreadUnmakeCurrentv"), (uintptr_t)os_thread_unmakecurrent);
}

/* ---- hooks de tela/render como FUNÇÃO (bully-NX hooka; nós só setávamos flags
 * srp). GameRenderer::Setup pode dimensionar a textura/fbo pela tela -> se
 * Width/Height retornam 0, a whitetexture sai 0x0 e falha (NULL). ---- */
static int os_screen_w(void) { return bully_screen_w(); }
static int os_screen_h(void) { return bully_screen_h(); }
static int os_can_render(void) { return 1; }
static int os_is_suspended(void) { return 0; }
static void hook_screen(void) {
  hook_x64(so_symbol(&mod_game, "_Z17OS_ScreenGetWidthv"), (uintptr_t)os_screen_w);
  hook_x64(so_symbol(&mod_game, "_Z18OS_ScreenGetHeightv"), (uintptr_t)os_screen_h);
  hook_x64(so_symbol(&mod_game, "_Z16OS_CanGameRenderv"), (uintptr_t)os_can_render);
  hook_x64(so_symbol(&mod_game, "_Z18OS_IsGameSuspendedv"), (uintptr_t)os_is_suspended);
}

/* ---- __cxa_guard: o guard de static do jogo (NDK) pode travar/falhar no
 * ambiente so-loader (futex/pthread) -> statics C++ não inicializam (ex: o
 * registro dos recursos default / whitetexture). Substituímos por uma versão
 * simples correta (Itanium ABI: byte 0 = inicializado), igual bully-NX. ---- */
static int my_cxa_guard_acquire(char *g) { return g && *g == 0; }
static void my_cxa_guard_release(char *g) { if (g) *g = 1; }
static void my_cxa_guard_abort(char *g) { (void)g; }
static void hook_cxa(void) {
  hook_x64(so_symbol(&mod_game, "__cxa_guard_acquire"), (uintptr_t)my_cxa_guard_acquire);
  hook_x64(so_symbol(&mod_game, "__cxa_guard_release"), (uintptr_t)my_cxa_guard_release);
  hook_x64(so_symbol(&mod_game, "__cxa_guard_abort"), (uintptr_t)my_cxa_guard_abort);
}

/* ---- CLARITY (resolution scale) HIGH forcada ----
 * BullySettings::GetResolutionDefault() faz uma VERIFICACAO DE HARDWARE
 * (perfprofile: std::map<ProfileResolution,ResolutionSetting>) e devolve o
 * ResolutionSetting do perfil do aparelho: potente (X5M 4GB) -> 2 (RS_High),
 * fraco (Mali-450 1GB) -> 0 (RS_Low). Por isso a Clarity nao "pega" pelo
 * settings.ini nem persiste -- a engine sobrescreve pela verificacao toda vez.
 * Aqui forcamos o retorno = RS_High em TODOS os devices. Enum confirmado na
 * tabela .rodata 0x5e4834: RS_Low=0, RS_Med=1, RS_High=2.
 * Override: BULLY_CLARITY=low|med|high (default high). */
static int my_GetResolutionDefault(void *self) {
  (void)self;
  const char *e = getenv("BULLY_CLARITY");
  if (e) { if (!strcmp(e, "low")) return 0; if (!strcmp(e, "med")) return 1; }
  return 2; /* RS_High */
}
static void hook_clarity(void) {
  uintptr_t s = so_symbol(&mod_game, "_ZN13BullySettings20GetResolutionDefaultEv");
  /* o build compat (GCC Debian) nao resolve esse simbolo C++ pelo nome (so_symbol=0,
   * embora os outros resolvam) -> fallback pelo OFFSET FIXO no libGame 1.4.311
   * (BuildID 6139a628): o .text comeca no vaddr 0, entao text_base + 0x1034040. */
  if (!s && text_base) s = (uintptr_t)text_base + 0x1034040;
  fprintf(stderr, "[clarity] hook_clarity: s=%p (text_base=%p)\n", (void*)s, (void*)text_base);
  if (s) {
    hook_x64(s, (uintptr_t)my_GetResolutionDefault);
    fprintf(stderr, "[clarity] GetResolutionDefault hooked -> RS_High (verificacao de hw ignorada)\n");
  }
}

/* ---- SHADOWS (e outras linhas) no menu de display ----
 * BullySettings::GetDisplayShadowOption() retorna 0 (linha ESCONDIDA) a nao ser
 * que o tier grafico do device seja >= 3; o Mali-450 real reporta tier baixo ->
 * a opcao some (sobra so a Clarity). Forcamos a exibicao + o range, igual o
 * hook_clarity faz com a resolucao. GetMaxShadowOption define quantos niveis a
 * opcao tem (Off/On). Offsets fixos = vaddr (text comeca em vaddr 0), iguais ao
 * libGame 1.4.311 (BuildID 6139a628). Opt-in: BULLY_SHADOWS_MENU=1.
 * BULLY_SHADOWS_MAX ajusta o nº de niveis (default 1 = Off/On). */
static int my_GetDisplayShadowOption(void *self) { (void)self; return 1; }
static int my_GetMaxShadowOption(void *self) {
  (void)self;
  const char *e = getenv("BULLY_SHADOWS_MAX");
  return e && *e ? atoi(e) : 1;
}
static void hook_shadow_menu(void) {
  if (!getenv("BULLY_SHADOWS_MENU")) return;
  uintptr_t a = so_symbol(&mod_game, "_ZN13BullySettings22GetDisplayShadowOptionEv");
  if (!a && text_base) a = (uintptr_t)text_base + 0x1033ccc;
  uintptr_t b = so_symbol(&mod_game, "_ZN13BullySettings18GetMaxShadowOptionEv");
  if (!b && text_base) b = (uintptr_t)text_base + 0x1033d24;
  if (a) hook_x64(a, (uintptr_t)my_GetDisplayShadowOption);
  if (b) hook_x64(b, (uintptr_t)my_GetMaxShadowOption);
  fprintf(stderr, "[shadows] menu option forced: display=%p max=%p\n", (void *)a, (void *)b);
}

/* ---- DIAG/FORCE do nivel de sombra no render ----
 * GetShadowLevel mapeia o setting -> nivel de render (Off/Low=0, Med=1, High=2);
 * nivel>=1 ativa o shadow-map RTT que crasha o Utgard. BULLY_SHADOW_FORCE=N
 * forca o retorno (reproduz Med/High no boot sem navegar o menu, p/ debugar). */
/* ---- BULLY_SHADOW_FORCE=N: forca o nivel de sombra (0=Off 1=Low 2=Med 3=High)
 * NO LOAD via GetShadowDefault. Medium/High SO funcionam setados no load (o
 * engine cria o shadow-map junto com a cena); mudar AO VIVO no menu de pausa
 * crasha o driver Mali (Utgard) ao alocar o RT no meio do frame. Por isso o
 * menu fica em Off/Low (BULLY_SHADOWS_MAX=1, ambos rodam ao vivo); quem quiser
 * Med/High usa este knob de launcher (aplicado limpo no load). Off por padrao. */
static int g_shadow_force = -2;
static int my_GetShadowDefault(void *self) {
  (void)self;
  return g_shadow_force >= 0 ? g_shadow_force : 0;
}
static void hook_shadow_force(void) {
  const char *e = getenv("BULLY_SHADOW_FORCE");
  if (!e) return;
  g_shadow_force = atoi(e);
  uintptr_t a = so_symbol(&mod_game, "_ZN13BullySettings16GetShadowDefaultEv");
  if (!a && text_base) a = (uintptr_t)text_base + 0x1033f40;
  if (a) hook_x64(a, (uintptr_t)my_GetShadowDefault);
  fprintf(stderr, "[shadows] FORCE setting=%d no load (GetShadowDefault=%p)\n", g_shadow_force, (void *)a);
}

/* ---- ASYNC FILE WORKER (porta do bully-NX) — A PEÇA QUE FALTAVA ----
 * No Android os arquivos carregam ASSÍNCRONO: uma fila (AndroidFile::
 * firstAsyncFile) é avançada por AND_FileUpdated(delta) a cada frame por um
 * worker. Sem ele, os loads de recurso (incl. a whitetexture / default
 * resources) NUNCA completam -> GameRenderer::Setup pega NULL -> crash. */
static void (*g_AND_FileUpdated)(double) = NULL;
static volatile uintptr_t *g_first_async = NULL;
static void *async_file_worker(void *a) {
  (void)a;
  /* GTA SA: chama AND_FileUpdated INCONDICIONALMENTE a cada ~1ms (é como o
   * Android dirige a fila, todo frame). O gate por firstAsyncFile!=0 do bully
   * deixava AMERICAN.GXT travado (o read fica enfileirado mas nunca é drenado). */
  int logn = 0;
  for (;;) {
    if (g_AND_FileUpdated) {
      g_AND_FileUpdated(0.002);
      if (logn < 3) { fprintf(stderr, "[async] AND_FileUpdated tick #%d (firstAsyncFile=%p)\n",
                              logn, g_first_async ? (void *)*g_first_async : NULL); logn++; }
    }
    usleep(1000);
  }
  return NULL;
}
static void start_async_file_worker(void) {
  g_AND_FileUpdated = (void (*)(double))so_symbol(&mod_game, "_Z14AND_FileUpdated");
  g_first_async =
      (volatile uintptr_t *)so_symbol(&mod_game, "_ZN11AndroidFile14firstAsyncFileE");
  fprintf(stderr, "[async] AND_FileUpdated=%p firstAsyncFile=%p\n",
          (void *)g_AND_FileUpdated, (void *)g_first_async);
  if (g_AND_FileUpdated && g_first_async) {
    pthread_t t;
    if (pthread_create(&t, NULL, async_file_worker, NULL) == 0) {
      pthread_detach(t);
      fprintf(stderr, "[async] worker started\n");
    }
  }
}

/* ---- orquestração de thread (porta do bully-NX, adaptada x86_64) ----
 * O engine lê handle[0x69]=running (OS_ThreadIsRunning) e handle[0x28]=pthread_t
 * (OS_ThreadWait). Sem gerenciar isso, o sync de thread quebra e o renderer/
 * init de recursos default (whitetexture) não fica pronto -> GameRenderer::Setup
 * pega ResourceManager::Get<Texture2D>("whitetexture")=NULL -> SIGSEGV.
 * No x86_64 o pthread do host já dá TLS (não precisa do armSetTlsRw do Switch). */
volatile int g_gamemain_alive = 0; /* 0=não iniciou 1=rodando 2=retornou */
typedef struct { unsigned (*func)(void *); void *arg; char *handle; int is_gm; } OsThreadData;

static void *os_thread_entry(void *p) {
  OsThreadData *td = p;
  unsigned (*func)(void *) = td->func;
  void *arg = td->arg;
  char *h = td->handle;
  int gm = td->is_gm;
  free(td);
  if (h) h[0x69] = 1;
  if (gm) g_gamemain_alive = 1;
  int ret = func ? (int)func(arg) : 0;
  if (h) h[0x69] = 0;
  if (gm) g_gamemain_alive = 2;
  return (void *)(intptr_t)ret;
}

static void *my_OS_ThreadLaunch(unsigned (*func)(void *), void *arg, unsigned r2,
                                const char *name, int r4, int prio) {
  (void)r2; (void)r4; (void)prio;
  char *h = calloc(1, 0x400); /* handle grande p/ a struct privada do engine */
  if (!h) return NULL;
  OsThreadData *td = malloc(sizeof(*td));
  td->func = func; td->arg = arg; td->handle = h;
  td->is_gm = (name && strcmp(name, "GameMain") == 0);
  pthread_t t;
  if (pthread_create(&t, NULL, os_thread_entry, td) != 0) { free(td); free(h); return NULL; }
  h[0x69] = 1;                       /* OS_ThreadIsRunning */
  memcpy(h + 0x28, &t, sizeof(t));   /* OS_ThreadWait/join */
  fprintf(stderr, "[thr] OS_ThreadLaunch '%s' -> handle=%p\n", name ? name : "?", (void *)h);
  return h;
}

static void my_OS_ThreadWait(void *thread) {
  if (!thread) return;
  pthread_t t;
  memcpy(&t, (char *)thread + 0x28, sizeof(t));
  pthread_join(t, NULL);
}

/* bypassa o wrapper de thread JNI do jogo (crasha em pthread_getspecific null) */
static int my_NVThreadSpawnJNIThread(long *out, const void *attr, const char *name,
                                     void *(*entry)(void *), void *arg) {
  (void)attr; (void)name;
  if (!entry) return -1;
  pthread_t t;
  int rc = pthread_create(&t, NULL, entry, arg);
  if (rc == 0 && out)
    memcpy(out, &t, sizeof(*out) < sizeof(t) ? sizeof(*out) : sizeof(t));
  return rc;
}

static void hook_threads(void) {
  hook_x64(so_symbol(&mod_game, "_Z15OS_ThreadLaunchPFjPvES_jPKci16OSThreadPriority"),
           (uintptr_t)my_OS_ThreadLaunch);
  hook_x64(so_symbol(&mod_game, "_Z13OS_ThreadWaitPv"), (uintptr_t)my_OS_ThreadWait);
  hook_x64(so_symbol(&mod_game, "_Z22NVThreadSpawnJNIThreadPlPK14pthread_attr_tPKcPFPvS5_ES5_"),
           (uintptr_t)my_NVThreadSpawnJNIThread);
}

static void hook_nvapk(void) {
#define HK(sym, fn) hook_x64(so_symbol(&mod_game, sym), (uintptr_t)(fn))
  HK("_Z9NvAPKInitP8_jobject", nv_init);   /* GTA SA: 1 arg (Bully tinha 3) */
  HK("_Z9NvAPKOpenPKc", nv_open);
  /* NvAPKOpenFromPack não existe no GTA SA (era do Bully) — removido */
  HK("_Z9NvAPKReadPvmmS_", nv_read);
  HK("_Z9NvAPKSeekPvli", nv_seek);
  HK("_Z10NvAPKClosePv", nv_close);
  HK("_Z9NvAPKTellPv", nv_tell);
  HK("_Z9NvAPKSizePv", nv_size);
  HK("_Z8NvAPKEOFPv", nv_eof);
  HK("_Z9NvAPKGetcPv", nv_getc);
  HK("_Z9NvAPKGetsPciPv", nv_gets);
#undef HK
}

/* v8.3: le MemAvailable do /proc/meminfo em MB; -1 se indisponivel (kernel<3.14).
 * Usado pelo despejo anti-OOM p/ disparar por PRESSAO REAL de RAM (raro) em vez
 * de teto fixo de textura (que disparava a cada 2s e estourava o audio no A53). */
static long bully_memavail_mb(void) {
  FILE *mf = fopen("/proc/meminfo", "r");
  if (!mf) return -1;
  char line[160]; long kb = -1;
  while (fgets(line, sizeof line, mf))
    if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
  fclose(mf);
  return kb < 0 ? -1 : kb / 1024;
}
/* ============================================================================
 * DRIVER GTA SAN ANDREAS -- modelo NVIDIA NvEventQueueActivity (NAO o impl* do
 * Bully). Blueprint: fork Vita (TheOfficialFloW/gtasa_vita, MIT) jni_patch.c:
 * jni_load, traduzido p/ aarch64/Linux e conferido contra o binario real.
 *
 * Fluxo (provado estaticamente em libGTASA.so):
 *   IsAndroidPaused = 0
 *   JNI_OnLoad(fake_vm)  -> o jogo chama RegisterNatives(env,cls,methods,13)
 *   natives[0] = metodo "init" (Z)Z = NVEventJNIInit(env, thiz, initGraphics)
 *   init(fake_env, 0, 1) -> spawna NVEventMainLoopThreadFunc via pthread_create
 *                           (glibc REAL) -> roda NVEventAppMain (loop do jogo).
 *   O jogo dirige o proprio loop e nos chama de volta via JNI:
 *     InitEGLAndGLES2 / makeCurrent / swapBuffers (GL) e GetGamepad* (input,
 *     POLL). Por isso NAO dirigimos frames aqui (≠ Bully). main() so mantem o
 *     processo vivo + bombeia eventos SDL (janela/quit).
 *
 * JNINativeMethod aarch64 = 24 bytes {name@0, sig@8, fn@16} (Vita/armv7 = 12B).
 * fake_vm/fake_env: offsets aarch64 = idx*8 (Vita usa idx*4). Ver build_env().
 * ==========================================================================*/

struct JNINativeMethod64 { const char *name; const char *sig; void *fn; };

/* raise/abort do jogo: captura o CALL SITE (return address -> libGame+offset ->
 * addr2line = linha exata da fonte). O motor chama raise(SIGSEGV)/abort em erro
 * fatal (ex: asset ausente/corrompido); sem isto o crash cai no libc sem pista. */
int gtasa_raise(int sig) {
  void *ra = __builtin_return_address(0);
  extern void *text_base;
  unsigned long off = text_base ? (unsigned long)((uintptr_t)ra - (uintptr_t)text_base) : 0;
  fprintf(stderr, "\n[gtasa] *** GAME raise(%d) de ra=%p (libGame+0x%lx) ***\n", sig, ra, off);
  fflush(stderr);
  _exit(70 + sig);
}
void gtasa_abort(void) {
  void *ra = __builtin_return_address(0);
  extern void *text_base;
  unsigned long off = text_base ? (unsigned long)((uintptr_t)ra - (uintptr_t)text_base) : 0;
  fprintf(stderr, "\n[gtasa] *** GAME abort() de ra=%p (libGame+0x%lx) ***\n", ra, off);
  fflush(stderr);
  _exit(99);
}

/* ---- pthread_create: BYPASS do wrapper NVThreadSpawnProc (igual Vita
 * pthread_create_fake) ------------------------------------------------------
 * O jogo lança o loop principal via pthread_create(start=NVThreadSpawnProc,
 * arg) onde arg[8]=NVEventMainLoopThreadFunc. NVThreadSpawnProc depende de TLS
 * de NVThread (pthread_getspecific de uma key própria) que o nosso ambiente
 * fake NUNCA popula -> deref nulo -> SIGSEGV em libGame+0x332080. O Vita evita
 * isso rodando arg[8] DIRETO. Fazemos igual, mas numa thread REAL do glibc
 * (multi-thread do Linux, ≠ Vita single). Só desviamos ESSA thread; as demais
 * (StartThread/áudio) seguem pro pthread_create real. Offsets do libGTASA
 * md5 eb1b906f (NVThreadSpawnProc=0x332040, 1a instr stp x26,x25=0xa9bb67fa). */
#include <dlfcn.h>
typedef void *(*thread_entry_t)(void *);
static int (*g_real_pthread_create)(pthread_t *, const void *, thread_entry_t, void *) = NULL;
int gtasa_pthread_create(pthread_t *th, const void *attr, thread_entry_t start, void *arg) {
  if (!g_real_pthread_create)
    g_real_pthread_create = (void *)dlsym(RTLD_DEFAULT, "pthread_create");
  extern void *text_base;
  uintptr_t nv = (uintptr_t)text_base + 0x332040; /* NVThreadSpawnProc */
  if (text_base && (uintptr_t)start == nv && arg &&
      *(uint32_t *)nv == 0xa9bb67fau) {           /* confirma a 1a instr */
    thread_entry_t entry = *(thread_entry_t *)((char *)arg + 8); /* NVEventMainLoopThreadFunc */
    fprintf(stderr, "[gtasa] pthread_create NVEvent -> bypass NVThreadSpawnProc, entry=%p\n", (void *)entry);
    return g_real_pthread_create(th, attr, entry, arg);
  }
  return g_real_pthread_create(th, attr, start, arg);
}

static int gtasa_ScreenGetWidth(void)  { return bully_screen_w(); }
static int gtasa_ScreenGetHeight(void) { return bully_screen_h(); }
/* callback RpMaterial* f(RpMaterial* mat, void* data): passthrough (retorna o
 * material, continua o ForAllMaterials) SEM tocar env-map. Usado p/ desligar o
 * environment mapping dos veículos (RpMatFX) que crasha no Mali-450. */
static void *gtasa_material_passthrough(void *mat, void *data) { (void)data; return mat; }

/* OS_ThreadSetValue: Vita neutraliza o mutex da RenderQueue (handle+601=0).
 * my_OS_ThreadLaunch/my_OS_ThreadWait/my_NVThreadSpawnJNIThread já estão
 * definidos acima (herdados do bully; offsets do handle 0x28=pthread_t,
 * 0x69=running batem 1:1 com OS_ThreadWait/OS_ThreadIsRunning do GTA SA). */
static void *gtasa_OS_ThreadSetValue(void *rq) { if (rq) *(uint8_t *)((char *)rq + 601) = 0; return NULL; }

/* patch de 1 instrucao (32b) num offset do libGTASA (text ja writable durante o
 * patch_game). Offsets do md5 eb1b906f. */
static void patch32(uintptr_t off, uint32_t insn) {
  extern void *text_base;
  if (text_base) *(uint32_t *)((uintptr_t)text_base + off) = insn;
}
/* hook por-nome SEGURO: se o simbolo nao existe, PULA (nao fatal; nao escreve 0
 * num addr invalido). so_symbol()=so_find_addr() abortaria o processo. */
static void hook_safe(const char *name, uintptr_t dst) {
  uintptr_t a = so_find_addr_safe(name);
  if (a) hook_arm64(a, dst);
  else fprintf(stderr, "[gtasa] hook skip (ausente): %s\n", name);
}

/* patch_game do SA -- SOMENTE hooks por-NOME (portaveis armv7->aarch64). Os
 * hooks por-offset-cru do Vita (fix heli/hydraulics/cutscene/skin) sao
 * Thumb-only e NAO valem no aarch64 -> omitidos (nao afetam a 1a imagem;
 * reintroduzir por-simbolo depois). */
static void patch_game_gtasa(void) {
  uintptr_t a;
  if ((a = so_find_addr_safe("UseCloudSaves"))) *(uint8_t *)a = 0;
  if ((a = so_find_addr_safe("UseTouchSense"))) *(uint8_t *)a = 0;
  if (getenv("GTASA_NO_DETAIL") && (a = so_find_addr_safe("gNoDetailTextures")))
    *(int *)a = 1;

  hook_safe("_Z14IsRemovedTracki",       (uintptr_t)ret0);
  hook_safe("_Z13SaveTelemetryv",        (uintptr_t)ret0);
  hook_safe("_Z13LoadTelemetryv",        (uintptr_t)ret0);
  hook_safe("_Z11updateUsageb",          (uintptr_t)ret0);
  /* AND_FileUpdated NÃO é stubado: o motor NX carrega arquivos ASSÍNCRONO
   * (AndroidFile::firstAsyncFile) e OS_FileRead BLOQUEIA até o read completar.
   * Quem avança a fila é AND_FileUpdated(delta), chamado pelo async worker
   * (start_async_file_worker) — sem ele, os loads (ex: AMERICAN.GXT) travam. */
  hook_safe("_Z17AND_BillingUpdateb",    (uintptr_t)ret0);
  hook_safe("_Z20AND_SystemInitializev", (uintptr_t)ret0);
  hook_safe("_Z13ProcessEventsb",        (uintptr_t)ret0);  /* 0 = NAO sair (1=exit!) */

  hook_safe("_Z24NVThreadGetCurrentJNIEnvv", (uintptr_t)NVThreadGetCurrentJNIEnv);
  hook_safe("_Z17OS_ScreenGetWidthv",  (uintptr_t)gtasa_ScreenGetWidth);
  hook_safe("_Z18OS_ScreenGetHeightv", (uintptr_t)gtasa_ScreenGetHeight);

  /* NÃO pulamos mais AddRussian/AddJapaneseTexture: com GTASA_NO_NVAPK + ci_fopen
   * os .met dessas fontes leem certo, e o dicionário de fontes precisa das TRÊS
   * texturas (senão o texto do menu sai BRANCO). A LÍNGUA continua EN (locale=0);
   * carregar o atlas de fonte ≠ mostrar japonês. */

  /* Threads do engine: substitui OS_ThreadLaunch pela impl LIMPA (pthread glibc
   * rodando func direto). Isso ELIMINA o wrapper NVThreadSpawnProc/ANDRunThread
   * (que dependia de TLS de NVThread não populado -> pthread_kill(self,SIGSEGV)).
   * Só a thread do loop principal (NVEventInit->pthread_create direto) continua
   * pelo bypass em gtasa_pthread_create. Igual Vita (hook OS_ThreadLaunch). */
  hook_safe("_Z15OS_ThreadLaunchPFjPvES_jPKci16OSThreadPriority", (uintptr_t)my_OS_ThreadLaunch);
  hook_safe("_Z13OS_ThreadWaitPv", (uintptr_t)my_OS_ThreadWait);
  hook_safe("_Z17OS_ThreadSetValuePv", (uintptr_t)gtasa_OS_ThreadSetValue);
  hook_safe("_Z22NVThreadSpawnJNIThreadPlPK14pthread_attr_tPKcPFPvS5_ES5_", (uintptr_t)my_NVThreadSpawnJNIThread);

  /* ENV-MAP dos veículos (RpMatFX): RpMatFXMaterialGetEffects lê dado MatFX
   * podre -> SIGSEGV no carregamento do 1º carro (mundo). É só o reflexo/brilho
   * da lataria; desligar (passthrough) destrava o mundo. Opt-out: GTASA_ENVMAP. */
  if (!getenv("GTASA_ENVMAP")) {
    hook_safe("_ZN17CVehicleModelInfo19SetEnvironmentMapCBEP10RpMaterialPv",   (uintptr_t)gtasa_material_passthrough);
    hook_safe("_ZN17CVehicleModelInfo16SetEnvMapCoeffCBEP10RpMaterialPv",      (uintptr_t)gtasa_material_passthrough);
    hook_safe("_ZN17CVehicleModelInfo22SetEnvMapCoeffAtomicCBEP8RpAtomicPv",   (uintptr_t)gtasa_material_passthrough);
  }

  /* ÁUDIO: LIGADO por padrão via bridge OpenSL ES -> SDL_Audio (opensl_shim.c).
   * GTASA_NOAUDIO=1 desliga (stuba PlaySound; slCreateEngine falha -> silêncio
   * sem corromper heap). */
  if (getenv("GTASA_NOAUDIO"))
    hook_safe("_ZN16CAEAudioHardware9PlaySoundEstttssf", (uintptr_t)ret0);

  /* LÍNGUA = INGLÊS (AMERICAN=0). InitialiseLanguage cai no fallback RUSSIAN(5)
   * porque OS_LanguageUserSelected/DeviceRegion (JNI que nosso env não resolve)
   * devolvem lixo. Forçar 0 = menu/HUD em inglês. */
  hook_safe("_Z23OS_LanguageUserSelectedv", (uintptr_t)ret0);
  hook_safe("_Z23OS_LanguageDeviceRegionv", (uintptr_t)ret0);
  /* InitialiseLanguage+0x24 tem `csel w8, w8(=8), w0, ne` — se um FLAG de região
   * (CIS/russo) != 0, ignora o getter e força idx 4->RUSSIAN. Patch p/ `mov w8,w0`
   * (0x2a0003e8): sempre usa OS_LanguageUserSelected (=0) -> AMERICAN. */
  patch32(0x70aa84, 0x2a0003e8);
}

void jni_load(void) {
  fprintf(stderr, "=== driver GTA SA (NVEvent) ===\n");

  /* env/vm falsos (offsets aarch64). build_env preenche o fake_env. */
  build_env();
  for (unsigned i = 0; i < sizeof(fake_vm) / sizeof(uintptr_t); i++)
    ((uintptr_t *)fake_vm)[i] = (uintptr_t)ret0;
  *(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm;             /* aponta p/ si */
  *(uintptr_t *)(fake_vm + 0x20) = (uintptr_t)AttachCurrentThread; /* idx 4 */
  *(uintptr_t *)(fake_vm + 0x30) = (uintptr_t)GetEnv;             /* idx 6 (Vita idx3 armv7) */
  *(uintptr_t *)(fake_vm + 0x38) = (uintptr_t)AttachCurrentThread; /* idx 7 (daemon) */

  /* IsAndroidPaused = 0 (default 1 -> loop do jogo ficaria pausado) */
  { uintptr_t a = so_find_addr_safe("IsAndroidPaused");
    if (a) { *(int *)a = 0; fprintf(stderr, "[gtasa] IsAndroidPaused=0 @%p\n", (void *)a); }
    else fprintf(stderr, "[gtasa] AVISO: IsAndroidPaused ausente\n"); }

  /* GL + input na MAIN; solta o contexto p/ a render thread do jogo pegar via
   * makeCurrent (EGL e per-thread). bully_init_gl e idempotente. */
  if (!bully_init_gl()) fprintf(stderr, "[gtasa] AVISO: bully_init_gl falhou\n");
  if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0)
    fprintf(stderr, "[pad] InitSubSystem: %s\n", SDL_GetError());
  jni_init_input();
  /* IGNORE eventos de joystick no pump: o jogo (thread propria) chama
   * SDL_GameControllerUpdate via GetGamepadButtons -> unico atualizador, sem
   * corrida com o SDL_PumpEvents da main. */
  SDL_GameControllerEventState(SDL_IGNORE);
  SDL_JoystickEventState(SDL_IGNORE);
  bully_release_current();

  /* hooks patcham a TEXT (RX apos finalize) -> writable durante o patch */
  so_make_text_writable();
  patch_game_gtasa();
  hook_nvapk();               /* NvAPK* -> asset_archive (le os data zips/OBB) */
  so_make_text_executable();
  so_flush_caches();
  asset_archive_init();

  /* JNI_OnLoad -> o jogo chama RegisterNatives -> preenche `natives` */
  int (*JNI_OnLoad)(void *, void *) = (void *)so_find_addr_safe("JNI_OnLoad");
  if (!JNI_OnLoad) { fprintf(stderr, "[gtasa] ERRO: JNI_OnLoad ausente\n"); return; }
  fprintf(stderr, "[gtasa] JNI_OnLoad...\n");
  int jver = JNI_OnLoad(fake_vm, NULL);
  fprintf(stderr, "[gtasa] JNI_OnLoad => 0x%x, natives=%p\n", jver, natives);
  if (!natives) { fprintf(stderr, "[gtasa] ERRO: RegisterNatives nao capturou natives\n"); return; }

  /* natives[0] = "init" (Z)Z = NVEventJNIInit(env, thiz, initGraphics).
   * CAPTURA JÁ os eventos de ciclo de vida (natives[7]=setWindowSize,
   * [9]=resumeEvent) porque init() dispara MAIS RegisterNatives (WarGamepad,
   * Billing) que SOBRESCREVEM o natives_buf -> depois de init() esta tabela some. */
  struct JNINativeMethod64 *m = (struct JNINativeMethod64 *)natives;
  for (int i = 0; i < 13; i++)
    fprintf(stderr, "[gtasa]   natives[%d] %s %s -> %p\n", i,
            m[i].name ? m[i].name : "?", m[i].sig ? m[i].sig : "?", (void *)m[i].fn);
  int (*init)(void *env, void *thiz, int init_graphics) = (void *)m[0].fn;
  void (*setWindowSize)(void *, void *, int, int) = (void *)m[7].fn;  /* "(II)V" */
  void (*resumeEvent)(void *, void *)             = (void *)m[9].fn;  /* "()V"   */
  if (!init) { fprintf(stderr, "[gtasa] ERRO: natives[0].fn nulo\n"); return; }

  fprintf(stderr, "[gtasa] init(env,0,1) -> spawna NVEventAppMain...\n");
  int r = init(fake_env, NULL, 1);
  fprintf(stderr, "[gtasa] init retornou %d (jogo rodando em thread propria)\n", r);

  /* worker de I/O assíncrono: avança AndroidFile::firstAsyncFile via
   * AND_FileUpdated -> completa os reads que OS_FileRead enfileira (senão o
   * carregamento de AMERICAN.GXT/gta.dat/etc. BLOQUEIA pra sempre). */
  start_async_file_worker();

  /* CICLO DE VIDA DA ACTIVITY (normalmente vindo do Java NvEventQueueActivity):
   * sem Java, NVEventAppMain fica BLOQUEADO esperando o surface/resume. Injetamos
   * os eventos que a Activity mandaria: setWindowSize(w,h) = surface pronta ->
   * dispara a criação de EGL/GLES no engine; resumeEvent() = foco/resume -> começa
   * a renderizar. Delay curto p/ a thread do loop já estar em NVEventGetNextEvent. */
  usleep(200000);
  if (setWindowSize) {
    fprintf(stderr, "[gtasa] -> setWindowSize(%d,%d)\n", bully_screen_w(), bully_screen_h());
    setWindowSize(fake_env, NULL, bully_screen_w(), bully_screen_h());
  }
  usleep(100000);
  if (resumeEvent) {
    fprintf(stderr, "[gtasa] -> resumeEvent()\n");
    resumeEvent(fake_env, NULL);
  }

  /* KEEP-ALIVE: NVEventAppMain roda em thread propria e nos chama via JNI.
   * main() bombeia SDL (janela/quit) e segura o processo. Input e via poll
   * (GetGamepad*) na thread do jogo -> aqui so tratamos QUIT + hotkey de sair. */
  fprintf(stderr, "[gtasa] -- loop keep-alive / pump SDL --\n");
  extern void gtasa_diag_all_threads(void);
  unsigned long kf = 0;
  for (;;) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) { fprintf(stderr, "[gtasa] SDL_QUIT\n"); _exit(0); }
    }
    check_exit_hotkey();  /* SELECT+START -> _exit(0) (le estado ja atualizado) */
    /* DIAG: se após ~12s (750 frames) ninguém renderizou, dumpa onde cada thread
     * do jogo está travada (opt-out: GTASA_NODIAG). Uma vez só. */
    if (kf == 750 && !getenv("GTASA_NODIAG")) {
      fprintf(stderr, "[gtasa] DIAG: dump de threads (sem render em ~12s)\n");
      gtasa_diag_all_threads();
    }
    kf++;
    SDL_Delay(16);
  }
}
