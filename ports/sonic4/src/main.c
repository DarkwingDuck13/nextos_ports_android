/*
 * main.c -- Sonic the Hedgehog 4: Episode II (Sega "NN/Ninja" engine + "fox"
 * wrapper, libfox.so, armv7, GLES2) so-loader p/ NextOS armv7 + Mali-450 (fbdev,
 * GLES2 via SDL2).
 *
 * Modelo GLSurfaceView (JNI-driven): a Activity Java dirige a engine. Nós
 * replicamos esse driver aqui chamando os entry points Java_com_mineloader_fox_
 * foxJniLib_*: init -> SetGamePath -> SetLanguageId -> DrawEGLCreated ->
 * loop{ FileProcess, GameProcess, DrawFrame } + input.
 *
 * Framework so_util/egl_shim/jni_shim/imports REUSADO do Shantae (ELF32-ARM).
 * Estudo: ports/sonic4/STUDY.md.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "so_util.h"
#include "egl_shim.h"
#include "jni_shim.h"

#define GAME_SO "lib/armeabi-v7a/libfox.so"
#define GAME_HEAP_MB 256

#define SCREEN_W 1280
#define SCREEN_H 720

extern DynLibFunction shantae_overrides[];
extern const int shantae_overrides_count;
extern DynLibFunction revc_pthread_table[];
extern const int revc_pthread_count;

volatile uintptr_t g_load_base = 0;
extern int dys_screen_w, dys_screen_h; /* resolução real da tela (egl_shim) */

static DynLibFunction *g_base;
static int g_base_n;
static void build_base_table(void) {
  g_base_n = shantae_overrides_count + revc_pthread_count;
  g_base = malloc(sizeof(DynLibFunction) * g_base_n);
  memcpy(g_base, shantae_overrides, sizeof(DynLibFunction) * shantae_overrides_count);
  memcpy(g_base + shantae_overrides_count, revc_pthread_table,
         sizeof(DynLibFunction) * revc_pthread_count);
}

/* ---- patch "return 0" detectando ARM vs Thumb pelo bit baixo do símbolo ---- */
static void patch_ret0(const char *sym) {
  uintptr_t raw = so_find_addr_safe(sym);
  if (!raw) { fprintf(stderr, "patch: símbolo %s NÃO encontrado\n", sym); return; }
  int thumb = raw & 1;
  uintptr_t a = raw & ~1u;
  uintptr_t pg = a & ~0xFFFUL;
  if (mprotect((void *)pg, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    fprintf(stderr, "patch: mprotect %s falhou\n", sym); return;
  }
  if (thumb) {
    ((uint16_t *)a)[0] = 0x2000; /* movs r0,#0 */
    ((uint16_t *)a)[1] = 0x4770; /* bx lr      */
  } else {
    ((uint32_t *)a)[0] = 0xe3a00000; /* mov r0,#0 (ARM) */
    ((uint32_t *)a)[1] = 0xe12fff1e; /* bx lr     (ARM) */
  }
  mprotect((void *)pg, 0x2000, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)a, (char *)a + 8);
  fprintf(stderr, "patch: %s -> return 0 @0x%lx (%s)\n", sym,
          (unsigned long)a, thumb ? "Thumb" : "ARM");
}

