/*
 * Mega Man 1 Mobile (Cocos2d-x 3.9, Capcom) -> armhf Linux so-loader (Mali-450 fbdev)
 *
 * Módulo ÚNICO: libcocos2dcpp.so (ELF32-ARM, softfp, GNU STL -> host libstdc++).
 * Fluxo JNI-render-driven (igual Chrono Trigger / Cocos2d-x Android):
 *   JNI_OnLoad -> nativeSetApkPath -> nativeSetContext
 *   -> nativeInit(w,h) [cria GLView, cocos_android_app_init, Application::run]
 *   -> loop: nativeRender() [Director::mainLoop] + SDL_GL_SwapWindow
 * Input: SDL teclado/controle -> nativeKeyEvent (o jogo consome EventKeyboard via
 *        APPLET::GetMaskCode); + touch (nativeTouchesBegin/End) p/ VirtualPad.
 *
 * Infra armhf reusada dos so-loaders VERDES (Shantae/RE4/Terraria): so_util ELF32,
 * softfp_shim, imports (shantae_overrides), pthread_bridge, jni_shim, opensles_shim.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <SDL2/SDL.h>

#include "so_util.h"
#include "jni_shim.h"

typedef int jint;
typedef unsigned char jboolean;

#define SO_NAME "libcocos2dcpp.so"
#define GAME_HEAP_MB 224

extern DynLibFunction shantae_overrides[];
extern const int shantae_overrides_count;
extern DynLibFunction revc_pthread_table[];
extern const int revc_pthread_count;
extern void *softfp_resolve(const char *);

/* ---- Android keycodes (nativeKeyEvent) ---- */
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

/* ---- Cocos2d-x JNI entry points ---- */
static jint (*p_JNI_OnLoad)(void *vm, void *reserved);
static void (*nativeSetContext)(void *env, void *thiz, void *ctx, void *assetmgr);
static void (*nativeSetApkPath)(void *env, void *thiz, void *apkPath);
static void (*nativeInit)(void *env, void *thiz, int w, int h);
static void (*nativeRender)(void *env, void *thiz);
static void (*nativeOnPause)(void *env, void *thiz);
static void (*nativeOnResume)(void *env, void *thiz);
static void (*nativeKeyEvent)(void *env, void *thiz, int keyCode, jboolean isPressed);
/* nativeTouches* recebem FLOAT x,y. O jogo é softfp (args float em regs core);
   nosso código é hardfp -> declarar pcs("aapcs") p/ passar floats corretamente. */
#define SFCALL __attribute__((pcs("aapcs")))
static void (SFCALL *nativeTouchesBegin)(void *env, void *thiz, int id, float x, float y);
static void (SFCALL *nativeTouchesEnd)(void *env, void *thiz, int id, float x, float y);
static void (SFCALL *nativeTouchesMove)(void *env, void *thiz, int id, float x, float y);
static void (*initCricket)(void *env, void *thiz);

static SDL_GameController *g_gamepad = NULL;
static void *g_env = NULL;

/* ---- cocos2d::EventKeyboard::KeyCode (o jogo consome via APPLET::GetMaskCode) ----
   Decodificado da tabela de máscaras: direcional = arrows; ação = SPACE + outros. */
#define CKEY_LEFT   26
#define CKEY_RIGHT  27
#define CKEY_UP     28
#define CKEY_DOWN   29
#define CKEY_SPACE  59

/* dispatch cocos EventKeyboard DIRETO (bypass do mapa android->cocos que não
   cobre as setas). Replica nativeKeyEvent: dispatcher = *(Director+0x98). */
static void *(*p_MultiInput_getInstance)(void);
static void *(*p_GDM_getInstance)(void);   /* GlobalDataManager (input bitmask em +4/+8) */
static void *(*p_Director_getInstance)(void);
/* escreve o bitmask de input onde o Mega Man lê: GlobalDataManager+4 (e +8). */
static void mm_set_input_mask(unsigned m){
  if(!p_GDM_getInstance) return;
  char *gdm=(char*)p_GDM_getInstance(); if(!gdm) return;
  *(unsigned*)(gdm+4)=m; *(unsigned*)(gdm+8)=m;
}
/* mask do gamepad (setado no loop de eventos) OR'd no input do jogo via hook. */
static volatile unsigned g_pad_mask = 0;
static void (*g_vpad_orig)(void *self) = NULL;
static volatile long g_vpad_calls = 0;      /* nº de game-frames (=chamadas a VirtualPad::update) */
static volatile long g_sim_calls = 0;       /* nº de ticks de SIMULAÇÃO (GT_MANAGER::Caller) */
static void (*g_gtcaller_orig)(void *self) = NULL;
__attribute__((target("thumb")))
static void my_gtcaller(void *self){ g_sim_calls++; if(g_gtcaller_orig) g_gtcaller_orig(self); }
__attribute__((target("thumb")))
static void my_vpad_update(void *self){
  g_vpad_calls++;
  if(g_vpad_orig) g_vpad_orig(self);         /* roda a VirtualPad::update original */
  if(g_pad_mask && p_GDM_getInstance){        /* OR o mask do gamepad físico */
    char *gdm=(char*)p_GDM_getInstance();
    if(gdm){ *(unsigned*)(gdm+4)|=g_pad_mask; *(unsigned*)(gdm+8)|=g_pad_mask; }
  }
}
/* Popula o Lib_MultiInput DIRETO (bypass do dispatch cocos): marca finger id com
   estado (1=began, 2=held), bitmask e posição GL (Y-up). Layout do onTouchBegan/
   Moved: bitmask=*(+56), flags=*(+32)[id], pos=*(+44)[id*2..+1]. */
