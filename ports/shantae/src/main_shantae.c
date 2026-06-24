/*
 * main_shantae.c -- SHANTAE: and the Pirate's Curse (WayForward "Black" engine,
 * NativeActivity native, armv7) so-loader p/ NextOS armv7 + Mali-450 (Utgard,
 * GLES2 via SDL2). Módulo ÚNICO: libShantaeCurse_android.so (C++ estático, sem
 * libc++_shared separado). Entrada = android_main (native_app_glue clássico).
 *
 * Resolve imports via: shantae_overrides (imports.c) + revc_pthread_table
 * (pthread_bridge.c) + dlsym(RTLD_DEFAULT) das libs do device.
 *
 * Baseado nos so-loaders VERDES (Crazy Taxi / Terraria / Dysmantle): mesmo
 * framework so_util/egl_shim/android_shim/opensles_shim/jni_shim, portado p/
 * ELF32-ARM. Sem hooks específicos de outro jogo.
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
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "so_util.h"
#include "android_shim.h"
#include "egl_shim.h"
#include "jni_shim.h"

#define GAME_SO "lib/libShantaeCurse_android.so"
#define GAME_HEAP_MB 96
/* vaddr de android_main no .so (nm -D): usado p/ load_base (simbolicação). */
#define ANDROID_MAIN_VADDR 0x16cb84

extern DynLibFunction shantae_overrides[];
extern const int shantae_overrides_count;
extern DynLibFunction revc_pthread_table[];
extern const int revc_pthread_count;

static DynLibFunction *g_base;
static int g_base_n;
static void build_base_table(void) {
  g_base_n = shantae_overrides_count + revc_pthread_count;
  g_base = malloc(sizeof(DynLibFunction) * g_base_n);
  memcpy(g_base, shantae_overrides, sizeof(DynLibFunction) * shantae_overrides_count);
  memcpy(g_base + shantae_overrides_count, revc_pthread_table,
         sizeof(DynLibFunction) * revc_pthread_count);
}

volatile uintptr_t g_load_base = 0; /* exposto p/ imports.c (__gnu_Unwind_Find_exidx) */

/* resolve addr -> "mapname+off" lendo /proc/self/maps (async-signal-safe-ish) */
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
  fprintf(stderr, "\n=== CRASH sig=%d addr=%p tid=%d ===\n", sig, info->si_addr,
          (int)syscall(__NR_gettid));
  mcontext_t *m = &u->uc_mcontext;
  uintptr_t pc = m->arm_pc, lr = m->arm_lr;
  char r[300];
  resolve_addr(pc, r, sizeof(r));
  fprintf(stderr, "  PC=0x%lx %s", (unsigned long)pc, r);
  if (g_load_base && pc >= g_load_base)
    fprintf(stderr, "  {%s+0x%lx}", GAME_SO, (unsigned long)(pc - g_load_base));
  fprintf(stderr, "\n");
  resolve_addr(lr, r, sizeof(r));
  fprintf(stderr, "  LR=0x%lx %s", (unsigned long)lr, r);
  if (g_load_base && lr >= g_load_base)
    fprintf(stderr, "  {%s+0x%lx}", GAME_SO, (unsigned long)(lr - g_load_base));
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
  /* stack scan: retornos dentro do módulo do jogo */
  if (g_load_base) {
    fprintf(stderr, "  --- stack scan (game module) ---\n");
    uintptr_t sp = m->arm_sp; int cnt = 0;
    for (uintptr_t a = sp; a < sp + 0x2000 && cnt < 24; a += 4) {
      uintptr_t v = *(uintptr_t *)a;
      if (v >= g_load_base && v < g_load_base + (uintptr_t)GAME_HEAP_MB * 1024 * 1024) {
        fprintf(stderr, "    [sp+0x%lx] %s+0x%lx\n",
                (unsigned long)(a - sp), GAME_SO, (unsigned long)(v - g_load_base));
        cnt++;
      }
    }
  }
  fflush(stderr);
  _exit(128 + sig);
}

static void bt_handler(int sig, siginfo_t *info, void *uc) {
  (void)info;
  ucontext_t *u = (ucontext_t *)uc;
  mcontext_t *m = &u->uc_mcontext;
  char r[300]; resolve_addr(m->arm_pc, r, sizeof(r));
  fprintf(stderr, "\n[BT sig=%d tid=%d] PC=0x%lx %s", sig,
          (int)syscall(__NR_gettid), (unsigned long)m->arm_pc, r);
  if (g_load_base && m->arm_pc >= g_load_base)
    fprintf(stderr, " {%s+0x%lx}", GAME_SO, (unsigned long)(m->arm_pc - g_load_base));
  fprintf(stderr, "\n"); fflush(stderr);
}

static void install_crash_handler(void) {
  struct sigaction sa; memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL); sigaction(SIGABRT, &sa, NULL);
  struct sigaction sb; memset(&sb, 0, sizeof(sb));
  sb.sa_sigaction = bt_handler; sb.sa_flags = SA_SIGINFO;
  sigaction(SIGUSR1, &sb, NULL);
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