/* patch "return val" (val pequeno) detectando ARM/Thumb */
static void patch_retval(const char *sym, int val) {
  uintptr_t raw = so_find_addr_safe(sym);
  if (!raw) { fprintf(stderr, "patch: %s NÃO encontrado\n", sym); return; }
  int thumb = raw & 1; uintptr_t a = raw & ~1u, pg = a & ~0xFFFUL;
  if (mprotect((void *)pg, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return;
  if (thumb) { ((uint16_t *)a)[0] = 0x2000 | (val & 0xff); ((uint16_t *)a)[1] = 0x4770; }
  else { ((uint32_t *)a)[0] = 0xe3a00000 | (val & 0xff); ((uint32_t *)a)[1] = 0xe12fff1e; }
  mprotect((void *)pg, 0x2000, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)a, (char *)a + 8);
  fprintf(stderr, "patch: %s -> return %d @0x%lx (%s)\n", sym, val,
          (unsigned long)a, thumb ? "Thumb" : "ARM");
}

/* patch de UMA instrução ARM em offset de byte dentro de um símbolo (ARM mode). */
static void patch_word_at(const char *sym, unsigned off, uint32_t insn) {
  uintptr_t raw = so_find_addr_safe(sym);
  if (!raw) { fprintf(stderr, "patch_word: %s NÃO encontrado\n", sym); return; }
  uintptr_t a = (raw & ~1u) + off, pg = a & ~0xFFFUL;
  if (mprotect((void *)pg, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    fprintf(stderr, "patch_word: mprotect %s falhou\n", sym); return;
  }
  ((uint32_t *)a)[0] = insn;
  mprotect((void *)pg, 0x2000, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)a, (char *)a + 4);
  fprintf(stderr, "patch_word: %s+0x%x = 0x%08x @0x%lx\n", sym, off, insn,
          (unsigned long)a);
}

static void load_module(const char *name, int heap_mb, DynLibFunction *tbl, int n) {
  size_t hs = (size_t)heap_mb * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) { fprintf(stderr, "mmap %d MB falhou\n", heap_mb); exit(1); }
  fprintf(stderr, "== carregando %s (heap %p, %d MB) ==\n", name, heap, heap_mb);
  if (so_load(name, heap, hs) < 0) { fprintf(stderr, "so_load(%s) falhou\n", name); exit(1); }
  if (so_relocate() < 0) { fprintf(stderr, "so_relocate falhou\n"); exit(1); }
  so_resolve(tbl, n, 0);
  so_finalize();
  so_flush_caches();
  so_execute_init_array();
  fprintf(stderr, "== %s: text=%p+%zu data=%p+%zu ==\n", name,
          text_base, text_size, data_base, data_size);
}

/* ---- fox JNI entry points (Java_com_mineloader_fox_foxJniLib_*) ---- */
typedef void *JEnv;
static struct {
  void (*init)(JEnv, void *, void *, void *);
  void (*SetGamePath)(JEnv, void *, void *, void *);
  void (*coreGetLPKFileInfo)(JEnv, void *, void *, void *);
  void (*SetLanguageId)(JEnv, void *, int);
  void (*DrawEGLCreated)(JEnv, void *);
  void (*DrawFrame)(JEnv, void *);
  void (*GameProcess)(JEnv, void *);
  void (*FileProcess)(JEnv, void *);
  int  (*HasController)(JEnv, void *);
  void (*SetPadData)(JEnv, void *, int, int, int, int, int, int);
  void (*SetTPData)(JEnv, void *, int, int, int, int);
  void (*resumeEvent)(JEnv, void *);
  void (*WindowFocusChanged)(JEnv, void *, int);
} fox;

