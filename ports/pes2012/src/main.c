/*
 * main.c — loader ARMHF (gerado por new-port-arm.sh) p/ sonic4ep1.
 *
 * Multi-módulo bionic→glibc. Marmalade e invertido em relacao aos jogos comuns:
 * as extensoes libs3eAPKExpansion/Dialog/Flurry dependem dos s3eEdk* exportados
 * por libs3e_android.so. Portanto carregamos a principal primeiro, tiramos
 * snapshot dos simbolos dela, e so entao carregamos as extensoes.
 */
#include <ctype.h>
#include <setjmp.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "android_shim.h"
#include "egl_shim.h"
#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

#define MEMORY_MB 320
#define SO_NAME "libpes2012.so"

static uintptr_t g_main_text_base;
static size_t g_main_text_size;
static uintptr_t g_gamefile_read_import_addr;
static void *g_fake_vm_for_ext;
static void *sonic4ep1_gl_hook(const char *symbol, void *p);
extern void *softfp_resolve(const char *nm);

typedef void (*motion_native_fn)(void *, void *, int, int, int, int);
typedef void (*set_pixels_native_fn)(void *, void *, int, int, void *,
                                     unsigned char);
static motion_native_fn g_on_motion_event;
static void *g_motion_env;
static void *g_motion_thiz;
static volatile int g_autotap_fired;

static int env_int_or_default(const char *name, int fallback) {
  const char *v = getenv(name);
  if (!v || !*v)
    return fallback;
  char *end = NULL;
  long parsed = strtol(v, &end, 10);
  return (end && end != v) ? (int)parsed : fallback;
}

static void *autotap_thread(void *arg) {
  (void)arg;
  if (!g_on_motion_event)
    return NULL;

  int x = 640;
  int y = 360;
  const char *tap = getenv("SONIC4EP1_AUTOTAP");
  if (tap && *tap) {
    int tx = 0, ty = 0;
    if (sscanf(tap, "%d,%d", &tx, &ty) == 2) {
      x = tx;
      y = ty;
    }
  }

  int delay_ms = env_int_or_default("SONIC4EP1_AUTOTAP_DELAY_MS", 3500);
  int count = env_int_or_default("SONIC4EP1_AUTOTAP_COUNT", 1);
  int down_action = env_int_or_default("SONIC4EP1_AUTOTAP_DOWN", 0);
  int up_action = env_int_or_default("SONIC4EP1_AUTOTAP_UP", 1);
  if (count < 1)
    count = 1;
  if (count > 10)
    count = 10;
  const char *order = getenv("SONIC4EP1_AUTOTAP_ORDER");
  if (!order || !*order)
    order = "apxy";

  usleep((useconds_t)delay_ms * 1000);
  for (int i = 0; i < count; i++) {
    int down_args[4] = {down_action, 0, x, y};
    int up_args[4] = {up_action, 0, x, y};
    if (strcmp(order, "axyp") == 0) {
      down_args[0] = down_action;
      down_args[1] = x;
      down_args[2] = y;
      down_args[3] = 0;
      up_args[0] = up_action;
      up_args[1] = x;
      up_args[2] = y;
      up_args[3] = 0;
    } else if (strcmp(order, "xyap") == 0) {
      down_args[0] = x;
      down_args[1] = y;
      down_args[2] = down_action;
      down_args[3] = 0;
      up_args[0] = x;
      up_args[1] = y;
      up_args[2] = up_action;
      up_args[3] = 0;
    } else if (strcmp(order, "xypa") == 0) {
      down_args[0] = x;
      down_args[1] = y;
      down_args[2] = 0;
      down_args[3] = down_action;
      up_args[0] = x;
      up_args[1] = y;
      up_args[2] = 0;
      up_args[3] = up_action;
    } else if (strcmp(order, "paxy") == 0) {
      down_args[0] = 0;
      down_args[1] = down_action;
      down_args[2] = x;
      down_args[3] = y;
      up_args[0] = 0;
      up_args[1] = up_action;
      up_args[2] = x;
      up_args[3] = y;
    } else if (strcmp(order, "pxya") == 0) {
      down_args[0] = 0;
      down_args[1] = x;
      down_args[2] = y;
      down_args[3] = down_action;
      up_args[0] = 0;
      up_args[1] = x;
      up_args[2] = y;
      up_args[3] = up_action;
    }
    debugPrintf("autotap: pulse %d/%d order=%s down=(%d,%d,%d,%d) "
                "up=(%d,%d,%d,%d)\n",
                i + 1, count, order, down_args[0], down_args[1],
                down_args[2], down_args[3], up_args[0], up_args[1],
                up_args[2], up_args[3]);
    g_autotap_fired = 1;
    g_on_motion_event(g_motion_env, g_motion_thiz, down_args[0],
                      down_args[1], down_args[2], down_args[3]);
    usleep(120000);
    g_on_motion_event(g_motion_env, g_motion_thiz, up_args[0], up_args[1],
                      up_args[2], up_args[3]);
    usleep(700000);
  }
  return NULL;
}

static void maybe_start_autotap(void *env, void *thiz) {
  const char *tap = getenv("SONIC4EP1_AUTOTAP");
  if (!tap || !*tap)
    return;

  if (!g_on_motion_event)
    g_on_motion_event = (motion_native_fn)jni_find_native("onMotionEvent");
  g_motion_env = env;
  g_motion_thiz = thiz;
  if (!g_on_motion_event) {
    debugPrintf("autotap: onMotionEvent nao registrado\n");
    return;
  }

  pthread_t tid;
  int rc = pthread_create(&tid, NULL, autotap_thread, NULL);
  if (rc == 0) {
    pthread_detach(tid);
    debugPrintf("autotap: thread iniciada (%p)\n", (void *)g_on_motion_event);
  } else {
    debugPrintf("autotap: pthread_create falhou rc=%d\n", rc);
  }
}

#define MAP_SNAPSHOT_MAX 256
struct map_snapshot {
  uintptr_t start;
  uintptr_t end;
  unsigned long off;
  char perms[8];
  char path[128];
};
static struct map_snapshot g_maps[MAP_SNAPSHOT_MAX];
static int g_maps_n;

static void snapshot_maps(void) {
  FILE *fp = fopen("/proc/self/maps", "r");
  if (!fp)
    return;

  g_maps_n = 0;
  char line[512];
  while (g_maps_n < MAP_SNAPSHOT_MAX && fgets(line, sizeof(line), fp)) {
    unsigned long start = 0, end = 0, off = 0, inode = 0;
    char perms[8] = {0};
    char dev[32] = {0};
    char path[256] = {0};
    int n = sscanf(line, "%lx-%lx %7s %lx %31s %lu %255[^\n]", &start, &end,
                   perms, &off, dev, &inode, path);
    if (n < 6)
      continue;
    char *name = path;
    while (*name == ' ')
      name++;
    if (!*name)
      name = "[anon]";
    g_maps[g_maps_n].start = (uintptr_t)start;
    g_maps[g_maps_n].end = (uintptr_t)end;
    g_maps[g_maps_n].off = off;
    snprintf(g_maps[g_maps_n].perms, sizeof(g_maps[g_maps_n].perms), "%s",
             perms);
    snprintf(g_maps[g_maps_n].path, sizeof(g_maps[g_maps_n].path), "%s",
             name);
    g_maps_n++;
  }
  fclose(fp);
}

static void print_map_suffix(uintptr_t addr) {
  for (int i = 0; i < g_maps_n; i++) {
    if (addr >= g_maps[i].start && addr < g_maps[i].end) {
      fprintf(stderr, " (%s+0x%lx map=%lx-%lx)", g_maps[i].path,
              (unsigned long)(g_maps[i].off + (addr - g_maps[i].start)),
              (unsigned long)g_maps[i].start, (unsigned long)g_maps[i].end);
      break;
    }
  }
}

static int map_info(uintptr_t addr, const char **path, unsigned long *off) {
  for (int i = 0; i < g_maps_n; i++) {
    if (addr >= g_maps[i].start && addr < g_maps[i].end) {
      if (path)
        *path = g_maps[i].path;
      if (off)
        *off = g_maps[i].off + (addr - g_maps[i].start);
      return 1;
    }
  }
  return 0;
}

static void dump_code_near_addr(const char *label, uintptr_t addr) {
  for (int i = 0; i < g_maps_n; i++) {
    if (addr < g_maps[i].start || addr >= g_maps[i].end ||
        !strchr(g_maps[i].perms, 'r'))
      continue;
    uintptr_t lo = addr > g_maps[i].start + 32 ? addr - 32 : g_maps[i].start;
    uintptr_t hi = addr + 64 < g_maps[i].end ? addr + 64 : g_maps[i].end;
    lo &= ~(uintptr_t)3;
    fprintf(stderr, "  --- code bytes near %s ---\n", label);
    for (uintptr_t a = lo; a < hi; a += 16) {
      fprintf(stderr, "    %p:", (void *)a);
      for (uintptr_t b = a; b < a + 16 && b < hi; b++)
        fprintf(stderr, " %02x", *(unsigned char *)b);
      fprintf(stderr, "\n");
    }
    fprintf(stderr, "  --- code words near %s ---\n", label);
    for (uintptr_t a = lo; a + 4 <= hi; a += 4)
      fprintf(stderr, "    %p: %08x%s\n", (void *)a, *(uint32_t *)a,
              (a <= addr && addr < a + 4) ? "  <HERE>" : "");
    break;
  }
}

/* ---- crash handler ARMHF (campos arm_pc/arm_r0/arm_lr do sigcontext 32-bit) ---- */
static void crash_handler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  mcontext_t *m = &uc->uc_mcontext;
  uintptr_t pc = m->arm_pc, lr = m->arm_lr, fault = (uintptr_t)info->si_addr;
  uintptr_t text = (uintptr_t)text_base;
  uintptr_t main_text = g_main_text_base;
  snapshot_maps();

  const char *pc_path = NULL;
  unsigned long pc_off = 0;
  int pc_mapped = map_info(pc, &pc_path, &pc_off);
  static int suppressed_self_signal;
  if ((sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) &&
      m->arm_r2 == (uintptr_t)sig &&
      m->arm_r7 == 0x10c &&
      pc_mapped && pc_path && strstr(pc_path, "libc.so") &&
      pc_off >= 0x7c700 && pc_off <= 0x7c730 &&
      suppressed_self_signal < 64) {
    fprintf(stderr,
            "\n=== SUPPRESS self signal %d via pthread_kill pc=%p libc+0x%lx tid=0x%lx count=%d ===\n",
            sig,
            (void *)pc, pc_off, (unsigned long)m->arm_r1,
            suppressed_self_signal + 1);
    fflush(stderr);
    suppressed_self_signal++;
    m->arm_r0 = 0;
    return;
  }

  fprintf(stderr, "\n=== CRASH sig=%d addr=%p ===\n", sig, (void *)fault);
  fprintf(stderr, "  PC=%p", (void *)pc);
  if (main_text && pc >= main_text && pc < main_text + g_main_text_size)
    fprintf(stderr, " (%s+0x%lx)", SO_NAME,
            (unsigned long)(pc - main_text));
  else if (pc >= text && pc < text + text_size)
    fprintf(stderr, " (%s+0x%lx)", SO_NAME, (unsigned long)(pc - text));
  else
    print_map_suffix(pc);
  fprintf(stderr, "\n  LR=%p", (void *)lr);
  if (main_text && lr >= main_text && lr < main_text + g_main_text_size)
    fprintf(stderr, " (%s+0x%lx)", SO_NAME,
            (unsigned long)(lr - main_text));
  else if (lr >= text && lr < text + text_size)
    fprintf(stderr, " (%s+0x%lx)", SO_NAME, (unsigned long)(lr - text));
  else
    print_map_suffix(lr);
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
  if (g_gamefile_read_import_addr) {
    const char *imp_path = NULL;
    unsigned long imp_off = 0;
    if (map_info(g_gamefile_read_import_addr, &imp_path, &imp_off)) {
      uint32_t *w = (uint32_t *)g_gamefile_read_import_addr;
      fprintf(stderr,
              "  GAMEFILE_READ_IMPORT=%p words=%08x %08x map=%s+0x%lx\n",
              (void *)g_gamefile_read_import_addr, w[0], w[1],
              imp_path ? imp_path : "?", imp_off);
    }
  }

  dump_code_near_addr("PC", pc);
  if (lr != pc)
    dump_code_near_addr("LR", lr);

  /* backtrace simples via FP chain (ARM: [fp-4]=lr salvo, [fp-8]=fp ant.;
   * varia por -fomit-frame-pointer, então é best-effort) */
  fprintf(stderr, "  --- stack scan (retornos no .so) ---\n");
  uintptr_t sp = m->arm_sp;
  int n = 0;
  for (uintptr_t a = sp; a < sp + 0x2000 && n < 24; a += 4) {
    uintptr_t v = *(uintptr_t *)a;
    if (main_text && v >= main_text && v < main_text + g_main_text_size) {
      fprintf(stderr, "    [sp+0x%lx] %s+0x%lx\n", (unsigned long)(a - sp),
              SO_NAME, (unsigned long)(v - main_text));
      n++;
    } else if (v >= text && v < text + text_size) {
      fprintf(stderr, "    [sp+0x%lx] %s+0x%lx\n", (unsigned long)(a - sp),
              SO_NAME, (unsigned long)(v - text));
      n++;
    }
  }
  fprintf(stderr, "  --- stack scan (mapas executaveis) ---\n");
  n = 0;
  for (uintptr_t a = sp; a < sp + 0x2000 && n < 48; a += 4) {
    uintptr_t v = *(uintptr_t *)a;
    for (int i = 0; i < g_maps_n; i++) {
      if (!strchr(g_maps[i].perms, 'x'))
        continue;
      if (v >= g_maps[i].start && v < g_maps[i].end) {
        fprintf(stderr, "    [sp+0x%lx] %p %s+0x%lx map=%lx-%lx %s\n",
                (unsigned long)(a - sp), (void *)v, g_maps[i].path,
                (unsigned long)(g_maps[i].off + (v - g_maps[i].start)),
                (unsigned long)g_maps[i].start, (unsigned long)g_maps[i].end,
                g_maps[i].perms);
        n++;
        break;
      }
    }
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

static void probe_handler(int sig, siginfo_t *info, void *uctx) {
  (void)info;
  ucontext_t *uc = (ucontext_t *)uctx;
  mcontext_t *m = &uc->uc_mcontext;
  uintptr_t pc = m->arm_pc, lr = m->arm_lr;
  uintptr_t main_text = g_main_text_base;

  fprintf(stderr, "\n=== PROBE sig=%d tid=%ld ===\n", sig,
          (long)syscall(SYS_gettid));
  fprintf(stderr, "  PC=%p", (void *)pc);
  if (main_text && pc >= main_text && pc < main_text + g_main_text_size)
    fprintf(stderr, " (%s+0x%lx)", SO_NAME,
            (unsigned long)(pc - main_text));
  else
    print_map_suffix(pc);
  fprintf(stderr, "\n  LR=%p", (void *)lr);
  if (main_text && lr >= main_text && lr < main_text + g_main_text_size)
    fprintf(stderr, " (%s+0x%lx)", SO_NAME,
            (unsigned long)(lr - main_text));
  else
    print_map_suffix(lr);
  fprintf(stderr, "\n");
  fprintf(stderr, "  r0=%08lx r1=%08lx r2=%08lx r3=%08lx sp=%08lx\n",
          (unsigned long)m->arm_r0, (unsigned long)m->arm_r1,
          (unsigned long)m->arm_r2, (unsigned long)m->arm_r3,
          (unsigned long)m->arm_sp);
  fprintf(stderr, "=== END PROBE ===\n");
  fflush(stderr);
}

static void install_probe_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = probe_handler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGUSR1, &sa, NULL);
}

/* tabela combinada acumulada (base + snapshots dos módulos já carregados) */
static DynLibFunction *g_comb;
static int g_comb_n;
static uintptr_t g_main_jni_onload;
static uintptr_t g_main_android_main;
struct ext_register {
  const char *name;
  uintptr_t addr;
};
static struct ext_register g_ext_registers[8];
static int g_ext_registers_n;
static void comb_append(DynLibFunction *tbl, int n) {
  g_comb = realloc(g_comb, sizeof(DynLibFunction) * (g_comb_n + n));
  memcpy(g_comb + g_comb_n, tbl, sizeof(DynLibFunction) * n);
  g_comb_n += n;
}

static const char *env_or_default(const char *name, const char *fallback) {
  const char *v = getenv(name);
  return (v && *v) ? v : fallback;
}

static int marm_cxa_guard_acquire(unsigned char *g) {
  if (!g || g[0])
    return 0;
  if (g[1])
    return 0;
  g[1] = 1;
  return 1;
}

static void marm_cxa_guard_release(unsigned char *g) {
  if (!g)
    return;
  g[0] = 1;
  g[1] = 0;
}

static void marm_cxa_guard_abort(unsigned char *g) {
  if (g)
    g[1] = 0;
}

static void *marm_s3eEdkJNIGetVM(void) {
  return g_fake_vm_for_ext;
}

static void marm_s3eDeviceExit(void) {
  debugPrintf("hook: s3eDeviceExit chamado; ignorando para diagnostico\n");
}

static void marm_s3eDebugOutputString(const char *msg) {
  void *ra = __builtin_return_address(0);
  debugPrintf("hook: s3eDebugOutputString: %s [caller=%p base-rel=0x%lx]\n",
              msg ? msg : "(null)", ra,
              g_main_text_base ? (unsigned long)((uintptr_t)ra - g_main_text_base)
                               : 0ul);
}

static int (*real_s3eKeyboardGetState)(int);
static int (*real_s3eAudioGetInt)(int);
static uintptr_t g_audio_diag_game_base;
uintptr_t pes_get_game_base_for_diag(void) { return g_audio_diag_game_base; }

static void *make_arm_trampoline(uintptr_t addr) {
  if (!addr)
    return NULL;
  uint32_t *src = (uint32_t *)addr;
  uint32_t *tr = mmap(NULL, 16, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr == MAP_FAILED)
    return NULL;
  tr[0] = src[0];
  tr[1] = src[1];
  tr[2] = 0xe51ff004u;      /* LDR PC, [PC, #-4] */
  tr[3] = (uint32_t)(addr + 8);
  __builtin___clear_cache((char *)tr, (char *)tr + 16);
  return tr;
}

static int marm_s3eKeyboardGetState(int key) {
  static int log_count;
  const char *force = getenv("SONIC4EP1_S3EKEY_FORCE");
  const char *value = getenv("SONIC4EP1_S3EKEY_VALUE");
  int forced_key = force && *force ? atoi(force) : -1;
  int forced_value = value && *value ? atoi(value) : 1;
  int original = real_s3eKeyboardGetState ? real_s3eKeyboardGetState(key) : 0;

  if (getenv("SONIC4EP1_TRACE_S3EKEY") &&
      (original != 0 || log_count++ < 5000))
    debugPrintf("s3eKeyboardGetState(%d) -> %d%s\n", key, original,
                key == forced_key ? " FORCED" : "");

  if (key == forced_key)
    return forced_value;
  return original;
}

static int marm_s3eAudioGetInt(int key) {
  const char *force = getenv("PES_AUDIO_GETINT_FORCE");
  const char *force_key_env = getenv("PES_AUDIO_GETINT_FORCE_KEY");
  int force_key = force_key_env && *force_key_env ? atoi(force_key_env) : -1;
  int have_force = force && *force && (force_key < 0 || force_key == key);
  int original = have_force ? atoi(force) :
      (real_s3eAudioGetInt ? real_s3eAudioGetInt(key) : 0);
  int ret = original;

  if (have_force)
    ret = atoi(force);

  if (getenv("PES_AUDIO_GETINT_LOG")) {
    static int log_count;
    if (log_count < 240 || original != ret) {
      void *ra = __builtin_return_address(0);
      unsigned off = g_main_text_base && (uintptr_t)ra >= g_main_text_base ?
          (unsigned)((uintptr_t)ra - g_main_text_base) : 0;
      debugPrintf("AUDIOGETINT: key=%d original=%d ret=%d caller=%p off=0x%x\n",
                  key, original, ret, ra, off);
      log_count++;
    }
  }
  if (getenv("PES_AUDIO_GETINT_STACK") && g_audio_diag_game_base) {
    static int stack_logs;
    if (stack_logs < 80) {
      uintptr_t sp = 0;
      __asm__ volatile("mov %0, sp" : "=r"(sp));
      char hits[256];
      int used = 0;
      hits[0] = 0;
      for (unsigned i = 0; i < 160; i++) {
        uintptr_t v = ((uintptr_t *)sp)[i];
        if (v >= g_audio_diag_game_base &&
            v < g_audio_diag_game_base + 0x320000u) {
          int n = snprintf(hits + used, sizeof(hits) - (size_t)used,
                           "%s+0x%x@sp+0x%x", used ? " " : "",
                           (unsigned)(v - g_audio_diag_game_base), i * 4);
          if (n <= 0)
            break;
          used += n;
          if (used >= (int)sizeof(hits) - 32)
            break;
        }
      }
      debugPrintf("AUDIOGETINT_STACK: key=%d ret=%d %s\n",
                  key, ret, hits[0] ? hits : "no-game-ret");
      stack_logs++;
    }
  }
  return ret;
}

/* trampoline thumb-safe: copia 8 bytes de prologo (sem PC-rel) + salta p/ addr+8 */
static void *make_thumb_tramp(uintptr_t addr) {
  uintptr_t a = addr & ~1u;
  uint16_t *src = (uint16_t *)a;
  uint16_t *tr = mmap(NULL, 32, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr == MAP_FAILED)
    return NULL;
  for (int i = 0; i < 4; i++)
    tr[i] = src[i];
  tr[4] = 0xf8df;
  tr[5] = 0xf000;
  *(uint32_t *)(tr + 6) = (uint32_t)((a + 8) | 1u);
  __builtin___clear_cache((char *)tr, (char *)tr + 32);
  return (void *)((uintptr_t)tr | 1u);
}

/* Trampoline thumb que RELOCALIZA um BL de 32-bit no prólogo. Copia as 4
 * halfwords (8 bytes), mas se as halfwords [2..3] forem um BL (F000-F7FF +
 * D000-FFFF), recomputa o offset p/ o mesmo alvo absoluto a partir da posição do
 * trampoline. Necessário p/ s3eFileOpen (prólogo `push;movs;bl helper`). */
static void *make_thumb_tramp_reloc(uintptr_t addr) {
  uintptr_t a = addr & ~1u;
  uint16_t *src = (uint16_t *)a;
  uint16_t *tr = mmap(NULL, 32, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr == MAP_FAILED)
    return NULL;
  for (int i = 0; i < 4; i++)
    tr[i] = src[i];
  /* halfwords [2..3] = BL? (hw1 em [0xF000..0xF7FF], hw2 em [0xD000..0xFFFF]) */
  uint16_t h1 = src[2], h2 = src[3];
  if ((h1 & 0xF800) == 0xF000 && (h2 & 0xD000) == 0xD000) {
    uint32_t S = (h1 >> 10) & 1, imm10 = h1 & 0x3ff;
    uint32_t J1 = (h2 >> 13) & 1, J2 = (h2 >> 11) & 1, imm11 = h2 & 0x7ff;
    uint32_t I1 = 1 - (J1 ^ S), I2 = 1 - (J2 ^ S);
    int32_t off = (int32_t)((S << 24) | (I1 << 23) | (I2 << 22) | (imm10 << 12) |
                            (imm11 << 1));
    if (off & (1 << 24)) off |= 0xFE000000; /* sign-extend 25-bit */
    uintptr_t target = (a + 2 * 2 + 4) + off;       /* PC do BL orig = a+4, +4 */
    uintptr_t newpc = (uintptr_t)(tr + 2);          /* posição do BL no tramp */
    int32_t noff = (int32_t)(target - (newpc + 4));
    uint32_t nS = (noff >> 24) & 1;
    uint32_t nI1 = (noff >> 23) & 1, nI2 = (noff >> 22) & 1;
    uint32_t nimm10 = (noff >> 12) & 0x3ff, nimm11 = (noff >> 1) & 0x7ff;
    uint32_t nJ1 = (1 - nI1) ^ nS, nJ2 = (1 - nI2) ^ nS;
    tr[2] = 0xF000 | (nS << 10) | nimm10;
    tr[3] = 0xD000 | (nJ1 << 13) | (nJ2 << 11) | nimm11;
  }
  tr[4] = 0xf8df;
  tr[5] = 0xf000;
  *(uint32_t *)(tr + 6) = (uint32_t)((a + 8) | 1u);
  __builtin___clear_cache((char *)tr, (char *)tr + 32);
  return (void *)((uintptr_t)tr | 1u);
}

/* Trampoline thumb COMPLETO: copia 4 halfwords do prólogo relocalizando `ldr
 * rX,[pc,#imm]` (via literal local) e um `bl` de 32-bit, depois salta p/ addr+8.
 * Cobre os prólogos de s3eFileGetSize/Close (ldr-pc) e s3eFileOpen (bl). */
static void *make_thumb_tramp_full(uintptr_t addr) {
  uintptr_t a = addr & ~1u;
  uint16_t *src = (uint16_t *)a;
  uint16_t *tr = mmap(NULL, 128, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr == MAP_FAILED)
    return NULL;
  int litn = 0;               /* literais em tr[48..] (halfword idx 48) */
  int o = 0;
  uint32_t *litpool = (uint32_t *)(tr + 48);
  for (int i = 0; i < 4; i++) {
    uint16_t h = src[i];
    if ((h & 0xF800) == 0x4800) { /* ldr rX,[pc,#imm8] */
      uint32_t rd = (h >> 8) & 7, imm8 = h & 0xff;
      uintptr_t litaddr = ((a + i * 2 + 4) & ~3u) + imm8 * 4;
      uint32_t val = *(uint32_t *)litaddr;
      litpool[litn] = val;
      uintptr_t newpc = ((uintptr_t)(tr + o) + 4) & ~3u;
      uint32_t noff = (uint32_t)((uintptr_t)&litpool[litn] - newpc) / 4;
      tr[o++] = 0x4800 | (rd << 8) | (noff & 0xff);
      litn++;
    } else if (i <= 2 && (h & 0xF800) == 0xF000 &&
               (src[i + 1] & 0xD000) == 0xD000) { /* bl 32-bit */
      uint16_t h2 = src[i + 1];
      uint32_t S = (h >> 10) & 1, imm10 = h & 0x3ff;
      uint32_t J1 = (h2 >> 13) & 1, J2 = (h2 >> 11) & 1, imm11 = h2 & 0x7ff;
      uint32_t I1 = 1 - (J1 ^ S), I2 = 1 - (J2 ^ S);
      int32_t off = (int32_t)((S << 24) | (I1 << 23) | (I2 << 22) |
                              (imm10 << 12) | (imm11 << 1));
      if (off & (1 << 24)) off |= 0xFE000000;
      uintptr_t target = (a + i * 2 + 4) + off;
      if (o & 1)
        tr[o++] = 0xbf00;       /* alinha literal/ldr.w em palavra */
      tr[o++] = 0xf8df;         /* ldr.w ip, [pc, #4] */
      tr[o++] = 0xc004;
      tr[o++] = 0x47e0;         /* blx ip */
      tr[o++] = 0xe001;         /* retorno do BLX pula o literal */
      *(uint32_t *)(tr + o) = (uint32_t)(target | 1u);
      o += 2;
      i++; /* consumiu 2 halfwords */
    } else {
      tr[o++] = h;
    }
  }
  if (o & 1)
    tr[o++] = 0xbf00;
  tr[o++] = 0xf8df; tr[o++] = 0xf000;          /* ldr.w pc,[pc,#0] */
  *(uint32_t *)(tr + o) = (uint32_t)((a + 8) | 1u);
  __builtin___clear_cache((char *)tr, (char *)tr + 128);
  return (void *)((uintptr_t)tr | 1u);
}

typedef int (*fn4_t)(int, int, int, int);
/* Redirect da API de memória s3e (heaps de tamanho fixo, heap 6 nao criada,
 * heap 0 pequena) p/ malloc/free/realloc do sistema (RAM 832MB). r0=size no
 * MallocBase; r0=ptr no Free; (r0=ptr,r1=size) no Realloc. */
static void *w_s3eMallocBase(int size, int a, int b, int c) {
  (void)a; (void)b; (void)c;
  return malloc(size > 0 ? (size_t)size : 1);
}
static void w_s3eFreeBase(void *p) { if (p) free(p); }
/* Bypass da verificação de licença (DRM Google Play LVL): o jogo (EXEC) verifica
 * a assinatura RSA da resposta via s3eCryptoVerifyRsa -> sem Google Play falha
 * ("License verification failed"). MAS o mesmo s3eCryptoVerifyRsa é usado pelo
 * LOADER p/ verificar a assinatura (VÁLIDA) do próprio .s3e. Então: se o caller
 * for o loader (return-addr na região do .so), roda o REAL; se for o exec
 * (return-addr fora, addr baixo), força "válido". */
typedef int (*fn6_t)(int, int, int, int, int, int);
static fn6_t real_verifyrsa;
static int w_s3eCryptoVerifyRsa(int a, int b, int c, int d, int e, int f) {
  uintptr_t ra = (uintptr_t)__builtin_return_address(0);
  if (real_verifyrsa && ra >= g_main_text_base &&
      ra < g_main_text_base + g_main_text_size) {
    return real_verifyrsa(a, b, c, d, e, f); /* loader/.s3e: verify real */
  }
  const char *v = getenv("PES_RSA_VAL"); /* exec/licença: força válido */
  return v ? atoi(v) : 1;
}
static void *w_s3eReallocBase(void *p, int size, int a, int b) {
  (void)a; (void)b;
  return realloc(p, size > 0 ? (size_t)size : 1);
}

/* 13 inits de subsistema chamados pelas branches por-bit do s3eDeviceCheckCaps.
 * Um(ns) falha(m) no so-loader -> aborta a cadeia -> ThreadCore/TLS nao roda ->
 * crash pos-load. Wrapper: roda o original e FORCA return 0 (registra sempre). */
#define CAPS_INITS(X) \
  X(38084) X(3b6ac) X(3f22c) X(3f9a8) X(42788) X(42e00) X(438bc) \
  X(44a64) X(4b89c) X(4ee30) X(59e0c) X(5b198) X(5fc00)
#define DECL_CI(OFF) \
  static fn4_t real_ci_##OFF; \
  static int w_ci_##OFF(int a, int b, int c, int d) { \
    int r = real_ci_##OFF(a, b, c, d); \
    if (r) debugPrintf("caps-init 0x" #OFF " retornou %d -> forcando 0\n", r); \
    return 0; \
  }
CAPS_INITS(DECL_CI)

static fn4_t real_ld_2413c, real_ld_2460c, real_ld_248ac, real_ld_24504;
static fn4_t real_ld_47e64, real_ld_49b70, real_ld_470b0;
static void add_native_handle(void *h);
/* s3eFileOpen exportado (0x4753c) — HOOKADO por install_s3efile_exports p/ nosso
 * fopen real. Chamamos via ponteiro pro endereco hookado. */
static void *(*g_s3eFileOpen)(const char *, const char *);

static int pes_ends_with_ci(const char *s, const char *suffix) {
  if (!s || !suffix)
    return 0;
  size_t sn = strlen(s), fn = strlen(suffix);
  if (sn < fn)
    return 0;
  s += sn - fn;
  for (size_t i = 0; i < fn; i++) {
    if (tolower((unsigned char)s[i]) !=
        tolower((unsigned char)suffix[i]))
      return 0;
  }
  return 1;
}

static int pes_contains_ci(const char *s, const char *needle) {
  if (!s || !needle || !*needle)
    return 0;
  size_t nn = strlen(needle);
  for (; *s; s++) {
    size_t i = 0;
    while (i < nn && s[i] &&
           tolower((unsigned char)s[i]) ==
               tolower((unsigned char)needle[i]))
      i++;
    if (i == nn)
      return 1;
  }
  return 0;
}

static int pes_should_skip_group_path(const char *path) {
  if (!path || !pes_ends_with_ci(path, ".group.bin"))
    return 0;
  if (getenv("PES_SKIP_SOUND") && pes_contains_ci(path, "sound"))
    return 1;
  if (getenv("PES_SKIP_MUSIC_GROUPS") && !getenv("PES_MUSIC_REAL") &&
      pes_contains_ci(path, "music"))
    return 1;
  return 0;
}

static int pes_group_should_prefer_archive(const char *path) {
  if (!path || !pes_ends_with_ci(path, ".group.bin"))
    return 0;
  if (getenv("PES_FORCE_GRP_ARCH"))
    return 1;
  if (getenv("PES_SOUND_DISK"))
    return 0;
  return pes_contains_ci(path, "sound/") || pes_contains_ci(path, "sound\\") ||
         pes_contains_ci(path, "music/") || pes_contains_ci(path, "music\\");
}

static int w_ld_470b0(int a, int b, int c, int d) {
  const char *path = (const char *)a;
  /* 0x470b0 = fopen INTERNO (VFS s3e vazio) -> falha pro exec E pros assets
   * (menu/hd/*.group.bin etc). Roteia .s3e direto pelo s3eFileOpen exportado
   * (case-insensitive, fopen real). Pros demais: tenta o original; se FALHAR,
   * cai no nosso s3eFileOpen (pega assets do fs real). */
  if (path && g_s3eFileOpen && !getenv("PES_NO_OPEN_BYPASS")) {
    size_t n = strlen(path);
    if (n > 4 && !strcmp(path + n - 4, ".s3e")) {
      void *h = g_s3eFileOpen(path, "rb");
      debugPrintf("TRACE 0x470b0 OPEN-BYPASS \"%s\" -> %p\n", path, h);
      return (int)(intptr_t)h;
    }
    if (pes_should_skip_group_path(path)) {
      static int d = 0;
      if (d < 24) {
        debugPrintf("GRPSKIP: 0x470b0 '%s' -> NULL\n", path);
        d++;
      }
      return 0;
    }
    /* .group.bin: os grupos de menu/database do APK existem em claro no disco;
     * sound/music só existem no OBB e precisam passar pelo user-fs nativo. */
    extern void *g_arch_open_bn(const char *);
    if (n >= 10 && !strcmp(path + n - 10, ".group.bin") &&
        !getenv("PES_NO_GRP_ARCH")) {
      if (!pes_group_should_prefer_archive(path)) {
        void *dh = g_s3eFileOpen(path, "rb");
        if (dh) {
          debugPrintf("TRACE 0x470b0 GRP-DISK \"%s\" -> %p\n", path, dh);
          return (int)(intptr_t)dh;
        }
      }
      if (getenv("PES_GRP_REAL_WORKER") && real_ld_470b0) {
        const char *rel = path;
        if (!strncmp(rel, "data-gles1/", 11) ||
            !strncmp(rel, "data-gles1\\", 11) ||
            !strncmp(rel, "data-gles2/", 11) ||
            !strncmp(rel, "data-gles2\\", 11))
          rel += 11;
        const char *sl = strrchr(rel, '/');
        const char *bs = strrchr(rel, '\\');
        if (bs && (!sl || bs > sl))
          sl = bs;
        const char *base = sl ? sl + 1 : rel;
        char full_lc[512], full_bs[512], base_lc[256];
        size_t j = 0;
        for (const char *c = rel; *c && j < sizeof(full_lc) - 1; c++)
          full_lc[j++] = (char)tolower((unsigned char)*c);
        full_lc[j] = 0;
        snprintf(full_bs, sizeof(full_bs), "%s", full_lc);
        for (char *c = full_bs; *c; c++)
          if (*c == '/')
            *c = '\\';
        j = 0;
        for (const char *c = base; *c && j < sizeof(base_lc) - 1; c++)
          base_lc[j++] = (char)tolower((unsigned char)*c);
        base_lc[j] = 0;
        const char *forms[6];
        if (getenv("PES_GRP_REAL_WORKER_UNSAFE_FORMS")) {
          forms[0] = path;
          forms[1] = full_bs;
          forms[2] = full_lc;
          forms[3] = base_lc;
          forms[4] = rel;
          forms[5] = NULL;
        } else {
          forms[0] = base_lc;
          forms[1] = full_bs;
          forms[2] = full_lc;
          forms[3] = NULL;
        }
        for (int i = 0; forms[i]; i++) {
          static int try_logs = 0;
          if (try_logs < 40) {
            debugPrintf("TRACE 0x470b0 GRP-WORKER try \"%s\" as \"%s\"\n",
                        path, forms[i]);
            try_logs++;
          }
          int wr = real_ld_470b0((int)(intptr_t)forms[i], b, c, d);
          if (wr) {
            void *h = (void *)(intptr_t)wr;
            add_native_handle(h);
            debugPrintf("TRACE 0x470b0 GRP-WORKER \"%s\" as \"%s\" -> %p\n",
                        path, forms[i], h);
            return wr;
          }
        }
      }
      void *ah = g_arch_open_bn(path); /* tenta várias formas internamente */
      debugPrintf("TRACE 0x470b0 GRP-ARCH \"%s\" -> %p\n", path, ah);
      if (ah) return (int)(intptr_t)ah;
    }
    int r = real_ld_470b0(a, b, c, d);
    if (r == 0) { /* VFS interno falhou -> tenta fs real */
      void *h = g_s3eFileOpen(path, "rb");
      if (h) {
        debugPrintf("TRACE 0x470b0 FALLBACK \"%s\" -> %p\n", path, h);
        return (int)(intptr_t)h;
      }
    }
    return r;
  }
  return real_ld_470b0(a, b, c, d);
}
static int w_ld_47e64(int a, int b, int c, int d) {
  const char *path = (const char *)a;
  /* VFS interno do s3e: path sem "raw://" cai no VFS s3e (vazio no so-loader) e
   * falha. O exec (.s3e) vem como nome puro. Redireciona pro "raw://"+abs =
   * filesystem real. So o .s3e (nao quebra outros lookups). */
  if (path && !getenv("PES_NO_VFS_REDIRECT")) {
    size_t n = strlen(path);
    int is_s3e = (n > 4 && !strcmp(path + n - 4, ".s3e"));
    int has_scheme = (strstr(path, "://") != NULL);
    if (is_s3e && !has_scheme) {
      static char buf[512];
      const char *home = getenv("HOME");
      if (path[0] == '/')
        snprintf(buf, sizeof(buf), "raw://%s", path);
      else
        snprintf(buf, sizeof(buf), "raw://%s/%s", home ? home : ".", path);
      debugPrintf("TRACE >0x47e64 REDIR \"%s\" -> \"%s\" (mode=%d)\n", path, buf, b);
      int r = real_ld_47e64((int)(intptr_t)buf, b, c, d);
      debugPrintf("TRACE <0x47e64 REDIR = 0x%x\n", r);
      return r;
    }
  }
  debugPrintf("TRACE >0x47e64(path=\"%s\" mode=%d)\n", path ? path : "(null)", b);
  int r = real_ld_47e64(a, b, c, d);
  debugPrintf("TRACE <0x47e64 = 0x%x\n", r);
  return r;
}
static int w_ld_49b70(int a, int b, int c, int d) {
  const char *path = (const char *)b;
  char *buf = (char *)(intptr_t)a;
  /* 0x49b70 RESOLVE o path via VFS interno (vazio) -> falha p/ o exec. Bypass:
   * p/ .s3e, escreve o path real em buf e retorna 0 (sucesso) -> 0x2460c segue
   * pro add+loader (que abre via fopen). */
  if (path && buf && !getenv("PES_NO_RESOLVE_BYPASS")) {
    size_t n = strlen(path);
    if (n > 4 && !strcmp(path + n - 4, ".s3e")) {
      strncpy(buf, path, 126);
      buf[126] = 0;
      debugPrintf("TRACE 0x49b70 RESOLVE-BYPASS \"%s\" -> buf, ret 0\n", path);
      return 0;
    }
  }
  debugPrintf("TRACE >0x49b70(buf=0x%x path=\"%s\" r2=%d)\n", a,
              path ? path : "(null)", c);
  int r = real_ld_49b70(a, b, c, d);
  debugPrintf("TRACE <0x49b70 = %d\n", r);
  return r;
}
static int w_ld_2413c(int a, int b, int c, int d) {
  debugPrintf("TRACE >0x2413c(a=0x%x)\n", a);
  int r = real_ld_2413c(a, b, c, d);
  debugPrintf("TRACE <0x2413c = %d\n", r);
  return r;
}
static int w_ld_2460c(int a, int b, int c, int d) {
  uintptr_t mgr = g_main_text_base + 0x86350;
  debugPrintf("TRACE >0x2460c(a=0x%x) mgr[0]=0x%x count[+32]=0x%x\n", a,
              *(unsigned int *)mgr, *(unsigned int *)(mgr + 32));
  int r = real_ld_2460c(a, b, c, d);
  debugPrintf("TRACE <0x2460c = %d  mgr[0]=0x%x count[+32]=0x%x\n", r,
              *(unsigned int *)mgr, *(unsigned int *)(mgr + 32));
  return r;
}
static int w_ld_248ac(int a, int b, int c, int d) {
  debugPrintf("TRACE >0x248ac(a=0x%x)\n", a);
  int r = real_ld_248ac(a, b, c, d);
  debugPrintf("TRACE <0x248ac = %d\n", r);
  return r;
}
static int w_ld_24504(int a, int b, int c, int d) {
  debugPrintf("TRACE >0x24504(a=0x%x)\n", a);
  int r = real_ld_24504(a, b, c, d);
  debugPrintf("TRACE <0x24504 = 0x%x\n", r);
  return r;
}