/* Patch runtime: escreve instruções Thumb num símbolo do jogo. */
static void patch_thumb(const char *sym, const uint16_t *halfwords, int n) {
  uintptr_t a = so_find_addr_safe(sym);
  if (!a) { fprintf(stderr, "patch: símbolo %s não encontrado\n", sym); return; }
  a &= ~1u; /* limpa bit Thumb p/ escrever */
  uintptr_t pg = a & ~0xFFFUL;
  if (mprotect((void *)pg, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    fprintf(stderr, "patch: mprotect %s falhou\n", sym); return;
  }
  for (int i = 0; i < n; i++) ((uint16_t *)a)[i] = halfwords[i];
  mprotect((void *)pg, 0x2000, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)a, (char *)a + n * 2);
  fprintf(stderr, "patch: %s @ 0x%lx (%d halfwords)\n", sym, (unsigned long)a, n);
}
/* movs r0,#0 ; bx lr  (return 0 em Thumb) */
static void patch_thumb_ret0(const char *sym) {
  uint16_t hw[] = {0x2000, 0x4770};
  patch_thumb(sym, hw, 2);
}

static void load_module(const char *name, int heap_mb, DynLibFunction *tbl, int n) {
  size_t hs = (size_t)heap_mb * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) { fprintf(stderr, "mmap %d MB falhou (%s)\n", heap_mb, name); exit(1); }
  fprintf(stderr, "== carregando %s (heap %p, %d MB) ==\n", name, heap, heap_mb);
  if (so_load(name, heap, hs) < 0) { fprintf(stderr, "so_load(%s) falhou\n", name); exit(1); }
  if (so_relocate() < 0) { fprintf(stderr, "so_relocate(%s) falhou\n", name); exit(1); }
  so_resolve(tbl, n, 0);
  so_finalize();
  so_flush_caches();
  so_execute_init_array();
  fprintf(stderr, "== %s: text=%p+%zu data=%p+%zu ==\n", name, text_base, text_size, data_base, data_size);
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  install_crash_handler();
  fprintf(stderr, "=== SHANTAE (WayForward Black) so-loader / NextOS armv7 Mali-450 ===\n");

  jni_shim_set_package("com.wayforward.shantaepc", 0);

  preload_device_libs();
  build_base_table();

  load_module(GAME_SO, GAME_HEAP_MB, g_base, g_base_n);

  /* Google Analytics: precisa de Play Services/JNI real (device ID etc.) que não
   * temos -> GetDeviceId faz memcpy p/ buffer nulo -> crash. Telemetria não é
   * essencial: stub Analytics_Init=return 0 pula toda a subárvore (Create/
   * GoogleAnalyticsSession/GetDeviceId). SHANTAE_KEEP_ANALYTICS=1 mantém. */
  if (!getenv("SHANTAE_KEEP_ANALYTICS"))
    patch_thumb_ret0("_Z14Analytics_Initv");

  uintptr_t am = so_find_addr("android_main");
  if (!am) { fprintf(stderr, "android_main NÃO encontrado\n"); exit(1); }
  g_load_base = (am & ~1u) - ANDROID_MAIN_VADDR;
  fprintf(stderr, "android_main @ 0x%lx  load_base=0x%lx\n",
          (unsigned long)am, (unsigned long)g_load_base);

  struct android_app *app = android_shim_init();
  if (!app) { fprintf(stderr, "android_shim_init falhou\n"); exit(1); }

  /* g_pAndroidApp: ponteiro global que o jogo lê (LanguageGlobal etc.) p/ chegar
   * em app->activity->vm (JNI). Normalmente setado por ANativeActivity_onCreate,
   * que NÃO chamamos (vamos direto no android_main) -> setamos manualmente. */
  { uintptr_t gp = so_find_addr_safe("g_pAndroidApp");
    if (gp) { *(void **)gp = app; fprintf(stderr, "g_pAndroidApp @0x%lx = %p\n",
                                          (unsigned long)gp, (void *)app); }
    else fprintf(stderr, "AVISO: g_pAndroidApp não encontrado\n"); }

  /* cria janela SDL + contexto GLES2 (Mali fbdev) ANTES do jogo usar EGL */
  egl_shim_create_window();

  android_shim_send_cmd(app, APP_CMD_START);
  android_shim_send_cmd(app, APP_CMD_RESUME);
  android_shim_send_cmd(app, APP_CMD_INIT_WINDOW);
  android_shim_send_cmd(app, APP_CMD_GAINED_FOCUS);

  fprintf(stderr, "=== chamando android_main ===\n");
  void (*android_main_func)(struct android_app *) = (void (*)(struct android_app *))am;
  android_main_func(app);

  fprintf(stderr, "=== android_main retornou ===\n");
  _exit(0);
}
