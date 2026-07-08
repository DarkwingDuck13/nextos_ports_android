/*
 * main.c -- Bully: Anniversary Edition (build Android) so-loader p/ NextOS
 * aarch64 + Mali-450 fbdev. Loader de DOIS módulos (libc++_shared + libGame),
 * igual ao reVC, mas entry = JNI estático (jni_load), não SDL_main.
 *
 * Módulo A = libc++_shared.so (ABI __ndk1) -> std C++ runtime.
 * Módulo B = libGame.so (engine). Resolve via: bully_stub_table + pthread_bridge
 *            + símbolos do libc++ (snapshot) + dlsym(RTLD_DEFAULT) das libs do
 *            device (SDL2/GLESv2/EGL/OpenAL/mpg123/glibc) pré-carregadas GLOBAL.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "so_util.h"

#define CXX_SO  "libc++_shared.so"
#define GAME_SO "libGTASA.so"
#define CXX_HEAP_MB  48
#define GAME_HEAP_MB 128

/* dummy Module p/ os externs do jni_shim (compat so_util_x64.h) */
int mod_game, mod_cxx;

/* 🩹 CANARY BIONIC (causa-raiz do "stack smashing detected" em alguns devices,
 * ex: H700/Mali-G31 Knulli/muOS): o libGame (compilado p/ bionic) le a
 * stack-guard de tpidr_el0+0x28 (TLS_SLOT_STACK_GUARD do Android). Sob glibc
 * esse endereco cai num campo do TCB que MUDA durante a execucao (instavel em
 * threads recem-criadas como a GameMain) -> prologo le X, epilogo le Y ->
 * __stack_chk_fail -> abort. Em outras glibc (NextOS 2.43, X5M) o slot calhava
 * estavel, por isso so quebrava em alguns devices. FIX (proven no Dysmantle):
 * reservar um TLS local NA IMAGEM com um pad NUNCA escrito -> tpidr+0x28 fica
 * estavel p/ toda thread -> a canary nunca mais da mismatch. `used` impede o
 * linker de descartar; ancorado por leitura volatil no main(). */
__attribute__((used, aligned(16))) _Thread_local char g_bionic_guard_pad[256];

extern DynLibFunction bully_stub_table[];
extern const int bully_stub_count;
extern DynLibFunction revc_pthread_table[];
extern const int revc_pthread_count;
extern DynLibFunction gtasa_stub_table[];   /* stubs Android/Rockstar/OpenSL/cxa do GTA SA */
extern const int gtasa_stub_count;
extern void bully_imports_init(void);
extern void jni_load(void); /* driver JNI estático (jni_shim.c) */

static DynLibFunction *g_base;
static int g_base_n;

static void build_base_table(void) {
  g_base_n = bully_stub_count + revc_pthread_count;
  g_base = malloc(sizeof(DynLibFunction) * g_base_n);
  memcpy(g_base, bully_stub_table, sizeof(DynLibFunction) * bully_stub_count);
  memcpy(g_base + bully_stub_count, revc_pthread_table,
         sizeof(DynLibFunction) * revc_pthread_count);
}

/* dump de contexto (regs + stack scan) — compartilhado por crash_handler e o
 * diag_handler (SIGUSR1, p/ ver onde cada thread do jogo está bloqueada). */
