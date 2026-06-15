/*
 * main.c — loader ARMHF do NFS Most Wanted (com.ea.games.nfs13_row).
 *
 * Engine EA/Ironmonkey: entry = JNI_OnLoad em libapp.so + Java
 * GameActivityMain.nativeOnCreate. Multi-módulo (libapp + libc++_shared +
 * libNimble + FMOD). F0: carrega libapp, resolve imports (tabela + fallback
 * dlsym), acha JNI_OnLoad. F1+: multi-módulo + boot JNI/GameActivity.
 */
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "android_shim.h"
#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

#define MEMORY_MB 64
#define SO_NAME "libapp.so"
/* 🔑 pad TLS do canary bionic (DYSMANTLE): as render threads batem no padrão
 * memcpy(0,tid,11) (thread-entry/canary instável sob glibc). Pad _Thread_local
 * 256B aligned(16) NUNCA escrito cobre o slot bionic e estabiliza. */
__attribute__((aligned(16))) _Thread_local char g_bionic_guard_pad[256];

/* ---- crash handler ARMHF (campos arm_pc/arm_r0/arm_lr do sigcontext 32-bit) ---- */
extern void *g_real_dynamic_cast;
/* flags cacheados (imports.c) — NÃO chamar getenv em signal/hot path (race c/ setenv) */
extern int g_nfs_nodcastrec, g_nfs_norecover, g_nfs_noassertignore;
extern void nfs_cache_flags(void);
/* SIGUSR1 sampler: dumpa PC/LR + backtrace (varre stack p/ retornos em libapp) e
 * CONTINUA. Usado p/ achar onde o busy-loop do Nimble spinna. */
