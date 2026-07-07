/*
 * main.c -- LEGO BATMAN 2: DC Super Heroes (TT Fusion, libLEGO_SH1.so arm64,
 * app dirigida por Java: GLSurfaceView + JNI) so-loader p/ NextOS aarch64 +
 * Mali-450 (Utgard, GLES2 via SDL2/EGL fbdev).
 *
 * Base = framework do CASTLE OF ILLUSION (port FINALIZADO neste device:
 * so_util ELF64, canary bionic tpidr+0x28, egl_shim SDL2, opensles_shim,
 * imports.c com kit GL Mali). DIFERENCA-CHAVE: nao ha android_main — o
 * bootstrap replica o GameActivity.onCreate/GLSurfaceView decompilado do dex:
 *
 *   Fusion.nativeSetWritePath/SavePath/CachePath
 *   Fusion.nativeInitializeAssetManager(assetMgr)
 *   Fusion.nativeSetCommandLine("") / nativeSetDeviceStrings(...)
 *   Fusion.nativeAddAssetPackLocation(<pack>/1079/1079) x5
 *   GLSurfaceView: nativeInit(config, activity) -> nativeResize(w,h,0,0,0,0)
 *   -> nativeWindowFocusChanged(1) -> nativeResume -> loop nativeRender()+swap
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "so_util.h"
#include "egl_shim.h"
#include "jni_shim.h"

#define GAME_SO      "lib/libLEGO_SH1.so"
#define GAME_HEAP_MB 192
#define SCREEN_W 1280
#define SCREEN_H 720

volatile uintptr_t g_load_base = 0;
volatile int g_movie_skip_pending = 0; /* jni startMoviePlayback -> pula no loop */

/* stub p/ o path ETC1-texbake herdado do imports.c do Dysmantle */
const char *bk_last_bmp_name(void) { return ""; }

extern DynLibFunction coi_overrides[];
extern const int coi_overrides_count;
extern DynLibFunction revc_pthread_table[];
extern const int revc_pthread_count;
extern DynLibFunction coi_extra[];
extern const int coi_extra_count;

/* canary bionic (tpidr_el0+0x28) — segredo Dysmantle/COI */
__attribute__((aligned(16))) static _Thread_local char g_bionic_guard_pad[256];

static DynLibFunction *g_base;
static int g_base_n;
static void build_base_table(void) {
  g_base_n = coi_overrides_count + revc_pthread_count + coi_extra_count;
  g_base = malloc(sizeof(DynLibFunction) * g_base_n);
  int o = 0;
  memcpy(g_base + o, coi_overrides, sizeof(DynLibFunction) * coi_overrides_count);
  o += coi_overrides_count;
  memcpy(g_base + o, revc_pthread_table, sizeof(DynLibFunction) * revc_pthread_count);
  o += revc_pthread_count;
  memcpy(g_base + o, coi_extra, sizeof(DynLibFunction) * coi_extra_count);
}

/* ---- simbolizacao no crash (identico ao COI) ---- */
static void resolve_addr(uintptr_t a, char *out, int outsz) {
  int fd = open("/proc/self/maps", O_RDONLY);
  out[0] = 0;
  if (fd < 0) return;
  char buf[8192]; int n; char line[400]; int li = 0;
  while ((n = read(fd, buf, sizeof(buf))) > 0)
    for (int i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '\n' || li >= (int)sizeof(line) - 1) {
        line[li] = 0;
        unsigned long s, e; char perm[8]; char path[256]; path[0] = 0;
        if (sscanf(line, "%lx-%lx %7s %*x %*s %*d %255s", &s, &e, perm, path) >= 3 &&
            a >= s && a < e) {
          const char *base = strrchr(path, '/');
          base = base ? base + 1 : (path[0] ? path : "?");
          snprintf(out, outsz, "%s+0x%lx", base, (unsigned long)(a - s));
          close(fd); return;
        }
        li = 0;
      } else line[li++] = c;
    }
  close(fd);
}