static void mm_force_touch(int id, int state, float glx, float gly){
  if(!p_MultiInput_getInstance) return;
  char *mi=(char*)p_MultiInput_getInstance(); if(!mi) return;
  unsigned *bm=*(unsigned**)(mi+56); if(bm){ if(state) bm[id>>5]|=(1u<<(id&31)); else bm[id>>5]&=~(1u<<(id&31)); }
  int *flags=*(int**)(mi+32); if(flags) flags[id]=state;
  float *pos=*(float**)(mi+44); if(pos){ pos[id*2]=glx; pos[id*2+1]=gly; }
}
/* lê o estado do Lib_MultiInput: bitmask de touches ativas [+56], flags [+32]. */
static void mm_dump_multiinput(const char *tag){
  if(!p_MultiInput_getInstance) return;
  char *mi = (char*)p_MultiInput_getInstance();
  if(!mi){ fprintf(stderr,"[MTI %s] inst NULL\n",tag); return; }
  unsigned *bmArr = *(unsigned**)(mi+56);   /* ptr p/ array de bitmask */
  int *flags = *(int**)(mi+32);              /* ptr p/ array de flags[id] */
  float *pos = *(float**)(mi+44);            /* ptr p/ array de pos (x,y por id) */
  unsigned bm = bmArr ? bmArr[0] : 0xdead;
  int f0 = flags ? flags[0] : -1;
  fprintf(stderr,"[MTI %s] bmArr=%p bm=0x%x flags=%p f0=%d f1=%d pos=%p p0=(%.1f,%.1f) p_id0=(%.1f,%.1f)\n",
          tag, (void*)bmArr, bm, (void*)flags, f0, flags?flags[1]:-1, (void*)pos,
          pos?pos[0]:-1, pos?pos[1]:-1, pos?pos[0]:-1, pos?pos[1]:-1);
}
static void (*p_EventKeyboard_ctor)(void *self, int keyCode, int pressed);
static void (*p_EventDispatcher_dispatch)(void *disp, void *event);
static void (*p_EventKeyboard_dtor)(void *self);
static void mm_send_cocos_key(int keyCode, int pressed) {
  if (!p_Director_getInstance || !p_EventKeyboard_ctor || !p_EventDispatcher_dispatch) return;
  void *director = p_Director_getInstance();
  if (!director) { if(getenv("MM_INPUTLOG"))fprintf(stderr,"[key] director NULL\n"); return; }
  void *disp = *(void **)((char *)director + 0x98);
  if (getenv("MM_INPUTLOG")) fprintf(stderr,"[key] kc=%d pr=%d dir=%p disp=%p\n",keyCode,pressed,director,disp);
  char event[96]; memset(event, 0, sizeof event);
  p_EventKeyboard_ctor(event, keyCode, pressed);
  if (disp) p_EventDispatcher_dispatch(disp, event);
  if (p_EventKeyboard_dtor) p_EventKeyboard_dtor(event);
}
/* NÃO static: imports.c (my_find_exidx) referencia g_load_base como global. */
volatile uintptr_t g_load_base = 0;

/* ---- crash handler (armhf) — do shantae, p/ diagnosticar SIGSEGV no boot ---- */
static void resolve_addr(uintptr_t a, char *out, int outsz) {
  int fd = open("/proc/self/maps", O_RDONLY);
  out[0] = 0;
  if (fd < 0) return;
  char buf[8192]; int n; char line[400]; int li = 0;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    for (int i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '\n' || li >= (int)sizeof(line) - 1) {
        line[li] = 0;
        unsigned long s, e; char perm[8]; char path[256]; path[0] = 0;
        if (sscanf(line, "%lx-%lx %7s %*x %*s %*d %255s", &s, &e, perm, path) >= 3) {
          if (a >= s && a < e) {
            const char *base = strrchr(path, '/');
            base = base ? base + 1 : (path[0] ? path : "?");
            snprintf(out, outsz, "%s+0x%lx", base, (unsigned long)(a - s));
            close(fd); return;
          }
        }
        li = 0;
      } else line[li++] = c;
    }
  }
  close(fd);
}
static void crash_handler(int sig, siginfo_t *info, void *uc) {
  ucontext_t *u = (ucontext_t *)uc;
  mcontext_t *m = &u->uc_mcontext;
  char r[300];
  fprintf(stderr, "\n=== CRASH sig=%d addr=%p tid=%d ===\n", sig, info->si_addr,
          (int)syscall(__NR_gettid));
  resolve_addr(m->arm_pc, r, sizeof(r));
  fprintf(stderr, "  PC=0x%lx %s", (unsigned long)m->arm_pc, r);
  if (g_load_base && m->arm_pc >= g_load_base)
    fprintf(stderr, "  {%s+0x%lx}", SO_NAME, (unsigned long)(m->arm_pc - g_load_base));
  fprintf(stderr, "\n");
  resolve_addr(m->arm_lr, r, sizeof(r));
  fprintf(stderr, "  LR=0x%lx %s", (unsigned long)m->arm_lr, r);
  if (g_load_base && m->arm_lr >= g_load_base)
    fprintf(stderr, "  {%s+0x%lx}", SO_NAME, (unsigned long)(m->arm_lr - g_load_base));
  fprintf(stderr, "\n");
  fprintf(stderr, "  r0=%08lx r1=%08lx r2=%08lx r3=%08lx r4=%08lx r5=%08lx\n",
          (unsigned long)m->arm_r0, (unsigned long)m->arm_r1, (unsigned long)m->arm_r2,
          (unsigned long)m->arm_r3, (unsigned long)m->arm_r4, (unsigned long)m->arm_r5);
  fprintf(stderr, "  sp=%08lx fp=%08lx ip=%08lx\n", (unsigned long)m->arm_sp,
          (unsigned long)m->arm_fp, (unsigned long)m->arm_ip);
  if (g_load_base) {
    uintptr_t sp = m->arm_sp; int cnt = 0;
    for (uintptr_t a = sp; a < sp + 0x2000 && cnt < 20; a += 4) {
      uintptr_t v = *(uintptr_t *)a;
      if (v >= g_load_base && v < g_load_base + (uintptr_t)GAME_HEAP_MB * 1024 * 1024) {
        fprintf(stderr, "    [sp+0x%lx] %s+0x%lx\n", (unsigned long)(a - sp), SO_NAME,
                (unsigned long)(v - g_load_base));
        cnt++;
      }
    }
  }
  fflush(stderr);
  _exit(128 + sig);
}
static void install_crash_handler(void) {
  struct sigaction sa; memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL); sigaction(SIGABRT, &sa, NULL);
}

static void preload_device_libs(void) {
  static const char *libs[] = {
      "libSDL2-2.0.so.0", "libGLESv2.so", "libEGL.so",
      "libOpenSLES.so", "libstdc++.so.6", "libm.so.6", "libdl.so.2", NULL };
  for (int i = 0; libs[i]; i++) {
    void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr, "preload: %s %s\n", libs[i], h ? "OK" : dlerror());
  }
}

/* ---- controle gamepad -> MULTITOUCH nos controles virtuais na tela ----
   O jogo é TOUCH em menus E gameplay. Mapeamos o gamepad físico (Xbox) p/
   toques nas posições dos controles virtuais (dpad esq, botões dir). ids
   distintos p/ multitouch (andar+pular+atirar simultâneo). Base 1280x720. */
#define VP_DPAD_CX  95.0f
#define VP_DPAD_CY  500.0f
#define VP_DPAD_OFF 58.0f
#define VP_JUMP_X   1175.0f   /* pulo (gameplay) / confirmar✓ (menu) */
#define VP_JUMP_Y   620.0f
#define VP_SHOOT_X  1085.0f
#define VP_SHOOT_Y  475.0f
#define VP_WEAPON_X 1175.0f
#define VP_WEAPON_Y 310.0f
#define VP_BACK_X   1175.0f   /* voltar (menu) */
#define VP_BACK_Y   455.0f
#define VP_PAUSE_X  1210.0f
#define VP_PAUSE_Y  55.0f
enum { TID_DPAD=0, TID_JUMP, TID_SHOOT, TID_WEAPON, TID_BACK, TID_PAUSE };

