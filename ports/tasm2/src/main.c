/*
 * main.c — loader ARMHF (gerado por new-port-arm.sh) p/ tasm2.
 *
 * Multi-módulo bionic→glibc: carrega as dependências (libgenerator.so) cada uma
 * no seu heap + so_snapshot_symbols, acumula numa tabela combinada, e carrega o
 * módulo principal (libtasm2.so) resolvendo contra tudo + fallback dlsym. Acha
 * o entry (JNI_OnLoad / android_main). F2+ = boot do engine (JNI/NativeActivity).
 */
#include <setjmp.h>
#include <signal.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "android_shim.h"
#include "egl_shim.h"
#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

#define MEMORY_MB 320
#define SO_NAME "libtasm2.so"
extern int dys_screen_w, dys_screen_h;
__attribute__((aligned(16))) _Thread_local char g_bionic_guard_pad[256];

static void dump_pointer_words(const char *name, uintptr_t p, uintptr_t sp) {
  uintptr_t text = (uintptr_t)text_base;
  uintptr_t data = (uintptr_t)data_base;
  int readable = 0;

  if (p >= sp - 0x4000 && p + 16 >= p && p + 16 < sp + 0x8000)
    readable = 1;
  if (text_base && p >= text && p + 16 >= p && p + 16 < text + text_size)
    readable = 1;
  if (data_base && p >= data && p + 16 >= p && p + 16 < data + data_size)
    readable = 1;

  if (!readable)
    return;

  uintptr_t *w = (uintptr_t *)p;
  fprintf(stderr, "  %s[%p]: %08lx %08lx %08lx %08lx\n", name, (void *)p,
          (unsigned long)w[0], (unsigned long)w[1],
          (unsigned long)w[2], (unsigned long)w[3]);
}

static void dump_stack_scan(uintptr_t sp) {
  uintptr_t text = (uintptr_t)text_base;
  fprintf(stderr, "  --- stack scan (retornos no .so) ---\n");
  int n = 0;
  for (uintptr_t a = sp; a < sp + 0x2000 && n < 24; a += 4) {
    uintptr_t v = *(uintptr_t *)a;
    if (v >= text && v < text + text_size) {
      fprintf(stderr, "    [sp+0x%lx] %s+0x%lx\n", (unsigned long)(a - sp),
              SO_NAME, (unsigned long)(v - text));
      n++;
    }
  }
}

/* ---- crash handler ARMHF (campos arm_pc/arm_r0/arm_lr do sigcontext 32-bit) ---- */
static void crash_handler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  mcontext_t *m = &uc->uc_mcontext;
  uintptr_t pc = m->arm_pc, lr = m->arm_lr, fault = (uintptr_t)info->si_addr;
  uintptr_t text = (uintptr_t)text_base;

  fprintf(stderr, "\n=== CRASH sig=%d addr=%p ===\n", sig, (void *)fault);
  fprintf(stderr, "  tid=%ld\n", (long)syscall(SYS_gettid));
  fprintf(stderr, "  PC=%p", (void *)pc);
  if (pc >= text && pc < text + text_size)
    fprintf(stderr, " (%s+0x%lx)", SO_NAME, (unsigned long)(pc - text));
  else {
    Dl_info di;
    if (dladdr((void *)pc, &di) && di.dli_fname) {
      fprintf(stderr, " (%s+0x%lx", di.dli_fname,
              (unsigned long)(pc - (uintptr_t)di.dli_fbase));
      if (di.dli_sname)
        fprintf(stderr, ":%s+0x%lx", di.dli_sname,
                (unsigned long)(pc - (uintptr_t)di.dli_saddr));
      fprintf(stderr, ")");
    }
  }
  fprintf(stderr, "\n  LR=%p", (void *)lr);
  if (lr >= text && lr < text + text_size)
    fprintf(stderr, " (%s+0x%lx)", SO_NAME, (unsigned long)(lr - text));
  else {
    Dl_info di;
    if (dladdr((void *)lr, &di) && di.dli_fname) {
      fprintf(stderr, " (%s+0x%lx", di.dli_fname,
              (unsigned long)(lr - (uintptr_t)di.dli_fbase));
      if (di.dli_sname)
        fprintf(stderr, ":%s+0x%lx", di.dli_sname,
                (unsigned long)(lr - (uintptr_t)di.dli_saddr));
      fprintf(stderr, ")");
    }
  }
  fprintf(stderr, "\n");
  fprintf(stderr, "  r0=%08lx r1=%08lx r2=%08lx r3=%08lx\n",
          (unsigned long)m->arm_r0, (unsigned long)m->arm_r1,
          (unsigned long)m->arm_r2, (unsigned long)m->arm_r3);
  fprintf(stderr, "  r4=%08lx r5=%08lx r6=%08lx r7=%08lx\n",
          (unsigned long)m->arm_r4, (unsigned long)m->arm_r5,
          (unsigned long)m->arm_r6, (unsigned long)m->arm_r7);
  fprintf(stderr, "  r8=%08lx r9=%08lx r10=%08lx fp=%08lx ip=%08lx sp=%08lx\n",
          (unsigned long)m->arm_r8, (unsigned long)m->arm_r9,
          (unsigned long)m->arm_r10, (unsigned long)m->arm_fp,
          (unsigned long)m->arm_ip, (unsigned long)m->arm_sp);
  dump_pointer_words("r0", m->arm_r0, m->arm_sp);
  dump_pointer_words("r1", m->arm_r1, m->arm_sp);
  dump_pointer_words("r3", m->arm_r3, m->arm_sp);
  dump_pointer_words("r5", m->arm_r5, m->arm_sp);
  dump_stack_scan(m->arm_sp);

  if (sig == SIGSEGV && m->arm_r2 == SIGSEGV &&
      getenv("TASM2_IGNORE_SELF_SIGSEGV")) {
    fprintf(stderr, "  [TASM2] SIGSEGV autoenviado ignorado por teste\n");
    return;
  }

  void *bt[32];
  int bt_n = backtrace(bt, 32);
  fprintf(stderr, "  --- backtrace ---\n");
  for (int i = 0; i < bt_n; i++) {
    uintptr_t a = (uintptr_t)bt[i];
    fprintf(stderr, "    #%02d %p", i, bt[i]);
    if (a >= text && a < text + text_size) {
      fprintf(stderr, " (%s+0x%lx)", SO_NAME, (unsigned long)(a - text));
    } else {
      Dl_info di;
      if (dladdr(bt[i], &di) && di.dli_fname) {
        fprintf(stderr, " (%s+0x%lx", di.dli_fname,
                (unsigned long)(a - (uintptr_t)di.dli_fbase));
        if (di.dli_sname)
          fprintf(stderr, ":%s+0x%lx", di.dli_sname,
                  (unsigned long)(a - (uintptr_t)di.dli_saddr));
        fprintf(stderr, ")");
      }
    }
    fprintf(stderr, "\n");
  }

  fprintf(stderr, "=== END CRASH ===\n");
  fflush(stderr);
  _exit(128 + sig);
}

