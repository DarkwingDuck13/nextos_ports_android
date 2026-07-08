/* gtasa_stubs.c -- imports específicos do GTA San Andreas que NÃO existem no
 * glibc/GLES/libc++ do device (Android/Rockstar/OpenSL/cxa_guard). Portado do
 * default_dynlib do fork Vita (TheOfficialFloW/gtasa_vita, MIT), traduzido para
 * aarch64/Linux. Esta tabela é anexada à tabela combinada do módulo B (libGTASA)
 * ANTES do fallback dlsym do so_resolve, então cobre os símbolos que ficariam
 * "UNRESOLVED" (e crashariam ao serem chamados). Nomes conferidos 1:1 contra
 * `nm -D -u libGTASA.so`. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include "so_util.h"

/* desvio de pthread_create (definido em jni_shim.c) */
extern int gtasa_pthread_create(void *th, const void *attr, void *start, void *arg);
/* captura de raise/abort do jogo (definidos em jni_shim.c) */
extern int  gtasa_raise(int sig);
extern void gtasa_abort(void);

/* --- retornos triviais (x0 = 0/1) --- */
static int s_ret0(void) { return 0; }
static int s_ret1(void) { return 1; }
/* OpenSL slCreateEngine(SLObjectItf* pEngine, ...): retorna FALHA (nao-zero) p/ o
 * subsistema de audio abortar a init de forma limpa (sem deref de engine nulo).
 * SL_RESULT_FEATURE_UNSUPPORTED = 8. Áudio fica off até bridge OpenSL->OpenAL. */
static int s_slCreateEngine(void) { return 8; }

/* --- __cxa_guard (Itanium ABI, byte 0 = inicializado) --- */
static int  s_cxa_guard_acquire(char *g) { return g && g[0] == 0; }
static void s_cxa_guard_release(char *g) { if (g) g[0] = 1; }
static void s_cxa_guard_abort(char *g)   { (void)g; }
static void s_cxa_pure_virtual(void)     { fprintf(stderr, "[gtasa] __cxa_pure_virtual chamado\n"); }

/* --- símbolos de DADOS (o jogo lê/escreve ints; OpenSL IDs = ponteiros opacos) --- */
static int  d_EnterGameFromSCFunc = 0;
static int  d_SigningOutfromApp   = 0;
static int  d_hasTouchScreen      = 0;   /* 0 = sem touch (device é gamepad) */
static int  d_RTPrioLevel         = 0;
static int  d_SL_IID_ENGINE       = 0;
static int  d_SL_IID_PLAY         = 0;
static int  d_SL_IID_BUFFERQUEUE  = 0;
static int  d_SL_IID_ANDROIDSIMPLEBUFFERQUEUE = 0;