static void crash_handler(int sig, siginfo_t *info, void *uc) {
  /* serializa: 2+ threads crashando juntas embaralham o dump */
  static volatile int lock = 0;
  while (__sync_lock_test_and_set(&lock, 1)) usleep(1000);
  ucontext_t *u = (ucontext_t *)uc;
  mcontext_t *m = &u->uc_mcontext;
  fprintf(stderr, "\n=== CRASH sig=%d addr=%p tid=%d ===\n", sig, info->si_addr,
          (int)syscall(__NR_gettid));
  char r[300];
  resolve_addr(m->pc, r, sizeof(r));
  fprintf(stderr, "  PC=%p %s", (void *)m->pc, r);
  if (g_load_base && m->pc >= g_load_base)
    fprintf(stderr, "  {game+0x%lx}", (unsigned long)(m->pc - g_load_base));
  fprintf(stderr, "\n");
  resolve_addr(m->regs[30], r, sizeof(r));
  fprintf(stderr, "  LR=%p %s", (void *)m->regs[30], r);
  if (g_load_base && m->regs[30] >= g_load_base)
    fprintf(stderr, "  {game+0x%lx}", (unsigned long)(m->regs[30] - g_load_base));
  fprintf(stderr, "\n");
  for (int i = 0; i < 29; i += 3)
    fprintf(stderr, "  x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx\n",
            i, (unsigned long)m->regs[i], i+1, (unsigned long)m->regs[i+1],
            i+2, (unsigned long)m->regs[i+2]);
  fprintf(stderr, "  sp=%lx fp=%lx\n", (unsigned long)m->sp, (unsigned long)m->regs[29]);
  uintptr_t fp = m->regs[29];
  for (int f = 0; f < 24 && fp; f++) {
    uintptr_t *p = (uintptr_t *)fp; uintptr_t next = p[0], lr = p[1];
    if (!lr) break;
    resolve_addr(lr, r, sizeof(r));
    fprintf(stderr, "  #%-2d lr %p %s", f, (void *)lr, r);
    if (g_load_base && lr >= g_load_base)
      fprintf(stderr, "  {game+0x%lx}", (unsigned long)(lr - g_load_base));
    fprintf(stderr, "\n");
    if (next <= fp) break; fp = next;
  }
  /* varredura da pilha: qualquer qword que caia no text do jogo = possivel
   * retorno (acha o caller mesmo quando o memcpy nao montou frame) */
  { extern void *text_base; extern size_t text_size;
    uintptr_t lo = (uintptr_t)text_base, hi = lo + text_size;
    uintptr_t *sp = (uintptr_t *)(m->sp & ~7UL);
    fprintf(stderr, "  [scan pilha por game-addrs]\n");
    for (int k = 0; k < 320; k++) {
      uintptr_t v = sp[k];
      if (v >= lo && v < hi)
        fprintf(stderr, "    sp+0x%03x: game+0x%lx\n", k * 8,
                (unsigned long)(v - lo));
    } }
  fflush(stderr);
  _exit(128 + sig);
}

static void bt_handler(int sig, siginfo_t *info, void *uc) {
  (void)info;
  ucontext_t *u = (ucontext_t *)uc;
  mcontext_t *m = &u->uc_mcontext;
  char r[300];
  resolve_addr(m->pc, r, sizeof(r));
  fprintf(stderr, "\n[BT sig=%d tid=%d] PC=%p %s", sig,
          (int)syscall(__NR_gettid), (void *)m->pc, r);
  if (g_load_base && m->pc >= g_load_base)
    fprintf(stderr, " {game+0x%lx}", (unsigned long)(m->pc - g_load_base));
  fprintf(stderr, "\n");
  uintptr_t fp = m->regs[29];
  for (int f = 0; f < 20 && fp; f++) {
    uintptr_t *p = (uintptr_t *)fp; uintptr_t next = p[0], lr = p[1];
    if (!lr) break;
    resolve_addr(lr, r, sizeof(r));
    fprintf(stderr, "  #%-2d lr %p %s", f, (void *)lr, r);
    if (g_load_base && lr >= g_load_base)
      fprintf(stderr, " {game+0x%lx}", (unsigned long)(lr - g_load_base));
    fprintf(stderr, "\n");
    if (next <= fp) break; fp = next;
  }
  fflush(stderr);
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
      "libOpenSLES.so", "libm.so.6", "libdl.so.2", "libz.so.1", NULL };
  for (int i = 0; libs[i]; i++) {
    void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr, "preload: %s %s\n", libs[i], h ? "OK" : dlerror());
  }
}

/* ---- natives do jogo (resolvidos por nome no .so) ---- */
#define FUS(n) "Java_com_wbgames_LEGOgame_Fusion_" n
#define GLS(n) "Java_com_wbgames_LEGOgame_GameGLSurfaceView_" n

typedef void (*fn_env_str)(void *, void *, void *);
typedef void (*fn_env_obj)(void *, void *, void *);
typedef void (*fn_env_v)(void *, void *);
typedef void (*fn_env_b)(void *, void *, unsigned char);
typedef void (*fn_env_4str)(void *, void *, void *, void *, void *, void *);
typedef void (*fn_env_init)(void *, void *, void *, void *);
typedef void (*fn_env_6i)(void *, void *, int, int, int, int, int, int);
typedef void (*fn_touch)(void *, void *, int, float, float, float);

static void *fake_vm, *fake_env;