static void mm_tbegin(int id,float x,float y){ if(nativeTouchesBegin)nativeTouchesBegin(g_env,NULL,id,x,y); }
static void mm_tend(int id,float x,float y){ if(nativeTouchesEnd)nativeTouchesEnd(g_env,NULL,id,x,y); }
static void mm_tmove(int id,float x,float y){ if(nativeTouchesMove)nativeTouchesMove(g_env,NULL,id,x,y); }

/* estado direcional (dpad + stick). Direcional vai por INJEÇÃO DE BITS direta
   (g_pad_mask -> hook -> GDM), determinístico (touch do dpad erra right/down).
   Bits (= GetMaskCode): LEFT=0x1000 RIGHT=0x2000 UP=0x4000 DOWN=0x8000. */
static int dp_up,dp_dn,dp_lf,dp_rt, stk_x,stk_y;
static volatile unsigned g_pad_mask;   /* fwd: bits injetados no input do jogo */
static volatile long long g_per_us = 0;   /* microssegundos por frame (velocidade); L1/R1 ajustam */
#define MM_SPEED_CFG "mm_speed.cfg"        /* persistência da velocidade (na pasta do jogo) */
static void speed_save(void){
  FILE *f = fopen(MM_SPEED_CFG, "w");
  if (f) { fprintf(f, "%lld\n", (long long)g_per_us); fclose(f); }
}
static long long speed_load(void){
  FILE *f = fopen(MM_SPEED_CFG, "r"); if (!f) return 0;
  long long v = 0; if (fscanf(f, "%lld", &v) != 1) v = 0; fclose(f);
  if (v < 8000 || v > 120000) v = 0;
  return v;
}
static void adjust_speed(int slower){     /* slower=1: mais lento; 0: mais rápido */
  long long step = 6667;                   /* passo GRANDE e perceptível (~1 "fps-equivalente" por toque) */
  g_per_us += slower ? step : -step;
  if (g_per_us < 8000) g_per_us = 8000;    /* teto ~125fps */
  if (g_per_us > 120000) g_per_us = 120000;/* piso ~8fps */
  speed_save();
  double fps = 1000000.0/(double)g_per_us;
  long long spf = g_per_us*44100/1000000;
  fprintf(stderr,"[speed] per_us=%lld fps=%.1f MM_SPF=%lld (salvo)\n",(long long)g_per_us,fps,(long long)spf);
}
static void update_dpad(void){
  unsigned m=0;
  if(dp_rt||stk_x>0) m|=0x2000;
  if(dp_lf||stk_x<0) m|=0x1000;
  if(dp_up||stk_y<0) m|=0x4000;
  if(dp_dn||stk_y>0) m|=0x8000;
  g_pad_mask = m;
}
/* botão de ação -> toque hold (begin/end) numa posição fixa */
static void btn_touch(int id,int pressed,float x,float y){ if(pressed)mm_tbegin(id,x,y); else mm_tend(id,x,y); }

/* Botões de ação cocos KeyCode (a confirmar semântica em gameplay).
   Da tabela de máscaras: SPACE=0x10000; candidatos de ação em 124-130 e nums. */
#define CKEY_ACT_A  CKEY_SPACE     /* A = pulo/confirm (provisório) */
#define CKEY_ACT_B  124            /* B = tiro (mask 0x400, provisório) */
#define CKEY_ACT_X  125            /* mask 0x800 */
#define CKEY_ACT_Y  126            /* mask 0x20000 */
#define CKEY_START  127            /* mask 0x40000 (start/pause provisório) */
#define CKEY_SELECT 128            /* mask 0x80000 */

/* SDL keyboard -> cocos KeyCode */
static int map_key_cocos(SDL_Keycode k) {
  switch (k) {
    case SDLK_UP: return CKEY_UP;
    case SDLK_DOWN: return CKEY_DOWN;
    case SDLK_LEFT: return CKEY_LEFT;
    case SDLK_RIGHT: return CKEY_RIGHT;
    case SDLK_SPACE: case SDLK_z: return CKEY_ACT_A;
    case SDLK_LCTRL: case SDLK_x: return CKEY_ACT_B;
    case SDLK_a: return CKEY_ACT_X;
    case SDLK_s: return CKEY_ACT_Y;
    case SDLK_RETURN: return CKEY_START;
    case SDLK_BACKSPACE: return CKEY_SELECT;
    default: return -1;
  }
}
/* SDL controller (Xbox default) -> cocos KeyCode */
static int map_btn_cocos(int b) {
  switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return CKEY_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return CKEY_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return CKEY_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return CKEY_RIGHT;
    case SDL_CONTROLLER_BUTTON_A: return CKEY_ACT_A;
    case SDL_CONTROLLER_BUTTON_B: return CKEY_ACT_B;
    case SDL_CONTROLLER_BUTTON_X: return CKEY_ACT_X;
    case SDL_CONTROLLER_BUTTON_Y: return CKEY_ACT_Y;
    case SDL_CONTROLLER_BUTTON_START: return CKEY_START;
    case SDL_CONTROLLER_BUTTON_BACK: return CKEY_SELECT;
    default: return -1;
  }
}
static void open_gamepad(void) {
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      g_gamepad = SDL_GameControllerOpen(i);
      if (g_gamepad) { fprintf(stderr, "Gamepad: %s\n", SDL_GameControllerName(g_gamepad)); break; }
    }
  }
}

/* ================= Cricket Audio: custom file handler ==================
   Cricket carrega banks/streams via JNI Java AssetManager (não temos) -> banks
   NULL. Registramos um CkCustomFile handler via ReadStream::setFileHandler: o
   Cricket chama nosso handler(path) e devolvemos um CkCustomFile que lê o arquivo
   do filesystem (assets/<path>) via fopen -> banks/streams carregam nativamente.
   Vtable CkCustomFile: [0]=~dtor [1]=~dtor(del) [2]=isValid [3]=read(buf,n)
   [4]=getSize [5]=setPos(pos) [6]=getPos. */