static void dump_ctx(void *uc) {
  uintptr_t tb = (uintptr_t)text_base;
#if defined(__aarch64__)
  ucontext_t *u = (ucontext_t *)uc;
  uintptr_t pc = u->uc_mcontext.pc;
  uintptr_t lr = u->uc_mcontext.regs[30];
  uintptr_t sp = u->uc_mcontext.sp;
  fprintf(stderr, "  PC=%p", (void *)pc);
  if (tb && pc >= tb && pc < tb + text_size)
    fprintf(stderr, " = libGame+0x%lx", (unsigned long)(pc - tb));
  Dl_info di;
  if (dladdr((void *)pc, &di) && di.dli_fname)
    fprintf(stderr, " [%s%s%s+0x%lx]", di.dli_fname,
            di.dli_sname ? " " : "", di.dli_sname ? di.dli_sname : "",
            (unsigned long)(pc - (uintptr_t)(di.dli_saddr ? di.dli_saddr : di.dli_fbase)));
  fprintf(stderr, "\n");
  /* LR (x30) = quem chamou -> normalmente aponta p/ libGame (offset mapeável) */
  fprintf(stderr, "  LR=%p", (void *)lr);
  if (tb && lr >= tb && lr < tb + text_size)
    fprintf(stderr, " = libGame+0x%lx", (unsigned long)(lr - tb));
  if (dladdr((void *)lr, &di) && di.dli_fname)
    fprintf(stderr, " [%s+0x%lx]", di.dli_fname,
            (unsigned long)(lr - (uintptr_t)di.dli_fbase));
  fprintf(stderr, "  SP=%p\n", (void *)sp);
  for (int r = 0; r <= 30; r += 2) {
    uintptr_t a = u->uc_mcontext.regs[r], b = u->uc_mcontext.regs[r + 1];
    fprintf(stderr, "  x%-2d=%016lx  x%-2d=%016lx", r, (unsigned long)a, r + 1, (unsigned long)b);
    if (tb && a >= tb && a < tb + text_size) fprintf(stderr, "  (x%d=libGame+0x%lx)", r, (unsigned long)(a - tb));
    if (tb && b >= tb && b < tb + text_size) fprintf(stderr, "  (x%d=libGame+0x%lx)", r + 1, (unsigned long)(b - tb));
    fprintf(stderr, "\n");
  }
  /* backtrace REAL via cadeia de frame pointer x29: [x29]=fp anterior,
   * [x29+8]=LR salvo. Muito mais confiável que varrer a stack. */
  fprintf(stderr, "  --- backtrace (x29 chain) ---\n");
  uintptr_t fp = u->uc_mcontext.regs[29];
  fprintf(stderr, "    #0 PC libGame+0x%lx\n", tb && pc>=tb && pc<tb+text_size ? (unsigned long)(pc-tb) : 0);
  for (int i = 1; i < 24 && fp && fp > 0x1000 && (fp & 7) == 0; i++) {
    uintptr_t nfp = ((uintptr_t *)fp)[0];
    uintptr_t ret = ((uintptr_t *)fp)[1];
    if (tb && ret >= tb && ret < tb + text_size)
      fprintf(stderr, "    #%d libGame+0x%lx\n", i, (unsigned long)(ret - tb));
    else {
      Dl_info d2;
      if (dladdr((void *)ret, &d2) && d2.dli_fname)
        fprintf(stderr, "    #%d %s+0x%lx\n", i, d2.dli_fname, (unsigned long)(ret - (uintptr_t)d2.dli_fbase));
      else fprintf(stderr, "    #%d %p\n", i, (void *)ret);
    }
    if (nfp <= fp) break;  /* fp deve crescer */
    fp = nfp;
  }
  /* stack scan raso como backup */
  fprintf(stderr, "  --- stack scan ---\n");
  uintptr_t *stk = (uintptr_t *)sp;
  for (int i = 0; i < 96 && stk; i++) {
    uintptr_t v = stk[i];
    if (tb && v >= tb && v < tb + text_size)
      fprintf(stderr, "    sp+0x%03x: libGame+0x%lx\n", i * 8, (unsigned long)(v - tb));
  }
#else
  (void)uc; (void)tb;
#endif
  fflush(stderr);
}
static void crash_handler(int sig, siginfo_t *info, void *uc) {
  uintptr_t tb = (uintptr_t)text_base;
  uintptr_t fault = (uintptr_t)info->si_addr;
  fprintf(stderr, "\n=== CRASH sig=%d addr=%p ===\n", sig, info->si_addr);
  if (tb && fault >= tb && fault < tb + text_size)
    fprintf(stderr, "  libGame+0x%lx (text_base=%p)\n", (unsigned long)(fault - tb), text_base);
  else
    fprintf(stderr, "  fora do libGame text (text_base=%p size=%zu)\n", text_base, text_size);
  dump_ctx(uc);
  _exit(128 + sig);
}
/* SIGUSR1: dump NÃO-fatal (diagnóstico de deadlock). O main manda p/ cada thread
 * do jogo -> imprime o PC (libGame+offset) de onde ela está parada. */
static void diag_handler(int sig, siginfo_t *info, void *uc) {
  (void)info;
  fprintf(stderr, "\n=== DIAG thread tid=%d sig=%d ===\n", (int)syscall(SYS_gettid), sig);
  dump_ctx(uc);
}
static void install_crash_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);  /* "double free/corruption" do glibc -> abort -> pega backtrace */
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = diag_handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGUSR1, &sa, NULL);
}
/* dispara o dump de todas as threads-irmãs (chamado do keep-alive) */
void gtasa_diag_all_threads(void) {
  int self = (int)syscall(SYS_gettid);
  int pid = getpid();
  DIR *d = opendir("/proc/self/task");
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    int tid = atoi(e->d_name);
    if (tid <= 0 || tid == self) continue;
    syscall(SYS_tgkill, pid, tid, SIGUSR1);
    usleep(50000);
  }
  closedir(d);
}