static uintptr_t need(const char *sym) {
  uintptr_t a = so_find_addr_safe(sym);
  if (!a) { fprintf(stderr, "FALTA native: %s\n", sym); }
  return a;
}

/* jstring via NewStringUTF do fake env (indice 167 da vtable JNI) */
static void *jstr(const char *s) {
  void ***env = (void ***)fake_env;
  void *(*newstr)(void *, const char *) = (void *(*)(void *, const char *))(*env)[167];
  return newstr(fake_env, s);
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  install_crash_handler();
  fprintf(stderr, "=== LEGO BATMAN 2 (TT Fusion) so-loader / NextOS aarch64 Mali-450 ===\n");

  { uintptr_t tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    uintptr_t slot = tp + 0x28, lo = (uintptr_t)g_bionic_guard_pad;
    fprintf(stderr, "TLS guard: slot+0x28=0x%lx pad=[0x%lx..0x%lx] %s\n",
            (unsigned long)slot, (unsigned long)lo,
            (unsigned long)(lo + sizeof(g_bionic_guard_pad)),
            (slot >= lo && slot + 8 <= lo + sizeof(g_bionic_guard_pad)) ? "DENTRO" : "FORA(!)"); }

  jni_shim_set_package("com.wb.goog.legobdc", 1079);

  preload_device_libs();
  build_base_table();

  /* modulo unico: libLEGO_SH1.so */
  size_t hs = (size_t)GAME_HEAP_MB * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) { fprintf(stderr, "mmap heap falhou\n"); return 1; }
  fprintf(stderr, "== carregando %s ==\n", GAME_SO);
  if (so_load(GAME_SO, heap, hs) < 0) { fprintf(stderr, "so_load falhou\n"); return 1; }
  if (so_relocate() < 0) { fprintf(stderr, "so_relocate falhou\n"); return 1; }
  so_resolve(g_base, g_base_n, 0);
  so_finalize();
  so_flush_caches();
  so_execute_init_array();
  g_load_base = (uintptr_t)text_base;
  fprintf(stderr, "== game text=%p+%zu data=%p+%zu ==\n", text_base, text_size,
          data_base, data_size);

  jni_shim_init(&fake_vm, &fake_env);

  /* JNI_OnLoad primeiro (runtime Java faria isso no loadLibrary) */
  { uintptr_t ol = so_find_addr_safe("JNI_OnLoad");
    if (ol) {
      int (*jol)(void *, void *) = (int (*)(void *, void *))ol;
      int v = jol(fake_vm, NULL);
      fprintf(stderr, "JNI_OnLoad(vm=%p) -> 0x%x\n", fake_vm, v);
    } else fprintf(stderr, "AVISO: sem JNI_OnLoad\n"); }

  /* janela EGL + UNICO contexto real ANTES do nativeInit. O modo single-context
   * handoff (egl_shim) e ativado quando o jogo chama eglCreateContext; esta
   * thread (render) adquire o contexto no 1o egl_shim_gl_acquire abaixo. */
  egl_shim_create_window();
  egl_shim_CreateContext(NULL, NULL, NULL, NULL); /* ativa handoff, fake token */
  egl_shim_gl_acquire();                          /* render-thread pega o ctx */

  char cwd[512]; if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, ".");
  char pbuf[1024];

  /* --- sequencia do GameActivity.onCreate (dex decompilado) --- */
  static int fake_cls, fake_activity, fake_assets;
  void *env = fake_env, *cls = &fake_cls;

  snprintf(pbuf, sizeof(pbuf), "%s/gamedata", cwd);
  mkdir(pbuf, 0755);
  fn_env_str f_str;

  if ((f_str = (fn_env_str)need(FUS("nativeSetWritePath"))))
    { snprintf(pbuf, sizeof(pbuf), "%s/gamedata", cwd); f_str(env, cls, jstr(pbuf));
      fprintf(stderr, "nativeSetWritePath(%s)\n", pbuf); }
  if ((f_str = (fn_env_str)need(FUS("nativeSetSavePath"))))
    { snprintf(pbuf, sizeof(pbuf), "%s/gamedata", cwd); f_str(env, cls, jstr(pbuf));
      fprintf(stderr, "nativeSetSavePath(%s)\n", pbuf); }
  if ((f_str = (fn_env_str)need(FUS("nativeSetCachePath"))))
    { snprintf(pbuf, sizeof(pbuf), "%s/cache", cwd); mkdir(pbuf, 0755);
      f_str(env, cls, jstr(pbuf));
      fprintf(stderr, "nativeSetCachePath(%s)\n", pbuf); }

  { fn_env_obj f = (fn_env_obj)need(FUS("nativeInitializeAssetManager"));
    if (f) { f(env, cls, &fake_assets); fprintf(stderr, "nativeInitializeAssetManager OK\n"); } }

  if ((f_str = (fn_env_str)need(FUS("nativeSetCommandLine"))))
    { f_str(env, cls, jstr("")); fprintf(stderr, "nativeSetCommandLine(\"\")\n"); }

  { fn_env_4str f = (fn_env_4str)need(FUS("nativeSetDeviceStrings"));
    if (f) { f(env, cls, jstr("NextOS"), jstr("nextos"), jstr("NextOS"), jstr("mali"));
      fprintf(stderr, "nativeSetDeviceStrings OK\n"); } }

  { fn_env_str f = (fn_env_str)need(FUS("nativeAddAssetPackLocation"));
    static const char *packs[] = { "assets_cutscenes", "assets_music",
                                   "assets_shaders", "assets_main", "assets_lofi" };
    for (int i = 0; f && i < 5; i++) {
      snprintf(pbuf, sizeof(pbuf), "%s/files/assetpacks/%s/1079/1079", cwd, packs[i]);
      f(env, cls, jstr(pbuf));
      fprintf(stderr, "nativeAddAssetPackLocation(%s)\n", pbuf);
    }
  }

  /* --- GLSurfaceView: coldBoot -> init -> resize -> focus -> resume --- */
  /* nativeColdBoot ANTES do nativeInit (receita lswtfa): inicializa o estado que
   * o engine espera; sem ele o render-thread fica esperando "load completo". */
  { void (*f)(void *, void *) = (void (*)(void *, void *))
        so_find_addr_safe(GLS("nativeColdBoot"));
    if (f) { fprintf(stderr, "== nativeColdBoot ==\n"); f(env, cls); } }
  { fn_env_init f = (fn_env_init)need(GLS("nativeInit"));
    if (!f) return 1;
    fprintf(stderr, "== nativeInit ==\n");
    f(env, cls, NULL /*EGLConfig*/, &fake_activity); }

  { fn_env_6i f = (fn_env_6i)need(GLS("nativeResize"));
    if (f) { f(env, cls, SCREEN_W, SCREEN_H, 0, 0, 0, 0);
      fprintf(stderr, "nativeResize(%dx%d)\n", SCREEN_W, SCREEN_H); } }

  { fn_env_b f = (fn_env_b)need(GLS("nativeWindowFocusChanged"));
    if (f) { f(env, cls, 1); fprintf(stderr, "nativeWindowFocusChanged(1)\n"); } }

  { fn_env_v f = (fn_env_v)need(GLS("nativeResume"));
    if (f) { f(env, cls); fprintf(stderr, "nativeResume()\n"); } }

  /* --- game loop: nativeRender + swap + input debug (lb2_tap/lb2_shot) --- */
  fn_env_v f_render = (fn_env_v)need(GLS("nativeRender"));
  if (!f_render) return 1;
  fn_touch f_down = (fn_touch)so_find_addr_safe(FUS("nativeTouchEventDown"));
  fn_touch f_up   = (fn_touch)so_find_addr_safe(FUS("nativeTouchEventUp"));

  /* nativeSkippedMovie: sinaliza "filme pulado" p/ a engine avancar sem player Java */
  void (*f_skipmovie)(void *, void *) = (void (*)(void *, void *))
      so_find_addr_safe(GLS("nativeSkippedMovie"));

  fprintf(stderr, "== entrando no game loop ==\n");
  unsigned frame = 0;
  int tap_frames = 0; float tap_x = 0, tap_y = 0;
  for (;;) {
    egl_shim_gl_acquire();       /* render-thread reassume o ctx (loader pode ter pego) */
    if (g_movie_skip_pending && f_skipmovie) {
      g_movie_skip_pending = 0;
      f_skipmovie(env, cls);
      fprintf(stderr, "nativeSkippedMovie() -> engine avanca\n");
    }
    f_render(env, cls);
    egl_shim_SwapBuffers(NULL, NULL);  /* faz acquire+swap+release_if_wanted */
    frame++;

    /* tap injetavel: echo "x y" > /dev/shm/lb2_tap */
    if ((frame & 7) == 0) {
      FILE *tf = fopen("/dev/shm/lb2_tap", "r");
      if (tf) {
        if (fscanf(tf, "%f %f", &tap_x, &tap_y) == 2 && f_down) {
          f_down(env, cls, 0, tap_x, tap_y, 1.0f);
          tap_frames = 4;
          fprintf(stderr, "[tap] down %.0f,%.0f\n", tap_x, tap_y);
        }
        fclose(tf); unlink("/dev/shm/lb2_tap");
      }
    }
    if (tap_frames > 0 && --tap_frames == 0 && f_up) {
      f_up(env, cls, 0, tap_x, tap_y, 0.0f);
      fprintf(stderr, "[tap] up\n");
    }
  }
  return 0;
}