typedef struct { void **vptr; FILE *fp; int size; } MMCkFile;
static int  ckf_isValid(MMCkFile *s){ return (s && s->fp) ? 1 : 0; }
static int  ckf_read(MMCkFile *s, void *buf, int n){ return (s && s->fp) ? (int)fread(buf,1,n,s->fp) : 0; }
static int  ckf_getSize(MMCkFile *s){ return s ? s->size : 0; }
static void ckf_setPos(MMCkFile *s, int pos){ if(s && s->fp) fseek(s->fp,pos,SEEK_SET); }
static int  ckf_getPos(MMCkFile *s){ return (s && s->fp) ? (int)ftell(s->fp) : 0; }
static void ckf_dtor(MMCkFile *s){ if(s && s->fp){ fclose(s->fp); s->fp=NULL; } }
static void ckf_dtor_del(MMCkFile *s){ if(s){ if(s->fp) fclose(s->fp); free(s); } }
static void *g_ckf_vtable[8];
static void mm_ckf_init_vtable(void){
  g_ckf_vtable[0]=(void*)ckf_dtor; g_ckf_vtable[1]=(void*)ckf_dtor_del;
  g_ckf_vtable[2]=(void*)ckf_isValid; g_ckf_vtable[3]=(void*)ckf_read;
  g_ckf_vtable[4]=(void*)ckf_getSize; g_ckf_vtable[5]=(void*)ckf_setPos;
  g_ckf_vtable[6]=(void*)ckf_getPos; g_ckf_vtable[7]=0;
}
/* handler: recebe o path que o Cricket pede; abre assets/<path> (tenta variações) */
static void *mm_ckfile_handler(const char *path, void *data){
  (void)data;
  if(!path) return NULL;
  char cand[1024]; FILE *fp=NULL;
  const char *tries[3]; int nt=0;
  snprintf(cand,sizeof cand,"assets/%s",path); tries[nt++]=strdup(cand);
  if(!strchr(path,'/')){ snprintf(cand,sizeof cand,"assets/sound/%s",path); tries[nt++]=strdup(cand); }
  tries[nt++]=strdup(path);
  const char *opened=NULL;
  for(int i=0;i<nt;i++){ fp=fopen(tries[i],"rb"); if(fp){opened=tries[i];break;} }
  if(getenv("MM_CKFLOG")) fprintf(stderr,"[ckf] handler('%s') -> %s\n",path,opened?opened:"MISS");
  for(int i=0;i<nt;i++) free((void*)tries[i]);
  if(!fp) return NULL;
  MMCkFile *f=malloc(sizeof(MMCkFile));
  f->vptr=g_ckf_vtable; f->fp=fp;
  fseek(fp,0,SEEK_END); f->size=(int)ftell(fp); fseek(fp,0,SEEK_SET);
  return f;
}

/* Patch runtime: escreve halfwords Thumb num símbolo do jogo (do shantae). */
static void patch_thumb(const char *sym, const uint16_t *hw, int n) {
  uintptr_t a = so_find_addr_safe(sym);
  if (!a) { fprintf(stderr, "patch: símbolo %s não encontrado\n", sym); return; }
  a &= ~1u;
  uintptr_t pg = a & ~0xFFFUL;
  if (mprotect((void *)pg, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    fprintf(stderr, "patch: mprotect %s falhou\n", sym); return;
  }
  for (int i = 0; i < n; i++) ((uint16_t *)a)[i] = hw[i];
  mprotect((void *)pg, 0x2000, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)a, (char *)a + n * 2);
  fprintf(stderr, "patch: %s @ 0x%lx (%d hw)\n", sym, (unsigned long)a, n);
}
/* movs r0,#0 ; bx lr */
static void patch_thumb_ret0(const char *sym) {
  uint16_t hw[] = {0x2000, 0x4770}; patch_thumb(sym, hw, 2);
}

/* ---- hook Thumb inline (call-original) ----
   Faz o símbolo saltar p/ `repl` (Thumb). Devolve um trampolim que executa o
   PREFIXO original (relocando 1 bl) e retorna ao resto da função. Prefixo
   suportado: 8 bytes = 2 instr curtas (2B) + 1 bl (4B) [caso VirtualPad::update]. */
static void thumb_encode_bl(uint16_t *out, uintptr_t at, uintptr_t target){
  int32_t off = (int32_t)(target - (at + 4));
  uint32_t S=(off>>24)&1, I1=(off>>23)&1, I2=(off>>22)&1;
  uint32_t imm10=(off>>12)&0x3FF, imm11=(off>>1)&0x7FF;
  uint32_t J1=(~(I1^S))&1, J2=(~(I2^S))&1;
  out[0]=0xF000|(S<<10)|imm10;
  out[1]=0xD000|(J1<<13)|(J2<<11)|imm11;
}
/* jump absoluto Thumb (movw ip;movt ip;bx ip) = 5 halfwords, INDEPENDE de
   alinhamento (não usa PC-rel). o[] recebe as instruções. */
static void write_abs_jump(uint16_t *o, uint32_t t){
  uint32_t lo=t&0xFFFF, hi=t>>16;
  o[0]=0xF240 | (((lo>>11)&1)<<10) | ((lo>>12)&0xF);
  o[1]=(((lo>>8)&7)<<12) | (12<<8) | (lo&0xFF);
  o[2]=0xF2C0 | (((hi>>11)&1)<<10) | ((hi>>12)&0xF);
  o[3]=(((hi>>8)&7)<<12) | (12<<8) | (hi&0xFF);
  o[4]=0x4760;                       /* bx ip */
}
/* hook simples: prefixo de 5 instr curtas (10B) SEM PC-rel, copiadas verbatim.
   Usa jump absoluto (alinhamento-agnóstico). */
