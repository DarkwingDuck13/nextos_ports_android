/*
 * main.c -- NieR (NierM, UE4 4.24.3 Android) so-loader p/ NextOS aarch64 / Mali-450.
 *
 * Loader de DOIS módulos (igual Bully/reVC), mas entry = android_main (NativeActivity):
 *   Módulo A = libgnustl_shared.so  -> provê o C++ runtime gnustl (std::string COW ABI
 *              `_ZNSs*`, std::ostream, condition_variable, type_info vtables, etc.)
 *   Módulo B = libUE4.so (engine)   -> resolve UND via:
 *              dynlib_functions (egl_shim + gl + ovrp stubs) + revc_pthread_table
 *              + snapshot(gnustl) + dlsym(RTLD_DEFAULT) das libs do device
 *              (SDL2/GLESv2/EGL/glibc/libm/libgcc) pré-carregadas GLOBAL.
 *
 * UE4 chama android_main(struct android_app*) (glue NativeActivity). O android_shim
 * fabrica o app + JNIEnv falso (jni_shim) + SDL. RHI: UE4 escolhe OpenGL/Vulkan; no
 * Mali-450 só há GLES2 -> egl_shim dá contexto ES2 via SDL2-mali.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "android_shim.h"
#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

#define GNUSTL_SO "libgnustl_shared.so"
#define GAME_SO   "libUE4.so"
#define GNUSTL_HEAP_MB 48
#define GAME_HEAP_MB   400   /* device .164 tem 832MB RAM / ~461 livre; mmap anon e' lazy */

int mod_game, mod_cxx; /* compat externs */

/* 🩹 CANARY BIONIC: libUE4 (bionic) lê stack-guard de tpidr_el0+0x28; sob glibc esse
 * slot é instável em threads novas -> __stack_chk_fail. Pad TLS nunca-escrito estabiliza.
 * (proven Dysmantle/Bully). */
__attribute__((used, aligned(16))) _Thread_local char g_bionic_guard_pad[256];

extern DynLibFunction revc_pthread_table[];
extern const int revc_pthread_count;
extern void nier_set_gmalloc_var(void **v);
extern void *nier_FMemory_Malloc(unsigned long long count, unsigned int align);
extern void *nier_FMemory_Realloc(void *ptr, unsigned long long count, unsigned int align);
extern void nier_FMemory_Free(void *ptr);
extern unsigned long long nier_FMemory_GetAllocSize(void *ptr);
extern unsigned long long nier_FMemory_QuantizeSize(unsigned long long count, unsigned int align);
extern int nier_CheckVerifyFailed(const char *expr, const char *file, int line, const unsigned short *fmt);
extern void *nier_GetHighPerfStat(void);
extern void *nier_GetHardwareWindow(void);
extern int nier_GetKeyMap(void *keys, void *names, unsigned max);
extern int nier_ComputeScreenDensity(int *out_density);

/* ---------------- crash handler (offset relativo ao libUE4) ---------------- */
/* atribui um endereço a uma região de /proc/self/maps (lib + offset no arquivo) */
static void attribute_addr(const char *lbl, uintptr_t a) {
  FILE *m = fopen("/proc/self/maps", "r");
  if (!m) { fprintf(stderr, "  %-3s %p\n", lbl, (void *)a); return; }
  char line[512]; int found = 0;
  while (fgets(line, sizeof(line), m)) {
    uintptr_t s, e; unsigned long off; char perms[8], path[300];
    path[0] = 0;
    if (sscanf(line, "%lx-%lx %7s %lx %*x:%*x %*u %299[^\n]", &s, &e, perms, &off, path) >= 4) {
      if (a >= s && a < e) {
        const char *base = path[0] ? path : "[anon]";
        const char *slash = base; for (const char *q = base; *q; q++) if (*q == '/') slash = q + 1;
        fprintf(stderr, "  %-3s %p  %s+0x%lx\n", lbl, (void *)a, slash, off + (unsigned long)(a - s));
        found = 1; break;
      }
    }
  }
  fclose(m);
  if (!found) fprintf(stderr, "  %-3s %p  (sem regiao)\n", lbl, (void *)a);
}

