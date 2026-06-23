/*
 * main_ducktales.c -- DuckTales: Remastered (WayForward, NativeActivity, armv7)
 *                     so-loader -> Mali-450 / Linux fbdev via SDL2.
 *
 * libducktales exports android_main(struct android_app*) + JNI_OnLoad and
 * imports the NDK glue funcs (ALooper/AInputQueue/AAsset/AConfiguration/
 * ANativeWindow). We provide a fake android_app (android_shim) and call
 * android_main directly -- the Syberia/RE4 model.
 *
 * fmod audio: libducktales NEEDED libfmodex.so + libfmodevent.so. We load
 * both as so_modules; libducktales' FMOD::* imports resolve against them via
 * dt_fmod_lookup(). fmod's OpenSL backend dlopen("libOpenSLES.so") is made to
 * fail so fmod falls back to org.fmod.FMODAudioDevice (jni_shim bridge->pulse).
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <ucontext.h>

#include "android_shim.h"
#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

extern void egl_shim_create_window(void);

/* --- stubs for symbols the RE4 shims expect from main_re4 (Unity-specific) --- */
void re4_signal_gameplay(int on) { (void)on; }
void my_exit(int code) { _exit(code); }
const char *re4_addr_mod(uintptr_t a) { (void)a; return "?"; }
int g_gameplay = 0, g_gameplay_frame = 0, g_re4_frame = 0;
int re4_in_unity(uintptr_t a) { (void)a; return 0; }
void re4_frame_end_present(void) {}
void *re4_gl_override(const char *procname) { (void)procname; return NULL; }

extern void hook_arm(uintptr_t addr, uintptr_t dst);
extern int egl_shim_frame_count(void);

/* ---- timestamped log line to stderr (launcher tees to logs/) ---- */
static void tslog(const char *tag, const char *msg) {
  time_t t = time(NULL);
  struct tm tmv;
  localtime_r(&t, &tmv);
  char ts[32];
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
  fprintf(stderr, "[%s] [%s] %s\n", ts, tag, msg);
  fflush(stderr);
}

/* ---- WATCHDOG: hard self-exit so the game can never wedge the device ----
   DUCK_MAXSECONDS=N force-exits after N seconds. Heartbeat every 5s reports
   elapsed time + GL frame count so a stuck run is visible in the logs. */
static volatile int g_wd_seconds = 0;
static void *watchdog_thread(void *arg) {
  (void)arg;
  int last_frames = -1, stuck = 0;
  for (int i = 1; i <= g_wd_seconds; i++) {
    sleep(1);
    if (i % 5 == 0) {
      int f = egl_shim_frame_count();
      char m[128];
      snprintf(m, sizeof(m), "alive %ds/%ds frames=%d", i, g_wd_seconds, f);
      tslog("watchdog", m);
      if (f == last_frames && f > 0) {
        if (++stuck >= 3) { tslog("watchdog", "RENDER STUCK (no new frames 15s) -> force exit"); _exit(42); }
      } else stuck = 0;
      last_frames = f;
    }
  }
  tslog("watchdog", "MAX_SECONDS reached -> force exit");
  _exit(0);
}
static void start_watchdog(void) {
  const char *s = getenv("DUCK_MAXSECONDS");
  if (!s) return;
  g_wd_seconds = atoi(s);
  if (g_wd_seconds <= 0) return;
  pthread_t th;
  pthread_create(&th, NULL, watchdog_thread, NULL);
  pthread_detach(th);
  char m[64]; snprintf(m, sizeof(m), "armed: %ds", g_wd_seconds);
  tslog("watchdog", m);
}

/* The NVIDIA gamepad helper enumerates axes/buttons via JNI reflection
   (GetObjectClass/GetMethodID/CallObjectMethod). Our fake JNIEnv returns
   garbage that gets dereferenced -> crash. Real input flows through
   AInputQueue/onInputEvent, so we bail these enum calls with 0 devices. */
static int nv_gamepad_enum_stub(void *env, void *obj, int *count) {
  (void)env; (void)obj;
  if (count) *count = 0;
  return 0;
}

/* wfSystem::GetCpuCount() drives both the worker-thread count and the
   barrier in wfMCP::Exec. Forcing it keeps spawn and barrier consistent so we
   can run with N workers (e.g. 1 = serial, to separate races from data bugs). */
static int g_forced_cpucount = 0;
static int forced_cpucount(void) { return g_forced_cpucount; }

