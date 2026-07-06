/*
 * Don’t Starve Pocket Edition Android/arm64 NativeActivity loader for NextOS.
 *
 * The game is a Klei native app: libc++ -> FMOD -> FMOD Studio -> libDontStarve,
 * then android_main(struct android_app *).  Java is replaced by jni_shim and
 * Android NDK pieces are supplied by android_shim/imports.
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

#include "android_shim.h"
#include "egl_shim.h"
#include "jni_shim.h"
#include "so_util.h"

#define CXX_SO        "libc++_shared.so"
#define FMOD_SO       "libfmod.so"
#define FMODSTUDIO_SO "libfmodstudio.so"
#define GAME_SO       "libDontStarve.so"

#define CXX_HEAP_MB        64
#define FMOD_HEAP_MB       32
#define FMODSTUDIO_HEAP_MB 32
#define GAME_HEAP_MB      256

#define PACKAGE_NAME "com.kleientertainment.doNotStarvePocket"

/* readelf/nm from the APK arm64 binary. */
#define ANDROID_MAIN_VADDR 0x320d0c
#define EH_FRAME_VADDR     0x283e98
#define JNIHELPER_ASK_PERMISSIONS_VADDR 0x31d42c
#define JNIHELPER_ARE_PERMISSIONS_GRANTED_VADDR 0x3201cc
#define JNIHELPER_PERMISSION_GRANTED_OFFSET 0x5e41

volatile uintptr_t g_load_base = 0;
static uintptr_t g_fmod_jni_onload = 0;
static uintptr_t g_fmod_jni_init = 0;

extern DynLibFunction dontstarve_overrides[];
extern const int dontstarve_overrides_count;
extern DynLibFunction revc_pthread_table[];
extern const int revc_pthread_count;

__attribute__((aligned(16))) static _Thread_local char g_bionic_guard_pad[256];

static DynLibFunction *g_base;
static int g_base_n;

static void build_base_table(void) {
  g_base_n = dontstarve_overrides_count + revc_pthread_count;
  g_base = malloc(sizeof(DynLibFunction) * g_base_n);
  if (!g_base) {
    fprintf(stderr, "malloc base table failed\n");
    exit(1);
  }
  memcpy(g_base, dontstarve_overrides,
         sizeof(DynLibFunction) * dontstarve_overrides_count);
  memcpy(g_base + dontstarve_overrides_count, revc_pthread_table,
         sizeof(DynLibFunction) * revc_pthread_count);
}

static void resolve_addr(uintptr_t a, char *out, int outsz) {
  int fd = open("/proc/self/maps", O_RDONLY);
  out[0] = 0;
  if (fd < 0)
    return;
  char buf[8192];
  char line[512];
  int li = 0;
  int n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    for (int i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '\n' || li >= (int)sizeof(line) - 1) {
        line[li] = 0;
        unsigned long s, e;
        char perm[8], path[256];
        path[0] = 0;
        if (sscanf(line, "%lx-%lx %7s %*x %*s %*d %255s", &s, &e, perm,
                   path) >= 3 &&
            a >= s && a < e) {
          const char *base = strrchr(path, '/');
          base = base ? base + 1 : (path[0] ? path : "?");
          snprintf(out, outsz, "%s+0x%lx", base, (unsigned long)(a - s));
          close(fd);
          return;
        }
        li = 0;
      } else {
        line[li++] = c;
      }
    }
  }
  close(fd);
}