#define RES(f, sym) do { fox.f = (void *)so_find_addr_safe(sym); \
  fprintf(stderr, "resolve %-22s = %p\n", #f, (void *)fox.f); } while (0)

static void *g_env, *g_thiz;
/* thread dedicada de file-system: roda amFS_proc (loop infinito, cond_wait). */
static void *fs_thread_fn(void *arg) {
  (void)arg;
  fox.FileProcess(g_env, g_thiz);
  return NULL;
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  fprintf(stderr, "=== SONIC 4 EPISODE II (NN/fox) so-loader / NextOS armv7 Mali-450 ===\n");

  jni_shim_set_package("com.sega.sonic4episode2", 22);
  { const char *d = getenv("SONIC_DATADIR"); jni_shim_set_local_path(d ? d : "."); }

  build_base_table();
  load_module(GAME_SO, GAME_HEAP_MB, g_base, g_base_n);

  /* destravar trial -> jogo completo: GsTrialIsTrial() -> 0 */
  patch_ret0("_Z14GsTrialIsTrialv");
  patch_ret0("_Z21GsTrialIsTrial_VerTwov");
  /* sem camada de vídeo: forçar "vídeo não está tocando" p/ o game passar do
     intro (clMovie poll videoIsPlaying/MediaPlayerisPlaying espera o fim). */
  if (!getenv("SONIC_KEEPVIDEO")) {
    patch_ret0("_Z14videoIsPlayingv");
    patch_ret0("_Z20MediaPlayerisPlayingi");
    /* clMovie::willPlayMovieBeforeGameStart(int) -> 0: pula o movie de intro
       (SEGA logo) de vez, sem precisar da camada de vídeo. */
    patch_ret0("_ZNK2gm5movie7clMovie28willPlayMovieBeforeGameStartEi");
    /* clMovie::isEnd() -> 1: o game espera o movie de intro "terminar". */
    patch_retval("_ZN2gm5movie7clMovie5isEndEv", 1);
  }
  /* Sonic4F2F::isGamePause -> 0: o F2F pausa o jogo (ads/consent que não temos)
     e fox_FrameUpdate pula amTaskExecute (state machine) qdo pausado -> preto. */
  if (!getenv("SONIC_KEEPPAUSE"))
    patch_ret0("_ZN9Sonic4F2F11isGamePauseEv");
  /* 🔑 SJni_IsUpshellShow -> 0: chama CallBooleanMethod JNI ("a tela de upsell/ads
     está aberta?"). Nosso jni_shim devolve 1 (true) por default => a engine acha que
     o upshell está SEMPRE aberto e o título trava em CStateInitialize::Next pra sempre
     (o gate 1 nunca libera). Forçar 0 (nenhum upsell) destrava a state machine do título. */
  if (!getenv("SONIC_KEEPUPSHELL"))
    patch_ret0("_Z18SJni_IsUpshellShowv");
  /* 🔑 GATE DO MENU (title -> menu): CStateWaitSignIn::Next só avança pro menu
     (state 0x43) se GsUserSetupIsCompleted()!=0 E GsUserIsEnable()!=0 (setup de
     conta Google Play Games / online). Sem login, o título volta pro Waiting (loop).
     Forçar ambos -> 1 (usuário "configurado/habilitado") destrava título -> menu. */
  if (!getenv("SONIC_KEEPSIGNIN")) {
    patch_retval("_Z22GsUserSetupIsCompletedm", 1);
    patch_retval("_Z14GsUserIsEnablem", 1);
  }
  /* gate3 do título: CStateInitialize::Next espera CDemoResourceManager IsValid()
     (recursos do attract-demo do evento 3) que nunca valida (id 2 não carrega).
     NOP no `beq` (offset +0x5c) faz a state machine avançar pro Opening/LogoMainFadeIn
     (mostra o título: bg + logo SONIC, que carregam) -> Waiting, SEM o crash que
     forçar IsValid->1 global causava (over-advance pro save). */
  if (!getenv("SONIC_KEEPDEMOGATE"))
    patch_word_at("_ZN2dm5title16CStateInitialize4NextEv", 0x5c, 0xe1a00000);
  /* fallback opt-in: forçar IsValid->1 global (crasha no save, só p/ depurar). */
  if (getenv("SONIC_FORCEDEMOGATE"))
    patch_retval("_ZThn20_NK2dm8resource20CResourceManagerTask7IsValidENS0_12EDemoEventID4TypeE", 1);
  /* 🔑 fingir SÓ o attract-demo do título (resType 6) como carregado, mantendo os
     outros recursos REAIS (evita o crash do force-global IsValid->1). A função local
     do is-loaded do title-demo = container CManagerState<CTitleViewTask>::Act+0x20
     (0x2522a4): checa o objeto demo global (null)->0. Forçar return 1 deixa o
     demo-gate do título/menu passar SEM o attract-demo (emblema vazio). */
  if (!getenv("SONIC_NOFAKESOUND")) {
    /* o is-loaded que mais falha no demo-gate = dmSoundEffectIsSetUpEnd (o som do
       demo: container criado pelo setup, mas o is-loaded checa vtable[12]=dados de
       som carregados). Fingir ->1 (sem som no demo é inofensivo). */
    patch_retval("_ZN2dm2se23dmSoundEffectIsSetUpEndEv", 1);
  }

  /* 🔑 F2F age gate / GDPR consent: ao apertar "Press any button" o jogo chama
     showAgeGate (dialog Java de idade/consent que não temos) e espera a resposta.
     Bypass: isEnoughtAge->1 (idade ok => haveRemoveAgeGate=1) + isConsentCountry->0
     (não é país GDPR => pula o consent). Assim avança do título pro menu sem o dialog. */
  if (!getenv("SONIC_KEEPAGEGATE")) {
    patch_retval("_ZN12F2FExtension12isEnoughtAgeEv", 1);
    patch_ret0("_ZN12F2FExtension16isConsentCountryEv");
    patch_ret0("_ZN12F2FExtension19getIsConsentCountryEv");
  }

  void *env = NULL, *vm = NULL;
  jni_shim_init(&vm, &env);
  void *thiz = (void *)0x53000001; /* fake jobject/jclass */
  g_env = env; g_thiz = thiz;

  /* JNI_OnLoad: o Android chamaria isso ao carregar o .so, setando o global
     JavaVM (f2fextension GetJNIEnv lê esse global -> NULL deref sem isso). */
  { int (*jniOnLoad)(void *, void *) = (void *)so_find_addr_safe("JNI_OnLoad");
    if (jniOnLoad) { int v = jniOnLoad(vm, NULL);
      fprintf(stderr, "JNI_OnLoad(vm) = 0x%x\n", v); }
    else fprintf(stderr, "AVISO: JNI_OnLoad não encontrado\n"); }

  egl_shim_create_window();
  egl_shim_bind_main();  /* GLSurfaceView: contexto current na thread do DrawFrame */

  RES(init,               "Java_com_mineloader_fox_foxJniLib_init");
  RES(SetGamePath,        "Java_com_mineloader_fox_foxJniLib_SetGamePath");
  RES(coreGetLPKFileInfo, "Java_com_mineloader_fox_foxJniLib_coreGetLPKFileInfo");
  RES(SetLanguageId,      "Java_com_mineloader_fox_foxJniLib_SetLanguageId");
  RES(DrawEGLCreated,     "Java_com_mineloader_fox_foxJniLib_DrawEGLCreated");
  RES(DrawFrame,          "Java_com_mineloader_fox_foxJniLib_DrawFrame");
  RES(GameProcess,        "Java_com_mineloader_fox_foxJniLib_GameProcess");
  RES(FileProcess,        "Java_com_mineloader_fox_foxJniLib_FileProcess");
  RES(HasController,      "Java_com_mineloader_fox_foxJniLib_HasController");
  RES(SetPadData,         "Java_com_mineloader_fox_foxJniLib_SetPadData");
  RES(SetTPData,          "Java_com_mineloader_fox_foxJniLib_SetTPData");
  RES(resumeEvent,        "Java_com_mineloader_fox_foxJniLib_resumeEvent");

  const char *gamedir = getenv("SONIC_DATADIR");
  if (!gamedir) gamedir = ".";
  const char *lpk = getenv("SONIC_LPK");
  if (!lpk) lpk = "data/main.22.com.sega.sonic4episode2.obb"; /* o OBB = LPK */

  /* SetGamePath(env, thiz, int id, String path) -> tsSetFileRootPath(id,path,0):
     id==255(0xff) => tsInitFileRootLPK(path) = fopen+indexa o LPK (o OBB!);
     id<254       => guarda 'path' como root de arquivos soltos.
     PRECISA vir ANTES do init (init lê font.nft de dentro do LPK). */
  fprintf(stderr, "=== fox: SetGamePath(255, LPK=%s) ===\n", lpk);
  if (fox.SetGamePath) fox.SetGamePath(env, thiz, (void *)255, jni_shim_new_string(lpk));

  fprintf(stderr, "=== fox: SetGamePath(0, dir=%s) ===\n", gamedir);
  if (fox.SetGamePath) fox.SetGamePath(env, thiz, (void *)0, jni_shim_new_string(gamedir));

  /* idioma: tabela lang_name_tbl = 0:JP 1:US(inglês) 2:FR 3:IT 4:GE 5:SP 6:KO 7:CH 8:TA.
     Id 1 = US/inglês (id 0 carregava as variantes _JP japonesas). */
  fprintf(stderr, "=== fox: SetLanguageId(1=US/EN) ===\n");
  if (fox.SetLanguageId) fox.SetLanguageId(env, thiz, 1);
  /* 🔑 SetLanguageId seta o global do SetAndroidLanguage (usado no TÍTULO), mas o
     MENU lê de OUTRO global via GsEnvGetLanguage() (default 0=JP) -> menu em japonês!
     Forçar GsEnvGetLanguage()->1 (US) deixa o menu/UI em inglês também. */
  if (!getenv("SONIC_KEEPJP"))
    patch_retval("_Z16GsEnvGetLanguagev", 1);

  /* f2fextension (camada F2F/ads): precisa do context/JavaVM senão getF2FJavaVM()
     retorna NULL -> crash em Android_getLocalPath. Chamar os setups JNI. */
  void (*f2f_setCtx)(JEnv, void *, void *) =
      (void *)so_find_addr_safe("Java_com_sega_f2fextension_f2fextensionInterface_nativeSetContext");
  void (*f2f_setObj)(JEnv, void *, void *) =
      (void *)so_find_addr_safe("Java_com_sega_f2fextension_f2fextensionInterface_SetJavaObj");
  void (*f2f_setApk)(JEnv, void *, void *) =
      (void *)so_find_addr_safe("Java_com_sega_f2fextension_f2fextensionInterface_nativeSetApkPath");
  if (f2f_setObj) { fprintf(stderr, "f2f SetJavaObj\n");    f2f_setObj(env, thiz, thiz); }
  if (f2f_setCtx) { fprintf(stderr, "f2f nativeSetContext\n"); f2f_setCtx(env, thiz, thiz); }
  if (f2f_setApk) { fprintf(stderr, "f2f nativeSetApkPath\n");
                    f2f_setApk(env, thiz, jni_shim_new_string("sonic4ep2.apk")); }

  /* 🔑 setScreenSize: a engine (foxShaderInit -> amRenderCreate) lê a resolução de
     tela de um global (2 floats) p/ dimensionar os FBOs/render targets. O Java
     chamaria setScreenSize(w,h) no onSurfaceChanged; SEM isso o global fica 0.0/0.0
     => FBO 0x0 INCOMPLETE => glDraw* falham (GL_INVALID_FRAMEBUFFER_OPERATION) =>
     TELA PRETA. setScreenSize faz `stm {w,h}` cru => são jfloat em regs CORE (JNI
     softfp) => passamos os BITS do float em r2/r3 (declarar args como unsigned, NÃO
     float, senão o ABI hardfp manda em s0/s1). */
  {
    extern int dys_screen_w, dys_screen_h;
    void (*setScreenSize)(void *, void *, unsigned, unsigned) =
        (void *)so_find_addr_safe(
            "Java_com_sega_f2fextension_f2fextensionInterface_setScreenSize");
    void (*setScreenScaleDesity)(void *, void *, unsigned) =
        (void *)so_find_addr_safe(
            "Java_com_sega_f2fextension_f2fextensionInterface_setScreenScaleDesity");
    if (setScreenSize) {
      union { float f; unsigned u; } w, h;
      w.f = (float)dys_screen_w; h.f = (float)dys_screen_h;
      fprintf(stderr, "=== setScreenSize(%d x %d) bits=%08x %08x ===\n",
              dys_screen_w, dys_screen_h, w.u, h.u);
      setScreenSize(env, thiz, w.u, h.u);
    } else fprintf(stderr, "AVISO: setScreenSize não encontrado\n");
    if (setScreenScaleDesity) {
      union { float f; unsigned u; } s; s.f = 1.0f;
      setScreenScaleDesity(env, thiz, s.u);  /* densidade/escala = 1.0 */
    }
  }

  /* 🔑🔑 init(env, thiz, WIDTH, HEIGHT): a JNI init repassa args 3/4 p/ fox_Init(w,h)
     -> amDrawInitVideo(w,h) que dimensiona _am_draw_video (os FBOs/render targets).
     Passávamos NULL,NULL = 0,0 => FBO 0x0 INCOMPLETE => glDraw* falham => TELA PRETA.
     Passar a resolução REAL (1280x720) faz os FBOs ficarem completos e renderizar. */
  fprintf(stderr, "=== fox: init(w=%d h=%d) ===\n", dys_screen_w, dys_screen_h);
  if (fox.init) fox.init(env, thiz, (void *)(intptr_t)dys_screen_w,
                         (void *)(intptr_t)dys_screen_h);

  fprintf(stderr, "=== fox: DrawEGLCreated ===\n");
  if (fox.DrawEGLCreated) fox.DrawEGLCreated(env, thiz);

  /* resumeEvent: o jogo começa PAUSADO (isGamePause); sem resume, fox_FrameUpdate
     retorna cedo e PULA amTaskExecute (a state machine) -> nada avança -> preto. */
  fprintf(stderr, "=== fox: resumeEvent (unpause) ===\n");
  if (fox.resumeEvent) fox.resumeEvent(env, thiz);

  /* FileProcess = amFS_proc = loop da THREAD de file-system (cond_wait quando
     ocioso). Roda na PRÓPRIA thread; o game thread enfileira requests e sinaliza. */
  if (fox.FileProcess) {
    pthread_t fs;
    pthread_create(&fs, NULL, fs_thread_fn, NULL);
    fprintf(stderr, "=== FS thread iniciada (FileProcess) ===\n");
  }

  /* intro video: a engine chama Android_playIntroVideo e espera o callback
     callBackIntroVideo (Java tocaria o mp4 e sinalizaria o fim). Sem vídeo,
     sinalizamos "terminado" cedo p/ a engine seguir pro título/menu. */
  void (*introCB)(JEnv, void *) =
      (void *)so_find_addr_safe("Java_com_sega_f2fextension_f2fextensionInterface_callBackIntroVideo");

  /* input: abrir o 1º gamepad SDL (se houver) */
  SDL_GameController *pad = NULL;
  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
    for (int i = 0; i < SDL_NumJoysticks(); i++)
      if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); if (pad) break; }
    fprintf(stderr, "=== gamepad: %s ===\n", pad ? "aberto" : "nenhum");
  }
  /* fox pad bits (descobertos do disasm): 0x8000=confirm/A. Direções a refinar.
     Provisório: D-pad/analog -> direções; A/START -> 0x8000 (confirm). */
  #define FOX_UP     0x0001
  #define FOX_DOWN   0x0002
  #define FOX_LEFT   0x0004
  #define FOX_RIGHT  0x0008
  #define FOX_A      0x8020  /* 0x8000=confirm título(AoPadSomeoneStand) | 0x20=decide menu(IsPressedDecide) */
  #define FOX_B      0x0080
  #define FOX_START  0x0010

  /* 🔑 demo-resource: o gate do título (CDemoResourceManager::IsValid evt3) trava
     pq o recurso "MenuDraw" (tipo 2) não está setado (dmMenuDrawIsSetUpEnd=0).
     dmMenuDrawSetUp() cria o singleton MenuDraw -> o is-loaded passa. A engine não
     o chama no nosso fluxo; chamar aqui pode destravar o gate NATURALMENTE (sem o
     NOP do beq) e deixar o menu renderizar. */
  if (!getenv("SONIC_NOSETUPS")) {  /* default-on: cria os singletons do demo set —
       o IsValid quer os DADOS buildados (o build assíncrono do manager não completa;
       type 6=attract-demo precisa de stage). Default = título via bypass do beq. */
    const char *setups[] = {
      "_ZN2dm10menucommon17dmMenuCommonSetUpEv", /* type 1 */
      "_ZN2dm8menudraw15dmMenuDrawSetUpEv",      /* type 2 */
      "_ZN2dm2se18dmSoundEffectSetUpEv",         /* type 3 */
      "_ZN2dm7message11SystemSetUpEv",           /* message */
    };
    for (unsigned i = 0; i < sizeof(setups)/sizeof(setups[0]); i++) {
      void (*fn)(void) = (void *)so_find_addr_safe(setups[i]);
      if (fn) { fprintf(stderr, "=== setup: %s ===\n", setups[i]); fn(); }
      else fprintf(stderr, "AVISO: setup %s não encontrado\n", setups[i]);
    }
  }

  /* "conectar" o pad: SetPadData(-2) seta o flag de pad conectado (o Java faria
     isso). Sem isso o amPadExecute pode ignorar o estado de botões injetado. */
  if (fox.SetPadData) {
    fox.SetPadData(env, thiz, -2, 0, 0, 0, 0, 0);
    fox.SetPadData(env, thiz, -5, 0, 0, 0, 0, 0);
  }

  fprintf(stderr, "=== entrando no loop principal (GameProcess/DrawFrame) ===\n");
  unsigned long frame = 0;
  for (;;) {
    /* --- input: drenar eventos SDL + montar a máscara fox + SetPadData --- */
    SDL_Event ev; while (SDL_PollEvent(&ev)) { /* drena (quit etc) */ }
    int mask = 0;
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    if (ks) {
      if (ks[SDL_SCANCODE_UP])    mask |= FOX_UP;
      if (ks[SDL_SCANCODE_DOWN])  mask |= FOX_DOWN;
      if (ks[SDL_SCANCODE_LEFT])  mask |= FOX_LEFT;
      if (ks[SDL_SCANCODE_RIGHT]) mask |= FOX_RIGHT;
      if (ks[SDL_SCANCODE_RETURN]||ks[SDL_SCANCODE_SPACE]||ks[SDL_SCANCODE_Z]) mask |= FOX_A;
      if (ks[SDL_SCANCODE_X])     mask |= FOX_B;
      if (ks[SDL_SCANCODE_RETURN])mask |= FOX_START;
    }
    if (pad) {
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP))    mask |= FOX_UP;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  mask |= FOX_DOWN;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  mask |= FOX_LEFT;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) mask |= FOX_RIGHT;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A))     mask |= FOX_A;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B))     mask |= FOX_B;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) mask |= FOX_A|FOX_START;
      Sint16 ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
      Sint16 ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
      if (ax < -12000) mask |= FOX_LEFT; else if (ax > 12000) mask |= FOX_RIGHT;
      if (ay < -12000) mask |= FOX_UP;   else if (ay > 12000) mask |= FOX_DOWN;
    }
    /* auto-press de teste: o trigger é a borda 0->1 (1 frame), então alterna
       0x8000/0 a cada frame após o título carregar -> trigger frequente p/ vencer
       a corrida com o poll do CStateWaiting::Next (SONIC_AUTOSTART). */
    /* press ÚNICO de teste (não contínuo): aperta confirm uma vez ~frame 420 por
       ~5 frames e SOLTA (pressionar contínuo reseta a sequência de saída do título). */
    if (getenv("SONIC_AUTOSTART")) {
      if (frame >= 600 && frame < 606) mask |= FOX_A;    /* título -> menu */
      if (frame >= 1300 && (frame & 1)) mask |= FOX_A;  /* menu: decide toggle janela larga */
    }
    if (fox.SetPadData) fox.SetPadData(env, thiz, mask, 0, 0, 0, 0, 0);

    if (fox.GameProcess) fox.GameProcess(env, thiz);
    if (fox.DrawFrame)   fox.DrawFrame(env, thiz);
    if (getenv("SONIC_TESTCLEAR")) {  /* diagnóstico: present/contexto OK? */
      extern void glClearColor(float, float, float, float);
      extern void glClear(unsigned int);
      glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
      glClear(0x4000 /* GL_COLOR_BUFFER_BIT */);
    }
    /* PIXDIAG: localizar onde está o conteúdo (FB0 vs FBO interno). Lê o pixel
       central do framebuffer default (0) e de alguns FBOs antes do present. */
    if (getenv("SONIC_PIXDIAG") && (frame % 60) == 0 && frame > 120) {
      extern void glBindFramebuffer(unsigned, unsigned);
      extern void glReadPixels(int,int,int,int,unsigned,unsigned,void*);
      unsigned char px[4]; int fb;
      for (fb = 0; fb <= 3; fb++) {
        glBindFramebuffer(0x8D40 /*GL_FRAMEBUFFER*/, (unsigned)fb);
        px[0]=px[1]=px[2]=px[3]=0;
        glReadPixels(640, 360, 1, 1, 0x1908 /*GL_RGBA*/, 0x1401 /*UBYTE*/, px);
        fprintf(stderr, "[PIXDIAG f%lu] FB%d center=%02x %02x %02x %02x\n",
                frame, fb, px[0], px[1], px[2], px[3]);
      }
      glBindFramebuffer(0x8D40, 0);
    }
    egl_shim_present();
    /* sinaliza intro-video done nos primeiros segundos */
    if (introCB && frame >= 30 && frame < 120 && (frame % 15) == 0) introCB(env, thiz);
    if ((frame % 60) == 0) fprintf(stderr, "[frame %lu]\n", frame);
    frame++;
    usleep(16000);
  }
  return 0;
}