static void install_crash_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
}

/* tabela combinada acumulada (base + snapshots dos módulos já carregados) */
static DynLibFunction *g_comb;
static int g_comb_n;
static void comb_append(DynLibFunction *tbl, int n) {
  g_comb = realloc(g_comb, sizeof(DynLibFunction) * (g_comb_n + n));
  memcpy(g_comb + g_comb_n, tbl, sizeof(DynLibFunction) * n);
  g_comb_n += n;
}

static void preload_device_libs(void) {
  static const char *libs[] = {
      "libSDL2-2.0.so.0", "libGLESv2.so", "libEGL.so",
      "libOpenSLES.so", "libm.so.6", "libdl.so.2", NULL };
  for (int i = 0; libs[i]; i++) {
    void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr, "preload: %s %s\n", libs[i], h ? "OK" : dlerror());
  }
}

static int g_profile_counter_a;
static int g_profile_counter_b;
static int g_profile_flag_a;
static int g_profile_flag_b;

static void init_tasm2_profile_globals(void) {
  const char *counter = getenv("TASM2_PROFILE_COUNTER");
  const char *flag = getenv("TASM2_PROFILE_FLAG");
  if (counter && *counter) {
    g_profile_counter_a = atoi(counter);
    g_profile_counter_b = atoi(counter);
  }
  if (flag && *flag) {
    g_profile_flag_a = atoi(flag);
    g_profile_flag_b = atoi(flag);
  }
}

static void patch_tasm2_hidden_globals(void) {
  if (getenv("TASM2_SKIP_HIDDEN_GLOBALS")) {
    fprintf(stderr, "[TASM2] hidden globals: skip por ambiente\n");
    return;
  }

  init_tasm2_profile_globals();

  struct hidden_ptr_patch {
    uint32_t vaddr;
    int *target;
    const char *name;
  };

  static const struct hidden_ptr_patch patches[] = {
      {0x013b7b30u, &g_profile_counter_a, "profile_counter_a"},
      {0x013b7b34u, &g_profile_flag_a, "profile_flag_a"},
      {0x013b7b38u, &g_profile_counter_b, "profile_counter_b"},
      {0x013b7b3cu, &g_profile_flag_b, "profile_flag_b"},
  };

  if (!data_base) {
    fprintf(stderr, "[TASM2] hidden globals: data_base ausente\n");
    return;
  }

  for (size_t i = 0; i < sizeof(patches) / sizeof(patches[0]); i++) {
    if ((size_t)patches[i].vaddr + sizeof(uint32_t) > data_size) {
      fprintf(stderr, "[TASM2] hidden global %s fora do modulo: 0x%08x\n",
              patches[i].name, patches[i].vaddr);
      continue;
    }

    uint32_t *slot = (uint32_t *)((uintptr_t)data_base + patches[i].vaddr);
    uint32_t before = *slot;
    uint32_t target = (uint32_t)(uintptr_t)patches[i].target;
    if (before == 0 || getenv("TASM2_FORCE_HIDDEN_GLOBALS"))
      *slot = target;

    fprintf(stderr,
            "[TASM2] hidden global %-17s @0x%08x %08x -> %08x val=%d\n",
            patches[i].name, patches[i].vaddr, before, *slot,
            *patches[i].target);
  }
}

