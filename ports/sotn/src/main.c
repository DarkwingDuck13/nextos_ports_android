// Castlevania: Symphony of the Night (DotEmu) so-loader for Mali-450 fbdev.
//
// libsotn.so links SDL2 2.0.8 statically and uses the standard SDLActivity
// flow.  We load it, resolve its imports against host libc/GLESv2/EGL/zlib
// plus our Android shims, build a fake JavaVM/JNIEnv, feed SDL the screen
// resolution, and finally call nativeRunMain() which runs the game's SDL_main
// (its own window/GL/render loop).  Rendering goes straight to Mali via the
// game's static SDL "android" video driver + our ANativeWindow->fbdev_window
// shim and dlopen()->libEGL passthrough.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

#define SO_NAME "libsotn.so"
#define HEAP_MB 64

// SDL pixel format passed to onNativeResize (RGB888). GL config is chosen by
// SDL/EGL independently; this is just what SDL reports as the display mode.
#define SDL_PIXELFORMAT_RGB888 0x16161804u

typedef int jint;

extern int g_sotn_screen_w;
extern int g_sotn_screen_h;

static jint (*p_JNI_OnLoad)(void *vm, void *reserved);
static void (*p_nativeSetupJNI)(void *env, void *cls);
static void (*p_AudioManager_nativeSetupJNI)(void *env, void *cls);
static void (*p_ControllerManager_nativeSetupJNI)(void *env, void *cls);
static void (*p_onNativeResize)(void *env, void *cls, jint w, jint h, jint fmt,
                                float rate);
static jint (*p_nativeRunMain)(void *env, void *cls, void *library,
                               void *function, void *arg_array);

/* CANARY BIONIC fix (proven on Bully/Dysmantle): libsotn is bionic-compiled and
 * reads the stack-guard from tpidr_el0+0x28 (TLS_SLOT_STACK_GUARD). Under glibc
 * that offset lands in a TCB field that changes at runtime -> prologue reads X,
 * epilogue reads Y -> __stack_chk_fail -> abort. Reserving a never-written
 * _Thread_local pad in the loader image shifts the static-TLS layout so
 * tpidr+0x28 falls inside this pad -> stable -> the canary never mismatches.
 * `used` keeps the linker from dropping it; anchored by a volatile read. */
__attribute__((used, aligned(16))) _Thread_local char g_bionic_guard_pad[256];

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  { volatile char c = g_bionic_guard_pad[0]; (void)c; } // anchor TLS pad
  debugPrintf("=== Castlevania SOTN so-loader (Mali-450) ===\n");

  const char *ws = getenv("SOTN_W");
  const char *hs = getenv("SOTN_H");
  if (ws)
    g_sotn_screen_w = atoi(ws);
  if (hs)
    g_sotn_screen_h = atoi(hs);
  if (g_sotn_screen_w <= 0)
    g_sotn_screen_w = 1280;
  if (g_sotn_screen_h <= 0)
    g_sotn_screen_h = 720;
  debugPrintf("Screen: %dx%d\n", g_sotn_screen_w, g_sotn_screen_h);

  // Make SDL pick the "android" video + audio drivers (audio -> JNI -> our
  // pacat sink). Don't force dummy.
  setenv("SDL_VIDEODRIVER", "android", 1);
  extern void sotn_audio_init(void);
  sotn_audio_init();

  // Pull libz (and GLES/EGL) into the global symbol scope so so_resolve's
  // dlsym(RTLD_DEFAULT) fallback can resolve inflate/crc32/gl*/egl* etc.
  const char *globlibs[] = {"libz.so.1",      "libz.so",  "libGLESv2.so",
                            "libGLESv1_CM.so", "libEGL.so", NULL};
  for (int i = 0; globlibs[i]; i++)
    dlopen(globlibs[i], RTLD_NOW | RTLD_GLOBAL);

  size_t heap_size = (size_t)HEAP_MB * 1024 * 1024;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED)
    fatal_error("mmap %d MB failed", HEAP_MB);

  if (so_load(SO_NAME, heap, heap_size) < 0)
    fatal_error("so_load(%s) failed", SO_NAME);
  debugPrintf("Loaded %s: text=%p+%zu data=%p+%zu\n", SO_NAME, text_base,
              text_size, data_base, data_size);

  if (so_relocate() < 0)
    fatal_error("so_relocate failed");
  if (so_resolve(dynlib_functions, dynlib_functions_count, 0) < 0)
    fatal_error("so_resolve failed");

  so_finalize();
  so_flush_caches();
  so_execute_init_array();

  p_JNI_OnLoad = (void *)so_find_addr_safe("JNI_OnLoad");
  p_nativeSetupJNI =
      (void *)so_find_addr_safe("Java_org_libsdl_app_SDLActivity_nativeSetupJNI");
  p_AudioManager_nativeSetupJNI = (void *)so_find_addr_safe(
      "Java_org_libsdl_app_SDLAudioManager_nativeSetupJNI");
  p_ControllerManager_nativeSetupJNI = (void *)so_find_addr_safe(
      "Java_org_libsdl_app_SDLControllerManager_nativeSetupJNI");
  p_onNativeResize =
      (void *)so_find_addr_safe("Java_org_libsdl_app_SDLActivity_onNativeResize");
  p_nativeRunMain =
      (void *)so_find_addr_safe("Java_org_libsdl_app_SDLActivity_nativeRunMain");

  if (!p_nativeRunMain || !p_nativeSetupJNI)
    fatal_error("missing SDL entry points (nativeRunMain/nativeSetupJNI)");

  void *fake_vm = NULL, *fake_env = NULL;
  jni_shim_init(&fake_vm, &fake_env);

  static int fake_activity_class;
  void *cls = &fake_activity_class;

  debugPrintf("JNI_OnLoad...\n");
  if (p_JNI_OnLoad)
    p_JNI_OnLoad(fake_vm, NULL);

  debugPrintf("nativeSetupJNI...\n");
  p_nativeSetupJNI(fake_env, cls);
  if (p_AudioManager_nativeSetupJNI)
    p_AudioManager_nativeSetupJNI(fake_env, cls);
  if (p_ControllerManager_nativeSetupJNI)
    p_ControllerManager_nativeSetupJNI(fake_env, cls);

  if (p_onNativeResize) {
    debugPrintf("onNativeResize(%d,%d)...\n", g_sotn_screen_w, g_sotn_screen_h);
    p_onNativeResize(fake_env, cls, g_sotn_screen_w, g_sotn_screen_h,
                     SDL_PIXELFORMAT_RGB888, 60.0f);
  }

  extern void sotn_input_start(void *env, void *cls);
  sotn_input_start(fake_env, cls);

  void *j_lib = jni_shim_make_jstring(SO_NAME);
  void *j_fn = jni_shim_make_jstring("SDL_main");

  debugPrintf("nativeRunMain -> SDL_main ...\n");
  jint rc = p_nativeRunMain(fake_env, cls, j_lib, j_fn, NULL);
  debugPrintf("nativeRunMain returned %d\n", (int)rc);

  return 0;
}