static void preload_device_libs(void) {
  static const char *libs[] = {
      "libSDL2-2.0.so.0", "libGLESv2.so",   "libEGL.so",
      "libopenal.so.1",   "libmpg123.so.0", "libm.so.6", NULL,
  };
  for (int i = 0; libs[i]; i++) {
    void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr, "preload: %s %s\n", libs[i], h ? "OK" : dlerror());
  }
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

/* ---- modo SPLASH de setup: tela GLES2 propria (extração/sem-APK) lendo o progresso
 * de um arquivo (o launcher atualiza enquanto faz o unzip). NAO carrega o libGame ->
 * standalone, roda em qualquer display/resolucao. Gate BULLY_SETUPSPLASH. ---- */
extern int bully_init_gl(void);
extern void bully_swap_buffers(void);
extern void bully_setup_draw(int phase, const char *status, int cur, int total);
static void run_setup_splash(void) {
  if (!bully_init_gl()) { fprintf(stderr, "[splash] sem GL\n"); return; }
  const char *pf = getenv("BULLY_SETUP_FILE"); if (!pf) pf = "/tmp/bully_setup.txt";
  const char *sf = getenv("BULLY_SETUP_STOP"); if (!sf) sf = "/tmp/bully_setup_stop";
  for (;;) {
    int phase = 1, cur = 0, total = 0; char status[192] = "";
    FILE *f = fopen(pf, "r");
    if (f) { if (fscanf(f, "%d %d %d ", &phase, &cur, &total) < 1) phase = 1;
             if (fgets(status, sizeof(status), f)) { size_t L = strlen(status); if (L && status[L-1]=='\n') status[L-1]='\0'; }
             fclose(f); }
    bully_setup_draw(phase, status[0] ? status : NULL, cur, total);
    bully_swap_buffers();
    if (access(sf, F_OK) == 0) break;
    usleep(60000);
  }
  fprintf(stderr, "[splash] fim\n");
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  { volatile char c = g_bionic_guard_pad[0]; (void)c; } /* ancora o TLS pad (nunca escreve) */
  install_crash_handler();
  fprintf(stderr, "=== Bully (Android) so-loader / NextOS aarch64 Mali-450 ===\n");

  bully_imports_init();
  preload_device_libs();
  if (getenv("BULLY_SETUPSPLASH")) { run_setup_splash(); return 0; } /* tela de setup standalone */
  if (getenv("BULLY_HALVEBAKE")) { extern void bully_halve_cache(void); bully_halve_cache(); return 0; } /* re-encoda cache ETC2 a 1/2 res (1GB) */
  build_base_table();

  /* Módulo A: libc++_shared.so */
  load_module(CXX_SO, CXX_HEAP_MB, g_base, g_base_n);
  int cxx_n = 0;
  DynLibFunction *cxx_tbl = so_snapshot_symbols(&cxx_n);
  if (!cxx_tbl || cxx_n <= 0) { fprintf(stderr, "snapshot %s vazio\n", CXX_SO); exit(1); }
  fprintf(stderr, "libc++: %d símbolos exportados\n", cxx_n);

  /* tabela combinada p/ o módulo B: base(stubs+pthread) + libc++ + stubs GTA SA.
   * gtasa_stub_table por ÚLTIMO (nomes exclusivos do SA: cxa_guard, cloud*,
   * SL_IID*, social club; sem colidir com os do bully) -> cobre os que ficariam
   * UNRESOLVED. O resto (libc/GLES/@LIBC) resolve via dlsym no so_resolve. */
  int comb_n = g_base_n + cxx_n + gtasa_stub_count;
  DynLibFunction *comb = malloc(sizeof(DynLibFunction) * comb_n);
  memcpy(comb, g_base, sizeof(DynLibFunction) * g_base_n);
  memcpy(comb + g_base_n, cxx_tbl, sizeof(DynLibFunction) * cxx_n);
  memcpy(comb + g_base_n + cxx_n, gtasa_stub_table,
         sizeof(DynLibFunction) * gtasa_stub_count);

  /* Módulo B: libGame.so */
  load_module(GAME_SO, GAME_HEAP_MB, comb, comb_n);

  /* driver JNI estático (build_env -> hooks -> impl* -> frame loop) */
  fprintf(stderr, "=== rodando jni_load (driver) ===\n");
  jni_load();

  fprintf(stderr, "=== jni_load retornou ===\n");
  return 0;
}