static int g_brk_skips = 0;
static void crash_handler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  uintptr_t pc = uc->uc_mcontext.pc;
  uintptr_t fault = (uintptr_t)info->si_addr;
  uintptr_t tb = (uintptr_t)text_base;

  /* ESTUDO: pular `brk` (PLATFORM_BREAK do check()) no texto do libUE4 -> seguir o init.
   * brk #imm = 0xD4200000..0xD43FFFFF. Gate NIER_NOBRKSKIP. */
  if ((sig == SIGTRAP || sig == SIGILL) && tb && pc >= tb && pc < tb + text_size &&
      !getenv("NIER_NOBRKSKIP")) {
    uint32_t insn = *(uint32_t *)pc;
    if ((insn & 0xFFE0001Fu) == 0xD4200000u) {
      if (g_brk_skips < 20)
        fprintf(stderr, "[brk-skip] libUE4+0x%lx (brk) -> pula\n", (unsigned long)(pc - tb));
      if (++g_brk_skips > 50000) {
        fprintf(stderr, "[brk-skip] >50000 skips (loop de check fatal) -> desisto\n");
        _exit(42);
      }
      uc->uc_mcontext.pc = pc + 4;
      return;
    }
  }
  fprintf(stderr, "\n=== CRASH sig=%d (%s) addr=%p ===\n", sig,
          sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" :
          sig == SIGABRT ? "SIGABRT" : sig == SIGILL ? "SIGILL" : "?",
          (void *)fault);
  fprintf(stderr, "PC=%p", (void *)pc);
  if (tb && pc >= tb && pc < tb + text_size)
    fprintf(stderr, " = libUE4+0x%lx", (unsigned long)(pc - tb));
  fprintf(stderr, "\n  text_base=%p size=0x%zx data_base=%p size=0x%zx\n",
          text_base, text_size, data_base, data_size);
  /* atribui PC + LR (caller imediato, mais confiavel que FP-walk) + x0..x4 */
  attribute_addr("PC", pc);
  attribute_addr("LR", uc->uc_mcontext.regs[30]);
  fprintf(stderr, "  x0=%lx x1=%lx x2=%lx x3=%lx x4=%lx x8=%lx\n",
          (unsigned long)uc->uc_mcontext.regs[0], (unsigned long)uc->uc_mcontext.regs[1],
          (unsigned long)uc->uc_mcontext.regs[2], (unsigned long)uc->uc_mcontext.regs[3],
          (unsigned long)uc->uc_mcontext.regs[4], (unsigned long)uc->uc_mcontext.regs[8]);

  /* 🔬 dump dos globais de feature-level (p/ achar valores ES3.1 a forcar) */
  if (tb) {
    int fd2 = open("/proc/self/mem", 0);
    if (fd2 >= 0) {
      int fl = -1, sp = -1, glmaj = -1; int spfl[4] = {-1,-1,-1,-1};
      pread(fd2, &fl,   4, (off_t)(tb + 0xaef5a30)); /* GMaxRHIFeatureLevel */
      pread(fd2, &sp,   4, (off_t)(tb + 0xb02ab3c)); /* GMaxRHIShaderPlatform */
      pread(fd2, spfl, 16, (off_t)(tb + 0xaef5a78)); /* GShaderPlatformForFeatureLevel[4] */
      pread(fd2, &glmaj,4, (off_t)(tb + 0xb180838)); /* FAndroidOpenGL::GLMajorVerion */
      fprintf(stderr, "  [RHI] GMaxRHIFeatureLevel=%d GMaxRHIShaderPlatform=%d GLMajorVer=%d\n", fl, sp, glmaj);
      fprintf(stderr, "  [RHI] GShaderPlatformForFeatureLevel[ES2,ES3_1,SM4,SM5]=%d,%d,%d,%d\n",
              spfl[0], spfl[1], spfl[2], spfl[3]);
      close(fd2);
    }
    fflush(stderr);
  }

  /* 🔬 DIAGNOSTICO TSet::EmplaceImpl (camada 16): no crash @libUE4+0x445d294 dumpa o
   * objeto TSet (x21=this) p/ ver se o problema e' hash-ptr NULL, HashSize, ou Elements.Data.
   * Leitura via /proc/self/mem (pread) p/ nunca re-faltar dentro do handler. */
  if (tb && pc == tb + 0x445d294) {
    unsigned long *R = (unsigned long *)uc->uc_mcontext.regs;
    fprintf(stderr, "  [TSet] x21(this)=%lx x9(Elem.Data)=%lx x11(idx*12)=%lx x24(elemId)=%lx\n",
            R[21], R[9], R[11], R[24]);
    int fd = open("/proc/self/mem", 0 /*O_RDONLY*/);
    if (fd >= 0) {
      unsigned long this_ = R[21];
      unsigned long edata = 0; unsigned int n8 = 0, n52 = 0, hashsz = 0; unsigned long hashptr = 0;
      unsigned int inl[8] = {0};
      pread(fd, &edata,   8, (off_t)this_);
      pread(fd, &n8,      4, (off_t)this_ + 8);
      pread(fd, &n52,     4, (off_t)this_ + 52);
      pread(fd, &hashptr, 8, (off_t)this_ + 64);
      pread(fd, &hashsz,  4, (off_t)this_ + 72);
      pread(fd, inl,     32, (off_t)this_ + 0x38);
      fprintf(stderr, "  [TSet] Elements.Data=%lx Num[+8]=%u Num[+52]=%u  HASH-PTR[+64]=%lx HashSize[+72]=%u\n",
              edata, n8, n52, hashptr, hashsz);
      fprintf(stderr, "  [TSet] inline[+0x38]= %08x %08x %08x %08x %08x %08x %08x %08x\n",
              inl[0], inl[1], inl[2], inl[3], inl[4], inl[5], inl[6], inl[7]);
      close(fd);
    }
    fflush(stderr);
  }
  /* backtrace via x29/lr chain */
  uintptr_t fp = uc->uc_mcontext.regs[29];
  for (int f = 1; f < 24 && fp; f++) {
    uintptr_t *p = (uintptr_t *)fp, nfp = p[0], lr = p[1];
    if (!lr) break;
    if (tb && lr >= tb && lr < tb + text_size) {
      char lbl[16]; snprintf(lbl, sizeof(lbl), "#%d", f);
      fprintf(stderr, "  %-3s lr %p (libUE4+0x%lx)\n", lbl, (void *)lr, (unsigned long)(lr - tb));
    } else {
      char lbl[16]; snprintf(lbl, sizeof(lbl), "#%d", f);
      attribute_addr(lbl, lr);
    }
    if (nfp <= fp) break;
    fp = nfp;
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
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGTRAP, &sa, NULL);
}