/* DIAGNOSTICO: conta chamadas a s3eDeviceYield. Se o game main LOOPA, dispara
 * rapido; se retornou/parkou, fica baixo. No-op (retorna 0) -> testa tambem se
 * o jogo roda inline free-running sem o yield real. */
static volatile int g_yield_count;
static int marm_s3eDeviceYield(int ms) {
  int n = ++g_yield_count;
  if (n <= 30 || (n % 120) == 0)
    debugPrintf("YIELD #%d (ms=%d)\n", n, ms);
  return 0;
}

static int ptr_range_mapped(const void *p, size_t len);
static const char *safe_cstr_preview(const char *s, char *buf, size_t len) {
  if (!s)
    return "(null)";
  if (len == 0)
    return "";
  snapshot_maps();
  if ((uintptr_t)s < 0x10000u || !ptr_range_mapped(s, 1)) {
    snprintf(buf, len, "<invalid:%p>", s);
    return buf;
  }

  size_t i = 0;
  for (; i + 1 < len; i++) {
    if (!ptr_range_mapped(s + i, 1)) {
      if (i + 6 < len) {
        memcpy(buf + i, "<...>", 6);
        i += 5;
      }
      break;
    }
    unsigned char c = (unsigned char)s[i];
    if (!c)
      break;
    buf[i] = (c >= 32 && c < 127) ? (char)c : '.';
  }
  buf[i < len ? i : len - 1] = 0;
  return buf;
}

static int marm_s3eDebugErrorShow(const char *title, const char *msg) {
  uintptr_t ra = (uintptr_t)__builtin_return_address(0);
  uintptr_t base = g_main_text_base;
  char title_buf[96], msg_buf[192];
  debugPrintf("hook: s3eDebugErrorShow: title=%s msg=%s [caller +0x%lx]\n",
              safe_cstr_preview(title, title_buf, sizeof(title_buf)),
              safe_cstr_preview(msg, msg_buf, sizeof(msg_buf)),
              base ? (unsigned long)(ra - base) : (unsigned long)ra);
  return 0;
}

static int marm_s3eEdkErrorSet(int group, int code, int type) {
  static int n;
  if (n < 4) {
    void *ra = __builtin_return_address(0);
    debugPrintf("hook: s3eEdkErrorSet(group=0x%x, code=%d, type=%d) caller base-rel=0x%lx str='%.40s'\n",
                group, code, type,
                g_main_text_base ? (unsigned long)((uintptr_t)ra - g_main_text_base) : 0ul,
                (group > 0x10000) ? (const char *)(uintptr_t)group : "?");
    n++;
  }
  return 0;
}

static int marm_file_init_finalizer_ok(void) {
  int (*orig)(void) = (int (*)(void))(g_main_text_base + 0x3cde0);
  debugPrintf("diag: file init finalizer 0x69114 -> 0x3cde0\n");
  int r = orig();
  debugPrintf("diag: file init finalizer original returned %d; forcing OK\n", r);
  return 0;
}

static int marm_entry_2688c(int arg) {
  int (*orig)(int) = (int (*)(int))(g_main_text_base + 0x267fc);
  debugPrintf("diag: entry 0x2688c(arg=%d) -> original 0x267fc\n", arg);
  int r = orig(arg);
  debugPrintf("diag: entry 0x2688c returned %d\n", r);
  return r;
}

/*
 * s3e.icf embutido. O s3e File VFS (rom://) nunca e montado neste so-loader,
 * entao o loader nunca acha s3e.icf como arquivo. A funcao 0x138a4 carrega o
 * icf EMBUTIDO via s3eFileOpenFromMemory([global_deploy+296]). Esse global e
 * zerado pela init do s3e DEPOIS de qualquer injecao precoce, entao injetamos
 * os ponteiros na ENTRADA do gate 0x13c44 (que chama 0x138a4 logo em seguida).
 */
static const char g_s3e_icf_text[] =
    "[S3E]\n"
    "GameExecutable=Sonic4epI.s3e\n"
    "MemSize=33554432\n"
    "SysGlesVersion=1\n"
    "DispFixRot=FixedLandscape\n"
    "MemTooSmallSkipCheck=1\n";

/* ===================================================================
 * SHIM da API s3eFile -> libc real (bypass do VFS s3e que nao monta).
 * O so-loader nao inicializa os drives do s3e File (rom://, ram://,
 * memory), entao s3eFileOpen/OpenFromMemory crasham (estado NULL).
 * Substituimos a API inteira por fopen/fread/fmemopen no cwd real,
 * onde estao Sonic4epI.s3e, s3e.icf, etc.
 * =================================================================== */
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

/* Diretorio de deploy (rom://) — preenchido no gate; usado como 2a raiz de
 * busca pra assets (ex.: AUDIO/) que nao estao no cwd. */
static char g_deploy_dir[512];

/*
 * Abre `rel` sob `base`, resolvendo CADA componente do path de forma
 * case-INSENSITIVE contra o FS real (o s3e File baixa os nomes pra minuscula:
 * "Sonic4epI.s3e" -> "sonic4epi.s3e"; no FS case-sensitive isso falha).
 * Tecnica reusada do port Castlevania. Retorna FILE* ou NULL.
 */
static FILE *ep1_ci_open(const char *base, const char *rel, const char *mode) {
  if (!rel || !*rel)
    return NULL;
  char cur[1024];
  if (base && *base)
    snprintf(cur, sizeof(cur), "%s", base);
  else
    snprintf(cur, sizeof(cur), ".");
  const char *seg = rel;
  while (*seg == '/')
    seg++;
  while (*seg) {
    const char *slash = strchr(seg, '/');
    size_t len = slash ? (size_t)(slash - seg) : strlen(seg);
    if (len == 0 || len >= 256) {
      if (slash) {
        seg = slash + 1;
        continue;
      }
      break;
    }
    char comp[256];
    memcpy(comp, seg, len);
    comp[len] = '\0';
    char cand[1300];
    snprintf(cand, sizeof(cand), "%s/%s", cur, comp);
    struct stat st;
    if (stat(cand, &st) == 0) {
      snprintf(cur, sizeof(cur), "%s", cand);
    } else {
      DIR *d = opendir(cur);
      int found = 0;
      if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
          if (strcasecmp(e->d_name, comp) == 0) {
            char nxt[1300];
            snprintf(nxt, sizeof(nxt), "%s/%s", cur, e->d_name);
            snprintf(cur, sizeof(cur), "%s", nxt);
            found = 1;
            break;
          }
        }
        closedir(d);
      }
      if (!found)
        return NULL;
    }
    if (!slash)
      break;
    seg = slash + 1;
  }
  return fopen(cur, (mode && *mode) ? mode : "rb");
}

static FILE *ep1_file_real_open(const char *p, const char *mode) {
  if (!p)
    return NULL;
  const char *s = p;
  const char *colon = strstr(p, "://"); /* rom:// ram:// raw:// */
  if (colon)
    s = colon + 3;
  while (*s == '/')
    s++;
  const char *m = (mode && *mode) ? mode : "rb";
  FILE *f = fopen(s, m);
  if (!f && s != p)
    f = fopen(p, m);
  int is_write = m && (strchr(m, 'w') || strchr(m, 'a') || strchr(m, '+'));
  /* os assets EM CLARO do APK estão SEM o prefixo "data-gles1/" (menu/, database/,
   * string/). O jogo pede "data-gles1/menu/...". Tenta também sem o prefixo. */
  const char *rel = s;
  if (!strncmp(rel, "data-gles1/", 11)) rel += 11;
  else if (!strncmp(rel, "data-gles1\\", 11)) rel += 11;
  if (!f && rel != s) {
    f = fopen(rel, m);
    if (!f && !is_write) f = ep1_ci_open(".", rel, m);
    if (!f && !is_write && g_deploy_dir[0]) f = ep1_ci_open(g_deploy_dir, rel, m);
  }
  /* fallback case-insensitive no cwd e depois no deploy_dir (assets extraidos
   * do APK: AUDIO/, splash, etc.). So pra leitura — escrita ja foi criada acima. */
  if (!f && !is_write)
    f = ep1_ci_open(".", s, m);
  if (!f && !is_write && g_deploy_dir[0])
    f = ep1_ci_open(g_deploy_dir, s, m);
  if (getenv("SONIC4EP1_FILELOG"))
    debugPrintf("s3eFile: open '%s' (real '%s' mode '%s') -> %p\n", p, s, m,
                (void *)f);
  return f;
}

struct pkg_disk_handle {
  uint32_t magic;
  int fd;
  off_t base;
  off_t size;
  char path[256];
};

#define PKGDISK_MAGIC 0x5047445au /* PGDZ */
#define PKGDISK_MAX 16
static struct pkg_disk_handle *g_pkgdisk[PKGDISK_MAX];
static int g_pkgdisk_n;

static int is_pkgdisk_handle(void *h) {
  /* membership PRIMEIRO: h pode ser handle nativo s3e (ex. 0x3e8, não-ponteiro)
   * — deref de p->magic antes da lista segfaulta. */
  struct pkg_disk_handle *p = (struct pkg_disk_handle *)h;
  if (!p)
    return 0;
  for (int i = 0; i < g_pkgdisk_n; i++)
    if (g_pkgdisk[i] == p)
      return p->magic == PKGDISK_MAGIC;
  return 0;
}

static void add_pkgdisk_handle(struct pkg_disk_handle *p) {
  if (!p)
    return;
  for (int i = 0; i < g_pkgdisk_n; i++)
    if (g_pkgdisk[i] == p)
      return;
  if (g_pkgdisk_n < PKGDISK_MAX)
    g_pkgdisk[g_pkgdisk_n++] = p;
}

static void del_pkgdisk_handle(struct pkg_disk_handle *p) {
  for (int i = 0; i < g_pkgdisk_n; i++) {
    if (g_pkgdisk[i] == p) {
      g_pkgdisk[i] = g_pkgdisk[--g_pkgdisk_n];
      return;
    }
  }
}

static void *open_package_dz_disk_fallback(const char *fn, const char *mode) {
  (void)mode;
  const char *home = getenv("HOME");
  if (!home || !*home)
    home = ".";

  char scheme_path[640] = {0};
  char slash_path[640] = {0};
  char abs_home[640];
  char cwd_path[640] = {0};
  char obb_home[900];
  const char *colon = fn ? strstr(fn, "://") : NULL;
  if (colon)
    snprintf(scheme_path, sizeof(scheme_path), "%s", colon + 3);
  if (fn && (strchr(fn, '\\') || !strncmp(fn, "raw:", 4))) {
    const char *src = fn;
    if (!strncmp(src, "raw:", 4))
      src += 4;
    size_t j = 0;
    while (*src == '\\' && j < sizeof(slash_path) - 1) {
      slash_path[j++] = '/';
      src++;
    }
    for (; *src && j < sizeof(slash_path) - 1; src++) {
      slash_path[j++] = *src == '\\' ? '/' : *src;
    }
    slash_path[j] = 0;
  }
  snprintf(abs_home, sizeof(abs_home), "%s/package.dz", home);
  if (getcwd(cwd_path, sizeof(cwd_path))) {
    size_t used = strlen(cwd_path);
    snprintf(cwd_path + used, sizeof(cwd_path) - used, "/package.dz");
  } else {
    cwd_path[0] = 0;
  }
  snprintf(obb_home, sizeof(obb_home),
           "%s/Android/obb/com.konami.pes2012/main.1000005.com.konami.pes2012.obb",
           home);

  const char *forms[8];
  int nf = 0;
  if (scheme_path[0])
    forms[nf++] = scheme_path;
  if (slash_path[0])
    forms[nf++] = slash_path;
  if (fn && fn[0] == '/')
    forms[nf++] = fn;
  forms[nf++] = abs_home;
  if (cwd_path[0])
    forms[nf++] = cwd_path;
  forms[nf++] = "package.dz";
  forms[nf++] = obb_home;
  forms[nf] = NULL;

  static int logs;
  for (int i = 0; forms[i]; i++) {
    errno = 0;
    int fd = open(forms[i], O_RDONLY);
    int e = errno;
    if (fd < 0) {
      if (logs < 24) {
        debugPrintf("PKGFILE openfd '%s' -> -1 errno=%d (%s)\n",
                    forms[i], e, strerror(e));
        logs++;
      }
      continue;
    }

    unsigned char b[8] = {0};
    int got = (int)read(fd, b, sizeof(b));
    off_t sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    int ok_outer = got >= 4 && b[0] == 0x79 && b[1] == 0x9c &&
                   b[2] == 0xa8 && b[3] == 0x0a;
    int ok_dtrz = got >= 4 && b[0] == 'D' && b[1] == 'T' &&
                  b[2] == 'R' && b[3] == 'Z';
    off_t base = 0;
    unsigned char inner[4] = {0};
    if (ok_outer && got >= 8) {
      uint32_t dz_off = (uint32_t)b[4] | ((uint32_t)b[5] << 8) |
                        ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
      if (dz_off > 0 && sz > (off_t)(dz_off + 4)) {
        lseek(fd, (off_t)dz_off, SEEK_SET);
        if (read(fd, inner, sizeof(inner)) == 4 &&
            inner[0] == 'D' && inner[1] == 'T' &&
            inner[2] == 'R' && inner[3] == 'Z') {
          base = (off_t)dz_off;
        }
      }
      lseek(fd, 0, SEEK_SET);
    }
    if (ok_outer || ok_dtrz) {
      struct pkg_disk_handle *h = calloc(1, sizeof(*h));
      if (!h) {
        close(fd);
        return NULL;
      }
      h->magic = PKGDISK_MAGIC;
      h->fd = fd;
      h->base = base;
      h->size = sz > base ? sz - base : 0;
      snprintf(h->path, sizeof(h->path), "%s", forms[i]);
      lseek(fd, h->base, SEEK_SET);
      add_pkgdisk_handle(h);
      debugPrintf("PKGFILE open '%s' via '%s' -> %p fd=%d base=0x%lx size=%ld magic=%02x%02x%02x%02x inner=%02x%02x%02x%02x got=%d\n",
                  fn ? fn : "(null)", forms[i], (void *)h, fd, (long)h->base,
                  (long)h->size, b[0], b[1], b[2], b[3],
                  inner[0], inner[1], inner[2], inner[3], got);
      return h;
    }

    if (logs < 24) {
      debugPrintf("PKGFILE invalid '%s' fd=%d magic=%02x%02x%02x%02x got=%d\n",
                  forms[i], fd, b[0], b[1], b[2], b[3], got);
      logs++;
    }
    close(fd);
  }

  return NULL;
}

/* Registro de FILE* abertos: o engine fecha alguns handles DUAS vezes (open ->
 * read -> close + path de cleanup fecha de novo). fclose dobrado libera o cookie
 * da glibc 2x -> "double free detected in tcache". Tornamos o Close idempotente. */
#define EP1_MAX_OPEN_FILES 512
static FILE *g_ep1_open_files[EP1_MAX_OPEN_FILES];
static int g_ep1_open_files_n;
static void ep1_track_open(FILE *f) {
  if (!f)
    return;
  for (int i = 0; i < g_ep1_open_files_n; i++)
    if (g_ep1_open_files[i] == f)
      return;
  if (g_ep1_open_files_n < EP1_MAX_OPEN_FILES)
    g_ep1_open_files[g_ep1_open_files_n++] = f;
}
/* retorna 1 se f estava aberto (e remove); 0 se ja fechado (double-close). */
static int ep1_track_close(FILE *f) {
  if (!f)
    return 0;
  for (int i = 0; i < g_ep1_open_files_n; i++) {
    if (g_ep1_open_files[i] == f) {
      g_ep1_open_files[i] = g_ep1_open_files[--g_ep1_open_files_n];
      return 1;
    }
  }
  return 0;
}

/* Ponteiros pras funções s3eFile REAIS (VFS interno do libpes2012.so, que lê do
 * package.dz montado e DESCRIPTOGRAFA os grupos on-the-fly). Capturados via
 * trampoline ANTES de hookar os exports. Usados p/ extrair-sob-demanda os
 * assets que só existem no OBB cifrado (sound/ etc). */
static void *(*real_s3eFileOpen)(const char *, const char *);
/* worker REAL do open (s3eFileOpen 0x4753c é wrapper: push;movs r2,#0;bl 0x470b0).
 * Chamamos o worker DIRETO (base+0x470b0) p/ pular o wrapper hookado -> sem
 * re-entrar nosso hook. Assinatura: worker(fn, mode, flags=0). */
static void *(*g_worker_open)(const char *, const char *, int);
static int (*real_s3eFileRead)(void *, unsigned int, unsigned int, void *);
static int (*real_s3eFileClose)(void *);
static unsigned int (*real_s3eFileGetSize)(void *);
static int (*real_s3eFileSeek)(void *, int, int);
static int (*real_s3eFileTell)(void *);

/* Rastreio de handles NATIVOS (do VFS s3e/archive). Read/Seek/Close/GetSize/Tell
 * roteiam p/ as funções reais nesses; nos demais (FILE* de disco), usam libc. */
#define NAT_MAX 1024
static void *g_nat[NAT_MAX];
static int g_nat_n;
static void *g_pkg_native_handle;
static int is_native_handle(void *h) {
  for (int i = 0; i < g_nat_n; i++) if (g_nat[i] == h) return 1;
  return 0;
}
static void add_native_handle(void *h) {
  for (int i = 0; h && i < g_nat_n; i++)
    if (g_nat[i] == h)
      return;
  if (h && g_nat_n < NAT_MAX) g_nat[g_nat_n++] = h;
}
static void del_native_handle(void *h) {
  if (h == g_pkg_native_handle)
    g_pkg_native_handle = NULL;
  for (int i = 0; i < g_nat_n; i++)
    if (g_nat[i] == h) { g_nat[i] = g_nat[--g_nat_n]; return; }
}
/* ARCHIVE handles: abertos direto pelo dispatcher user-fs do OBB (cbs[0]); as
 * leituras/seeks DESCRIPTOGRAFAM via cbs[1]/cbs[4]/cbs[5]. Layout Marmalade:
 * [0]Open(fn,mode) [1]Read(buf,esz,n,h) [2]Write [3]Close(h) [4]Seek(h,off,org)
 * [5]Tell(h). */