/* --- Serialize wfJob::Exec across the 10 worker threads ---
   The engine's job system has data races that are latent on Android's
   scheduler but fatal here (concurrent wfJob::Exec corrupts shared parse
   state -> NULL/garbage pointers). We keep all 10 threads (fewer deadlock on
   job dependencies) but let only ONE job body run at a time via a global lock.
   Implemented as an inline hook + trampoline that replays the 2 clobbered
   prologue instructions then continues into the original function. */
static pthread_mutex_t g_jobexec_lock;
static void (*g_wfjob_tramp)(void *) = NULL;
static void my_wfjob_exec(void *job) {
  pthread_mutex_lock(&g_jobexec_lock);   /* recursive: nested same-thread jobs OK */
  g_wfjob_tramp(job);
  pthread_mutex_unlock(&g_jobexec_lock);
}
static void install_jobexec_serializer(void) {
  if (!getenv("DUCK_SERIAL_JOBS")) return;   /* deadlocks on cross-thread deps; off by default */
  { pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_jobexec_lock, &a);
    pthread_mutexattr_destroy(&a); }
  uintptr_t a = so_find_addr_safe("_ZN5wfJob4ExecEP11wfJobThread");
  if (!a) { debugPrintf("[serial] wfJob::Exec not found\n"); return; }
  uint32_t *orig = (uint32_t *)a;
  uint32_t i0 = orig[0], i1 = orig[1];
  uint32_t *tr = (uint32_t *)mmap(NULL, 32, PROT_READ | PROT_WRITE | PROT_EXEC,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr == MAP_FAILED) return;
  tr[0] = i0; tr[1] = i1; tr[2] = 0xe51ff004u; tr[3] = (uint32_t)(a + 8);
  __builtin___clear_cache((char *)tr, (char *)tr + 16);
  g_wfjob_tramp = (void (*)(void *))tr;
  hook_arm(a, (uintptr_t)my_wfjob_exec);
  debugPrintf("[serial] wfJob::Exec serialized\n");
}

/* --- Lock the engine's internal (dlmalloc-style) pool allocator ---
   At file offsets 0x9392a4 (malloc) / 0x939e04 (free) there is a bundled
   size-classed free-list allocator with NO internal locking. Under our 10
   worker threads its doubly-linked free lists corrupt (unlink writes through
   garbage prev/next -> SIGSEGV). glibc malloc is already thread-safe; this
   private pool is not. We wrap malloc+free with one recursive global lock.
   Locking an allocator can't deadlock (it never waits on other jobs). */
/* 8-arg signature: these internal allocator functions read stack-passed args
   (e.g. ldr r4,[sp,#76] = 6th arg). Forwarding 8 args keeps the stack layout
   identical for the trampoline so its [sp,#NN] arg reads stay valid. */
typedef void *(*alloc_fn8)(void *, void *, void *, void *, void *, void *, void *, void *);
static alloc_fn8 g_pool_malloc_tramp = NULL;
static alloc_fn8 g_pool_free_tramp = NULL;
/* recursive spinlock — async-signal-safe (the allocator may be reached from a
   signal handler; pthread_mutex there would deadlock). Allocator ops are short. */
static volatile long g_pool_owner = 0;
static volatile int g_pool_depth = 0;
static void pool_lock(void) {
  long me = (long)pthread_self();
  if (g_pool_owner == me) { g_pool_depth++; return; }
  while (__sync_val_compare_and_swap(&g_pool_owner, 0, me) != 0) { /* spin */ }
  g_pool_depth = 1;
}
static void pool_unlock(void) {
  if (--g_pool_depth == 0) { __sync_synchronize(); g_pool_owner = 0; }
}
static void df_alloced(void *p);
static int g_df_on = 0;
static void *pool_malloc_wrap(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  pool_lock();
  void *r = g_pool_malloc_tramp(a, b, c, d, e, f, g, h);
  if (g_df_on) df_alloced(r);
  pool_unlock();
  return r;
}
/* double-free detector: direct-mapped "currently-freed" table. free() marks the
   slot; if it's already the same chunk -> freed twice with no malloc between =
   real double-free. malloc() clears the slot (chunk live again). */