static void preload_device_libs(void) {
  static const char *libs[] = {
      "libSDL2-2.0.so.0", "libGLESv2.so", "libEGL.so", "libm.so.6",
      "libz.so.1", "libstdc++.so.6", "libgcc_s.so.1", NULL,
  };
  for (int i = 0; libs[i]; i++) {
    void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr, "preload: %s %s\n", libs[i], h ? "OK" : dlerror());
  }
}

/* 🩹 Trace/UnrealInsights deadlock: Writer_Initialize() spawna thread que fica em
 * usleep e a main bloqueia num futex esperando-a (deadlock no init_array). UE4 roda
 * sem Trace -> patcha o prólogo p/ `ret`. PRECISA rodar ANTES do init_array. */
static void patch_kill_trace(void) {
  if (getenv("NIER_TRACE")) return;
  static const char *syms[] = {
      "_ZN5Trace7Private17Writer_InitializeEv",
      "_ZN5Trace7Private23Writer_InitializeTimingEv", NULL};
  so_make_text_writable();
  for (int i = 0; syms[i]; i++) {
    uintptr_t a = so_find_addr(syms[i]);
    if (a) { *(uint32_t *)a = 0xd65f03c0u; /* ret */
             fprintf(stderr, "[patch] %s @ %p -> ret\n", syms[i], (void *)a); }
    else     fprintf(stderr, "[patch] %s NAO encontrado\n", syms[i]);
  }

  /* FIX bootstrap do GMalloc: resolve &GMalloc (slot em libUE4+0xae94c20) e hooka
   * FMemory::Malloc p/ devolver memalign enquanto GMalloc==NULL. (texto ainda writable!) */
  if (!getenv("NIER_NOMALLOCFIX")) {
    void **slot = (void **)((uintptr_t)text_base + 0xae94c20); /* contém &GMalloc */
    void **gmalloc_var = (void **)(*slot);
    nier_set_gmalloc_var(gmalloc_var);
    struct { const char *sym; void *fn; } H[] = {
        {"_ZN7FMemory6MallocEyj",     (void *)&nier_FMemory_Malloc},
        {"_ZN7FMemory7ReallocEPvyj",  (void *)&nier_FMemory_Realloc},
        {"_ZN7FMemory4FreeEPv",       (void *)&nier_FMemory_Free},
        {"_ZN7FMemory12GetAllocSizeEPv", (void *)&nier_FMemory_GetAllocSize},
        {"_ZN7FMemory12QuantizeSizeEyj", (void *)&nier_FMemory_QuantizeSize},
    };
    for (unsigned i = 0; i < sizeof(H)/sizeof(H[0]); i++) {
      uintptr_t a = so_find_addr(H[i].sym);
      if (a) { hook_arm64(a, (uintptr_t)H[i].fn);
               fprintf(stderr, "[patch] %s @ %p hooked\n", H[i].sym, (void *)a); }
      else     fprintf(stderr, "[patch] %s NAO encontrado\n", H[i].sym);
    }
    fprintf(stderr, "[patch] GMalloc var=%p\n", (void *)gmalloc_var);

    /* GetKeyMap -> 0 (lixo de contagem corrompe TSet no loop da AndroidMain) */
    if (!getenv("NIER_NOKEYMAP")) {
      uintptr_t km = (uintptr_t)text_base + 0x51058ac;
      hook_arm64(km, (uintptr_t)&nier_GetKeyMap);
      fprintf(stderr, "[patch] FAndroidPlatformInput::GetKeyMap @ %p -> 0\n", (void *)km);
    }

    /* BYPASS (NIER_FORCE_ES31): pula o fatal de UBO layout em CompileGlobalShaderMap
     * (Resources==TexExpr*2; mismatch estrutural sampler ES3.1-vs-ES2). NOP nos branches
     * que entram no bloco fatal (0x755f0ac). */
    if (getenv("NIER_FORCE_ES31")) {
      *(uint32_t *)((uintptr_t)text_base + 0x755f058) = 0xd503201fu; /* cbz x22 -> nop */
      *(uint32_t *)((uintptr_t)text_base + 0x755f070) = 0xd503201fu; /* b.ne fatal -> nop */
      *(uint32_t *)((uintptr_t)text_base + 0x755f098) = 0xd503201fu; /* b.ne fatal -> nop */
      fprintf(stderr, "[patch] UBO-layout fatal bypass (CompileGlobalShaderMap)\n");
    }

    /* ComputePhysicalScreenDensity -> DPI fixo (string de device vazia crasha no FSlateApplication) */
    {
      uintptr_t cd = (uintptr_t)text_base + 0x513717c;
      hook_arm64(cd, (uintptr_t)&nier_ComputeScreenDensity);
      fprintf(stderr, "[patch] ComputePhysicalScreenDensity @ %p -> DPI 320\n", (void *)cd);
    }

    /* GetHardwareWindow_EventThread -> ponteiro fake nao-null p/ o RHI nao travar esperando janela */
    if (!getenv("NIER_NOHWWIN")) {
      uintptr_t hw = (uintptr_t)text_base + 0x5138038;
      hook_arm64(hw, (uintptr_t)&nier_GetHardwareWindow);
      fprintf(stderr, "[patch] GetHardwareWindow_EventThread @ %p -> fake nao-null\n", (void *)hw);
    }

    /* no-op LogSuppression DisassociateSuppress: Remove() retorna !=1 (mapa FName não acha) ->
     * check brk chamado centenas de vezes; a função só limpa categorias -> ret seguro. */
    if (!getenv("NIER_NOLOGSUP")) {
      uintptr_t ds = (uintptr_t)text_base + 0x4bc0a24;
      *(uint32_t *)ds = 0xd65f03c0u; /* ret */
      fprintf(stderr, "[patch] DisassociateSuppress @ %p -> ret\n", (void *)ds);
    }

    /* no-op Stats GetHighPerformanceEnableForStat: sondagem de hash em mapa vazio -> spin */
    if (!getenv("NIER_NOSTATFIX")) {
      uintptr_t gs = (uintptr_t)text_base + 0x4d1d840;
      hook_arm64(gs, (uintptr_t)&nier_GetHighPerfStat);
      fprintf(stderr, "[patch] GetHighPerformanceEnableForStat @ %p -> dummy\n", (void *)gs);
    }

    /* no-op NotifyRegistrationEvent (EDL/async-loader bookkeeping): o check
     * ExistingPackageState (AsyncPackageLoader.cpp:139) é fatal e o registro do EDL
     * não é usado no static-init (UClasses usam a lista deferred separada). ret. */
    if (!getenv("NIER_NONOTIFY")) {
      uintptr_t nre = (uintptr_t)text_base + 0x4edc510;
      *(uint32_t *)nre = 0xd65f03c0u; /* ret */
      fprintf(stderr, "[patch] NotifyRegistrationEvent @ %p -> ret\n", (void *)nre);
    }

    /* 🔑 DEADLOCK do mutex GAndroidWindowLock (M=*(libUE4+0xaedda30)): a event-thread
     * (pump @0x4442970) trava M em 0x4442aac e fica em ALooper_pollAll(-1) SEGURANDO M; a
     * AndroidMain (engine) bloqueia no mesmo M em 0x4434c84 antes do PreInit. No UE4 real a
     * AndroidMain pega M primeiro (segura no init, libera no window-ready) mas aqui o pump
     * ganha a corrida de startup. FIX: NOP no lock(0x4442aac) e unlock(0x4442b00) do pump ->
     * o pump roda sem segurar M, AndroidMain pega M livre e avanca p/ PreInit/Init/RHI.
     * Gate NIER_NOPUMPLOCKFIX. */
    if (!getenv("NIER_NOPUMPLOCKFIX")) {
      *(uint32_t *)((uintptr_t)text_base + 0x4442aac) = 0xd503201fu; /* nop (era bl mutex_lock) */
      *(uint32_t *)((uintptr_t)text_base + 0x4442b00) = 0xd503201fu; /* nop (era bl mutex_unlock) */
      fprintf(stderr, "[patch] pump-thread M lock/unlock @0x4442aac/0x4442b00 -> nop (anti-deadlock)\n");
    }

    /* hook do check()/assert do UE4 (offset fixo): captura msg + tenta pular */
    if (!getenv("NIER_NOCHECKSKIP")) {
      uintptr_t cv = (uintptr_t)text_base + 0x4be9a10; /* FDebug::CheckVerifyFailedImpl (entrada) */
      hook_arm64(cv, (uintptr_t)&nier_CheckVerifyFailed);
      fprintf(stderr, "[patch] CheckVerifyFailedImpl @ %p hooked (log+skip)\n", (void *)cv);
    }
  }

  so_make_text_executable();
  so_flush_caches();
}