static void sample_handler(int sig, siginfo_t *info, void *uctx) {
  (void)sig; (void)info;
  ucontext_t *uc = (ucontext_t *)uctx; mcontext_t *m = &uc->uc_mcontext;
  uintptr_t pc = m->arm_pc, lr = m->arm_lr, sp = m->arm_sp;
  uintptr_t text = (uintptr_t)text_base, tend = text + text_size;
  fprintf(stderr, "[SAMPLE] PC=%08lx LR=%08lx\n", (unsigned long)pc, (unsigned long)lr);
  /* resolve PC/LR contra /proc/self/maps (nomeia a lib + offset) */
  { FILE *mf = fopen("/proc/self/maps", "r"); if (mf) { char ln[400];
    uintptr_t rr[2] = { pc, lr }; const char *rn[2] = { "PC", "LR" };
    while (fgets(ln, sizeof ln, mf)) { unsigned long s, e; char pm[8], pa[256]; pa[0] = 0;
      if (sscanf(ln, "%lx-%lx %7s %*x %*s %*d %255s", &s, &e, pm, pa) >= 3 && pm[2] == 'x')
        for (int q = 0; q < 2; q++) if (rr[q] >= s && rr[q] < e) {
          const char *b = pa[0] ? (strrchr(pa, '/') ? strrchr(pa, '/') + 1 : pa) : "(anon)";
          fprintf(stderr, "  %s in %s+0x%lx\n", rn[q], b, rr[q] - s); }
    } fclose(mf); } }
  if (pc >= text && pc < tend) fprintf(stderr, "  PC=libapp+0x%lx\n", (unsigned long)(pc - text));
  if (lr >= text && lr < tend) fprintf(stderr, "  LR=libapp+0x%lx\n", (unsigned long)(lr - text));
  int n = 0;
  for (uintptr_t a = sp; a < sp + 0x800 && n < 16; a += 4) {
    uintptr_t v = *(uintptr_t *)a;
    if (v >= text && v < tend) { fprintf(stderr, "  [sp+0x%lx] libapp+0x%lx\n",
        (unsigned long)(a - sp), (unsigned long)(v - text)); n++; }
  }
  fflush(stderr);
}
static void crash_handler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  mcontext_t *m = &uc->uc_mcontext;
  int g_pc_mapped = 0;
  uintptr_t pc = m->arm_pc, lr = m->arm_lr, fault = (uintptr_t)info->si_addr;
  uintptr_t text = (uintptr_t)text_base;

  /* probe de legibilidade (my_dynamic_cast): fault esperado → volta sinalizando
   * "não legível". Checado ANTES de tudo (inclusive do assert-ignore). */
  if ((sig == SIGSEGV || sig == SIGBUS) && g_probe_armed) {
    g_probe_armed = 0;
    siglongjmp(g_probe_jmp, 1);
  }

  /* construtor do init_array crashou (precisa de ambiente bionic indisponível)
   * → pula ele e segue (so_execute_init_array armou o sigsetjmp). */
  /* 🎯 a engine RAISA SIGSEGV deliberadamente nos ASSERTS (debug build:
   * pthread_kill/raise como breakpoint de debugger). si_code<=0 = sinal ENVIADO
   * por tgkill/tkill/user (NÃO um fault de memória real, que é SEGV_MAPERR=1/
   * ACCERR=2). Ignoramos (return) = "continuar" no assert → a engine segue.
   * NFS_NOASSERTIGNORE=1 desliga. */
  if ((sig == SIGSEGV || sig == SIGBUS) && info->si_code <= 0 && !g_nfs_noassertignore) {
    static int a = 0;
    extern long nfs_io_last_seek, nfs_io_seeks, nfs_io_read_bytes;
    if (a < 8) { fprintf(stderr, "[ASSERT-IGNORE] raise(sig=%d) deliberado (si_code=%d) -> continua "
                 "[io: seeks=%ld last=%ld read=%ld]\n", sig, info->si_code,
                 nfs_io_seeks, nfs_io_last_seek, nfs_io_read_bytes); a++; }
    return;
  }

  if ((sig == SIGSEGV || sig == SIGBUS) && g_init_armed) {
    g_init_armed = 0;
    siglongjmp(g_init_jmp, 1);
  }

  /* 🎯 __dynamic_cast em objeto de typeinfo corrompido (vtable[-1] aponta p/
   * código). A engine, no parse de asset, faz dynamic_cast (e RTTI recursivo de
   * classes-base via blx interno) em objetos cujo typeinfo é lixo → o deref
   * `ldr ip,[r0,#24]` (off 0x36) faulta. Como o shim só pega a chamada externa
   * (libapp GOT), a recursão interna da libc++ escapa; aqui forçamos o retorno
   * NULL pulando p/ o epílogo (off 0xa6, `mov r0,r4; add sp,#64; pop`) com r4=0
   * — NULL é "cast falhou", resultado C++ válido. NFS_NODCASTREC=1 desliga. */
  if (sig == SIGSEGV && g_real_dynamic_cast && !g_nfs_nodcastrec) {
    uintptr_t dc = ((uintptr_t)g_real_dynamic_cast) & ~1u;
    if (pc == dc + 0x36) {
      static int dr = 0;
      m->arm_r4 = 0;
      m->arm_pc = dc + 0xa6;     /* epílogo: mov r0,r4(=0); add sp,#64; pop */
      if (dr < 8) { fprintf(stderr, "[DCAST-REC] typeinfo lixo @dyncast+0x36 -> retorna NULL (#%d)\n", dr); dr++; }
      return;
    }
  }

  /* 🩹 recupera CHAMADA a ponteiro de função NULL/inválido (blx NULL): a engine
   * itera listas de objetos chamando virtual vtable[N]; algum objeto tem o slot
   * NULL (virtual não-implementada / reloc não-resolvida). PC vira ~0 → faulta.
   * Pula a virtual-call (PC←LR, r0=0) → o caller avança o iterador e segue.
   * NFS_NORECOVER=1 desliga. */
  if (sig == SIGSEGV && !g_nfs_norecover && pc < 0x1000 && lr > 0x10000) {
    static int nrec = 0;
    m->arm_pc = lr; m->arm_r0 = 0; nrec++;
    if (nrec <= 8) fprintf(stderr, "[RECOVER-NULLCALL] blx NULL (pc=%lx) lr=%lx -> skip (#%d)\n",
                           (unsigned long)pc, (unsigned long)lr, nrec);
    return;
  }

  /* 🩹 recupera o memcpy/op com DESTINO NULO e n pequeno (padrão recorrente da
   * engine: destrutor de objeto garbage no parse de asset) — "retorna" da função
   * que crashou (PC←LR) pulando a cópia, p/ a engine seguir. NFS_NORECOVER=1 off. */
  if (sig == SIGSEGV && !g_nfs_norecover) {
    uintptr_t r0 = m->arm_r0, r2 = m->arm_r2;
    static int recov = 0;
    if (r0 < 0x10000 && r2 <= 64 && lr > 0x10000 && recov < 200000) {
      m->arm_r0 = lr; m->arm_pc = lr; recov++; /* r0=retorno(dst), pc=lr */
      if (recov <= 4) fprintf(stderr, "[RECOVER] copy dst=%lx n=%lx -> skip (pc<-lr)\n",
                              (unsigned long)r0, (unsigned long)r2);
      return;
    }
  }

  {
    extern long nfs_io_last_seek, nfs_io_seeks, nfs_io_read_bytes;
    fprintf(stderr, "\n[io-state] seeks=%ld last=%ld read=%ld\n",
            nfs_io_seeks, nfs_io_last_seek, nfs_io_read_bytes);
  }
  /* 🔎 environ: o crash em getenv (libc+0x367ac) sugere __environ NULL/lixo
   * (escrita selvagem). Dump p/ confirmar. */
  { extern char **environ;
    fprintf(stderr, "[environ] &environ=%p environ=%p", (void *)&environ, (void *)environ);
    if (environ && (uintptr_t)environ > 0x1000) {
      fprintf(stderr, " environ[0]=%p", (void *)environ[0]);
      if (environ[0] && (uintptr_t)environ[0] > 0x1000)
        fprintf(stderr, " (\"%.20s\")", environ[0]);
    }
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "\n=== CRASH sig=%d addr=%p ===\n", sig, (void *)fault);
  fprintf(stderr, "  PC=%p", (void *)pc);
  if (pc >= text && pc < text + text_size)
    fprintf(stderr, " (%s+0x%lx)", SO_NAME, (unsigned long)(pc - text));
  fprintf(stderr, "\n  LR=%p", (void *)lr);
  if (lr >= text && lr < text + text_size)
    fprintf(stderr, " (%s+0x%lx)", SO_NAME, (unsigned long)(lr - text));
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

  /* qual região é cada registrador (lê /proc/self/maps) — full line p/ PC/LR/r0 */
  { FILE *mf = fopen("/proc/self/maps", "r");
    if (mf) { char ln[512];
      uintptr_t regs[6] = { pc, lr, m->arm_r0, m->arm_r1, m->arm_r5, m->arm_r6 };
      const char *rn[6] = { "PC", "LR", "r0", "r1", "r5", "r6" };
      while (fgets(ln, sizeof(ln), mf)) {
        unsigned long s, e; char perm[8], path[300]; path[0] = 0;
        if (sscanf(ln, "%lx-%lx %7s %*x %*s %*d %299s", &s, &e, perm, path) >= 3) {
          if (pc >= s && pc < e && perm[2] == 'x') g_pc_mapped = 1;
          for (int q = 0; q < 6; q++)
            if (regs[q] >= s && regs[q] < e)
              fprintf(stderr, "  %s em [%08lx-%08lx %s %s] +0x%lx\n", rn[q], s, e, perm,
                      path[0] ? path : "(anon)", regs[q] - s);
        }
      }
      fclose(mf); }
  }
  /* hexdump do código em PC (só se PC está numa região executável mapeada —
   * senão o próprio dump faultaria e mataria o handler antes do stack scan) */
  if (pc > 0x1000 && g_pc_mapped) {
    fprintf(stderr, "  --- bytes @PC (%p) ---\n", (void *)(pc & ~3u));
    const uint32_t *w = (const uint32_t *)((pc & ~3u) - 16);
    for (int q = 0; q < 8; q++) fprintf(stderr, "    %p: %08x\n", (void *)(w + q), w[q]);
  }
  /* ring dos últimos dynamic_cast (acha o nó de shader selvagem) */
  { extern const void *g_last_dcast_sub, *g_last_dcast_vt, *g_last_dcast_ti, *g_last_dcast_caller;
    struct dcrec { const void *sub, *vt, *dst, *caller; };
    extern struct dcrec g_dcring[]; extern int g_dcring_i;
    fprintf(stderr, "  --- dcast ring (mais recente por último), text=%lx ---\n", (unsigned long)text);
    for (int q = 0; q < 12; q++) {
      struct dcrec *r = &g_dcring[(g_dcring_i + q) % 12];
      if (!r->sub && !r->vt) continue;
      long coff = ((uintptr_t)r->caller >= text && (uintptr_t)r->caller < text + text_size)
                      ? (long)((uintptr_t)r->caller - text) : -1;
      fprintf(stderr, "    sub=%p vt=%p dst=%p caller=%p(libapp+0x%lx)\n",
              r->sub, r->vt, r->dst, r->caller, coff);
    }
    fprintf(stderr, "  --- last dynamic_cast: sub=%p vt=%p ti=%p caller=%p ---\n",
            g_last_dcast_sub, g_last_dcast_vt, g_last_dcast_ti, g_last_dcast_caller);
    if ((uintptr_t)g_last_dcast_sub > 0x1000) {
      const uint32_t *o = (const uint32_t *)g_last_dcast_sub;
      fprintf(stderr, "    obj[0..7]:");
      for (int q = 0; q < 8; q++) fprintf(stderr, " %08x", o[q]);
      fprintf(stderr, "\n    obj as ascii: ");
      const unsigned char *b = (const unsigned char *)g_last_dcast_sub;
      for (int q = 0; q < 32; q++) fprintf(stderr, "%c", (b[q] >= 32 && b[q] < 127) ? b[q] : '.');
      fprintf(stderr, "\n");
    }
  }

  /* backtrace: varre a pilha e resolve cada retorno contra TODAS as regiões
   * executáveis (mapeia anon→módulo via faixas conhecidas no maps). */
  fprintf(stderr, "  --- stack scan (retornos em qq região r-x) ---\n");
  uintptr_t sp = m->arm_sp;
  int n = 0;
  /* carrega faixas r-x do maps p/ rotular cada retorno */
  struct { uintptr_t s, e; char tag[64]; } rx[64]; int nrx = 0;
  { FILE *mf = fopen("/proc/self/maps", "r");
    if (mf) { char ln[512];
      while (fgets(ln, sizeof(ln), mf) && nrx < 64) {
        unsigned long s, e; char perm[8], path[300]; path[0] = 0;
        if (sscanf(ln, "%lx-%lx %7s %*x %*s %*d %299s", &s, &e, perm, path) >= 3 && perm[2] == 'x') {
          rx[nrx].s = s; rx[nrx].e = e;
          const char *b = path[0] ? (strrchr(path, '/') ? strrchr(path, '/') + 1 : path) : "(anon)";
          snprintf(rx[nrx].tag, sizeof rx[nrx].tag, "%s", b); nrx++;
        }
      }
      fclose(mf); }
  }
  /* dump das regiões r-x (acha o que é a região ANON onde o PC do GL crasha) */
  fprintf(stderr, "  --- regiões r-x (%d) ---\n", nrx);
  for (int q = 0; q < nrx; q++) {
    int has_pc = (pc >= rx[q].s && pc < rx[q].e);
    fprintf(stderr, "    [%08lx-%08lx %6luK] %s%s\n", (unsigned long)rx[q].s, (unsigned long)rx[q].e,
            (unsigned long)((rx[q].e - rx[q].s) / 1024), rx[q].tag, has_pc ? "  <<< PC" : "");
  }
  /* 🔎 a região do PC é um .so pequeno r-x anon (não sabemos qual) — varre por
   * strings ASCII p/ identificar o módulo (nomes de função, "libXXX", etc.) */
  for (int q = 0; q < nrx; q++) {
    if (!(pc >= rx[q].s && pc < rx[q].e)) continue;
    fprintf(stderr, "  --- strings na região do PC [%08lx-%08lx] ---\n",
            (unsigned long)rx[q].s, (unsigned long)rx[q].e);
    int shown = 0;
    for (uintptr_t a = rx[q].s; a < rx[q].e && shown < 30; a++) {
      const unsigned char *s = (const unsigned char *)a;
      int len = 0;
      while (a + len < rx[q].e && s[len] >= 32 && s[len] < 127 && len < 80) len++;
      if (len >= 5) {
        fprintf(stderr, "    +0x%05lx: %.*s\n", (unsigned long)(a - rx[q].s), len, s);
        shown++;
      }
      a += len;
    }
    break;
  }
  for (uintptr_t a = sp; a < sp + 0x2000 && n < 40; a += 4) {
    uintptr_t v = *(uintptr_t *)a;
    if (v >= text && v < text + text_size) {
      fprintf(stderr, "    [sp+0x%lx] %s+0x%lx\n", (unsigned long)(a - sp), SO_NAME, (unsigned long)(v - text));
      n++;
    } else {
      for (int q = 0; q < nrx; q++)
        if (v >= rx[q].s && v < rx[q].e) {
          fprintf(stderr, "    [sp+0x%lx] %s+0x%lx (%08lx)\n", (unsigned long)(a - sp),
                  rx[q].tag, (unsigned long)(v - rx[q].s), (unsigned long)v);
          n++; break;
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
  { struct sigaction su; memset(&su, 0, sizeof su); su.sa_sigaction = sample_handler;
    su.sa_flags = SA_SIGINFO; sigaction(SIGUSR1, &su, NULL); }
}

/* tabela combinada acumulada (base + snapshots dos módulos já carregados) */
static DynLibFunction *g_comb;
static int g_comb_n;
static void comb_append(DynLibFunction *tbl, int n) {
  g_comb = realloc(g_comb, sizeof(DynLibFunction) * (g_comb_n + n));
  memcpy(g_comb + g_comb_n, tbl, sizeof(DynLibFunction) * n);
  g_comb_n += n;
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
  if (snapshot) {
    int n = 0;
    DynLibFunction *t = so_snapshot_symbols(&n);
    if (t && n > 0) { comb_append(t, n); debugPrintf("%s: +%d símbolos exportados\n", name, n); }
  }
  /* roda os construtores C++ estáticos (.init_array) deste módulo — necessário
   * antes de usar a engine (inicializa globais/singletons). Ordem = ordem de
   * carga (libc++ 1º). NFS_SKIPINIT="lib..." pula p/ bissecção de corrupção. */
  /* init_array (construtores C++): DEFAULT-OFF — os construtores do NFS
   * corrompem o heap/segfaltam sem o ambiente bionic completo (TODO F2.x:
   * rodar de forma segura). NFS_INIT=1 liga; NFS_SKIPINIT="lib..." filtra. */
  const char *skip = getenv("NFS_SKIPINIT");
  if (getenv("NFS_INIT") && !(skip && strstr(skip, name))) {
    so_execute_init_array();
    debugPrintf("%s: init_array OK\n", name);
  } else {
    debugPrintf("%s: init_array OFF\n", name);
  }
  return 0;
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  nfs_cache_flags(); /* lê os NFS_* UMA vez (antes de threads/setenv) → sem getenv em hot path */
  /* argv[1] = nº de frames (robusto: argv não é afetado pela corrupção do environ
   * pela engine; NFS_FRAMES por getenv estava falhando no hot path). */
  fprintf(stderr, "[ARGV] argc=%d argv1=%s\n", argc, argc > 1 ? argv[1] : "(none)");
  if (argc > 1) { extern int g_nfs_frames; int n = atoi(argv[1]); if (n > 0) g_nfs_frames = n;
    fprintf(stderr, "[ARGV] g_nfs_frames set to %d\n", g_nfs_frames); }
  install_crash_handler();
  __asm__ volatile("" : : "r"(g_bionic_guard_pad) : "memory"); /* força o pad no TLS */
  setvbuf(stdout, NULL, _IONBF, 0); /* logs visíveis no crash (init_array era a causa, não isto) */
  debugPrintf("=== NFS Most Wanted — loader ARMHF (Mali-450) ===\n");

  /* base = shims bionic→glibc (os 18 que o dlsym fallback não cobre)
   * + shims pthread (tradução de layout das primitivas de sincronização) */
  extern DynLibFunction nfs_shims[];
  extern int nfs_shims_count;
  extern DynLibFunction nfs_pthread_shims[];
  extern int nfs_pthread_shims_count;
  g_comb = malloc(sizeof(DynLibFunction) * (nfs_shims_count + nfs_pthread_shims_count));
  memcpy(g_comb, nfs_shims, sizeof(DynLibFunction) * nfs_shims_count);
  memcpy(g_comb + nfs_shims_count, nfs_pthread_shims,
         sizeof(DynLibFunction) * nfs_pthread_shims_count);
  g_comb_n = nfs_shims_count + nfs_pthread_shims_count;

  /* dependências primeiro (cada uma vira fonte de símbolos p/ as seguintes) */
  if (load_module("libc++_shared.so", 24, 1) < 0) return 1; /* std::__ndk1 */
  /* captura o __dynamic_cast REAL da libc++ (busca do FIM do g_comb p/ achar o
   * snapshot da libc++, não o nosso shim do início) p/ o my_dynamic_cast delegar
   * quando a cadeia typeinfo é válida. */
  { extern void *g_real_dynamic_cast;
    for (int i = g_comb_n - 1; i >= 0; i--)
      if (strcmp(g_comb[i].symbol, "__dynamic_cast") == 0 && g_comb[i].func) {
        g_real_dynamic_cast = (void *)g_comb[i].func; break;
      }
    debugPrintf("real __dynamic_cast=%p\n", g_real_dynamic_cast);
    /* 🔑 captura as 3 vtables de type_info da libc++ p/ o NOSSO walker de
     * dynamic_cast (bypassa o walk interno bugado do libcxxabi sob glibc). O
     * type_info->[0] = (vtable_symbol + 8), por isso somamos 8. */
    extern uintptr_t g_ti_vt_class, g_ti_vt_si, g_ti_vt_vmi;
    const char *tn[3] = { "_ZTVN10__cxxabiv117__class_type_infoE",
                          "_ZTVN10__cxxabiv120__si_class_type_infoE",
                          "_ZTVN10__cxxabiv121__vmi_class_type_infoE" };
    uintptr_t *tv[3] = { &g_ti_vt_class, &g_ti_vt_si, &g_ti_vt_vmi };
    for (int k = 0; k < 3; k++)
      for (int i = g_comb_n - 1; i >= 0; i--)
        if (strcmp(g_comb[i].symbol, tn[k]) == 0 && g_comb[i].func) { *tv[k] = g_comb[i].func + 8; break; }
    debugPrintf("ti_vt class=%p si=%p vmi=%p\n", (void*)g_ti_vt_class, (void*)g_ti_vt_si, (void*)g_ti_vt_vmi);
    extern void nfs_install_dyncast_hook(void);
    if (!getenv("NFS_NOHOOK")) nfs_install_dyncast_hook(); }
  if (load_module("libNimble.so", 4, 1) < 0) return 1;      /* bridge JNI */
  if (load_module("libfmodex.so", 8, 1) < 0) return 1;      /* áudio FMOD */
  if (load_module("libfmodevent.so", 8, 1) < 0) return 1;

  /* módulo principal: libapp (resolve contra tudo acima + dlsym) */
  if (load_module(SO_NAME, MEMORY_MB, 0) < 0) return 1;

  /* diag: hook de SKU::GetFileSystemPath p/ ver a chave de arquivo que a engine
   * procura no OBB (NFS_FSPATHLOG=1). */
  if (getenv("NFS_FSPATHLOG")) { extern void nfs_install_getfspath_hook(void);
    nfs_install_getfspath_hook(); }

  /* NFS_RELRO=1: protege .data.rel.ro do libapp (vtables/type_infos) como RO →
   * se a corrupção dos type_infos do shadergen for overflow gravando ali, vira
   * fault no WRITE (culpado no PC) em vez de crash silencioso depois. */
  if (getenv("NFS_RELRO")) {
    int np = so_protect_relro();
    debugPrintf("relro: %d seção(ões) protegida(s)\n", np);
  }

  uintptr_t jni_onload = so_find_addr_safe("JNI_OnLoad");
  uintptr_t native_oncreate =
      so_find_addr_safe("Java_com_ea_ironmonkey_GameActivityMain_nativeOnCreate");
  debugPrintf("JNI_OnLoad=%p nativeOnCreate=%p (combinada=%d símbolos)\n",
              (void *)jni_onload, (void *)native_oncreate, g_comb_n);

  /* ---- F2: boot JNI ---- */
  jni_shim_set_package("com.ea.games.nfs13_row", 0);
  void *vm = 0, *env = 0;
  jni_shim_init(&vm, &env);
  debugPrintf("jni_shim: vm=%p env=%p\n", vm, env);

  /* 🔑 JNI_OnLoad dos MÓDULOS SECUNDÁRIOS (libNimble/fmod): no Android o runtime
   * chama JNI_OnLoad de CADA .so carregado via System.loadLibrary. libNimble
   * (bridge JNI da EA) cacheia o JavaVM no seu JNI_OnLoad; se não chamarmos, o
   * global g_vm fica NULL → Nimble::getEnv deref NULL → SIGSEGV (região anon r-x
   * = texto do libNimble). Chamamos os do g_comb (snapshots) ANTES do libapp.
   * (libapp não está no g_comb — snapshot=0 — então não duplica.) */
  { uintptr_t called[8]; int nc = 0;
    for (int i = 0; i < g_comb_n && nc < 8; i++) {
      if (strcmp(g_comb[i].symbol, "JNI_OnLoad") != 0 || !g_comb[i].func) continue;
      int dup = 0;
      for (int k = 0; k < nc; k++) if (called[k] == g_comb[i].func) dup = 1;
      if (dup) continue;
      called[nc++] = g_comb[i].func;
      int (*fn)(void *, void *) = (int (*)(void *, void *))g_comb[i].func;
      int v = fn(vm, 0);
      debugPrintf("JNI_OnLoad[secundário %p] -> 0x%x\n", (void *)g_comb[i].func, v);
    }
  }

  if (jni_onload) {
    int (*JNI_OnLoad)(void *, void *) = (int (*)(void *, void *))jni_onload;
    int v = JNI_OnLoad(vm, 0);
    debugPrintf("JNI_OnLoad -> 0x%x\n", v);
  }

  static char fake_this[64], fake_surface[64];
#define NCALL0(sym) do { uintptr_t a = so_find_addr_safe(sym); \
    if (a) { debugPrintf(">> %s\n", sym); ((void(*)(void*,void*))a)(env, fake_this); } \
    else debugPrintf("!! %s nao achado\n", sym); } while (0)

  if (!getenv("NFS_NO_ONCREATE") && native_oncreate) {
    debugPrintf("chamando nativeOnCreate...\n");
    ((void(*)(void*,void*,void*,void*,void*,void*))native_oncreate)(env, fake_this, 0, 0, 0, 0);
    debugPrintf("nativeOnCreate retornou\n");

    /* ---- F3: ciclo de render (GameActivity lifecycle + RunLoop tick) ---- */
    extern void egl_shim_create_window(void);
    egl_shim_create_window(); /* SDL2 + EGL/GLES2 no Mali fbdev */

    NCALL0("Java_com_ea_ironmonkey_GameActivityMain_nativeOnStart");
    /* nativeSurfaceCreated(env, this, surface) — surface fake; a engine pega o
     * ANativeWindow via ANativeWindow_fromSurface (android_shim) */
    { uintptr_t a = so_find_addr_safe("Java_com_ea_ironmonkey_GameActivityMain_nativeSurfaceCreated");
      if (a) { debugPrintf(">> nativeSurfaceCreated\n");
        ((void(*)(void*,void*,void*))a)(env, fake_this, fake_surface); } }
    /* nativeSurfaceChanged(env, this, format, w, h) */
    { uintptr_t a = so_find_addr_safe("Java_com_ea_ironmonkey_GameActivityMain_nativeSurfaceChanged");
      if (a) { debugPrintf(">> nativeSurfaceChanged 1280x720\n");
        ((void(*)(void*,void*,int,int,int))a)(env, fake_this, 1, 1280, 720); } }
    NCALL0("Java_com_ea_ironmonkey_GameActivityMain_nativeOnResume");

    /* 🔑 nativeRestoreContext: no Android, quando o contexto GL do GLSurfaceView
     * fica pronto (onSurfaceCreated da render thread), o framework chama isto p/
     * a engine RECRIAR os recursos GL (shaders/texturas/FBOs). O log mostra
     * "[ResourceManager] ContextLost()" após o surfaceChanged → o renderer fica
     * em estado "contexto perdido" e PULA todos os draws até o restore. Sem essa
     * chamada: texturas carregam mas a engine nunca desenha (clears sem draws). */
    if (!getenv("NFS_NORESTORE")) {
      uintptr_t a = so_find_addr_safe("Java_com_ea_ironmonkey_GameActivityMain_nativeRestoreContext");
      if (a) { debugPrintf(">> nativeRestoreContext\n");
        ((void(*)(void*,void*))a)(env, fake_this); }
      else debugPrintf(">> nativeRestoreContext NAO ACHADO\n");
    }

    /* loop de render: RunLoop_nativeOnRunLoopTick a cada frame (a engine faz
     * eglSwapBuffers -> egl_shim -> SDL_GL_SwapWindow) */
    uintptr_t tick = so_find_addr_safe("Java_com_ea_ironmonkey_RunLoop_nativeOnRunLoopTick");
    debugPrintf(">> entrando no render loop (tick=%p)\n", (void *)tick);
    extern int g_nfs_frames; /* cacheado em nfs_cache_flags (environ corrompido aqui) */
    int frames = g_nfs_frames;
    fprintf(stderr, "[FRAMES] cap no render loop = %d\n", frames);
    /* 🔑 torna o contexto GL current NESTA thread (a engine assume que o
     * GLSurfaceView já fez isso) — sem isso toda chamada GL vai pro vazio. */
    { extern int egl_shim_make_root_current(void); egl_shim_make_root_current(); }
    int recov = getenv("NFS_TICKRECOVER") != NULL;
    for (int f = 0; f < frames && tick; f++) {
      if (recov) {
        /* tick recuperável: se crashar, pula esse frame e continua (a engine
         * pode avançar o estado/carregar assets nos frames seguintes) */
        if (sigsetjmp(g_init_jmp, 1) == 0) {
          g_init_armed = 1;
          ((void(*)(void*,void*))tick)(env, fake_this);
          g_init_armed = 0;
        } else {
          g_init_armed = 0;
          if (f < 3 || g_init_skips < 8) debugPrintf("[frame %d CRASHOU -> pulado]\n", f);
          g_init_skips++;
        }
      } else {
        ((void(*)(void*,void*))tick)(env, fake_this);
      }
      /* 🔎 PROBE de INPUT (NFS_INPROBE=1): nos frames 5/30/120 reporta o estado
       * da lista de handlers de TOQUE (sentinel intrusivo @0xadfd24: vazia ⟺
       * *(sentinel)==sentinel) e do flag de toque-desabilitado (*(*0xac696c)). */
      if (getenv("NFS_INPROBE") && (f == 5 || f == 30 || f == 120 || f == 300)) {
        extern void *text_base;
        uintptr_t tb = (uintptr_t)text_base;
        uintptr_t sentinel = tb + 0xadfd24;
        uintptr_t first = *(uintptr_t *)sentinel;
        uintptr_t flagpp = tb + 0xac696c;
        uintptr_t flagp = *(uintptr_t *)flagpp;
        int flagb = (flagp > 0x10000) ? *(unsigned char *)flagp : -1;
        int nnodes = 0;
        for (uintptr_t p = first; p != sentinel && p > 0x10000 && nnodes < 64; nnodes++)
          p = *(uintptr_t *)p; /* next em offset 0 (circular) */
        fprintf(stderr, "[INPROBE f=%d] touchlist %s (first=%#lx sentinel=%#lx nodes=%d) tdisable=%d(flagp=%#lx)\n",
                f, (first == sentinel) ? "EMPTY" : "NONEMPTY",
                (unsigned long)(first - tb), (unsigned long)(sentinel - tb), nnodes, flagb, (unsigned long)(flagp ? flagp - tb : 0));
      }
      /* 🔑 RE-EMITE nativeSurfaceChanged nos primeiros frames: a fn começa com
       * `if (graphics_singleton()==0) return;` — quando a chamamos logo após
       * nativeOnCreate o singleton de gráficos ainda não existe (criado no 1º
       * tick), então o tamanho da view fica 0x0 (glViewport 0,0,0,0 → tudo
       * culled → zero draws). Re-emitir após o tick processa o resize de fato. */
      if (!getenv("NFS_NORESURF") && (f == 0 || f == 1 || f == 2 || f == 5 || f == 15 || f == 40)) {
        uintptr_t sc = so_find_addr_safe("Java_com_ea_ironmonkey_GameActivityMain_nativeSurfaceChanged");
        if (sc) ((void(*)(void*,void*,int,int,int))sc)(env, fake_this, 1, 1280, 720);
      }
      /* 👆 INJETOR DE TOQUE: lê /storage/roms/nfs/tap.txt ("x y" em pixels) e
       * injeta DOWN(este frame)+UP(próximo) em nativeTouchScreenEvent. A engine é
       * SOFTFP → pcs("aapcs") passa os floats x,y no ABI dela. */
      {
        /* x,y são FLOAT. A engine é SOFTFP → recebe os floats como BITS em
         * registradores/stack core (iguais a int). Passamos os bits como int
         * (nossa chamada hardfp põe ints no mesmo lugar que softfp põe os floats).
         * À prova de falha (independe do atributo pcs). */
        typedef void (*touchfn_t)(void*,void*,int,int,int,int);
        static touchfn_t touch = NULL; static int tinit = 0;
        if (!tinit) { tinit = 1;
          touch = (touchfn_t)so_find_addr_safe("Java_com_ea_ironmonkey_GameGLSurfaceView_nativeTouchScreenEvent"); }
        /* 🔑 VIEW REAL: nativeTouchScreenEvent IGNORA o `thiz` que passamos e
         * busca o handler registrado na lista intrusiva @0xadfd24; o dispatch
         * faz IsSameObject(env, handler->view, thiz). Com fake_this o match
         * FALHA → toque descartado. Resolvemos a view real do 1º handler
         * (handler->vtable[2]() retorna o jobject da view) e passamos COMO thiz
         * → IsSameObject(view,view)=TRUE → match → dispatch. NFS_FAKEVIEW=1 volta
         * ao comportamento antigo (fake_this) p/ comparar. */
        void *tobj = fake_this;
        if (!getenv("NFS_FAKEVIEW")) {
          extern void *text_base; uintptr_t tb = (uintptr_t)text_base;
          uintptr_t sentinel = tb + 0xadfd24;
          uintptr_t node = *(uintptr_t *)sentinel;
          if (node != sentinel && node > 0x10000) {
            uintptr_t handler = *(uintptr_t *)(node + 8);
            if (handler > 0x10000) {
              uintptr_t hvt = *(uintptr_t *)handler;
              if (hvt > 0x10000) {
                void *(*get_view)(void *) = (void *(*)(void *))(*(uintptr_t *)(hvt + 8));
                void *v = get_view((void *)handler);
                if (v) tobj = v;
              }
            }
          }
        }
        /* 🔑 PRÉ-NORMALIZAÇÃO: nativeTouchScreenEvent faz x' = x / vtable7(),
         * y' = y / vtable7(). No nosso port vtable7 (dims da view p/ toque)
         * retorna 1 (não 1280/720) → o engine espera coords JÁ normalizadas
         * (0..1). tap.txt traz PIXELS (0..1280, 0..720); convertemos p/
         * (px/1280)*div e (py/720)*div (div=vtable7, robusto se algum dia ≠1).
         * NFS_TAPRAW=1 desliga (passa pixels crus). */
        union { float f; int i; } ux, uy;
        /* 🔑 HOLD do press: o botão destaca no DOWN mas o clique só dispara no
         * UP se a engine registrou o press ao longo de ticks. UP no frame
         * seguinte é cedo demais → seguramos NFS_TAPHOLD frames (default 6)
         * antes de soltar. Durante o hold reemitimos MOVE p/ manter o tracking. */
        static int pend_xi, pend_yi, pend_up = 0;
        static void *pend_obj;
        if (pend_up > 0) {
          pend_up--;
          /* só DOWN…(espera)…UP. MOVE durante o hold pode ser visto como DRAG
           * e cancelar o tap. NFS_TAPMOVE=1 reativa o MOVE p/ comparar. */
          if (pend_up == 0) { if (touch) touch(env, pend_obj, 1/*UP*/, 0, pend_xi, pend_yi); }
          else if (getenv("NFS_TAPMOVE")) { if (touch) touch(env, pend_obj, 2/*MOVE*/, 0, pend_xi, pend_yi); }
        }
        FILE *tf = fopen("/storage/roms/nfs/tap.txt", "r");
        if (tf) { float x, y; int ok = fscanf(tf, "%f %f", &x, &y);
          if (ok == 2 && touch) {
            float nx = x, ny = y;
            int d1 = 1, d2 = 1;
            extern void *text_base; uintptr_t tb = (uintptr_t)text_base;
            void *(*getter)(void *, void *) = (void *(*)(void *, void *))(tb + 0x54a244);
            void *h = getter(env, tobj);
            void *r4 = NULL; uintptr_t ev2off = 0;
            if (h && (uintptr_t)h > 0x10000) {
              uintptr_t hvt = *(uintptr_t *)h;
              int (*dim)(void *) = (int (*)(void *))(*(uintptr_t *)(hvt + 0x1c));
              d1 = dim(h); d2 = dim(h);
              /* r4 = h->vtable[9]() = input-target; r4->vtable[2] = receptor de
               * evento (down/move/up). Logamos o offset em libapp p/ decompilar. */
              void *(*v9)(void *) = (void *(*)(void *))(*(uintptr_t *)(hvt + 0x24));
              r4 = v9(h);
              if (r4 && (uintptr_t)r4 > 0x10000) {
                uintptr_t r4vt = *(uintptr_t *)r4;
                uintptr_t ev2 = *(uintptr_t *)(r4vt + 8);
                ev2off = ev2 - tb;
              }
            }
            /* DEFAULT = pixels crus: o tap-detector (0x54b99c) grava
             * round(coord+0.5) e compara |down-up|<14px, e vtable7()=1 (sem
             * escala) → a engine espera PIXELS de tela, não normalizado.
             * NFS_TAPNORM=1 normaliza (errado p/ este engine; só p/ comparar). */
            if (getenv("NFS_TAPNORM")) { nx = (x / 1280.0f) * d1; ny = (y / 720.0f) * d2; }
            ux.f = nx; uy.f = ny;
            FILE *lg = fopen("/storage/roms/nfs/taplog.txt", "a");
            if (lg) { fprintf(lg, "TAP px=(%g,%g) -> norm=(%.4f,%.4f) div=(%d,%d) view=%p h=%p r4=%p ev2=libapp+0x%lx\n",
                              x, y, nx, ny, d1, d2, tobj, h, r4, (unsigned long)ev2off); fclose(lg); }
            touch(env, tobj, 0/*DOWN*/, 0, ux.i, uy.i);
            { const char *hs = getenv("NFS_TAPHOLD"); int hold = hs ? atoi(hs) : 6;
              if (hold < 2) hold = 2; pend_up = hold; }
            pend_xi = ux.i; pend_yi = uy.i; pend_obj = tobj; }
          fclose(tf); remove("/storage/roms/nfs/tap.txt"); }
      }
      /* ⌨️🎮 INJETOR DE TECLA/GAMEPAD: lê key.txt (keycode Android int) e chama
       * nativeOnPhysicalKeyDown(env, obj, keycode, 0) + Up no frame seguinte.
       * Keycodes: 19=UP 20=DOWN 21=LEFT 22=RIGHT 23=CENTER 66=ENTER 96=BUTTON_A
       * 97=BUTTON_B 4=BACK. NFS suporta gamepad (MogaController) → menus navegáveis. */
      {
        typedef void (*keyfn_t)(void*,void*,int,int);
        static keyfn_t kdown=NULL, kup=NULL; static int kinit=0;
        static volatile char *g_inflag=NULL;
        if(!kinit){ kinit=1;
          kdown=(keyfn_t)so_find_addr_safe("Java_com_ea_ironmonkey_GameActivityMain_nativeOnPhysicalKeyDown");
          kup=(keyfn_t)so_find_addr_safe("Java_com_ea_ironmonkey_GameActivityMain_nativeOnPhysicalKeyUp");
          /* flag global "input desabilitado" (guarda do nativeOnPhysicalKeyDown:
           * if(*flag) return). VA da flag=0xadfd44; base=kdown-0x54cb98. */
          if(kdown) g_inflag=(volatile char*)((uintptr_t)kdown - 0x54cb98 + 0xadfd44); }
        /* NFS_FORCEINPUT=1: zera a flag todo frame (habilita touch/tecla) */
        if(g_inflag && getenv("NFS_FORCEINPUT")) *g_inflag = 0;
        static int pend_key=0, pend_kc=0;
        if(pend_key){ pend_key=0; if(kup) kup(env, fake_this, pend_kc, 0); }
        FILE *kf = fopen("/storage/roms/nfs/key.txt","r");
        if(kf){ int kc; if(fscanf(kf,"%d",&kc)==1 && kdown){
            FILE *lg=fopen("/storage/roms/nfs/taplog.txt","a");
            if(lg){ fprintf(lg,"KEY %d kdown=%p kup=%p\n",kc,(void*)kdown,(void*)kup); fclose(lg); }
            kdown(env, fake_this, kc, 0);
            pend_kc=kc; pend_key=1; }
          fclose(kf); remove("/storage/roms/nfs/key.txt"); }
      }
      /* 🎮 INJETOR DE GAMEPAD (MogaController) — CAMINHO DO MENU. O log da engine
       * mostra "ShowMogaHighlight" no EULA → os menus navegam por gamepad, não
       * por toque (nativeTouchScreenEvent não é consumido pelo menu).
       * MogaController_nativeOnKeyEvent(env, thiz, KeyEvent) lê getKeyCode() via
       * CallIntMethodV → g_moga_active faz nosso jni_shim devolver o keycode.
       * moga.txt = keycode Android (19=UP 20=DOWN 21=L 22=R 23=CENTER 66=ENTER
       * 96=A 97=B 4=BACK 108=START). DOWN(action0) este frame + UP(action1) no
       * próximo (alguns menus disparam no ACTION_UP). */
      {
        typedef void (*mogafn_t)(void*,void*,void*);
        static mogafn_t moga=NULL; static int minit=0;
        static char fake_keyevent[16];
        extern int g_moga_active, g_moga_keycode, g_moga_action, g_moga_calln;
        if(!minit){ minit=1;
          moga=(mogafn_t)so_find_addr_safe("Java_com_ea_ironmonkey_MogaController_nativeOnKeyEvent"); }
        static int mpend=0, mkc=0;
        if(mpend){ mpend=0; if(moga){ g_moga_active=1; g_moga_keycode=mkc; g_moga_action=1/*UP*/; g_moga_calln=0;
            moga(env, fake_this, fake_keyevent); g_moga_active=0; } }
        FILE *mf = fopen("/storage/roms/nfs/moga.txt","r");
        if(mf){ int kc; if(fscanf(mf,"%d",&kc)==1 && moga){
            FILE *lg=fopen("/storage/roms/nfs/taplog.txt","a");
            if(lg){ fprintf(lg,"MOGA %d moga=%p\n",kc,(void*)moga);
              /* 🔎 PROBE do observer de KEY (topo da pilha): replica
               * ctx=0x3f7c88(); 0x3f80f4(&out, ctx, 0) → out[0]=observer da
               * tela. Loga o offset de observer->vtable[2] p/ decompilar. */
              extern void *text_base; uintptr_t tb=(uintptr_t)text_base;
              void*(*getctx)(void)=(void*(*)(void))(tb+0x3f7c88);
              void(*lookup)(void*,void*,int)=(void(*)(void*,void*,int))(tb+0x3f80f4);
              void *ctx=getctx(); void *out[2]={0,0};
              lookup(out, ctx, 0);
              void *obs=out[0];
              if(obs && (uintptr_t)obs>0x10000){ uintptr_t ovt=*(uintptr_t*)obs;
                /* r4 = obs->vtable[9](); handler real = r4->vtable[2] */
                void*(*v9)(void*)=(void*(*)(void*))(*(uintptr_t*)(ovt+0x24));
                void *r4=v9(obs);
                uintptr_t h2=0,d2=0; void *deleg=NULL;
                if(r4&&(uintptr_t)r4>0x10000){ uintptr_t r4vt=*(uintptr_t*)r4; h2=*(uintptr_t*)(r4vt+8)-tb;
                  /* key branch forwarda p/ delegate=[r4+0x40]->vtable[2] */
                  deleg=*(void**)((char*)r4+0x40);
                  if(deleg&&(uintptr_t)deleg>0x10000){ uintptr_t dvt=*(uintptr_t*)deleg; d2=*(uintptr_t*)(dvt+8)-tb; }
                }
                fprintf(lg,"  KEYOBS obs=%p r4=%p handler=libapp+0x%lx deleg=%p deleg2=libapp+0x%lx\n",obs,r4,(unsigned long)h2,deleg,(unsigned long)d2);
              } else fprintf(lg,"  KEYOBS null (ctx=%p)\n",ctx);
              fclose(lg); }
            g_moga_active=1; g_moga_keycode=kc; g_moga_action=0/*DOWN*/; g_moga_calln=0;
            moga(env, fake_this, fake_keyevent); g_moga_active=0;
            mkc=kc; mpend=1; }
          fclose(mf); remove("/storage/roms/nfs/moga.txt"); }
      }
      /* 🕹️ INJETOR DE STICK (nativeOnMotionEvent) — a navegação do MENU pode usar
       * o analog stick (eixos lidos via getAxisValue, armazenados em [ctx+0x138/
       * 0x13c] e processados por 0x7566c), não o DPAD-evento. stick.txt = "x y"
       * floats (eixo 0/1 do stick esquerdo; y=-1 cima, y=+1 baixo). Pulsa: envia o
       * valor 1 frame, depois neutro (p/ o menu detectar a borda e navegar 1 passo). */
      {
        typedef void (*motionfn_t)(void*,void*,void*);
        static motionfn_t motion=NULL; static int motinit=0;
        static char fake_motionevent[16];
        extern int g_motion_active; extern float g_axis[32];
        if(!motinit){ motinit=1;
          motion=(motionfn_t)so_find_addr_safe("Java_com_ea_ironmonkey_MogaController_nativeOnMotionEvent"); }
        static int spend=0;
        if(spend){ spend=0; if(motion){ /* frame neutro (solta o stick) */
            for(int i=0;i<32;i++) g_axis[i]=0.0f; g_motion_active=1;
            motion(env, fake_this, fake_motionevent); g_motion_active=0; } }
        FILE *sf = fopen("/storage/roms/nfs/stick.txt","r");
        if(sf){ float sx,sy; if(fscanf(sf,"%f %f",&sx,&sy)==2 && motion){
            FILE *lg=fopen("/storage/roms/nfs/taplog.txt","a");
            if(lg){ fprintf(lg,"STICK x=%g y=%g motion=%p\n",sx,sy,(void*)motion); fclose(lg); }
            for(int i=0;i<32;i++) g_axis[i]=0.0f; g_axis[0]=sx; g_axis[1]=sy;
            g_motion_active=1; motion(env, fake_this, fake_motionevent); g_motion_active=0;
            spend=1; }
          fclose(sf); remove("/storage/roms/nfs/stick.txt"); }
      }
      /* 🎮🎮 PONTE DO CONTROLE FÍSICO → MogaController (via /dev/input/js0 CRU). O
       * SDL_GameController não atualiza o estado deste adaptador PS2-USB, então
       * lemos o joystick legado js0 direto. Botões → nativeOnKeyEvent; eixos →
       * nativeOnMotionEvent. NFS_PADCAL=1 loga índices crus (calibração). NFS_NOPAD desliga. */
      if (!getenv("NFS_NOPAD")) {
        typedef void (*mfn_t)(void*,void*,void*);
        static mfn_t pk=NULL, pm=NULL; static int pinit=0, jsfd=-2;
        static char pkev[16], pmev[16];
        static float axval[16]={0};  /* eixos js correntes (-1..1) */
        static int padcal=-1;
        extern int g_moga_active, g_moga_keycode, g_moga_action, g_moga_calln;
        extern int g_motion_active; extern float g_axis[32];
        if(!pinit){ pinit=1; padcal=getenv("NFS_PADCAL")?1:0;
          pk=(mfn_t)so_find_addr_safe("Java_com_ea_ironmonkey_MogaController_nativeOnKeyEvent");
          pm=(mfn_t)so_find_addr_safe("Java_com_ea_ironmonkey_MogaController_nativeOnMotionEvent"); }
        if(jsfd==-2){ jsfd=open("/dev/input/js0", O_RDONLY|O_NONBLOCK);
          FILE *lg=fopen("/storage/roms/nfs/taplog.txt","a"); if(lg){ fprintf(lg,"js0 fd=%d\n",jsfd); fclose(lg);} }
        /* mapa BOTÃO js -> Android keycode (ajustável p/ o adaptador; default chute
         * PS2-USB). 96=A/confirm 97=B/back 99=X 100=Y 19/20/21/22=DPAD 108=START. */
        static const int BTNMAP[16]={
          /*0*/100,/*1*/96,/*2*/97,/*3*/99,/*4*/102,/*5*/103,/*6*/104,/*7*/105,
          /*8*/109,/*9*/108,/*10*/0,/*11*/0,/*12*/19,/*13*/20,/*14*/21,/*15*/22 };
        if(jsfd>=0 && pk){
          unsigned char buf[8]; int rd;
          while((rd=read(jsfd, buf, 8))==8){
            short val=(short)(buf[4]|(buf[5]<<8));
            unsigned char type=buf[6]&0x7f, num=buf[7];
            int init=buf[6]&0x80;
            if(padcal && !init){ FILE *lg=fopen("/storage/roms/nfs/padlog.txt","a");
              if(lg){ fprintf(lg,"JS %s num=%d val=%d\n", type==1?"BTN":"AXIS", num, val); fclose(lg);} }
            if(init) continue;
            if(type==1 && num<16){ int kc=BTNMAP[num]; if(kc){
                g_moga_active=1; g_moga_keycode=kc; g_moga_action=val?0:1; g_moga_calln=0;
                pk(env, fake_this, pkev); g_moga_active=0; } }
            else if(type==2 && num<16){ axval[num]=val/32767.0f; }
          }
          /* eixos → nativeOnMotionEvent (contínuo p/ direção/acel). js axis 0=LX 1=LY. */
          if(pm){
            float lx=axval[0], ly=axval[1];
            if(lx<0.18f&&lx>-0.18f) lx=0; if(ly<0.18f&&ly>-0.18f) ly=0;
            for(int i=0;i<32;i++) g_axis[i]=0.0f;
            g_axis[0]=lx; g_axis[1]=ly;
            g_motion_active=1; pm(env, fake_this, pmev); g_motion_active=0;
          }
        }
      }
      /* 🎮 MOGA CONECTADO: seta os flags [ctx+0x135]=suporte e [ctx+0x133]=conectado
       * (vistos em 0x7913c, o setter chamado por nativeOnStateEvent) → o jogo acha
       * que há um gamepad Moga → troca do esquema ACELERÔMETRO (que gira o carro)
       * p/ o esquema GAMEPAD. NFS_NOMOGACONN desliga. */
      if(!getenv("NFS_NOMOGACONN")){
        extern void *text_base; uintptr_t tb=(uintptr_t)text_base;
        static void*(*getctx)(void)=NULL; if(!getctx) getctx=(void*(*)(void))(tb+0x3f7c88);
        void *ctx=getctx();
        if(ctx&&(uintptr_t)ctx>0x10000){ *((unsigned char*)ctx+0x135)=1; *((unsigned char*)ctx+0x133)=1; }
      }
      /* APRESENTA o frame: a engine renderiza no backbuffer mas não chama swap
       * (no Android isso é do GLSurfaceView). NÓS apresentamos pro fb0/Mali. */
      { extern void egl_shim_force_present(void); egl_shim_force_present(); }
      if (f < 5 || f % 60 == 0) debugPrintf("[frame %d]\n", f);
      usleep(16000);
    }
    debugPrintf("=== render loop terminou (%d frames) ===\n", frames);
  }

  debugPrintf("=== F3: render executado ===\n");
  return 0;
}