DynLibFunction gtasa_stub_table[] = {
  /* (AAsset_getLength/getRemainingLength/seek agora vêm REAIS do bully_stub_table
   * -> aa_getLength64/etc. Antes estavam ret0 aqui e travavam o CText::Load.) */

  /* saves na nuvem Rockstar (offline) */
  {"cloudGetBufferLen",  (uintptr_t)s_ret0}, {"cloudGetBufferPtr", (uintptr_t)s_ret0},
  {"cloudGetFree",       (uintptr_t)s_ret0}, {"cloudIsBusy",       (uintptr_t)s_ret0},
  {"cloudModAddWatch",   (uintptr_t)s_ret0}, {"cloudModFind",      (uintptr_t)s_ret0},
  {"cloudModReset",      (uintptr_t)s_ret0}, {"cloudStartCheckMod",(uintptr_t)s_ret0},
  {"cloudStartDownload", (uintptr_t)s_ret0}, {"cloudStartUpload",  (uintptr_t)s_ret0},
  {"GetCloudUploadResult",(uintptr_t)s_ret0},

  /* Social Club / Rockstar (offline) */
  {"GetRockstarID",            (uintptr_t)s_ret0},
  {"IsProfileStatsBusy",       (uintptr_t)s_ret1},
  {"scmainUpdate",             (uintptr_t)s_ret0},
  {"_Z12IsSCSignedInv",        (uintptr_t)s_ret0},
  {"_Z15EnterSocialCLubv",     (uintptr_t)s_ret0},
  {"_Z13SetJNEEnvFuncPFPvvE",  (uintptr_t)s_ret0},
  {"_Z16ProfileStatsSendPKci", (uintptr_t)s_ret0},

  /* telemetria (nuke) */
  {"_Z17TelemetryDataSendPKcS0_", (uintptr_t)s_ret0},
  {"_Z18TelemetryDataFlushv",     (uintptr_t)s_ret0},

  /* OpenSL ES (áudio off até bridge) */
  {"slCreateEngine",                    (uintptr_t)s_slCreateEngine},
  {"SL_IID_ENGINE",                     (uintptr_t)&d_SL_IID_ENGINE},
  {"SL_IID_PLAY",                       (uintptr_t)&d_SL_IID_PLAY},
  {"SL_IID_BUFFERQUEUE",                (uintptr_t)&d_SL_IID_BUFFERQUEUE},
  {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE",   (uintptr_t)&d_SL_IID_ANDROIDSIMPLEBUFFERQUEUE},

  /* EGL: init de display/contexto é NOSSA (egl_shim); estes ficam neutros */
  {"eglGetDisplay",   (uintptr_t)s_ret0},
  {"eglQueryString",  (uintptr_t)s_ret0},

  /* __cxa (guard de statics C++ roda no init_array ANTES do jni_load) */
  {"__cxa_guard_acquire", (uintptr_t)s_cxa_guard_acquire},
  {"__cxa_guard_release", (uintptr_t)s_cxa_guard_release},
  {"__cxa_guard_abort",   (uintptr_t)s_cxa_guard_abort},
  {"__cxa_pure_virtual",  (uintptr_t)s_cxa_pure_virtual},

  /* pthread_create: desvia o wrapper NVThreadSpawnProc da thread do loop
   * principal (TLS de NVThread não populado -> crash). Ver gtasa_pthread_create
   * em jni_shim.c. As outras threads passam direto pro glibc. */
  {"pthread_create", (uintptr_t)gtasa_pthread_create},
  {"raise", (uintptr_t)gtasa_raise},
  {"abort", (uintptr_t)gtasa_abort},

  /* sinais: o jogo instala o PRÓPRIO handler de crash via sigaction, o que
   * SOBRESCREVE o nosso (main.c) e vira core-dump sem diagnóstico. Vita stuba
   * ambos p/ ret0 -> o nosso handler pega o SIGSEGV e imprime libGTASA+offset. */
  {"sigaction",   (uintptr_t)s_ret0},
  {"sigemptyset", (uintptr_t)s_ret0},
  /* pthread_setname_np: ANDRunThread (wrapper de thread do engine) chama com um
   * `name` de arg parcialmente inicializado (pulamos NVThreadSpawnProc) -> a
   * glibc lê string lixo -> SIGSEGV. É só cosmético (nome da thread no debugger);
   * o Vita nunca nomeia threads. Stub -> a thread segue p/ pthread_setspecific +
   * a função real logo em seguida (0x321b4c). */
  {"pthread_setname_np", (uintptr_t)s_ret0},

  /* dados diversos */
  {"EnterGameFromSCFunc", (uintptr_t)&d_EnterGameFromSCFunc},
  {"SigningOutfromApp",   (uintptr_t)&d_SigningOutfromApp},
  {"hasTouchScreen",      (uintptr_t)&d_hasTouchScreen},
  {"RTPrioLevel",         (uintptr_t)&d_RTPrioLevel},
};
const int gtasa_stub_count = sizeof(gtasa_stub_table) / sizeof(gtasa_stub_table[0]);