static void load_module(const char *name, int heap_mb, DynLibFunction *tbl, int n,
                        void (*pre_init)(void)) {
  size_t hs = (size_t)heap_mb * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) { fprintf(stderr, "mmap %d MB falhou (%s)\n", heap_mb, name); exit(1); }
  fprintf(stderr, "== carregando %s (heap %p, %d MB) ==\n", name, heap, heap_mb);
  if (so_load(name, heap, hs) < 0)     { fprintf(stderr, "so_load(%s) falhou\n", name); exit(1); }
  fprintf(stderr, "[phase] %s: relocate...\n", name); fflush(stderr);
  if (so_relocate() < 0)               { fprintf(stderr, "so_relocate(%s) falhou\n", name); exit(1); }
  fprintf(stderr, "[phase] %s: resolve (%d syms)...\n", name, n); fflush(stderr);
  so_resolve(tbl, n, 0);
  fprintf(stderr, "[phase] %s: finalize...\n", name); fflush(stderr);
  so_finalize();
  so_flush_caches();
  if (pre_init) pre_init();   /* patches ANTES do init_array */
  fprintf(stderr, "[phase] %s: init_array...\n", name); fflush(stderr);
  so_execute_init_array();
  fprintf(stderr, "[phase] %s: init_array DONE\n", name); fflush(stderr);
  fprintf(stderr, "== %s: text=%p+%zu data=%p+%zu ==\n", name, text_base, text_size, data_base, data_size);
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  { volatile char c = g_bionic_guard_pad[0]; (void)c; } /* ancora o TLS pad */
  install_crash_handler();
  fprintf(stderr, "=== NieR (UE4 4.24) so-loader / NextOS aarch64 Mali-450 ===\n");

  jni_shim_set_package("com.YourCompany.NierM", 1); /* OBB versionCode (ver manifest) */

  preload_device_libs();

  /* tabela base = dynlib_functions (egl_shim+gl+ovrp) + ponte pthread */
  int base_n = (int)dynlib_numfunctions + revc_pthread_count;
  DynLibFunction *base = malloc(sizeof(DynLibFunction) * base_n);
  memcpy(base, dynlib_functions, sizeof(DynLibFunction) * dynlib_numfunctions);
  memcpy(base + dynlib_numfunctions, revc_pthread_table,
         sizeof(DynLibFunction) * revc_pthread_count);

  /* Módulo A: libgnustl_shared.so */
  load_module(GNUSTL_SO, GNUSTL_HEAP_MB, base, base_n, NULL);
  int cxx_n = 0;
  DynLibFunction *cxx_tbl = so_snapshot_symbols(&cxx_n);
  if (!cxx_tbl || cxx_n <= 0) { fprintf(stderr, "snapshot %s vazio\n", GNUSTL_SO); exit(1); }
  fprintf(stderr, "gnustl: %d símbolos exportados\n", cxx_n);

  /* tabela combinada p/ módulo B */
  int comb_n = base_n + cxx_n;
  DynLibFunction *comb = malloc(sizeof(DynLibFunction) * comb_n);
  memcpy(comb, base, sizeof(DynLibFunction) * base_n);
  memcpy(comb + base_n, cxx_tbl, sizeof(DynLibFunction) * cxx_n);

  /* Módulo B: libUE4.so (patch_kill_trace roda entre finalize e init_array) */
  load_module(GAME_SO, GAME_HEAP_MB, comb, comb_n, patch_kill_trace);

  /* entry = android_main (NativeActivity glue) */
  uintptr_t am = so_find_addr("android_main");
  if (!am) fatal_error("android_main não encontrado em %s", GAME_SO);
  fprintf(stderr, "android_main @ %p\n", (void *)am);

  struct android_app *app = android_shim_init();
  if (!app) fatal_error("android_shim_init falhou");

  /* No Android, o sistema chama JNI_OnLoad ao carregar o .so -> a UE4 guarda GJavaVM ali.
   * No so-loader ninguem chama -> GJavaVM fica NULL -> AndroidJavaEnv::GetJavaEnv crasha.
   * Chamamos manualmente com nosso fake VM. */
  if (!getenv("NIER_NOJNIONLOAD")) {
    uintptr_t jniol = so_find_addr("JNI_OnLoad");
    void *vm = app->activity ? app->activity->vm : NULL;
    if (jniol && vm) {
      int v = ((int (*)(void *, void *))jniol)(vm, NULL);
      fprintf(stderr, "[jni] JNI_OnLoad(vm=%p) -> 0x%x\n", vm, v);
    } else
      fprintf(stderr, "[jni] JNI_OnLoad nao chamado (jniol=%p vm=%p)\n", (void *)jniol, vm);
  }

  /* 🔑 Handshake AndroidMain<-Java: AndroidMain (0x44344c8) faz um loop Sleep(10ms) esperando
   * o flag em libUE4+0xaf0bc94 virar !=0. No Android esse flag e' setado por
   * Java_com_epicgames_ue4_GameActivity_nativeResumeMainInit (0x4433f74), chamado pela
   * UI-thread Java. No so-loader o Java nao roda -> setamos o flag direto (a propria
   * nativeResumeMainInit depois espera um 2o flag = handshake cross-thread; chamar a funcao
   * inteira bloquearia, entao so escrevemos o flag que destrava a main). Gate NIER_NORESUMEINIT. */
  if (!getenv("NIER_NORESUMEINIT")) {
    volatile unsigned char *flag = (unsigned char *)((uintptr_t)text_base + 0xaf0bc94);
    *flag = 1;
    fprintf(stderr, "[patch] AndroidMain resume-init flag @ libUE4+0xaf0bc94 = 1\n");
  }

  /* Ciclo de vida Android: START -> RESUME -> INIT_WINDOW -> GAINED_FOCUS. O FAppEventManager
   * so' marca bRunning=1 (offset+87) ao processar ON_RESUME (EAppEventState=6), que vem do
   * APP_CMD_RESUME; sem ele o main-loop da AndroidMain (0x4435008) nunca chama FEngineLoop::Tick
   * e fica dormindo. (mandavamos so' INIT_WINDOW + GAINED_FOCUS). */
  android_shim_send_cmd(app, APP_CMD_START);
  android_shim_send_cmd(app, APP_CMD_RESUME);
  android_shim_send_cmd(app, APP_CMD_INIT_WINDOW);
  android_shim_send_cmd(app, APP_CMD_GAINED_FOCUS);

  fprintf(stderr, "=== chamando android_main ===\n");
  ((void (*)(struct android_app *))am)(app);

  fprintf(stderr, "=== android_main retornou ===\n");
  _exit(0);
}