#define DF_SET 262144
static void *g_df_freed[DF_SET];
static void df_freed(void *p) {
  if (!p) return;
  unsigned idx = ((uintptr_t)p >> 4) & (DF_SET - 1);
  if (g_df_freed[idx] == p) {
    static int n = 0;
    if (n++ < 40) fprintf(stderr, "[DOUBLEFREE] chunk=%p\n", p);
  }
  g_df_freed[idx] = p;
}
static void df_alloced(void *p) {
  if (!p) return;
  unsigned idx = ((uintptr_t)p >> 4) & (DF_SET - 1);
  if (g_df_freed[idx] == p) g_df_freed[idx] = NULL;
}
static void *pool_free_wrap(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  pool_lock();
  if (g_df_on) df_freed(b);   /* r1 = chunk; r0 = mstate (constant) */
  void *r = g_pool_free_tramp(a, b, c, d, e, f, g, h);
  pool_unlock();
  return r;
}
static void *make_tramp(uintptr_t addr) {
  uint32_t *o = (uint32_t *)addr;
  uint32_t *tr = (uint32_t *)mmap(NULL, 32, PROT_READ | PROT_WRITE | PROT_EXEC,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr == MAP_FAILED) return NULL;
  tr[0] = o[0]; tr[1] = o[1]; tr[2] = 0xe51ff004u; tr[3] = (uint32_t)(addr + 8);
  __builtin___clear_cache((char *)tr, (char *)tr + 16);
  return tr;
}
static void install_pool_alloc_lock(void) {
  if (!getenv("DUCK_POOLLOCK") && !getenv("DUCK_DFDETECT")) return;   /* opt-in */
  if (getenv("DUCK_DFDETECT")) g_df_on = 1;
  uintptr_t base = (uintptr_t)text_base;
  uintptr_t m = base + 0x9392a4, f = base + 0x939e04;
  g_pool_malloc_tramp = (alloc_fn8)make_tramp(m);
  g_pool_free_tramp = (alloc_fn8)make_tramp(f);
  if (g_pool_malloc_tramp) hook_arm(m, (uintptr_t)pool_malloc_wrap);
  if (g_pool_free_tramp) hook_arm(f, (uintptr_t)pool_free_wrap);
  debugPrintf("[poollock] internal allocator malloc/free serialized\n");
}
static void install_ducktales_hooks(void) {
  so_make_text_writable();
  uintptr_t a;
  a = so_find_addr_safe("_Z16NvGetGamepadAxesP7_JNIEnvP8_jobjectRi");
  if (a) { hook_arm(a, (uintptr_t)nv_gamepad_enum_stub); debugPrintf("[hook] NvGetGamepadAxes->stub\n"); }
  a = so_find_addr_safe("_Z19NvGetGamepadButtonsP7_JNIEnvP8_jobjectRi");
  if (a) { hook_arm(a, (uintptr_t)nv_gamepad_enum_stub); debugPrintf("[hook] NvGetGamepadButtons->stub\n"); }

  /* Force wfSystem::GetCpuCount() (spawn count + barrier in one shot). */
  const char *cc = getenv("DUCK_CPUCOUNT");
  if (cc) {
    g_forced_cpucount = atoi(cc);
    uintptr_t g = so_find_addr_safe("_ZN8wfSystem11GetCpuCountEv");
    if (g) { hook_arm(g, (uintptr_t)forced_cpucount); debugPrintf("[hook] GetCpuCount -> %d\n", g_forced_cpucount); }
  }

  /* Optional: clamp wfLogicalCore job-thread count (default 10) for debugging
     concurrency. The ctor loop ends at `cmp r6, #10` (e356000a) at
     wfLogicalCore::wfLogicalCore+0x9c. Patch the immediate to N. */
  const char *jt = getenv("DUCK_JOBTHREADS");
  if (jt) {
    int n = atoi(jt); if (n < 1) n = 1; if (n > 10) n = 10;
    uintptr_t c = so_find_addr_safe("_ZN13wfLogicalCoreC1Ei");
    if (!c) c = so_find_addr_safe("_ZN13wfLogicalCoreC2Ei");
    if (c) {
      uint32_t *ins = (uint32_t *)(c + 0x9c);
      if ((*ins & 0xfffff000) == 0xe3560000) {  /* cmp r6, #imm */
        *ins = 0xe3560000 | (n & 0xff);
        debugPrintf("[hook] wfLogicalCore threads -> %d\n", n);
      } else debugPrintf("[hook] jobthread cmp not found (%08x)\n", *ins);
    }
  }
  install_jobexec_serializer();
  install_pool_alloc_lock();
  so_make_text_executable();
  so_flush_caches();
}

