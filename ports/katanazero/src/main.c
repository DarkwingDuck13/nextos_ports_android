/*
 * main.c — Katana ZERO (GameMaker Studio 2 / YYC) ARM64 -> NextOS/Mali-450.
 * Carrega libyoyo.so (self-contained, sem libc++_shared), resolve imports
 * contra os shims + glibc (RTLD_DEFAULT fallback no so_resolve), roda os
 * init_array e entra no driver JNI (katana_jni.c: jni_run).
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

#include "error.h"
#include "imports.h"
#include <dlfcn.h>
#include "so_util.h"
#include "util.h"

#define MEMORY_MB 384
#define SO_NAME "libyoyo.so"

static volatile int g_raise_skips = 0;
static void crash_handler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  uintptr_t pc = uc->uc_mcontext.pc;
  uintptr_t fault_addr = (uintptr_t)info->si_addr;
  uintptr_t text = (uintptr_t)text_base;
  uintptr_t data = (uintptr_t)data_base;

  /* si_code <= 0 = sinal ENVIADO (tgkill/raise/kill), nao fault de memoria real.
   * A libyoyo faz raise(SIGSEGV) deliberado (anti-tamper / VMBadRefs). Logamos o
   * caller real e CONTINUAMOS (return) p/ passar da trava e ver o fluxo. */
  if (info->si_code <= 0 && !getenv("KZ_NOSKIP")) {
    if (g_raise_skips < 60) {
      uintptr_t lr = uc->uc_mcontext.regs[30];
      fprintf(stderr, "[raise-skip #%d] sig=%d si_code=%d pc=%p lr=%p",
              g_raise_skips, sig, info->si_code, (void *)pc, (void *)lr);
      if (lr >= text && lr < text + text_size) fprintf(stderr, " lr=libyoyo+0x%lx", (unsigned long)(lr - text));
      /* scan da pilha p/ 1o return de codigo libyoyo */
      uintptr_t sp = uc->uc_mcontext.sp;
      for (uintptr_t a = sp; a < sp + 0x800; a += 8) {
        uintptr_t v = *(uintptr_t *)a;
        if ((v & 3) == 0 && v >= text && v < text + text_size) { fprintf(stderr, " caller=libyoyo+0x%lx", (unsigned long)(v - text)); break; }
      }
      fprintf(stderr, "\n");
    }
    g_raise_skips++;
    return; /* resume apos o svc do tgkill */
  }

  fprintf(stderr, "\n=== CRASH ===\n");
  fprintf(stderr, "Signal: %d (%s)\n", sig,
          sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGABRT ? "SIGABRT" : "?");
  fprintf(stderr, "Fault addr: %p\nPC:         %p\n", (void *)fault_addr, (void *)pc);
  if (pc >= text && pc < text + text_size)
    fprintf(stderr, "PC in .text: libyoyo.so+0x%lx\n", (unsigned long)(pc - text));
  else if (pc >= data && pc < data + data_size)
    fprintf(stderr, "PC in .data: libyoyo.so+0x%lx\n", (unsigned long)(pc - data));
  else
    fprintf(stderr, "PC outside libyoyo.so\n");

  fprintf(stderr, "\nRegisters:\n");
  for (int i = 0; i < 31; i++) {
    fprintf(stderr, "  x%-2d = 0x%016lx", i, (unsigned long)uc->uc_mcontext.regs[i]);
    if (i % 3 == 2 || i == 30) fprintf(stderr, "\n");
  }
  fprintf(stderr, "  sp  = 0x%016lx\n  pc  = 0x%016lx\n",
          (unsigned long)uc->uc_mcontext.sp, (unsigned long)uc->uc_mcontext.pc);

  /* backtrace via LR/FP chain */
  fprintf(stderr, "\nBacktrace:\n  #0  pc %p", (void *)pc);
  if (pc >= text && pc < text + text_size) fprintf(stderr, " (libyoyo.so+0x%lx)", (unsigned long)(pc - text));
  fprintf(stderr, "\n");
  uintptr_t fp = uc->uc_mcontext.regs[29];
  for (int frame = 1; frame < 32 && fp; frame++) {
    uintptr_t *fp_ptr = (uintptr_t *)fp;
    uintptr_t next_fp = fp_ptr[0], lr = fp_ptr[1];
    if (!lr) break;
    fprintf(stderr, "  #%-2d lr %p", frame, (void *)lr);
    if (lr >= text && lr < text + text_size) fprintf(stderr, " (libyoyo.so+0x%lx)", (unsigned long)(lr - text));
    fprintf(stderr, "\n");
    if (next_fp <= fp) break;
    fp = next_fp;
  }
  fprintf(stderr, "\nso text_base=%p text_size=0x%zx data_base=%p data_size=0x%zx\n",
          text_base, text_size, data_base, data_size);
  /* stack scan: acha returns reais dentro da libyoyo (FP-chain falha em leaf libc) */
  { uintptr_t sp = uc->uc_mcontext.sp, t = (uintptr_t)text_base;
    fprintf(stderr, "Stack scan (libyoyo returns):\n");
    int n = 0;
    for (uintptr_t a = sp; a < sp + 0x2000 && n < 24; a += 8) {
      uintptr_t v = *(uintptr_t *)a;
      if (v >= t && v < t + text_size) { fprintf(stderr, "  sp+0x%-4lx -> libyoyo+0x%lx\n",
            (unsigned long)(a - sp), (unsigned long)(v - t)); n++; }
    }
  }
  /* identifica a syslib do PC (e do LR) */
  { Dl_info di; if (dladdr((void *)pc, &di) && di.dli_fname)
      fprintf(stderr, "PC -> %s + 0x%lx  sym=%s\n", di.dli_fname,
              (unsigned long)(pc - (uintptr_t)di.dli_fbase), di.dli_sname ? di.dli_sname : "?");
    uintptr_t lr0 = uc->uc_mcontext.regs[30];
    if (dladdr((void *)lr0, &di) && di.dli_fname)
      fprintf(stderr, "LR -> %s + 0x%lx  sym=%s\n", di.dli_fname,
              (unsigned long)(lr0 - (uintptr_t)di.dli_fbase), di.dli_sname ? di.dli_sname : "?");
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
  sigaction(SIGFPE, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  install_crash_handler();
  debugPrintf("=== Katana ZERO ARM64 (GameMaker/YYC) NextOS port ===\n");

  size_t heap_size = (size_t)MEMORY_MB * 1024 * 1024;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) fatal_error("Failed to allocate %d MB heap", MEMORY_MB);
  debugPrintf("Heap allocated: %p (%d MB)\n", heap, MEMORY_MB);

  debugPrintf("Loading %s...\n", SO_NAME);
  if (so_load(SO_NAME, heap, heap_size) < 0) fatal_error("Failed to load %s", SO_NAME);
  debugPrintf("Loaded %s: text=%p+%zu data=%p+%zu\n", SO_NAME, text_base, text_size, data_base, data_size);

  debugPrintf("Relocating...\n");
  if (so_relocate() < 0) fatal_error("Failed to relocate %s", SO_NAME);

  debugPrintf("Resolving %zu imports...\n", dynlib_numfunctions);
  if (so_resolve(dynlib_functions, dynlib_numfunctions, 0) < 0) fatal_error("Failed to resolve imports");

  so_finalize();
  so_flush_caches();

  debugPrintf("Running init array...\n");
  so_execute_init_array();

  /* DIAG: a GOT de find@plt (0x47f2ca0) deve = interno find (text+0x1260b68).
   * Se != , o crash em IniFile::find vem de um GOT mal-resolvido. */
  { uintptr_t tb = (uintptr_t)text_base;
    uint32_t insn = *(uint32_t *)(tb + 0x125f358);
    uintptr_t gotfind = *(uintptr_t *)(tb + 0x47f2ca0);
    uintptr_t gotmemcmp_slot = 0;
    fprintf(stderr, "[diag] text_base=%p insn@125f358=0x%08x GOT[find]@47f2ca0=0x%lx internal_find=0x%lx delta=0x%lx\n",
            text_base, insn, (unsigned long)gotfind, (unsigned long)(tb + 0x1260b68),
            (unsigned long)(gotfind - (tb + 0x1260b68)));
    extern int raise(int);
    fprintf(stderr, "[diag] raise=%p so_find(find)=0x%lx\n", (void*)&raise,
            (unsigned long)so_find_addr_safe("_ZNSt6__ndk16__treeINS_12__value_typeINS_12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEEP7SectionEENS_19__map_value_compareIS7_SA_NS_4lessIS7_EELb1EEENS5_ISA_EEE4findIS7_EENS_15__tree_iteratorISA_PNS_11__tree_nodeISA_PvEElEERKT_"));
    (void)gotmemcmp_slot;
  }

  extern void jni_run(void);
  debugPrintf("=== Katana ZERO JNI driver ===\n");
  jni_run();
  debugPrintf("jni_run retornou\n");
  _exit(0);
}
