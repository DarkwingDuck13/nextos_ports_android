/*
 * main.c — loader ARMHF (gerado por new-port-arm.sh) p/ sonic4ep1.
 *
 * Multi-módulo bionic→glibc. Marmalade e invertido em relacao aos jogos comuns:
 * as extensoes libs3eAPKExpansion/Dialog/Flurry dependem dos s3eEdk* exportados
 * por libs3e_android.so. Portanto carregamos a principal primeiro, tiramos
 * snapshot dos simbolos dela, e so entao carregamos as extensoes.
 */
#include <setjmp.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <errno.h>
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
#define SO_NAME "libs3e_android.so"

static uintptr_t g_main_text_base;
static size_t g_main_text_size;
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

#define MAP_SNAPSHOT_MAX 96
struct map_snapshot {
  uintptr_t start;
  uintptr_t end;
  unsigned long off;
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

/* ---- crash handler ARMHF (campos arm_pc/arm_r0/arm_lr do sigcontext 32-bit) ---- */
static void crash_handler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  mcontext_t *m = &uc->uc_mcontext;
  uintptr_t pc = m->arm_pc, lr = m->arm_lr, fault = (uintptr_t)info->si_addr;
  uintptr_t text = (uintptr_t)text_base;
  uintptr_t main_text = g_main_text_base;

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

static int marm_s3eDebugErrorShow(const char *title, const char *msg) {
  uintptr_t ra = (uintptr_t)__builtin_return_address(0);
  uintptr_t base = g_main_text_base;
  debugPrintf("hook: s3eDebugErrorShow: title=%s msg=%s [caller +0x%lx]\n",
              title ? title : "(null)", msg ? msg : "(null)",
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
  /* fallback case-insensitive no cwd e depois no deploy_dir (assets extraidos
   * do APK: AUDIO/, splash, etc.). So pra leitura — escrita ja foi criada acima. */
  int is_write = m && (strchr(m, 'w') || strchr(m, 'a') || strchr(m, '+'));
  if (!f && !is_write)
    f = ep1_ci_open(".", s, m);
  if (!f && !is_write && g_deploy_dir[0])
    f = ep1_ci_open(g_deploy_dir, s, m);
  if (getenv("SONIC4EP1_FILELOG"))
    debugPrintf("s3eFile: open '%s' (real '%s' mode '%s') -> %p\n", p, s, m,
                (void *)f);
  return f;
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

static void *my_s3eFileOpen(const char *fn, const char *mode) {
  FILE *f = ep1_file_real_open(fn, mode);
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
  if (f && ep1_track_close((FILE *)f))
    fclose((FILE *)f); /* so fecha se ainda estava aberto (ignora double-close) */
  return 0;            /* S3E_RESULT_SUCCESS */
}
static int my_s3eFileRead(void *buf, unsigned int esz, unsigned int n,
                          void *f) {
  if (!f || esz == 0)
    return 0;
  int got = (int)fread(buf, esz, n, (FILE *)f);
  if (getenv("SONIC4EP1_FILELOG")) {
    static int rc = 0;
    if (rc++ < 20)
      debugPrintf("s3eFile: READ f=%p esz=%u n=%u -> %d\n", f, esz, n, got);
  }
  return got;
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
  int whence = origin == 1 ? SEEK_CUR : origin == 2 ? SEEK_END : SEEK_SET;
  return fseek((FILE *)f, off, whence) == 0 ? 0 : 1;
}
static int my_s3eFileTell(void *f) { return f ? (int)ftell((FILE *)f) : -1; }
static unsigned int my_s3eFileGetSize(void *f) {
  if (!f)
    return 0;
  long cur = ftell((FILE *)f);
  fseek((FILE *)f, 0, SEEK_END);
  long sz = ftell((FILE *)f);
  fseek((FILE *)f, cur, SEEK_SET);
  return (unsigned int)(sz < 0 ? 0 : sz);
}
static int my_s3eFileCheckExists(const char *fn) {
  if (!fn)
    return 0;
  const char *s = fn;
  const char *colon = strstr(fn, "://");
  if (colon)
    s = colon + 3;
  while (*s == '/')
    s++;
  int ok = (access(s, F_OK) == 0) || (s != fn && access(fn, F_OK) == 0);
  return ok ? 1 : 0; /* S3E_TRUE/FALSE */
}
static char *my_s3eFileReadString(char *str, unsigned int maxLen, void *f) {
  if (!f || !str || maxLen == 0)
    return NULL;
  return fgets(str, (int)maxLen, (FILE *)f);
}
static int my_s3eFileFlush(void *f) {
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
  uintptr_t base = g_main_text_base ? g_main_text_base : (uintptr_t)text_base;

  so_make_text_writable();

  install_s3efile_shims(base);
  install_s3econfig_shims(base);

  /*
   * SURFACE FLUSH stub: 0x8330c processa a fila de surfaces (present) lendo o
   * surface object via TLS (NULL no nosso ambiente sem render thread) -> crash
   * em 0x8333c. O present real do jogo vai por doDraw/glSwapBuffers (JNI ->
   * egl_shim), entao stubamos esse flush. Retorna void; ret0 e seguro.
   */
  if (base && !getenv("SONIC4EP1_NO_SURFSTUB")) {
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
  if (base && !getenv("SONIC4EP1_NO_CAPFIX")) {
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
  if (base && !getenv("SONIC4EP1_NO_ICF_INJECT")) {
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
  return getenv("SONIC4EP1_TRACE_GL") || getenv("SONIC4EP1_SKIP_NAN_MATRIX");
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
  real_glDrawArrays(mode, first, count);
  gl_trace_err("glDrawArrays");
}

static void (*real_glDrawElements)(unsigned int, int, unsigned int, const void *);
static void wrap_glDrawElements(unsigned int mode, int count, unsigned int type,
                                const void *indices) {
  gl_trace_msg("glDrawElements", "mode=0x%x count=%d type=0x%x indices=%p",
               mode, count, type, indices);
  real_glDrawElements(mode, count, type, indices);
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
  } else {
    gl_trace_msg("glLoadMatrixf", "NULL");
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
  gl_trace_err("glCompressedTexImage2D");
}

GL_WRAP_VOID1(glEnable, unsigned int, "cap=0x%x")
GL_WRAP_VOID1(glDisable, unsigned int, "cap=0x%x")
GL_WRAP_VOID1(glMatrixMode, unsigned int, "mode=0x%x")
GL_WRAP_VOID0(glLoadIdentity)

static void (*real_glBindTexture)(unsigned int, unsigned int);
static void wrap_glBindTexture(unsigned int target, unsigned int texture) {
  gl_trace_msg("glBindTexture", "target=0x%x texture=%u", target, texture);
  real_glBindTexture(target, texture);
  gl_trace_err("glBindTexture");
}

static void (*real_glVertexPointer)(int, unsigned int, int, const void *);
static void wrap_glVertexPointer(int size, unsigned int type, int stride,
                                 const void *ptr) {
  gl_trace_msg("glVertexPointer", "size=%d type=0x%x stride=%d ptr=%p", size,
               type, stride, ptr);
  real_glVertexPointer(size, type, stride, ptr);
  gl_trace_err("glVertexPointer");
}

static void (*real_glTexCoordPointer)(int, unsigned int, int, const void *);
static void wrap_glTexCoordPointer(int size, unsigned int type, int stride,
                                   const void *ptr) {
  gl_trace_msg("glTexCoordPointer", "size=%d type=0x%x stride=%d ptr=%p", size,
               type, stride, ptr);
  real_glTexCoordPointer(size, type, stride, ptr);
  gl_trace_err("glTexCoordPointer");
}

static void (*real_glColorPointer)(int, unsigned int, int, const void *);
static void wrap_glColorPointer(int size, unsigned int type, int stride,
                                const void *ptr) {
  gl_trace_msg("glColorPointer", "size=%d type=0x%x stride=%d ptr=%p", size,
               type, stride, ptr);
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
  if (load_module("libs3eAPKExpansion.so", 8, 1) < 0) return 1;
  if (load_module("libs3eDialog.so", 8, 1) < 0) return 1;
  if (load_module("libs3eFlurry.so", 8, 1) < 0) return 1;
  install_apkexpansion_hooks();

  debugPrintf("entry: JNI_OnLoad=%p android_main=%p (combinada=%d símbolos)\n",
              (void *)g_main_jni_onload, (void *)g_main_android_main, g_comb_n);

  jni_shim_set_package("com.sega.sonic4epi", 6200011);
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
      snapshot_maps();
      runNative(env, thiz, jni_shim_new_string(arg1),
                jni_shim_new_string(arg2), jni_shim_new_string(arg3));
      debugPrintf("F3: runNative retornou\n");
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
          for (;;) {
            egl_shim_bind_main();
            os_tick(env, thiz);
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