static void patch_tasm2_text_guards(void) {
  if (getenv("TASM2_SKIP_SENTINEL_PATCH")) {
    fprintf(stderr, "[TASM2] sentinel patch: skip por ambiente\n");
    return;
  }
  if (!text_base || 0x00ffc9f0u > text_size) {
    fprintf(stderr, "[TASM2] sentinel patch fora do modulo\n");
    return;
  }

  uint32_t *fn = (uint32_t *)((uintptr_t)text_base + 0x00ffc9e8u);
  uint32_t before0 = fn[0];
  uint32_t before1 = fn[1];
  so_make_text_writable();
  fn[0] = 0xe3a00000u; /* mov r0, #0 */
  fn[1] = 0xe12fff1eu; /* bx lr */
  __builtin___clear_cache((char *)fn, (char *)fn + 8);
  so_make_text_executable();
  fprintf(stderr,
          "[TASM2] sentinel patch @0x00ffc9e8 %08x %08x -> %08x %08x\n",
          before0, before1, fn[0], fn[1]);

  if (getenv("TASM2_SKIP_OBF_VALIDATION_PATCH")) {
    fprintf(stderr, "[TASM2] obf validation patch: skip por ambiente\n");
    return;
  }
  if (!text_base || 0x003f0158u > text_size) {
    fprintf(stderr, "[TASM2] obf validation patch fora do modulo\n");
    return;
  }

  uint32_t *fallback = (uint32_t *)((uintptr_t)text_base + 0x003f0150u);
  uint32_t old0 = fallback[0];
  uint32_t old1 = fallback[1];
  so_make_text_writable();
  fallback[0] = 0xe1a00008u; /* mov r0, r8 */
  fallback[1] = 0xeafffff6u; /* b 0x003f0134 */
  __builtin___clear_cache((char *)fallback, (char *)fallback + 8);
  so_make_text_executable();
  fprintf(stderr,
          "[TASM2] obf validation patch @0x003f0150 %08x %08x -> %08x %08x\n",
          old0, old1, fallback[0], fallback[1]);
}

static void patch_tasm2_gaia_local_status(void) {
  if (getenv("TASM2_SKIP_GAIA_STATUS_PATCH")) {
    fprintf(stderr, "[TASM2] gaia status patch: skip por ambiente\n");
    return;
  }
  if (!getenv("TASM2_FORCE_GAIA_STATUS_PATCH")) {
    fprintf(stderr, "[TASM2] gaia status patch: desligado por padrao\n");
    return;
  }
  if (!text_base || 0x00c8e34cu > text_size) {
    fprintf(stderr, "[TASM2] gaia status patch fora do modulo\n");
    return;
  }

  uint32_t *p = (uint32_t *)((uintptr_t)text_base + 0x00c8e340u);
  uint32_t old0 = p[0];
  uint32_t old1 = p[1];
  uint32_t old2 = p[2];
  so_make_text_writable();
  p[0] = 0xe3a03001u; /* mov r3, #1 */
  p[1] = 0xe584301cu; /* str r3, [r4, #28] */
  p[2] = 0xea000009u; /* b 0x00c8e374 */
  __builtin___clear_cache((char *)p, (char *)p + 12);
  so_make_text_executable();
  fprintf(stderr,
          "[TASM2] gaia status patch @0x00c8e340 %08x %08x %08x -> %08x %08x %08x\n",
          old0, old1, old2, p[0], p[1], p[2]);
}

static void patch_tasm2_gaia_service_flags(void) {
  if (!getenv("TASM2_DISABLE_GAIA_FLAGS"))
    return;
  if (!text_base || 0x00c8e020u > text_size) {
    fprintf(stderr, "[TASM2] gaia flags patch fora do modulo\n");
    return;
  }

  uint32_t *anon_flag = (uint32_t *)((uintptr_t)text_base + 0x00c8e014u);
  uint32_t *key_flag = (uint32_t *)((uintptr_t)text_base + 0x00c8e01cu);
  uint32_t old_anon = anon_flag[0];
  uint32_t old_key = key_flag[0];
  so_make_text_writable();
  anon_flag[0] = 0xe3a00000u; /* mov r0, #0 */
  key_flag[0] = 0xe3a00000u;  /* mov r0, #0 */
  __builtin___clear_cache((char *)anon_flag, (char *)key_flag + 8);
  so_make_text_executable();
  fprintf(stderr,
          "[TASM2] gaia flags patch @0x00c8e014/@0x00c8e01c %08x/%08x -> %08x/%08x\n",
          old_anon, old_key, anon_flag[0], key_flag[0]);
}

typedef int (*jni_onload_fn)(void *vm, void *reserved);
typedef void (*jni_void2_fn)(void *env, void *obj_or_class);
typedef void (*jni_string_arg_fn)(void *env, void *obj_or_class, void *str);
typedef void (*jni_resize_fn)(void *env, void *clazz, int width, int height);
typedef unsigned char (*jni_bool_long_fn)(void *env, void *clazz,
                                          long long value);
typedef void (*jni_key_fn)(void *env, void *clazz, int keycode);
typedef void (*jni_touch_fn)(void *env, void *clazz, int action, int x, int y,
                             int pointer_id);
typedef void (*jni_setpaths_fn)(void *env, void *clazz, void *p0, void *p1,
                                void *p2);

static const char *env_or_default(const char *name, const char *fallback) {
  const char *v = getenv(name);
  return (v && *v) ? v : fallback;
}

static int env_int_or_default(const char *name, int fallback) {
  const char *v = getenv(name);
  return (v && *v) ? atoi(v) : fallback;
}