static void *hook_thumb_simple(const char *sym, void *repl){
  uintptr_t a = so_find_addr_safe(sym); if(!a){ fprintf(stderr,"hookS: %s nao achado\n",sym); return NULL; }
  a &= ~1u; uint16_t *o=(uint16_t*)a;
  uint16_t *tr = mmap(NULL,64,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(tr==MAP_FAILED) return NULL;
  for(int i=0;i<5;i++) tr[i]=o[i];                 /* copia 5 instr (10B) */
  write_abs_jump(&tr[5], (uint32_t)(a+10)|1);        /* volta p/ a+10 */
  __builtin___clear_cache((char*)tr,(char*)tr+64);
  uintptr_t pg=a&~0xFFFUL; mprotect((void*)pg,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC);
  write_abs_jump(o, (uint32_t)repl);                 /* entry -> repl */
  mprotect((void*)pg,0x2000,PROT_READ|PROT_EXEC); __builtin___clear_cache((char*)a,(char*)a+10);
  fprintf(stderr,"hookS %s @0x%lx -> %p\n",sym,(unsigned long)a,repl);
  return (void*)((uintptr_t)tr|1);
}
static void *hook_thumb_call(const char *sym, void *repl){
  uintptr_t a = so_find_addr_safe(sym);
  if(!a){ fprintf(stderr,"hook: %s nao achado\n",sym); return NULL; }
  a &= ~1u;
  uint16_t *o = (uint16_t*)a;
  /* trampolim: [instr0][instr1][bl relocado][ldr.w pc,[pc]][addr a+8] */
  uint16_t *tr = mmap(NULL, 64, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(tr==MAP_FAILED) return NULL;
  /* decodifica o bl original (o[2],o[3]) -> alvo absoluto (getInstance) */
  uint32_t S=(o[2]>>10)&1, J1=(o[3]>>13)&1, J2=(o[3]>>11)&1;
  uint32_t I1=(~J1^S)&1, I2=(~J2^S)&1;
  int32_t imm=(S<<24)|(I1<<23)|(I2<<22)|((o[2]&0x3FF)<<12)|((o[3]&0x7FF)<<1);
  if(S) imm|=0xFE000000;
  uintptr_t bltgt = (a+4+4) + imm;
  /* trampolim (Thumb), com BL substituído por ldr ip,[pc];blx ip (alcance ∞):
     [0]push [1]adds [2-3]ldr.w ip,[pc,#8] [4]blx ip [5-6]ldr.w pc,[pc,#8]
     [7]nop  [8-9].word bltgt|1  [10-11].word (a+8)|1 */
  tr[0]=o[0]; tr[1]=o[1];
  tr[2]=0xF8DF; tr[3]=0xC008;   /* ldr.w ip,[pc,#8] -> byte16 */
  tr[4]=0x47E0;                 /* blx ip (chama getInstance, lr=byte10) */
  tr[5]=0xF8DF; tr[6]=0xF008;   /* ldr.w pc,[pc,#8] -> byte20 */
  tr[7]=0xBF00;                 /* nop */
  *(uint32_t*)&tr[8]=(uint32_t)bltgt|1;
  *(uint32_t*)&tr[10]=(uint32_t)(a+8)|1;
  __builtin___clear_cache((char*)tr,(char*)tr+64);
  /* patch entry: ldr.w pc,[pc,#0] ; addr=repl */
  uintptr_t pg=a&~0xFFFUL;
  mprotect((void*)pg,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC);
  o[0]=0xF8DF; o[1]=0xF000; *(uint32_t*)&o[2]=(uint32_t)repl; /* repl já é Thumb (bit0 set pelo compilador? garantir) */
  mprotect((void*)pg,0x2000,PROT_READ|PROT_EXEC);
  __builtin___clear_cache((char*)a,(char*)a+8);
  fprintf(stderr,"hook %s @0x%lx -> repl=%p tramp=%p bltgt=0x%lx\n",sym,(unsigned long)a,repl,tr,(unsigned long)bltgt);
  return (void*)((uintptr_t)tr | 1);   /* Thumb bit: chamadas ao trampolim entram em Thumb */
}
/* bx lr (retorno void, preserva r0) */
static void patch_thumb_ret(const char *sym) {
  uint16_t hw[] = {0x4770}; patch_thumb(sym, hw, 1);
}
/* patch entry -> salta p/ target (ldr.w pc,[pc]; addr). NÃO chama original. */
static void patch_thumb_jump(const char *sym, void *target) {
  uintptr_t a = so_find_addr_safe(sym);
  if (!a) { fprintf(stderr, "jump: %s nao achado\n", sym); return; }
  a &= ~1u;
  uintptr_t pg = a & ~0xFFFUL;
  mprotect((void *)pg, 0x2000, PROT_READ|PROT_WRITE|PROT_EXEC);
  uint16_t *o = (uint16_t *)a;
  o[0]=0xF8DF; o[1]=0xF000; *(uint32_t *)&o[2]=(uint32_t)target;
  mprotect((void *)pg, 0x2000, PROT_READ|PROT_EXEC);
  __builtin___clear_cache((char *)a, (char *)a + 8);
  fprintf(stderr, "jump %s @0x%lx -> %p\n", sym, (unsigned long)a, target);
}

static DynLibFunction *g_base; static int g_base_n;
static void build_base_table(void) {
  g_base_n = shantae_overrides_count + revc_pthread_count;
  g_base = malloc(sizeof(DynLibFunction) * g_base_n);
  memcpy(g_base, shantae_overrides, sizeof(DynLibFunction) * shantae_overrides_count);
  memcpy(g_base + shantae_overrides_count, revc_pthread_table,
         sizeof(DynLibFunction) * revc_pthread_count);
}

/* ================= ÁUDIO: Cricket AudioTrack -> SDL ==================
   Cricket renderiza PCM 16-bit stereo @44100 e escreve via AudioTrackProxy::write
   (Java AudioTrack, não temos). Hookamos write -> lê o short[] e produz num ring
   buffer; o callback SDL drena. O produtor BLOQUEIA se o ring encher -> paceia a
   thread de áudio do Cricket (= paceia a lógica do jogo -> conserta a velocidade). */
extern unsigned char *jni_shim_get_array(void *handle, int *outlen);
#define ARING (1<<18)               /* 256KB, potência de 2 */
static unsigned char g_aring[ARING];
static volatile int g_ahead=0, g_atail=0;
static SDL_AudioDeviceID g_adev=0;
static volatile long g_wr_calls=0, g_wr_bytes=0, g_cb_calls=0;
static volatile long long g_frames_played=0;   /* frames que o SDL já tocou (posição do "AudioTrack") */
static int aring_used(void){ int u=g_ahead-g_atail; if(u<0)u+=ARING; return u; }
static void SDLCALL audio_cb(void *ud, Uint8 *stream, int len){
  (void)ud; g_cb_calls++;
  for(int i=0;i<len;i++){
    if(g_atail!=g_ahead){ stream[i]=g_aring[g_atail]; g_atail=(g_atail+1)&(ARING-1); }
    else stream[i]=0;
  }
  g_frames_played += len/4;   /* 4 bytes/frame (stereo 16-bit) */
}
/* hook de AudioTrackProxy::getPlaybackHeadPosition() -> frames tocados pelo SDL.
   Sem isto o head fica 0 e o updateLoop do Cricket nunca renderiza (buffer "cheio"). */
__attribute__((target("thumb")))
static int my_getPlaybackHeadPosition(void *self){ (void)self; return (int)g_frames_played; }
static void aring_write(const unsigned char *d, int n){
  int off=0;
  while(off<n){
    int guard=0;
    while((ARING-1-aring_used())<=0){ usleep(1000); if(++guard>2000) return; } /* bloqueia (paceia) */
    int fr=ARING-1-aring_used(); int c=n-off; if(c>fr)c=fr;
    for(int i=0;i<c;i++){ g_aring[g_ahead]=d[off+i]; g_ahead=(g_ahead+1)&(ARING-1); }
    off+=c;
  }
}
/* hook de AudioTrackProxy::write(this, jshortArray, count) -> PCM p/ SDL.
   count = nº de SHORTS (=frames*2 stereo); PCM = count*2 bytes. retorna count
   (o assert de renderBuffer exige write_return == count). */
__attribute__((target("thumb")))
static int my_audiotrack_write(void *self, void *jarr, int count){
  (void)self;
  int len=0; unsigned char *d = jni_shim_get_array(jarr, &len);
  int bytes=count*2; g_wr_calls++; g_wr_bytes+=bytes;
  if(d && len>0){ if(bytes>len)bytes=len; if(bytes>0) aring_write(d, bytes); }
  return count;
}

/* screenshot via glReadPixels (fb0 falha durante render Mali). bottom-up. */
extern void glReadPixels(int x, int y, int w, int h, unsigned fmt, unsigned type, void *px);
static void mm_shot(int w, int h, int id) {
  size_t n = (size_t)w * h * 4;
  unsigned char *buf = malloc(n); if (!buf) return;
  glReadPixels(0, 0, w, h, 0x1908 /*RGBA*/, 0x1401 /*UBYTE*/, buf);
  char path[256]; snprintf(path, sizeof path, "%s/shot_%d.raw", getenv("HOME") ?: ".", id);
  FILE *f = fopen(path, "wb"); if (f) { fwrite(buf, 1, n, f); fclose(f); fprintf(stderr, "SHOT %s\n", path); }
  free(buf);
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  install_crash_handler();
  fprintf(stderr, "=== Mega Man 1 (Cocos2d-x 3.9) armhf so-loader / Mali-450 ===\n");

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
  }
  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) != 0) { dm.w = 1280; dm.h = 720; }

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

  SDL_Window *window = SDL_CreateWindow("Mega Man",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, dm.w, dm.h,
      SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN);
  if (!window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
  SDL_GLContext glc = SDL_GL_CreateContext(window);
  if (!glc) { fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError()); return 1; }
  int w, h; SDL_GL_GetDrawableSize(window, &w, &h);
  fprintf(stderr, "Window %dx%d\n", w, h);
  open_gamepad();

  /* saída de áudio SDL: Cricket escreve PCM -> ring -> callback drena. */
  if (!getenv("MM_NOAUDIO")) {
    SDL_AudioSpec want, have; memset(&want, 0, sizeof want);
    want.freq = 44100; want.format = AUDIO_S16LSB; want.channels = 2;
    want.samples = 1024; want.callback = audio_cb;
    g_adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_adev) { SDL_PauseAudioDevice(g_adev, 0); fprintf(stderr, "SDL audio: %dHz %dch\n", have.freq, have.channels); }
    else fprintf(stderr, "SDL_OpenAudioDevice falhou: %s\n", SDL_GetError());
  }

  preload_device_libs();
  build_base_table();

  /* ---- carrega libcocos2dcpp.so (módulo único) ---- */
  size_t heap_size = (size_t)GAME_HEAP_MB * 1024 * 1024;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) { fprintf(stderr, "mmap %d MB falhou\n", GAME_HEAP_MB); return 1; }
  if (so_load(SO_NAME, heap, heap_size) < 0) { fprintf(stderr, "so_load %s falhou\n", SO_NAME); return 1; }
  g_load_base = (uintptr_t)text_base;
  fprintf(stderr, "Loaded %s: text=%p+%zu data=%p+%zu\n", SO_NAME, text_base, text_size, data_base, data_size);
  if (so_relocate() < 0) { fprintf(stderr, "so_relocate falhou\n"); return 1; }
  so_resolve(g_base, g_base_n, 0);
  so_flush_caches();
  so_execute_init_array();
  fprintf(stderr, "init_array done\n");

  /* Cricket: registrar nosso file handler (lê banks/streams do filesystem)
     ANTES do initCricket carregar os banks. MM_NOCKF=1 desativa. */
  if (!getenv("MM_NOCKF")) {
    void (*setFileHandler)(void*,void*) =
      (void*)so_find_addr_safe("_ZN3Cki10ReadStream14setFileHandlerEPFP12CkCustomFilePKcPvES5_");
    if (setFileHandler) {
      mm_ckf_init_vtable();
      setFileHandler((void*)mm_ckfile_handler, NULL);
      fprintf(stderr, "Cricket setFileHandler registrado (filesystem)\n");
    } else fprintf(stderr, "WARN: setFileHandler não achado\n");
  }

  /* Cricket Audio (debug build) loga via Logger/TextWriter::writef -> glibc
     vsnprintf e crasha (ponteiro-lixo no formato). Logging é não-essencial ->
     neutralizar os formatadores. MM_KEEPCKLOG=1 mantém (p/ diagnóstico). */
  if (!getenv("MM_KEEPCKLOG")) {
    patch_thumb_ret("_ZN3Cki6Logger6writefE9CkLogTypePKcz");
    patch_thumb_ret("_ZN3Cki10TextWriter6writefEPKcz");
    patch_thumb_ret("_ZN3Cki10TextWriter7writefvEPKcSt9__va_list");
  }
  /* NÃO stubar DebugWriter::fail: é noreturn (o assert do Cricket) e os callers
     têm `udf` (trap) logo após -> stubá-lo cai no udf (SIGILL). Em vez disso,
     eliminar as CONDIÇÕES de assert (GetJavaVM no jni_shim, sample rate abaixo). */

  /* Cki::Audio::getNativeSampleRate lê um sample rate global; se ==0 (nossa JNI
     não fornece o valor nativo do AudioTrack) -> DebugWriter::fail + udf. Patch:
     retorna 44100 (HW rate universal). movw r0,#44100 ; bx lr. */
  {
    uint16_t hw[] = {0xf64a, 0x4044, 0x4770};
    patch_thumb("_ZN3Cki5Audio19getNativeSampleRateEv", hw, 3);
  }
  /* Saída de áudio via Java AudioTrack (GraphOutputJavaAndroid::renderBuffer):
     AudioTrackProxy::write() chama AudioTrack.write() por JNI (não temos) ->
     retorna 0 != esperado -> assert (udf). Neutralizar renderBuffer (silencioso)
     p/ o thread de áudio não abortar -> imagem renderiza. TODO: rotear áudio p/
     OpenSL/SDL. MM_KEEPCKOUT=1 mantém (diagnóstico). */
  /* ÁUDIO: NÃO stubar renderBuffer (deixa renderizar o PCM). Em vez disso,
     redirecionar AudioTrackProxy::write -> nosso handler (PCM -> SDL ring). */
  if (!getenv("MM_NOAUDIO")) {
    patch_thumb_jump("_ZN3Cki15AudioTrackProxy5writeEP12_jshortArrayi", (void*)my_audiotrack_write);
    patch_thumb_jump("_ZN3Cki15AudioTrackProxy23getPlaybackHeadPositionEv", (void*)my_getPlaybackHeadPosition);
  } else
    patch_thumb_ret("_ZN3Cki22GraphOutputJavaAndroid12renderBufferEv");
  /* (playSe/se_play NÃO stubados: com o file handler os banks carregam do
     filesystem -> Sound::newBankSound recebe bank válido, sem crash. MM_STUBSE=1
     re-stuba se necessário.) */
  if (getenv("MM_STUBSE")) {
    patch_thumb_ret0("_ZN8fine_lib18Lib_SoundCkManager6playSeEiii");
    patch_thumb_ret0("_ZN13MEDIA_MANAGER7se_playEii");
  }
  /* CONTROLE: hook em VirtualPad::update (chama original + OR o mask do gamepad
     em GlobalDataManager+4/+8, onde o Mega Man lê o input). */
  if (!getenv("MM_NOPADHOOK"))
    g_vpad_orig = (void(*)(void*))hook_thumb_call("_ZN10VirtualPad6updateEv", (void*)my_vpad_update);
  if (getenv("MM_SIMHOOK"))     /* mede a taxa da simulação (instável — só diag) */
    g_gtcaller_orig = (void(*)(void*))hook_thumb_simple("_ZN10GT_MANAGER6CallerEv", (void*)my_gtcaller);

  p_JNI_OnLoad    = (void *)so_find_addr_safe("JNI_OnLoad");
  nativeSetContext= (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetContext");
  nativeSetApkPath= (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetApkPath");
  nativeInit      = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
  nativeRender    = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");
  nativeOnPause   = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause");
  nativeOnResume  = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume");
  nativeKeyEvent  = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyEvent");
  nativeTouchesBegin = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin");
  nativeTouchesEnd   = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd");
  nativeTouchesMove  = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove");
  initCricket     = (void *)so_find_addr_safe("Java_org_cocos2dx_cpp_AppActivity_initCricket");

  p_MultiInput_getInstance = (void *)so_find_addr_safe("_ZN8fine_lib14Lib_MultiInput11getInstanceEv");
  p_GDM_getInstance = (void *)so_find_addr_safe("_ZN17GlobalDataManager11getInstanceEv");
  p_Director_getInstance   = (void *)so_find_addr_safe("_ZN7cocos2d8Director11getInstanceEv");
  p_EventKeyboard_ctor     = (void *)so_find_addr_safe("_ZN7cocos2d13EventKeyboardC1ENS0_7KeyCodeEb");
  p_EventDispatcher_dispatch = (void *)so_find_addr_safe("_ZN7cocos2d15EventDispatcher13dispatchEventEPNS_5EventE");
  p_EventKeyboard_dtor     = (void *)so_find_addr_safe("_ZN7cocos2d13EventKeyboardD1Ev");

  if (!nativeInit || !nativeRender) { fprintf(stderr, "FALTA nativeInit/nativeRender\n"); return 1; }

  void *fake_vm = NULL, *fake_env = NULL;
  jni_shim_init(&fake_vm, &fake_env);
  g_env = fake_env;

  fprintf(stderr, "JNI_OnLoad...\n");
  if (p_JNI_OnLoad) p_JNI_OnLoad(fake_vm, NULL);

  void *dummy = (void *)0xDEADBEEF;
  void *apk = jni_make_string("/storage/roms/ports/megaman1/base.apk");
  if (nativeSetApkPath) { fprintf(stderr, "nativeSetApkPath\n"); nativeSetApkPath(fake_env, NULL, apk); }
  if (nativeSetContext) { fprintf(stderr, "nativeSetContext\n"); nativeSetContext(fake_env, NULL, dummy, dummy); }
  /* initCricket (áudio Cricket): com DebugWriter::fail neutralizado, o ctor
     SystemAndroid não aborta em falha de asset. MM_NOCRICKET=1 pula. */
  if (initCricket && !getenv("MM_NOCRICKET")) { fprintf(stderr, "initCricket\n"); initCricket(fake_env, NULL); }

  fprintf(stderr, "nativeInit(%d,%d)...\n", w, h);
  nativeInit(fake_env, NULL, w, h);
  if (nativeOnResume) nativeOnResume(fake_env, NULL);

  fprintf(stderr, "Entering main loop...\n");
  int running = 1;
  SDL_Event e;
  int autopress = getenv("MM_AUTOPRESS") != NULL;
  long fc = 0;
  while (running) {
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
        case SDL_QUIT: running = 0; break;
        case SDL_KEYDOWN: case SDL_KEYUP: {
          if (e.key.repeat) break;
          int pr = (e.type==SDL_KEYDOWN);
          switch (e.key.keysym.sym) {
            case SDLK_ESCAPE: if(pr) running=0; break;
            case SDLK_UP:    dp_up=pr; update_dpad(); break;
            case SDLK_DOWN:  dp_dn=pr; update_dpad(); break;
            case SDLK_LEFT:  dp_lf=pr; update_dpad(); break;
            case SDLK_RIGHT: dp_rt=pr; update_dpad(); break;
            case SDLK_SPACE: case SDLK_z: btn_touch(TID_JUMP,pr,VP_JUMP_X,VP_JUMP_Y); break;
            case SDLK_x: btn_touch(TID_SHOOT,pr,VP_SHOOT_X,VP_SHOOT_Y); break;
            case SDLK_c: btn_touch(TID_WEAPON,pr,VP_WEAPON_X,VP_WEAPON_Y); break;
            case SDLK_BACKSPACE: btn_touch(TID_BACK,pr,VP_BACK_X,VP_BACK_Y); break;
            case SDLK_RETURN: btn_touch(TID_PAUSE,pr,VP_PAUSE_X,VP_PAUSE_Y); break;
          }
          break;
        }
        case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: {
          int pr = (e.type==SDL_CONTROLLERBUTTONDOWN);
          /* HOTKEY DE SAIR no proprio binario: Select+Start juntos -> encerra
             (sem depender do gptokeyb). Rastreia estado dos dois botoes. */
          static int held_sel=0, held_start=0;
          if (e.cbutton.button==SDL_CONTROLLER_BUTTON_BACK)  held_sel=pr;
          if (e.cbutton.button==SDL_CONTROLLER_BUTTON_START) held_start=pr;
          if (held_sel && held_start) { fprintf(stderr,"Select+Start -> sair\n"); running=0; break; }
          switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:    dp_up=pr; update_dpad(); break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  dp_dn=pr; update_dpad(); break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  dp_lf=pr; update_dpad(); break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: dp_rt=pr; update_dpad(); break;
            case SDL_CONTROLLER_BUTTON_A: btn_touch(TID_JUMP,pr,VP_JUMP_X,VP_JUMP_Y); break;   /* pulo/confirmar */
            case SDL_CONTROLLER_BUTTON_X: btn_touch(TID_SHOOT,pr,VP_SHOOT_X,VP_SHOOT_Y); break; /* tiro */
            case SDL_CONTROLLER_BUTTON_Y: btn_touch(TID_WEAPON,pr,VP_WEAPON_X,VP_WEAPON_Y); break;/* arma */
            case SDL_CONTROLLER_BUTTON_B: btn_touch(TID_BACK,pr,VP_BACK_X,VP_BACK_Y); break;     /* voltar */
            case SDL_CONTROLLER_BUTTON_START: btn_touch(TID_PAUSE,pr,VP_PAUSE_X,VP_PAUSE_Y); break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  if(pr) adjust_speed(0); break; /* L1: mais rápido */
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: if(pr) adjust_speed(1); break; /* R1: mais lento */
          }
          break;
        }
        case SDL_CONTROLLERAXISMOTION: {
          const int TH=16000;
          if (e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTX) { stk_x = e.caxis.value>TH?1:e.caxis.value<-TH?-1:0; update_dpad(); }
          else if (e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTY) { stk_y = e.caxis.value>TH?1:e.caxis.value<-TH?-1:0; update_dpad(); }
          break;
        }
      }
    }
    /* MM_AUTOPRESS: toca o centro periodicamente p/ passar logos/título (VirtualPad touch) */
    if (autopress) {
      fc++;
      int sub = fc % 120;
      if (sub == 30 && nativeTouchesBegin) nativeTouchesBegin(g_env, NULL, 0, w/2.0f, h/2.0f);
      if (sub == 45 && nativeTouchesEnd)   nativeTouchesEnd(g_env, NULL, 0, w/2.0f, h/2.0f);
    }
    /* MM_NAVTEST: injeta teclas via nativeKeyEvent p/ validar controle no menu.
       shot 1=inicial, DOWN, shot 2, DOWN, shot 3, UP+confirm(A/START/ENTER), shot 4. */
    if (getenv("MM_NAVTEST")) {
      static long f = 0; f++;
      #define TOUCH(x,y) do{ if(nativeTouchesBegin)nativeTouchesBegin(g_env,NULL,0,(float)(x),(float)(y)); }while(0)
      #define UNTOUCH(x,y) do{ if(nativeTouchesEnd)nativeTouchesEnd(g_env,NULL,0,(float)(x),(float)(y)); }while(0)
      /* checkmark confirmar em (1180,620); drillar main->mode->stage->gameplay */
      #define TAPCHK() do{ TOUCH(1180,620); }while(0)
      #define RELCHK() do{ UNTOUCH(1180,620); }while(0)
      if (f==120) mm_shot(w,h,1);
      if (f==180){ TAPCHK(); fprintf(stderr,"CONFIRM 1\n"); } if (f==200) RELCHK();
      if (f==340) mm_shot(w,h,2);
      if (f==400){ TAPCHK(); fprintf(stderr,"CONFIRM 2\n"); } if (f==420) RELCHK();
      if (f==560) mm_shot(w,h,3);
      if (f==620){ TAPCHK(); fprintf(stderr,"CONFIRM 3\n"); } if (f==640) RELCHK();
      if (f==780) mm_shot(w,h,4);
      if (f==840){ TAPCHK(); fprintf(stderr,"CONFIRM 4\n"); } if (f==860) RELCHK();
      if (f==1000) mm_shot(w,h,5);                       /* stage select */
      if (f==1060){ TAPCHK(); fprintf(stderr,"CONFIRM 5 (enter stage)\n"); } if (f==1080) RELCHK();
      /* MEDIÇÃO OBJETIVA: segura DIREITA e captura RAJADA de 6 shots (30 game-frames
         cada) com timestamp wall-clock -> host mede quanto o jogo muda por render
         e por segundo real. */
      if (f==1300){ dp_rt=1; update_dpad(); fprintf(stderr,"WALK RIGHT\n"); }
      if (f>=1310 && f<=1460 && (f-1310)%30==0){
        int id = 6 + (int)((f-1310)/30);   /* shots 6..11 */
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
        mm_shot(w,h,id);
        fprintf(stderr,"BURST id=%d f=%ld wall_ms=%lld\n", id, f,
                (long long)(ts.tv_sec*1000LL+ts.tv_nsec/1000000));
      }
      if (f==1470){ dp_rt=0; update_dpad(); fprintf(stderr,"NAVTEST done\n"); }
    }
    nativeRender(g_env, NULL);
    SDL_GL_SwapWindow(window);
    /* PACING preciso do frame (jogo é frame-based; velocidade = fps de render).
       per_us = MM_SPF samples @44100 em microssegundos (MM_SPF=1470 -> 30.00fps
       exato; maior=mais lento). Acumulador monotônico -> SEM drift nem jitter.
       Assim o vídeo casa com o áudio (ambos tempo-real, mesma base). */
    {
      static long long next_us = 0;
      if (!g_per_us) {
        long long saved = speed_load();          /* 1) valor salvo (L1/R1) */
        if (saved) g_per_us = saved;
        else {                                   /* 2) MM_SPF ou 3) default 15fps */
          const char *e = getenv("MM_SPF"); long long spf = e ? atoll(e) : 2940;
          if (spf < 1) spf = 2940;               /* 2940 = 44100/15 -> 15.00fps */
          g_per_us = spf * 1000000LL / 44100;
        }
        fprintf(stderr, "[speed] inicial per_us=%lld fps=%.1f\n", (long long)g_per_us, 1000000.0/(double)g_per_us);
      }
      struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      long long now_us = ts.tv_sec*1000000LL + ts.tv_nsec/1000;
      if (!next_us) next_us = now_us;
      next_us += g_per_us;
      long long d = next_us - now_us;
      if (d > 0 && d < g_per_us*4) { struct timespec s={d/1000000,(d%1000000)*1000}; nanosleep(&s,NULL); }
      else next_us = now_us;                  /* atrasou -> resync */
    }
    if (getenv("MM_ADIAG")) {
      static long fcnt=0; static long long t0=0;
      fcnt++;
      struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
      long long ms=ts.tv_sec*1000LL+ts.tv_nsec/1000000;
      if(!t0)t0=ms;
      if(ms-t0>=1000){ fprintf(stderr,"[adiag] render_fps=%ld gameframes=%ld SIM=%ld wr=%ld\n",
                       fcnt,g_vpad_calls,g_sim_calls,g_wr_calls);
                       fcnt=0; g_vpad_calls=0; g_sim_calls=0; g_wr_calls=0; g_wr_bytes=0; g_cb_calls=0; t0=ms; }
    }
  }

  fprintf(stderr, "Exiting...\n");
  if (nativeOnPause) nativeOnPause(g_env, NULL);
  if (g_gamepad) SDL_GameControllerClose(g_gamepad);
  SDL_GL_DeleteContext(glc);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