typedef void *(*arch_open_fn)(const char *, const char *);
typedef int (*arch_read_fn)(void *, unsigned int, unsigned int, void *);
typedef int (*arch_close_fn)(void *);
typedef int (*arch_seek_fn)(void *, int, int);
typedef int (*arch_tell_fn)(void *);
#define ARCH_MAX 256
static void *g_arch[ARCH_MAX];
static int g_arch_n;
static int is_arch_handle(void *h) {
  for (int i = 0; i < g_arch_n; i++) if (g_arch[i] == h) return 1;
  return 0;
}
static void add_arch_handle(void *h) {
  if (h && g_arch_n < ARCH_MAX) g_arch[g_arch_n++] = h;
}
static void del_arch_handle(void *h) {
  for (int i = 0; i < g_arch_n; i++)
    if (g_arch[i] == h) { g_arch[i] = g_arch[--g_arch_n]; return; }
}
extern void *g_ufs_cbs;
/* abre um asset via o dispatcher do archive (descriptografa on-the-fly) */
extern uintptr_t g_cbs_copy[8];
extern int g_cbs_ok;
static void maybe_extract_obb_groups_once(void);
/* dump de uma região mapeada p/ análise offline: [tag][addr][len][bytes]. */
void follow_dump(FILE *o, uintptr_t p, const char *tag, size_t sz) {
  if (!o || !p) return;
  /* limita ao que está mapeado */
  size_t ok = 0;
  while (ok < sz && ptr_range_mapped((void *)(p + ok), 1)) ok++;
  if (ok == 0) return;
  char hdr[64];
  int hn = snprintf(hdr, sizeof hdr, "\n===DUMP %s @%08lx len=%zu===\n",
                    tag, (unsigned long)p, ok);
  fwrite(hdr, 1, hn, o);
  fwrite((void *)p, 1, ok, o);
}
static void *arch_open(const char *bn) {
  if (!g_cbs_ok) return NULL;
  static int diag = 0;
  if (diag < 2) { /* o dispatcher: ldr r0,[pc,#40]=global; if(global+2!=0)return0 */
    uintptr_t base = g_cbs_copy[0] & ~1u;
    uintptr_t global = *(uintptr_t *)(base + 0x30);
    debugPrintf("ARCH_DIAG global=%p [+2]=%u count[+4]=%u\n", (void *)global,
                *(unsigned char *)(global + 2), *(unsigned int *)(global + 4));
    diag++;
  }
  static int d = 0;
  if (d < 8) { debugPrintf("ARCH_OPEN cbs[0]=%p bn='%s'\n", (void *)g_cbs_copy[0], bn); d++; }
  if (getenv("PES_IDX_DUMP")) { /* dump do índice ANTES do Open (sobrevive a hang) */
    static int dumped = 0;
    if (!dumped) {
      dumped = 1;
      uintptr_t base = g_cbs_copy[0] & ~1u;
      uintptr_t global = *(uintptr_t *)(base + 0x30);
      FILE *df = fopen("idxdump.bin", "wb");
      if (df && global) {
        /* dump global (1KB) + segue TODO ponteiro de global e do fs_obj,
         * dumpando 256KB por região mapeada. rótulos p/ análise offline. */
        void follow_dump(FILE *o, uintptr_t p, const char *tag, size_t sz);
        follow_dump(df, global, "GLOBAL", 1024);
        for (int gi = 0; gi < 32; gi++) {
          uintptr_t l = *(uintptr_t *)(global + gi * 4);
          if (l > 0x10000 && ptr_range_mapped((void *)l, 16)) {
            char t[32]; snprintf(t, sizeof t, "G+%d", gi * 4);
            follow_dump(df, l, t, 262144);
          }
        }
        fclose(df);
        debugPrintf("IDX_DUMP: escrito idxdump.bin (global=%p)\n", (void *)global);
      } else if (df) fclose(df);
    }
  }
  arch_open_fn Open = (arch_open_fn)g_cbs_copy[0];
  void *h = Open(bn, "rb");
  if (h) {
    add_arch_handle(h);
    if (getenv("PES_ARCH_HDUMP")) { /* s15d: +0 pos/limit +4 off +12 entry +16 buf +24 size +28 flag */
      unsigned w[8];
      memcpy(w, h, sizeof(w));
      debugPrintf("ARCH_HDUMP h=%p [0]=%08x [1]=%08x [2]=%08x [3(entry)]=%08x "
                  "[4(buf)]=%08x [5]=%08x [6(size)]=%08x [7(flag)]=%08x\n",
                  h, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
      unsigned char *e = (unsigned char *)(uintptr_t)w[3];
      if (e && ptr_range_mapped(e, 16)) {
        unsigned ew[4];
        memcpy(ew, e, sizeof(ew)); /* entrada 16B pode ser DESALINHADA no blob */
        debugPrintf("ARCH_HDUMP entry=%p [0]=%08x [1]=%08x [2]=%08x [3]=%08x\n",
                    (void *)e, ew[0], ew[1], ew[2], ew[3]);
      }
      /* fs_obj: dispatcher global -> lista de fs; fs_obj+4 = cipher (s15d) */
      uintptr_t base = g_cbs_copy[0] & ~1u;
      uintptr_t global = *(uintptr_t *)(base + 0x30);
      if (global && ptr_range_mapped((void *)global, 48)) {
        unsigned gw[12];
        memcpy(gw, (void *)global, sizeof(gw));
        debugPrintf("ARCH_HDUMP global=%p: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                    (void *)global, gw[0], gw[1], gw[2], gw[3], gw[4], gw[5],
                    gw[6], gw[7], gw[8], gw[9], gw[10], gw[11]);
        for (int gi = 10; gi <= 10; gi++) { /* global2 = [global+40] (dump s17) */
          uintptr_t l = gw[gi];
          if (l && ptr_range_mapped((void *)l, 8)) {
            uintptr_t fs = *(uintptr_t *)l; /* global2[0] */
            if (fs && ptr_range_mapped((void *)fs, 0x84)) {
              unsigned fw[6];
              memcpy(fw, (void *)fs, 24);
              unsigned cnt = *(unsigned *)(fs + 0x6c), flg = *(unsigned *)(fs + 0x74);
              debugPrintf("ARCH_HDUMP fs_obj?[g+%d]=%p hdr=%08x cipher[+4]=%08x tab[+12]=%08x +0x6c(count)=%u +0x74(flag)=%u\n",
                          gi * 4, (void *)fs, fw[0], fw[1], fw[3], cnt, flg);
              uintptr_t ci = fw[1];
              if (ci && ptr_range_mapped((void *)ci, 24)) {
                unsigned cw[6];
                memcpy(cw, (void *)ci, sizeof(cw));
                debugPrintf("ARCH_HDUMP cipher=%p: vt?=%08x %08x %08x %08x %08x %08x\n",
                            (void *)ci, cw[0], cw[1], cw[2], cw[3], cw[4], cw[5]);
              } else {
                debugPrintf("ARCH_HDUMP cipher=%p NAO-MAPEADO/NULL\n", (void *)ci);
              }
            }
          }
        }
      }
    }
  }
  return h;
}
static void normalize_group_path(char *dst, size_t dstn, const char *src,
                                 int only_base) {
  if (!dstn)
    return;
  dst[0] = 0;
  if (!src)
    return;
  if (!strncmp(src, "data-gles1/", 11) || !strncmp(src, "data-gles1\\", 11) ||
      !strncmp(src, "data-gles2/", 11) || !strncmp(src, "data-gles2\\", 11))
    src += 11;
  while (*src == '/' || *src == '\\')
    src++;
  const char *base = src;
  if (only_base) {
    for (const char *c = src; *c; c++)
      if (*c == '/' || *c == '\\')
        base = c + 1;
  }
  size_t j = 0;
  for (const char *c = base; *c && j + 1 < dstn; c++) {
    char ch = (char)tolower((unsigned char)*c);
    dst[j++] = (ch == '\\') ? '/' : ch;
  }
  dst[j] = 0;
}

static int extract_env_matches_group(const char *path) {
  const char *env = getenv("PES_EXTRACT_OBB_GROUPS");
  if (!env || !*env || !path)
    return 0;
  if (!strcmp(env, "1"))
    return 1;

  char want[512], want_base[256];
  normalize_group_path(want, sizeof(want), path, 0);
  normalize_group_path(want_base, sizeof(want_base), path, 1);

  char *copy = strdup(env);
  if (!copy)
    return 0;
  int ok = 0;
  for (char *tok = strtok(copy, ",;\n"); tok; tok = strtok(NULL, ",;\n")) {
    while (*tok == ' ' || *tok == '\t')
      tok++;
    char *end = tok + strlen(tok);
    while (end > tok && (end[-1] == ' ' || end[-1] == '\t'))
      *--end = 0;
    if (!*tok)
      continue;
    char have[512], have_base[256];
    normalize_group_path(have, sizeof(have), tok, 0);
    normalize_group_path(have_base, sizeof(have_base), tok, 1);
    if (!strcmp(have, want) || !strcmp(have, want_base) ||
        !strcmp(have_base, want_base)) {
      ok = 1;
      break;
    }
  }
  free(copy);
  return ok;
}

static void make_parent_dirs_for_file(char *path) {
  for (char *c = path; *c; c++) {
    if (*c == '/' || *c == '\\') {
      char save = *c;
      *c = 0;
      if (path[0])
        mkdir(path, 0755);
      *c = save == '\\' ? '/' : save;
    }
  }
}

static void extract_open_archive_group(const char *path, void *h) {
  if (!h || !g_cbs_ok || !extract_env_matches_group(path))
    return;
  size_t n = path ? strlen(path) : 0;
  if (n < 10 || strcmp(path + n - 10, ".group.bin") != 0)
    return;

  static __thread int busy = 0;
  if (busy)
    return;
  busy = 1;

  const char *base = path;
  if (!strncmp(base, "data-gles1/", 11) || !strncmp(base, "data-gles1\\", 11) ||
      !strncmp(base, "data-gles2/", 11) || !strncmp(base, "data-gles2\\", 11))
    base += 11;
  while (*base == '/' || *base == '\\')
    base++;

  char out[1024];
  snprintf(out, sizeof(out), "%s", base);
  for (char *c = out; *c; c++)
    if (*c == '\\')
      *c = '/';
  make_parent_dirs_for_file(out);

  arch_read_fn Read = (arch_read_fn)g_cbs_copy[1];
  arch_seek_fn Seek = (arch_seek_fn)g_cbs_copy[4];
  arch_tell_fn Tell = (arch_tell_fn)g_cbs_copy[5];
  int before = Tell ? Tell(h) : -1;
  unsigned long total = 0;
  FILE *w = fopen(out, "wb");
  if (w && Read) {
    static char chunk[65536];
    for (;;) {
      int got = Read(chunk, 1, sizeof(chunk), h);
      if (got <= 0)
        break;
      fwrite(chunk, 1, (size_t)got, w);
      total += (unsigned long)got;
      if ((unsigned)got < sizeof(chunk) || total > 96u * 1024u * 1024u)
        break;
    }
    fclose(w);
  }
  int seek0 = Seek ? Seek(h, 0, SEEK_SET) : -9999;
  int after = Tell ? Tell(h) : -1;
  debugPrintf("ARCHIVE: open-handle dump '%s' -> '%s' before=%d bytes=%lu seek0=%d after=%d\n",
              path, out, before, total, seek0, after);
  busy = 0;
}
/* wrapper não-static: tenta VÁRIAS formas de path no dispatcher do archive
 * (basename lower/orig, full forward/backslash lower) até uma achar. */
void *g_arch_open_bn(const char *path) {
  if (!path) return NULL;
  if (pes_should_skip_group_path(path)) {
    static int d = 0;
    if (d < 24) {
      debugPrintf("GRPSKIP: archive '%s' -> NULL\n", path);
      d++;
    }
    return NULL;
  }
  const char *sl = strrchr(path, '/'); const char *bs = strrchr(path, '\\');
  if (bs && (!sl || bs > sl)) sl = bs;
  const char *base = sl ? sl + 1 : path;
  /* remove prefixo "data-gles1/" p/ as formas full */
  const char *rel = path;
  if (!strncmp(rel, "data-gles1/", 11)) rel += 11;
  else if (!strncmp(rel, "data-gles1\\", 11)) rel += 11;
  char bnl[256], bno[256], ff[512], fb[512]; size_t j;
  j = 0; for (const char *c = base; *c && j < 255; c++) bnl[j++] = (char)tolower((unsigned char)*c); bnl[j] = 0;
  j = 0; for (const char *c = base; *c && j < 255; c++) bno[j++] = *c; bno[j] = 0;
  j = 0; for (const char *c = rel; *c && j < 511; c++) ff[j++] = (char)tolower((unsigned char)*c); ff[j] = 0;
  j = 0; for (const char *c = rel; *c && j < 511; c++) { char ch = (char)tolower((unsigned char)*c); fb[j++] = (ch == '/') ? '\\' : ch; } fb[j] = 0;
  const char *forms[6] = { bnl, fb, ff, bno, path, NULL };
  for (int i = 0; forms[i]; i++) {
    void *h = arch_open(forms[i]);
    static int d = 0;
    if (d < 30) { debugPrintf("ARCH try '%s' -> %p\n", forms[i], h); d++; }
    if (h) {
      extract_open_archive_group(path, h);
      if (getenv("PES_EAGER_EXTRACT_OBB_GROUPS"))
        maybe_extract_obb_groups_once();
      return h;
    }
  }
  return NULL;
}

/* Extrai `fn` do archive s3e (descriptografando) e grava EM CLARO no disco no
 * mesmo path, criando os diretórios. Retorna FILE* do arquivo gravado, ou NULL
 * se o archive não tem o arquivo. */
/* Só liberado depois que o jogo montou o package.dz (senão o VFS s3e crasha com
 * estado NULL — aviso do Opus). Setado pelo fsm_hook (jni_shim) no estado 10
 * (mount) via pes_archive_set_ready(). */
int g_archive_ready = 0;
void pes_archive_set_ready(void) { g_archive_ready = 1; }

/* Abre um asset no archive s3e NATIVO tentando variantes de path (o índice do
 * OBB usa basename lowercase + '\\'; o jogo pede com prefixo data-gles1/, '/' e
 * case-misto). Retorna handle nativo ou NULL. */
static void *archive_native_open(const char *fn) {
  if (!real_s3eFileOpen || !fn)
    return NULL;
  const char *base = fn;
  if (!strncmp(fn, "data-gles1/", 11)) base = fn + 11;
  else if (!strncmp(fn, "data-gles2/", 11)) base = fn + 11;
  char lo[1024], bs[1024];
  snprintf(lo, sizeof(lo), "%s", base);
  for (char *c = lo; *c; c++) *c = (char)tolower((unsigned char)*c);
  snprintf(bs, sizeof(bs), "%s", lo);
  for (char *c = bs; *c; c++) if (*c == '/') *c = '\\';
  const char *bn = strrchr(lo, '/'); bn = bn ? bn + 1 : lo;
  /* NÃO tenta o fn cru com prefixo data-gles1/ (o s3eFileOpen nativo entra em
   * loop de resolução p/ esse path). Usa só as formas do índice do archive. */
  const char *forms[5] = { bs, lo, bn, base, NULL };
  static int dbg = 0;
  int logd = dbg < 8;
  for (int i = 0; forms[i]; i++) {
    if (logd) { debugPrintf("NATIVE arch try '%s'...\n", forms[i]); }
    void *h = real_s3eFileOpen(forms[i], "rb");
    if (logd) debugPrintf("NATIVE arch try '%s' -> %p\n", forms[i], h);
    if (h) { if (logd) dbg++; return h; }
  }
  if (logd) dbg++;
  return NULL;
}
static FILE *ep1_extract_from_archive(const char *fn);
/* chamável do fsm_hook (jni_shim, thread MAIN) p/ extrair na thread certa */
int pes_try_extract(const char *fn) {
  FILE *f = ep1_extract_from_archive(fn);
  if (f) { fclose(f); return 1; }
  return 0;
}

/* MARSHALING: a Open callback do archive crasha na thread de LOADING (estado s3e)
 * mas roda OK na MAIN. A thread de loading enfileira e espera; a MAIN
 * (pes_marshal_drain, no glSwapBuffers) extrai. Na MAIN, extrai direto. */
static volatile int g_marshal_req = 0, g_marshal_done = 0;
static char g_marshal_path[512];
static pthread_mutex_t g_marshal_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile uintptr_t g_main_tid = 0;
void pes_set_main_tid(void) { if (!g_main_tid) g_main_tid = (uintptr_t)pthread_self(); }
/* BULK EXTRACT: na thread MAIN (no swap), extrai uma lista explícita de grupos
 * via a cifra do archive, logando bytes por arquivo. Isola se a cifra funciona.
 * PES_BULK_EXTRACT="a.group.bin,b.group.bin,..." (roda 1x quando rdy=1). */
static void pes_bulk_extract_once(void) {
  const char *env = getenv("PES_BULK_EXTRACT");
  if (!env || !*env || !g_archive_ready || !g_cbs_ok)
    return;
  static int done = 0;
  if (done) return;
  done = 1;
  debugPrintf("BULK: iniciando extração g_archive_ready=%d g_cbs_ok=%d\n",
              g_archive_ready, g_cbs_ok);
  char *copy = strdup(env);
  if (!copy) return;
  for (char *tok = strtok(copy, ",;\n"); tok; tok = strtok(NULL, ",;\n")) {
    while (*tok == ' ' || *tok == '\t') tok++;
    if (!*tok) continue;
    FILE *f = ep1_extract_from_archive(tok);
    if (f) fclose(f);
    debugPrintf("BULK: done '%s'\n", tok);
  }
  free(copy);
}
void pes_marshal_drain(void) {
  pes_bulk_extract_once();
  if (!g_marshal_req) return;
  FILE *f = ep1_extract_from_archive(g_marshal_path);
  if (f) fclose(f);
  g_marshal_done = 1;
  __sync_synchronize();
  g_marshal_req = 0;
}
static int pes_marshal_extract(const char *fn) {
  static int dbg = 0;
  int ismain = g_main_tid && (uintptr_t)pthread_self() == g_main_tid;
  if (dbg < 8) {
    debugPrintf("MARSHAL: fn=%s main_tid=%lx self=%lx ismain=%d\n", fn,
                (unsigned long)g_main_tid, (unsigned long)pthread_self(), ismain);
    dbg++;
  }
  if (ismain) {
    FILE *f = ep1_extract_from_archive(fn);
    if (f) { fclose(f); return 1; }
    return 0;
  }
  pthread_mutex_lock(&g_marshal_lock);
  snprintf(g_marshal_path, sizeof(g_marshal_path), "%s", fn);
  g_marshal_done = 0;
  __sync_synchronize();
  g_marshal_req = 1;
  for (int i = 0; i < 8000 && !g_marshal_done; i++) usleep(1000); /* até 8s */
  int ok = g_marshal_done;
  pthread_mutex_unlock(&g_marshal_lock);
  return ok;
}

/* callbacks do user-fs do archive (capturadas no hook de s3eFileAddUserFileSys).
 * Open(filename,mode)=cbs[0]; Read(buf,esz,count,handle)=cbs[1]. A Read
 * DESCRIPTOGRAFA os dados do OBB on-the-fly. */
extern void *g_ufs_cbs;
typedef void *(*ufs_open_fn)(const char *, const char *);
typedef int (*ufs_read_fn)(void *, unsigned int, unsigned int, void *);

static FILE *ep1_extract_from_archive(const char *fn) {
  if (getenv("PES_NO_ARCHIVE_EXTRACT") || !fn || !g_archive_ready || !g_cbs_ok)
    return NULL;
  /* só assets .group.bin (carregados após o mount); nunca .s3e/config/early. */
  size_t n = strlen(fn);
  if (n < 10 || strcmp(fn + n - 10, ".group.bin") != 0)
    return NULL;
  static __thread int busy = 0;
  if (busy)
    return NULL;
  busy = 1;
  debugPrintf("ARCHIVE: ep1_extract entrou fn=%s cbs0=%p\n", fn,
              (void *)g_cbs_copy[0]);
  ufs_open_fn Open = (ufs_open_fn)g_cbs_copy[0];
  ufs_read_fn Read = (ufs_read_fn)g_cbs_copy[1];
  static int dbg = 0;
  int logd = dbg < 40;
  FILE *ret = NULL;
  /* o índice do OBB usa basename lowercase + path com '\\'. O jogo pede com '/'
   * e case-misto. Tenta variantes até a Open callback achar. */
  const char *base = fn;
  if (!strncmp(fn, "data-gles1/", 11)) base = fn + 11;
  char v[1024];
  snprintf(v, sizeof(v), "%s", base);
  for (char *c = v; *c; c++) *c = (char)tolower((unsigned char)*c);
  char vbs[1024];
  snprintf(vbs, sizeof(vbs), "%s", v);
  for (char *c = vbs; *c; c++) if (*c == '/') *c = '\\';
  const char *bn = strrchr(v, '/'); bn = bn ? bn + 1 : v; /* basename lowercase */
  /* o índice do OBB indexa por BASENAME lowercase ("soundmenu.group.bin"); as
   * formas full-path (backslash/barra) fazem o sub_open crashar. Basename 1º. */
  const char *forms[6] = { bn, v, vbs, base, NULL };
  void *h = NULL;
  for (int i = 0; forms[i] && !h; i++) {
    h = Open(forms[i], "rb");
    if (logd) debugPrintf("ARCHIVE: ufsOpen('%s') -> %p\n", forms[i], h);
  }
  if (logd) dbg++;
  if (h) {
    const char *s = base;
    while (*s == '/') s++;
    char dirs[1024];
    snprintf(dirs, sizeof(dirs), "%s", s);
    for (char *c = dirs; *c; c++)
      if (*c == '/' || *c == '\\') { *c = 0; if (dirs[0]) mkdir(dirs, 0755); *c = '/'; }
    FILE *w = fopen(dirs, "wb");
    unsigned long total = 0;
    if (w) {
      static char chunk[65536];
      for (;;) {
        int got = Read(chunk, 1, sizeof(chunk), h); /* DESCRIPTOGRAFA */
        if (got <= 0) break;
        fwrite(chunk, 1, (size_t)got, w);
        total += (unsigned long)got;
        if ((unsigned)got < sizeof(chunk)) break;
        if (total > 96u * 1024u * 1024u) break;
      }
      fclose(w);
    }
    if (g_cbs_copy[3])
      ((arch_close_fn)g_cbs_copy[3])(h);
    debugPrintf("ARCHIVE: extraído '%s' -> '%s' bytes=%lu\n", fn, dirs, total);
    if (total > 0) ret = fopen(dirs, "rb");
  }
  busy = 0;
  return ret;
}

static void maybe_extract_obb_groups_once(void) {
  const char *env = getenv("PES_EXTRACT_OBB_GROUPS");
  if (!env || !*env)
    return;
  static int done;
  if (done)
    return;
  done = 1;

  static const char *defaults[] = {
      "data-gles1/menu/hd/menuAssetLoader.group.bin",
      "data-gles1/menu/hd/global.group.bin",
      "data-gles1/menu/hd/globalPES.group.bin",
      "data-gles1/menu/hd/menuLogo.group.bin",
      "data-gles1/sound/menu/soundMenu.group.bin",
      NULL,
  };

  debugPrintf("ARCHIVE: PES_EXTRACT_OBB_GROUPS='%s'\n", env);
  if (!strcmp(env, "1")) {
    for (int i = 0; defaults[i]; i++) {
      FILE *f = ep1_extract_from_archive(defaults[i]);
      if (f)
        fclose(f);
    }
    return;
  }

  char *copy = strdup(env);
  if (!copy)
    return;
  for (char *tok = strtok(copy, ",;\n"); tok; tok = strtok(NULL, ",;\n")) {
    while (*tok == ' ' || *tok == '\t')
      tok++;
    if (!*tok)
      continue;
    FILE *f = ep1_extract_from_archive(tok);
    if (f)
      fclose(f);
  }
  free(copy);
}

static __thread int g_open_depth = 0;
static __thread void *g_open_lr = 0;
static void *my_s3eFileOpen_impl(const char *fn, const char *mode);
static void *my_s3eFileOpen(const char *fn, const char *mode) {
  void *lr; __asm__ volatile("mov %0, lr" : "=r"(lr));
  g_open_lr = lr;
  if (g_open_depth > 8) { /* recursão sem fim (safety net) */
    static int d = 0;
    if (d < 8) { debugPrintf("OPEN RECURSION depth=%d fn=%s\n", g_open_depth, fn ? fn : "(null)"); d++; }
    return NULL;
  }
  g_open_depth++;
  void *r = my_s3eFileOpen_impl(fn, mode);
  g_open_depth--;
  return r;
}
static void *my_s3eFileOpen_impl(const char *fn, const char *mode) {
  int rd = !mode || !(strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'));
  int native_ok = fn && real_s3eFileOpen && getenv("PES_NATIVE_ARCHIVE");
  if (rd && pes_should_skip_group_path(fn)) {
    static int d = 0;
    if (d < 24) {
      debugPrintf("GRPSKIP: s3eFileOpen '%s' mode='%s' -> NULL\n", fn,
                  mode ? mode : "?");
      d++;
    }
    return NULL;
  }
  /* RE-ENTRY (depth>=2): o worker (chamado por nós no depth 1) re-chamou o
   * s3eFileOpen na sua resolução interna. NÃO passar pro real (recursaria); só
   * tenta o DISCO e retorna (miss=NULL) -> o worker continua p/ o dispatch
   * user-fs (archive) e acha o asset lá. */
  if (native_ok && g_open_depth >= 2) {
    if (getenv("PES_OPENLOG")) {
      static int d = 0;
      if (d < 60) { debugPrintf("OPENLOG reentry depth=%d fn='%s' lr=%p\n", g_open_depth, fn ? fn : "(null)", g_open_lr); d++; }
    }
    /* só DISCO -> miss=NULL. O worker (que nos chamou) então faz o próprio
     * dispatch user-fs (blx r7) com o nome LOWERCASE que passamos no depth 1,
     * casando o basename no índice. NÃO chamar cbs[0] aqui (crasha out-of-ctx). */
    FILE *df = ep1_file_real_open(fn, mode);
    ep1_track_open(df);
    return (void *)df;
  }
  if (getenv("PES_OPENLOG")) {
    void *lr = __builtin_return_address(1);
    static int d = 0;
    if (d < 60) { debugPrintf("OPENLOG depth=%d fn='%s' lr=%p\n", g_open_depth, fn ? fn : "(null)", lr); d++; }
  }
  /* DUAL-MODE: package.dz (backing do archive) SEMPRE pelo VFS s3e NATIVO, p/ o
   * archive_open ler+DESCRIPTOGRAFAR os grupos. Abre a forma que retorna um
   * handle LEGÍVEL (magic 79 9c a8 0a); reusa o mesmo p/ todos os opens. */
  if (native_ok && getenv("PES_PKG_NATIVE") && strstr(fn, "package.dz")) {
    /* abre SEMPRE a forma raw:// absoluta (handle válido); a forma relativa
     * "package.dz" dá handle ruim no VFS nativo. Handle independente por open. */
    char rawp[640];
    const char *home = getenv("HOME");
    snprintf(rawp, sizeof(rawp), "raw://%s/package.dz", home ? home : ".");
    const char *openpath = rawp;
    void *h = real_s3eFileOpen(openpath, mode ? mode : "rb");
    if (!h && g_pkg_native_handle) {
      h = g_pkg_native_handle;
      if (real_s3eFileSeek)
        real_s3eFileSeek(h, 0, 0);
      debugPrintf("NATIVE pkg('%s'->'%s') reusa handle %p\n", fn, openpath, h);
    }
    /* valida o magic 79 9c a8 0a */
    if (h) {
      unsigned char b[4] = {0};
      if (real_s3eFileSeek) real_s3eFileSeek(h, 0, 0);
      int g = real_s3eFileRead ? real_s3eFileRead(b, 1, 4, h) : 0;
      if (real_s3eFileSeek) real_s3eFileSeek(h, 0, 0);
      if (!(g == 4 && b[0] == 0x79 && b[1] == 0x9c && b[2] == 0xa8 && b[3] == 0x0a)) {
        debugPrintf("NATIVE pkg('%s'->'%s') %p INVALIDO magic=%02x%02x%02x%02x\n",
                    fn, openpath, h, b[0], b[1], b[2], b[3]);
        if (real_s3eFileClose) real_s3eFileClose(h);
        h = NULL;
      }
    } else {
      static int pkg_null_logs = 0;
      if (pkg_null_logs < 20) {
        debugPrintf("NATIVE pkg('%s'->'%s') NULL mode='%s'\n", fn, openpath,
                    mode ? mode : "?");
        pkg_null_logs++;
      }
      void *pf = open_package_dz_disk_fallback(fn, mode ? mode : "rb");
      if (pf)
        return pf;
    }
    if (h) {
      add_native_handle(h);
      g_pkg_native_handle = h;
      g_archive_ready = 1;
      debugPrintf("NATIVE open pkg('%s'->'%s') -> %p (validado)\n", fn, openpath, h);
      /* teste: o handle nativo é legível? (magic esperado 79 9c a8 0a) */
      if (getenv("PES_TEST_PKGREAD") && real_s3eFileRead) {
        unsigned char b[16] = {0};
        int g = real_s3eFileRead(b, 1, 16, h);
        debugPrintf("PKGREAD: got=%d bytes=%02x%02x%02x%02x sz=%u\n", g,
                    b[0], b[1], b[2], b[3],
                    real_s3eFileGetSize ? real_s3eFileGetSize(h) : 0);
        /* teste SEEK (acesso aleatório, o que o archive_open faz) */
        debugPrintf("PKGSEEK: real_s3eFileSeek=%p tell=%p ...\n",
                    (void *)real_s3eFileSeek, (void *)real_s3eFileTell);
        if (real_s3eFileSeek) {
          int sr = real_s3eFileSeek(h, 0x800, 0); /* SEEK_SET p/ o dir 0x800 */
          unsigned char c[8] = {0};
          int g2 = real_s3eFileRead(c, 1, 8, h);
          int tl = real_s3eFileTell ? real_s3eFileTell(h) : -1;
          debugPrintf("PKGSEEK: seek=%d tell=%d got=%d bytes=%02x%02x%02x%02x%02x\n",
                      sr, tl, g2, c[0], c[1], c[2], c[3], c[4]);
        }
      }
      return h;
    }
  }
  FILE *f = ep1_file_real_open(fn, mode);
  if (f && rd && g_archive_ready && fn &&
      (getenv("PES_PREFER_ARCH_GROUPS") ||
       (native_ok && pes_group_should_prefer_archive(fn)))) {
    size_t n = strlen(fn);
    if (n >= 10 && !strcmp(fn + n - 10, ".group.bin")) {
      fclose(f);
      f = NULL;
    }
  }
  /* asset .group.bin que NÃO está no disco: o índice do OBB indexa por BASENAME
   * lowercase ("soundmenu.group.bin"); o jogo pede o full-path -> o archive não
   * casa -> "Cannot open". Reescreve p/ basename lowercase e abre pelo VFS s3e
   * NATIVO (o archive_open casa no índice e a Read descriptografa on-the-fly). */
  /* só no nível MAIS EXTERNO (depth==1). RAIZ do "Cannot open": o índice do OBB
   * é BASENAME LOWERCASE ("soundmenu.group.bin") mas o jogo pede camelCase
   * ("soundMenu.group.bin") -> o sub_open não casa -> NULL -> e o worker fica
   * re-tentando (recursão infinita). Fix: LOWERCASE o path completo (preserva a
   * estrutura que o worker espera) e chama o s3eFileOpen ORIGINAL -> sub_open
   * casa o basename lowercase -> acha -> recursão termina. */
  if (!f && native_ok && rd && g_archive_ready && g_open_depth == 1) {
    size_t n = strlen(fn);
    if (n >= 10 && !strcmp(fn + n - 10, ".group.bin")) {
      /* "data-gles1/" pode ser o MOUNT do OBB (user-fs). Passa o path COMPLETO
       * ORIGINAL ao VFS interno (roteia o mount->user-fs); o sub_open extrai o
       * basename. Tenta variantes (original, lowercase, basename lowercase). */
      const char *slash = strrchr(fn, '/');
      const char *bslash = strrchr(fn, '\\');
      if (bslash && (!slash || bslash > slash)) slash = bslash;
      const char *base = slash ? slash + 1 : fn;
      char lc[512], bn[256]; size_t j = 0;
      for (const char *c = fn; *c && j < sizeof(lc) - 1; c++)
        lc[j++] = (char)tolower((unsigned char)*c);
      lc[j] = 0;
      j = 0;
      for (const char *c = base; *c && j < sizeof(bn) - 1; c++)
        bn[j++] = (char)tolower((unsigned char)*c);
      bn[j] = 0;
      const char *forms[4] = { fn, lc, bn, NULL };
      void *h = NULL; const char *used = fn;
      for (int i = 0; forms[i] && !h; i++) {
        if (g_worker_open) h = g_worker_open(forms[i], "rb", 0);
        used = forms[i];
      }
      static int d = 0;
      if (d < 40) { debugPrintf("GRP worker fn='%s' used='%s' -> %p\n", fn, used, h); d++; }
      if (h) { add_native_handle(h); return h; }
    }
  }
  ep1_track_open(f);
  return (void *)f;
}
static void *my_s3eFileOpenFromMemory(void *buf, unsigned int size) {
  FILE *f = fmemopen(buf, size, "rb");
  ep1_track_open(f);
  if (getenv("SONIC4EP1_FILELOG"))
    debugPrintf("s3eFile: openFromMemory(%p,%u) -> %p\n", buf, size, (void *)f);
  return (void *)f;
}
static int my_s3eFileClose(void *f) {
  if (is_pkgdisk_handle(f)) {
    struct pkg_disk_handle *p = (struct pkg_disk_handle *)f;
    static int d = 0;
    if (d < 12) {
      debugPrintf("PKGIO Close h=%p fd=%d base=0x%lx path='%s'\n",
                  f, p->fd, (long)p->base, p->path);
      d++;
    }
    del_pkgdisk_handle(p);
    if (p->fd >= 0)
      close(p->fd);
    p->magic = 0;
    free(p);
    return 0;
  }
  if (is_arch_handle(f)) {
    del_arch_handle(f);
    return g_cbs_ok ? ((arch_close_fn)g_cbs_copy[3])(f) : 0;
  }
  if (is_native_handle(f)) { del_native_handle(f); return real_s3eFileClose ? real_s3eFileClose(f) : 0; }
  if (f && ep1_track_close((FILE *)f))
    fclose((FILE *)f); /* so fecha se ainda estava aberto (ignora double-close) */
  return 0;            /* S3E_RESULT_SUCCESS */
}
static int my_s3eFileRead(void *buf, unsigned int esz, unsigned int n,
                          void *f) {
  if (!f || esz == 0)
    return 0;
  if (getenv("PES_PKGIO_TRACE")) {
    static int e = 0;
    int force = (esz == 9 && n == 1);
    if (force || e < 40) {
      uint32_t magic = 0;
      if (ptr_range_mapped(f, sizeof(magic)))
        magic = *(uint32_t *)f;
      debugPrintf("READ_ENTRY buf=%p esz=%u n=%u f=%p magic=%08x pkg=%d arch=%d nat=%d\n",
                  buf, esz, n, f, magic, is_pkgdisk_handle(f),
                  is_arch_handle(f), is_native_handle(f));
      e++;
    }
  }
  if (is_pkgdisk_handle(f)) {
    struct pkg_disk_handle *p = (struct pkg_disk_handle *)f;
    size_t want = (size_t)esz * (size_t)n;
    ssize_t got = read(p->fd, buf, want);
    static int d = 0;
    if (d < 40) {
      off_t pos = lseek(p->fd, 0, SEEK_CUR);
      debugPrintf("PKGIO Read h=%p fd=%d esz=%u n=%u want=%zu -> %ld pos=%ld raw=%ld\n",
                  f, p->fd, esz, n, want, (long)got,
                  (long)(pos - p->base), (long)pos);
      d++;
    }
    return got > 0 ? (int)((size_t)got / (size_t)esz) : 0;
  }
  if (is_arch_handle(f)) {
    int got = g_cbs_ok ? ((arch_read_fn)g_cbs_copy[1])(buf, esz, n, f) : 0; /* DESCRIPTOGRAFA */
    if (got == 0 && g_cbs_ok && getenv("PES_ARCH_SEEK0_RETRY")) {
      /* handle pode vir posicionado no EOF/estado sujo -> seek(0)+retry */
      if (g_cbs_copy[4]) ((arch_seek_fn)g_cbs_copy[4])(f, 0, 0 /*SEEK_SET*/);
      int got2 = ((arch_read_fn)g_cbs_copy[1])(buf, esz, n, f);
      static int dr = 0;
      if (dr < 12) { debugPrintf("ARCH Read RETRY-after-seek0 h=%p esz=%u n=%u -> %d (era 0)\n", f, esz, n, got2); dr++; }
      got = got2;
    }
    static int d = 0;
    if (d < 12) {
      unsigned hw[8]; if (ptr_range_mapped(f, 32)) memcpy(hw, f, 32);
      debugPrintf("ARCH Read h=%p esz=%u n=%u -> %d [hdr %08x %08x off=%08x entry=%08x buf=%08x sz=%08x]\n",
                  f, esz, n, got, hw[0], hw[1], hw[1], hw[3], hw[4], hw[6]); d++; }
    return got;
  }
  if (is_native_handle(f)) {
    static int d=0; if(d<12){debugPrintf("NATIVE Read h=%p esz=%u n=%u\n",f,esz,n);d++;}
    return real_s3eFileRead ? real_s3eFileRead(buf, esz, n, f) : 0;
  }
  int got = (int)fread(buf, esz, n, (FILE *)f);
  if (getenv("SONIC4EP1_FILELOG")) {
    static int rc = 0;
    if (rc++ < 20)
      debugPrintf("s3eFile: READ f=%p esz=%u n=%u -> %d\n", f, esz, n, got);
  }
  return got;
}
static int my_s3eFileGetChar(void *f) {
  unsigned char c = 0;
  int got = my_s3eFileRead(&c, 1, 1, f);
  static int d = 0;
  if (getenv("PES_PKGIO_TRACE") && d < 80 &&
      (is_pkgdisk_handle(f) || is_arch_handle(f) || is_native_handle(f))) {
    debugPrintf("PKGIO GetChar h=%p -> got=%d c=%02x\n", f, got, c);
    d++;
  }
  return got == 1 ? (int)c : 0;
}
static int my_s3eFileWrite(const void *buf, unsigned int esz, unsigned int n,
                           void *f) {
  if (!f || esz == 0)
    return 0;
  return (int)fwrite(buf, esz, n, (FILE *)f);
}
static int my_s3eFileSeek(void *f, int off, int origin) {
  if (!f)
    return 1; /* error */
  if (is_pkgdisk_handle(f)) {
    struct pkg_disk_handle *p = (struct pkg_disk_handle *)f;
    off_t target;
    if (origin == 1) {
      off_t cur = lseek(p->fd, 0, SEEK_CUR);
      target = cur + (off_t)off;
    } else if (origin == 2) {
      target = p->base + p->size + (off_t)off;
    } else {
      target = p->base + (off_t)off;
    }
    off_t r = lseek(p->fd, target, SEEK_SET);
    static int d = 0;
    if (d < 40) {
      debugPrintf("PKGIO Seek h=%p fd=%d off=%d org=%d -> pos=%ld raw=%ld\n",
                  f, p->fd, off, origin, (long)(r - p->base), (long)r);
      d++;
    }
    return r < 0 ? 1 : 0;
  }
  if (is_arch_handle(f)) {
    return g_cbs_ok ? ((arch_seek_fn)g_cbs_copy[4])(f, off, origin) : 1;
  }
  if (is_native_handle(f)) {
    static int d=0; if(d<12){debugPrintf("NATIVE Seek h=%p off=%d org=%d\n",f,off,origin);d++;}
    return real_s3eFileSeek ? real_s3eFileSeek(f, off, origin) : 1;
  }
  int whence = origin == 1 ? SEEK_CUR : origin == 2 ? SEEK_END : SEEK_SET;
  return fseek((FILE *)f, off, whence) == 0 ? 0 : 1;
}
static int my_s3eFileTell(void *f) {
  if (is_pkgdisk_handle(f)) {
    struct pkg_disk_handle *p = (struct pkg_disk_handle *)f;
    off_t r = lseek(p->fd, 0, SEEK_CUR);
    return r < 0 ? -1 : (int)(r - p->base);
  }
  if (is_arch_handle(f)) {
    return g_cbs_ok ? ((arch_tell_fn)g_cbs_copy[5])(f) : -1;
  }
  if (is_native_handle(f)) return real_s3eFileTell ? real_s3eFileTell(f) : -1;
  return f ? (int)ftell((FILE *)f) : -1;
}
static int my_s3eFileEOF(void *f) {
  if (!f)
    return 1;
  if (is_pkgdisk_handle(f)) {
    struct pkg_disk_handle *p = (struct pkg_disk_handle *)f;
    off_t r = lseek(p->fd, 0, SEEK_CUR);
    return (r < 0 || (r - p->base) >= p->size) ? 1 : 0;
  }
  if (is_arch_handle(f)) {
    if (!g_cbs_ok)
      return 1;
    int cur = ((arch_tell_fn)g_cbs_copy[5])(f);
    ((arch_seek_fn)g_cbs_copy[4])(f, 0, 2 /* SEEK_END */);
    int end = ((arch_tell_fn)g_cbs_copy[5])(f);
    ((arch_seek_fn)g_cbs_copy[4])(f, cur, 0 /* SEEK_SET */);
    return cur >= end ? 1 : 0;
  }
  if (is_native_handle(f)) {
    if (!real_s3eFileTell || !real_s3eFileGetSize)
      return 0;
    return real_s3eFileTell(f) >= (int)real_s3eFileGetSize(f) ? 1 : 0;
  }
  return feof((FILE *)f) ? 1 : 0;
}
static unsigned int my_s3eFileGetSize(void *f) {
  if (!f)
    return 0;
  if (is_pkgdisk_handle(f)) {
    struct pkg_disk_handle *p = (struct pkg_disk_handle *)f;
    return p->size > 0 ? (unsigned int)p->size : 0;
  }
  if (is_arch_handle(f)) { /* size = seek(end)+tell, restaura */
    if (!g_cbs_ok) return 0;
    int cur = ((arch_tell_fn)g_cbs_copy[5])(f);
    ((arch_seek_fn)g_cbs_copy[4])(f, 0, 2 /*SEEK_END*/);
    int sz = ((arch_tell_fn)g_cbs_copy[5])(f);
    ((arch_seek_fn)g_cbs_copy[4])(f, cur, 0 /*SEEK_SET*/);
    return (unsigned int)(sz < 0 ? 0 : sz);
  }
  if (is_native_handle(f)) return real_s3eFileGetSize ? real_s3eFileGetSize(f) : 0;
  long cur = ftell((FILE *)f);
  fseek((FILE *)f, 0, SEEK_END);
  long sz = ftell((FILE *)f);
  fseek((FILE *)f, cur, SEEK_SET);
  return (unsigned int)(sz < 0 ? 0 : sz);
}
static int my_s3eFileCheckExists(const char *fn) {
  if (!fn)
    return 0;
  if (pes_should_skip_group_path(fn)) {
    static int d = 0;
    if (d < 24) {
      debugPrintf("GRPSKIP: CheckExists '%s' -> 0\n", fn);
      d++;
    }
    return 0;
  }
  const char *s = fn;
  const char *colon = strstr(fn, "://");
  if (colon)
    s = colon + 3;
  while (*s == '/')
    s++;
  const char *rel = s; /* assets em claro estão SEM o prefixo data-gles1/ */
  if (!strncmp(rel, "data-gles1/", 11)) rel += 11;
  else if (!strncmp(rel, "data-gles1\\", 11)) rel += 11;
  int ok = (access(s, F_OK) == 0) || (s != fn && access(fn, F_OK) == 0) ||
           (rel != s && access(rel, F_OK) == 0);
  /* não está no disco: checa no archive s3e nativo (.group.bin do OBB). */
  size_t n = fn ? strlen(fn) : 0;
  if (!ok && n >= 10 && !strcmp(fn + n - 10, ".group.bin")) {
    static int dbg = 0;
    if (dbg < 6) {
      debugPrintf("CHKEXIST grp '%s' ok=%d ro=%p rdy=%d\n", fn, ok,
                  (void *)real_s3eFileOpen, g_archive_ready);
      dbg++;
    }
    if (g_archive_ready && getenv("PES_NATIVE_ARCHIVE")) {
      /* sound/*.group.bin só existe no OBB cifrado (não no disco). Retorna 0
       * (não existe) p/ o jogo PULAR o som (opcional?) em vez de abortar no open.
       * Os assets de menu/gráficos estão no disco -> menu renderiza mudo. */
      if (pes_should_skip_group_path(fn))
        ok = 0;
      else
        ok = 1; /* otimista p/ os demais grupos */
    }
  }
  return ok ? 1 : 0; /* S3E_TRUE/FALSE */
}
static char *my_s3eFileReadString(char *str, unsigned int maxLen, void *f) {
  if (!f || !str || maxLen == 0)
    return NULL;
  if (is_pkgdisk_handle(f) || is_arch_handle(f) || is_native_handle(f)) {
    /* Handles sintéticos/nativos não são FILE*. Ler char-a-char evita cair no
     * fgets() da libc com um ponteiro que não pertence ao stdio. */
    unsigned int i = 0;
    while (i + 1 < maxLen) {
      char c;
      if (my_s3eFileRead(&c, 1, 1, f) != 1)
        break;
      str[i++] = c;
      if (c == '\n')
        break;
    }
    if (i == 0)
      return NULL;
    str[i] = 0;
    if (getenv("PES_PKGIO_TRACE") && is_pkgdisk_handle(f)) {
      static int d = 0;
      if (d < 16) {
        debugPrintf("PKGIO ReadString h=%p max=%u -> '%s'\n", f, maxLen, str);
        d++;
      }
    }
    return str;
  }
  return fgets(str, (int)maxLen, (FILE *)f);
}
static int my_s3eFileFlush(void *f) {
  if (is_pkgdisk_handle(f) || is_arch_handle(f) || is_native_handle(f))
    return 0;
  if (f)
    fflush((FILE *)f);
  return 0;
}
static int my_s3eFileGetError(void) { return 0; }
/* Listagem de diretorio real (opendir/readdir) — o loader varre o dir pra achar
 * o executavel Sonic4epI.s3e. Handle = DIR*. */
static void *my_s3eFileListDirectory(const char *d) {
  const char *s = d ? d : ".";
  const char *colon = d ? strstr(d, "://") : NULL;
  if (colon)
    s = colon + 3;
  while (*s == '/')
    s++;
  if (!*s)
    s = ".";
  DIR *dir = opendir(s);
  if (!dir)
    dir = opendir(".");
  if (getenv("SONIC4EP1_FILELOG"))
    debugPrintf("s3eFile: listDir '%s' (real '%s') -> %p\n", d ? d : "?", s,
                (void *)dir);
  return (void *)dir;
}
static int my_s3eFileListNext(void *list, char *nameBuf, unsigned int len) {
  if (!list || !nameBuf)
    return 1;
  struct dirent *e;
  while ((e = readdir((DIR *)list)) != NULL) {
    if (e->d_name[0] == '.' &&
        (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
      continue;
    snprintf(nameBuf, len > 0 ? len : 256u, "%s", e->d_name);
    return 0;
  }
  return 1;
}
static int my_s3eFileListClose(void *list) {
  if (list)
    closedir((DIR *)list);
  return 0;
}

static void *pes_safe_memcpy(void *dst, const void *src, size_t n) {
  if (n > 0) {
    int bad = (!dst || !src || (uintptr_t)dst < 0x10000u ||
               (uintptr_t)src < 0x10000u);
    if (bad) {
      static int d = 0;
      if (d < 80) {
        debugPrintf("MEMCPY_GUARD: dst=%p src=%p n=%zu caller=%p -> skip\n",
                    dst, src, n, __builtin_return_address(0));
        d++;
      }
      return dst;
    }
  }
  return memcpy(dst, src, n);
}

static void *libc_memcpy_guard(void *dst, const void *src, size_t n) {
  if (n > 0) {
    int bad = (!dst || !src || (uintptr_t)dst < 0x10000u ||
               (uintptr_t)src < 0x10000u);
    if (bad) {
      static int d = 0;
      if (d < 120) {
        debugPrintf("LIBC_MEMCPY_GUARD: dst=%p src=%p n=%zu caller=%p -> skip\n",
                    dst, src, n, __builtin_return_address(0));
        d++;
      }
      return dst;
    }
  }

  volatile unsigned char *d = (volatile unsigned char *)dst;
  const volatile unsigned char *s = (const volatile unsigned char *)src;
  for (size_t i = 0; i < n; i++)
    d[i] = s[i];
  return dst;
}

static void install_libc_memcpy_guard(void) {
  static uintptr_t patched;
  void *p = dlsym(RTLD_NEXT, "memcpy");
  if (!p)
    p = dlsym(RTLD_DEFAULT, "memcpy");
  uintptr_t a = (uintptr_t)p;
  if (!a || a == (uintptr_t)memcpy || patched == a)
    return;
  long ps = sysconf(_SC_PAGESIZE);
  if (ps <= 0) ps = 4096;
  uintptr_t lo = (a & ~((uintptr_t)ps - 1));
  if (mprotect((void *)lo, (size_t)ps, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    debugPrintf("LIBC_MEMCPY_GUARD: mprotect(%p) falhou errno=%d\n",
                (void *)lo, errno);
    return;
  }
  hook_arm(a, (uintptr_t)libc_memcpy_guard);
  __builtin___clear_cache((char *)(a & ~1u), (char *)((a & ~1u) + 16));
  patched = a;
  debugPrintf("LIBC_MEMCPY_GUARD: hook libc memcpy @%p -> %p\n",
              (void *)a, (void *)libc_memcpy_guard);
}

static void install_memcpy_guard(void) {
  uintptr_t got = so_find_rel_addr_safe("memcpy");
  if (!got)
    return;
  uintptr_t old = *(uintptr_t *)got;
  *(uintptr_t *)got = (uintptr_t)pes_safe_memcpy;
  debugPrintf("MEMCPY_GUARD: GOT memcpy @%p %p -> %p\n", (void *)got,
              (void *)old, (void *)pes_safe_memcpy);
  if (getenv("PES_HOOK_LIBC_MEMCPY"))
    install_libc_memcpy_guard();
}

/* Resolver de path do loader (0x67708): por padrao usa internals do VFS (drive
 * table vazio no nosso ambiente) e falha pro executavel -> o loader nao carrega
 * o jogo. Substituimos por uma copia direta do nome (path = relativo ao cwd),
 * retornando sucesso; o load real vai via s3eFileOpen (nosso fopen). */
static int my_s3eFile_resolve(char *out, const char *in, int type, int size) {
  (void)type;
  if (out && in) {
    const char *s = in;
    const char *colon = strstr(in, "://");
    if (colon)
      s = colon + 3;
    while (*s == '/')
      s++;
    size_t cap = size > 0 ? (size_t)size : 256u;
    if (getenv("SONIC4EP1_RAW")) {
      /* forca o drive raw:// (FS real) com path absoluto */
      char cwd[512];
      if (getcwd(cwd, sizeof(cwd)))
        snprintf(out, cap, "raw://%s/%s", cwd, s);
      else
        snprintf(out, cap, "raw:///storage/roms/ports/sonic4ep1/%s", s);
    } else {
      snprintf(out, cap, "%s", s);
    }
  }
  if (getenv("SONIC4EP1_FILELOG"))
    debugPrintf("s3eFile: resolve '%s' -> '%s'\n", in ? in : "?",
                out ? out : "?");
  return 0;
}

/* ===== Registro do drive rom:// (MURO FINAL do VFS) =====
 * O engine abre o executavel Sonic4epI.s3e pelo VFS INTERNO (0x62d90/0x62adc),
 * que le o drive em FS_global+1008; o backend (+8) e NULL (rom drive nao
 * inicializado no so-loader) -> crash 0x62f28. Usamos a propria API do engine
 * s3eFileAddUserFileSys (0x682f8) p/ CONSTRUIR um drive real-FS valido (callbacks
 * = nossos wrappers libc) num slot de usuario, depois COPIAMOS esse drive pro
 * slot rom (+1008), que e onde o open de path-sem-scheme (Sonic4epI.s3e) resolve. */
#define EP1_FS_GLOBAL 0xc9a88u
#define EP1_DRV_STRIDE 284u
#define EP1_ROM_SLOT 1008u
static int g_romdrive_done;

/* func ptr do descritor rom-fs (desc+52). Contrato desconhecido — loga args
 * pra revelar como/quando o engine chama. ARM aapcs: args extras sao ignorados. */
static int g_drvfn_calls;
static int my_drive_func(void *r0, void *r1, void *r2, void *r3) {
  if (g_drvfn_calls < 8) {
    debugPrintf("romdrive: DRIVE_FUNC#%d r0=%p r1=%p r2=%p r3=%p\n",
                g_drvfn_calls, r0, r1, r2, r3);
    /* tenta interpretar args como strings + dump de r0 */
    char *s1 = (char *)r1;
    debugPrintf("  r1 as str: '%.40s'\n", s1 ? s1 : "(null)");
    if (r0) {
      unsigned int *w = (unsigned int *)r0;
      debugPrintf("  r0[0..6]: %08x %08x %08x %08x %08x %08x\n", w[0], w[1],
                  w[2], w[3], w[4], w[5]);
      debugPrintf("  r0 as str: '%.40s'\n", (char *)r0);
    }
  }
  g_drvfn_calls++;
  return 1;
}

static void ep1_register_rom_drive(void) {
  uintptr_t base = g_main_text_base;
  if (!base)
    return;
  unsigned char *FS = (unsigned char *)(base + EP1_FS_GLOBAL);
  static uintptr_t cb[24];
  cb[0] = (uintptr_t)my_s3eFileOpen;         /* 0  Open(path,mode) */
  cb[1] = (uintptr_t)my_s3eFileClose;        /* 4  Close(fp) */
  cb[2] = (uintptr_t)my_s3eFileRead;         /* 8  Read */
  cb[3] = (uintptr_t)my_s3eFileWrite;        /* 12 Write */
  cb[4] = (uintptr_t)my_s3eFileSeek;         /* 16 Seek */
  cb[5] = (uintptr_t)my_s3eFileTell;         /* 20 Tell */
  cb[6] = (uintptr_t)my_s3eFileGetSize;      /* 24 */
  cb[7] = (uintptr_t)my_s3eFileCheckExists;  /* 28 */
  cb[8] = (uintptr_t)my_s3eFileFlush;        /* 32 */
  cb[9] = (uintptr_t)my_s3eFileGetError;     /* 36 */
  cb[10] = (uintptr_t)my_s3eFileReadString;  /* 40 */
  /* HIBRIDO: deixa o engine CONSTRUIR um drive valido (com callbacks inline) via
   * s3eFileAddUserFileSys num slot de usuario (11-14), depois copia o slot INTEIRO
   * (284B) pro slot 0 (+156) — onde o .s3e (path sem scheme) resolve. Adiciona um
   * desc minimo em slot0[+8] pra resolve de 0x62adc nao crashar (le slot[8][4]). */
  unsigned user_off[4] = {3280u, 3564u, 3848u, 4132u};
  unsigned char before[4];
  for (int i = 0; i < 4; i++)
    before[i] = FS[user_off[i]];
  int (*addfs)(void *) = (int (*)(void *))(base + 0x682f8);
  int r = addfs(cb);
  int us = -1;
  for (int i = 0; i < 4; i++)
    if (FS[user_off[i]] && !before[i]) {
      us = (int)user_off[i];
      break;
    }
  if (us >= 0 && getenv("SONIC4EP1_DUMPSLOT")) {
    unsigned int *w = (unsigned int *)(FS + us);
    for (int i = 0; i < 71; i += 4)
      debugPrintf("slot[+%d]=%08x %08x %08x %08x\n", i * 4, w[i], w[i + 1],
                  w[i + 2], w[i + 3]);
    /* dump do descritor que o slot[+8] aponta (vtable + scheme do drive) */
    unsigned char *dp = *(unsigned char **)(FS + us + 8);
    if (dp) {
      unsigned int *dw = (unsigned int *)dp;
      for (int i = 0; i < 32; i += 4)
        debugPrintf("desc[+%d]=%08x %08x %08x %08x\n", i * 4, dw[i], dw[i + 1],
                    dw[i + 2], dw[i + 3]);
      debugPrintf("desc bytes[0..32]: ");
      for (int i = 0; i < 32; i++)
        debugPrintf("%02x ", dp[i]);
      debugPrintf("\n");
    }
  }
  static unsigned char desc[1024];
  desc[4] = 0;
  *(uintptr_t *)(desc + 52) = (uintptr_t)my_drive_func;
  (void)desc;
  if (us >= 0) {
    /* O descritor que slot[+8] aponta esta ZERADO (so-loader pulou o init da
     * vtable do user-FS). O match em 0x62360 le desc[+52]/desc[+64] (func de
     * match) — NULL => drive ignorado. Preenchemos a vtable do descritor: match
     * func retorna 1, e o open usa os callbacks INLINE do slot (+28 = my_s3eFileOpen). */
    unsigned char *dp = *(unsigned char **)(FS + us + 8);
    if (dp) {
      *(uintptr_t *)(dp + 52) = (uintptr_t)my_drive_func;
      *(uintptr_t *)(dp + 64) = (uintptr_t)my_drive_func;
    }
    /* copia o drive INTEIRO (incl slot[+8]=descritor preenchido) pros built-in 0-4 */
    unsigned slot_off[5] = {156u, 440u, 724u, 1008u, 1292u};
    for (int i = 0; i < 5; i++)
      memcpy(FS + slot_off[i], FS + us, EP1_DRV_STRIDE);
  }
  /* O ponteiro de OPEN do file-manager ([global+540], global=base+0xc8904) fica
   * NULL no so-loader: a init 0x145f4 preenche a vtable mas 0x6ce74 zera +504..+544.
   * O open do .s3e (base+0x390xx) faz `if([mgr+540]==0) "Can't open"`. Restauramos
   * o ponteiro com a func real da tabela (r3=base+0xc1ed4, entry +0xa94 = +0x43e9c). */
  if (!getenv("SONIC4EP1_NO_FMOPEN")) {
    uintptr_t *openptr = (uintptr_t *)(base + 0xc8b20);
    debugPrintf("romdrive: [mgr+540] era %p, setando base+0x43e9c\n",
                (void *)*openptr);
    *openptr = base + 0x43e9c;
  }
  /* V (base+0xc7b38) = tabela de funcoes EDK do driver de FS (zerada — registro
   * nao roda). O thunk de open le [V+24]. Preenchemos [V+24] com nossa func pra
   * revelar o contrato do open EDK e seguir o fluxo. */
  if (!getenv("SONIC4EP1_NO_VFILL")) {
    uintptr_t *vopen = (uintptr_t *)(base + 0xc7b38 + 24);
    debugPrintf("romdrive: [V+24] era %p, setando my_drive_func\n", (void *)*vopen);
    *vopen = (uintptr_t)my_drive_func;
  }
  debugPrintf("romdrive: addfs=%d userslot=%d slot[+8]=%p -> copiado p/ slots 0-4\n",
              r, us, us >= 0 ? *(void **)(FS + us + 8) : (void *)0);
}

/* Chamado pelo jni_shim no 1o doDraw (config pronta, File subsystem init'd,
 * ANTES do load do executavel .s3e). Registra o rom drive UMA vez. */
void ep1_rom_drive_register_once(void) {
  if (g_romdrive_done)
    return;
  if (!getenv("SONIC4EP1_ROMDRIVE") && !getenv("SONIC4EP1_SURFOBJ"))
    return;
  g_romdrive_done = 1;
  if (getenv("SONIC4EP1_ROMDRIVE"))
    ep1_register_rom_drive();
  /* surface object lido via TLS (0x82d60 = pthread_getspecific(key-1), key em
   * base+0xc9a74) — NULL no so-loader (sem render thread) -> crash 0x5db04.
   * Setamos o TLS na main thread (onde o .s3e carrega) apontando p/ o surface
   * object global (base+0xcf784) ou um buffer zerado. */
  if (getenv("SONIC4EP1_SURFOBJ")) {
    static unsigned char surfobj[2048];
    uintptr_t base = g_main_text_base;
    if (base) {
      unsigned *keyp = (unsigned *)(base + 0xc9a74);
      void *obj = surfobj; /* base+0xcf784 vinha lixo (0x4); usa buffer zerado */
      if (*keyp == 0) {
        pthread_key_t nk;
        pthread_key_create(&nk, NULL);
        *keyp = (unsigned)nk + 1;
      }
      pthread_setspecific((pthread_key_t)(*keyp - 1), obj);
      debugPrintf("surfobj: TLS key@0xc9a74=%u obj=%p set\n", *keyp, obj);
    }
  }
}

/* Open INTERNO do loader do .s3e (0x62d90, retorna o handle que DecompInit usa).
 * Walk de drives interno falha (subsistema EDK não-init). Redirecionamos pro nosso
 * fopen (case-insensitive). Assinatura (path, mode, flags). */
static void *my_s3eFile_internal_open(const char *path, const char *mode,
                                      int flags) {
  (void)flags;
  /* o load do .s3e + acesso ao surface acontecem NESTA thread; setamos o surface
   * TLS aqui pra garantir a thread certa (a key/obj é per-thread). */
  if (getenv("SONIC4EP1_SURFOBJ")) {
    static unsigned char so[2048];
    uintptr_t b = g_main_text_base;
    if (b) {
      unsigned key = *(unsigned *)(b + 0xc9a74);
      if (key)
        pthread_setspecific((pthread_key_t)(key - 1), so);
    }
  }
  FILE *f = ep1_file_real_open(path, (mode && *mode) ? mode : "rb");
  if (getenv("SONIC4EP1_FILELOG"))
    debugPrintf("s3eFile: INTERNAL(0x62d90) open '%s' mode '%s' -> %p\n",
                path ? path : "?", mode ? mode : "?", (void *)f);
  return f;
}

/* TLS getter 0x82d60 = pthread_getspecific(r0-1). O surface object (key em
 * base+0xc9a74) é NULL na thread do acesso. Hookamos o getter pra devolver um
 * surface object ZERADO quando a key for a do surface — independente de thread. */
static unsigned char g_ep1_surfobj[4096];
static unsigned char g_ep1_scratch[4096];
static void *my_tls_getter(unsigned kp1) {
  /* alguns campos do surface object sao ponteiros aninhados (ex.: +8); aponta
   * pro scratch zerado pra deref/escrita nao bater em NULL. */
  if (!*(void **)(g_ep1_surfobj + 8))
    *(void **)(g_ep1_surfobj + 8) = g_ep1_scratch;
  /* key 0 (não-inicializada) ou TLS NULL -> buffer ZERADO não-NULL; os paths
   * "if(obj)...else default" do engine pegam o default em vez de deref NULL. */
  if (kp1 == 0)
    return g_ep1_surfobj;
  void *v = pthread_getspecific((pthread_key_t)(kp1 - 1));
  if (!v)
    return g_ep1_surfobj;
  return v;
}

/* ---- cpuinfo gate 0x3e970: device-register (0x58cdc) faz `bne early-return`
 * se cpuinfo!=0, pulando TODA a registracao de subsistemas (incl. surface
 * 0x7f928). Logamos o retorno real; se SONIC4EP1_CPUINFO0, forcamos 0 (sucesso/
 * device reconhecido) p/ a registracao rodar natural. ---- */
static unsigned (*real_cpuinfo_3e970)(void);
static int g_cpuinfo_logs;
static unsigned diag_cpuinfo_3e970(void) {
  unsigned r = real_cpuinfo_3e970 ? real_cpuinfo_3e970() : 0;
  if (g_cpuinfo_logs < 6) {
    debugPrintf("CPUINFO: 0x3e970 real_ret=%u%s\n", r,
                getenv("SONIC4EP1_CPUINFO0") ? " -> FORCADO 0" : "");
    g_cpuinfo_logs++;
  }
  if (getenv("SONIC4EP1_CPUINFO0"))
    return 0;
  return r;
}

/* ---- subsistemas que FALHAM ao registrar no device-register (0x58cdc) fazem ele
 * dar bail (return 1) -> bootstrap A != 0 -> s3eMain NAO carrega o modulo do jogo.
 * O File subsystem (0x5f02c) falha no so-loader (sem backend real), mas File JA
 * funciona via nossos hooks (0x62d90/0x63438...). Forcamos o register a retornar 0
 * (sucesso) p/ o dispatch CONTINUAR e registrar Fibre/ThreadCore/Surface (que criam
 * as TLS keys do surface). Idem outros registers que falhem. ---- */
/* NAO chamamos o original (1a instr e ldr [pc] PC-relativa, intrampolinavel; e o
 * register real falha de qualquer forma) — so retornamos 0 = "File OK". */
static int my_filereg_5f02c(void) {
  static int once;
  if (!once) { debugPrintf("FILEREG: 0x5f02c -> 0 (skip, File via hooks)\n"); once = 1; }
  return 0;
}

/* ---- config interna 0x52628(key) = config[section][key] (byte). No so-loader a
 * tabela de config esta vazia/quebrada e retorna LIXO (!=0) p/ chaves "DisableXxx",
 * fazendo o device-register PULAR subsistemas — fatal p/ "DisableThreadCore"
 * (a init de TLS 0x7f928 que cria a key do surface em 0xc9a74). Hookamos: chaves
 * "Disable*" -> 0 (NADA desabilitado = default correto); resto -> original. ---- */
static int (*real_cfg_internal_52628)(const char *);
static int my_cfg_internal_52628(const char *key) {
  if (key && strncmp(key, "Disable", 7) == 0) {
    static int logs;
    /* teste: DisableThreads=1 (sem worker pool -> jobs INLINE/sincrono na main,
     * evitando o dispatch async quebrado que dava timeout no load de recursos).
     * ThreadCore (TLS) FICA habilitado (=0). gated por SONIC4EP1_NOTHREADS. */
    int v = 0;
    if (getenv("SONIC4EP1_NOTHREADS") && strcmp(key, "DisableThreads") == 0)
      v = 1;
    if (logs < 24) { debugPrintf("CFGINT: '%s' -> %d\n", key, v); logs++; }
    return v;
  }
  return real_cfg_internal_52628 ? real_cfg_internal_52628(key) : 0;
}

/* ---- device-register 0x58cdc: registra os subsistemas (File/Surface...).
 * Se faz early-return (0x58e50, r0=1) por falha de um subsistema, pula o
 * surface (0x7f928). Logamos entrada+retorno. ---- */
static unsigned (*real_devreg_58cdc)(unsigned);
static int g_devreg_logs;
static unsigned diag_devreg_58cdc(unsigned a) {
  unsigned r = real_devreg_58cdc ? real_devreg_58cdc(a) : 1;
  if (g_devreg_logs < 8) {
    debugPrintf("DEVREG: 0x58cdc(arg=%u) -> ret=%u\n", a, r);
    g_devreg_logs++;
  }
  return r;
}

/* ---- SURFDIAG: descobrir se a init do surface roda e em qual thread ---- */
static unsigned (*real_surf_init_7f928)(void);
static unsigned diag_surf_init_7f928(void) {
  debugPrintf("SURFDIAG: surface_init 0x7f928 ENTER thread=%p\n",
              (void *)pthread_self());
  unsigned r = real_surf_init_7f928 ? real_surf_init_7f928() : 1;
  uintptr_t b = g_main_text_base;
  unsigned key = b ? *(unsigned *)(b + 0xc9a74) : 0;
  void *bound = key ? pthread_getspecific((pthread_key_t)(key - 1)) : (void *)0;
  debugPrintf("SURFDIAG: surface_init 0x7f928 RET=%u key=%u bound_on_init_thread=%p\n",
              r, key, bound);
  return r;
}
static int g_surfdiag_get_logs;
static void *diag_tls_getter(unsigned kp1) {
  void *v = (kp1 == 0) ? (void *)0
                       : pthread_getspecific((pthread_key_t)(kp1 - 1));
  if (g_surfdiag_get_logs < 8) {
    debugPrintf("SURFDIAG: TLS getter kp1=%u thread=%p -> %p\n", kp1,
                (void *)pthread_self(), v);
    g_surfdiag_get_logs++;
  }
  return v; /* valor REAL (pode ser NULL -> crash, mas ja logamos) */
}

/* ---- s3eAPKExpansion: o game (módulo .s3e) carrega os recursos (resources.dz)
 * via OBB/APKExpansion; a extensao Android retorna "Not implemented" sem o sistema
 * de expansao -> game aborta "Error loading resources (Not implemented)". resources.dz
 * JA esta no deploy. Hookamos as funcs da ext (na apkexp.so, base = RegisterExt-0xd1c)
 * p/ dizer "baixado/completo" + path local. Ordem dos wrappers (por endereco) casa a
 * ordem dos nomes na .so: GetAbsolutePath, GetDownloadState, GetMainExpansionFilename,
 * Initialize, Start, Stop. ---- */
static char g_apkexp_dir[1024];
static const char *apk_GetAbsolutePath(void) {
  debugPrintf("APKEXP: GetAbsolutePath -> '%s'\n", g_apkexp_dir);
  return g_apkexp_dir;
}
static int apk_GetDownloadState(void) {
  debugPrintf("APKEXP: GetDownloadState -> 3 (complete)\n");
  return 3; /* palpite: COMPLETE; refinar pelo log */
}
static const char *apk_GetMainExpansionFilename(void) {
  debugPrintf("APKEXP: GetMainExpansionFilename -> 'resources.dz'\n");
  return "resources.dz";
}
static int apk_Initialize(void) { debugPrintf("APKEXP: Initialize\n"); return 0; }
static int apk_Start(void) { debugPrintf("APKEXP: Start\n"); return 0; }
static int apk_Stop(void) { debugPrintf("APKEXP: Stop\n"); return 0; }

static void install_apkexpansion_hooks(void) {
  if (!getenv("SONIC4EP1_APKEXP")) /* opt-in: hook na apkexp.so ainda instavel */
    return;
  uintptr_t reg = 0;
  for (int i = 0; i < g_ext_registers_n; i++)
    if (g_ext_registers[i].name && strstr(g_ext_registers[i].name, "APKExpansion"))
      reg = (uintptr_t)g_ext_registers[i].addr;
  if (!reg) { debugPrintf("APKEXP: RegisterExt nao achado\n"); return; }
  uintptr_t b = reg - 0x0d1c; /* RegisterExt @ off 0xd1c na apkexp.so */
  /* o text da apkexp.so vem R-X; hook_arm escreve no codigo -> mprotect RWX antes. */
  long pg = sysconf(_SC_PAGESIZE);
  uintptr_t lo = (b + 0x800) & ~((uintptr_t)pg - 1);
  uintptr_t hi = ((b + 0xc00) + pg - 1) & ~((uintptr_t)pg - 1);
  int mpr = mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC);
  debugPrintf("APKEXP: reg=%p base=%p mprotect(%p..%p)=%d errno=%d\n",
              (void *)reg, (void *)b, (void *)lo, (void *)hi, mpr, errno);
  /* prova: tenta ler+escrever em base+0x8a0 num try simples */
  volatile uint32_t *probe = (volatile uint32_t *)(b + 0x8a0);
  debugPrintf("APKEXP: probe read [base+0x8a0]=0x%08x (vai escrever...)\n", *probe);
  const char *dd = getenv("SONIC4EP1_DEPLOY_DIR");
  if (dd && *dd) snprintf(g_apkexp_dir, sizeof(g_apkexp_dir), "%s", dd);
  else if (!getcwd(g_apkexp_dir, sizeof(g_apkexp_dir))) snprintf(g_apkexp_dir, sizeof(g_apkexp_dir), ".");
  hook_arm(b + 0x8a0, (uintptr_t)apk_GetAbsolutePath);
  hook_arm(b + 0x900, (uintptr_t)apk_GetDownloadState);
  hook_arm(b + 0x950, (uintptr_t)apk_GetMainExpansionFilename);
  hook_arm(b + 0x9a0, (uintptr_t)apk_Initialize);
  hook_arm(b + 0x9fc, (uintptr_t)apk_Start);
  hook_arm(b + 0xa6c, (uintptr_t)apk_Stop);
  debugPrintf("APKEXP: 6 funcs hookadas (base=%p, dir='%s')\n", (void *)b, g_apkexp_dir);
}

/* ---- TLSMAP: emulacao do s3eThreadLocal (wrappers get=0x82d60 / set=0x82d70).
 * O esquema nativo guarda a "key" em globais (0xc9a74 etc.) e os wrappers fazem
 * pthread_get/setspecific(key-1). No so-loader as keys vem invalidas (malloc-ptr/0)
 * -> getspecific NULL -> crash do surface/fibre/threadcore. Como o so-loader e
 * single-thread (main thread dirige tudo), substituimos por um MAPA GLOBAL
 * key->valor: set(key,val) guarda, get(key) devolve o mesmo. Round-trip perfeito,
 * conserta TODAS as TLS de uma vez (convergente). ---- */
/* PER-THREAD: o engine cria 1 worker thread (carregamento de recursos). A key e
 * composta (thread, key) p/ cada thread ter seu proprio valor — senao o worker le
 * o estado-de-surface da main, malfunciona, e o job nunca completa (wait timeout =
 * "Error loading resources"). Mutex protege o array (acesso de 2 threads). */
#define EP1_TLS_MAX 4096
static uintptr_t g_tls_tid[EP1_TLS_MAX];
static uintptr_t g_tls_keys[EP1_TLS_MAX];
static void *g_tls_vals[EP1_TLS_MAX];
static int g_tls_n;
static pthread_mutex_t g_tls_lock = PTHREAD_MUTEX_INITIALIZER;
static void *my_s3e_tls_get(uintptr_t key) {
  if (key == 0) return (void *)0;
  uintptr_t tid = (uintptr_t)pthread_self();
  void *r = (void *)0;
  pthread_mutex_lock(&g_tls_lock);
  for (int i = 0; i < g_tls_n; i++)
    if (g_tls_keys[i] == key && g_tls_tid[i] == tid) { r = g_tls_vals[i]; break; }
  pthread_mutex_unlock(&g_tls_lock);
  return r;
}
static void my_s3e_tls_set(uintptr_t key, void *val) {
  if (key == 0) return;
  uintptr_t tid = (uintptr_t)pthread_self();
  pthread_mutex_lock(&g_tls_lock);
  for (int i = 0; i < g_tls_n; i++)
    if (g_tls_keys[i] == key && g_tls_tid[i] == tid) {
      g_tls_vals[i] = val; pthread_mutex_unlock(&g_tls_lock); return;
    }
  if (g_tls_n < EP1_TLS_MAX) {
    g_tls_tid[g_tls_n] = tid; g_tls_keys[g_tls_n] = key;
    g_tls_vals[g_tls_n] = val; g_tls_n++;
  }
  pthread_mutex_unlock(&g_tls_lock);
}

static void install_s3efile_shims(uintptr_t base) {
  if (!base || getenv("SONIC4EP1_NO_FILESHIM"))
    return;
  if (!getenv("SONIC4EP1_NO_TLSMAP")) {
    hook_arm(base + 0x82d60, (uintptr_t)my_s3e_tls_get);
    hook_arm(base + 0x82d70, (uintptr_t)my_s3e_tls_set);
    debugPrintf("TLSMAP: s3eThreadLocal get/set emulados (mapa global)\n");
  }
  if (getenv("SONIC4EP1_HOOK62D90"))
    hook_arm(base + 0x62d90, (uintptr_t)my_s3eFile_internal_open);
  if (getenv("SONIC4EP1_SURFDIAG")) {
    real_surf_init_7f928 = (unsigned (*)(void))make_arm_trampoline(base + 0x7f928);
    hook_arm(base + 0x7f928, (uintptr_t)diag_surf_init_7f928);
    hook_arm(base + 0x82d60, (uintptr_t)diag_tls_getter);
    real_devreg_58cdc = (unsigned (*)(unsigned))make_arm_trampoline(base + 0x58cdc);
    hook_arm(base + 0x58cdc, (uintptr_t)diag_devreg_58cdc);
    debugPrintf("SURFDIAG: hooks 0x7f928 + 0x82d60 + 0x58cdc instalados\n");
  }
  if (!getenv("SONIC4EP1_NO_CFG_NODISABLE")) {
    real_cfg_internal_52628 =
        (int (*)(const char *))make_arm_trampoline(base + 0x52628);
    hook_arm(base + 0x52628, (uintptr_t)my_cfg_internal_52628);
    debugPrintf("CFGINT: hook 0x52628 (Disable*->0) instalado\n");
  }
  if (getenv("SONIC4EP1_FAKE_FILEREG")) {
    hook_arm(base + 0x5f02c, (uintptr_t)my_filereg_5f02c);
    debugPrintf("FILEREG: hook 0x5f02c instalado\n");
  }
  if (getenv("SONIC4EP1_CPUINFO_HOOK") || getenv("SONIC4EP1_CPUINFO0")) {
    real_cpuinfo_3e970 = (unsigned (*)(void))make_arm_trampoline(base + 0x3e970);
    hook_arm(base + 0x3e970, (uintptr_t)diag_cpuinfo_3e970);
    debugPrintf("CPUINFO: hook 0x3e970 instalado\n");
  }
  if (getenv("SONIC4EP1_SURFOBJ") && !getenv("SONIC4EP1_SURFDIAG"))
    hook_arm(base + 0x82d60, (uintptr_t)my_tls_getter);
  if (getenv("SONIC4EP1_RESOLVE"))
    hook_arm(base + 0x67708, (uintptr_t)my_s3eFile_resolve);
  hook_arm(base + 0x63438, (uintptr_t)my_s3eFileOpen);
  hook_arm(base + 0x5fbdc, (uintptr_t)my_s3eFileOpenFromMemory);
  hook_arm(base + 0x60180, (uintptr_t)my_s3eFileClose);
  hook_arm(base + 0x602f8, (uintptr_t)my_s3eFileRead);
  hook_arm(base + 0x6059c, (uintptr_t)my_s3eFileWrite);
  hook_arm(base + 0x61398, (uintptr_t)my_s3eFileSeek);
  hook_arm(base + 0x61584, (uintptr_t)my_s3eFileTell);
  hook_arm(base + 0x5fef4, (uintptr_t)my_s3eFileGetSize);
  hook_arm(base + 0x62bc4, (uintptr_t)my_s3eFileCheckExists);
  hook_arm(base + 0x6085c, (uintptr_t)my_s3eFileReadString);
  hook_arm(base + 0x616b0, (uintptr_t)my_s3eFileFlush);
  hook_arm(base + 0x5f01c, (uintptr_t)my_s3eFileGetError);
  hook_arm(base + 0x61800, (uintptr_t)my_s3eFileListDirectory);
  hook_arm(base + 0x61f8c, (uintptr_t)my_s3eFileListNext);
  hook_arm(base + 0x5e07c, (uintptr_t)my_s3eFileListClose);
  debugPrintf("shim: s3eFile API -> libc real (cwd)\n");
}

/* ===================================================================
 * SHIM s3eConfig -> serve valores do nosso icf (o objeto de config do s3e
 * fica com campos NULL pois o bootstrap nao roda; o parser crasha). Damos
 * os valores que o loader/jogo precisam direto.
 * =================================================================== */
struct ep1_cfg_kv {
  const char *key;
  int ival;
  const char *sval;
};
static const struct ep1_cfg_kv g_cfg[] = {
    {"MemSize", 33554432, "33554432"},
    {"MemSize0", 33554432, "33554432"},
    {"MemSize1", 16777216, "16777216"},
    {"MemRequiredToRunApp", 0, "0"},
    {"MemTooSmallSkipCheck", 1, "1"},
    {"SysGlesVersion", 1, "1"},
    {"GameExecutable", 0, "Sonic4epI.s3e"},
    {"DispFixRot", 0, "FixedLandscape"},
    {"VirtualRotate", -90, "-90"},
    {"DoubleBuffer", 1, "1"},
    /* CHAVE: o file system Android do s3e monta rom://(resource) e ram://(write)
     * a partir destes paths REAIS. Apontando pro dir do port, os drives ficam
     * backed por FS real -> o loader le Sonic4epI.s3e de verdade. */
    {"AndroidFileRstPath", 0, "/storage/roms/ports/sonic4ep1"},
    {"AndroidFileRamPath", 0, "/storage/roms/ports/sonic4ep1"},
    {"AndroidFileUseSdcard", 0, "0"},
    {"FileUseCase", 1, "1"},
    {"NumClassFactories", 512, "512"},
    {"NumMemBuckets", 512, "512"},
};
static const struct ep1_cfg_kv *ep1_cfg_find(const char *key) {
  if (!key)
    return NULL;
  for (unsigned i = 0; i < sizeof(g_cfg) / sizeof(g_cfg[0]); i++)
    if (strcasecmp(key, g_cfg[i].key) == 0)
      return &g_cfg[i];
  return NULL;
}
/* O surface object existe em [0xcf784] mas o getter le via TLS (pthread_getspecific
 * com a chave em [0xe64d8]/[0xe6474]; o wrapper 0x82d60 usa chave-1). O TLS nunca
 * e setado (sem render thread real) -> NULL deref em 0x8333c. Setamos o TLS na
 * thread do gate (onde o config e lido) apontando pro surface object global. */
static void ep1_try_set_surface_tls(void) {
  uintptr_t base = g_main_text_base;
  if (!base)
    return;
  void *obj = *(void **)(base + 0xcf784);
  if (!obj)
    return;
  static int done;
  if (done)
    return;
  unsigned k1 = *(unsigned *)(base + 0xe64d8);
  unsigned k2 = *(unsigned *)(base + 0xe6474);
  if (k1)
    pthread_setspecific((pthread_key_t)(k1 - 1), obj);
  if (k2 && k2 != k1)
    pthread_setspecific((pthread_key_t)(k2 - 1), obj);
  if (k1 || k2) {
    done = 1;
    debugPrintf("shim: surface TLS setado obj=%p k1=%u k2=%u\n", obj, k1, k2);
  }
}

static int my_s3eConfigGetInt(const char *group, const char *key, int *value) {
  (void)group;
  ep1_try_set_surface_tls();
  const struct ep1_cfg_kv *kv = ep1_cfg_find(key);
  if (getenv("SONIC4EP1_CFGLOG"))
    debugPrintf("s3eConfigGetInt(%s,%s) -> %s\n", group ? group : "?",
                key ? key : "?", kv ? "hit" : "miss");
  if (kv && value) {
    *value = kv->ival;
    return 0; /* S3E_RESULT_SUCCESS */
  }
  return 1; /* not found -> loader usa default */
}
static int my_s3eConfigGetString(const char *group, const char *key,
                                 char *dest) {
  (void)group;
  const struct ep1_cfg_kv *kv = ep1_cfg_find(key);
  if (getenv("SONIC4EP1_CFGLOG"))
    debugPrintf("s3eConfigGetString(%s,%s) -> %s\n", group ? group : "?",
                key ? key : "?", kv && kv->sval ? kv->sval : "miss");
  if (kv && kv->sval && dest) {
    strcpy(dest, kv->sval);
    return 0;
  }
  return 1;
}
static int my_cfg_load_noop(void *a, void *b, void *c) {
  (void)a;
  (void)b;
  (void)c;
  return 0; /* pula o parser do config que crasha (objeto NULL) */
}
/* variantes por HASH: nao da pra mapear hash->valor, retornamos "nao achado"
 * (1) sem dereferenciar o objeto config NULL -> loader/jogo usam default. */
static int my_s3eConfigGetIntHash(unsigned g, unsigned k, int *v) {
  (void)g;
  (void)k;
  (void)v;
  return 1;
}
static int my_s3eConfigGetStringHash(unsigned g, unsigned k, char *d) {
  (void)g;
  (void)k;
  (void)d;
  return 1;
}
static void install_s3econfig_shims(uintptr_t base) {
  if (!base)
    return;
  /* tabela de string-intern do config (*0xc875c) nunca e alocada -> NULL deref
   * em 0x51fac. Alocamos uma tabela zerada; o caminho de "add" cresce os
   * buffers via realloc(NULL,...) = malloc. Com ela, o parser REAL do config
   * funciona (le o s3e.icf via File shim) e popula o config -> os reads internos
   * (0x52628) do surface setup retornam os valores certos. */
  if (!getenv("SONIC4EP1_NO_STRTAB")) {
    void *tab = calloc(512, 1);
    *(void **)(base + 0xc875c) = tab;
    debugPrintf("shim: alocada tabela string-intern config @ %p\n", tab);
  }
  /* GetXxx + parser no-op: por padrao ON (servimos o config do g_cfg direto,
   * pois a string-table fake quebra o intern/lookup do parser real). */
  if (!getenv("SONIC4EP1_NO_CFGSHIM")) {
    hook_arm(base + 0x52560, (uintptr_t)my_s3eConfigGetInt);
    hook_arm(base + 0x522b4, (uintptr_t)my_s3eConfigGetString);
    hook_arm(base + 0x524d4, (uintptr_t)my_s3eConfigGetIntHash);
    hook_arm(base + 0x521f4, (uintptr_t)my_s3eConfigGetStringHash);
    debugPrintf("shim: s3eConfig GetXxx no-op (opt-in)\n");
  }
  /* s3 (2026-06-29): 0x3a9f4 NAO e parser de config — a tabela JNINativeMethod
   * diz que e o NATIVO `setViewNative` (assinatura (JNIEnv*,jobject,jobject);
   * faz NewGlobalRef/GetObjectClass/GetMethodID na view e cacheia os handles
   * glSwapBuffers/doDraw/soundInit nos globais do engine). Stubá-lo (s2, por
   * engano) impede o engine de obter esses handles -> o surface object nunca e
   * criado -> crash do surface (TLS 0x82d60 / 0x5db04 / 0x7be40). Por isso AGORA
   * fica OFF por padrao (setViewNative roda); so stuba se SONIC4EP1_STUB_SETVIEW. */
  if (getenv("SONIC4EP1_STUB_SETVIEW")) {
    hook_arm(base + 0x3a9f4, (uintptr_t)my_cfg_load_noop);
    debugPrintf("shim: setViewNative 0x3a9f4 STUBADO (opt-in)\n");
  }
}

static int (*real_gate_13c44)(void);
static int marm_gate_13c44(void) {
  uintptr_t base = g_main_text_base;
  /* Injecao do icf EMBUTIDO (via s3eFileOpenFromMemory) so se pedido — precisa
   * do drive ram:// que pode nao estar registrado. Por padrao deixamos o loader
   * achar o ./s3e.icf REAL via rom:// (deploy_dir=cwd abaixo). */
  if (getenv("SONIC4EP1_ICF_EMBED")) {
    *(uintptr_t *)(base + 0xc3168) = (uintptr_t)g_s3e_icf_text;
    *(const char **)(base + 0xc3270) = g_s3e_icf_text;
  }
  /*
   * deploy_dir (rom:// source) = [global2(0xc5950)+396]. Por padrao = arg1
   * "sonic4ep1.apk" (precisaria de backend zip/AssetManager que nao temos).
   * Apontamos pro DIR REAL (cwd) para o s3e File montar rom:// no FS real,
   * onde estao Sonic4epI.s3e e s3e.icf.
   */
  const char *dd = getenv("SONIC4EP1_DEPLOY_DIR");
  if (dd && *dd)
    snprintf(g_deploy_dir, sizeof(g_deploy_dir), "%s", dd);
  else if (!getcwd(g_deploy_dir, sizeof(g_deploy_dir)))
    snprintf(g_deploy_dir, sizeof(g_deploy_dir), ".");
  *(const char **)(base + 0xc5950 + 396) = g_deploy_dir;
  int gr = real_gate_13c44 ? real_gate_13c44() : 1;
  /* s3 (2026-06-29): o SURFDIAG mostrou que a init do surface 0x7f928 NUNCA e
   * chamada pelo device-register no so-loader -> a TLS key em 0xc9a74 fica 0 e o
   * surface object NULL -> crash 0x5db04. Forcamos 0x7f928 AQUI: bootstrap A ja
   * registrou os subsistemas (device globals prontos) e estamos na MAIN thread
   * (mesma do frame loop), entao o MakeCurrent interno de 0x7f928 binda o surface
   * na thread certa. Gated por SONIC4EP1_FORCE_SURFINIT (default ON via run.sh). */
  if (getenv("SONIC4EP1_FORCE_SURFINIT") && base) {
    unsigned before = *(unsigned *)(base + 0xc9a74);
    unsigned sr = ((unsigned (*)(void))(base + 0x7f928))();
    unsigned after = *(unsigned *)(base + 0xc9a74);
    void *bound = after ? pthread_getspecific((pthread_key_t)(after - 1)) : 0;
    debugPrintf("FORCE_SURFINIT: 0x7f928 ret=%u key %u->%u bound=%p thread=%p\n",
                sr, before, after, bound, (void *)pthread_self());
  }
  return gr;
}

static void patch_rel_ret0(uintptr_t rel, const char *name) {
  uintptr_t base = g_main_text_base ? g_main_text_base : (uintptr_t)text_base;
  if (!base)
    return;
  uintptr_t addr = base + rel;
  uint32_t *p = (uint32_t *)addr;
  p[0] = 0xe3a00000u; /* mov r0,#0 */
  p[1] = 0xe12fff1eu; /* bx lr */
  debugPrintf("diag patch: %s @ %p -> return 0\n", name, (void *)addr);
}

static void patch_ret0_list_from_env(void) {
  const char *list = getenv("SONIC4EP1_RET0_LIST");
  if (!list || !*list)
    return;

  char *copy = strdup(list);
  if (!copy)
    return;
  for (char *tok = strtok(copy, ",;: "); tok; tok = strtok(NULL, ",;: ")) {
    char *end = NULL;
    unsigned long rel = strtoul(tok, &end, 0);
    if (end && *end == '\0')
      patch_rel_ret0((uintptr_t)rel, tok);
    else
      debugPrintf("diag patch: ignorando rel invalido \"%s\"\n", tok);
  }
  free(copy);
}

/* Diag: log de s3eFileAddUserFileSys (o archive package.dz registra um user-fs
 * cujo read DESCRIPTOGRAFA). Vê se/quando é chamado e os args (callbacks). */
static int (*real_addufs)(void *, void *) = NULL;
void *g_ufs_cbs = NULL, *g_ufs_ud = NULL;
/* a struct cbs passada a s3eFileAddUserFileSys é TEMPORÁRIA (o runtime copia);
 * o ponteiro fica dangling. COPIAMOS os valores no registro. */
uintptr_t g_cbs_copy[8];
int g_cbs_ok = 0;
static FILE *ep1_extract_from_archive(const char *fn);
typedef void *(*find_resource_class_fn)(void *, unsigned);
static find_resource_class_fn real_find_resource_class;
static uintptr_t g_find_resource_class_addr;
typedef void *(*find_resource_outer_fn)(void *, unsigned, unsigned, unsigned);
static find_resource_outer_fn real_find_resource_outer;
static uintptr_t g_find_resource_outer_addr;
typedef void *(*find_resource_full_fn)(void *, unsigned, unsigned, unsigned,
                                       unsigned);
static find_resource_full_fn real_find_resource_full;
static uintptr_t g_find_resource_full_addr;
typedef void *(*resource_slot_fn)(void *, int, int, int);
static resource_slot_fn real_resource_slot;
static uintptr_t g_resource_slot_addr;
static void *g_resource_slot_fallback;
typedef void *(*resource_index_apply_fn)(void *, unsigned, unsigned, unsigned);
static resource_index_apply_fn real_resource_index_apply;
static uintptr_t g_resource_index_apply_addr;
typedef void *(*resource_group_update_fn)(void *);
static resource_group_update_fn real_resource_group_update;
static uintptr_t g_resource_group_update_addr;
typedef int (*resource_flags_fn)(void *, unsigned, unsigned, unsigned);
static resource_flags_fn real_resource_flags;
static uintptr_t g_resource_flags_addr;
static uintptr_t g_resource_field32_addr;
typedef void (*resource_bind_fn)(void *, void *, unsigned, unsigned);
static resource_bind_fn real_resource_bind;
static uintptr_t g_resource_bind_addr;
static uintptr_t g_resource_set90_addr;
typedef void *(*resource_parent_ctor_fn)(void *, void *, unsigned, unsigned);
typedef void *(*resource_default_ctor_fn)(void);
static resource_parent_ctor_fn real_resource_parent_ctor;
static resource_default_ctor_fn real_resource_default_ctor;
static uintptr_t g_resource_parent_ctor_addr;
typedef void *(*resource_small_ctor_fn)(void *);
static resource_small_ctor_fn real_resource_small_ctor;
static uintptr_t g_resource_small_ctor_addr;
typedef void *(*resource_array_find_fn)(void *, unsigned);
static resource_array_find_fn real_resource_array_find;
static uintptr_t g_resource_array_find_addr;
typedef int (*resource_call8_fn)(void *);
static resource_call8_fn real_resource_call8;
static uintptr_t g_resource_call8_addr;
typedef void (*resource_iter28_fn)(void *);
static resource_iter28_fn real_resource_iter28;
static uintptr_t g_resource_iter28_addr;
typedef void *(*resource_self3c_fn)(void *);
static resource_self3c_fn real_resource_self3c;
static uintptr_t g_resource_self3c_addr;
typedef int (*resource_self34_fn)(void *);
static resource_self34_fn real_resource_self34;
static uintptr_t g_resource_self34_addr;
typedef int (*resource_self8_fn)(void *);
static resource_self8_fn real_resource_self8;
static uintptr_t g_resource_self8_addr;
typedef int (*resource_self14_fn)(void *);
static resource_self14_fn real_resource_self14;
static uintptr_t g_resource_self14_addr;
typedef int (*resource_self1c_fn)(void *);
static resource_self1c_fn real_resource_self1c;
static uintptr_t g_resource_self1c_addr;
typedef void (*scoreloop_sync_fn)(void *, void *);
static scoreloop_sync_fn real_scoreloop_sync;
static uintptr_t g_scoreloop_sync_addr;
typedef void *(*class_factory_find_fn)(void *, unsigned);
static class_factory_find_fn real_class_factory_find;
static uintptr_t g_class_factory_find_addr;
typedef void *(*class_factory_make_fn)(unsigned);
static class_factory_make_fn real_class_factory_make;
static uintptr_t g_class_factory_make_addr;
typedef void *(*gx_font_ctx_fn)(void);
static gx_font_ctx_fn real_gx_font_ctx;
static uintptr_t g_gx_font_ctx_addr;
typedef void *(*music_play_info_fn)(void *, void *, unsigned, unsigned);
static music_play_info_fn real_music_play_info;
static uintptr_t g_music_play_info_addr;
static uintptr_t g_class_diag_game_base;
static uintptr_t g_class_factory_make_plain;
static uintptr_t g_resource_class_vtable;
#define SEEN_RESOURCE_CLASS_MAX 128
static struct {
  unsigned hash;
  void *entry;
} g_seen_resource_classes[SEEN_RESOURCE_CLASS_MAX];
static unsigned g_seen_resource_class_count;
static __thread void *g_last_tagged_mgr;
static __thread unsigned g_last_tagged_hash;
static __thread void *g_last_tagged_obj;
typedef void *(*sound_group_load_fn)(void *, unsigned);
static sound_group_load_fn real_sound_group_load;
static uintptr_t g_sound_group_load_addr;
static uintptr_t g_game_file_imports_base;

static int ptr_range_mapped(const void *p, size_t len) {
  uintptr_t a = (uintptr_t)p;
  if (!a || len == 0)
    return 0;
  uintptr_t e = a + len;
  if (e < a)
    return 0;
  for (int i = 0; i < g_maps_n; i++)
    if (a >= g_maps[i].start && e <= g_maps[i].end)
      return 1;
  return 0;
}

static int make_exec_page_writable(uintptr_t addr, size_t len) {
  long ps = sysconf(_SC_PAGESIZE);
  if (ps <= 0) ps = 4096;
  uintptr_t lo = (addr & ~((uintptr_t)ps - 1));
  uintptr_t hi = (addr + len + (uintptr_t)ps - 1) & ~((uintptr_t)ps - 1);
  int r = mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC);
  if (r != 0)
    debugPrintf("NULLREG: mprotect(%p..%p) falhou errno=%d\n",
                (void *)lo, (void *)hi, errno);
  return r;
}

static void hook_thumb_abs_any_align(uintptr_t thumb_addr, uintptr_t dst) {
  uintptr_t a = thumb_addr & ~1u;
  if ((a & 2u) == 0) {
    hook_arm(thumb_addr, dst);
    return;
  }

  uint16_t *h = (uint16_t *)a;
  h[0] = 0x4b01; /* ldr r3, [pc, #4] ; literal fica alinhado em a+6 */
  h[1] = 0x4718; /* bx r3 */
  h[2] = 0xbf00; /* nop/alinhamento */
  *(uint32_t *)(h + 3) = (uint32_t)dst;
}

static void scan_class_hash_once(unsigned hash) {
  const char *env = getenv("PES_SCAN_CLASS_HASH");
  if (!env || !*env)
    return;

  unsigned target = (unsigned)strtoul(env, NULL, 0);
  if (target && target != hash)
    return;

  enum { MAX_SCANNED = 16, MAX_HITS = 80 };
  static unsigned scanned[MAX_SCANNED];
  static unsigned scanned_count;
  for (unsigned i = 0; i < scanned_count; i++)
    if (scanned[i] == hash)
      return;
  if (scanned_count < MAX_SCANNED)
    scanned[scanned_count++] = hash;

  snapshot_maps();
  unsigned char pat[4] = {
      (unsigned char)(hash & 0xffu),
      (unsigned char)((hash >> 8) & 0xffu),
      (unsigned char)((hash >> 16) & 0xffu),
      (unsigned char)((hash >> 24) & 0xffu),
  };
  unsigned hits = 0;
  debugPrintf("CLASS_SCAN: hash=0x%08x begin maps=%d\n", hash, g_maps_n);
  for (int mi = 0; mi < g_maps_n && hits < MAX_HITS; mi++) {
    if (!strchr(g_maps[mi].perms, 'r'))
      continue;
    uintptr_t start = g_maps[mi].start;
    uintptr_t end = g_maps[mi].end;
    if (end <= start + 4 || start < 0x10000u)
      continue;
    for (uintptr_t a = start; a + 4 <= end && hits < MAX_HITS; a++) {
      if (memcmp((void *)a, pat, sizeof(pat)) != 0)
        continue;
      uintptr_t off = g_maps[mi].off + (a - start);
      uintptr_t lo = a >= start + 16 ? a - 16 : start;
      uintptr_t hi = a + 32 < end ? a + 32 : end;
      char words[256];
      size_t pos = 0;
      words[0] = 0;
      lo &= ~(uintptr_t)3;
      for (uintptr_t w = lo; w + 4 <= hi && pos + 12 < sizeof(words); w += 4) {
        uint32_t v;
        memcpy(&v, (void *)w, sizeof(v));
        pos += snprintf(words + pos, sizeof(words) - pos, "%s%08x",
                        w == a ? "*" : (w + 4 == a ? " " : " "), v);
      }
      debugPrintf("CLASS_SCAN: hit[%u] addr=%p map=%s+0x%lx words:%s\n",
                  hits, (void *)a, g_maps[mi].path,
                  (unsigned long)off, words);
      hits++;
    }
  }
  debugPrintf("CLASS_SCAN: hash=0x%08x hits=%u\n", hash, hits);
}

static void dump_class_registry_once(void *registry, uintptr_t entries,
                                     unsigned count) {
  if (!getenv("PES_DUMP_CLASS_REG") || !registry || !entries || !count)
    return;

  enum { MAX_DUMPED = 32 };
  static void *dumped[MAX_DUMPED];
  static unsigned dumped_count;
  for (unsigned i = 0; i < dumped_count; i++)
    if (dumped[i] == registry)
      return;
  if (dumped_count < MAX_DUMPED)
    dumped[dumped_count++] = registry;

  unsigned n = count < 8 ? count : 8;
  debugPrintf("CLASS_REG: registry=%p entries=%p count=%u dump=%u\n",
              registry, (void *)entries, count, n);
  for (unsigned i = 0; i < n; i++) {
    uintptr_t e = entries + (uintptr_t)i * 48u;
    if (!ptr_range_mapped((void *)e, 48)) {
      debugPrintf("CLASS_REG:  [%u] entry=%p unmapped\n", i, (void *)e);
      continue;
    }
    uint32_t w[12];
    memcpy(w, (void *)e, sizeof(w));
    debugPrintf("CLASS_REG:  [%u] %p: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                i, (void *)e, w[0], w[1], w[2], w[3], w[4], w[5],
                w[6], w[7], w[8], w[9], w[10], w[11]);
  }
}

static void remember_resource_classes(uintptr_t entries, unsigned count) {
  if (!entries || !count)
    return;

  unsigned n = count < 64 ? count : 64;
  for (unsigned i = 0; i < n; i++) {
    uintptr_t e = entries + (uintptr_t)i * 48u;
    if (!ptr_range_mapped((void *)e, 48))
      continue;
    unsigned hash = *(unsigned *)(e + 4);
    if (!hash)
      continue;
    int found = 0;
    for (unsigned j = 0; j < g_seen_resource_class_count; j++) {
      if (g_seen_resource_classes[j].hash == hash) {
        g_seen_resource_classes[j].entry = (void *)e;
        found = 1;
        break;
      }
    }
    if (!found && g_seen_resource_class_count < SEEN_RESOURCE_CLASS_MAX) {
      g_seen_resource_classes[g_seen_resource_class_count].hash = hash;
      g_seen_resource_classes[g_seen_resource_class_count].entry = (void *)e;
      g_seen_resource_class_count++;
      if (getenv("PES_CLASS_FALLBACK_SEEN"))
        debugPrintf("CLASS_SEEN: hash=0x%08x entry=%p\n", hash, (void *)e);
    }
  }
}

static void *seen_resource_class(unsigned hash) {
  if (!hash || !getenv("PES_CLASS_FALLBACK_SEEN"))
    return NULL;
  for (unsigned i = 0; i < g_seen_resource_class_count; i++) {
    if (g_seen_resource_classes[i].hash == hash &&
        g_seen_resource_classes[i].entry &&
        ptr_range_mapped(g_seen_resource_classes[i].entry, 48)) {
      static int d = 0;
      if (d < 80) {
        debugPrintf("CLASS_SEEN: fallback hash=0x%08x -> %p\n", hash,
                    g_seen_resource_classes[i].entry);
        d++;
      }
      return g_seen_resource_classes[i].entry;
    }
  }
  return NULL;
}

static int get_factory_table(uintptr_t *out_entries, unsigned *out_count) {
  if (!g_class_factory_make_plain)
    return 0;
  uintptr_t lit_addr = g_class_factory_make_plain + 0x24u;
  if (!ptr_range_mapped((void *)lit_addr, sizeof(uint32_t)))
    return 0;
  uintptr_t holder = (uintptr_t)*(uint32_t *)lit_addr;
  if (!ptr_range_mapped((void *)holder, sizeof(uintptr_t)))
    return 0;
  uintptr_t table = *(uintptr_t *)holder;
  if (!ptr_range_mapped((void *)table, 8))
    return 0;
  uintptr_t entries = *(uintptr_t *)table;
  unsigned count = *(unsigned *)(table + 4);
  if (!entries || count > 512 ||
      !ptr_range_mapped((void *)entries, (size_t)count * 12u))
    return 0;
  if (out_entries)
    *out_entries = entries;
  if (out_count)
    *out_count = count;
  return 1;
}

static int factory_table_has_hash(unsigned hash) {
  uintptr_t entries = 0;
  unsigned count = 0;
  if (!hash || !get_factory_table(&entries, &count))
    return 0;
  for (unsigned i = 0; i < count; i++) {
    uintptr_t e = entries + (uintptr_t)i * 12u;
    if (*(unsigned *)e == hash)
      return 1;
  }
  return 0;
}

static uintptr_t factory_ctor_for_hash(unsigned hash) {
  uintptr_t entries = 0;
  unsigned count = 0;
  if (!hash || !get_factory_table(&entries, &count))
    return 0;
  for (unsigned i = 0; i < count; i++) {
    uintptr_t e = entries + (uintptr_t)i * 12u;
    if (*(unsigned *)e == hash)
      return *(uintptr_t *)(e + 4);
  }
  return 0;
}

static void *synthetic_resource_class(unsigned hash) {
  static const unsigned known_safe[] = {
      0xb4502910u, 0xc8e42197u, 0x6097ed50u, 0x207e2246u,
      0xc084661du, 0x164fd9d6u,
  };
  int allowed = 0;
  if (getenv("PES_CLASS_SYNTH_ALL")) {
    allowed = factory_table_has_hash(hash);
  } else {
    for (unsigned i = 0; i < sizeof(known_safe) / sizeof(known_safe[0]); i++)
      if (hash == known_safe[i])
        allowed = 1;
  }
  if (!hash || !g_resource_class_vtable || !allowed)
    return NULL;

  enum { MAX_SYNTH = 32, CLASS_ENTRY_SIZE = 48 };
  static unsigned char entries[MAX_SYNTH][CLASS_ENTRY_SIZE];
  static void *loaded_vecs[MAX_SYNTH][8];
  static unsigned hashes[MAX_SYNTH];
  static unsigned used;

  for (unsigned i = 0; i < used; i++)
    if (hashes[i] == hash)
      return entries[i];
  if (used >= MAX_SYNTH)
    return NULL;

  unsigned char *entry = entries[used];
  memset(entry, 0, CLASS_ENTRY_SIZE);
  *(uintptr_t *)(entry + 0) = g_resource_class_vtable;
  *(unsigned *)(entry + 4) = hash;
  int make_placeholders = getenv("PES_SYNTH_CTOR_PLACEHOLDERS") ||
      (hash == 0xc084661du && getenv("PES_SYNTH_C084_PLACEHOLDERS"));
  uintptr_t ctor = make_placeholders ? factory_ctor_for_hash(hash) : 0;
  unsigned loaded_count = 0;
  if (ctor && ptr_range_mapped((void *)(ctor & ~(uintptr_t)1u), 2)) {
    typedef void *(*ctor0_fn)(void);
    ctor0_fn Ctor = (ctor0_fn)(ctor | 1u);
    for (unsigned i = 0; i < sizeof(loaded_vecs[0]) / sizeof(loaded_vecs[0][0]);
         i++) {
      void *obj = Ctor();
      if (!obj || !ptr_range_mapped(obj, 8))
        break;
      *(unsigned *)((char *)obj + 4) = 0;
      loaded_vecs[used][loaded_count++] = obj;
    }
  }
  if (loaded_count > 0) {
    *(uintptr_t *)(entry + 12) = (uintptr_t)loaded_vecs[used];
    *(unsigned *)(entry + 16) = loaded_count;
    *(unsigned *)(entry + 20) = loaded_count;
  }
  hashes[used] = hash;
  used++;
  debugPrintf("CLASS_SYNTH: hash=0x%08x -> entry=%p vt=%p ctor=%p placeholders=%u\n",
              hash, entry, (void *)g_resource_class_vtable, (void *)ctor,
              loaded_count);
  return entry;
}

static void *w_find_resource_class(void *registry, unsigned hash) {
  if ((uintptr_t)registry == 0x10000000u) {
    static int d = 0;
    if (d < 20) {
      debugPrintf("NULLREG: find_resource_class(registry=0x10000000, hash=0x%08x) sentinel -> NULL\n",
                  hash);
      d++;
    }
    return NULL;
  }
  if (!ptr_range_mapped(registry, 24)) {
    snapshot_maps();
  }
  if (!ptr_range_mapped(registry, 24)) {
    static int d = 0;
    if (d < 20) {
      debugPrintf("NULLREG: find_resource_class(registry=%p, hash=0x%08x) invalid -> NULL\n",
                  registry, hash);
      d++;
    }
    return NULL;
  }

  uintptr_t entries = *(uintptr_t *)((char *)registry + 16);
  unsigned count = *(unsigned *)((char *)registry + 20);
  if (count > 0) {
    size_t bytes = (size_t)count * 48u;
    if (count > 0x10000 || bytes / 48u != count ||
        !ptr_range_mapped((void *)entries, bytes)) {
      snapshot_maps();
      if (count > 0x10000 || bytes / 48u != count ||
          !ptr_range_mapped((void *)entries, bytes)) {
        static int d = 0;
        if (d < 20) {
          debugPrintf("NULLREG: find_resource_class registry=%p entries=%p count=%u invalid -> NULL\n",
                      registry, (void *)entries, count);
          d++;
        }
        return NULL;
      }
    }
  }

  void *ret = real_find_resource_class ?
      real_find_resource_class(registry, hash) : NULL;
  if (entries && count > 0 && ptr_range_mapped((void *)entries, 4))
    g_resource_class_vtable = *(uintptr_t *)entries;
  remember_resource_classes(entries, count);
  dump_class_registry_once(registry, entries, count);
  if (!ret)
    scan_class_hash_once(hash);
  if (!ret)
    ret = seen_resource_class(hash);
  if (!ret && getenv("PES_CLASS_SYNTH"))
    ret = synthetic_resource_class(hash);
  if (!ret && getenv("PES_CLASS_FALLBACK_FIRST") && entries && count > 0 &&
      ptr_range_mapped((void *)entries, 48)) {
    ret = (void *)entries;
  }
  static int ok_logs = 0;
  if (ok_logs < 120) {
    unsigned k0 = 0, k1 = 0, k2 = 0, k3 = 0;
    if (entries && count > 0 && ptr_range_mapped((void *)entries, 48))
      k0 = *(unsigned *)((char *)entries + 4);
    if (entries && count > 1 && ptr_range_mapped((void *)(entries + 48), 48))
      k1 = *(unsigned *)((char *)entries + 48 + 4);
    if (entries && count > 2 && ptr_range_mapped((void *)(entries + 96), 48))
      k2 = *(unsigned *)((char *)entries + 96 + 4);
    if (entries && count > 3 && ptr_range_mapped((void *)(entries + 144), 48))
      k3 = *(unsigned *)((char *)entries + 144 + 4);
    debugPrintf("NULLREG: find_resource_class(registry=%p, hash=0x%08x entries=%p count=%u keys=%08x,%08x,%08x,%08x) -> %p\n",
                registry, hash, (void *)entries, count, k0, k1, k2, k3,
                ret);
    ok_logs++;
  }
  return ret;
}

static void *fake_music_group_object(void);

static void *w_find_resource_outer(void *registry, unsigned a1, unsigned a2,
                                   unsigned flags) {
  if (!registry || (uintptr_t)registry == 0x10000000u ||
      !ptr_range_mapped(registry, 40)) {
    snapshot_maps();
  }
  if (!registry || (uintptr_t)registry == 0x10000000u ||
      !ptr_range_mapped(registry, 40)) {
    if (getenv("PES_FAKE_MUSIC_GROUP") && a2 == 0x164fd9d6u) {
      void *ret = fake_music_group_object();
      static int music_logs = 0;
      if (music_logs < 40) {
        debugPrintf("MUSICSKIP: fake outer registry=%p res=0x%08x class=0x%08x flags=0x%08x -> %p\n",
                    registry, a1, a2, flags, ret);
        music_logs++;
      }
      return ret;
    }
    static int d = 0;
    if (d < 20) {
      debugPrintf("NULLREG: find_resource_outer(registry=%p, a1=0x%08x, a2=0x%08x, flags=0x%08x) invalid -> NULL\n",
                  registry, a1, a2, flags);
      d++;
    }
    return NULL;
  }
  return real_find_resource_outer ?
      real_find_resource_outer(registry, a1, a2, flags) : NULL;
}

static void *w_find_resource_full(void *group, unsigned res_hash,
                                  unsigned class_hash, unsigned flags,
                                  unsigned opt) {
  g_last_tagged_mgr = NULL;
  g_last_tagged_hash = 0;
  g_last_tagged_obj = NULL;
  void *ret = real_find_resource_full ?
      real_find_resource_full(group, res_hash, class_hash, flags, opt) : NULL;
  int allow_return = getenv("PES_RETURN_TAGGED_RES_ALL") ||
      (getenv("PES_RETURN_TAGGED_RES") && class_hash == 0xc084661du);
  if (!ret && allow_return &&
      g_last_tagged_hash == res_hash && g_last_tagged_obj &&
      ptr_range_mapped(g_last_tagged_obj, 8)) {
    static int d = 0;
    if (d < 80) {
      debugPrintf("RESOURCE_FULL: returning tagged obj=%p group=%p class=0x%08x res=0x%08x mgr=%p flags=0x%x opt=0x%x\n",
                  g_last_tagged_obj, group, class_hash, res_hash,
                  g_last_tagged_mgr, flags, opt);
      d++;
    }
    return g_last_tagged_obj;
  }
  if (!ret && getenv("PES_FAKE_MUSIC_GROUP") &&
      class_hash == 0x164fd9d6u) {
    ret = fake_music_group_object();
    static int d = 0;
    if (d < 40) {
      debugPrintf("MUSICSKIP: fake CMusicGroup group=%p class=0x%08x res=0x%08x -> %p\n",
                  group, class_hash, res_hash, ret);
      d++;
    }
  }
  return ret;
}

static void *fake_music_group_object(void) {
  static void *items[1];
  static unsigned char info[0x40];
  static unsigned char group[0x20];
  static int init;
  if (!init) {
    memset(info, 0, sizeof(info));
    memset(group, 0, sizeof(group));
    float one = 1.0f;
    memcpy(info + 0x10, &one, sizeof(one));
    info[0x14] = 0;
    items[0] = info;
    *(void **)(group + 0x10) = items;
    *(unsigned *)(group + 0x14) = 1;
    *(unsigned *)(group + 0x18) = 1;
    init = 1;
  }
  return group;
}

static int is_fake_music_info(void *p) {
  void *group = fake_music_group_object();
  void **items = *(void ***)((char *)group + 0x10);
  return items && p == items[0];
}

static void *w_music_play_info(void *controller, void *info, unsigned volume,
                               unsigned a3) {
  if (getenv("PES_SKIP_MUSIC_PLAY") || is_fake_music_info(info) ||
      !info || !ptr_range_mapped(info, 0x1c)) {
    static int d = 0;
    if (d < 40) {
      debugPrintf("MUSICSKIP: play controller=%p info=%p volume=0x%x -> no-op\n",
                  controller, info, volume);
      d++;
    }
    return controller;
  }
  void *name_obj = *(void **)((char *)info + 0x18);
  if (!name_obj || !ptr_range_mapped((char *)name_obj + 0x1c, 8)) {
    static int d = 0;
    if (d < 40) {
      debugPrintf("MUSICSKIP: play info=%p bad-name=%p volume=0x%x -> no-op\n",
                  info, name_obj, volume);
      d++;
    }
    return controller;
  }
  return real_music_play_info ?
      real_music_play_info(controller, info, volume, a3) : controller;
}

static void *resource_slot_bad_return(void) {
  if (getenv("PES_SLOT_NULL_RET_NULL"))
    return NULL;
  return g_resource_slot_fallback;
}

static void *w_resource_index_apply(void *obj, unsigned index, unsigned value,
                                    unsigned a3) {
  (void)a3;
  if (getenv("PES_FAKE_MUSIC_GROUP") && obj == fake_music_group_object()) {
    static int d = 0;
    if (d < 40) {
      debugPrintf("MUSICSKIP: fake resource_index_apply(obj=%p, index=%u, value=0x%x) -> obj\n",
                  obj, index, value);
      d++;
    }
    return obj;
  }
  if (!obj || (uintptr_t)obj == 0x10000000u ||
      !ptr_range_mapped(obj, 20)) {
    static int d = 0;
    if (d < 40) {
      debugPrintf("NULLREG: resource_index_apply(obj=%p, index=%u, value=0x%x) invalid -> NULL\n",
                  obj, index, value);
      d++;
    }
    return NULL;
  }
  void *items = *(void **)((char *)obj + 16);
  if (!items || !ptr_range_mapped(items, (size_t)(index + 1) * sizeof(void *))) {
    static int d = 0;
    if (d < 40) {
      debugPrintf("NULLREG: resource_index_apply(obj=%p, index=%u, items=%p, value=0x%x) bad-items -> NULL\n",
                  obj, index, items, value);
      d++;
    }
    return NULL;
  }
  return real_resource_index_apply ?
      real_resource_index_apply(obj, index, value, a3) : NULL;
}

static int pes_resource_current_group_valid(void) {
  uintptr_t game_base = g_class_diag_game_base;
  if (!game_base)
    return 1;
  uintptr_t lit = game_base + 0xcaa04u;
  if (!ptr_range_mapped((void *)lit, sizeof(uint32_t)))
    return 1;
  uintptr_t holder = (uintptr_t)*(uint32_t *)lit;
  if (!holder || !ptr_range_mapped((void *)holder, sizeof(void *)))
    return 0;
  void *root = *(void **)holder;
  if (!root || !ptr_range_mapped(root, 0x68))
    return 0;
  void *cur = *(void **)((char *)root + 0x64);
  return cur && ptr_range_mapped(cur, 0x10);
}

static void *w_resource_group_update(void *obj) {
  if (!pes_resource_current_group_valid()) {
    static int d = 0;
    if (d < 40) {
      debugPrintf("NULLREG: resource_group_update(obj=%p) current-group NULL -> skip\n",
                  obj);
      d++;
    }
    return obj;
  }
  return real_resource_group_update ? real_resource_group_update(obj) : obj;
}

static void *w_resource_slot(void *obj, int index, int a2, int a3) {
  if (!obj || (uintptr_t)obj == 0x10000000u || !ptr_range_mapped(obj, 136)) {
    snapshot_maps();
  }
  if (!obj || (uintptr_t)obj == 0x10000000u || !ptr_range_mapped(obj, 136)) {
    void *ret = resource_slot_bad_return();
    static int d = 0;
    if (d < 30) {
      debugPrintf("NULLREG: resource_slot(obj=%p, index=%d, a2=0x%x, a3=0x%x) invalid -> %p\n",
                  obj, index, a2, a3, ret);
      d++;
    }
    return ret;
  }
  return real_resource_slot ? real_resource_slot(obj, index, a2, a3) :
                              resource_slot_bad_return();
}

static int w_resource_flags(void *obj, unsigned mask, unsigned a2,
                            unsigned a3) {
  static int d = 0;
  if (d < 40) {
    debugPrintf("NULLREG: resource_flags(obj=%p, mask=0x%x, a2=0x%x, a3=0x%x) -> 0\n",
                obj, mask, a2, a3);
    d++;
  }
  if (getenv("PES_RESFLAGS_REAL") && obj && ptr_range_mapped(obj, 48))
    return real_resource_flags ? real_resource_flags(obj, mask, a2, a3) : 0;
  return 0;
}

static void *w_resource_field32(void *obj) {
  if (!obj || !ptr_range_mapped(obj, 36)) {
    snapshot_maps();
  }
  if (!obj || !ptr_range_mapped(obj, 36)) {
    static int d = 0;
    if (d < 40) {
      debugPrintf("NULLREG: resource_field32(obj=%p) invalid -> NULL\n", obj);
      d++;
    }
    return NULL;
  }
  return *(void **)((char *)obj + 32);
}

static void w_resource_bind(void *obj, void *res, unsigned a2, unsigned a3) {
  if (!obj || !res || !ptr_range_mapped(obj, 48) ||
      !ptr_range_mapped(res, 16)) {
    snapshot_maps();
  }
  if (!obj || !res || !ptr_range_mapped(obj, 48) ||
      !ptr_range_mapped(res, 16)) {
    static int d = 0;
    if (d < 40) {
      debugPrintf("NULLREG: resource_bind(obj=%p, res=%p, a2=0x%x, a3=0x%x) invalid -> skip\n",
                  obj, res, a2, a3);
      d++;
    }
    return;
  }
  if (real_resource_bind)
    real_resource_bind(obj, res, a2, a3);
}

static void w_resource_set90(void *obj, void *val) {
  if (!obj || !ptr_range_mapped(obj, 0x94)) {
    snapshot_maps();
  }
  if (!obj || !ptr_range_mapped(obj, 0x94)) {
    static int d = 0;
    if (d < 40) {
      debugPrintf("NULLREG: resource_set90(obj=%p, val=%p) invalid -> skip\n",
                  obj, val);
      d++;
    }
    return;
  }
  *(void **)((char *)obj + 0x90) = val;
}

static void *w_resource_parent_ctor(void *a0, void *parent, unsigned a2,
                                    unsigned a3) {
  if (!parent || !ptr_range_mapped(parent, 8)) {
    snapshot_maps();
  }
  if (!parent || !ptr_range_mapped(parent, 8)) {
    static int d = 0;
    if (d < 40) {
      debugPrintf("NULLREG: resource_parent_ctor(a0=%p, parent=%p, a2=0x%x, a3=0x%x) invalid -> default\n",
                  a0, parent, a2, a3);
      d++;
    }
    return real_resource_default_ctor ? real_resource_default_ctor() : NULL;
  }
  return real_resource_parent_ctor ?
      real_resource_parent_ctor(a0, parent, a2, a3) : NULL;
}

static void *w_resource_small_ctor(void *obj) {
  if (!obj) {
    obj = calloc(1, 0x100);
    static int d = 0;
    if (d < 40) {
      debugPrintf("NULLREG: resource_small_ctor(obj=NULL) -> alloc %p size=0x100\n",
                  obj);
      d++;
    }
  }
  if (!obj)
    return NULL;
  return real_resource_small_ctor ? real_resource_small_ctor(obj) : obj;
}

static void *dummy_resource_for_key(unsigned key, void *template_obj) {
  enum { MAX_DUMMY_RES = 32, DUMMY_RES_SIZE = 0x100 };
  static struct {
    unsigned key;
    void *ptr;
  } cache[MAX_DUMMY_RES];
  static int next_slot;
  static void *last_template_obj;

  for (int i = 0; i < MAX_DUMMY_RES; i++)
    if (cache[i].ptr && cache[i].key == key)
      return cache[i].ptr;

  int slot = next_slot++ % MAX_DUMMY_RES;
  if (template_obj && ptr_range_mapped(template_obj, 0x40)) {
    last_template_obj = template_obj;
    cache[slot].key = key;
    cache[slot].ptr = template_obj;
    return template_obj;
  }
  if (last_template_obj && ptr_range_mapped(last_template_obj, 0x40)) {
    cache[slot].key = key;
    cache[slot].ptr = last_template_obj;
    return last_template_obj;
  }

  void *obj = calloc(1, DUMMY_RES_SIZE);
  if (!obj)
    return NULL;
  *(unsigned *)((char *)obj + 4) = key;
  *(unsigned *)((char *)obj + 0x0c) = 0;

  cache[slot].key = key;
  cache[slot].ptr = obj;
  return obj;
}

static void *w_resource_array_find(void *list, unsigned key) {
  if (key == 0x9b83d7fdu) {
    static int skip_logs = 0;
    if (skip_logs < 40) {
      debugPrintf("NULLREG: resource_array_find(list=%p, key=0x%x) remove-probe -> NULL\n",
                  list, key);
      skip_logs++;
    }
    return NULL;
  }

  if (getenv("PES_ARRAY_FIND_NULL")) {
    static int null_logs = 0;
    if (null_logs < 80) {
      debugPrintf("NULLREG: resource_array_find(list=%p, key=0x%x) forced NULL\n",
                  list, key);
      null_logs++;
    }
    return NULL;
  }

  if (!list || !ptr_range_mapped(list, 8)) {
    snapshot_maps();
  }
  if (!list || !ptr_range_mapped(list, 8)) {
    static int d = 0;
    if (getenv("PES_ARRAY_FIND_INVALID_NULL")) {
      if (d < 40) {
        debugPrintf("NULLREG: resource_array_find(list=%p, key=0x%x) invalid -> NULL\n",
                    list, key);
        d++;
      }
      return NULL;
    }
    void *dummy = dummy_resource_for_key(key, NULL);
    if (d < 40) {
      debugPrintf("NULLREG: resource_array_find(list=%p, key=0x%x) invalid -> dummy %p\n",
                  list, key, dummy);
      d++;
    }
    return dummy;
  }

  void **items = *(void ***)list;
  unsigned count = *(unsigned *)((char *)list + 4);
  if (count > 0x10000u || (count && !items)) {
    snapshot_maps();
  }
  size_t bytes = (size_t)count * sizeof(void *);
  if (count > 0x10000u || (count && bytes / sizeof(void *) != count) ||
      (count && !ptr_range_mapped(items, bytes))) {
    snapshot_maps();
    if (count > 0x10000u || (count && bytes / sizeof(void *) != count) ||
        (count && !ptr_range_mapped(items, bytes))) {
      void *dummy = dummy_resource_for_key(key, NULL);
      static int bad_logs = 0;
      if (bad_logs < 40) {
        debugPrintf("NULLREG: resource_array_find(list=%p, items=%p, count=%u, key=0x%x) bad list -> dummy %p\n",
                    list, items, count, key, dummy);
        bad_logs++;
      }
      return dummy;
    }
  }

  for (unsigned i = 0; i < count; i++) {
    void *item = items[i];
    if (!item || !ptr_range_mapped(item, 8))
      continue;
    unsigned item_key = *(unsigned *)((char *)item + 4);
    if (item_key == key) {
      static int hit_logs = 0;
      if (hit_logs < 80) {
        debugPrintf("NULLREG: resource_array_find(list=%p, key=0x%x) real-hit[%u]=%p\n",
                    list, key, i, item);
        hit_logs++;
      }
      return item;
    }
  }

  static int miss_logs = 0;
  if (miss_logs < 80) {
    debugPrintf("NULLREG: resource_array_find(list=%p, items=%p, count=%u, key=0x%x) real-miss -> NULL\n",
                list, items, count, key);
    miss_logs++;
  }
  if (getenv("PES_ARRAY_FIND_DUMMY_ON_MISS"))
    return dummy_resource_for_key(key, NULL);
  return NULL;
}

static int w_resource_call8(void *obj) {
  static int d = 0;
  if (d < 80) {
    debugPrintf("NULLREG: resource_call8(obj=%p) -> 0\n", obj);
    d++;
  }
  if (getenv("PES_CALL8_REAL") && real_resource_call8)
    return real_resource_call8(obj);
  return 0;
}

static void w_resource_iter28(void *obj) {
  if (!obj || !ptr_range_mapped(obj, 0x30)) {
    snapshot_maps();
  }
  if (!obj || !ptr_range_mapped(obj, 0x30)) {
    static int d = 0;
    if (d < 80) {
      debugPrintf("NULLREG: resource_iter28(obj=%p) invalid -> skip\n", obj);
      d++;
    }
    return;
  }
  if (real_resource_iter28)
    real_resource_iter28(obj);
}

static void *w_resource_self3c(void *self) {
  if (!self || !ptr_range_mapped(self, 0x44)) {
    snapshot_maps();
  }
  if (!self || !ptr_range_mapped(self, 0x44)) {
    static int d = 0;
    if (d < 80) {
      debugPrintf("NULLREG: resource_self3c(self=%p) invalid -> NULL\n", self);
      d++;
    }
    return NULL;
  }
  return real_resource_self3c ? real_resource_self3c(self) : self;
}

static int w_resource_self34(void *self) {
  if (!self || !ptr_range_mapped(self, 0x38)) {
    snapshot_maps();
  }
  if (!self || !ptr_range_mapped(self, 0x38)) {
    static int d = 0;
    if (d < 80) {
      debugPrintf("NULLREG: resource_self34(self=%p) invalid -> 0\n", self);
      d++;
    }
    return 0;
  }
  return real_resource_self34 ? real_resource_self34(self) : 0;
}

static int w_resource_self8(void *self) {
  if (!self || !ptr_range_mapped(self, 9)) {
    snapshot_maps();
  }
  if (!self || !ptr_range_mapped(self, 9)) {
    static int d = 0;
    if (d < 80) {
      debugPrintf("NULLREG: resource_self8(self=%p) invalid -> 0\n", self);
      d++;
    }
    return 0;
  }
  return real_resource_self8 ? real_resource_self8(self) : 0;
}

static int w_resource_self14(void *self) {
  if (!self || !ptr_range_mapped(self, 0x18)) {
    snapshot_maps();
  }
  if (!self || !ptr_range_mapped(self, 0x18)) {
    static int d = 0;
    if (d < 80) {
      debugPrintf("NULLREG: resource_self14(self=%p) invalid -> 0\n", self);
      d++;
    }
    return 0;
  }
  return real_resource_self14 ? real_resource_self14(self) : 0;
}

static int w_resource_self1c(void *self) {
  if (!self || !ptr_range_mapped(self, 0x20)) {
    snapshot_maps();
  }
  if (!self || !ptr_range_mapped(self, 0x20)) {
    static int d = 0;
    if (d < 80) {
      debugPrintf("NULLREG: resource_self1c(self=%p) invalid -> 0\n", self);
      d++;
    }
    return 0;
  }
  return real_resource_self1c ? real_resource_self1c(self) : 0;
}

static void w_scoreloop_sync(void *self, void *request) {
  if (getenv("PES_SCORELOOP_SKIP_ALL")) {
    static int d = 0;
    if (d < 80) {
      debugPrintf("SCGUARD: scoreloop_sync(self=%p, request=%p) -> skip-all\n",
                  self, request);
      d++;
    }
    return;
  }
  if (!self || !ptr_range_mapped(self, 0x2c)) {
    snapshot_maps();
  }
  if (!self || !ptr_range_mapped(self, 0x2c)) {
    static int d = 0;
    if (d < 80) {
      debugPrintf("SCGUARD: scoreloop_sync(self=%p, request=%p) invalid -> skip\n",
                  self, request);
      d++;
    }
    return;
  }
  if (real_scoreloop_sync)
    real_scoreloop_sync(self, request);
}

static void *w_class_factory_find(void *mgr, unsigned hash) {
  void *lr;
  __asm__ volatile("mov %0, lr" : "=r"(lr));
  void *ret = real_class_factory_find ?
      real_class_factory_find(mgr, hash) : (void *)0xffffffffu;
  static int logs = 0;
  if (logs < 160) {
    unsigned count = 0;
    uintptr_t entries = 0;
    if (mgr && ptr_range_mapped(mgr, 0x24)) {
      entries = *(uintptr_t *)((char *)mgr + 0x1c);
      count = *(unsigned *)((char *)mgr + 0x20);
    }
    unsigned k0 = 0, k1 = 0, k2 = 0, k3 = 0;
    uintptr_t v0 = 0, v1 = 0, v2 = 0, v3 = 0;
    if (entries && count > 0 && ptr_range_mapped((void *)entries, 8)) {
      k0 = *(unsigned *)(entries + 0);
      v0 = *(uintptr_t *)(entries + 4);
    }
    if (entries && count > 1 && ptr_range_mapped((void *)(entries + 8), 8)) {
      k1 = *(unsigned *)(entries + 8);
      v1 = *(uintptr_t *)(entries + 12);
    }
    if (entries && count > 2 && ptr_range_mapped((void *)(entries + 16), 8)) {
      k2 = *(unsigned *)(entries + 16);
      v2 = *(uintptr_t *)(entries + 20);
    }
    if (entries && count > 3 && ptr_range_mapped((void *)(entries + 24), 8)) {
      k3 = *(unsigned *)(entries + 24);
      v3 = *(uintptr_t *)(entries + 28);
    }
    uintptr_t lra = (uintptr_t)lr & ~(uintptr_t)1u;
    unsigned lro = g_class_diag_game_base && lra >= g_class_diag_game_base ?
        (unsigned)(lra - g_class_diag_game_base) : 0;
    debugPrintf("CLASS_FACTORY: lookup hash=0x%08x mgr=%p entries=%p count=%u keys=%08x,%08x,%08x,%08x vals=%p,%p,%p,%p lr=%p off=0x%x -> %p\n",
                hash, mgr, (void *)entries, count, k0, k1, k2, k3,
                (void *)v0, (void *)v1, (void *)v2, (void *)v3, lr,
                lro, ret);
    logs++;
  }
  if (getenv("PES_DUMP_RESOURCE_ENTRY") && ret == (void *)0xffffffffu &&
      mgr && ptr_range_mapped(mgr, 48)) {
    enum { MAX_DUMPED = 24 };
    static void *dumped_mgr[MAX_DUMPED];
    static unsigned dumped_hash[MAX_DUMPED];
    static unsigned dumped_count;
    int seen = 0;
    for (unsigned i = 0; i < dumped_count; i++)
      if (dumped_mgr[i] == mgr && dumped_hash[i] == hash)
        seen = 1;
    if (!seen) {
      if (dumped_count < MAX_DUMPED) {
        dumped_mgr[dumped_count] = mgr;
        dumped_hash[dumped_count] = hash;
        dumped_count++;
      }
      uint32_t w[12];
      memcpy(w, mgr, sizeof(w));
      debugPrintf("RESOURCE_ENTRY: miss hash=0x%08x mgr=%p words=%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                  hash, mgr, w[0], w[1], w[2], w[3], w[4], w[5], w[6],
                  w[7], w[8], w[9], w[10], w[11]);
      uintptr_t loaded = w[3];
      unsigned loaded_count = w[4];
      uintptr_t lazy = w[7];
      unsigned lazy_count = w[8];
      debugPrintf("RESOURCE_ENTRY: loaded=%p count=%u cap=%u lazy=%p lazy_count=%u\n",
                  (void *)loaded, loaded_count, w[5], (void *)lazy,
                  lazy_count);
      for (unsigned i = 0; i < loaded_count && i < 12; i++) {
        uintptr_t slot = loaded + (uintptr_t)i * sizeof(uintptr_t);
        if (!ptr_range_mapped((void *)slot, sizeof(uintptr_t)))
          break;
        uintptr_t obj = *(uintptr_t *)slot;
        unsigned obj_hash = 0;
        uintptr_t vt = 0;
        if (obj && ptr_range_mapped((void *)obj, 16)) {
          vt = *(uintptr_t *)obj;
          obj_hash = *(unsigned *)(obj + 4);
        }
        debugPrintf("RESOURCE_ENTRY:  loaded[%u] slot=%p obj=%p vt=%p hash=0x%08x\n",
                    i, (void *)slot, (void *)obj, (void *)vt, obj_hash);
      }
      for (unsigned i = 0; i < lazy_count && i < 16; i++) {
        uintptr_t e = lazy + (uintptr_t)i * 8u;
        if (!ptr_range_mapped((void *)e, 8))
          break;
        debugPrintf("RESOURCE_ENTRY:  lazy[%u] hash=0x%08x off=0x%08x\n",
                    i, *(unsigned *)e, *(unsigned *)(e + 4));
      }
    }
  }
  if (getenv("PES_TAG_PLACEHOLDER_RES") && ret == (void *)0xffffffffu &&
      hash && mgr && ptr_range_mapped(mgr, 24)) {
    uintptr_t loaded = *(uintptr_t *)((char *)mgr + 12);
    unsigned loaded_count = *(unsigned *)((char *)mgr + 16);
    if (loaded && loaded_count <= 256 &&
        ptr_range_mapped((void *)loaded,
                         (size_t)loaded_count * sizeof(uintptr_t))) {
      for (unsigned i = 0; i < loaded_count; i++) {
        uintptr_t slot = loaded + (uintptr_t)i * sizeof(uintptr_t);
        uintptr_t obj = *(uintptr_t *)slot;
        if (!obj || !ptr_range_mapped((void *)obj, 8))
          continue;
        unsigned *obj_hash = (unsigned *)(obj + 4);
        if (*obj_hash == hash)
          break;
        if (*obj_hash == 0) {
          *obj_hash = hash;
          g_last_tagged_mgr = mgr;
          g_last_tagged_hash = hash;
          g_last_tagged_obj = (void *)obj;
          debugPrintf("RESOURCE_ENTRY: tag placeholder mgr=%p slot=%u obj=%p hash=0x%08x\n",
                      mgr, i, (void *)obj, hash);
          break;
        }
      }
    }
  }
  return ret;
}

static void *w_class_factory_make(unsigned hash) {
  void *lr;
  __asm__ volatile("mov %0, lr" : "=r"(lr));
  void *ret = real_class_factory_make ?
      real_class_factory_make(hash) : NULL;
  static int logs = 0;
  if (logs < 160 || !ret) {
    uintptr_t lra = (uintptr_t)lr & ~(uintptr_t)1u;
    unsigned lro = g_class_diag_game_base && lra >= g_class_diag_game_base ?
        (unsigned)(lra - g_class_diag_game_base) : 0;
    uintptr_t vt = 0;
    if (ret && ptr_range_mapped(ret, 4))
      vt = *(uintptr_t *)ret;
    debugPrintf("CLASS_FACTORY: make hash=0x%08x lr=%p off=0x%x -> %p vt=%p\n",
                hash, lr, lro, ret, (void *)vt);
    logs++;
  }
  return ret;
}

static int fake_gx_font_ret0(void *a, void *b, void *c, void *d) {
  (void)a; (void)b; (void)c; (void)d;
  return 0;
}

static int fake_gx_font_glyph(void *out, void *ctx, void *data, unsigned ch) {
  (void)ctx; (void)data; (void)ch;
  if (out && ptr_range_mapped(out, 8))
    memset(out, 0, 8);
  return 0;
}

static void *fake_gx_font_ctx(void) {
  static uintptr_t vt[8];
  static uintptr_t obj;
  if (!vt[2]) {
    vt[2] = (uintptr_t)fake_gx_font_ret0;   /* vtable +0x08 */
    vt[3] = (uintptr_t)fake_gx_font_ret0;   /* vtable +0x0c */
    vt[5] = (uintptr_t)fake_gx_font_ret0;   /* vtable +0x14 */
    vt[6] = (uintptr_t)fake_gx_font_ret0;   /* vtable +0x18 */
    vt[7] = (uintptr_t)fake_gx_font_glyph;  /* vtable +0x1c */
    obj = (uintptr_t)vt;
  }
  return &obj;
}

static void *w_gx_font_ctx(void) {
  void *ret = real_gx_font_ctx ? real_gx_font_ctx() : NULL;
  if (!ret && getenv("PES_FAKE_GX_FONT_CTX")) {
    ret = fake_gx_font_ctx();
    static int d = 0;
    if (d < 40) {
      debugPrintf("GXFONT: real ctx NULL -> fake %p\n", ret);
      d++;
    }
  }
  return ret;
}

static void dump_factory_table_once(uintptr_t class_factory_make) {
  if (!getenv("PES_DUMP_FACTORY_TABLE"))
    return;

  static int done;
  if (done)
    return;
  done = 1;

  snapshot_maps();
  uintptr_t lit_addr = class_factory_make + 0x24u; /* ldr literal in a4acc */
  if (!ptr_range_mapped((void *)lit_addr, sizeof(uint32_t))) {
    debugPrintf("CLASS_FACTORY: table literal unmapped @%p\n",
                (void *)lit_addr);
    return;
  }

  uintptr_t holder = (uintptr_t)*(uint32_t *)lit_addr;
  if (!ptr_range_mapped((void *)holder, sizeof(uintptr_t))) {
    debugPrintf("CLASS_FACTORY: table holder unmapped literal=%p holder=%p\n",
                (void *)lit_addr, (void *)holder);
    return;
  }

  uintptr_t table = *(uintptr_t *)holder;
  if (!ptr_range_mapped((void *)table, 8)) {
    debugPrintf("CLASS_FACTORY: table header unmapped holder=%p table=%p\n",
                (void *)holder, (void *)table);
    return;
  }

  uintptr_t entries = *(uintptr_t *)table;
  unsigned count = *(unsigned *)(table + 4);
  debugPrintf("CLASS_FACTORY: table holder=%p table=%p entries=%p count=%u\n",
              (void *)holder, (void *)table, (void *)entries, count);
  if (!entries || count > 512 ||
      !ptr_range_mapped((void *)entries, (size_t)count * 12u)) {
    debugPrintf("CLASS_FACTORY: table entries invalid entries=%p count=%u\n",
                (void *)entries, count);
    return;
  }

  static const unsigned interesting[] = {
      0xb4502910u, 0xc8e42197u, 0x6097ed50u, 0x207e2246u,
      0xd85096b8u, 0x3521f539u, 0xfce10f4cu, 0x990313adu,
  };
  for (unsigned i = 0; i < count; i++) {
    uintptr_t e = entries + (uintptr_t)i * 12u;
    unsigned hash = *(unsigned *)e;
    uintptr_t ctor = *(uintptr_t *)(e + 4);
    uintptr_t aux = *(uintptr_t *)(e + 8);
    int mark = 0;
    for (unsigned j = 0; j < sizeof(interesting) / sizeof(interesting[0]); j++)
      if (hash == interesting[j])
        mark = 1;
    if (mark || getenv("PES_DUMP_FACTORY_TABLE_ALL")) {
      debugPrintf("CLASS_FACTORY: table[%u] hash=0x%08x ctor=%p aux=%p%s\n",
                  i, hash, (void *)ctor, (void *)aux, mark ? " *" : "");
    }
  }
}

static uintptr_t decode_arm_b_target(uintptr_t addr) {
  uint32_t ins = *(uint32_t *)addr;
  if ((ins & 0x0e000000u) != 0x0a000000u)
    return 0;
  int32_t imm = (int32_t)(ins & 0x00ffffffu);
  if (imm & 0x00800000)
    imm |= (int32_t)0xff000000u;
  int32_t disp = imm * 4;
  return (uintptr_t)((intptr_t)addr + 8 + disp);
}

static void install_game_file_import_hooks(uintptr_t game_base) {
  if (!game_base || getenv("PES_NO_GAME_FILE_IMPORTS"))
    return;
  if (g_game_file_imports_base == game_base)
    return;

  if (getenv("PES_DUMP_GAME_FILE_IMPORTS")) {
    FILE *f = fopen("/storage/roms/ports/pes2012/gamefile-submount.bin", "wb");
    if (f) {
      fwrite((void *)(game_base + 0xf7240u), 1, 0x500, f);
      fclose(f);
      debugPrintf("GAMEFILE: dump submount @%p len=0x500\n",
                  (void *)(game_base + 0xf7240u));
    }
    FILE *g = fopen("/storage/roms/ports/pes2012/gamefile-imports.bin", "wb");
    if (g) {
      fwrite((void *)(game_base + 0x2da40u), 1, 0x80, g);
      fclose(g);
      debugPrintf("GAMEFILE: dump imports @%p len=0x80\n",
                  (void *)(game_base + 0x2da40u));
    }
  }

  struct {
    uintptr_t off;
    uintptr_t fn;
    const char *name;
  } hooks[] = {
      {0x2da4cu, (uintptr_t)my_s3eFileClose, "s3eFileClose"},
      {0x2da58u, (uintptr_t)my_s3eFileGetChar, "s3eFileGetChar"},
      {0x2da80u, (uintptr_t)my_s3eFileOpen, "s3eFileOpen"},
      {0x2da8cu, (uintptr_t)my_s3eFileRead, "s3eFileRead"},
      {0x2da90u, (uintptr_t)my_s3eFileSeek, "s3eFileSeek"},
  };

  for (unsigned i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
    uintptr_t stub = game_base + hooks[i].off;
    uintptr_t addr = decode_arm_b_target(stub);
    if (!addr)
      addr = stub;
    if (make_exec_page_writable(addr, 16) == 0) {
      hook_arm(addr, hooks[i].fn);
      __builtin___clear_cache((char *)addr, (char *)addr + 16);
      debugPrintf("GAMEFILE: hook %s import stub=%p thunk=%p (game_base=%p)\n",
                  hooks[i].name, (void *)stub, (void *)addr, (void *)game_base);
      if (getenv("PES_GAMEFILE_VERIFY")) {
        uint32_t *sw = (uint32_t *)stub;
        uint32_t *tw = (uint32_t *)addr;
        debugPrintf("GAMEFILE: verify %s stub=%p words=%08x thunk=%p words=%08x %08x dst=%p\n",
                    hooks[i].name, (void *)stub, sw[0], (void *)addr,
                    tw[0], tw[1], (void *)hooks[i].fn);
      }
      if (hooks[i].off == 0x2da8cu)
        g_gamefile_read_import_addr = addr;
    }
  }
  long ps = sysconf(_SC_PAGESIZE);
  if (ps <= 0)
    ps = 4096;
  uintptr_t ilo = (game_base + 0x2da40u) & ~((uintptr_t)ps - 1);
  __builtin___clear_cache((char *)ilo, (char *)(ilo + (uintptr_t)ps));
  if (getenv("PES_GAMEFILE_VERIFY"))
    debugPrintf("GAMEFILE: flushed import page %p..%p\n", (void *)ilo,
                (void *)(ilo + (uintptr_t)ps));
  g_game_file_imports_base = game_base;
}

static void *w_sound_group_load(void *mgr, unsigned idx) {
  if (getenv("PES_SKIP_SOUND_GROUPS") && !getenv("PES_SOUND_REAL") &&
      idx < 6) {
    static const char *names[6] = {
        "crowdAmbient", "crowdAmbientLoop", "crowdEvent",
        "crowdInstrument", "ingame", "menu"};
    if (mgr && ptr_range_mapped((char *)mgr + 12 + idx * 4, sizeof(void *)))
      *(void **)((char *)mgr + 12 + idx * 4) = NULL;
    static int d = 0;
    if (d < 24) {
      debugPrintf("SOUNDSKIP: group idx=%u (%s) mgr=%p -> NULL\n", idx,
                  names[idx], mgr);
      d++;
    }
    return NULL;
  }
  return real_sound_group_load ? real_sound_group_load(mgr, idx) : NULL;
}

static void install_sound_group_skip_guard(uintptr_t cbs0) {
  if (!cbs0 || !getenv("PES_SKIP_SOUND_GROUPS") || getenv("PES_SOUND_REAL") ||
      getenv("PES_NO_SOUND_SKIP"))
    return;

  uintptr_t sub_open = (cbs0 & ~1u) - 0x1577c8u;
  uintptr_t game_base = sub_open - 0xf7490u;
  uintptr_t load_sound_group = game_base + 0x11f6fcu;

  install_game_file_import_hooks(game_base);

  if (g_sound_group_load_addr == load_sound_group)
    return;

  snapshot_maps();
  if (!ptr_range_mapped((void *)load_sound_group, 16)) {
    debugPrintf("SOUNDSKIP: loader nao mapeado @%p (base=%p sub_open=%p)\n",
                (void *)load_sound_group, (void *)game_base,
                (void *)sub_open);
    return;
  }

  if (make_exec_page_writable(load_sound_group, 16) == 0) {
    real_sound_group_load =
        (sound_group_load_fn)make_arm_trampoline(load_sound_group);
    if (real_sound_group_load) {
      hook_arm(load_sound_group, (uintptr_t)w_sound_group_load);
      __builtin___clear_cache((char *)load_sound_group,
                              (char *)load_sound_group + 16);
      g_sound_group_load_addr = load_sound_group;
      debugPrintf("SOUNDSKIP: hook sound group loader @%p (game_base=%p tramp=%p)\n",
                  (void *)load_sound_group, (void *)game_base,
                  (void *)real_sound_group_load);
    } else {
      debugPrintf("SOUNDSKIP: falha criando trampoline loader=%p\n",
                  (void *)load_sound_group);
    }
  }
}

static void install_null_registry_guard(uintptr_t cbs0) {
  /* s18: os guards NULLREG do s17 CAUSAM crashes (neutralizam funções reais de
   * carregamento de recurso, ex. resource_call8 retornando 0 p/ objetos válidos).
   * Com eles OFF o jogo renderiza "Installing..." e só trava no sound group, sem
   * crashar. Agora são OPT-IN (PES_NULLREG_PATCH=1); off por padrão. */
  if (!cbs0 || !getenv("PES_NULLREG_PATCH") || getenv("PES_NO_NULLREG_PATCH"))
    return;

  snapshot_maps();
  uintptr_t sub_open = (cbs0 & ~1u) - 0x1577c8u;
  uintptr_t game_base = sub_open - 0xf7490u;
  install_game_file_import_hooks(game_base);
  uintptr_t small_ctor = sub_open - 0x2d054u;
  uintptr_t self8 = sub_open - 0xb4408u;
  uintptr_t self14 = sub_open - 0xb2808u;
  uintptr_t self1c = sub_open - 0xb2178u;
  uintptr_t self34 = sub_open - 0xb4774u;
  uintptr_t self3c = sub_open - 0xb4930u;
  uintptr_t parent_ctor = sub_open + 0x25424u;
  uintptr_t default_ctor = sub_open + 0x25460u;
  uintptr_t iter28 = sub_open - 0x18710u;
  uintptr_t set90 = sub_open - 0x127a8u;
  uintptr_t bind = sub_open - 0x114fcu;
  uintptr_t slot = sub_open + 0x1558bcu;
  uintptr_t call8 = sub_open + 0x16f064u;
  uintptr_t flags = sub_open + 0xcfb38u;
  uintptr_t field32 = sub_open + 0xcfc8cu;
  uintptr_t array_find = sub_open + 0xd21c6u;
  uintptr_t outer = sub_open + 0xce17cu;
  uintptr_t target = sub_open + 0xce2acu;
  g_class_diag_game_base = game_base;
  g_audio_diag_game_base = game_base;
  uintptr_t class_factory_make = game_base + 0xa4accu;
  uintptr_t class_factory_find = game_base + 0xca5e0u;
  uintptr_t find_resource_full = game_base + 0x1c560cu;
  uintptr_t gx_font_ctx = game_base + 0xa7518u;
  uintptr_t resource_group_update = game_base + 0xca71cu;
  uintptr_t resource_index_apply = game_base + 0xdc760u;
  uintptr_t music_play_info = game_base + 0x1aa7c8u;
  uintptr_t scoreloop_sync = game_base + 0x46b34u;
  g_class_factory_make_plain = class_factory_make;
  dump_factory_table_once(class_factory_make);
  if (getenv("PES_DUMP_RESOURCE_CRASH")) {
    uintptr_t dump = (sub_open - 0xb0a00u) & ~(uintptr_t)3;
    FILE *f = fopen("/storage/roms/ports/pes2012/resource-crash.bin", "wb");
    if (f) {
      fwrite((void *)dump, 1, 0x800, f);
      fclose(f);
      debugPrintf("NULLREG: dump resource crash area @%p len=0x800\n",
                  (void *)dump);
    }
    struct {
      const char *name;
      uintptr_t addr;
      size_t len;
    } dumps[] = {
        {"resource-array-find.bin", array_find - 0x100u, 0x500u},
        {"resource-find-class.bin", target - 0x100u, 0x500u},
        {"resource-outer.bin", outer - 0x100u, 0x500u},
    };
    for (unsigned i = 0; i < sizeof(dumps) / sizeof(dumps[0]); i++) {
      char path[256];
      snprintf(path, sizeof(path), "/storage/roms/ports/pes2012/%s",
               dumps[i].name);
      FILE *df = fopen(path, "wb");
      if (df) {
        fwrite((void *)dumps[i].addr, 1, dumps[i].len, df);
        fclose(df);
        debugPrintf("NULLREG: dump %s @%p len=0x%zx\n", dumps[i].name,
                    (void *)dumps[i].addr, dumps[i].len);
      }
    }
  }
  uintptr_t thumb_flags = flags | 1u;
  if (g_resource_flags_addr != thumb_flags) {
    if (make_exec_page_writable(flags, 16) == 0) {
      real_resource_flags =
          (resource_flags_fn)make_thumb_tramp_full(thumb_flags);
      if (real_resource_flags) {
        hook_arm(thumb_flags, (uintptr_t)w_resource_flags);
        __builtin___clear_cache((char *)flags, (char *)flags + 16);
        g_resource_flags_addr = thumb_flags;
        debugPrintf("NULLREG: hook resource_flags @%p (sub_open=%p tramp=%p)\n",
                    (void *)thumb_flags, (void *)sub_open,
                    (void *)real_resource_flags);
      } else {
        debugPrintf("NULLREG: falha criando trampoline resource_flags=%p\n",
                    (void *)thumb_flags);
      }
    }
  }

  if (g_resource_self34_addr != self34) {
    if (make_exec_page_writable(self34, 16) == 0) {
      real_resource_self34 =
          (resource_self34_fn)make_arm_trampoline(self34);
      if (real_resource_self34) {
        hook_arm(self34, (uintptr_t)w_resource_self34);
        __builtin___clear_cache((char *)self34, (char *)self34 + 16);
        g_resource_self34_addr = self34;
        debugPrintf("NULLREG: hook resource_self34 @%p (sub_open=%p tramp=%p)\n",
                    (void *)self34, (void *)sub_open,
                    (void *)real_resource_self34);
      } else {
        debugPrintf("NULLREG: falha criando trampoline resource_self34=%p\n",
                    (void *)self34);
      }
    }
  }

  if (g_resource_self8_addr != self8) {
    if (make_exec_page_writable(self8, 16) == 0) {
      real_resource_self8 =
          (resource_self8_fn)make_arm_trampoline(self8);
      if (real_resource_self8) {
        hook_arm(self8, (uintptr_t)w_resource_self8);
        __builtin___clear_cache((char *)self8, (char *)self8 + 16);
        g_resource_self8_addr = self8;
        debugPrintf("NULLREG: hook resource_self8 @%p (sub_open=%p tramp=%p)\n",
                    (void *)self8, (void *)sub_open,
                    (void *)real_resource_self8);
      } else {
        debugPrintf("NULLREG: falha criando trampoline resource_self8=%p\n",
                    (void *)self8);
      }
    }
  }

  if (g_resource_self14_addr != self14) {
    if (make_exec_page_writable(self14, 16) == 0) {
      real_resource_self14 =
          (resource_self14_fn)make_arm_trampoline(self14);
      if (real_resource_self14) {
        hook_arm(self14, (uintptr_t)w_resource_self14);
        __builtin___clear_cache((char *)self14, (char *)self14 + 16);
        g_resource_self14_addr = self14;
        debugPrintf("NULLREG: hook resource_self14 @%p (sub_open=%p tramp=%p)\n",
                    (void *)self14, (void *)sub_open,
                    (void *)real_resource_self14);
      } else {
        debugPrintf("NULLREG: falha criando trampoline resource_self14=%p\n",
                    (void *)self14);
      }
    }
  }

  if (g_resource_self1c_addr != self1c) {
    if (make_exec_page_writable(self1c, 16) == 0) {
      real_resource_self1c =
          (resource_self1c_fn)make_arm_trampoline(self1c);
      if (real_resource_self1c) {
        hook_arm(self1c, (uintptr_t)w_resource_self1c);
        __builtin___clear_cache((char *)self1c, (char *)self1c + 16);
        g_resource_self1c_addr = self1c;
        debugPrintf("NULLREG: hook resource_self1c @%p (sub_open=%p tramp=%p)\n",
                    (void *)self1c, (void *)sub_open,
                    (void *)real_resource_self1c);
      } else {
        debugPrintf("NULLREG: falha criando trampoline resource_self1c=%p\n",
                    (void *)self1c);
      }
    }
  }

  if (g_scoreloop_sync_addr != scoreloop_sync) {
    if (make_exec_page_writable(scoreloop_sync, 16) == 0) {
      real_scoreloop_sync =
          (scoreloop_sync_fn)make_arm_trampoline(scoreloop_sync);
      if (real_scoreloop_sync) {
        hook_arm(scoreloop_sync, (uintptr_t)w_scoreloop_sync);
        __builtin___clear_cache((char *)scoreloop_sync,
                                (char *)scoreloop_sync + 16);
        g_scoreloop_sync_addr = scoreloop_sync;
        debugPrintf("SCGUARD: hook scoreloop_sync @%p (game_base=%p tramp=%p)\n",
                    (void *)scoreloop_sync, (void *)game_base,
                    (void *)real_scoreloop_sync);
      } else {
        debugPrintf("SCGUARD: falha criando trampoline scoreloop_sync=%p\n",
                    (void *)scoreloop_sync);
      }
    }
  }

  if (g_resource_self3c_addr != self3c) {
    if (make_exec_page_writable(self3c, 16) == 0) {
      real_resource_self3c =
          (resource_self3c_fn)make_arm_trampoline(self3c);
      if (real_resource_self3c) {
        hook_arm(self3c, (uintptr_t)w_resource_self3c);
        __builtin___clear_cache((char *)self3c, (char *)self3c + 16);
        g_resource_self3c_addr = self3c;
        debugPrintf("NULLREG: hook resource_self3c @%p (sub_open=%p tramp=%p)\n",
                    (void *)self3c, (void *)sub_open,
                    (void *)real_resource_self3c);
      } else {
        debugPrintf("NULLREG: falha criando trampoline resource_self3c=%p\n",
                    (void *)self3c);
      }
    }
  }

  if (g_resource_iter28_addr != iter28) {
    if (make_exec_page_writable(iter28, 16) == 0) {
      real_resource_iter28 =
          (resource_iter28_fn)make_arm_trampoline(iter28);
      if (real_resource_iter28) {
        hook_arm(iter28, (uintptr_t)w_resource_iter28);
        __builtin___clear_cache((char *)iter28, (char *)iter28 + 16);
        g_resource_iter28_addr = iter28;
        debugPrintf("NULLREG: hook resource_iter28 @%p (sub_open=%p tramp=%p)\n",
                    (void *)iter28, (void *)sub_open,
                    (void *)real_resource_iter28);
      } else {
        debugPrintf("NULLREG: falha criando trampoline resource_iter28=%p\n",
                    (void *)iter28);
      }
    }
  }

  uintptr_t thumb_call8 = call8 | 1u;
  if (g_resource_call8_addr != thumb_call8) {
    if (make_exec_page_writable(call8, 16) == 0) {
      real_resource_call8 =
          (resource_call8_fn)make_thumb_tramp_full(thumb_call8);
      if (real_resource_call8) {
        hook_arm(thumb_call8, (uintptr_t)w_resource_call8);
        __builtin___clear_cache((char *)call8, (char *)call8 + 16);
        g_resource_call8_addr = thumb_call8;
        debugPrintf("NULLREG: hook resource_call8 @%p (sub_open=%p tramp=%p)\n",
                    (void *)thumb_call8, (void *)sub_open,
                    (void *)real_resource_call8);
      } else {
        debugPrintf("NULLREG: falha criando trampoline resource_call8=%p\n",
                    (void *)thumb_call8);
      }
    }
  }

  uintptr_t thumb_array_find = array_find | 1u;
  if (g_resource_array_find_addr != thumb_array_find) {
    if (make_exec_page_writable(array_find, 16) == 0) {
      real_resource_array_find = NULL;
      hook_thumb_abs_any_align(thumb_array_find,
                               (uintptr_t)w_resource_array_find);
      __builtin___clear_cache((char *)array_find,
                              (char *)array_find + 16);
      g_resource_array_find_addr = thumb_array_find;
      debugPrintf("NULLREG: hook resource_array_find @%p (sub_open=%p stub any-align)\n",
                  (void *)thumb_array_find, (void *)sub_open);
    }
  }

  uintptr_t thumb_small_ctor = small_ctor | 1u;
  if (g_resource_small_ctor_addr != thumb_small_ctor) {
    if (make_exec_page_writable(small_ctor, 16) == 0) {
      real_resource_small_ctor =
          (resource_small_ctor_fn)make_thumb_tramp_full(thumb_small_ctor);
      if (real_resource_small_ctor) {
        hook_arm(thumb_small_ctor, (uintptr_t)w_resource_small_ctor);
        __builtin___clear_cache((char *)small_ctor,
                                (char *)small_ctor + 16);
        g_resource_small_ctor_addr = thumb_small_ctor;
        debugPrintf("NULLREG: hook resource_small_ctor @%p (sub_open=%p tramp=%p)\n",
                    (void *)thumb_small_ctor, (void *)sub_open,
                    (void *)real_resource_small_ctor);
      } else {
        debugPrintf("NULLREG: falha criando trampoline resource_small_ctor=%p\n",
                    (void *)thumb_small_ctor);
      }
    }
  }

  uintptr_t thumb_parent_ctor = parent_ctor | 1u;
  if (g_resource_parent_ctor_addr != thumb_parent_ctor) {
    real_resource_default_ctor = (resource_default_ctor_fn)(default_ctor | 1u);
    if (make_exec_page_writable(parent_ctor, 16) == 0) {
      real_resource_parent_ctor =
          (resource_parent_ctor_fn)make_thumb_tramp_full(thumb_parent_ctor);
      if (real_resource_parent_ctor) {
        hook_arm(thumb_parent_ctor, (uintptr_t)w_resource_parent_ctor);
        __builtin___clear_cache((char *)parent_ctor,
                                (char *)parent_ctor + 16);
        g_resource_parent_ctor_addr = thumb_parent_ctor;
        debugPrintf("NULLREG: hook resource_parent_ctor @%p default=%p (sub_open=%p tramp=%p)\n",
                    (void *)thumb_parent_ctor,
                    (void *)(default_ctor | 1u), (void *)sub_open,
                    (void *)real_resource_parent_ctor);
      } else {
        debugPrintf("NULLREG: falha criando trampoline resource_parent_ctor=%p\n",
                    (void *)thumb_parent_ctor);
      }
    }
  }

  uintptr_t thumb_set90 = set90 | 1u;
  if (g_resource_set90_addr != thumb_set90) {
    if (make_exec_page_writable(set90, 8) == 0) {
      hook_arm(thumb_set90, (uintptr_t)w_resource_set90);
      __builtin___clear_cache((char *)set90, (char *)set90 + 8);
      g_resource_set90_addr = thumb_set90;
      debugPrintf("NULLREG: hook resource_set90 @%p (sub_open=%p)\n",
                  (void *)thumb_set90, (void *)sub_open);
    }
  }

  uintptr_t thumb_bind = bind | 1u;
  if (g_resource_bind_addr != thumb_bind) {
    if (make_exec_page_writable(bind, 16) == 0) {
      real_resource_bind =
          (resource_bind_fn)make_thumb_tramp_full(thumb_bind);
      if (real_resource_bind) {
        hook_arm(thumb_bind, (uintptr_t)w_resource_bind);
        __builtin___clear_cache((char *)bind, (char *)bind + 16);
        g_resource_bind_addr = thumb_bind;
        debugPrintf("NULLREG: hook resource_bind @%p (sub_open=%p tramp=%p)\n",
                    (void *)thumb_bind, (void *)sub_open,
                    (void *)real_resource_bind);
      } else {
        debugPrintf("NULLREG: falha criando trampoline resource_bind=%p\n",
                    (void *)thumb_bind);
      }
    }
  }

  uintptr_t thumb_field32 = field32 | 1u;
  if (g_resource_field32_addr != thumb_field32) {
    if (make_exec_page_writable(field32, 8) == 0) {
      hook_arm(thumb_field32, (uintptr_t)w_resource_field32);
      __builtin___clear_cache((char *)field32, (char *)field32 + 8);
      g_resource_field32_addr = thumb_field32;
      debugPrintf("NULLREG: hook resource_field32 @%p (sub_open=%p)\n",
                  (void *)thumb_field32, (void *)sub_open);
    }
  }

  if (g_resource_slot_addr != slot) {
    uintptr_t fallback_lit = slot + 0x4cu;
    g_resource_slot_fallback = NULL;
    if (ptr_range_mapped((void *)fallback_lit, sizeof(uintptr_t))) {
      void *fallback = (void *)(uintptr_t)*(uint32_t *)fallback_lit;
      if (!fallback || ptr_range_mapped(fallback, 4))
        g_resource_slot_fallback = fallback;
    }
    if (make_exec_page_writable(slot, 16) == 0) {
      uint32_t first = *(uint32_t *)slot;
      uint32_t second = *(uint32_t *)(slot + 4);
      int arm_like = (first == 0xe3500000u || first == 0xe3500001u ||
                      second == 0xe92d4010u || second == 0xe92d4030u);
      real_resource_slot =
          (resource_slot_fn)(arm_like ? make_arm_trampoline(slot) :
                                      make_thumb_tramp_full(slot | 1u));
      if (real_resource_slot) {
        hook_arm(arm_like ? slot : (slot | 1u), (uintptr_t)w_resource_slot);
        __builtin___clear_cache((char *)slot, (char *)slot + 16);
        g_resource_slot_addr = slot;
        debugPrintf("NULLREG: hook resource_slot @%p mode=%s fallback=%p tramp=%p\n",
                    (void *)(arm_like ? slot : (slot | 1u)),
                    arm_like ? "ARM" : "Thumb", g_resource_slot_fallback,
                    (void *)real_resource_slot);
      } else {
        debugPrintf("NULLREG: falha criando trampoline resource_slot=%p\n",
                    (void *)slot);
      }
    }
  }

  if (g_resource_index_apply_addr != resource_index_apply) {
    if (!ptr_range_mapped((void *)resource_index_apply, 16))
      snapshot_maps();
    if (!ptr_range_mapped((void *)resource_index_apply, 16)) {
      debugPrintf("NULLREG: resource_index_apply nao mapeado @%p (game_base=%p sub_open=%p)\n",
                  (void *)resource_index_apply, (void *)game_base,
                  (void *)sub_open);
    } else if (make_exec_page_writable(resource_index_apply, 16) == 0) {
      real_resource_index_apply =
          (resource_index_apply_fn)make_arm_trampoline(resource_index_apply);
      if (real_resource_index_apply) {
        hook_arm(resource_index_apply, (uintptr_t)w_resource_index_apply);
        __builtin___clear_cache((char *)resource_index_apply,
                                (char *)resource_index_apply + 16);
        g_resource_index_apply_addr = resource_index_apply;
        debugPrintf("NULLREG: hook resource_index_apply @%p (game_base=%p tramp=%p)\n",
                    (void *)resource_index_apply, (void *)game_base,
                    (void *)real_resource_index_apply);
      } else {
        debugPrintf("NULLREG: falha criando trampoline resource_index_apply=%p\n",
                    (void *)resource_index_apply);
      }
    }
  }

  uintptr_t thumb_group_update = resource_group_update | 1u;
  if (getenv("PES_NULL_GROUP_GUARD") &&
      g_resource_group_update_addr != thumb_group_update) {
    if (!ptr_range_mapped((void *)resource_group_update, 16))
      snapshot_maps();
    if (!ptr_range_mapped((void *)resource_group_update, 16)) {
      debugPrintf("NULLREG: resource_group_update nao mapeado @%p (game_base=%p sub_open=%p)\n",
                  (void *)resource_group_update, (void *)game_base,
                  (void *)sub_open);
    } else if (make_exec_page_writable(resource_group_update, 16) == 0) {
      real_resource_group_update =
          (resource_group_update_fn)make_thumb_tramp_full(thumb_group_update);
      if (real_resource_group_update) {
        hook_arm(thumb_group_update, (uintptr_t)w_resource_group_update);
        __builtin___clear_cache((char *)resource_group_update,
                                (char *)resource_group_update + 16);
        g_resource_group_update_addr = thumb_group_update;
        debugPrintf("NULLREG: hook resource_group_update @%p (game_base=%p tramp=%p)\n",
                    (void *)thumb_group_update, (void *)game_base,
                    (void *)real_resource_group_update);
      } else {
        debugPrintf("NULLREG: falha criando trampoline resource_group_update=%p\n",
                    (void *)thumb_group_update);
      }
    }
  }

  if ((getenv("PES_SKIP_MUSIC_PLAY") || getenv("PES_FAKE_MUSIC_GROUP")) &&
      g_music_play_info_addr != music_play_info) {
    if (!ptr_range_mapped((void *)music_play_info, 16))
      snapshot_maps();
    if (!ptr_range_mapped((void *)music_play_info, 16)) {
      debugPrintf("MUSICSKIP: play-info nao mapeado @%p (game_base=%p sub_open=%p)\n",
                  (void *)music_play_info, (void *)game_base,
                  (void *)sub_open);
    } else if (make_exec_page_writable(music_play_info, 16) == 0) {
      real_music_play_info =
          (music_play_info_fn)make_arm_trampoline(music_play_info);
      if (real_music_play_info) {
        hook_arm(music_play_info, (uintptr_t)w_music_play_info);
        __builtin___clear_cache((char *)music_play_info,
                                (char *)music_play_info + 16);
        g_music_play_info_addr = music_play_info;
        debugPrintf("MUSICSKIP: hook play-info @%p (game_base=%p tramp=%p)\n",
                    (void *)music_play_info, (void *)game_base,
                    (void *)real_music_play_info);
      } else {
        debugPrintf("MUSICSKIP: falha criando trampoline play-info=%p\n",
                    (void *)music_play_info);
      }
    }
  }

  uintptr_t thumb_outer = outer | 1u;
  uintptr_t thumb_target = target | 1u;
  uintptr_t thumb_full = find_resource_full | 1u;
  uintptr_t thumb_gx_font_ctx = gx_font_ctx | 1u;
  uintptr_t thumb_factory_make = class_factory_make | 1u;
  uintptr_t thumb_factory = class_factory_find | 1u;
  if (getenv("PES_CLASS_FACTORY_MAKE_LOG") &&
      g_class_factory_make_addr != thumb_factory_make) {
    if (!ptr_range_mapped((void *)class_factory_make, 16))
      snapshot_maps();
    if (!ptr_range_mapped((void *)class_factory_make, 16)) {
      debugPrintf("CLASS_FACTORY: make nao mapeado @%p (game_base=%p sub_open=%p)\n",
                  (void *)class_factory_make, (void *)game_base,
                  (void *)sub_open);
    } else if (make_exec_page_writable(class_factory_make, 16) == 0) {
      real_class_factory_make =
          (class_factory_make_fn)make_thumb_tramp_full(thumb_factory_make);
      if (real_class_factory_make) {
        hook_arm(thumb_factory_make, (uintptr_t)w_class_factory_make);
        __builtin___clear_cache((char *)class_factory_make,
                                (char *)class_factory_make + 16);
        g_class_factory_make_addr = thumb_factory_make;
        debugPrintf("CLASS_FACTORY: hook make @%p (game_base=%p tramp=%p)\n",
                    (void *)thumb_factory_make, (void *)game_base,
                    (void *)real_class_factory_make);
      } else {
        debugPrintf("CLASS_FACTORY: falha criando trampoline make=%p\n",
                    (void *)thumb_factory_make);
      }
    }
  }

  if (getenv("PES_CLASS_FACTORY_LOG") &&
      g_class_factory_find_addr != thumb_factory) {
    if (!ptr_range_mapped((void *)class_factory_find, 16))
      snapshot_maps();
    if (!ptr_range_mapped((void *)class_factory_find, 16)) {
      debugPrintf("CLASS_FACTORY: lookup nao mapeado @%p (game_base=%p sub_open=%p)\n",
                  (void *)class_factory_find, (void *)game_base,
                  (void *)sub_open);
    } else if (make_exec_page_writable(class_factory_find, 16) == 0) {
      real_class_factory_find =
          (class_factory_find_fn)make_thumb_tramp_full(thumb_factory);
      if (real_class_factory_find) {
        hook_arm(thumb_factory, (uintptr_t)w_class_factory_find);
        __builtin___clear_cache((char *)class_factory_find,
                                (char *)class_factory_find + 16);
        g_class_factory_find_addr = thumb_factory;
        debugPrintf("CLASS_FACTORY: hook lookup @%p (game_base=%p tramp=%p)\n",
                    (void *)thumb_factory, (void *)game_base,
                    (void *)real_class_factory_find);
      } else {
        debugPrintf("CLASS_FACTORY: falha criando trampoline lookup=%p\n",
                    (void *)thumb_factory);
      }
    }
  }

  if (getenv("PES_RETURN_TAGGED_RES") &&
      g_find_resource_full_addr != thumb_full) {
    if (!ptr_range_mapped((void *)find_resource_full, 16))
      snapshot_maps();
    if (!ptr_range_mapped((void *)find_resource_full, 16)) {
      debugPrintf("RESOURCE_FULL: lookup nao mapeado @%p (game_base=%p sub_open=%p)\n",
                  (void *)find_resource_full, (void *)game_base,
                  (void *)sub_open);
    } else if (make_exec_page_writable(find_resource_full, 16) == 0) {
      real_find_resource_full =
          (find_resource_full_fn)make_thumb_tramp_full(thumb_full);
      if (real_find_resource_full) {
        hook_arm(thumb_full, (uintptr_t)w_find_resource_full);
        __builtin___clear_cache((char *)find_resource_full,
                                (char *)find_resource_full + 16);
        g_find_resource_full_addr = thumb_full;
        debugPrintf("RESOURCE_FULL: hook lookup @%p (game_base=%p tramp=%p)\n",
                    (void *)thumb_full, (void *)game_base,
                    (void *)real_find_resource_full);
      } else {
        debugPrintf("RESOURCE_FULL: falha criando trampoline full=%p\n",
                    (void *)thumb_full);
      }
    }
  }

  if (getenv("PES_FAKE_GX_FONT_CTX") &&
      g_gx_font_ctx_addr != thumb_gx_font_ctx) {
    if (!ptr_range_mapped((void *)gx_font_ctx, 8))
      snapshot_maps();
    if (!ptr_range_mapped((void *)gx_font_ctx, 8)) {
      debugPrintf("GXFONT: ctx getter nao mapeado @%p (game_base=%p sub_open=%p)\n",
                  (void *)gx_font_ctx, (void *)game_base, (void *)sub_open);
    } else if (make_exec_page_writable(gx_font_ctx, 16) == 0) {
      real_gx_font_ctx = (gx_font_ctx_fn)make_thumb_tramp_full(thumb_gx_font_ctx);
      if (real_gx_font_ctx) {
        hook_arm(thumb_gx_font_ctx, (uintptr_t)w_gx_font_ctx);
        __builtin___clear_cache((char *)gx_font_ctx,
                                (char *)gx_font_ctx + 16);
        g_gx_font_ctx_addr = thumb_gx_font_ctx;
        debugPrintf("GXFONT: hook ctx getter @%p (game_base=%p tramp=%p)\n",
                    (void *)thumb_gx_font_ctx, (void *)game_base,
                    (void *)real_gx_font_ctx);
      } else {
        debugPrintf("GXFONT: falha criando trampoline ctx=%p\n",
                    (void *)thumb_gx_font_ctx);
      }
    }
  }

  if (g_find_resource_outer_addr != thumb_outer) {
    if (make_exec_page_writable(outer, 16) == 0) {
      real_find_resource_outer =
          (find_resource_outer_fn)make_thumb_tramp_full(thumb_outer);
      if (real_find_resource_outer) {
        hook_arm(thumb_outer, (uintptr_t)w_find_resource_outer);
        __builtin___clear_cache((char *)outer, (char *)outer + 16);
        g_find_resource_outer_addr = thumb_outer;
        debugPrintf("NULLREG: hook find_resource_outer @%p (sub_open=%p tramp=%p)\n",
                    (void *)thumb_outer, (void *)sub_open,
                    (void *)real_find_resource_outer);
      } else {
        debugPrintf("NULLREG: falha criando trampoline outer=%p\n",
                    (void *)thumb_outer);
      }
    }
  }

  if (g_find_resource_class_addr == thumb_target)
    return;
  if (make_exec_page_writable(target, 16) != 0)
    return;
  real_find_resource_class =
      (find_resource_class_fn)make_thumb_tramp_full(thumb_target);
  if (!real_find_resource_class) {
    debugPrintf("NULLREG: falha criando trampoline target=%p\n",
                (void *)thumb_target);
    return;
  }
  hook_arm(thumb_target, (uintptr_t)w_find_resource_class);
  __builtin___clear_cache((char *)target, (char *)target + 16);
  g_find_resource_class_addr = thumb_target;
  debugPrintf("NULLREG: hook find_resource_class @%p (sub_open=%p cbs0=%p tramp=%p)\n",
              (void *)thumb_target, (void *)sub_open, (void *)cbs0,
              (void *)real_find_resource_class);
}
/* logger de cbs[0] (Open do user-fs): chamado PELO worker (contexto certo) ->
 * mostra o filename que o sub_open recebe e o resultado. */
static void *(*g_orig_cbs0)(const char *, const char *);
static void *ufs_open_logger(const char *fn, const char *mode) {
  void *h = g_orig_cbs0 ? g_orig_cbs0(fn, mode) : NULL;
  static int d = 0;
  if (d < 40) { debugPrintf("UFSOPEN '%s' mode='%s' -> %p\n", fn ? fn : "(null)",
                            mode ? mode : "?", h); d++; }
  return h;
}
static int my_addufs_diag(void *cbs, void *ud) {
  /* lê count ANTES do registro (do dispatcher cbs[0], literal @+0x30) */
  if (cbs) {
    uintptr_t *p = (uintptr_t *)cbs;
    uintptr_t base = p[0] & ~1u;
    uintptr_t global = *(uintptr_t *)(base + 0x30);
    debugPrintf("ADDUFS pre: global=%p count=%u\n", (void *)global,
                *(unsigned int *)(global + 4));
  }
  int r = real_addufs ? real_addufs(cbs, ud) : -1;
  if (cbs) {
    uintptr_t *p = (uintptr_t *)cbs;
    uintptr_t base = p[0] & ~1u;
    uintptr_t global = *(uintptr_t *)(base + 0x30);
    debugPrintf("ADDUFS post: global=%p count=%u r=%d\n", (void *)global,
                *(unsigned int *)(global + 4), r);
  }
  g_ufs_cbs = cbs; g_ufs_ud = ud;
  if (cbs) { /* COPIA os valores AGORA (a struct fica dangling depois) */
    uintptr_t *p = (uintptr_t *)cbs;
    for (int i = 0; i < 8; i++) g_cbs_copy[i] = p[i];
    g_cbs_ok = 1;
    debugPrintf("USERFS: copiado cbs[0]=%p cbs[1]=%p cbs[3]=%p cbs[4]=%p cbs[5]=%p\n",
                (void *)g_cbs_copy[0], (void *)g_cbs_copy[1], (void *)g_cbs_copy[3],
                (void *)g_cbs_copy[4], (void *)g_cbs_copy[5]);
    install_sound_group_skip_guard(g_cbs_copy[0]);
    install_null_registry_guard(g_cbs_copy[0]);
  }
  g_archive_ready = 1; /* archive registrado; a extração roda no 1o open real */
  if (cbs && getenv("PES_UFSLOG")) {
    uintptr_t *p = (uintptr_t *)cbs;
    g_orig_cbs0 = (void *(*)(const char *, const char *))p[0];
    p[0] = (uintptr_t)ufs_open_logger;
    debugPrintf("UFSLOG: cbs[0] hookado (orig=%p)\n", (void *)g_orig_cbs0);
  }
  debugPrintf("USERFS: s3eFileAddUserFileSys(cbs=%p ud=%p) -> %d\n", cbs, ud, r);
  if (cbs && getenv("PES_DUMP_UFS")) {
    uintptr_t *p = (uintptr_t *)cbs;
    for (int i = 0; i < 8; i++)
      debugPrintf("USERFS: cbs[%d] = %p\n", i, (void *)p[i]);
    FILE *f = fopen("/storage/roms/ports/pes2012/ufsopen.bin", "wb");
    if (f && p[0]) { fwrite((void *)(p[0] & ~1u), 1, 0x200, f); fclose(f); }
    FILE *g = fopen("/storage/roms/ports/pes2012/ufsread.bin", "wb");
    if (g && p[1]) { fwrite((void *)(p[1] & ~1u), 1, 0x80, g); fclose(g); }
    /* worker read+decrypt = cbs[1] - 0x1576fc (bl na Read callback) */
    uintptr_t worker = (p[1] & ~1u) - 0x1576fc;
    FILE *h = fopen("/storage/roms/ports/pes2012/ufswork.bin", "wb");
    if (h) { fwrite((void *)worker, 1, 0x800, h); fclose(h); }
    debugPrintf("USERFS: dumps ok cbs[1]=%p worker=%p\n", (void *)p[1],
                (void *)worker);
  }
  return r;
}

/* PES: hooka os s3eFile* EXPORTADOS (por simbolo) -> nossas impls libc. O modulo
   XE3U chama a API s3e via o loader; substituindo os entrypoints exportados, todo
   acesso a arquivo (PES2012.s3e, OBB, saves) passa pelo nosso fopen. */
static void install_s3efile_exports(void) {
  /* captura sempre o user-fs do archive (Open/Read descriptografam) p/ o
   * extract-on-demand chamar as callbacks direto. */
  if (getenv("PES_NATIVE_ARCHIVE")) {
    uintptr_t au = so_find_addr_safe("s3eFileAddUserFileSys");
    if (au) {
      real_addufs = (int (*)(void *, void *))(
          (au & 1) ? make_thumb_tramp_full(au) : make_arm_trampoline(au));
      hook_arm(au, (uintptr_t)my_addufs_diag);
    }
  }
  if (getenv("PES_NO_FILESHIM")) return;
  struct { const char *sym; void *fn; } m[] = {
    {"s3eFileOpen",           (void *)my_s3eFileOpen},
    {"s3eFileOpenFromMemory", (void *)my_s3eFileOpenFromMemory},
    {"s3eFileClose",          (void *)my_s3eFileClose},
    {"s3eFileGetChar",        (void *)my_s3eFileGetChar},
    {"s3eFileRead",           (void *)my_s3eFileRead},
    {"s3eFileWrite",          (void *)my_s3eFileWrite},
    {"s3eFileSeek",           (void *)my_s3eFileSeek},
    {"s3eFileTell",           (void *)my_s3eFileTell},
    {"s3eFileGetSize",        (void *)my_s3eFileGetSize},
    {"s3eFileGetError",       (void *)my_s3eFileGetError},
    {"s3eFileFlush",          (void *)my_s3eFileFlush},
    {"s3eFileCheckExists",    (void *)my_s3eFileCheckExists},
    {"s3eFileReadString",     (void *)my_s3eFileReadString},
    {"s3eFileListDirectory",  (void *)my_s3eFileListDirectory},
    {"s3eFileListNext",       (void *)my_s3eFileListNext},
    {"s3eFileListClose",      (void *)my_s3eFileListClose},
  };
  /* TLSMAP: o archive_open (na thread de loading) usa s3eThreadLocal; sem o mapa,
   * getspecific vem NULL -> crash. Habilita p/ o PES também. */
  if (getenv("PES_NATIVE_ARCHIVE") && !getenv("PES_NO_TLSMAP")) {
    uintptr_t b = g_main_text_base ? g_main_text_base : (uintptr_t)text_base;
    hook_arm(b + 0x82d60, (uintptr_t)my_s3e_tls_get);
    hook_arm(b + 0x82d70, (uintptr_t)my_s3e_tls_set);
    debugPrintf("ARCHIVE: TLSMAP (s3eThreadLocal) habilitado p/ PES\n");
  }
  /* ANTES de sobrescrever: captura trampolines das funções s3eFile REAIS p/
   * extrair-sob-demanda os assets cifrados do OBB (o VFS interno descriptografa).
   * thumb (bit0=1) vs ARM. */
  if (getenv("PES_NATIVE_ARCHIVE")) {
#define TRAMP(sym) ({ uintptr_t _a = so_find_addr_safe(sym); \
    (_a & 1) ? make_thumb_tramp_full(_a) : make_arm_trampoline(_a); })
    real_s3eFileOpen = (void *(*)(const char *, const char *))TRAMP("s3eFileOpen");
    real_s3eFileRead =
        (int (*)(void *, unsigned int, unsigned int, void *))TRAMP("s3eFileRead");
    real_s3eFileClose = (int (*)(void *))TRAMP("s3eFileClose");
    real_s3eFileGetSize = (unsigned int (*)(void *))TRAMP("s3eFileGetSize");
    real_s3eFileSeek = (int (*)(void *, int, int))TRAMP("s3eFileSeek");
    real_s3eFileTell = (int (*)(void *))TRAMP("s3eFileTell");
#undef TRAMP
    /* worker do open = s3eFileOpen - 0x48c (0x4753c wrapper -> 0x470b0 worker).
     * Chamado DIRETO p/ pular o wrapper hookado (evita re-entrar nosso hook). */
    uintptr_t oa = so_find_addr_safe("s3eFileOpen");
    if (oa) g_worker_open = (void *(*)(const char *, const char *, int))(
        (((oa & ~1u) - 0x48c) | 1u));
    debugPrintf("ARCHIVE: worker_open=%p (s3eFileOpen=%lx)\n",
                (void *)g_worker_open, (unsigned long)oa);
    debugPrintf("ARCHIVE: trampolines reais open=%p read=%p size=%p seek=%p\n",
                (void *)real_s3eFileOpen, (void *)real_s3eFileRead,
                (void *)real_s3eFileGetSize, (void *)real_s3eFileSeek);
  }
  for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); i++) {
    uintptr_t a = so_find_addr_safe(m[i].sym);
    if (a) { hook_arm(a, (uintptr_t)m[i].fn);
             debugPrintf("hook export %s @0x%lx\n", m[i].sym, (unsigned long)a); }
    else debugPrintf("WARN: export %s nao achado\n", m[i].sym);
  }
}