static uintptr_t need_symbol(const char *name) {
  uintptr_t p = so_find_addr_safe(name);
  if (!p)
    fprintf(stderr, "[TASM2] simbolo ausente: %s\n", name);
  return p;
}

static SDL_GameController *g_tasm2_controller;
static int g_tasm2_start_down;
static int g_tasm2_select_down;
static int g_tasm2_axis_left;
static int g_tasm2_axis_right;
static int g_tasm2_axis_up;
static int g_tasm2_axis_down;

static void tasm2_open_controller(void) {
  if (g_tasm2_controller)
    return;
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (!SDL_IsGameController(i))
      continue;
    g_tasm2_controller = SDL_GameControllerOpen(i);
    if (g_tasm2_controller) {
      fprintf(stderr, "[TASM2] controller: %s\n",
              SDL_GameControllerName(g_tasm2_controller));
      break;
    }
  }
}

static int tasm2_scancode_to_keycode(SDL_Scancode sc) {
  switch (sc) {
  case SDL_SCANCODE_UP: return AKEYCODE_DPAD_UP;
  case SDL_SCANCODE_DOWN: return AKEYCODE_DPAD_DOWN;
  case SDL_SCANCODE_LEFT: return AKEYCODE_DPAD_LEFT;
  case SDL_SCANCODE_RIGHT: return AKEYCODE_DPAD_RIGHT;
  case SDL_SCANCODE_RETURN: return AKEYCODE_BUTTON_START;
  case SDL_SCANCODE_ESCAPE: return AKEYCODE_BUTTON_SELECT;
  case SDL_SCANCODE_X: return AKEYCODE_BUTTON_A;
  case SDL_SCANCODE_C: return AKEYCODE_BUTTON_B;
  case SDL_SCANCODE_Q: return AKEYCODE_BUTTON_X;
  case SDL_SCANCODE_T: return AKEYCODE_BUTTON_Y;
  case SDL_SCANCODE_H: return AKEYCODE_BUTTON_L1;
  case SDL_SCANCODE_J: return AKEYCODE_BUTTON_R1;
  case SDL_SCANCODE_K: return AKEYCODE_BUTTON_L2;
  case SDL_SCANCODE_L: return AKEYCODE_BUTTON_R2;
  case SDL_SCANCODE_N: return AKEYCODE_BUTTON_THUMBL;
  case SDL_SCANCODE_M: return AKEYCODE_BUTTON_THUMBR;
  default: return -1;
  }
}

static int tasm2_button_to_keycode(uint8_t button) {
  switch (button) {
  case SDL_CONTROLLER_BUTTON_A: return AKEYCODE_BUTTON_A;
  case SDL_CONTROLLER_BUTTON_B: return AKEYCODE_BUTTON_B;
  case SDL_CONTROLLER_BUTTON_X: return AKEYCODE_BUTTON_X;
  case SDL_CONTROLLER_BUTTON_Y: return AKEYCODE_BUTTON_Y;
  case SDL_CONTROLLER_BUTTON_START: return AKEYCODE_BUTTON_START;
  case SDL_CONTROLLER_BUTTON_BACK: return AKEYCODE_BUTTON_SELECT;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return AKEYCODE_BUTTON_L1;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return AKEYCODE_BUTTON_R1;
  case SDL_CONTROLLER_BUTTON_LEFTSTICK: return AKEYCODE_BUTTON_THUMBL;
  case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return AKEYCODE_BUTTON_THUMBR;
  case SDL_CONTROLLER_BUTTON_DPAD_UP: return AKEYCODE_DPAD_UP;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return AKEYCODE_DPAD_DOWN;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return AKEYCODE_DPAD_LEFT;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return AKEYCODE_DPAD_RIGHT;
  default: return -1;
  }
}

static void tasm2_send_key(void *env, void *clazz, uintptr_t on_key_down,
                           uintptr_t on_key_up, int keycode, int down) {
  if (keycode < 0)
    return;
  if (keycode == AKEYCODE_BUTTON_START)
    g_tasm2_start_down = down;
  if (keycode == AKEYCODE_BUTTON_SELECT)
    g_tasm2_select_down = down;
  if (g_tasm2_start_down && g_tasm2_select_down) {
    fprintf(stderr, "[TASM2] SELECT+START -> saindo\n");
    _exit(0);
  }

  uintptr_t fn = down ? on_key_down : on_key_up;
  if (!fn)
    return;
  if (getenv("TASM2_INPUT_DEBUG"))
    fprintf(stderr, "[TASM2] input key %s %d\n", down ? "down" : "up",
            keycode);
  ((jni_key_fn)fn)(env, clazz, keycode);
}

static void tasm2_send_center_touch(void *env, void *clazz, uintptr_t touch,
                                    int down) {
  if (!touch)
    return;
  int action = down ? 1 : 0;
  int x = dys_screen_w > 0 ? dys_screen_w / 2 : 640;
  int y = dys_screen_h > 0 ? dys_screen_h / 2 : 360;
  if (getenv("TASM2_INPUT_DEBUG"))
    fprintf(stderr, "[TASM2] input touch %s %d,%d\n",
            down ? "down" : "up", x, y);
  ((jni_touch_fn)touch)(env, clazz, action, x, y, 0);
}