/* ---- loaded fmod modules (for cross-lib symbol resolution) ---- */
static so_module *g_m_fmodex = NULL;
static so_module *g_m_fmodevent = NULL;
static so_module *g_m_main = NULL;
static uintptr_t g_main_text = 0, g_main_text_sz = 0;
static uintptr_t g_fx_text = 0, g_fx_sz = 0, g_fe_text = 0, g_fe_sz = 0;

/* Install bionic<->glibc pthread shims (mutex/cond/sem/attr/create) by
   overriding the import table BEFORE resolution. Bionic pthread_mutex_t is 4
   bytes vs glibc 24 -> without this the game's small mutex buffers overflow
   into the heap (malloc: invalid size). */
extern void recon_wire_pthread(void (*)(const char *, void *));
static void dt_set_import(const char *name, void *fn) {
  for (size_t i = 0; i < dynlib_numfunctions; i++)
    if (!strcmp(dynlib_functions[i].symbol, name)) {
      dynlib_functions[i].func = (uintptr_t)fn;
      return;
    }
}

void *dt_fmod_lookup(const char *nm) {
  so_module *mods[2] = {g_m_fmodex, g_m_fmodevent};
  for (int i = 0; i < 2; i++) {
    if (!mods[i]) continue;
    so_module *cur = so_save();
    so_use(mods[i]);
    uintptr_t a = so_find_addr_safe(nm);
    so_use(cur);
    free(cur);
    if (a) return (void *)a;
  }
  return NULL;
}

/* ---- ARM32 crash handler (debug) ---- */
static void crash_handler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  uintptr_t pc = uc->uc_mcontext.arm_pc;
  uintptr_t lr = uc->uc_mcontext.arm_lr;
  uintptr_t sp = uc->uc_mcontext.arm_sp;
  fprintf(stderr, "\n=== CRASH sig=%d fault=%p ===\n", sig, info->si_addr);
  fprintf(stderr, "pc=%p lr=%p sp=%p\n", (void *)pc, (void *)lr, (void *)sp);
#define INMOD(a,b,s,nm) do{ if((a)>=(b)&&(a)<(b)+(s)) fprintf(stderr,"   %s+0x%lx\n",nm,(unsigned long)((a)-(b))); }while(0)
  INMOD(pc, g_main_text, g_main_text_sz, "duck.pc"); INMOD(lr, g_main_text, g_main_text_sz, "duck.lr");
  INMOD(pc, g_fx_text, g_fx_sz, "fmodex.pc"); INMOD(lr, g_fx_text, g_fx_sz, "fmodex.lr");
  fprintf(stderr, "r0=%lx r1=%lx r2=%lx r3=%lx r4=%lx r5=%lx\n",
          (unsigned long)uc->uc_mcontext.arm_r0, (unsigned long)uc->uc_mcontext.arm_r1,
          (unsigned long)uc->uc_mcontext.arm_r2, (unsigned long)uc->uc_mcontext.arm_r3,
          (unsigned long)uc->uc_mcontext.arm_r4, (unsigned long)uc->uc_mcontext.arm_r5);
  /* scan stack for return addresses inside loaded modules */
  fprintf(stderr, "backtrace (stack scan):\n");
  uintptr_t *s = (uintptr_t *)(sp & ~3u);
  int shown = 0;
  for (int i = 0; i < 4096 && shown < 24; i++) {
    uintptr_t v = s[i];
    if (v >= g_main_text && v < g_main_text + g_main_text_sz) {
      fprintf(stderr, "  duck+0x%lx\n", (unsigned long)(v - g_main_text)); shown++;
    } else if (g_fx_text && v >= g_fx_text && v < g_fx_text + g_fx_sz) {
      fprintf(stderr, "  fmodex+0x%lx\n", (unsigned long)(v - g_fx_text)); shown++;
    }
  }
  /* dump maps line covering pc */
  FILE *m = fopen("/proc/self/maps", "r");
  if (m) { char ln[512]; while (fgets(ln, sizeof(ln), m)) { unsigned long s, e;
    if (sscanf(ln, "%lx-%lx", &s, &e) == 2 && pc >= s && pc < e) fprintf(stderr, ">>> %s", ln); }
    fclose(m); }
  fprintf(stderr, "=== END CRASH ===\n");
  fflush(stderr);
  _exit(128 + sig);
}
static void install_crash_handler(void) {
  struct sigaction sa; memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL); sigaction(SIGILL, &sa, NULL);
}