static void patch_marmalade_guards(void) {
  uintptr_t acquire = so_find_addr_safe("__cxa_guard_acquire");
  uintptr_t release = so_find_addr_safe("__cxa_guard_release");
  uintptr_t abort = so_find_addr_safe("__cxa_guard_abort");
  uintptr_t getvm = so_find_addr_safe("s3eEdkJNIGetVM");
  uintptr_t devexit = so_find_addr_safe("s3eDeviceExit");
  uintptr_t dbgout = so_find_addr_safe("s3eDebugOutputString");
  uintptr_t dbgerr = so_find_addr_safe("s3eDebugErrorShow");
  uintptr_t edkerr = so_find_addr_safe("s3eEdkErrorSet");
  uintptr_t kbd_get_state = so_find_addr_safe("s3eKeyboardGetState");
  uintptr_t audio_get_int = so_find_addr_safe("s3eAudioGetInt");
  uintptr_t base = g_main_text_base ? g_main_text_base : (uintptr_t)text_base;

  so_make_text_writable();
  install_memcpy_guard();

  /* PES: hooka os s3eFile* EXPORTADOS por simbolo (nao os offsets internos do
     build Sonic). Muito mais robusto entre builds Marmalade. */
  install_s3efile_exports();
  if (getenv("PES_SONICPATCH")) {
    install_s3efile_shims(base);
    install_s3econfig_shims(base);
  }

  /*
   * CAPS GATE do exec-loader (causa-raiz s2: exec nunca carrega -> tela preta).
   * O exec-loader (0x24348) chama s3eDeviceCheckCaps(0xa216148) em 0x41374 e
   * retorna cedo se falhar -> 0x2d39c pega o RUN-stub (0x2ef88=bx lr) e nada
   * roda. Em 0x413de o check faz `r4 = required & ~registrados` (bics r4,r6) e
   * so passa (beq 0x414c0 -> ret 0) se r4==0. Como a ICF (com as device-caps)
   * NUNCA carrega neste so-loader, registrados=so 0x10000000 e o gate falha.
   * Fix: patch `bics r4,r6` (0x43b4) -> `movs r4,#0` (0x2400) => r4=0 => passa,
   * preservando os `str r0/r1,[r5,#32/36]` (device-caps) logo antes.
   * Confirmado por strace: PES2012.s3e nunca era aberto sem isto.
   */
  if (base && !getenv("PES_NO_CAPSGATE")) {
    if (getenv("PES_CAPS_OLD")) {
      volatile uint16_t *p = (volatile uint16_t *)(base + 0x413de);
      *p = 0x2400u; /* movs r4,#0 (modo antigo: pula os inits) */
      __builtin___clear_cache((char *)(base + 0x413de), (char *)(base + 0x413e2));
    } else {
      /* CERTO: deixa o `bics r4,r6` original correr -> a cadeia de tst RODA os 13
       * inits dos subsistemas (incl ThreadCore/TLS=0x5b198). Cada branch faz
       * `bl <init>; cmp r0,#0; beq <registra>; b 0x413ba(aborta se falhou)`.
       * Patchamos o `cmp r0,#0` (0x2800) logo apos cada `bl <init>` p/
       * `movs r0,#0` (0x2000) -> r0=0 -> beq sempre registra, nunca aborta.
       * Todos os subsistemas ficam registrados e o gate passa (0x414c0). */
      static const unsigned cmp_offs[] = {
        0x414f2, 0x41512, 0x41532, 0x41552, 0x41572, 0x41592, 0x415b2,
        0x415d2, 0x415f2, 0x41612, 0x41632, 0x41652, 0x41672,
      };
      for (unsigned i = 0; i < sizeof(cmp_offs)/sizeof(cmp_offs[0]); i++) {
        volatile uint16_t *p = (volatile uint16_t *)(base + cmp_offs[i]);
        if (*p == 0x2800u) {
          *p = 0x2000u; /* movs r0,#0 */
          __builtin___clear_cache((char *)(base + cmp_offs[i]),
                                  (char *)(base + cmp_offs[i] + 2));
        } else {
          debugPrintf("WARN caps-init cmp @0x%x = 0x%04x (esperado 0x2800)\n",
                      cmp_offs[i], *p);
        }
      }
      debugPrintf("patch: CAPS GATE — 13 inits force-success (bics RODA, subsist registram)\n");
    }
  }

  /*
   * CONFIG TABLE pre-alloc (2a causa-raiz: crash apos exec-load).
   * O subsistema s3eConfig (cluster 0x3b000-0x3d000, exporta s3eConfigGetInt@
   * 0x3b468 etc) guarda as vars parseadas numa tabela cujo ponteiro vive em
   * *0x8b3cc (.bss). Normalmente s3eDeviceCreate aloca essa tabela; nosso
   * so-loader pula isso -> *0x8b3cc=NULL. Ao carregar o exec, o parser de config
   * faz um LOOKUP (0x3c50c: r4=*0x8b3cc; count=[r4+4]) ANTES de popular ->
   * NULL-deref em 0x3c522 (r4=0). Mesma tecnica do shim Sonic (*0xc875c):
   * pre-alocamos uma tabela ZERADA; o caminho de "add" (0x3c5e6) cresce os
   * buffers via realloc(NULL,...)=malloc, entao o parser REAL popula o config.
   * (count=0 => lookup cai no add-path gracioso em vez de crashar.)
   */
  /* DIAGNOSTICO s4: forca o code-loader do exec (0x2460c) a rodar sempre.
   * 0x2d368: bl 0x2423c; se r0!=0 RETORNA cedo (pula 0x2460c=code-loader). Se
   * 0x2423c retorna !=0, o XE3U nunca e lido/descomprimido -> game code nao roda.
   * Patch: `beq 0x2d382`(0xd004)@0x2d376 -> `b 0x2d382`(0xe004) = sempre chama 0x2460c. */
  if (base && !getenv("PES_NO_LOADFIX")) {
    real_ld_2413c = (fn4_t)make_thumb_tramp(base + 0x2413c + 1);
    real_ld_2460c = (fn4_t)make_thumb_tramp(base + 0x2460c + 1);
    real_ld_248ac = (fn4_t)make_thumb_tramp(base + 0x248ac + 1);
    real_ld_24504 = (fn4_t)make_thumb_tramp(base + 0x24504 + 1);
    real_ld_47e64 = (fn4_t)make_thumb_tramp(base + 0x47e64 + 1);
    real_ld_49b70 = (fn4_t)make_thumb_tramp(base + 0x49b70 + 1);
    real_ld_470b0 = (fn4_t)make_thumb_tramp(base + 0x470b0 + 1);
    /* s3eFileOpen exportado ja esta hookado (install_s3efile_exports) -> chamar
     * o endereco faz nosso fopen real. */
    g_s3eFileOpen = (void *(*)(const char *, const char *))(base + 0x4753c + 1);
    hook_arm(base + 0x470b0 + 1, (uintptr_t)w_ld_470b0);
    hook_arm(base + 0x47e64 + 1, (uintptr_t)w_ld_47e64);
    hook_arm(base + 0x49b70 + 1, (uintptr_t)w_ld_49b70);
    hook_arm(base + 0x2413c + 1, (uintptr_t)w_ld_2413c);
    hook_arm(base + 0x2460c + 1, (uintptr_t)w_ld_2460c);
    hook_arm(base + 0x248ac + 1, (uintptr_t)w_ld_248ac);
    hook_arm(base + 0x24504 + 1, (uintptr_t)w_ld_24504);
    debugPrintf("hook: TRACE_LOAD em 2413c/2460c/248ac/24504\n");
  }

  if (base && getenv("PES_FORCE_CODELOAD")) {
    volatile uint16_t *p = (volatile uint16_t *)(base + 0x2d376);
    debugPrintf("patch: FORCE_CODELOAD @0x2d376 (era 0x%04x -> 0xe004)\n", *p);
    *p = 0xe004u; /* beq 0x2d382 -> b 0x2d382 (sempre chama 0x2460c) */
    /* 0x2460c retorna 1 -> 0x2d390 (bne 0x2d378) pula 0x248ac (loader real).
     * NOP no bne p/ SEMPRE cair em 0x2d392 = bl 0x248ac. */
    volatile uint16_t *q = (volatile uint16_t *)(base + 0x2d390);
    debugPrintf("patch: FORCE 0x248ac @0x2d390 (era 0x%04x -> 0xbf00 nop)\n", *q);
    *q = 0xbf00u; /* nop */
    __builtin___clear_cache((char *)(base + 0x2d376), (char *)(base + 0x2d394));
  }

  /* s7: o caps-fix (0x413de r4=0) passa o s3eDeviceCheckCaps mas PULA as branches
   * por-bit que sao os INITS dos subsistemas (ThreadCore/TLS=0x5b198, etc). Sem o
   * ThreadCore, a TLS key (0x8c688) fica 0 -> NULL-deref em 0x437f4 pos-load.
   * Rodamos os inits dos subsistemas manualmente aqui (idempotentes). */
  /* s9: o jogo seleciona a heap 6 (via contexto) e aloca -> s3eMallocBase falha
   * "heap 6 is not created" -> NULL -> crash no jogo. As heaps sao criadas por
   * config MemSize%d; MemSize6 nao existe. Fix: s3eMallocBase (0x4f178) pega o
   * indice da heap do CONTEXTO em 0x4f190 (`ldr r4,[r0]`); patch p/ `movs r4,#0`
   * = sempre heap 0 (a principal, MemSize=64MB). */
  if (base && !getenv("PES_NO_SYSMEM")) {
    hook_arm(base + 0x4f178 + 1, (uintptr_t)w_s3eMallocBase);  /* s3eMallocBase */
    hook_arm(base + 0x4e3ec + 1, (uintptr_t)w_s3eFreeBase);    /* s3eFreeBase */
    hook_arm(base + 0x4f360 + 1, (uintptr_t)w_s3eReallocBase); /* s3eReallocBase */
    real_verifyrsa = (fn6_t)make_thumb_tramp(base + 0x3da34 + 1);
    hook_arm(base + 0x3da34 + 1, (uintptr_t)w_s3eCryptoVerifyRsa); /* licença */
    debugPrintf("patch: s3e memory API -> system malloc/free/realloc; RSA verify -> valido\n");
  }

  if (base && getenv("PES_YIELD_NOOP")) {
    hook_arm(base + 0x41cf0 + 1, (uintptr_t)marm_s3eDeviceYield);
    debugPrintf("hook: s3eDeviceYield 0x41cf0 -> conta+no-op (diagnostico)\n");
  }

  if (base && !getenv("PES_NO_CFGTABLE")) {
    void **slot = (void **)(base + 0x8b3cc);
    if (*slot == NULL) {
      *slot = calloc(65536, 1);
      debugPrintf("patch: CONFIG TABLE pre-alloc @ *0x8b3cc = %p\n", *slot);
    } else {
      debugPrintf("patch: CONFIG TABLE ja alocada @ *0x8b3cc = %p\n", *slot);
    }
  }

  /*
   * SURFACE FLUSH stub: 0x8330c processa a fila de surfaces (present) lendo o
   * surface object via TLS (NULL no nosso ambiente sem render thread) -> crash
   * em 0x8333c. O present real do jogo vai por doDraw/glSwapBuffers (JNI ->
   * egl_shim), entao stubamos esse flush. Retorna void; ret0 e seguro.
   */
  if (base && getenv("PES_SONICPATCH") && !getenv("SONIC4EP1_NO_SURFSTUB")) {
    volatile uint32_t *p = (volatile uint32_t *)(base + 0x8330c);
    p[0] = 0xe3a00000u; /* mov r0, #0 */
    p[1] = 0xe12fff1eu; /* bx lr */
    __builtin___clear_cache((char *)(base + 0x8330c), (char *)(base + 0x83314));
    debugPrintf("patch: surface-flush stub @ 0x8330c\n");
  }

  /*
   * CAPS FIX (causa raiz da tela preta):
   * O loader s3e monta as device-capabilities como
   *   caps = avail | 0x10000000 | [config+0x340]   (em 0x58cdc/0x58d60).
   * O [config+0x340] vem do ICF (s3e.icf/app.icf), que NUNCA carrega neste
   * so-loader -> caps fica so 0x10000000, e o gate 0x13c44 exige 0x0a216148
   * -> runNative retorna na hora -> tela preta.
   * Aqui patchamos `ldr r3,[r1,#832]` (0x58d60) para `mvn r3,#0` (r3=0xffffffff),
   * fazendo os caps cobrirem tudo que o app exige. Confirmado via gdb: gate passa.
   */
  if (base && getenv("PES_SONICPATCH") && !getenv("SONIC4EP1_NO_CAPFIX")) {
    volatile uint32_t *p = (volatile uint32_t *)(base + 0x58d60);
    /* s3 (2026-06-29): o device-register (0x58cdc) faz `r4 = requested & ~[r6+76]`
     * e registra os subsistemas em r4. [r6+76] = 0x10000000 | r3 (r3 vem de
     * [config+832], PATCHADO aqui). Com `mvn r3,#0` (=0xffffffff) TODOS os bits
     * ficam "ja registrados" -> r4=0 -> NADA registra, incl. o ThreadCore/TLS
     * (bit 0x8, init 0x7f928 que cria a key do surface em 0xc9a74) -> crash do
     * surface. Solucao: `mvn r3,#8` (=0xfffffff7) marca tudo MENOS o bit 0x8,
     * entao o dispatch registra SO o ThreadCore (cria a TLS/surface) e o resto
     * fica "disponivel" pro gate 0x13c44. SONIC4EP1_CAPFIX_ALLBITS volta ao antigo. */
    if (getenv("SONIC4EP1_CAPFIX_ALLBITS"))
      *p = 0xe3e03000u; /* mvn r3, #0  -> caps = 0xffffffff (comportamento antigo) */
    else
      *p = 0xe3e03008u; /* mvn r3, #8  -> caps = 0xfffffff7 (registra ThreadCore) */
    __builtin___clear_cache((char *)(base + 0x58d60), (char *)(base + 0x58d64));
    debugPrintf("patch: CAPS FIX @ %p (0x58d60 ldr->mvn r3,#%d)\n",
                (void *)(base + 0x58d60),
                getenv("SONIC4EP1_CAPFIX_ALLBITS") ? 0 : 8);
  }

  /*
   * INJECT s3e.icf EMBUTIDO:
   * O so-loader nao monta o VFS s3e (rom:// fica vazio), entao o loader nunca
   * acha s3e.icf como arquivo -> "could not find s3e.icf" -> NULL deref/crash.
   * A funcao 0x138a4 carrega o icf EMBUTIDO via s3eFileOpenFromMemory(ptr) onde
   *   ptr = [global_deploy+296]  (global_deploy @ .bss 0xc3148)
   *   e [global_deploy+32] precisa ser != 0 (flag de presenca).
   * Injetamos o ponteiro pro nosso texto de icf nesses globals.
   */
  if (base && getenv("PES_SONICPATCH") && !getenv("SONIC4EP1_NO_ICF_INJECT")) {
    real_gate_13c44 = (int (*)(void))make_arm_trampoline(base + 0x13c44);
    hook_arm(base + 0x13c44, (uintptr_t)marm_gate_13c44);
    debugPrintf("hook: gate 0x13c44 -> inject s3e.icf embutido (tramp=%p)\n",
                (void *)real_gate_13c44);
  }

  if (acquire) {
    hook_arm(acquire, (uintptr_t)marm_cxa_guard_acquire);
    debugPrintf("patch: __cxa_guard_acquire @ %p\n", (void *)acquire);
  }
  if (release) {
    hook_arm(release, (uintptr_t)marm_cxa_guard_release);
    debugPrintf("patch: __cxa_guard_release @ %p\n", (void *)release);
  }
  if (abort) {
    hook_arm(abort, (uintptr_t)marm_cxa_guard_abort);
    debugPrintf("patch: __cxa_guard_abort @ %p\n", (void *)abort);
  }
  if (getvm) {
    hook_arm(getvm, (uintptr_t)marm_s3eEdkJNIGetVM);
    debugPrintf("patch: s3eEdkJNIGetVM @ %p\n", (void *)getvm);
  }
  if (devexit) {
    hook_arm(devexit, (uintptr_t)marm_s3eDeviceExit);
    debugPrintf("patch: s3eDeviceExit @ %p\n", (void *)devexit);
  }
  if (dbgout) {
    hook_arm(dbgout, (uintptr_t)marm_s3eDebugOutputString);
    debugPrintf("patch: s3eDebugOutputString @ %p\n", (void *)dbgout);
  }
  if (dbgerr) {
    hook_arm(dbgerr, (uintptr_t)marm_s3eDebugErrorShow);
    debugPrintf("patch: s3eDebugErrorShow @ %p\n", (void *)dbgerr);
  }
  if (edkerr && getenv("SONIC4EP1_TRACE_ERRORS")) {
    hook_arm(edkerr, (uintptr_t)marm_s3eEdkErrorSet);
    debugPrintf("diag patch: s3eEdkErrorSet @ %p\n", (void *)edkerr);
  }
  if (kbd_get_state &&
      (getenv("SONIC4EP1_TRACE_S3EKEY") ||
       getenv("SONIC4EP1_S3EKEY_FORCE"))) {
    real_s3eKeyboardGetState =
        (int (*)(int))make_arm_trampoline(kbd_get_state);
    hook_arm(kbd_get_state, (uintptr_t)marm_s3eKeyboardGetState);
    debugPrintf("diag patch: s3eKeyboardGetState @ %p tramp=%p\n",
                (void *)kbd_get_state, (void *)real_s3eKeyboardGetState);
  }
  if (audio_get_int &&
      (getenv("PES_AUDIO_GETINT_LOG") ||
       getenv("PES_AUDIO_GETINT_FORCE"))) {
    real_s3eAudioGetInt =
        (int (*)(int))((audio_get_int & 1)
                           ? make_thumb_tramp_full(audio_get_int)
                           : make_arm_trampoline(audio_get_int));
    hook_arm(audio_get_int, (uintptr_t)marm_s3eAudioGetInt);
    debugPrintf("diag patch: s3eAudioGetInt @ %p tramp=%p\n",
                (void *)audio_get_int, (void *)real_s3eAudioGetInt);
  }
  if (getenv("SONIC4EP1_BYPASS_13C44"))
    patch_rel_ret0(0x13c44, "loader init check 0x13c44");
  if (getenv("SONIC4EP1_FORCE_FILEINIT_OK") && base) {
    hook_arm(base + 0x69114, (uintptr_t)marm_file_init_finalizer_ok);
    debugPrintf("diag patch: file init finalizer @ %p -> force OK after real call\n",
                (void *)(base + 0x69114));
  }
  patch_ret0_list_from_env();
  if (getenv("SONIC4EP1_TRACE_BOOT") && base) {
    hook_arm(base + 0x2688c, (uintptr_t)marm_entry_2688c);
    debugPrintf("diag patch: entry 0x2688c @ %p\n",
                (void *)(base + 0x2688c));
  }
  so_flush_caches();
  so_make_text_executable();
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
  if (strcmp(name, SO_NAME) == 0)
    patch_marmalade_guards();
  if (!getenv("SONIC4EP1_NO_INIT_ARRAY")) {
    debugPrintf("%s: init_array...\n", name);
    so_execute_init_array();
    debugPrintf("%s: init_array OK\n", name);
  }
  if (snapshot) {
    int n = 0;
    DynLibFunction *t = so_snapshot_symbols(&n);
    if (t && n > 0) { comb_append(t, n); debugPrintf("%s: +%d símbolos exportados\n", name, n); }
  }
  if (strcmp(name, SO_NAME) == 0) {
    g_main_text_base = (uintptr_t)text_base;
    g_main_text_size = text_size;
    debugPrintf("PES_TEXTBASE=%p (add offset for gdb)\n", (void *)g_main_text_base);
    g_main_jni_onload = so_find_addr_safe("JNI_OnLoad");
    g_main_android_main = so_find_addr_safe("android_main");
  } else {
    uintptr_t reg = so_find_addr_safe("RegisterExt");
    if (reg && g_ext_registers_n < (int)(sizeof(g_ext_registers) / sizeof(g_ext_registers[0]))) {
      g_ext_registers[g_ext_registers_n].name = name;
      g_ext_registers[g_ext_registers_n].addr = reg;
      g_ext_registers_n++;
      debugPrintf("%s: RegisterExt=%p\n", name, (void *)reg);
    }
  }
  return 0;
}