static void crash_handler(int sig, siginfo_t *info, void *uc) {
  ucontext_t *u = (ucontext_t *)uc;
  mcontext_t *m = &u->uc_mcontext;
  if (info && info->si_code <= 0 && m->regs[8] == __NR_tgkill) {
    fprintf(stderr, "[signal] swallowed self-tgkill sig=%d addr=%p code=%d\n",
            sig, info->si_addr, info->si_code);
    return;
  }
  fprintf(stderr, "\n=== CRASH sig=%d addr=%p tid=%d ===\n", sig,
          info->si_addr, (int)syscall(__NR_gettid));
  char r[300];
  resolve_addr(m->pc, r, sizeof(r));
  fprintf(stderr, "  PC=%p %s", (void *)m->pc, r);
  if (g_load_base && m->pc >= g_load_base)
    fprintf(stderr, "  {%s+0x%lx}", GAME_SO,
            (unsigned long)(m->pc - g_load_base));
  fprintf(stderr, "\n");
  resolve_addr(m->regs[30], r, sizeof(r));
  fprintf(stderr, "  LR=%p %s", (void *)m->regs[30], r);
  if (g_load_base && m->regs[30] >= g_load_base)
    fprintf(stderr, "  {%s+0x%lx}", GAME_SO,
            (unsigned long)(m->regs[30] - g_load_base));
  fprintf(stderr, "\n");
  for (int i = 0; i < 29; i += 3) {
    fprintf(stderr, "  x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx\n", i,
            (unsigned long)m->regs[i], i + 1, (unsigned long)m->regs[i + 1],
            i + 2, (unsigned long)m->regs[i + 2]);
  }
  fprintf(stderr, "  sp=%lx fp=%lx\n", (unsigned long)m->sp,
          (unsigned long)m->regs[29]);
  uintptr_t fp = m->regs[29];
  for (int f = 0; f < 24 && fp; f++) {
    uintptr_t *p = (uintptr_t *)fp;
    uintptr_t next = p[0], lr = p[1];
    if (!lr)
      break;
    resolve_addr(lr, r, sizeof(r));
    fprintf(stderr, "  #%-2d lr %p %s", f, (void *)lr, r);
    if (g_load_base && lr >= g_load_base)
      fprintf(stderr, "  {%s+0x%lx}", GAME_SO,
              (unsigned long)(lr - g_load_base));
    fprintf(stderr, "\n");
    if (next <= fp)
      break;
    fp = next;
  }
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
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
}

static void preload_device_libs(void) {
  static const char *libs[] = {
      "libSDL2-2.0.so.0", "libGLESv2.so", "libEGL.so", "libm.so.6",
      "libdl.so.2", "libz.so.1", NULL};
  for (int i = 0; libs[i]; i++) {
    void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr, "preload: %s %s\n", libs[i], h ? "OK" : dlerror());
  }
}

static void dontstarve_ask_permissions_hook(void *self) {
  if (self)
    *((unsigned char *)self + JNIHELPER_PERMISSION_GRANTED_OFFSET) = 1;
  fprintf(stderr, "[patch] askPermissions -> granted\n");
}

static unsigned char dontstarve_are_permissions_granted_hook(void *self) {
  if (self)
    *((unsigned char *)self + JNIHELPER_PERMISSION_GRANTED_OFFSET) = 1;
  return 1;
}

static DynLibFunction *load_module(const char *name, int heap_mb,
                                   DynLibFunction *tbl, int n, int *out_n) {
  size_t hs = (size_t)heap_mb * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) {
    fprintf(stderr, "mmap %d MB failed for %s\n", heap_mb, name);
    exit(1);
  }
  fprintf(stderr, "== loading %s (heap %p, %d MB) ==\n", name, heap, heap_mb);
  if (so_load(name, heap, hs) < 0) {
    fprintf(stderr, "so_load(%s) failed\n", name);
    exit(1);
  }
  if (so_relocate() < 0) {
    fprintf(stderr, "so_relocate(%s) failed\n", name);
    exit(1);
  }
  so_resolve(tbl, n, 0);
  so_finalize();
  so_flush_caches();
  so_execute_init_array();
  fprintf(stderr, "== %s: text=%p+%zu data=%p+%zu ==\n", name, text_base,
          text_size, data_base, data_size);
  if (out_n)
    return so_snapshot_symbols(out_n);
  return NULL;
}