/* ---- load one .so into its own mmap region ---- */
static so_module *load_lib(const char *path, size_t mb) {
  size_t sz = mb * 1024 * 1024;
  void *region = mmap(NULL, sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED) { debugPrintf("mmap fail %s\n", path); return NULL; }
  if (so_load(path, region, sz) < 0) { debugPrintf("so_load fail %s\n", path); return NULL; }
  if (so_relocate() < 0) { debugPrintf("reloc fail %s\n", path); return NULL; }
  so_resolve(dynlib_functions, dynlib_numfunctions, 0);
  so_finalize();
  so_execute_init_array();
  debugPrintf("loaded %s text=%p+%zu\n", path, text_base, text_size);
  return so_save();
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  /* no core dumps (keep the small system partition safe) */
  struct rlimit rl = {0, 0};
  setrlimit(RLIMIT_CORE, &rl);
  install_crash_handler();
  tslog("init", "=== DuckTales Remastered -> Mali-450 (armv7 so-loader) ===");

  /* wire pthread shims into the import table before any resolution */
  recon_wire_pthread(dt_set_import);
  /* bionic-layout stat/fstat: glibc fstat writes st_size at the wrong offset
     for NDK-built code -> wrong file sizes -> truncated buffers. */
  { extern int bionic_fstat(int, void *), bionic_stat(const char *, void *),
        bionic_lstat(const char *, void *), bionic_fstatat(int, const char *, void *, int);
    dt_set_import("fstat", (void *)bionic_fstat);
    dt_set_import("stat", (void *)bionic_stat);
    dt_set_import("lstat", (void *)bionic_lstat);
    dt_set_import("fstatat", (void *)bionic_fstatat); }
  tslog("init", "pthread + bionic-stat shims wired");

  const char *libdir = getenv("DUCK_LIBDIR");
  if (!libdir) libdir = "./lib";
  char p[1024];

  /* 1. fmodex (no fmod deps), 2. fmodevent (needs fmodex), 3. ducktales */
  snprintf(p, sizeof(p), "%s/libfmodex.so", libdir);
  g_m_fmodex = load_lib(p, 16);
  if (!g_m_fmodex) debugPrintf("WARN: libfmodex failed (audio off)\n");
  else { g_fx_text = (uintptr_t)text_base; g_fx_sz = text_size; }

  snprintf(p, sizeof(p), "%s/libfmodevent.so", libdir);
  g_m_fmodevent = load_lib(p, 8);
  if (g_m_fmodevent) { g_fe_text = (uintptr_t)text_base; g_fe_sz = text_size; }
  if (!g_m_fmodevent) debugPrintf("WARN: libfmodevent failed\n");

  snprintf(p, sizeof(p), "%s/libducktales.so", libdir);
  size_t hs = 160 * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) fatal_error("heap mmap failed");
  if (so_load(p, heap, hs) < 0) fatal_error("load libducktales failed");
  if (so_relocate() < 0) fatal_error("relocate libducktales failed");
  debugPrintf("Resolving %zu imports...\n", dynlib_numfunctions);
  so_resolve(dynlib_functions, dynlib_numfunctions, 0);
  so_finalize();
  so_flush_caches();
  so_execute_init_array();
  g_main_text = (uintptr_t)text_base; g_main_text_sz = text_size;
  install_ducktales_hooks();
  g_m_main = so_save();

  uintptr_t android_main_addr = so_find_addr("android_main");
  if (!android_main_addr) fatal_error("android_main not found");
  debugPrintf("android_main @ %p\n", (void *)android_main_addr);

  /* JNI_OnLoad (registers natives, inits FMODAudioDevice bridge) */
  uintptr_t onload = so_find_addr_safe("JNI_OnLoad");
  if (onload) {
    void *fake_vm = NULL, *fake_env = NULL;
    jni_shim_init(&fake_vm, &fake_env);
    typedef int (*onload_t)(void *, void *);
    int ver = ((onload_t)onload)(fake_vm, NULL);
    debugPrintf("JNI_OnLoad -> 0x%x\n", ver);
  }

  /* SDL window + GL context up front (main thread) */
  egl_shim_create_window();

  /* fake android environment */
  struct android_app *app = android_shim_init();
  if (!app) fatal_error("android_shim_init failed");

  android_shim_send_cmd(app, APP_CMD_INIT_WINDOW);
  android_shim_send_cmd(app, APP_CMD_GAINED_FOCUS);

  start_watchdog();
  tslog("init", "calling android_main");
  void (*amain)(struct android_app *) = (void (*)(struct android_app *))android_main_addr;
  amain(app);
  tslog("init", "android_main returned");
  _exit(0);
}