static void tasm2_set_axis_key(void *env, void *clazz, uintptr_t on_key_down,
                               uintptr_t on_key_up, int *state, int down,
                               int keycode) {
  if (*state == down)
    return;
  *state = down;
  tasm2_send_key(env, clazz, on_key_down, on_key_up, keycode, down);
}

static int run_gl2jni_boot(void) {
  uintptr_t jni_onload = need_symbol("JNI_OnLoad");
  uintptr_t native_init =
      need_symbol("Java_com_gameloft_glf_GL2JNIActivity_nativeInit");
  uintptr_t set_paths = need_symbol("Java_com_gameloft_glf_GL2JNILib_setPaths");
  uintptr_t init = need_symbol("Java_com_gameloft_glf_GL2JNILib_init");
  uintptr_t resize = need_symbol("Java_com_gameloft_glf_GL2JNILib_resize");
  uintptr_t on_resume = need_symbol("Java_com_gameloft_glf_GL2JNILib_onResume");
  uintptr_t step = need_symbol("Java_com_gameloft_glf_GL2JNILib_step");
  uintptr_t on_key_down =
      so_find_addr_safe("Java_com_gameloft_glf_GL2JNILib_OnKeyDown");
  uintptr_t on_key_up =
      so_find_addr_safe("Java_com_gameloft_glf_GL2JNILib_OnKeyUp");
  uintptr_t touch_event =
      so_find_addr_safe("Java_com_gameloft_glf_GL2JNILib_touchEvent");
  uintptr_t end_splash =
      so_find_addr_safe("Java_com_gameloft_glf_GL2JNILib_EndSplashScreen");
  uintptr_t splash_func =
      so_find_addr_safe("Java_com_gameloft_glf_GL2JNILib_SplashScreenFunc");
  uintptr_t splash_glot = so_find_addr_safe(
      "Java_com_gameloft_android_ANMP_GloftASHM_SplashScreenActivity_splashScreenFuncGLOT");
  uintptr_t init_view_settings =
      so_find_addr_safe("Java_com_gameloft_glf_GL2JNILib_InitViewSettings");
  uintptr_t sutils_init = so_find_addr_safe(
      "Java_com_gameloft_android_ANMP_GloftASHM_GLUtils_SUtils_nativeInit");
  uintptr_t device_init = so_find_addr_safe(
      "Java_com_gameloft_android_ANMP_GloftASHM_GLUtils_Device_nativeInit");
  uintptr_t datasharing_init = so_find_addr_safe(
      "Java_com_gameloft_android_ANMP_GloftASHM_DataSharing_nativeInit");
  uintptr_t installer_init = so_find_addr_safe(
      "Java_com_gameloft_android_ANMP_GloftASHM_installer_GameInstaller_initNative");
  uintptr_t installer_start = so_find_addr_safe(
      "Java_com_gameloft_android_ANMP_GloftASHM_installer_GameInstaller_nativeStart");
  uintptr_t gdrm_init = so_find_addr_safe(
      "Java_com_gameloft_android_ANMP_GloftASHM_installer_GDRMPolicy_initNativeAP");
  uintptr_t gdrm_allow = so_find_addr_safe(
      "Java_com_gameloft_android_ANMP_GloftASHM_installer_GDRMPolicy_nativeAllow");
  uintptr_t gameoptions_resume = so_find_addr_safe(
      "Java_com_gameloft_gameoptions_GameOptions_onResumeGame");
  uintptr_t gameoptions_pause = so_find_addr_safe(
      "Java_com_gameloft_gameoptions_GameOptions_onPauseGame");
  /* GameAPI (GLSocialLib) — resultado do login online chega ASSINCRONO no
   * Android (thread worker), nao re-entrante dentro de InitGameAPI. Simulamos
   * isso disparando notify+complete do loop principal num frame posterior. */
  uintptr_t gameapi_init = so_find_addr_safe(
      "Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocialLib_nativeInit");
  uintptr_t gameapi_notify = so_find_addr_safe(
      "Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocialLib_nativeGameAPINotifyAuthChanges");
  uintptr_t gameapi_complete = so_find_addr_safe(
      "Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocialLib_nativeGameAPIComplete");
  uintptr_t platform_init = so_find_addr_safe(
      "Java_com_gameloft_GLSocialLib_PlatformAndroid_nativeInit");
  if (!jni_onload || !native_init || !init || !resize || !on_resume || !step)
    return -1;

  void *vm = NULL;
  void *env = NULL;
  static int fake_activity;
  static int fake_gl_class;
  static int fake_sutils_class;
  static int fake_device_class;
  static int fake_datasharing_class;
  static int fake_installer;
  static int fake_gdrm_policy;
  static int fake_gameoptions_class;
  static int fake_gameapi_class;

  jni_shim_init(&vm, &env);
  if (set_paths)
    jni_shim_set_setpaths_callback((jni_shim_setpaths_fn)set_paths);
  fprintf(stderr, "[TASM2] JNI fake vm=%p env=%p\n", vm, env);

  int ver = ((jni_onload_fn)jni_onload)(vm, NULL);
  fprintf(stderr, "[TASM2] JNI_OnLoad -> 0x%x\n", ver);
  if (ver < 0)
    return -1;

  ((jni_void2_fn)native_init)(env, &fake_activity);
  fprintf(stderr, "[TASM2] GL2JNIActivity.nativeInit OK\n");

  if (splash_glot && getenv("TASM2_CALL_SPLASH_GLOT")) {
    void *arg = jni_shim_make_string(env_or_default("TASM2_SPLASH_ARG", ""));
    ((jni_string_arg_fn)splash_glot)(env, &fake_activity, arg);
    fprintf(stderr, "[TASM2] SplashScreenActivity.splashScreenFuncGLOT OK\n");
  }

  if (!getenv("TASM2_SKIP_UTILS")) {
    if (sutils_init && !getenv("TASM2_SKIP_SUTILS")) {
      ((jni_void2_fn)sutils_init)(env, &fake_sutils_class);
      fprintf(stderr, "[TASM2] SUtils.nativeInit OK\n");
    }
    if (device_init && !getenv("TASM2_SKIP_DEVICE")) {
      ((jni_void2_fn)device_init)(env, &fake_device_class);
      fprintf(stderr, "[TASM2] Device.nativeInit OK\n");
    }
    if (datasharing_init && !getenv("TASM2_SKIP_DATASHARING")) {
      ((jni_void2_fn)datasharing_init)(env, &fake_datasharing_class);
      fprintf(stderr, "[TASM2] DataSharing.nativeInit OK\n");
    }
  }

  if (installer_init && getenv("TASM2_CALL_INSTALLER_INIT")) {
    ((jni_void2_fn)installer_init)(env, &fake_installer);
    fprintf(stderr, "[TASM2] GameInstaller.initNative OK\n");
  }
  if (installer_start && getenv("TASM2_CALL_INSTALLER_START")) {
    ((jni_void2_fn)installer_start)(env, &fake_installer);
    fprintf(stderr, "[TASM2] GameInstaller.nativeStart OK\n");
  }
  if (gdrm_init && getenv("TASM2_CALL_GDRM_INIT")) {
    ((jni_void2_fn)gdrm_init)(env, &fake_gdrm_policy);
    fprintf(stderr, "[TASM2] GDRMPolicy.initNativeAP OK\n");
  }
  if (gdrm_allow && getenv("TASM2_CALL_GDRM_ALLOW")) {
    const char *v = getenv("TASM2_GDRM_ALLOW_TIME");
    long long t = (v && *v) ? strtoll(v, NULL, 10) : 1538585115LL;
    unsigned char ok =
        ((jni_bool_long_fn)gdrm_allow)(env, &fake_gdrm_policy, t);
    fprintf(stderr, "[TASM2] GDRMPolicy.nativeAllow(%lld) -> %d\n", t, ok);
  }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
    fprintf(stderr, "[TASM2] SDL_Init falhou: %s\n", SDL_GetError());
    return -1;
  }
  tasm2_open_controller();
  egl_shim_create_window();
  egl_shim_make_bootstrap_current();

  ((jni_void2_fn)init)(env, &fake_gl_class);
  fprintf(stderr, "[TASM2] GL2JNILib.init OK\n");

  if (splash_func && getenv("TASM2_CALL_SPLASH_FUNC")) {
    void *arg = jni_shim_make_string(env_or_default("TASM2_SPLASH_ARG", ""));
    ((jni_string_arg_fn)splash_func)(env, &fake_gl_class, arg);
    fprintf(stderr, "[TASM2] GL2JNILib.SplashScreenFunc OK\n");
  }

  if (init_view_settings && getenv("TASM2_INIT_VIEWSETTINGS")) {
    ((jni_void2_fn)init_view_settings)(env, &fake_gl_class);
    fprintf(stderr, "[TASM2] GL2JNILib.InitViewSettings OK\n");
  }

  if (set_paths && getenv("TASM2_CALL_SETPATHS")) {
    const char *data_path = env_or_default(
        "TASM2_SETPATH_DATA",
        "/storage/emulated/0/Android/data/com.gameloft.android.ANMP.GloftASHM/files/");
    const char *private_path = env_or_default(
        "TASM2_SETPATH_PRIVATE",
        "/data/data/com.gameloft.android.ANMP.GloftASHM/");
    const char *obb_path = env_or_default(
        "TASM2_SETPATH_OBB",
        "/storage/roms/ports/tasm2/obb/");
    void *data = jni_shim_make_string(data_path);
    void *priv = jni_shim_make_string(private_path);
    void *obb = jni_shim_make_string(obb_path);
    ((jni_setpaths_fn)set_paths)(env, &fake_gl_class, data, priv, obb);
    fprintf(stderr, "[TASM2] GL2JNILib.setPaths OK data=%s private=%s obb=%s\n",
            data_path, private_path, obb_path);
  }

  ((jni_resize_fn)resize)(env, &fake_gl_class, 1280, 720);
  fprintf(stderr, "[TASM2] GL2JNILib.resize OK\n");
  ((jni_void2_fn)on_resume)(env, &fake_gl_class);
  fprintf(stderr, "[TASM2] GL2JNILib.onResume OK\n");
  if (gameoptions_resume && getenv("TASM2_CALL_GAMEOPTIONS_RESUME")) {
    ((jni_void2_fn)gameoptions_resume)(env, &fake_gameoptions_class);
    fprintf(stderr, "[TASM2] GameOptions.onResumeGame OK\n");
  }

  int end_splash_frame = env_int_or_default("TASM2_END_SPLASH_FRAME", -1);
  int end_splash_done = 0;
  if (end_splash && end_splash_frame >= 0) {
    fprintf(stderr, "[TASM2] GL2JNILib.EndSplashScreen agendado frame=%d\n",
            end_splash_frame);
  } else if (end_splash && getenv("TASM2_END_SPLASH")) {
    ((jni_void2_fn)end_splash)(env, &fake_gl_class);
    end_splash_done = 1;
    fprintf(stderr, "[TASM2] GL2JNILib.EndSplashScreen OK\n");
  }

  int frames = -1;
  frames = env_int_or_default("TASM2_FRAMES", -1);
  if (frames < 0)
    frames = -1;

  int gameapi_complete_frame =
      env_int_or_default("TASM2_GAMEAPI_COMPLETE_FRAME", -1);
  int gameapi_complete_done = 0;
  typedef void (*gameapi_notify_main_fn)(void *env, void *clazz,
                                         unsigned char logged, void *account);

  for (int i = 0; frames < 0 || i < frames; i++) {
    if (end_splash && !end_splash_done && end_splash_frame >= 0 &&
        i >= end_splash_frame) {
      ((jni_void2_fn)end_splash)(env, &fake_gl_class);
      end_splash_done = 1;
      fprintf(stderr, "[TASM2] GL2JNILib.EndSplashScreen OK frame=%d\n", i);
    }

    if (!gameapi_complete_done && gameapi_complete_frame >= 0 &&
        i >= gameapi_complete_frame) {
      gameapi_complete_done = 1;
      if (getenv("TASM2_GAMEAPI_COMPLETE_PLATFORM_INIT") && platform_init)
        ((jni_void2_fn)platform_init)(env, &fake_gameapi_class);
      if (getenv("TASM2_GAMEAPI_COMPLETE_INIT") && gameapi_init)
        ((jni_void2_fn)gameapi_init)(env, &fake_gameapi_class);
      if (gameapi_notify) {
        const char *account = env_or_default("TASM2_GAMEAPI_ACCOUNT",
                                             "nextos-tasm2");
        void *acc = jni_shim_make_string(account);
        ((gameapi_notify_main_fn)gameapi_notify)(env, &fake_gameapi_class, 1,
                                                 acc);
        fprintf(stderr, "[TASM2] deferido nativeGameAPINotifyAuthChanges OK\n");
      }
      if (gameapi_complete) {
        ((jni_void2_fn)gameapi_complete)(env, &fake_gameapi_class);
        fprintf(stderr, "[TASM2] deferido nativeGameAPIComplete OK frame=%d\n",
                i);
      }
    }

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT)
        return 0;
      if (ev.type == SDL_CONTROLLERDEVICEADDED) {
        tasm2_open_controller();
        continue;
      }
      if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
        if (g_tasm2_controller) {
          SDL_GameControllerClose(g_tasm2_controller);
          g_tasm2_controller = NULL;
        }
        continue;
      }
      if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
        if (ev.type == SDL_KEYDOWN && ev.key.repeat)
          continue;
        int keycode = tasm2_scancode_to_keycode(ev.key.keysym.scancode);
        tasm2_send_key(env, &fake_gl_class, on_key_down, on_key_up, keycode,
                       ev.type == SDL_KEYDOWN);
        if (keycode == AKEYCODE_BUTTON_A || keycode == AKEYCODE_BUTTON_START)
          tasm2_send_center_touch(env, &fake_gl_class, touch_event,
                                  ev.type == SDL_KEYDOWN);
        continue;
      }
      if (ev.type == SDL_CONTROLLERBUTTONDOWN ||
          ev.type == SDL_CONTROLLERBUTTONUP) {
        int keycode = tasm2_button_to_keycode(ev.cbutton.button);
        tasm2_send_key(env, &fake_gl_class, on_key_down, on_key_up, keycode,
                       ev.type == SDL_CONTROLLERBUTTONDOWN);
        if (keycode == AKEYCODE_BUTTON_A || keycode == AKEYCODE_BUTTON_START)
          tasm2_send_center_touch(env, &fake_gl_class, touch_event,
                                  ev.type == SDL_CONTROLLERBUTTONDOWN);
        continue;
      }
      if (ev.type == SDL_CONTROLLERAXISMOTION) {
        const int threshold = 16000;
        int v = ev.caxis.value;
        if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
          tasm2_set_axis_key(env, &fake_gl_class, on_key_down, on_key_up,
                             &g_tasm2_axis_left, v < -threshold,
                             AKEYCODE_DPAD_LEFT);
          tasm2_set_axis_key(env, &fake_gl_class, on_key_down, on_key_up,
                             &g_tasm2_axis_right, v > threshold,
                             AKEYCODE_DPAD_RIGHT);
        } else if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
          tasm2_set_axis_key(env, &fake_gl_class, on_key_down, on_key_up,
                             &g_tasm2_axis_up, v < -threshold,
                             AKEYCODE_DPAD_UP);
          tasm2_set_axis_key(env, &fake_gl_class, on_key_down, on_key_up,
                             &g_tasm2_axis_down, v > threshold,
                             AKEYCODE_DPAD_DOWN);
        }
        continue;
      }
    }
    if (getenv("TASM2_AUTOKEY") && i >= 180) {
      int cyc = (i - 180) % 90;
      if (cyc == 0)
        tasm2_send_key(env, &fake_gl_class, on_key_down, on_key_up,
                       AKEYCODE_BUTTON_A, 1);
      if (cyc == 0)
        tasm2_send_center_touch(env, &fake_gl_class, touch_event, 1);
      if (cyc == 12)
        tasm2_send_key(env, &fake_gl_class, on_key_down, on_key_up,
                       AKEYCODE_BUTTON_A, 0);
      if (cyc == 12)
        tasm2_send_center_touch(env, &fake_gl_class, touch_event, 0);
      if (i == 360)
        tasm2_send_key(env, &fake_gl_class, on_key_down, on_key_up,
                       AKEYCODE_BUTTON_START, 1);
      if (i == 360)
        tasm2_send_center_touch(env, &fake_gl_class, touch_event, 1);
      if (i == 372)
        tasm2_send_key(env, &fake_gl_class, on_key_down, on_key_up,
                       AKEYCODE_BUTTON_START, 0);
      if (i == 372)
        tasm2_send_center_touch(env, &fake_gl_class, touch_event, 0);
    }
    ((jni_void2_fn)step)(env, &fake_gl_class);
    if (!getenv("TASM2_SKIP_MANUAL_SWAP"))
      egl_shim_SwapBuffers(EGL_NO_DISPLAY, EGL_NO_SURFACE);
    SDL_Delay(16);
  }
  fprintf(stderr, "[TASM2] boot loop finalizado (%d frames)\n", frames);
  if (gameoptions_pause && getenv("TASM2_CALL_GAMEOPTIONS_PAUSE")) {
    ((jni_void2_fn)gameoptions_pause)(env, &fake_gameoptions_class);
    fprintf(stderr, "[TASM2] GameOptions.onPauseGame OK\n");
  }
  return 0;
}