static DynLibFunction *tbl_concat(DynLibFunction *a, int an, DynLibFunction *b,
                                  int bn, int *out_n) {
  DynLibFunction *c = malloc(sizeof(DynLibFunction) * (an + bn));
  if (!c) {
    fprintf(stderr, "malloc concat table failed\n");
    exit(1);
  }
  memcpy(c, a, sizeof(DynLibFunction) * an);
  memcpy(c + an, b, sizeof(DynLibFunction) * bn);
  *out_n = an + bn;
  return c;
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  install_crash_handler();
  fprintf(stderr, "=== Dont Starve Pocket Edition / NextOS aarch64 ===\n");

  uintptr_t tp;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  uintptr_t slot = tp + 0x28;
  uintptr_t lo = (uintptr_t)g_bionic_guard_pad;
  fprintf(stderr, "TLS guard slot=0x%lx pad=[0x%lx..0x%lx] %s\n",
          (unsigned long)slot, (unsigned long)lo,
          (unsigned long)(lo + sizeof(g_bionic_guard_pad)),
          (slot >= lo && slot + 8 <= lo + sizeof(g_bionic_guard_pad)) ? "OK"
                                                                      : "WARN");

  setenv("DONTSTARVE_ASSETS", "assets", 0);
  jni_shim_set_package(PACKAGE_NAME, 0);
  preload_device_libs();
  build_base_table();

  int cxx_n = 0;
  DynLibFunction *cxx =
      load_module(CXX_SO, CXX_HEAP_MB, g_base, g_base_n, &cxx_n);
  fprintf(stderr, "libc++ symbols: %d\n", cxx_n);
  int t1n;
  DynLibFunction *t1 = tbl_concat(g_base, g_base_n, cxx, cxx_n, &t1n);

  int fmod_n = 0;
  DynLibFunction *fmod = load_module(FMOD_SO, FMOD_HEAP_MB, t1, t1n, &fmod_n);
  g_fmod_jni_onload = so_find_addr_safe("JNI_OnLoad");
  g_fmod_jni_init = so_find_addr_safe("FMOD_Android_JNI_Init");
  fprintf(stderr, "libfmod symbols: %d\n", fmod_n);
  int t2n;
  DynLibFunction *t2 = tbl_concat(t1, t1n, fmod, fmod_n, &t2n);

  int fst_n = 0;
  DynLibFunction *fst =
      load_module(FMODSTUDIO_SO, FMODSTUDIO_HEAP_MB, t2, t2n, &fst_n);
  fprintf(stderr, "libfmodstudio symbols: %d\n", fst_n);
  int t3n;
  DynLibFunction *t3 = tbl_concat(t2, t2n, fst, fst_n, &t3n);

  load_module(GAME_SO, GAME_HEAP_MB, t3, t3n, NULL);

  uintptr_t am = so_find_addr("android_main");
  if (!am) {
    fprintf(stderr, "android_main not found\n");
    exit(1);
  }
  g_load_base = am - ANDROID_MAIN_VADDR;
  fprintf(stderr, "android_main @ 0x%lx load_base=0x%lx\n",
          (unsigned long)am, (unsigned long)g_load_base);

  so_make_text_writable();
  hook_arm64(g_load_base + JNIHELPER_ASK_PERMISSIONS_VADDR,
             (uintptr_t)dontstarve_ask_permissions_hook);
  hook_arm64(g_load_base + JNIHELPER_ARE_PERMISSIONS_GRANTED_VADDR,
             (uintptr_t)dontstarve_are_permissions_granted_hook);
  so_make_text_executable();
  fprintf(stderr, "[patch] permissions forced granted\n");

  extern void __register_frame(void *);
  __register_frame((void *)(g_load_base + EH_FRAME_VADDR));
  fprintf(stderr, "__register_frame eh_frame @ 0x%lx\n",
          (unsigned long)(g_load_base + EH_FRAME_VADDR));

  struct android_app *app = android_shim_init();
  if (!app) {
    fprintf(stderr, "android_shim_init failed\n");
    exit(1);
  }
  app->activity->assetManager = (void *)1;

  if (g_fmod_jni_onload) {
    int (*fn)(void *, void *) = (void *)g_fmod_jni_onload;
    int r = fn(app->activity->vm, NULL);
    fprintf(stderr, "FMOD JNI_OnLoad -> 0x%x\n", r);
  }
  if (g_fmod_jni_init) {
    int (*fn)(void *, void *) = (void *)g_fmod_jni_init;
    int r = fn(app->activity->vm, app->activity->clazz);
    fprintf(stderr, "FMOD_Android_JNI_Init -> %d\n", r);
  }

  uintptr_t nsam =
      so_find_addr_safe("Java_PACKAGE_NAME_DoNotStarveActivity_nativeSetAssetManager");
  if (nsam) {
    fprintf(stderr, "calling nativeSetAssetManager @ 0x%lx\n",
            (unsigned long)nsam);
    void (*fn)(void *, void *, void *) = (void *)nsam;
    fn(app->activity->env, NULL, (void *)1);
  }

  egl_shim_create_window();

  android_shim_send_cmd(app, APP_CMD_START);
  android_shim_send_cmd(app, APP_CMD_RESUME);
  android_shim_send_cmd(app, APP_CMD_INIT_WINDOW);
  android_shim_send_cmd(app, APP_CMD_INPUT_CHANGED);
  android_shim_send_cmd(app, APP_CMD_GAINED_FOCUS);

  fprintf(stderr, "=== calling android_main ===\n");
  void (*android_main_func)(struct android_app *) =
      (void (*)(struct android_app *))am;
  android_main_func(app);

  fprintf(stderr, "=== android_main returned ===\n");
  _exit(0);
}
