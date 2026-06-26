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

  fprintf(stderr, "=== fox: SetLanguageId(0=EN) ===\n");
  if (fox.SetLanguageId) fox.SetLanguageId(env, thiz, 0);

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

  fprintf(stderr, "=== fox: init ===\n");
  if (fox.init) fox.init(env, thiz, NULL, NULL);

  fprintf(stderr, "=== fox: DrawEGLCreated ===\n");
  if (fox.DrawEGLCreated) fox.DrawEGLCreated(env, thiz);

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

  fprintf(stderr, "=== entrando no loop principal (GameProcess/DrawFrame) ===\n");
  unsigned long frame = 0;
  for (;;) {
    if (fox.GameProcess) fox.GameProcess(env, thiz);
    if (fox.DrawFrame)   fox.DrawFrame(env, thiz);
    if (getenv("SONIC_TESTCLEAR")) {  /* diagnóstico: present/contexto OK? */
      extern void glClearColor(float, float, float, float);
      extern void glClear(unsigned int);
      glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
      glClear(0x4000 /* GL_COLOR_BUFFER_BIT */);
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