static void call_extension_registers(void) {
  for (int i = 0; i < g_ext_registers_n; i++) {
    int (*RegisterExt)(void) = (int (*)(void))g_ext_registers[i].addr;
    debugPrintf("%s: chamando RegisterExt...\n", g_ext_registers[i].name);
    int r = RegisterExt();
    debugPrintf("%s: RegisterExt -> %d\n", g_ext_registers[i].name, r);
  }
}

static int ext_name_matches(const char *base, const char *loaded_name) {
  if (!base || !loaded_name)
    return 0;
  if (strcmp(base, loaded_name) == 0)
    return 1;
  if (strstr(base, "APKExpansion") || strstr(base, "KExpansion"))
    return strstr(loaded_name, "APKExpansion") != NULL;
  if (strstr(base, "Dialog") || strstr(base, "alog"))
    return strstr(loaded_name, "Dialog") != NULL;
  if (strstr(base, "Flurry") || strstr(base, "urry"))
    return strstr(loaded_name, "Flurry") != NULL;
  return 0;
}

void *sonic4ep1_dlopen(const char *filename, int flags) {
  const char *base = filename ? strrchr(filename, '/') : NULL;
  base = base ? base + 1 : filename;
  for (int i = 0; i < g_ext_registers_n; i++) {
    if (ext_name_matches(base, g_ext_registers[i].name)) {
      debugPrintf("dlopen shim: %s -> ext[%d]\n", filename, i);
      return &g_ext_registers[i];
    }
  }
  void *h = dlopen(filename, flags);
  debugPrintf("dlopen shim: host %s -> %p\n", filename ? filename : "(null)", h);
  return h;
}