/* carrega 1 módulo no seu próprio heap, reloca, resolve contra a tabela
 * combinada (+ fallback dlsym no so_resolve) e, se snapshot!=0, acumula os
 * símbolos exportados na combinada p/ os módulos seguintes. */
static int load_module(const char *name, int heap_mb, int snapshot) {
  size_t hs = (size_t)heap_mb * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) { fprintf(stderr, "mmap %s falhou\n", name); return -1; }
  debugPrintf("--- %s (heap %p, %d MB) ---\n", name, heap, heap_mb);
  if (so_load(name, heap, hs) < 0) { fprintf(stderr, "so_load %s falhou\n", name); return -1; }
  if (so_relocate() < 0) { fprintf(stderr, "so_relocate %s falhou\n", name); return -1; }
  if (so_resolve(g_comb ? g_comb : dynlib_functions,
                 g_comb ? g_comb_n : (int)dynlib_numfunctions, 0) < 0) {
    fprintf(stderr, "so_resolve %s falhou\n", name); return -1;
  }
  so_finalize();
  so_flush_caches();
  if (!getenv("TASM2_SKIP_INIT"))
    so_execute_init_array();
  if (snapshot) {
    int n = 0;
    DynLibFunction *t = so_snapshot_symbols(&n);
    if (t && n > 0) { comb_append(t, n); debugPrintf("%s: +%d símbolos exportados\n", name, n); }
  }
  return 0;
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  install_crash_handler();
  debugPrintf("=== tasm2 — loader ARMHF (Mali-450) ===\n");
  jni_shim_set_package("com.gameloft.android.ANMP.GloftASHM", 12032);
  preload_device_libs();

  /* base = shims bionic→glibc (os 18 que o dlsym fallback não cobre) */
  extern DynLibFunction port_shims[];
  extern int port_shims_count;
  extern DynLibFunction revc_pthread_table[];
  extern const int revc_pthread_count;
  g_comb_n = port_shims_count + revc_pthread_count;
  g_comb = malloc(sizeof(DynLibFunction) * g_comb_n);
  memcpy(g_comb, port_shims, sizeof(DynLibFunction) * port_shims_count);
  memcpy(g_comb + port_shims_count, revc_pthread_table,
         sizeof(DynLibFunction) * revc_pthread_count);

  /* dependências primeiro — cada uma vira fonte de símbolos p/ as seguintes.
   * (gerado: libc++_shared.so SEMPRE 1º; demais .so do APK depois) */
  if (load_module("libgenerator.so", 8, 1) < 0) return 1;
  /* módulo principal (resolve contra tudo acima + fallback dlsym) */
  if (load_module(SO_NAME, MEMORY_MB, 0) < 0) return 1;
  patch_tasm2_hidden_globals();
  patch_tasm2_text_guards();
  patch_tasm2_gaia_local_status();
  patch_tasm2_gaia_service_flags();

  uintptr_t jni_onload = so_find_addr_safe("JNI_OnLoad");
  uintptr_t android_main = so_find_addr_safe("android_main");
  debugPrintf("entry: JNI_OnLoad=%p android_main=%p (combinada=%d símbolos)\n",
              (void *)jni_onload, (void *)android_main, g_comb_n);

  if (!getenv("TASM2_SKIP_BOOT")) {
    int r = run_gl2jni_boot();
    fprintf(stderr, "[TASM2] GL2JNI boot -> %d\n", r);
    return r == 0 ? 0 : 2;
  }

  debugPrintf("=== F1 OK: multi-módulo carregado+resolvido. ===\n");
  return 0;
}