void *sonic4ep1_dlsym(void *handle, const char *symbol) {
  for (int i = 0; i < g_ext_registers_n; i++) {
    if (handle == &g_ext_registers[i]) {
      if (symbol && strcmp(symbol, "RegisterExt") == 0) {
        debugPrintf("dlsym shim: %s:RegisterExt -> %p\n", g_ext_registers[i].name,
                    (void *)g_ext_registers[i].addr);
        return (void *)g_ext_registers[i].addr;
      }
      debugPrintf("dlsym shim: %s:%s -> NULL\n", g_ext_registers[i].name,
                  symbol ? symbol : "(null)");
      return NULL;
    }
  }
  void *p = softfp_resolve(symbol);
  if (p) {
    debugPrintf("dlsym shim: softfp %s -> %p\n", symbol ? symbol : "(null)", p);
    return p;
  }
  p = dlsym(handle, symbol);
  p = sonic4ep1_gl_hook(symbol, p);
  debugPrintf("dlsym shim: host %s -> %p\n", symbol ? symbol : "(null)", p);
  return p;
}

int sonic4ep1_dlclose(void *handle) {
  for (int i = 0; i < g_ext_registers_n; i++) {
    if (handle == &g_ext_registers[i]) {
      debugPrintf("dlclose shim: %s\n", g_ext_registers[i].name);
      return 0;
    }
  }
  return dlclose(handle);
}

char *sonic4ep1_dlerror(void) {
  return dlerror();
}

static int trace_gl(void) {
  static int cached = -1;
  if (cached < 0)
    cached = getenv("SONIC4EP1_TRACE_GL") ? 1 : 0;
  if (cached && getenv("SONIC4EP1_TRACE_GL_AFTER_AUTOTAP") &&
      !g_autotap_fired)
    return 0;
  return cached;
}

static int want_gl_hook(void) {
  return getenv("SONIC4EP1_TRACE_GL") || getenv("SONIC4EP1_SKIP_NAN_MATRIX") ||
         getenv("PES_GL_PROBE") || getenv("PES_GL_FORCE_OPAQUE") ||
         getenv("PES_GL_DISABLE_BLEND") || getenv("PES_GL_FORCE_TEXFILTER");
}

static int gl_probe_enabled(void) {
  static int cached = -1;
  if (cached < 0)
    cached = getenv("PES_GL_PROBE") ? 1 : 0;
  return cached;
}

static void gl_probe_msg(const char *name, const char *fmt, ...) {
  if (!gl_probe_enabled())
    return;
  static int log_count = 0;
  if (log_count++ >= env_int_or_default("PES_GL_PROBE_LIMIT", 400))
    return;
  fprintf(stderr, "[glprobe] %s", name);
  if (fmt && *fmt) {
    fprintf(stderr, " ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
  }
  fprintf(stderr, "\n");
}

static unsigned int g_gl_matrix_mode_seen;
static const void *g_gl_color_ptr_seen;
static int g_gl_color_size_seen;
static unsigned int g_gl_color_type_seen;
static int g_gl_color_stride_seen;
static unsigned int g_gl_last_texture_seen;
static int g_gl_client_vertex_seen;
static int g_gl_client_color_seen;
static int g_gl_client_tex_seen;
static int g_gl_draw_probe_count;

static int gl_bytes_per_component(unsigned int type) {
  switch (type) {
  case 0x1400: /* GL_BYTE */
  case 0x1401: /* GL_UNSIGNED_BYTE */
    return 1;
  case 0x1402: /* GL_SHORT */
  case 0x1403: /* GL_UNSIGNED_SHORT */
    return 2;
  case 0x1404: /* GL_INT */
  case 0x1405: /* GL_UNSIGNED_INT */
  case 0x1406: /* GL_FLOAT */
  case 0x140c: /* GL_FIXED */
    return 4;
  default:
    return 0;
  }
}

static int gl_type_signed(unsigned int type) {
  return type == 0x1400 || type == 0x1402 || type == 0x1404 ||
         type == 0x1406 || type == 0x140c;
}

static int gl_component_i(const void *base, unsigned int type, int idx) {
  const unsigned char *p = (const unsigned char *)base;
  switch (type) {
  case 0x1400:
    return ((const signed char *)base)[idx];
  case 0x1401:
    return p[idx];
  case 0x1402:
    return ((const short *)base)[idx];
  case 0x1403:
    return ((const unsigned short *)base)[idx];
  case 0x1404:
  case 0x140c:
    return ((const int *)base)[idx];
  case 0x1405:
    return (int)((const unsigned int *)base)[idx];
  default:
    return 0;
  }
}

static float gl_component_f(const void *base, unsigned int type, int idx) {
  if (type == 0x1406)
    return ((const float *)base)[idx];
  if (type == 0x140c)
    return (float)((const int *)base)[idx] / 65536.0f;
  return (float)gl_component_i(base, type, idx);
}

static int gl_sample_span_mapped(const void *ptr, size_t bytes) {
  if (!ptr || bytes == 0)
    return 0;
  if (ptr_range_mapped(ptr, bytes))
    return 1;
  snapshot_maps();
  return ptr_range_mapped(ptr, bytes);
}

static void gl_probe_pointer_sample(const char *name, int size,
                                    unsigned int type, int stride,
                                    const void *ptr) {
  if (!gl_probe_enabled() || !ptr || size <= 0)
    return;
  int bpc = gl_bytes_per_component(type);
  if (bpc <= 0)
    return;
  int step = stride > 0 ? stride : size * bpc;
  size_t need = (size_t)(step * 3 + size * bpc);
  if (!gl_sample_span_mapped(ptr, need)) {
    gl_probe_msg(name, "sample unmapped ptr=%p need=%zu", ptr, need);
    return;
  }
  const unsigned char *row = (const unsigned char *)ptr;
  if (type == 0x1406 || type == 0x140c) {
    gl_probe_msg(name,
                 "sample tex=%u client(v=%d c=%d t=%d) mode=0x%x "
                 "v0=(%.3f %.3f %.3f %.3f) v1=(%.3f %.3f %.3f %.3f)",
                 g_gl_last_texture_seen, g_gl_client_vertex_seen,
                 g_gl_client_color_seen, g_gl_client_tex_seen,
                 g_gl_matrix_mode_seen,
                 size > 0 ? gl_component_f(row, type, 0) : 0.0f,
                 size > 1 ? gl_component_f(row, type, 1) : 0.0f,
                 size > 2 ? gl_component_f(row, type, 2) : 0.0f,
                 size > 3 ? gl_component_f(row, type, 3) : 0.0f,
                 size > 0 ? gl_component_f(row + step, type, 0) : 0.0f,
                 size > 1 ? gl_component_f(row + step, type, 1) : 0.0f,
                 size > 2 ? gl_component_f(row + step, type, 2) : 0.0f,
                 size > 3 ? gl_component_f(row + step, type, 3) : 0.0f);
  } else {
    gl_probe_msg(name,
                 "sample tex=%u signed=%d c0=(%d %d %d %d) c1=(%d %d %d %d)",
                 g_gl_last_texture_seen, gl_type_signed(type),
                 size > 0 ? gl_component_i(row, type, 0) : 0,
                 size > 1 ? gl_component_i(row, type, 1) : 0,
                 size > 2 ? gl_component_i(row, type, 2) : 0,
                 size > 3 ? gl_component_i(row, type, 3) : 0,
                 size > 0 ? gl_component_i(row + step, type, 0) : 0,
                 size > 1 ? gl_component_i(row + step, type, 1) : 0,
                 size > 2 ? gl_component_i(row + step, type, 2) : 0,
                 size > 3 ? gl_component_i(row + step, type, 3) : 0);
  }
}

static void gl_probe_draw_state(const char *name) {
  if (!gl_probe_enabled())
    return;
  int limit = env_int_or_default("PES_GL_DRAW_STATE_LIMIT", 24);
  if (g_gl_draw_probe_count++ >= limit)
    return;
  static void (*real_glGetFloatv_probe)(unsigned int, float *);
  static void (*real_glGetIntegerv_probe)(unsigned int, int *);
  static void (*real_glReadPixels_probe)(int, int, int, int, unsigned int,
                                         unsigned int, void *);
  if (!real_glGetFloatv_probe)
    real_glGetFloatv_probe =
        (void (*)(unsigned int, float *))dlsym(RTLD_DEFAULT, "glGetFloatv");
  if (!real_glGetIntegerv_probe)
    real_glGetIntegerv_probe =
        (void (*)(unsigned int, int *))dlsym(RTLD_DEFAULT, "glGetIntegerv");
  if (!real_glReadPixels_probe)
    real_glReadPixels_probe =
        (void (*)(int, int, int, int, unsigned int, unsigned int, void *))dlsym(
            RTLD_DEFAULT, "glReadPixels");
  float mv[16], pr[16], tx[16];
  int vp[4] = {0, 0, 0, 0};
  unsigned char px[4] = {0, 0, 0, 0};
  memset(mv, 0, sizeof(mv));
  memset(pr, 0, sizeof(pr));
  memset(tx, 0, sizeof(tx));
  if (real_glGetFloatv_probe) {
    real_glGetFloatv_probe(0x0ba6, mv); /* MODELVIEW_MATRIX */
    real_glGetFloatv_probe(0x0ba7, pr); /* PROJECTION_MATRIX */
    real_glGetFloatv_probe(0x0ba8, tx); /* TEXTURE_MATRIX */
  }
  if (real_glGetIntegerv_probe)
    real_glGetIntegerv_probe(0x0ba2, vp); /* VIEWPORT */
  if (real_glReadPixels_probe && vp[2] > 0 && vp[3] > 0)
    real_glReadPixels_probe(vp[0] + vp[2] / 2, vp[1] + vp[3] / 2, 1, 1,
                            0x1908, 0x1401, px);
  gl_probe_msg(name,
               "state vp=%d,%d %dx%d center=%u,%u,%u,%u "
               "MV0=%.5g %.5g %.5g %.5g MV3=%.5g %.5g %.5g %.5g "
               "PR0=%.5g %.5g %.5g %.5g PR3=%.5g %.5g %.5g %.5g "
               "TX0=%.5g %.5g %.5g %.5g TX3=%.5g %.5g %.5g %.5g",
               vp[0], vp[1], vp[2], vp[3], px[0], px[1], px[2], px[3],
               mv[0], mv[1], mv[2], mv[3], mv[12], mv[13], mv[14], mv[15],
               pr[0], pr[1], pr[2], pr[3], pr[12], pr[13], pr[14], pr[15],
               tx[0], tx[1], tx[2], tx[3], tx[12], tx[13], tx[14], tx[15]);
}

static void gl_force_opaque_color_array_if_needed(void) {
  if (!getenv("PES_GL_FORCE_OPAQUE") || !g_gl_color_ptr_seen ||
      g_gl_color_size_seen < 4 || g_gl_color_type_seen != 0x1401)
    return;
  int bpc = gl_bytes_per_component(g_gl_color_type_seen);
  int step = g_gl_color_stride_seen > 0 ? g_gl_color_stride_seen
                                        : g_gl_color_size_seen * bpc;
  if (step <= 0)
    return;
  unsigned char *row = (unsigned char *)g_gl_color_ptr_seen;
  size_t need = (size_t)(step * 511 + g_gl_color_size_seen * bpc);
  if (!gl_sample_span_mapped(row, need))
    return;
  for (int i = 0; i < 512; i++)
    row[i * step + 3] = 255;
}

static void gl_trace_msg(const char *name, const char *fmt, ...) {
  if (!trace_gl())
    return;
  static int log_count = 0;
  if (log_count++ >= env_int_or_default("SONIC4EP1_TRACE_GL_LIMIT", 240))
    return;
  fprintf(stderr, "[gl] %s", name);
  if (fmt && *fmt) {
    fprintf(stderr, " ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
  }
  fprintf(stderr, "\n");
}

static unsigned int gl_trace_get_error(void) {
  static unsigned int (*real_glGetError)(void);
  if (!real_glGetError)
    real_glGetError = (unsigned int (*)(void))dlsym(RTLD_DEFAULT, "glGetError");
  return real_glGetError ? real_glGetError() : 0;
}

static void gl_trace_err(const char *name) {
  if (!trace_gl())
    return;
  unsigned int err = gl_trace_get_error();
  if (err)
    gl_trace_msg(name, "ERROR=0x%x", err);
}

#define GL_WRAP_VOID0(name) \
  static void (*real_##name)(void); \
  static void wrap_##name(void) { \
    gl_trace_msg(#name, ""); \
    real_##name(); \
    gl_trace_err(#name); \
  }
#define GL_WRAP_VOID1(name, t0, f) \
  static void (*real_##name)(t0); \
  static void wrap_##name(t0 a0) { \
    gl_trace_msg(#name, f, a0); \
    real_##name(a0); \
    gl_trace_err(#name); \
  }

static void (*real_glClearColor)(float, float, float, float);
static void wrap_glClearColor(float r, float g, float b, float a) {
  gl_trace_msg("glClearColor", "%.3f %.3f %.3f %.3f", r, g, b, a);
  real_glClearColor(r, g, b, a);
  gl_trace_err("glClearColor");
}

static void (*real_glClear)(unsigned int);
static void wrap_glClear(unsigned int mask) {
  gl_trace_msg("glClear", "mask=0x%x", mask);
  real_glClear(mask);
  gl_trace_err("glClear");
}

static void (*real_glViewport)(int, int, int, int);
static void wrap_glViewport(int x, int y, int w, int h) {
  gl_trace_msg("glViewport", "%d %d %d %d", x, y, w, h);
  real_glViewport(x, y, w, h);
  gl_trace_err("glViewport");
}

static void (*real_glScissor)(int, int, int, int);
static void wrap_glScissor(int x, int y, int w, int h) {
  gl_trace_msg("glScissor", "%d %d %d %d", x, y, w, h);
  real_glScissor(x, y, w, h);
  gl_trace_err("glScissor");
}

static void (*real_glDrawArrays)(unsigned int, int, int);
static void wrap_glDrawArrays(unsigned int mode, int first, int count) {
  gl_trace_msg("glDrawArrays", "mode=0x%x first=%d count=%d", mode, first,
               count);
  gl_force_opaque_color_array_if_needed();
  gl_probe_msg("glDrawArrays",
               "mode=0x%x first=%d count=%d tex=%u client(v=%d c=%d t=%d)",
               mode, first, count, g_gl_last_texture_seen,
               g_gl_client_vertex_seen, g_gl_client_color_seen,
               g_gl_client_tex_seen);
  real_glDrawArrays(mode, first, count);
  gl_probe_draw_state("glDrawArrays");
  gl_trace_err("glDrawArrays");
}

static void (*real_glDrawElements)(unsigned int, int, unsigned int, const void *);
static void wrap_glDrawElements(unsigned int mode, int count, unsigned int type,
                                const void *indices) {
  gl_trace_msg("glDrawElements", "mode=0x%x count=%d type=0x%x indices=%p",
               mode, count, type, indices);
  gl_force_opaque_color_array_if_needed();
  if (gl_probe_enabled()) {
    int first[6] = {0, 0, 0, 0, 0, 0};
    int n = count < 6 ? count : 6;
    if (indices && type == 0x1403 &&
        gl_sample_span_mapped(indices, (size_t)n * sizeof(unsigned short))) {
      const unsigned short *idx = (const unsigned short *)indices;
      for (int i = 0; i < n; i++)
        first[i] = idx[i];
    }
    gl_probe_msg("glDrawElements",
                 "mode=0x%x count=%d type=0x%x idx=%p first=%d,%d,%d,%d,%d,%d "
                 "tex=%u client(v=%d c=%d t=%d)",
                 mode, count, type, indices, first[0], first[1], first[2],
                 first[3], first[4], first[5], g_gl_last_texture_seen,
                 g_gl_client_vertex_seen, g_gl_client_color_seen,
                 g_gl_client_tex_seen);
  }
  real_glDrawElements(mode, count, type, indices);
  gl_probe_draw_state("glDrawElements");
  gl_trace_err("glDrawElements");
}

static void (*real_glBindBuffer)(unsigned int, unsigned int);
static void wrap_glBindBuffer(unsigned int target, unsigned int buffer) {
  gl_trace_msg("glBindBuffer", "target=0x%x buffer=%u", target, buffer);
  real_glBindBuffer(target, buffer);
  gl_trace_err("glBindBuffer");
}

static void (*real_glBufferData)(unsigned int, int, const void *, unsigned int);
static void wrap_glBufferData(unsigned int target, int size, const void *data,
                              unsigned int usage) {
  gl_trace_msg("glBufferData", "target=0x%x size=%d data=%p usage=0x%x",
               target, size, data, usage);
  real_glBufferData(target, size, data, usage);
  gl_trace_err("glBufferData");
}

static void (*real_glBufferSubData)(unsigned int, int, int, const void *);
static void wrap_glBufferSubData(unsigned int target, int offset, int size,
                                 const void *data) {
  gl_trace_msg("glBufferSubData", "target=0x%x offset=%d size=%d data=%p",
               target, offset, size, data);
  real_glBufferSubData(target, offset, size, data);
  gl_trace_err("glBufferSubData");
}

static void (*real_glGenBuffers)(int, unsigned int *);
static void wrap_glGenBuffers(int n, unsigned int *buffers) {
  real_glGenBuffers(n, buffers);
  gl_trace_msg("glGenBuffers", "n=%d first=%u", n,
               (buffers && n > 0) ? buffers[0] : 0);
  gl_trace_err("glGenBuffers");
}

static void (*real_glLoadMatrixf)(const float *);
static void wrap_glLoadMatrixf(const float *m) {
  if (m) {
    gl_trace_msg("glLoadMatrixf", "%.3f %.3f %.3f %.3f ...", m[0], m[1],
                 m[2], m[3]);
    gl_probe_msg("glLoadMatrixf",
                 "mode=0x%x row0=%.6g %.6g %.6g %.6g row1=%.6g %.6g %.6g %.6g "
                 "row2=%.6g %.6g %.6g %.6g row3=%.6g %.6g %.6g %.6g",
                 g_gl_matrix_mode_seen, m[0], m[1], m[2], m[3], m[4], m[5],
                 m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14],
                 m[15]);
  } else {
    gl_trace_msg("glLoadMatrixf", "NULL");
    gl_probe_msg("glLoadMatrixf", "mode=0x%x NULL", g_gl_matrix_mode_seen);
  }
  real_glLoadMatrixf(m);
  gl_trace_err("glLoadMatrixf");
}

static void (*real_glMultMatrixf)(const float *);
static void wrap_glMultMatrixf(const float *m) {
  int bad = 0;
  static float last_good[16] = {
      1.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 1.0f,
  };
  if (m) {
    for (int i = 0; i < 16; i++) {
      if (m[i] != m[i] || m[i] > 1.0e20f || m[i] < -1.0e20f) {
        bad = 1;
        break;
      }
    }
  }
  if (m) {
    gl_trace_msg("glMultMatrixf", "%.3f %.3f %.3f %.3f ...", m[0], m[1],
                 m[2], m[3]);
  } else {
    gl_trace_msg("glMultMatrixf", "NULL");
  }
  if (bad && getenv("SONIC4EP1_SKIP_NAN_MATRIX")) {
    gl_trace_msg("glMultMatrixf", "skip bad matrix");
    return;
  }
  if (bad && getenv("SONIC4EP1_FIX_NAN_MATRIX")) {
    float fixed[16];
    for (int i = 0; i < 16; i++) {
      if (m[i] != m[i] || m[i] > 1.0e20f || m[i] < -1.0e20f)
        fixed[i] = last_good[i];
      else
        fixed[i] = m[i];
    }
    gl_trace_msg("glMultMatrixf", "fixed bad matrix");
    real_glMultMatrixf(fixed);
    gl_trace_err("glMultMatrixf");
    return;
  }
  if (m && !bad)
    memcpy(last_good, m, sizeof(last_good));
  real_glMultMatrixf(m);
  gl_trace_err("glMultMatrixf");
}

static void (*real_glCurrentPaletteMatrixOES)(unsigned int);
static void wrap_glCurrentPaletteMatrixOES(unsigned int matrix) {
  gl_trace_msg("glCurrentPaletteMatrixOES", "%u", matrix);
  real_glCurrentPaletteMatrixOES(matrix);
  gl_trace_err("glCurrentPaletteMatrixOES");
}

static void (*real_glLoadPaletteFromModelViewMatrixOES)(void);
static void wrap_glLoadPaletteFromModelViewMatrixOES(void) {
  gl_trace_msg("glLoadPaletteFromModelViewMatrixOES", "");
  real_glLoadPaletteFromModelViewMatrixOES();
  gl_trace_err("glLoadPaletteFromModelViewMatrixOES");
}

static void (*real_glMatrixIndexPointerOES)(int, unsigned int, int,
                                            const void *);
static void wrap_glMatrixIndexPointerOES(int size, unsigned int type,
                                         int stride, const void *ptr) {
  gl_trace_msg("glMatrixIndexPointerOES", "size=%d type=0x%x stride=%d ptr=%p",
               size, type, stride, ptr);
  real_glMatrixIndexPointerOES(size, type, stride, ptr);
  gl_trace_err("glMatrixIndexPointerOES");
}

static void (*real_glWeightPointerOES)(int, unsigned int, int, const void *);
static void wrap_glWeightPointerOES(int size, unsigned int type, int stride,
                                    const void *ptr) {
  gl_trace_msg("glWeightPointerOES", "size=%d type=0x%x stride=%d ptr=%p",
               size, type, stride, ptr);
  real_glWeightPointerOES(size, type, stride, ptr);
  gl_trace_err("glWeightPointerOES");
}

static void (*real_glTexImage2D)(unsigned int, int, int, int, int, int,
                                 unsigned int, unsigned int, const void *);
static void (*real_glTexParameteri_for_upload)(unsigned int, unsigned int, int);

static void gl_force_texture_filter_if_needed(unsigned int target) {
  if (!getenv("PES_GL_FORCE_TEXFILTER"))
    return;
  if (!real_glTexParameteri_for_upload)
    real_glTexParameteri_for_upload =
        (void (*)(unsigned int, unsigned int, int))dlsym(RTLD_DEFAULT,
                                                        "glTexParameteri");
  if (!real_glTexParameteri_for_upload)
    return;
  real_glTexParameteri_for_upload(target, 0x2801, 0x2601); /* MIN_FILTER=LINEAR */
  real_glTexParameteri_for_upload(target, 0x2800, 0x2601); /* MAG_FILTER=LINEAR */
  gl_probe_msg("glTexParameteri", "forced target=0x%x min/mag=GL_LINEAR",
               target);
}

static void wrap_glTexImage2D(unsigned int target, int level, int internal,
                              int w, int h, int border, unsigned int format,
                              unsigned int type, const void *pixels) {
  gl_trace_msg("glTexImage2D",
               "target=0x%x level=%d internal=0x%x %dx%d border=%d fmt=0x%x type=0x%x pix=%p",
               target, level, internal, w, h, border, format, type, pixels);
  if (trace_gl() && getenv("SONIC4EP1_TRACE_TEX") && pixels && w > 0 &&
      h > 0) {
    static int tex_log_count = 0;
    if (tex_log_count++ < 80) {
      const unsigned char *p = (const unsigned char *)pixels;
      if (type == 0x8033) {
        const unsigned short *s = (const unsigned short *)pixels;
        fprintf(stderr,
                "[tex] %dx%d fmt=0x%x type=4444 first=%04x %04x %04x %04x "
                "bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                w, h, format, s[0], s[1], s[2], s[3], p[0], p[1], p[2],
                p[3], p[4], p[5], p[6], p[7]);
      } else {
        fprintf(stderr,
                "[tex] %dx%d fmt=0x%x type=0x%x bytes=%02x %02x %02x %02x "
                "%02x %02x %02x %02x\n",
                w, h, format, type, p[0], p[1], p[2], p[3], p[4], p[5],
                p[6], p[7]);
      }
    }
  }
  real_glTexImage2D(target, level, internal, w, h, border, format, type, pixels);
  gl_force_texture_filter_if_needed(target);
  gl_trace_err("glTexImage2D");
}

static void (*real_glCompressedTexImage2D)(unsigned int, int, unsigned int, int,
                                           int, int, int, const void *);
static void wrap_glCompressedTexImage2D(unsigned int target, int level,
                                        unsigned int internal, int w, int h,
                                        int border, int image_size,
                                        const void *data) {
  gl_trace_msg("glCompressedTexImage2D",
               "target=0x%x level=%d internal=0x%x %dx%d border=%d size=%d data=%p",
               target, level, internal, w, h, border, image_size, data);
  real_glCompressedTexImage2D(target, level, internal, w, h, border, image_size,
                              data);
  gl_force_texture_filter_if_needed(target);
  gl_trace_err("glCompressedTexImage2D");
}

GL_WRAP_VOID1(glEnable, unsigned int, "cap=0x%x")
GL_WRAP_VOID1(glDisable, unsigned int, "cap=0x%x")

static void (*real_glMatrixMode)(unsigned int);
static void wrap_glMatrixMode(unsigned int mode) {
  g_gl_matrix_mode_seen = mode;
  gl_trace_msg("glMatrixMode", "mode=0x%x", mode);
  gl_probe_msg("glMatrixMode", "mode=0x%x", mode);
  real_glMatrixMode(mode);
  gl_trace_err("glMatrixMode");
}

GL_WRAP_VOID0(glLoadIdentity)

static void (*real_glBindTexture)(unsigned int, unsigned int);
static void wrap_glBindTexture(unsigned int target, unsigned int texture) {
  if (target == 0x0de1)
    g_gl_last_texture_seen = texture;
  gl_trace_msg("glBindTexture", "target=0x%x texture=%u", target, texture);
  gl_probe_msg("glBindTexture", "target=0x%x texture=%u", target, texture);
  real_glBindTexture(target, texture);
  gl_trace_err("glBindTexture");
}

static void (*real_glBlendFunc)(unsigned int, unsigned int);
static void wrap_glBlendFunc(unsigned int sfactor, unsigned int dfactor) {
  gl_probe_msg("glBlendFunc", "src=0x%x dst=0x%x", sfactor, dfactor);
  if (getenv("PES_GL_DISABLE_BLEND")) {
    sfactor = 1; /* GL_ONE */
    dfactor = 0; /* GL_ZERO */
    gl_probe_msg("glBlendFunc", "forced src=GL_ONE dst=GL_ZERO");
  }
  real_glBlendFunc(sfactor, dfactor);
  gl_trace_err("glBlendFunc");
}

static void (*real_glEnableClientState)(unsigned int);
static void wrap_glEnableClientState(unsigned int array) {
  if (array == 0x8074)
    g_gl_client_vertex_seen = 1;
  else if (array == 0x8076)
    g_gl_client_color_seen = 1;
  else if (array == 0x8078)
    g_gl_client_tex_seen = 1;
  gl_probe_msg("glEnableClientState", "array=0x%x -> v=%d c=%d t=%d", array,
               g_gl_client_vertex_seen, g_gl_client_color_seen,
               g_gl_client_tex_seen);
  real_glEnableClientState(array);
  gl_trace_err("glEnableClientState");
}

static void (*real_glDisableClientState)(unsigned int);
static void wrap_glDisableClientState(unsigned int array) {
  if (array == 0x8074)
    g_gl_client_vertex_seen = 0;
  else if (array == 0x8076)
    g_gl_client_color_seen = 0;
  else if (array == 0x8078)
    g_gl_client_tex_seen = 0;
  gl_probe_msg("glDisableClientState", "array=0x%x -> v=%d c=%d t=%d", array,
               g_gl_client_vertex_seen, g_gl_client_color_seen,
               g_gl_client_tex_seen);
  real_glDisableClientState(array);
  gl_trace_err("glDisableClientState");
}

static void (*real_glColorMask)(unsigned char, unsigned char, unsigned char,
                                unsigned char);
static void wrap_glColorMask(unsigned char r, unsigned char g, unsigned char b,
                             unsigned char a) {
  gl_probe_msg("glColorMask", "%u %u %u %u", r, g, b, a);
  real_glColorMask(r, g, b, a);
  gl_trace_err("glColorMask");
}

static void (*real_glDepthMask)(unsigned char);
static void wrap_glDepthMask(unsigned char flag) {
  gl_probe_msg("glDepthMask", "%u", flag);
  real_glDepthMask(flag);
  gl_trace_err("glDepthMask");
}

static void (*real_glVertexPointer)(int, unsigned int, int, const void *);
static void wrap_glVertexPointer(int size, unsigned int type, int stride,
                                 const void *ptr) {
  gl_trace_msg("glVertexPointer", "size=%d type=0x%x stride=%d ptr=%p", size,
               type, stride, ptr);
  gl_probe_msg("glVertexPointer", "size=%d type=0x%x stride=%d ptr=%p", size,
               type, stride, ptr);
  gl_probe_pointer_sample("glVertexPointer", size, type, stride, ptr);
  real_glVertexPointer(size, type, stride, ptr);
  gl_trace_err("glVertexPointer");
}

static void (*real_glTexCoordPointer)(int, unsigned int, int, const void *);
static void wrap_glTexCoordPointer(int size, unsigned int type, int stride,
                                   const void *ptr) {
  gl_trace_msg("glTexCoordPointer", "size=%d type=0x%x stride=%d ptr=%p", size,
               type, stride, ptr);
  gl_probe_msg("glTexCoordPointer", "size=%d type=0x%x stride=%d ptr=%p", size,
               type, stride, ptr);
  gl_probe_pointer_sample("glTexCoordPointer", size, type, stride, ptr);
  real_glTexCoordPointer(size, type, stride, ptr);
  gl_trace_err("glTexCoordPointer");
}

static void (*real_glColorPointer)(int, unsigned int, int, const void *);
static void wrap_glColorPointer(int size, unsigned int type, int stride,
                                const void *ptr) {
  gl_trace_msg("glColorPointer", "size=%d type=0x%x stride=%d ptr=%p", size,
               type, stride, ptr);
  g_gl_color_ptr_seen = ptr;
  g_gl_color_size_seen = size;
  g_gl_color_type_seen = type;
  g_gl_color_stride_seen = stride;
  gl_probe_msg("glColorPointer", "size=%d type=0x%x stride=%d ptr=%p", size,
               type, stride, ptr);
  gl_probe_pointer_sample("glColorPointer", size, type, stride, ptr);
  real_glColorPointer(size, type, stride, ptr);
  gl_trace_err("glColorPointer");
}

static const unsigned char *(*real_glGetString)(unsigned);
static const unsigned char *wrap_glGetString(unsigned name) {
  const unsigned char *r = real_glGetString ? real_glGetString(name) : 0;
  debugPrintf("GL: glGetString(0x%x) -> '%s'\n", name,
              r ? (const char *)r : "(null)");
  return r;
}
static int (*real_glGetIntegerv_n)(unsigned, int *);

static void *sonic4ep1_gl_hook(const char *symbol, void *p) {
  if (!symbol || !p)
    return p;
  if (getenv("SONIC4EP1_GLLOG") && strcmp(symbol, "glGetString") == 0) {
    real_glGetString = (const unsigned char *(*)(unsigned))p;
    return (void *)wrap_glGetString;
  }
  if (!want_gl_hook())
    return p;
#define GL_HOOK(name) \
  if (strcmp(symbol, #name) == 0) { \
    real_##name = (__typeof__(real_##name))p; \
    return (void *)wrap_##name; \
  }
  GL_HOOK(glClearColor)
  GL_HOOK(glClear)
  GL_HOOK(glViewport)
  GL_HOOK(glScissor)
  GL_HOOK(glDrawArrays)
  GL_HOOK(glDrawElements)
  GL_HOOK(glBindBuffer)
  GL_HOOK(glBufferData)
  GL_HOOK(glBufferSubData)
  GL_HOOK(glGenBuffers)
  GL_HOOK(glLoadMatrixf)
  GL_HOOK(glMultMatrixf)
  GL_HOOK(glCurrentPaletteMatrixOES)
  GL_HOOK(glLoadPaletteFromModelViewMatrixOES)
  GL_HOOK(glMatrixIndexPointerOES)
  GL_HOOK(glWeightPointerOES)
  GL_HOOK(glTexImage2D)
  GL_HOOK(glCompressedTexImage2D)
  GL_HOOK(glBindTexture)
  GL_HOOK(glBlendFunc)
  GL_HOOK(glEnableClientState)
  GL_HOOK(glDisableClientState)
  GL_HOOK(glColorMask)
  GL_HOOK(glDepthMask)
  GL_HOOK(glEnable)
  GL_HOOK(glDisable)
  GL_HOOK(glMatrixMode)
  GL_HOOK(glLoadIdentity)
  GL_HOOK(glVertexPointer)
  GL_HOOK(glTexCoordPointer)
  GL_HOOK(glColorPointer)
#undef GL_HOOK
  return p;
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  install_crash_handler();
  install_probe_handler();
  debugPrintf("=== sonic4ep1 — loader ARMHF (Mali-450) ===\n");

  /* base = shims bionic→glibc (os 18 que o dlsym fallback não cobre) */
  extern DynLibFunction port_shims[];
  extern int port_shims_count;
  g_comb = malloc(sizeof(DynLibFunction) * port_shims_count);
  memcpy(g_comb, port_shims, sizeof(DynLibFunction) * port_shims_count);
  g_comb_n = port_shims_count;

  /* Principal primeiro: ela exporta s3eEdk* / s3e* que as extensoes usam. */
  if (load_module(SO_NAME, MEMORY_MB, 1) < 0) return 1;

  debugPrintf("entry: JNI_OnLoad=%p android_main=%p (combinada=%d símbolos)\n",
              (void *)g_main_jni_onload, (void *)g_main_android_main, g_comb_n);

  jni_shim_set_package("com.konami.pes2012", 1000005);
  void *vm = NULL;
  void *env = NULL;
  jni_shim_init(&vm, &env);
  g_fake_vm_for_ext = vm;

  if (g_main_jni_onload) {
    int (*JNI_OnLoad)(void *, void *) =
        (int (*)(void *, void *))g_main_jni_onload;
    int v = JNI_OnLoad(vm, NULL);
    debugPrintf("JNI_OnLoad -> 0x%x\n", v);
  } else {
    debugPrintf("AVISO: JNI_OnLoad nao encontrado\n");
  }
  jni_dump_natives();

  if (getenv("SONIC4EP1_RUN_NATIVE")) {
    void (*initNative)(void *, void *) = jni_find_native("initNative");
    void (*setViewNative)(void *, void *, void *) =
        jni_find_native("setViewNative");
    void (*runNative)(void *, void *, void *, void *, void *) =
        jni_find_native("runNative");
    set_pixels_native_fn setPixelsNative =
        (set_pixels_native_fn)jni_find_native("setPixelsNative");
    g_on_motion_event = (motion_native_fn)jni_find_native("onMotionEvent");
    debugPrintf("F3: onMotionEvent=%p\n", (void *)g_on_motion_event);
    void *thiz = (void *)0x53010001;
    void *view = (void *)0x53010002;
    const char *arg1 = env_or_default("SONIC4EP1_ARG1", "sonic4ep1.apk");
    const char *arg2 = env_or_default("SONIC4EP1_ARG2", ".");
    const char *arg3 = env_or_default("SONIC4EP1_ARG3", "");

    debugPrintf("=== F3: SDL/GL init + Marmalade runNative ===\n");
    debugPrintf("F3 args: arg1=\"%s\" arg2=\"%s\" arg3=\"%s\"\n", arg1, arg2,
                arg3);
    android_shim_init();
    egl_shim_create_window();
    /*
     * BOOTSTRAP do s3e device: o so-loader pula o onCreate/s3eDeviceCreate que
     * inicializa os subsistemas core (config/string-intern/file/surface). Sem
     * isso o initNative crasha usando config NULL. Chamamos a sequencia de init
     * dos subsistemas (a mesma do gate 0x13db0) ANTES do initNative.
     */
    if (g_main_text_base && getenv("SONIC4EP1_BOOTSTRAP")) { /* opt-in: crasha fora de ordem */
      uintptr_t b = g_main_text_base;
      debugPrintf("F3: s3e bootstrap (subsystem inits 0x13db0-seq)...\n");
      ((void (*)(void))(b + 0x59544))();
      ((void (*)(int))(b + 0x57280))(0);
      ((void (*)(void))(b + 0x5f798))();
      ((void (*)(void))(b + 0x7e1b8))();
      ((void (*)(void))(b + 0x5be40))();
      ((void (*)(void))(b + 0x7fa54))();
      ((void (*)(void))(b + 0x7c2c0))();
      debugPrintf("F3: s3e bootstrap OK\n");
    }
    if (initNative) {
      debugPrintf("F3: initNative\n");
      initNative(env, thiz);
    } else {
      debugPrintf("F3: initNative nao registrado\n");
    }
    if (setViewNative) {
      debugPrintf("F3: setViewNative(fake view=%p)\n", view);
      setViewNative(env, thiz, view);
    } else {
      debugPrintf("F3: setViewNative nao registrado\n");
    }
    if (setPixelsNative) {
      extern int sonic4ep1_screen_w;
      extern int sonic4ep1_screen_h;
      int pixel_count = sonic4ep1_screen_w * sonic4ep1_screen_h;
      size_t need = (size_t)pixel_count * sizeof(int);
      int *pixels;
      if (getenv("SONIC4EP1_GUARD_PIXELS")) {
        /* coloca o buffer colado numa pagina-guarda PROT_NONE: qualquer
         * overflow do engine falha NA HORA na instrucao exata (diagnostico). */
        long pg = sysconf(_SC_PAGESIZE);
        size_t body = (need + (size_t)pg - 1) & ~((size_t)pg - 1);
        unsigned char *m = (unsigned char *)mmap(NULL, body + (size_t)pg,
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        mprotect(m + body, (size_t)pg, PROT_NONE);
        pixels = (int *)(m + body - need);
        memset(pixels, 0, need);
        debugPrintf("F3: GUARD pixels=%p guard=%p\n", (void *)pixels,
                    (void *)(m + body));
      } else {
        /* margem de 256KB: se o engine escreve alem de w*h*4 (stride/POT), a
         * sobra cai aqui em vez de corromper o heap da glibc. */
        pixels = (int *)calloc(need + 262144u, 1);
      }
      void *pixel_array = jni_shim_make_array(pixels, pixel_count);
      debugPrintf("F3: setPixelsNative(%dx%d, pixels=%p array=%p)\n",
                  sonic4ep1_screen_w, sonic4ep1_screen_h, (void *)pixels,
                  pixel_array);
      setPixelsNative(env, view, sonic4ep1_screen_w, sonic4ep1_screen_h,
                      pixel_array, 1);
    } else {
      debugPrintf("F3: setPixelsNative nao registrado\n");
    }
    maybe_start_autotap(env, thiz);
    if (runNative) {
      debugPrintf("F3: runNative...\n");
      debugPrintf("F3: runNative@off=0x%lx initNative@off=0x%lx setViewNative@off=0x%lx\n",
                  g_main_text_base ? (unsigned long)((uintptr_t)runNative - g_main_text_base) : 0ul,
                  g_main_text_base ? (unsigned long)((uintptr_t)initNative - g_main_text_base) : 0ul,
                  g_main_text_base ? (unsigned long)((uintptr_t)setViewNative - g_main_text_base) : 0ul);
      snapshot_maps();
      runNative(env, thiz, jni_shim_new_string(arg1),
                jni_shim_new_string(arg2), jni_shim_new_string(arg3));
      debugPrintf("F3: runNative retornou\n");
      /* s6: chama DIRETO o s3e-file-loader (0x24504) com o path do exec. Ele faz
       * fopen("rb")+0x23054 (loader/decompressor XE3U). Normalmente so roda via
       * 0x248ac quando 0x2460c=0, mas 0x2460c retorna 1 (exec ja "registrado" na
       * tabela sem descomprimir) -> o code-load nunca dispara. Forcamos aqui. */
      if (g_main_text_base && getenv("PES_CALL_LOADER")) {
        uintptr_t b = g_main_text_base;
        int (*s3e_load)(const char *) =
            (int (*)(const char *))(b + 0x24504 + 1);
        const char *xp = env_or_default("PES_EXEC_PATH", "PES2012.s3e");
        debugPrintf("F3: chamando s3e-file-loader 0x24504(\"%s\")...\n", xp);
        int lr = s3e_load(xp);
        debugPrintf("F3: 0x24504 -> %d\n", lr);
      }
      if (!getenv("SONIC4EP1_EXIT_AFTER_RUN")) {
        /*
         * runNative inicializa o s3e e RETORNA (no Android os frames sao
         * dirigidos pela render thread do GLSurfaceView via doDraw/glSwapBuffers
         * -> runOnOSTickNative). Nao temos render thread, entao DIRIGIMOS o loop
         * de frames aqui: chamamos runOnOSTickNative (1 frame do s3e: update +
         * render) + present a cada iteracao.
         */
        void (*os_tick)(void *, void *) =
            (void (*)(void *, void *))jni_shim_os_tick_fn();
        if (!os_tick)
          os_tick = (void (*)(void *, void *))jni_find_native("runOnOSTickNative");
        debugPrintf("F3: os_tick=%p\n", (void *)os_tick);
        if (os_tick && !getenv("SONIC4EP1_NO_FRAMELOOP")) {
          debugPrintf("F3: dirigindo loop de frames (runOnOSTickNative)\n");
          /* PES usa o modelo YIELD/fibre (nao registra OS_TICK callback -> os_tick
           * 0x5a4c8 e no-op): o game main roda num fibre e deu s3eDeviceYield no
           * runNative; runOnOSTickNative nao o resume. Chamamos s3eDeviceUnYield
           * (0x40fcc) por-frame p/ RESUMIR o fibre do jogo (1 frame do game main). */
          /* pump selecionavel p/ testar o modelo yield/fibre do PES */
          const char *pump = getenv("PES_PUMP"); /* unyield|sched|execpush|none */
          uintptr_t b = g_main_text_base;
          void (*fn_unyield)(void) = (void (*)(void))(b + 0x40fcc + 1);
          void (*fn_sched)(void)   = (void (*)(void))(b + 0x2d400 + 1); /* wraps 0x2d39c */
          void (*fn_execpush)(int, int, int) =
              (void (*)(int, int, int))(b + 0x40cbc + 1);
          debugPrintf("F3: PES_PUMP=%s (b=%p)\n", pump ? pump : "(default sched)", (void *)b);
          for (;;) {
            egl_shim_bind_main();
            os_tick(env, thiz);
            if (b && pump) {
              if (!strcmp(pump, "sched")) fn_sched();
              else if (!strcmp(pump, "unyield")) fn_unyield();
              else if (!strcmp(pump, "execpush")) fn_execpush(0, 0, 0);
              /* default (pump==NULL) ou "none" -> so os_tick */
            }
            egl_shim_swap_main();
            usleep(16000); /* ~60fps */
          }
        }
        debugPrintf("F3: mantendo processo vivo para threads Marmalade\n");
        for (;;)
          pause();
      }
    } else {
      debugPrintf("F3: runNative nao registrado\n");
    }
  }

  debugPrintf("=== F2 OK: JNI_OnLoad chamado; nativos capturados. ===\n");
  return 0;
}
