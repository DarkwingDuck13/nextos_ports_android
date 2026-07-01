/*
 * main.c -- Sonic the Hedgehog 4: Episode II (Sega "NN/Ninja" engine + "fox"
 * wrapper, libfox.so, armv7, GLES2) so-loader p/ NextOS armv7 + Mali-450 (fbdev,
 * GLES2 via SDL2).
 *
 * Modelo GLSurfaceView (JNI-driven): a Activity Java dirige a engine. Nós
 * replicamos esse driver aqui chamando os entry points Java_com_mineloader_fox_
 * foxJniLib_*: init -> SetGamePath -> SetLanguageId -> DrawEGLCreated ->
 * loop{ FileProcess, GameProcess, DrawFrame } + input.
 *
 * Framework so_util/egl_shim/jni_shim/imports REUSADO do Shantae (ELF32-ARM).
 * Estudo: ports/sonic4/STUDY.md.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <setjmp.h>
#include <execinfo.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "so_util.h"
#include "egl_shim.h"
#include "jni_shim.h"

#ifdef __aarch64__
#define GAME_SO "lib/arm64-v8a/libfox.so"
#else
#define GAME_SO "lib/armeabi-v7a/libfox.so"
#endif
#define GAME_HEAP_MB 256

/* resolução vem 100% automática do egl_shim (SDL_GL_GetDrawableSize do device);
   sem números fixos aqui — qualquer tela é pega na hora. */

extern DynLibFunction shantae_overrides[];
extern const int shantae_overrides_count;
extern DynLibFunction revc_pthread_table[];
extern const int revc_pthread_count;

volatile uintptr_t g_load_base = 0;
volatile unsigned long sonic_frame_for_imports = 0;
volatile int sonic_game_started = 0;
static volatile int sonic_in_draw_frame = 0;
static int g_fbclear = 0; /* SONIC_FBCLEAR: glClear por frame (fix candidato dos rastros) */
extern int sonic_screen_w, sonic_screen_h; /* resolução real da tela (egl_shim) */

/* 🔧 HANDLER DE CRASH (workflow de debug): captura SIGSEGV/ABRT/BUS/ILL/FPE,
   grava backtrace com offset `libfox+0xNNN` (= bt[i]-text_base, resolvível por
   readelf) num `crash.log` PERSISTENTE (append; o log.txt é truncado no relaunch
   pelo launcher, por isso o crash some). Inclui frame atual e se estava no DrawFrame. */
/* 🛡️ chamada protegida do teardown do attract-demo (ver my_ep2_CStartDemo_ReleaseInstance):
   se estamos DENTRO da guarda e vem um SIGSEGV/SIGBUS, recupera via siglongjmp em vez de morrer. */
static sigjmp_buf g_demo_guard_env;
static volatile sig_atomic_t g_demo_guard_active = 0;
static unsigned long g_demo_guard_recovered = 0;
static int g_demoguard_on = 1;                 /* setado em load_module (SONIC_NO_DEMOGUARD desliga) */
/* faixa do destrutor ~CStartDemo (ep2) onde o UAF crasha; epílogo = pop {r4,r5,r6,pc}. */
#define CSD_DTOR_LO 0x4253f8UL
#define CSD_DTOR_HI 0x4254e0UL
#define CSD_DTOR_EPILOGUE 0x4254d8UL

static void sonic_crash_handler(int sig, siginfo_t *si, void *uc) {
  if (g_demo_guard_active && (sig == SIGSEGV || sig == SIGBUS)) {
    g_demo_guard_active = 0;
    g_demo_guard_recovered++;
    siglongjmp(g_demo_guard_env, 1);          /* recupera: pula de volta pro sigsetjmp */
  }
#if defined(__arm__)
  /* 🛡️ DEMOGUARD GERAL: SIGSEGV/SIGBUS DENTRO do ~CStartDemo, por QUALQUER caminho (Act Clear->
     mapa, ReleaseInstance, delete direto...) = o use-after-free do demo. Recupera redirecionando
     o PC pro epílogo (pop {r4,r5,r6,pc}): o frame é estável (só push {r4,r5,r6,lr} no início, sem
     outro mexer no SP), então o pop retorna limpo pro chamador, pulando o resto do destrutor
     (vaza o objeto corrompido, mas NÃO fecha o jogo). Cobre o crash que o hook do ReleaseInstance
     não pegava (tester: crash após Act Clear nas fases pesadas, libfox+0x4254b8). */
  if (g_demoguard_on && (sig == SIGSEGV || sig == SIGBUS) && uc && text_base) {
    ucontext_t *u = (ucontext_t *)uc;
    unsigned long off = u->uc_mcontext.arm_pc - (uintptr_t)text_base;
    if (off >= CSD_DTOR_LO && off < CSD_DTOR_HI) {
      /* push {r4,r5,r6,lr} é @+0x425400. Antes dele o frame não existe -> retorna via LR (o
         chamador); depois dele -> retorna via epílogo (pop {r4,r5,r6,pc}). Nos dois casos r4/r5/r6
         do chamador ficam corretos. Pula o resto do destrutor (vaza o objeto corrompido, sem crash). */
      if (off < 0x425404UL)
        u->uc_mcontext.arm_pc = u->uc_mcontext.arm_lr;
      else
        u->uc_mcontext.arm_pc = (uintptr_t)text_base + CSD_DTOR_EPILOGUE;
      g_demo_guard_recovered++;
      fprintf(stderr, "[DEMOGUARD] SIGSEGV em ~CStartDemo (libfox+0x%lx) -> RECUPERADO #%lu\n",
              off, g_demo_guard_recovered);
      return;                                 /* kernel resume no epílogo/caller -> jogo segue */
    }
  }
#endif
  void *bt[48];
  int n = backtrace(bt, 48);
  /* PC/LR exatos do ucontext (armhf) — a instrução do crash mesmo sem unwind da libfox. */
  unsigned long pc = 0, lr = 0, sp = 0;
  unsigned long R[13] = {0};
#if defined(__arm__)
  if (uc) { ucontext_t *u = (ucontext_t *)uc;
    pc = u->uc_mcontext.arm_pc; lr = u->uc_mcontext.arm_lr; sp = u->uc_mcontext.arm_sp;
    R[0]=u->uc_mcontext.arm_r0; R[1]=u->uc_mcontext.arm_r1; R[2]=u->uc_mcontext.arm_r2;
    R[3]=u->uc_mcontext.arm_r3; R[4]=u->uc_mcontext.arm_r4; R[5]=u->uc_mcontext.arm_r5;
    R[6]=u->uc_mcontext.arm_r6; R[7]=u->uc_mcontext.arm_r7; R[8]=u->uc_mcontext.arm_r8;
    R[9]=u->uc_mcontext.arm_r9; R[10]=u->uc_mcontext.arm_r10; R[11]=u->uc_mcontext.arm_fp;
    R[12]=u->uc_mcontext.arm_ip; }
#endif
  uintptr_t tb = (uintptr_t)text_base;
  FILE *fs[2]; fs[0] = stderr; fs[1] = fopen("crash.log", "a");
  for (int k = 0; k < 2; k++) {
    FILE *o = fs[k]; if (!o) continue;
    fprintf(o, "\n==== CRASH sig=%d addr=%p text_base=0x%lx frame=%lu in_draw=%d ====\n",
            sig, si ? si->si_addr : NULL, (unsigned long)tb,
            sonic_frame_for_imports, sonic_in_draw_frame);
    fprintf(o, "  PC=0x%lx libfox+0x%lx   LR=0x%lx libfox+0x%lx   SP=0x%lx\n",
            pc, pc - tb, lr, lr - tb, sp);
    fprintf(o, "  r0=0x%lx r1=0x%lx r2=0x%lx r3=0x%lx r4=0x%lx r5=0x%lx r6=0x%lx\n",
            R[0],R[1],R[2],R[3],R[4],R[5],R[6]);
    fprintf(o, "  r7=0x%lx r8=0x%lx r9=0x%lx r10=0x%lx fp=0x%lx ip=0x%lx\n",
            R[7],R[8],R[9],R[10],R[11],R[12]);
    for (int i = 0; i < n; i++) {
      long off = (long)((uintptr_t)bt[i] - tb);
      fprintf(o, "  #%-2d %p  libfox+0x%lx\n", i, bt[i], off);
    }
    fflush(o);
  }
  if (fs[1]) fclose(fs[1]);
  signal(sig, SIG_DFL);
  raise(sig);
}
static void sonic_install_crash_handler(void) {
  struct sigaction sa; memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = sonic_crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  int sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };
  for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
    sigaction(sigs[i], &sa, NULL);
}

static struct {
  int pending;
  int done;
  unsigned long uid;
  int (*AoStorageLoadIsFinished)(void);
  int (*AoStorageLoadIsSuccessed)(void);
  int (*AoStorageGetError)(void);
  void (*CopyBackupComp)(unsigned long);
  void (*SetSaveEnable)(unsigned long, long);
  void (*DmBuildSysDataFromBackup)(void);
} g_native_save_load;

static struct {
  int ready;
  int built;
  int missing_logged;
  int (*AoStorageLoadIsFinished)(void);
  int (*AoStorageLoadIsSuccessed)(void);
  int (*AoStorageGetError)(void);
  void (*DmBuildSysDataFromBackup)(void);
  void (*UpdateStageUnlockState)(void);
  int (*IsStageUnlocked)(unsigned long, int);
  int (*IsStageClear)(unsigned long, int);
  void *(*SProgressCreateInstance)(unsigned long);
  int (*GetStageUnlockState)(void *);
  int (*GetSsUnlockState)(void *);
  int (*GetEpMetalUnlockState)(void *);
} g_save_bootstrap;

static DynLibFunction *g_base;
static int g_base_n;
static int env_flag_enabled(const char *name) {
  const char *v = getenv(name);
  return v && *v && strcmp(v, "0") != 0 && strcasecmp(v, "false") != 0 &&
         strcasecmp(v, "no") != 0 && strcasecmp(v, "off") != 0;
}

/* single-instance: mata qualquer outra instância do MESMO binário ANTES de
   inicializar fb/EGL — 2 jogos juntos travam o device. Mesmo método /proc/PID/exe
   do launcher antigo (readlink casa o caminho real; pkill -x/-f não casa pois o
   exe vira ./sonic4), agora no binário p/ deixar o launcher enxuto/padrão. */
static void sonic_kill_other_instances(void) {
  char self_exe[4096];
  ssize_t n = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
  if (n <= 0) return;
  self_exe[n] = '\0';
  pid_t me = getpid();
  for (int pass = 0; pass < 2; pass++) {
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *e;
    int killed = 0;
    while ((e = readdir(d))) {
      if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
      pid_t pid = (pid_t)atoi(e->d_name);
      if (pid <= 0 || pid == me) continue;
      char path[64], tgt[4096];
      snprintf(path, sizeof(path), "/proc/%d/exe", pid);
      ssize_t tn = readlink(path, tgt, sizeof(tgt) - 1);
      if (tn <= 0) continue;
      tgt[tn] = '\0';
      if (strcmp(tgt, self_exe) != 0) continue;
      fprintf(stderr, "=== matando instância anterior pid %d (%s) [%s] ===\n",
              pid, tgt, pass == 0 ? "TERM" : "KILL");
      kill(pid, pass == 0 ? SIGTERM : SIGKILL);
      killed++;
    }
    closedir(d);
    if (!killed) break;       /* 0 outras instâncias -> confirmado, sai */
    usleep(700 * 1000);       /* dá tempo do TERM antes do KILL */
  }
}

/* 🔑 Sessão/runtime FALLBACK: se o frontend (ES) NÃO exportou XDG_RUNTIME_DIR /
   WAYLAND_DISPLAY — acontece em muOS e alguns ROCKNIX — os backends de ÁUDIO
   (pulse/pipewire PRECISAM do runtime-dir; sem ele -> "pw_loop_new can't make
   support.system handle" = MUDO) e de VÍDEO (wayland precisa do socket; sem ele
   -> tela preta) FALHAM. Aqui só APONTAMOS pro que o sistema JÁ criou (não força
   driver nenhum; só preenche se estiver vazio). Em kmsdrm puro (muOS sem wayland)
   não há socket wayland -> WAYLAND_DISPLAY fica vazio -> SDL usa kmsdrm (correto). */
static int dir_ok_rw(const char *p) {
  if (!p || !*p) return 0;
  DIR *d = opendir(p); if (!d) return 0; closedir(d);
  return access(p, W_OK) == 0;    /* precisa ser GRAVAVEL p/ o socket do pipewire/pulse */
}
static void sonic_detect_session_runtime(void) {
  /* Só age se o runtime-dir ATUAL estiver ausente/não-gravável (não mexe num válido da sessão). */
  const char *cur = getenv("XDG_RUNTIME_DIR");
  if (!dir_ok_rw(cur)) {
    char ubuf[64];
    snprintf(ubuf, sizeof(ubuf), "/run/user/%u", (unsigned)getuid());
    const char *cands[] = { "/run/0-runtime-dir", "/var/run/0-runtime-dir",
                            "/run/user/0", "/var/run/user/0", ubuf, NULL };
    const char *chosen = NULL;
    for (int i = 0; cands[i]; i++)
      if (dir_ok_rw(cands[i])) { chosen = cands[i]; break; }
    /* 🔊 muOS/ROCKNIX sem runtime-dir: pipewire/pulse falham ("pw.loop can't make
       support.system handle: No such file or directory") -> sem servidor de som -> cai no
       ALSA cru -> speaker busy -> HDMI (mudo). Se NADA válido existe, CRIA um dir gravável
       0700 p/ o servidor de som conseguir criar o socket. SONIC_NO_RTDIR=1 desliga. */
    static char made[80];
    if (!chosen && !getenv("SONIC_NO_RTDIR")) {
      snprintf(made, sizeof(made), "/tmp/sonic-rt-%u", (unsigned)getuid());
      mkdir(made, 0700);
      if (dir_ok_rw(made)) { chmod(made, 0700); chosen = made; }
    }
    if (chosen) {
      setenv("XDG_RUNTIME_DIR", chosen, 1);
      fprintf(stderr, "=== XDG_RUNTIME_DIR = %s (fallback p/ pipewire/pulse) ===\n", chosen);
    }
  }
  if (!getenv("WAYLAND_DISPLAY")) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    DIR *d = rt ? opendir(rt) : NULL;
    if (d) {
      struct dirent *e;
      while ((e = readdir(d))) {
        if (strncmp(e->d_name, "wayland-", 8) == 0 && !strstr(e->d_name, ".lock")) {
          setenv("WAYLAND_DISPLAY", e->d_name, 1);
          fprintf(stderr, "=== WAYLAND_DISPLAY fallback = %s ===\n", e->d_name);
          break;
        }
      }
      closedir(d);
    }
  }
}

static void sonic_check_exit_hotkey(SDL_GameController *pad, const Uint8 *ks) {
  int keyboard_combo = ks && ks[SDL_SCANCODE_ESCAPE] &&
                       ks[SDL_SCANCODE_RETURN];
  int pad_combo = 0;
  if (pad) {
    SDL_GameControllerUpdate();
    pad_combo = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK) &&
                SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START);
  }
  if (keyboard_combo || pad_combo) {
    fprintf(stderr, "=== SELECT+START -> exit ===\n");
    fflush(NULL);
    sync();
    _exit(0);
  }
}

static void build_base_table(void) {
  g_base_n = shantae_overrides_count + revc_pthread_count;
  g_base = malloc(sizeof(DynLibFunction) * g_base_n);
  memcpy(g_base, shantae_overrides, sizeof(DynLibFunction) * shantae_overrides_count);
  memcpy(g_base + shantae_overrides_count, revc_pthread_table,
         sizeof(DynLibFunction) * revc_pthread_count);
}

/* ---- patch "return 0": ARM/Thumb (32-bit) ou A64 (aarch64) ---- */
static void patch_ret0(const char *sym) {
  uintptr_t raw = so_find_addr_safe(sym);
  if (!raw) { fprintf(stderr, "patch: símbolo %s NÃO encontrado\n", sym); return; }
  uintptr_t a = raw & ~(uintptr_t)1;
  uintptr_t pg = a & ~0xFFFUL;
  if (mprotect((void *)pg, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    fprintf(stderr, "patch: mprotect %s falhou\n", sym); return;
  }
#ifdef __aarch64__
  ((uint32_t *)a)[0] = 0x52800000; /* mov w0, #0 */
  ((uint32_t *)a)[1] = 0xd65f03c0; /* ret        */
  const char *mode = "A64";
#else
  int thumb = raw & 1;
  if (thumb) {
    ((uint16_t *)a)[0] = 0x2000; /* movs r0,#0 */
    ((uint16_t *)a)[1] = 0x4770; /* bx lr      */
  } else {
    ((uint32_t *)a)[0] = 0xe3a00000; /* mov r0,#0 (ARM) */
    ((uint32_t *)a)[1] = 0xe12fff1e; /* bx lr     (ARM) */
  }
  const char *mode = thumb ? "Thumb" : "ARM";
#endif
  mprotect((void *)pg, 0x2000, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)a, (char *)a + 8);
  fprintf(stderr, "patch: %s -> return 0 @0x%lx (%s)\n", sym,
          (unsigned long)a, mode);
}

/* patch "return val" (val pequeno) */
static void patch_retval(const char *sym, int val) {
  uintptr_t raw = so_find_addr_safe(sym);
  if (!raw) { fprintf(stderr, "patch: %s NÃO encontrado\n", sym); return; }
  uintptr_t a = raw & ~(uintptr_t)1, pg = a & ~0xFFFUL;
  if (mprotect((void *)pg, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return;
#ifdef __aarch64__
  ((uint32_t *)a)[0] = 0x52800000 | ((uint32_t)(val & 0xffff) << 5); /* movz w0,#val */
  ((uint32_t *)a)[1] = 0xd65f03c0; /* ret */
  const char *mode = "A64";
#else
  int thumb = raw & 1;
  if (thumb) { ((uint16_t *)a)[0] = 0x2000 | (val & 0xff); ((uint16_t *)a)[1] = 0x4770; }
  else { ((uint32_t *)a)[0] = 0xe3a00000 | (val & 0xff); ((uint32_t *)a)[1] = 0xe12fff1e; }
  const char *mode = thumb ? "Thumb" : "ARM";
#endif
  mprotect((void *)pg, 0x2000, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)a, (char *)a + 8);
  fprintf(stderr, "patch: %s -> return %d @0x%lx (%s)\n", sym, val,
          (unsigned long)a, mode);
}

/* patch de UMA instrução (32-bit) em offset de byte dentro de um símbolo.
   `insn` deve ser do ISA do build (ARM ou A64) — quem chama escolhe via #ifdef. */
static void patch_word_at(const char *sym, unsigned off, uint32_t insn) {
  uintptr_t raw = so_find_addr_safe(sym);
  if (!raw) { fprintf(stderr, "patch_word: %s NÃO encontrado\n", sym); return; }
  uintptr_t a = (raw & ~(uintptr_t)1) + off, pg = a & ~0xFFFUL;
  if (mprotect((void *)pg, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    fprintf(stderr, "patch_word: mprotect %s falhou\n", sym); return;
  }
  ((uint32_t *)a)[0] = insn;
  mprotect((void *)pg, 0x2000, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)a, (char *)a + 4);
  fprintf(stderr, "patch_word: %s+0x%x = 0x%08x @0x%lx\n", sym, off, insn,
          (unsigned long)a);
}

/* desvio absoluto pra `target`: ARM (8B) ou A64 (16B: ldr x16,#8; br x16; .quad) */
static void patch_arm_jump(const char *sym, void *target) {
  uintptr_t raw = so_find_addr_safe(sym);
  if (!raw) { fprintf(stderr, "patch_jump: %s NÃO encontrado\n", sym); return; }
  uintptr_t a = raw & ~(uintptr_t)1, pg = a & ~0xFFFUL;
#ifndef __aarch64__
  if (raw & 1) { fprintf(stderr, "patch_jump: %s é Thumb, ignorado\n", sym); return; }
#endif
  if (mprotect((void *)pg, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    fprintf(stderr, "patch_jump: mprotect %s falhou\n", sym); return;
  }
#ifdef __aarch64__
  ((uint32_t *)a)[0] = 0x58000050;          /* ldr x16, #8 (pc+8) */
  ((uint32_t *)a)[1] = 0xd61f0200;          /* br  x16            */
  *(uint64_t *)(a + 8) = (uint64_t)(uintptr_t)target;
  __builtin___clear_cache((char *)a, (char *)a + 16);
#else
  ((uint32_t *)a)[0] = 0xe51ff004;          /* ldr pc, [pc, #-4] */
  ((uint32_t *)a)[1] = (uint32_t)(uintptr_t)target;
  __builtin___clear_cache((char *)a, (char *)a + 8);
#endif
  mprotect((void *)pg, 0x2000, PROT_READ | PROT_EXEC);
  fprintf(stderr, "patch_jump: %s -> %p @0x%lx\n", sym, target,
          (unsigned long)a);
}

/* base/fim do heap da engine (mantido só p/ diagnóstico/log). */
static uintptr_t g_engine_heap_base = 0, g_engine_heap_end = 0;
/* 🔑 VALIDADE FROUXA (corrige a REGRESSÃO pós-v4.0 que travava a abertura da fase):
   as REGISTLIST da engine são malloc'd em endereços tipo 0xab.../0xac... — FORA do
   arena de 256MB do so_load. A checagem ESTRITA por arena [heap_base,heap_end) rejeitava
   TODA lista válida -> nenhuma textura era liberada -> a free-list do amTexMgr esgotava
   -> amTexMgrCreateTexId fazia `str r0,[r3]` com r3=head=NULL -> SIGSEGV (fase não abre).
   A v4.0 e o 1º fix de saída (a03b5fe) usavam range frouxo e funcionavam. Aqui o
   discriminador do caso stale (saída) NÃO é o range e sim o `count` fora de [1,65536]. */
static int sane_engine_ptr(const void *p) {
  uintptr_t v = (uintptr_t)p;
  return v > 0x10000UL && v < 0xfffff000UL && (v & 3) == 0; /* userspace + alinhado a 4 */
}
/* 🛡️ FIX do crash ao SAIR da fase (Return to Stage Select fecha o jogo):
   _amDrawReleaseTexture(node) libera a lista de texturas da cena de forma DIFERIDA
   (enfileirada por GameProcess, executada por DrawFrame via amDrawExecRegist). Na saída,
   o GameProcess enfileira o release E libera os dados da cena na MESMA passada (loop
   single-thread GameProcess->DrawFrame), então quando o DrawFrame drena a fila o ponteiro
   da lista (node+4 = P = {count,array}) já aponta pra memória LIBERADA -> count lixo
   (ex.: 0xfbfd18f0) -> indexa array fora -> SIGSEGV (e às vezes vira corrupção/OOM).
   Nossa versão VALIDA o count/ponteiros antes de iterar: lista válida = libera normal
   (sem vazar); lista corrompida = pula com segurança (os dados já foram liberados pela
   destruição da cena, então não há o que liberar). Idêntica ao original no caminho bom. */
static void (*g_real_TexMgrDecRef)(unsigned) = 0;
static unsigned long g_relsafe_ok = 0, g_relsafe_skip = 0;
static void my_amDrawReleaseTexture(void *node) {
  if (!node) return;
  void *P = ((void **)node)[1];                    /* P = [node+4] = lista {count, array} */
  long count = -1; void *array = 0;
  if (P && sane_engine_ptr(P)) {
    count = (long)((unsigned *)P)[0];              /* [P+0] = count */
    array = ((void **)P)[1];                       /* [P+4] = array de elementos de 8 BYTES */
  }
  if (count > 0 && count <= 65536 && array && sane_engine_ptr(array)) {
    if (!g_real_TexMgrDecRef)
      g_real_TexMgrDecRef = (void (*)(unsigned))so_find_addr_safe("amTexMgrDecRef");
    /* 🔑 STRIDE 8 BYTES (igual ao original `ldr [r6, r4, lsl #3]`): cada entrada do array
       e {u32 handle, u32 outro}; o handle e os 4 bytes baixos. Ler como u32[] (passo 4)
       pegava handles ERRADOS -> DecRef em slots errados -> corrompia o texmgr (free-list)
       -> crash em amTexMgrCreateTexId (special stage/transicoes). */
    const unsigned char *a = (const unsigned char *)array;
    for (long i = 0; i < count; i++) {
      unsigned h = *(const unsigned *)(a + (size_t)i * 8);
      if (h && g_real_TexMgrDecRef) g_real_TexMgrDecRef(h);  /* qualquer handle != 0, igual orig */
    }
    g_relsafe_ok++;
  } else {
    g_relsafe_skip++;
    if (g_relsafe_skip <= 20)
      fprintf(stderr, "[RELSAFE] release de textura com lista CORROMPIDA pulado "
              "(count=%ld P=%p array=%p) skip#%lu\n", count, (void *)P,
              (void *)array, g_relsafe_skip);
  }
  ((unsigned *)node)[0] = 0;                       /* node->field0 = 0 (igual ao original) */
}

/* 🔊 FIX som do 1up/jingle (mudo no gameplay) — CIRÚRGICO, DEFAULT ON.
   A engine poll-a MediaPlayerisPlaying(canal) por frame; quando "não toca" seta bit1=stop
   na SCB e DmSoundIsStopJingle manda PARAR o jingle. Patchar ->0 fixo (p/ pular o intro)
   matava TODO jingle na hora (som da caixa de vida/1up, anéis-suficientes, sons da special
   stage que passam pelo caminho DmSound). SOLUÇÃO: devolver o estado REAL só p/ os canais
   de JINGLE (key com "_jin_"); os outros mantêm ->0 (preserva o resto + pula o intro via
   willPlayMovie->0 + videoIsPlaying->0). Isto NÃO tem relação com o crash da fase (era o
   texmgr) — reabilitado após corrigir o guard do RELSAFE. SONIC_NO_JINGLE1UP=1 reverte. */
extern int sonic_audio_jingle_playing(int id);
static int my_MediaPlayerisPlaying(int i) {
  static int off = -1;
  if (off < 0) off = getenv("SONIC_NO_JINGLE1UP") ? 1 : 0;
  if (off) return 0;                                  /* fallback: comportamento v4.0 (mudo) */
  return sonic_audio_jingle_playing(i);               /* 1 só se canal i for jingle tocando */
}

/* 🛡️ FIX crash no teardown do attract-demo (reportado pelo tester na Electric Road, frame ~115k):
   `gm::start_demo::ep2::CStartDemo` é um SINGLETON (s_instance). Se o objeto sofre USE-AFTER-FREE
   (liberado externamente sem zerar s_instance), o ReleaseInstance nativo chama o destrutor virtual
   sobre memória corrompida -> SIGSEGV dentro de ~CStartDemo (ex.: str [this+0x28] em libfox+0x4254b8,
   PC=0x...4254b8). Reimplementamos o ReleaseInstance (fiel: null-check + blx vtable[0] + zera
   s_instance) MAS envolvemos a chamada do destrutor numa CHAMADA PROTEGIDA (sigsetjmp): se crashar,
   o handler faz siglongjmp de volta, a gente loga, zera s_instance e SEGUE (vaza o objeto meio-
   destruído, mas NÃO fecha o jogo). No caminho bom é idêntico ao original. SONIC_NO_DEMOGUARD desliga. */
static void **g_cstartdemo_s_instance = 0;
static void my_ep2_CStartDemo_ReleaseInstance(void) {
  if (!g_cstartdemo_s_instance)
    g_cstartdemo_s_instance =
        (void **)so_find_addr_safe("_ZN2gm10start_demo3ep210CStartDemo10s_instanceE");
  void **pinst = g_cstartdemo_s_instance;
  if (!pinst) return;
  void *inst = *pinst;
  if (!inst) return;                          /* igual ao original: s_instance null -> nada a fazer */
  g_demo_guard_active = 1;
  if (sigsetjmp(g_demo_guard_env, 1) == 0) {
    void (**vt)(void *) = *(void (***)(void *))inst;   /* vtable = *inst (pode crashar se UAF) */
    void (*dtor)(void *) = vt[0];                      /* vtable[0] = destrutor virtual (D0) */
    dtor(inst);                                        /* == blx [[inst]] do ReleaseInstance nativo */
    g_demo_guard_active = 0;
  } else {
    /* recuperado do SIGSEGV dentro do destrutor */
    fprintf(stderr, "[DEMOGUARD] ~CStartDemo crashou (use-after-free) -> RECUPERADO, "
                    "s_instance zerado (#%lu)\n", g_demo_guard_recovered);
  }
  *pinst = 0;                                 /* igual ao original: zera o singleton */
}

static int sonic_amThreadCheckDraw(long unused) {
  (void)unused;
  /* SONIC_THREADDRAW: testa o valor. O original retorna 1 se está na thread de draw
     registrada; single-thread (nossa main = draw thread) => deveria ser 1 SEMPRE.
     =1 força 1 (execute sempre, sem enfileirar — testa o bug de replicação do cassino);
     =0 força 0; vazio = sonic_in_draw_frame (comportamento atual). */
  static int mode = -2;
  if (mode == -2) { const char *m = getenv("SONIC_THREADDRAW");
    mode = m ? (m[0]-'0') : -1; }
  if (mode == 1) return 1;
  if (mode == 0) return 0;
  return sonic_in_draw_frame ? 1 : 0;
}

static void sonic_native_save_load_poll(const char *where) {
  if (!g_native_save_load.pending) return;
  if (g_native_save_load.AoStorageLoadIsFinished &&
      !g_native_save_load.AoStorageLoadIsFinished())
    return;

  int ok = g_native_save_load.AoStorageLoadIsSuccessed ?
      g_native_save_load.AoStorageLoadIsSuccessed() : 0;
  if (ok) {
    fprintf(stderr, "=== native save load OK (%s) ===\n", where);
    if (g_native_save_load.CopyBackupComp)
      g_native_save_load.CopyBackupComp(g_native_save_load.uid);
    if (g_native_save_load.SetSaveEnable)
      g_native_save_load.SetSaveEnable(g_native_save_load.uid, 1);
    if (g_native_save_load.DmBuildSysDataFromBackup)
      g_native_save_load.DmBuildSysDataFromBackup();
  } else {
    int err = g_native_save_load.AoStorageGetError ?
        g_native_save_load.AoStorageGetError() : -1;
    fprintf(stderr, "=== native save load FAIL err=%d (%s) ===\n", err, where);
    if (g_native_save_load.SetSaveEnable)
      g_native_save_load.SetSaveEnable(g_native_save_load.uid, 0);
    if (g_native_save_load.DmBuildSysDataFromBackup)
      g_native_save_load.DmBuildSysDataFromBackup();
  }
  g_native_save_load.pending = 0;
  g_native_save_load.done = 1;
}

static void sonic_save_bootstrap_init(void) {
  memset(&g_save_bootstrap, 0, sizeof(g_save_bootstrap));
  g_save_bootstrap.AoStorageLoadIsFinished =
      (void *)so_find_addr_safe("_Z23AoStorageLoadIsFinishedv");
  g_save_bootstrap.AoStorageLoadIsSuccessed =
      (void *)so_find_addr_safe("_Z24AoStorageLoadIsSuccessedv");
  g_save_bootstrap.AoStorageGetError =
      (void *)so_find_addr_safe("_Z17AoStorageGetErrorv");
  g_save_bootstrap.DmBuildSysDataFromBackup =
      (void *)so_find_addr_safe("_Z24DmBuildSysDataFromBackupv");
  g_save_bootstrap.UpdateStageUnlockState =
      (void *)so_find_addr_safe("_ZN2gs6backup7utility22UpdateStageUnlockStateEv");
  g_save_bootstrap.IsStageUnlocked =
      (void *)so_find_addr_safe("_ZN2gs6backup7utility15IsStageUnlockedEm21tag_GSE_MAIN_STAGE_ID");
  g_save_bootstrap.IsStageClear =
      (void *)so_find_addr_safe("_ZN2gs6backup7utility12IsStageClearEm21tag_GSE_MAIN_STAGE_ID");
  g_save_bootstrap.SProgressCreateInstance =
      (void *)so_find_addr_safe("_ZN2gs6backup9SProgress14CreateInstanceEm");
  g_save_bootstrap.GetStageUnlockState =
      (void *)so_find_addr_safe("_ZN2gs6backup9SProgress19GetStageUnlockStateEv");
  g_save_bootstrap.GetSsUnlockState =
      (void *)so_find_addr_safe("_ZNK2gs6backup9SProgress16GetSsUnlockStateEv");
  g_save_bootstrap.GetEpMetalUnlockState =
      (void *)so_find_addr_safe("_ZN2gs6backup9SProgress21GetEpMetalUnlockStateEv");

  g_save_bootstrap.ready =
      g_save_bootstrap.AoStorageLoadIsFinished &&
      g_save_bootstrap.AoStorageLoadIsSuccessed &&
      g_save_bootstrap.DmBuildSysDataFromBackup;
  if (!g_save_bootstrap.ready)
    fprintf(stderr, "AVISO: save bootstrap incompleto\n");
}

static void sonic_save_bootstrap_log_progress(const char *where) {
  if (!g_save_bootstrap.IsStageUnlocked || !g_save_bootstrap.IsStageClear)
    return;
  void *progress = g_save_bootstrap.SProgressCreateInstance ?
      g_save_bootstrap.SProgressCreateInstance(0) : NULL;
  int unlock_state = progress && g_save_bootstrap.GetStageUnlockState ?
      g_save_bootstrap.GetStageUnlockState(progress) : -1;
  int ss_state = progress && g_save_bootstrap.GetSsUnlockState ?
      g_save_bootstrap.GetSsUnlockState(progress) : -1;
  int epm_state = progress && g_save_bootstrap.GetEpMetalUnlockState ?
      g_save_bootstrap.GetEpMetalUnlockState(progress) : -1;
  fprintf(stderr, "=== save progress %s: unlock_state=%d ss=%d epm=%d ===\n",
          where, unlock_state, ss_state, epm_state);
  for (int sid = 0; sid <= 4; sid++) {
    fprintf(stderr, "=== save progress stage %d: unlocked=%d clear=%d ===\n",
            sid, g_save_bootstrap.IsStageUnlocked(0, sid),
            g_save_bootstrap.IsStageClear(0, sid));
  }
}

static void sonic_save_bootstrap_poll(unsigned long frame) {
  if (!g_save_bootstrap.ready || g_save_bootstrap.built)
    return;
  if (!g_save_bootstrap.AoStorageLoadIsFinished())
    return;

  if (!g_save_bootstrap.AoStorageLoadIsSuccessed()) {
    if (!g_save_bootstrap.missing_logged && frame > 120) {
      int err = g_save_bootstrap.AoStorageGetError ?
          g_save_bootstrap.AoStorageGetError() : -1;
      fprintf(stderr, "=== save bootstrap: load finished without success err=%d ===\n", err);
      g_save_bootstrap.missing_logged = 1;
    }
    return;
  }

  fprintf(stderr, "=== save bootstrap: DmBuildSysDataFromBackup @frame %lu ===\n", frame);
  g_save_bootstrap.DmBuildSysDataFromBackup();
  if (g_save_bootstrap.UpdateStageUnlockState)
    g_save_bootstrap.UpdateStageUnlockState();
  g_save_bootstrap.built = 1;
  sonic_save_bootstrap_log_progress("after-build");
}

static void sonic_native_save_load_start(void) {
  unsigned long uid = 0, account = 0;
  void (*AoAccountSetCurrentIdStart)(unsigned long) =
      (void *)so_find_addr_safe("_Z26AoAccountSetCurrentIdStartm");
  void (*AoStorageClearError)(void) =
      (void *)so_find_addr_safe("_Z19AoStorageClearErrorv");
  void (*AoStorageLoadStart)(unsigned long, void *, unsigned long,
                             unsigned long, unsigned long) =
      (void *)so_find_addr_safe("_Z18AoStorageLoadStartmPvmmm");
  void *(*GetBackup)(unsigned long) =
      (void *)so_find_addr_safe("_ZN2gs4user5CUtil9GetBackupEm");

  memset(&g_native_save_load, 0, sizeof(g_native_save_load));
  g_native_save_load.uid = uid;
  g_native_save_load.AoStorageLoadIsFinished =
      (void *)so_find_addr_safe("_Z23AoStorageLoadIsFinishedv");
  g_native_save_load.AoStorageLoadIsSuccessed =
      (void *)so_find_addr_safe("_Z24AoStorageLoadIsSuccessedv");
  g_native_save_load.AoStorageGetError =
      (void *)so_find_addr_safe("_Z17AoStorageGetErrorv");
  g_native_save_load.CopyBackupComp =
      (void *)so_find_addr_safe("_ZN2gs4user5CUtil14CopyBackupCompEm");
  g_native_save_load.SetSaveEnable =
      (void *)so_find_addr_safe("_ZN2gs4user5CUtil13SetSaveEnableEml");
  g_native_save_load.DmBuildSysDataFromBackup =
      (void *)so_find_addr_safe("_Z24DmBuildSysDataFromBackupv");

  if (!AoStorageClearError || !AoStorageLoadStart || !GetBackup ||
      !g_native_save_load.AoStorageLoadIsFinished ||
      !g_native_save_load.AoStorageLoadIsSuccessed ||
      !g_native_save_load.CopyBackupComp ||
      !g_native_save_load.SetSaveEnable ||
      !g_native_save_load.DmBuildSysDataFromBackup) {
    fprintf(stderr, "AVISO: native save load incompleto, mantendo fluxo normal\n");
    return;
  }

  if (AoAccountSetCurrentIdStart)
    AoAccountSetCurrentIdStart(account);
  void *backup = GetBackup(uid);
  if (!backup) {
    fprintf(stderr, "AVISO: native save load sem buffer de backup\n");
    return;
  }

  fprintf(stderr, "=== native save load start uid=%lu account=%lu backup=%p ===\n",
          uid, account, backup);
  AoStorageClearError();
  AoStorageLoadStart(account, backup, 1536, 0x594, 0x5bc);
  g_native_save_load.pending = 1;
  sonic_native_save_load_poll("start");
}

static void load_module(const char *name, int heap_mb, DynLibFunction *tbl, int n) {
  size_t hs = (size_t)heap_mb * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) { fprintf(stderr, "mmap %d MB falhou\n", heap_mb); exit(1); }
  g_engine_heap_base = (uintptr_t)heap;
  g_engine_heap_end = (uintptr_t)heap + hs;
  fprintf(stderr, "== carregando %s (heap %p, %d MB) ==\n", name, heap, heap_mb);
  if (so_load(name, heap, hs) < 0) { fprintf(stderr, "so_load(%s) falhou\n", name); exit(1); }
  if (so_relocate() < 0) { fprintf(stderr, "so_relocate falhou\n"); exit(1); }
  so_resolve(tbl, n, 0);
  so_finalize();
  so_flush_caches();
  so_execute_init_array();
  fprintf(stderr, "== %s: text=%p+%zu data=%p+%zu ==\n", name,
          text_base, text_size, data_base, data_size);
}

/* ---- fox JNI entry points (Java_com_mineloader_fox_foxJniLib_*) ---- */
typedef void *JEnv;
static struct {
  void (*init)(JEnv, void *, void *, void *);
  void (*SetGamePath)(JEnv, void *, void *, void *);
  void (*coreGetLPKFileInfo)(JEnv, void *, void *, void *);
  void (*SetLanguageId)(JEnv, void *, int);
  void (*DrawEGLCreated)(JEnv, void *);
  void (*DrawFrame)(JEnv, void *);
  void (*GameProcess)(JEnv, void *);
  void (*FileProcess)(JEnv, void *);
  void (*HasController)(JEnv, void *, int);
  void (*SetPadData)(JEnv, void *, int, int, int, int, int, int);
  void (*SetTPData)(JEnv, void *, int, int, int, int);
  void (*resumeEvent)(JEnv, void *);
  void (*WindowFocusChanged)(JEnv, void *, int);
} fox;

#define RES(f, sym) do { fox.f = (void *)so_find_addr_safe(sym); \
  fprintf(stderr, "resolve %-22s = %p\n", #f, (void *)fox.f); } while (0)

static void *g_env, *g_thiz;
/* thread dedicada de file-system: roda amFS_proc (loop infinito, cond_wait). */
static void *fs_thread_fn(void *arg) {
  (void)arg;
  fox.FileProcess(g_env, g_thiz);
  return NULL;
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  fprintf(stderr, "=== SONIC 4 EPISODE II (NN/fox) so-loader / NextOS armv7 Mali-450 ===\n");

  if (!getenv("SONIC_NO_CRASHLOG")) sonic_install_crash_handler();

  /* mata instância anterior + confirma 0 antes de tocar no fb/EGL (regra do device). */
  sonic_kill_other_instances();

  /* Config que ANTES vinha do launcher — agora no binário (launcher enxuto/padrão
     PortMaster). overwrite=0: se o ambiente já definiu, respeitamos.
     ⚠️ NUNCA forçamos SDL_VIDEODRIVER/SDL_AUDIODRIVER nem driver de GPU: o
     sistema/SDL escolhem wayland/kmsdrm/fbdev e pulse/alsa/pipewire sozinhos. */
  setenv("SDL_NO_SIGNAL_HANDLERS", "1", 0);            /* SDL não pisa nos sinais */
  setenv("SDL2COMPAT_FORCE_FULLSCREEN_DESKTOP", "1", 0);
  setenv("SDL_VIDEO_FULLSCREEN_DESKTOP", "1", 0);
  setenv("SONIC_DATADIR", ".", 0);  /* cwd = GAMEDIR (launcher faz cd); save/dados aqui */
  sonic_detect_session_runtime();   /* acha XDG_RUNTIME_DIR/WAYLAND_DISPLAY se a sessão não exportou (muOS sem som / ROCKNIX sem vídeo) */

  jni_shim_set_package("com.sega.sonic4episode2", 22);
  { const char *d = getenv("SONIC_DATADIR"); jni_shim_set_local_path(d ? d : "."); }

  build_base_table();
  load_module(GAME_SO, GAME_HEAP_MB, g_base, g_base_n);
  if (getenv("SONIC_DEBUG")) { fprintf(stderr, "== load_module OK, aplicando patches ==\n"); fflush(stderr); }

  /* destravar trial -> jogo completo: GsTrialIsTrial() -> 0 */
  patch_ret0("_Z14GsTrialIsTrialv");
  patch_ret0("_Z21GsTrialIsTrial_VerTwov");
  /* sem camada de vídeo: forçar "vídeo não está tocando" p/ o game passar do
     intro (clMovie poll videoIsPlaying/MediaPlayerisPlaying espera o fim). */
  if (!getenv("SONIC_KEEPVIDEO")) {
    patch_ret0("_Z14videoIsPlayingv");
    /* clMovie::willPlayMovieBeforeGameStart(int) -> 0: pula o movie de intro
       (SEGA logo) de vez, sem precisar da camada de vídeo. */
    patch_ret0("_ZNK2gm5movie7clMovie28willPlayMovieBeforeGameStartEi");
    /* clMovie::isEnd() -> 1: o game espera o movie de intro "terminar". */
    patch_retval("_ZN2gm5movie7clMovie5isEndEv", 1);
  }
  /* 🔊 MediaPlayerisPlaying: estado REAL da música (não mais ->0). Conserta o jingle do
     1up (o poll de estado da BGM usava isto; ->0 matava o jingle no gameplay). O intro já
     é pulado por willPlayMovie->0 + videoIsPlaying->0. SONIC_MP_PLAYING_ZERO=1 volta ao ->0
     antigo (fallback se algum device travar no intro). */
  if (getenv("SONIC_MP_PLAYING_ZERO"))
    patch_ret0("_Z20MediaPlayerisPlayingi");
  else
    patch_arm_jump("_Z20MediaPlayerisPlayingi", (void *)my_MediaPlayerisPlaying);
  /* Sonic4F2F::isGamePause -> 0: o F2F pausa o jogo (ads/consent que não temos)
     e fox_FrameUpdate pula amTaskExecute (state machine) qdo pausado -> preto. */
  if (!getenv("SONIC_KEEPPAUSE"))
    patch_ret0("_ZN9Sonic4F2F11isGamePauseEv");
  /* 🔑 SJni_IsUpshellShow -> 0: chama CallBooleanMethod JNI ("a tela de upsell/ads
     está aberta?"). Nosso jni_shim devolve 1 (true) por default => a engine acha que
     o upshell está SEMPRE aberto e o título trava em CStateInitialize::Next pra sempre
     (o gate 1 nunca libera). Forçar 0 (nenhum upsell) destrava a state machine do título. */
  if (!getenv("SONIC_KEEPUPSHELL"))
    patch_ret0("_Z18SJni_IsUpshellShowv");
  /* 🔑 GATE DO MENU (title -> menu): CStateWaitSignIn::Next só avança se o usuário
     estiver habilitado e o setup tiver terminado. Não forçamos
     GsUserSetupIsCompleted: a task nativa de setup precisa rodar porque ela carrega
     foxsave_0.dat e popula o backup global antes do menu/continue. */
  if (!getenv("SONIC_KEEPSIGNIN")) {
    patch_retval("_Z14GsUserIsEnablem", 1);
    /* GsUserIsSaveEnable precisa ficar real: o native save load abaixo carrega
       foxsave_0.dat e seta o flag via CUtil::SetSaveEnable. Forçar return 1
       mascarava falha de load e fazia o menu seguir com backup vazio. */
  }
  /* 🔑 AD INTERSTICIAL: ao selecionar Start, onMainMenuToWorldMap chama
     showInterstitial (ad fullscreen entre telas). Sem a camada de ad Java, o
     callback que carrega o world map NUNCA dispara → tela laranja travada.
     showInterstitial pula o ad se isUserRemoveAds()!=0 OU getInternetState()==0
     → aí chama o callback direto (carrega o world map). Forçar ambos. */
  if (!getenv("SONIC_KEEPADS")) {
    /* showInterstitial GUARDA o callback (cria o jogo) só se getInternetState!=0
       (path 0x4f1470, com o ad-obj null do nosso stub). getInternetState->1 garante
       que guarda. isUserRemoveAds->0 (ads não removidos) pra não pular. Depois
       disparamos o callback no loop via callbackInterstitialAds. */
    patch_ret0("_ZN12F2FExtension15isUserRemoveAdsEv");      /* ads NÃO removidos */
    patch_retval("_ZN12F2FExtension16getInternetStateEv", 1); /* internet "ON" */
  }
  /* 🔓 SONIC_UNLOCK_ALL (debug/teste): libera TODAS as fases independente do save —
     IsStageUnlocked/IsStageClear -> 1 (gate per-stage que o world map/stage-select usa).
     Pra reproduzir bugs em qualquer fase (ex.: Metal Sonic/Episode Metal) sem precisar
     do save certo. GetEpMetalUnlockState -> alto destrava as fases do Episode Metal. */
  if (getenv("SONIC_UNLOCK_ALL")) {
    patch_retval("_ZN2gs6backup7utility15IsStageUnlockedEm21tag_GSE_MAIN_STAGE_ID", 1);
    patch_retval("_ZN2gs6backup7utility12IsStageClearEm21tag_GSE_MAIN_STAGE_ID", 1);
    patch_retval("_ZN2gs6backup9SProgress21GetEpMetalUnlockStateEv", 8);
    fprintf(stderr, "=== SONIC_UNLOCK_ALL: todas as fases liberadas (IsStageUnlocked/Clear->1, EpMetal->8) ===\n");
  }
  /* 🔑 menu Finalize trava: `CMainMenuStateFinalize::Next` espera o demo manager
     terminar o teardown (vtbl[20]/IsClean), mas nosso fake (dmSoundEffectIsSetUpEnd
     ->1) mantém o recurso "carregado" => IsClean nunca true => menu nunca finaliza
     => próxima tela (world map) nunca carrega (laranja). Patch: `bne` (+0x20) -> `b`
     (sempre avança, pula a espera do teardown). */
  /* ⚠️ ERRADOS (opt-in agora): estes patches roteavam o menu pro EXIT em vez da
     transição Decision→onMainMenuToMainGame. Sem eles, onMainMenuToMainGame É
     chamado (a transição correta "Start"→jogo). */
  if (getenv("SONIC_FORCEMENUFINAL")) {
#ifndef __aarch64__
    patch_word_at("_ZN2dm8mainmenu22CMainMenuStateFinalize4NextEv", 0x20, 0xea000002);
#else
    fprintf(stderr, "AVISO: SONIC_FORCEMENUFINAL nao re-derivado p/ arm64, ignorado\n");
#endif
  }
  if (getenv("SONIC_FORCEEXITEM"))
    patch_ret0("_ZN9Sonic4F2F11isVisibleExENS_12EX_MENU_ITEME");
  /* gate3 do título: CStateInitialize::Next espera CDemoResourceManager IsValid()
     (recursos do attract-demo do evento 3) que nunca valida (id 2 não carrega).
     NOP no `beq` (offset +0x5c) faz a state machine avançar pro Opening/LogoMainFadeIn
     (mostra o título: bg + logo SONIC, que carregam) -> Waiting, SEM o crash que
     forçar IsValid->1 global causava (over-advance pro save). */
  if (!getenv("SONIC_KEEPDEMOGATE"))
#ifdef __aarch64__
    /* arm64 v3: o gate é `cbz w0,<skip>` após IsValid() em +0x54 -> NOP A64. */
    patch_word_at("_ZN2dm5title16CStateInitialize4NextEv", 0x54, 0xd503201f);
#else
    patch_word_at("_ZN2dm5title16CStateInitialize4NextEv", 0x5c, 0xe1a00000);
#endif
  /* fallback opt-in: forçar IsValid->1 global (crasha no save, só p/ depurar). */
  if (getenv("SONIC_FORCEDEMOGATE"))
    patch_retval("_ZThn20_NK2dm8resource20CResourceManagerTask7IsValidENS0_12EDemoEventID4TypeE", 1);
  /* 🔑 fingir SÓ o attract-demo do título (resType 6) como carregado, mantendo os
     outros recursos REAIS (evita o crash do force-global IsValid->1). A função local
     do is-loaded do title-demo = container CManagerState<CTitleViewTask>::Act+0x20
     (0x2522a4): checa o objeto demo global (null)->0. Forçar return 1 deixa o
     demo-gate do título/menu passar SEM o attract-demo (emblema vazio). */
  /* fake-sound do demo: por padrão NÃO aplicado (o launcher antigo setava sempre
     SONIC_NOFAKESOUND=1). Opt-in via SONIC_FAKESOUND p/ depurar o demo-gate:
     dmSoundEffectIsSetUpEnd->1 (o is-loaded que mais falha; sem som no demo é
     inofensivo). */
  if (getenv("SONIC_FAKESOUND")) {
    patch_retval("_ZN2dm2se23dmSoundEffectIsSetUpEndEv", 1);
  }

  /* 🔑 F2F age gate / GDPR consent: ao apertar "Press any button" o jogo chama
     showAgeGate (dialog Java de idade/consent que não temos) e espera a resposta.
     Bypass: isEnoughtAge->1 (idade ok => haveRemoveAgeGate=1) + isConsentCountry->0
     (não é país GDPR => pula o consent). Assim avança do título pro menu sem o dialog. */
  if (!getenv("SONIC_KEEPAGEGATE")) {
    patch_retval("_ZN12F2FExtension12isEnoughtAgeEv", 1);
    patch_ret0("_ZN12F2FExtension16isConsentCountryEv");
    patch_ret0("_ZN12F2FExtension19getIsConsentCountryEv");
  }

  if (!getenv("SONIC_NOSPLIT_DRAW_PHASE"))
    patch_arm_jump("_Z17amThreadCheckDrawl", (void *)sonic_amThreadCheckDraw);

  /* 🪶 LOWFX de volta como DEFAULT (FULLFX não resolveu os bugs reais — vídeo/crash/rastros
     — e custa perf). Bloom+sombras OFF por default. SONIC_FULLFX=1 restaura tudo;
     SONIC_NOBLOOM/SONIC_NOSHADOW overrides finos. (O fix REAL de rastro = glClear por frame,
     gated por SONIC_FBCLEAR, independente de FX.) */
  int sonic_lowfx = !env_flag_enabled("SONIC_FULLFX");
  /* - bloom (SsConstBloomIsEnable->0): pula extract hi-luminance + blur gaussiano
       + merge (3 passes full-screen por frame). Mantém água/efeitos de cena. */
  if (sonic_lowfx || env_flag_enabled("SONIC_NOBLOOM")) {
    patch_ret0("_Z20SsConstBloomIsEnablev"); /* bloom off */
    fprintf(stderr, "=== LOWFX: bloom desligado (perf) ===\n");
  }
  /* 🌈 SONIC_FORCETONEMAP: força o TONE-MAP (HDR->LDR Reinhard) LIGADO. O tone-map é
     SEPARADO do bloom (SsConstTonemapIsEnable). Desligar o bloom pode ter matado o
     tone-map junto -> o HDR das luzes do cassino estoura pra branco. Forçar o tone-map
     ON (mantendo bloom off) deve corrigir a exposição SEM o blur caro. */
  if (env_flag_enabled("SONIC_FORCETONEMAP")) {
    patch_retval("_Z23SsConstTonemapIsEnablev", 1);
    fprintf(stderr, "=== SONIC_FORCETONEMAP: tone-map HDR->LDR forcado ON ===\n");
  }
  /* 🛡️ FIX crash ao sair da fase (default ON; SONIC_NO_RELSAFE desliga): redireciona
     _amDrawReleaseTexture pra nossa versão que valida a lista antes de liberar. */
  if (!getenv("SONIC_NO_RELSAFE")) {
    patch_arm_jump("_Z21_amDrawReleaseTextureP14AMS_REGISTLIST",
                   (void *)my_amDrawReleaseTexture);
    fprintf(stderr, "=== RELSAFE: _amDrawReleaseTexture protegido (stale-P do exit) ===\n");
  }
  /* 🛡️ DEMOGUARD (default ON; SONIC_NO_DEMOGUARD desliga): protege o teardown do CStartDemo contra
     use-after-free. DUAS camadas: (1) hook do ep2::ReleaseInstance (release limpo p/ esse caminho);
     (2) recuperação GERAL por faixa de PC no crash handler (pega o destrutor por QUALQUER caminho —
     Act Clear->mapa, delete direto etc; foi o que faltava no crash do tester). */
  g_demoguard_on = getenv("SONIC_NO_DEMOGUARD") == NULL;
  if (g_demoguard_on) {
    patch_arm_jump("_ZN2gm10start_demo3ep210CStartDemo15ReleaseInstanceEv",
                   (void *)my_ep2_CStartDemo_ReleaseInstance);
    fprintf(stderr, "=== DEMOGUARD: ~CStartDemo protegido (hook ReleaseInstance + recuperacao geral por PC) ===\n");
  }
  /* 🧪 SONIC_SIMDEMOCRASH: SIMULA o crash do tester chamando o ~CStartDemo DIRETO com um objeto
     garbage (o caminho REAL do tester NÃO passa pelo ReleaseInstance — ex.: teardown do Act Clear).
     O crash cai DENTRO do destrutor -> exercita a RECUPERAÇÃO GERAL por faixa de PC:
       - guarda ON (default): RECUPERA (redireciona pro epílogo/caller) e segue -> dá pra JOGAR.
       - guarda OFF (+SONIC_NO_DEMOGUARD): crash CRU (jogo fecha) -> prova que é a guarda que salva. */
  if (getenv("SONIC_SIMDEMOCRASH")) {
    int guard_off = getenv("SONIC_NO_DEMOGUARD") != NULL;
    uintptr_t dtor = so_find_addr_safe("_ZTv0_n12_N2gm10start_demo3ep210CStartDemoD0Ev");
    fprintf(stderr, "=== SIMDEMOCRASH: ~CStartDemo@0x%lx com this=garbage (guarda=%s) ===\n",
            (unsigned long)dtor, guard_off ? "OFF (deve FECHAR)" : "ON (deve RECUPERAR)");
    if (dtor) {
      void (*d)(void *) = (void (*)(void *))(dtor & ~(uintptr_t)1);
      d((void *)0x1);                           /* crash DENTRO do destrutor; guarda ON -> recupera */
      fprintf(stderr, "=== SIMDEMOCRASH: SOBREVIVI (recuperado=%lu) -> segue ===\n",
              g_demo_guard_recovered);
    }
  }
  /* 🔆 SONIC_FREEZETONEMAP (SUSPEITO #1 do cassino "Electric Road"): CONGELA a
     AUTO-EXPOSIÇÃO. ChangeToneMapParam(midgray,lwhite) é chamado por frame pela
     adaptação de exposição — seta os destinos s_tonemap_*_dst+flags. No gameplay essa
     adaptação dispara e a exposição vai pro errado -> fundo ESTOURA pra branco; no
     PAUSE a lógica congela -> exposição para -> correto (fundo preto, luzes finas).
     No-opar ChangeToneMapParam congela a exposição no valor de Reset (fixo) = igual
     ao pause durante o gameplay. Se o cassino ficar correto -> era a auto-exposição. */
  if (env_flag_enabled("SONIC_FREEZETONEMAP")) {
    patch_ret0("_ZN2gm3pfx7CPfxSys18ChangeToneMapParamEff");
    fprintf(stderr, "=== SONIC_FREEZETONEMAP: auto-exposicao CONGELADA (exposicao fixa) ===\n");
  }
  /* 🌑 no-op nos draws de sombra de objeto/motion: pula o passe de sombra
     (shadow-map / blob) — ganho de fillrate+drawcall no Mali.
     NAO toca em GmShadowBuildCheck (gate de loading -> travaria). */
  if (sonic_lowfx || env_flag_enabled("SONIC_NOSHADOW")) {
    patch_ret0("_Z18SsDrawObjectShadowmP10NNS_OBJECTP12_NNS_TEXLISTy");
    patch_ret0("_Z18SsDrawObjectShadowP10NNS_OBJECTP12_NNS_TEXLISTy");
    patch_ret0("_Z24SsDrawMotionObjectShadowmP10AMS_MOTIONP12_NNS_TEXLISTy");
    patch_ret0("_Z24SsDrawMotionObjectShadowP10AMS_MOTIONP12_NNS_TEXLISTy");
    fprintf(stderr, "=== LOWFX: sombras de objeto desligadas (perf) ===\n");
  }
  /* 💡 SONIC_NOLIGHTMASK (DIAG/teste do bug do cassino): desliga o post-effect de
     máscara de luz (amPostEFLightMaskDraw) + distortion. O "blob branco estourado" do
     cassino é esse light-mask. Se sumir com isso, é ele. (GmPlyPostEfctLightMaskColGet
     lê uma intensidade clampada a 0xff -> luz branca máxima quando alto.) */
  if (getenv("SONIC_NOLIGHTMASK")) {
    patch_ret0("_Z21amPostEFLightMaskDrawmPA16_fS0_");
    patch_ret0("_Z22amPostEFDistortionDrawmPA16_fS0_");
    fprintf(stderr, "=== SONIC_NOLIGHTMASK: light-mask/distortion post-FX desligados ===\n");
  }
  /* 💡 SONIC_NOPOSTFX (DIAG do cassino estourado): mata o EXECUTOR de post-effect
     inteiro (_amPostEFExecEffect + amPostEFUpdate). Se as faixas/blow-up do cassino
     sumirem -> é o sistema de post-effect (feixe de luz) acumulando. Isola a raiz. */
  if (getenv("SONIC_NOPOSTFX")) {
    patch_ret0("_Z19_amPostEFExecEffectl");
    patch_ret0("_Z14amPostEFUpdatev");
    fprintf(stderr, "=== SONIC_NOPOSTFX: executor de post-effect desligado ===\n");
  }
  /* 🌟 SONIC_NOGODRAY: desliga o GOD RAY (raios de luz do fundo, gm::mapfar::C_MGR).
     O cassino tem god-ray com radial blur que está ESTOURANDO em faixas brancas gigantes
     (gsGxGetGodRayRadialBlur). Pular o draw remove as faixas. Se sumir, é o god-ray. */
  if (getenv("SONIC_NOGODRAY")) {
    patch_ret0("_ZN2gm6mapfar5C_MGR14FuncDrawGodrayEP16_OBS_OBJECT_WORK");
    fprintf(stderr, "=== SONIC_NOGODRAY: god-ray (faixas de luz do fundo) desligado ===\n");
  }
  /* 💧 SONIC_NOWATERFX (opt-in, NÃO no LOWFX por default — mexe mais no visual):
     no-op nos EFEITOS extras de água (ripple/waterfall-split), mantendo a SUPERFÍCIE
     (a água não some). Reduz overdraw de alpha das cenas de água. */
  if (env_flag_enabled("SONIC_NOWATERFX")) {
    patch_ret0("GmEffectWaterRippleBuild");
    patch_ret0("GmEffectWaterRippleFlush");
    patch_ret0("GmGmkWaterfallSplitBuild");
    patch_ret0("GmGmkWaterfallSplitFlush");
    fprintf(stderr, "=== SONIC_NOWATERFX: ripple/waterfall desligados (perf) ===\n");
  }

  void *env = NULL, *vm = NULL;
  jni_shim_init(&vm, &env);
  void *thiz = (void *)0x53000001; /* fake jobject/jclass */
  g_env = env; g_thiz = thiz;

  /* JNI_OnLoad: o Android chamaria isso ao carregar o .so, setando o global
     JavaVM (f2fextension GetJNIEnv lê esse global -> NULL deref sem isso). */
  { int (*jniOnLoad)(void *, void *) = (void *)so_find_addr_safe("JNI_OnLoad");
    if (jniOnLoad) { int v = jniOnLoad(vm, NULL);
      fprintf(stderr, "JNI_OnLoad(vm) = 0x%x\n", v); }
    else fprintf(stderr, "AVISO: JNI_OnLoad não encontrado\n"); }

  egl_shim_create_window();
  egl_shim_bind_main();  /* GLSurfaceView: contexto current na thread do DrawFrame */

  RES(init,               "Java_com_mineloader_fox_foxJniLib_init");
  RES(SetGamePath,        "Java_com_mineloader_fox_foxJniLib_SetGamePath");
  RES(coreGetLPKFileInfo, "Java_com_mineloader_fox_foxJniLib_coreGetLPKFileInfo");
  RES(SetLanguageId,      "Java_com_mineloader_fox_foxJniLib_SetLanguageId");
  RES(DrawEGLCreated,     "Java_com_mineloader_fox_foxJniLib_DrawEGLCreated");
  RES(DrawFrame,          "Java_com_mineloader_fox_foxJniLib_DrawFrame");
  RES(GameProcess,        "Java_com_mineloader_fox_foxJniLib_GameProcess");
  RES(FileProcess,        "Java_com_mineloader_fox_foxJniLib_FileProcess");
  RES(HasController,      "Java_com_mineloader_fox_foxJniLib_HasController");
  RES(SetPadData,         "Java_com_mineloader_fox_foxJniLib_SetPadData");
  RES(SetTPData,          "Java_com_mineloader_fox_foxJniLib_SetTPData");
  RES(resumeEvent,        "Java_com_mineloader_fox_foxJniLib_resumeEvent");

  const char *gamedir = getenv("SONIC_DATADIR");
  if (!gamedir) gamedir = ".";
  const char *lpk = getenv("SONIC_LPK");
  if (!lpk) lpk = "data/main.22.com.sega.sonic4episode2.obb"; /* o OBB = LPK */

  /* SetGamePath(env, thiz, int id, String path) -> tsSetFileRootPath(id,path,0):
     id==255(0xff) => tsInitFileRootLPK(path) = fopen+indexa o LPK (o OBB!);
     id<254       => guarda 'path' como root de arquivos soltos.
     PRECISA vir ANTES do init (init lê font.nft de dentro do LPK). */
  fprintf(stderr, "=== fox: SetGamePath(255, LPK=%s) ===\n", lpk);
  if (fox.SetGamePath) fox.SetGamePath(env, thiz, (void *)255, jni_shim_new_string(lpk));

  fprintf(stderr, "=== fox: SetGamePath(0, dir=%s) ===\n", gamedir);
  if (fox.SetGamePath) fox.SetGamePath(env, thiz, (void *)0, jni_shim_new_string(gamedir));

  /* idioma: tabela lang_name_tbl = 0:JP 1:US(inglês) 2:FR 3:IT 4:GE 5:SP 6:KO 7:CH 8:TA.
     Id 1 = US/inglês (id 0 carregava as variantes _JP japonesas). */
  fprintf(stderr, "=== fox: SetLanguageId(1=US/EN) ===\n");
  if (fox.SetLanguageId) fox.SetLanguageId(env, thiz, 1);
  /* 🔑 SetLanguageId seta o global do SetAndroidLanguage (usado no TÍTULO), mas o
     MENU lê de OUTRO global via GsEnvGetLanguage() (default 0=JP) -> menu em japonês!
     Forçar GsEnvGetLanguage()->1 (US) deixa o menu/UI em inglês também. */
  if (!getenv("SONIC_KEEPJP"))
    patch_retval("_Z16GsEnvGetLanguagev", 1);

  /* f2fextension (camada F2F/ads): precisa do context/JavaVM senão getF2FJavaVM()
     retorna NULL -> crash em Android_getLocalPath. Chamar os setups JNI. */
  void (*f2f_setCtx)(JEnv, void *, void *) =
      (void *)so_find_addr_safe("Java_com_sega_f2fextension_f2fextensionInterface_nativeSetContext");
  void (*f2f_setObj)(JEnv, void *, void *) =
      (void *)so_find_addr_safe("Java_com_sega_f2fextension_f2fextensionInterface_SetJavaObj");
  void (*f2f_setApk)(JEnv, void *, void *) =
      (void *)so_find_addr_safe("Java_com_sega_f2fextension_f2fextensionInterface_nativeSetApkPath");
  if (f2f_setObj) { fprintf(stderr, "f2f SetJavaObj\n");    f2f_setObj(env, thiz, thiz); }
  if (f2f_setCtx) { fprintf(stderr, "f2f nativeSetContext\n"); f2f_setCtx(env, thiz, thiz); }
  if (f2f_setApk) { fprintf(stderr, "f2f nativeSetApkPath\n");
                    f2f_setApk(env, thiz, jni_shim_new_string("sonic4ep2.apk")); }

  /* 🪶 SONIC_RENDERSCALE=N (50..99, default 100=off): downscale interno. O engine
     renderiza em N% e o present faz upscale p/ a janela cheia. Maior lever de GPU
     (fillrate ~ pixel²), custo = nitidez. OPT-IN. ⚠️ se sair no canto (não upscale),
     precisa do override de viewport no present. */
  int render_w = sonic_screen_w, render_h = sonic_screen_h;
  { const char *rs = getenv("SONIC_RENDERSCALE"); int pct = rs ? atoi(rs) : 100;
    if (pct >= 50 && pct < 100) {
      render_w = (sonic_screen_w * pct / 100) & ~1;
      render_h = (sonic_screen_h * pct / 100) & ~1;
      fprintf(stderr, "=== SONIC_RENDERSCALE %d%%: render %dx%d (janela %dx%d) ===\n",
              pct, render_w, render_h, sonic_screen_w, sonic_screen_h);
    } }

  /* 🔑 setScreenSize: a engine (foxShaderInit -> amRenderCreate) lê a resolução de
     tela de um global (2 floats) p/ dimensionar os FBOs/render targets. O Java
     chamaria setScreenSize(w,h) no onSurfaceChanged; SEM isso o global fica 0.0/0.0
     => FBO 0x0 INCOMPLETE => glDraw* falham (GL_INVALID_FRAMEBUFFER_OPERATION) =>
     TELA PRETA. setScreenSize faz `stm {w,h}` cru => são jfloat em regs CORE (JNI
     softfp) => passamos os BITS do float em r2/r3 (declarar args como unsigned, NÃO
     float, senão o ABI hardfp manda em s0/s1). */
  {
    extern int sonic_screen_w, sonic_screen_h;
#ifdef __aarch64__
    /* AArch64 (AAPCS64): jfloat vai em registrador FP (s0/s1) — passar float REAL. */
    void (*setScreenSize)(void *, void *, float, float) =
        (void *)so_find_addr_safe(
            "Java_com_sega_f2fextension_f2fextensionInterface_setScreenSize");
    void (*setScreenScaleDesity)(void *, void *, float) =
        (void *)so_find_addr_safe(
            "Java_com_sega_f2fextension_f2fextensionInterface_setScreenScaleDesity");
    if (setScreenSize) {
      fprintf(stderr, "=== setScreenSize(%d x %d) [A64 fp] ===\n",
              render_w, render_h);
      setScreenSize(env, thiz, (float)render_w, (float)render_h);
    } else fprintf(stderr, "AVISO: setScreenSize não encontrado\n");
    if (setScreenScaleDesity) setScreenScaleDesity(env, thiz, 1.0f);
#else
    /* armhf softfp: jfloat vem em regs CORE — passar os BITS do float. */
    void (*setScreenSize)(void *, void *, unsigned, unsigned) =
        (void *)so_find_addr_safe(
            "Java_com_sega_f2fextension_f2fextensionInterface_setScreenSize");
    void (*setScreenScaleDesity)(void *, void *, unsigned) =
        (void *)so_find_addr_safe(
            "Java_com_sega_f2fextension_f2fextensionInterface_setScreenScaleDesity");
    if (setScreenSize) {
      union { float f; unsigned u; } w, h;
      w.f = (float)render_w; h.f = (float)render_h;
      fprintf(stderr, "=== setScreenSize(%d x %d) bits=%08x %08x ===\n",
              render_w, render_h, w.u, h.u);
      setScreenSize(env, thiz, w.u, h.u);
    } else fprintf(stderr, "AVISO: setScreenSize não encontrado\n");
    if (setScreenScaleDesity) {
      union { float f; unsigned u; } s; s.f = 1.0f;
      setScreenScaleDesity(env, thiz, s.u);  /* densidade/escala = 1.0 */
    }
#endif
  }

  /* save path: stsSavePathData (buffer global) é o prefixo do save; vazio => o save
     vira "/foxsave_0.dat" no root (sem permissão) => save falha, e o Story Mode pode
     gatear nisso. Setar p/ o dir gravável (SONIC_DATADIR). */
  if (!getenv("SONIC_KEEPSAVEPATH")) {
    char *sp = (char *)so_find_addr_safe("stsSavePathData");
    const char *dd = getenv("SONIC_DATADIR"); if (!dd) dd = ".";
    if (sp) {
      size_t n = strlen(dd);
      snprintf(sp, 196, "%s%s", dd, (n > 0 && dd[n - 1] == '/') ? "" : "/");
      fprintf(stderr, "=== save path = %s ===\n", sp);
    }
  }

  /* 🔑🔑 init(env, thiz, WIDTH, HEIGHT): a JNI init repassa args 3/4 p/ fox_Init(w,h)
     -> amDrawInitVideo(w,h) que dimensiona _am_draw_video (os FBOs/render targets).
     Passávamos NULL,NULL = 0,0 => FBO 0x0 INCOMPLETE => glDraw* falham => TELA PRETA.
     Passar a resolução REAL (1280x720) faz os FBOs ficarem completos e renderizar. */
  fprintf(stderr, "=== fox: init(w=%d h=%d) ===\n", render_w, render_h);
  if (fox.init) fox.init(env, thiz, (void *)(intptr_t)render_w,
                         (void *)(intptr_t)render_h);

  fprintf(stderr, "=== fox: DrawEGLCreated ===\n");
  if (fox.DrawEGLCreated) fox.DrawEGLCreated(env, thiz);

  /* resumeEvent: o jogo começa PAUSADO (isGamePause); sem resume, fox_FrameUpdate
     retorna cedo e PULA amTaskExecute (a state machine) -> nada avança -> preto. */
  fprintf(stderr, "=== fox: resumeEvent (unpause) ===\n");
  if (fox.resumeEvent) fox.resumeEvent(env, thiz);
  sonic_save_bootstrap_init();

  /* FileProcess = amFS_proc = loop da THREAD de file-system (cond_wait quando
     ocioso). Roda na PRÓPRIA thread; o game thread enfileira requests e sinaliza. */
  if (fox.FileProcess) {
    pthread_t fs;
    pthread_create(&fs, NULL, fs_thread_fn, NULL);
    fprintf(stderr, "=== FS thread iniciada (FileProcess) ===\n");
  }

  if (env_flag_enabled("SONIC_FORCE_NATIVE_SAVE_LOAD"))
    sonic_native_save_load_start();

  if (getenv("SONIC_USEUSERSETUP")) {
    void (*GsUserSetupStart)(unsigned long, unsigned long) =
        (void *)so_find_addr_safe("_Z16GsUserSetupStartmm");
    if (GsUserSetupStart) {
      fprintf(stderr, "=== GsUserSetupStart(uid=0, account=0) ===\n");
      GsUserSetupStart(0, 0);
    } else {
      fprintf(stderr, "AVISO: GsUserSetupStart nao encontrado\n");
    }
  }

  /* intro video: a engine chama Android_playIntroVideo e espera o callback
     callBackIntroVideo (Java tocaria o mp4 e sinalizaria o fim). Sem vídeo,
     sinalizamos "terminado" cedo p/ a engine seguir pro título/menu. */
  void (*introCB)(JEnv, void *) =
      (void *)so_find_addr_safe("Java_com_sega_f2fextension_f2fextensionInterface_callBackIntroVideo");
  /* callback do ad intersticial: ao selecionar Start, onMainMenuToMainGame chama
     showInterstitial que GUARDA um callback (que cria o jogo/world map) e espera o
     ad fechar (callbackInterstitialAds do Java). Sem ad Java, disparamos nós: chamar
     callbackInterstitialAds(type=0, callback=0) dispara o callback guardado -> jogo. */
  void (*interCB)(JEnv, void *, int, int) =
      (void *)so_find_addr_safe("Java_com_sega_f2fextension_f2fextensionInterface_callbackInterstitialAds");

  /* input: abrir o 1º gamepad SDL (se houver) */
  SDL_GameController *pad = NULL;
  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
    for (int i = 0; i < SDL_NumJoysticks(); i++)
      if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); if (pad) break; }
    fprintf(stderr, "=== gamepad: %s ===\n", pad ? "aberto" : "nenhum");
  }
  /* 🔧 fox pad bits = MAPA REAL do jogo (decompilado de foxJniLib.s_remapKey, o keycode->bit
     do próprio jogo). gmPadValue = (1<<bitindex). Antes os bits de Y e ombros estavam ERRADOS
     (Y=0x100 era na verdade L1 -> Y "virava left" no world map e sua função nunca disparava).
     Xbox: A=96 X=99 B=97 Y=100 START=108 SELECT=109 L1=102 L2=104 R1=103 R2=105 L3=106 R3=107. */
  #define FOX_UP     0x0001  /* bit0  DPAD_UP   19 */
  #define FOX_DOWN   0x0002  /* bit1  DPAD_DOWN 20 */
  #define FOX_LEFT   0x0004  /* bit2  DPAD_LEFT 21 */
  #define FOX_RIGHT  0x0008  /* bit3  DPAD_RIGHT 22 */
  #define FOX_Y      0x0010  /* bit4  BUTTON_Y  100  (era 0x100 = L1, ERRADO) */
  #define FOX_A_GAME 0x0020  /* bit5  BUTTON_A  96  (decide/jump) */
  #define FOX_X      0x0040  /* bit6  BUTTON_X  99 */
  #define FOX_B      0x0080  /* bit7  BUTTON_B  97  (cancel) */
  #define FOX_L1     0x0100  /* bit8  BUTTON_L1 102 */
  #define FOX_L2     0x0200  /* bit9  BUTTON_L2 104 */
  #define FOX_L3     0x0400  /* bit10 THUMBL    106 */
  #define FOX_R1     0x0800  /* bit11 BUTTON_R1 103 */
  #define FOX_R2     0x1000  /* bit12 BUTTON_R2 105 */
  #define FOX_R3     0x2000  /* bit13 THUMBR    107 */
  #define FOX_BACK   0x4000  /* bit14 BUTTON_SELECT 109 / BACK */
  #define FOX_START  0x8000  /* bit15 BUTTON_START 108 (confirm título/pause) */
  #define FOX_A_MENU (FOX_A_GAME|FOX_START)  /* 0x8020 = A(decide)+START(confirm título) */
  #define FOX_PAUSE  FOX_START               /* pause in-game = START (check do engine é pad&0xC000) */

  /* 🔑 demo-resource: o gate do título (CDemoResourceManager::IsValid evt3) trava
     pq o recurso "MenuDraw" (tipo 2) não está setado (dmMenuDrawIsSetUpEnd=0).
     dmMenuDrawSetUp() cria o singleton MenuDraw -> o is-loaded passa. A engine não
     o chama no nosso fluxo; chamar aqui pode destravar o gate NATURALMENTE (sem o
     NOP do beq) e deixar o menu renderizar. */
  if (!getenv("SONIC_NOSETUPS")) {  /* default-on: cria os singletons do demo set —
       o IsValid quer os DADOS buildados (o build assíncrono do manager não completa;
       type 6=attract-demo precisa de stage). Default = título via bypass do beq. */
    const char *setups[] = {
      "_ZN2dm10menucommon17dmMenuCommonSetUpEv", /* type 1 */
      "_ZN2dm8menudraw15dmMenuDrawSetUpEv",      /* type 2 */
      "_ZN2dm2se18dmSoundEffectSetUpEv",         /* type 3 */
      "_ZN2dm7message11SystemSetUpEv",           /* message */
    };
    for (unsigned i = 0; i < sizeof(setups)/sizeof(setups[0]); i++) {
      void (*fn)(void) = (void *)so_find_addr_safe(setups[i]);
      if (fn) { fprintf(stderr, "=== setup: %s ===\n", setups[i]); fn(); }
      else fprintf(stderr, "AVISO: setup %s não encontrado\n", setups[i]);
    }
  }

  /* "conectar" o pad: HasController(env, thiz, 1) chama Sonic4F2F::setController(true);
     SetPadData(-2/-5) replica os toggles que o Java fazia para o wrapper fox. */
  if (fox.HasController) fox.HasController(env, thiz, 1);
  if (fox.SetPadData) {
    fox.SetPadData(env, thiz, -2, 0, 0, 0, 0, 0);
    fox.SetPadData(env, thiz, -5, 0, 0, 0, 0, 0);
  }

  fprintf(stderr, "=== entrando no loop principal (GameProcess/DrawFrame) ===\n");
  unsigned long frame = 0;
  int prev_a = 0;             /* borda de A p/ disparar interCB 1x por seleção */
  long inter_fire_at = -1;    /* frame agendado p/ 1 disparo de interCB */
  const char *interat = getenv("SONIC_INTERAT"); /* override: 1 disparo no frame N */
  const char *ar = getenv("SONIC_AUTORIGHT_AFTER");
  long autoright_after = ar ? atol(ar) : -1;
  const char *aj = getenv("SONIC_AUTOJUMP_AT");
  long autojump_at = aj ? atol(aj) : -1;
  const char *ap = getenv("SONIC_AUTOPAUSE_AT");
  long autopause_at = ap ? atol(ap) : -1;
  int autopause_state = 0;
  int start_was_down = 0;
  long inter_last_fire_frame = -1000000;
  int inter_gameplay_ignored = 0;
  int prev_mask = 0;
  const char *fs = getenv("SONIC_FRAME_SLEEP_US");
  long frame_sleep_us = fs ? atol(fs) : 0;
  fprintf(stderr, "=== frame sleep us: %ld ===\n", frame_sleep_us);
  g_fbclear = getenv("SONIC_FBCLEAR") != NULL;
  if (g_fbclear) fprintf(stderr, "=== SONIC_FBCLEAR: glClear por frame LIGADO ===\n");
  int gameplay_start_delay_done = 0;
  /* 🔑 CONTINUE/SAVE-COM-PROGRESSO: ao reabrir com save que tem fase interrompida
     (qualquer mundo/mapa já jogado), o título entra em CStateWaitViewPausing:
     OnEnter chama SetContinueShow() + SetContinueStart(0) e Next() fica polando
     isContinueStart() esperando 1 (continuar a fase) ou 2 (ir pro world map).
     Esse 1/2 só viria do diálogo Java SetContinueFlag (que não temos) -> trava
     eterna no título, menu nunca aparece. A flag de continue (global lida SÓ dentro
     de CStateWaitViewPausing::Next) é dirigida aqui: default 2 = caminho nativo
     ClearInterruptionData -> world map/menu (progresso de fases preservado em
     SProgress). SONIC_CONTINUE_MODE=1 resume direto na fase interrompida. */
  int (*sonic_isContinueStart)(void) =
      (void *)so_find_addr_safe("_Z15isContinueStartv");
  void (*sonic_SetContinueStart)(int) =
      (void *)so_find_addr_safe("_Z16SetContinueStarti");
  int sonic_continue_mode = 2;
  { const char *cm = getenv("SONIC_CONTINUE_MODE"); if (cm && *cm) sonic_continue_mode = atoi(cm); }
  int sonic_continue_disabled = getenv("SONIC_NO_CONTINUE_DRIVE") != NULL;
  long sonic_continue_log_n = 0;
  /* 🗺️ SONIC_WARP_STAGE=N: warp direto pra fase N. Força sm_select_stage_id=N (o ID
     que o world map carrega no confirm) + injeta confirm no world map. Enumerar N até
     cair na Electric Road (Episode Metal). Debug only. */
  int *wm_sel_id = (int *)so_find_addr_safe("_ZN2dm9world_map4CFix18sm_select_stage_idE");
  long warp_stage = getenv("SONIC_WARP_STAGE") ? atol(getenv("SONIC_WARP_STAGE")) : -1;
  if (warp_stage >= 0) fprintf(stderr, "=== SONIC_WARP_STAGE=%ld (sel_id=%p) ===\n", warp_stage, (void*)wm_sel_id);
  short *gm_direct = (short *)so_find_addr_safe("gmPaddirectFromPlayer0");
  short *gm_lx = (short *)so_find_addr_safe("gmPadAnalogLXFromPlayer0");
  short *gm_ly = (short *)so_find_addr_safe("gmPadAnalogLYFromPlayer0");
  if (getenv("SONIC_INPUTLOG"))
    fprintf(stderr, "=== gmPad globals direct=%p lx=%p ly=%p ===\n",
            (void *)gm_direct, (void *)gm_lx, (void *)gm_ly);
  /* 🔎 DIAG Y=Left (SONIC_KEYDUMP=1): o bug "Y age como Left no level select" não é
     colisão de bit no nosso FOX mask (Y=0x100, LEFT=0x4 no .data). Hipótese: o engine
     REMAPEIA os bits de tecla de menu (g_gs_env_key_*) em runtime (keymap OUYA/alt).
     Dump dos valores REAIS dessas globais em runtime fecha a questão: se LEFT != 0x4
     (ou == 0x100), é remap. Só LÊ globais (zero efeito no jogo). */
  short *gk_right  = (short *)so_find_addr_safe("g_gs_env_key_right");
  short *gk_left   = (short *)so_find_addr_safe("g_gs_env_key_left");
  short *gk_down   = (short *)so_find_addr_safe("g_gs_env_key_down");
  short *gk_up     = (short *)so_find_addr_safe("g_gs_env_key_up");
  short *gk_decide = (short *)so_find_addr_safe("g_gs_env_key_decide");
  short *gk_cancel = (short *)so_find_addr_safe("g_gs_env_key_cancel");
  long keydump_last = -100000;
  /* Special/bonus stage (moedas): sinal por-frame determinístico.
     g_SsMain = ss::CMain::s_main (singleton da special stage) @vaddr 0x99e480.
     Não é símbolo exportado, então resolvo via o vizinho exportado
     ss::CNet::s_instance (0x99e4a4) e subtraio 0x24. *pp != NULL => estamos na
     special stage. Necessário porque a special stage carrega por
     EvSpecialStageStart/CSSLoadingTask, caminho que NÃO loga marcador de
     "game start" -> sonic_game_started fica preso em 0 -> o A vira FOX_A_MENU
     (bit 0x8000) e o check de pausa da special stage (pad & 0xC000) faz o pulo
     PAUSAR. Em fases normais o log já seta started=1 (por isso só a bônus falha).
     FOX_A_GAME (0x20) mantém o bit "decide menu" (0x20), só dropa o 0x8000 do
     pause: confirma o resultado da bônus normalmente E acaba com a pausa. */
  void **sonic_ss_main_pp = NULL;
  {
    uintptr_t cnet = so_find_addr_safe("_ZN2ss4CNet10s_instanceE");
    if (cnet) sonic_ss_main_pp = (void **)(cnet - 0x99e4a4 + 0x99e480);
    if (getenv("SONIC_INPUTLOG"))
      fprintf(stderr, "=== g_SsMain pp=%p ===\n", (void *)sonic_ss_main_pp);
  }
  for (;;) {
    sonic_frame_for_imports = frame;
    /* --- input: drenar eventos SDL + montar a máscara fox + SetPadData --- */
    SDL_Event ev; while (SDL_PollEvent(&ev)) { /* drena (quit etc) */ }
    int mask = 0;
    int lx = 0, ly = 0;
    int rx = 0, ry = 0;
    int lt = 0, rt = 0;
    int start_down = 0;
    /* gameplay = fase normal (started por log) OU special/bonus stage ativa. */
    int sonic_in_gameplay = sonic_game_started ||
        (sonic_ss_main_pp && *sonic_ss_main_pp);
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    sonic_check_exit_hotkey(pad, ks);
    if (ks) {
      if (ks[SDL_SCANCODE_UP])    { mask |= FOX_UP;    ly = -32768; }
      if (ks[SDL_SCANCODE_DOWN])  { mask |= FOX_DOWN;  ly =  32767; }
      if (ks[SDL_SCANCODE_LEFT])  { mask |= FOX_LEFT;  lx = -32768; }
      if (ks[SDL_SCANCODE_RIGHT]) { mask |= FOX_RIGHT; lx =  32767; }
      if (ks[SDL_SCANCODE_W])     { mask |= FOX_UP;    ly = -32768; }
      if (ks[SDL_SCANCODE_S])     { mask |= FOX_DOWN;  ly =  32767; }
      if (ks[SDL_SCANCODE_A])     { mask |= FOX_LEFT;  lx = -32768; }
      if (ks[SDL_SCANCODE_D])     { mask |= FOX_RIGHT; lx =  32767; }
      if (ks[SDL_SCANCODE_SPACE]||ks[SDL_SCANCODE_Z])
        mask |= sonic_in_gameplay ? FOX_A_GAME : FOX_A_MENU;
      if (ks[SDL_SCANCODE_C])     mask |= FOX_B;
      if (ks[SDL_SCANCODE_X])     mask |= FOX_X;
      if (ks[SDL_SCANCODE_V] || ks[SDL_SCANCODE_Y]) mask |= FOX_Y;
      if (ks[SDL_SCANCODE_Q])     mask |= FOX_L1;
      if (ks[SDL_SCANCODE_E])     mask |= FOX_R1;
      if (ks[SDL_SCANCODE_1])     { mask |= FOX_L2; lt = 32767; }
      if (ks[SDL_SCANCODE_3])     { mask |= FOX_R2; rt = 32767; }
      /* L3/R3 (THUMBL/THUMBR) ficam fora do mask: não são ações no Sonic 4.
         (Com o mapa de bits corrigido já não colidem com pause/confirm; omitir é só
         pra não injetar input espúrio de stick-click.) */
      (void)0;
      if (ks[SDL_SCANCODE_ESCAPE]) mask |= FOX_BACK;
      if (ks[SDL_SCANCODE_RETURN]) {
        start_down = 1;
        if (!sonic_in_gameplay) mask |= FOX_A_MENU | FOX_START;
        else mask |= FOX_PAUSE;
      }
    }
    if (pad) {
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP))    { mask |= FOX_UP;    ly = -32768; }
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  { mask |= FOX_DOWN;  ly =  32767; }
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  { mask |= FOX_LEFT;  lx = -32768; }
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) { mask |= FOX_RIGHT; lx =  32767; }
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A))
        mask |= sonic_in_gameplay ? FOX_A_GAME : FOX_A_MENU;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) mask |= FOX_B;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X)) mask |= FOX_X;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y)) mask |= FOX_Y;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) mask |= FOX_L1;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) mask |= FOX_R1;
      /* LEFTSTICK/RIGHTSTICK (L3/R3) NÃO entram no mask: colidem com PAUSE(0x4000)/
         não são ações no Sonic 4. Ver nota acima. */
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK)) mask |= FOX_BACK;
      if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) {
        start_down = 1;
        if (!sonic_in_gameplay) mask |= FOX_A_MENU | FOX_START;
        else mask |= FOX_PAUSE;
      }
      Sint16 ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
      Sint16 ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
      Sint16 arx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
      Sint16 ary = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
      Sint16 alt = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
      Sint16 art = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
      if (ax < -12000) { mask |= FOX_LEFT;  lx = ax; }
      else if (ax > 12000) { mask |= FOX_RIGHT; lx = ax; }
      if (ay < -12000) { mask |= FOX_UP;    ly = ay; }
      else if (ay > 12000) { mask |= FOX_DOWN; ly = ay; }
      if (arx < -12000 || arx > 12000) rx = arx;
      if (ary < -12000 || ary > 12000) ry = ary;
      if (alt > 12000) { mask |= FOX_L2; lt = alt; }
      if (art > 12000) { mask |= FOX_R2; rt = art; }
    }
    /* auto-press de teste: o trigger é a borda 0->1 (1 frame), então alterna
       0x8000/0 a cada frame após o título carregar -> trigger frequente p/ vencer
       a corrida com o poll do CStateWaiting::Next (SONIC_AUTOSTART). */
    /* press ÚNICO de teste (não contínuo): aperta confirm uma vez ~frame 420 por
       ~5 frames e SOLTA (pressionar contínuo reseta a sequência de saída do título). */
    if (env_flag_enabled("SONIC_AUTOSTART")) {
      /* Pulsos curtos e espaçados: o primeiro sai do título; o segundo confirma
         Start/New Game. Pulsos extras ficam opt-in, porque depois que a fase
         carrega eles podem acionar flows de ad/menu e bagunçar o gameplay. */
      int extra = getenv("SONIC_AUTOSTART_EXTRA") != NULL;
      int autopulse =
          (frame >= 600 && frame < 606) ||
          (frame >= 900 && frame < 906) ||
          (extra && frame >= 1300 && frame < 1306) ||
          (extra && frame >= 1700 && frame < 1706);
      if (autopulse) {
        if (frame == 600 || frame == 900 || frame == 1300 || frame == 1700)
          fprintf(stderr, "=== AUTOSTART A pulse @frame %lu ===\n", frame);
        mask |= FOX_A_MENU;
      }
    }
    /* 🗺️ WARP: no world map, força o stage-id selecionado = warp_stage e injeta confirm
       (FOX_A_MENU) numa janela -> carrega a fase N direto. */
    if (warp_stage >= 0 && !sonic_game_started) {
      if (wm_sel_id) *wm_sel_id = (int)warp_stage;
      /* confirm pra ENTRAR na fase = decide (A=0x20), NÃO FOX_A_MENU (0x8020 tem START
         0x8000 que pode pausar). Pulsos espaçados (borda) p/ disparar o decide 1x. */
      if ((frame >= 1150 && frame < 1154) || (frame >= 1180 && frame < 1184) ||
          (frame >= 1210 && frame < 1214)) {
        if (frame == 1150) fprintf(stderr, "=== WARP confirm stage=%ld (decide 0x20) @frame %lu ===\n", warp_stage, frame);
        mask |= FOX_A_GAME;
      }
    }
    /* 🔎 DIAG Y=Left: SONIC_TESTBIT=0xNNNN injeta esse bit FOX no mask em pulsos
       de 4 frames a cada 60, a partir de SONIC_TESTBIT_AT (default 1500). Determinístico
       (sem uinput). Ex.: TESTBIT=0x0010 (FOX_Y novo) vs 0x0100 (L1, o Y antigo errado). */
    {
      static long testbit = -2; static long testat = 0;
      if (testbit == -2) { const char *e = getenv("SONIC_TESTBIT");
        testbit = e ? strtol(e, NULL, 0) : -1;
        const char *a = getenv("SONIC_TESTBIT_AT"); testat = a ? atol(a) : 1500; }
      if (testbit > 0 && (long)frame >= testat) {
        long ph = ((long)frame - testat) % 60;
        if (ph < 4) {
          if (ph == 0) fprintf(stderr, "=== TESTBIT 0x%04lx pulse @frame %lu ===\n", testbit, frame);
          mask |= (int)testbit;
        }
      }
    }
    if (sonic_game_started && autoright_after >= 0 && (long)frame >= autoright_after) {
      mask |= FOX_RIGHT;
      lx = 32767;
    }
    if (sonic_game_started && autojump_at >= 0 &&
        (long)frame >= autojump_at && (long)frame < autojump_at + 8)
      mask |= FOX_A_GAME;
    if (sonic_game_started && start_down && !start_was_down) {
      fprintf(stderr, "=== START native pause key @frame %lu ===\n", frame);
    }
    start_was_down = start_down;
    if (sonic_game_started && autopause_at >= 0 &&
        (long)frame >= autopause_at && (long)frame < autopause_at + 6) {
      if (autopause_state == 0)
        fprintf(stderr, "=== AUTOPAUSE native pause key @frame %lu ===\n", frame);
      mask |= FOX_PAUSE;
      autopause_state = 1;
    }
    /* (O hack antigo de suprimir Y/X fora do gameplay foi REMOVIDO: a causa real era o
       bit errado do Y; agora FOX_Y=0x10 (mapa real s_remapKey) -> Y dispara sua função
       própria e não aliasa mais pra L1/left.) */
    if (fox.SetPadData) fox.SetPadData(env, thiz, mask, 0, 0, 0, 0, 0);
    if (gm_direct) *gm_direct = (short)mask;
    if (gm_lx) *gm_lx = (short)lx;
    if (gm_ly) *gm_ly = (short)ly;
    if (getenv("SONIC_INPUTLOG") && mask != prev_mask)
      fprintf(stderr, "[input-change f%lu] started=%d mask=%04x prev=%04x\n",
              frame, sonic_game_started, mask & 0xffff, prev_mask & 0xffff);
    prev_mask = mask;
    if (getenv("SONIC_INPUTLOG") && sonic_game_started && (frame % 60) == 0)
      fprintf(stderr, "[input f%lu] mask=%04x lx=%d ly=%d rx=%d ry=%d lt=%d rt=%d\n",
              frame, mask & 0xffff, lx, ly, rx, ry, lt, rt);
    /* 🔎 SONIC_KEYDUMP: dump dos bits de tecla de menu REAIS em runtime (a cada ~2s)
       p/ confirmar/descartar remap do keymap (bug Y=Left). */
    if (getenv("SONIC_KEYDUMP") && (frame - keydump_last) >= 120) {
      keydump_last = frame;
      fprintf(stderr, "[keydump f%lu] L=%04x R=%04x U=%04x D=%04x decide=%04x cancel=%04x  (FOX_Y=0010 FOX_LEFT=0004)\n",
              frame,
              gk_left   ? (*gk_left   & 0xffff) : 0xdead,
              gk_right  ? (*gk_right  & 0xffff) : 0xdead,
              gk_up     ? (*gk_up     & 0xffff) : 0xdead,
              gk_down   ? (*gk_down   & 0xffff) : 0xdead,
              gk_decide ? (*gk_decide & 0xffff) : 0xdead,
              gk_cancel ? (*gk_cancel & 0xffff) : 0xdead);
    }

    sonic_native_save_load_poll("frame");
    sonic_save_bootstrap_poll(frame);
    /* dirige o continue do título (save com fase interrompida) — só fora do gameplay;
       a flag é lida exclusivamente por CStateWaitViewPausing::Next, então forçar
       o valor quando está 0 é seguro e usa o fluxo nativo. */
    if (!sonic_continue_disabled && !sonic_game_started &&
        sonic_isContinueStart && sonic_SetContinueStart &&
        sonic_isContinueStart() == 0) {
      sonic_SetContinueStart(sonic_continue_mode);
      if (sonic_continue_log_n < 3) {
        fprintf(stderr, "=== continue-drive: SetContinueStart(%d) @frame %lu ===\n",
                sonic_continue_mode, frame);
        sonic_continue_log_n++;
      }
    }
    sonic_in_draw_frame = 0;
    if (fox.GameProcess) fox.GameProcess(env, thiz);
    if (sonic_game_started && !gameplay_start_delay_done) {
      const char *d = getenv("SONIC_GAMEPLAY_START_DELAY_MS");
      int ms = d ? atoi(d) : 0;
      gameplay_start_delay_done = 1;
      if (ms > 0) {
        fprintf(stderr, "=== gameplay start delay %d ms @frame %lu ===\n", ms, frame);
        usleep((useconds_t)ms * 1000);
      }
    }
    sonic_in_draw_frame = 1;
    /* 🔧 FIX RASTROS (confirmado por FBO-STATS): o FBO 0 (tela final) é desenhado mas
       NUNCA limpo pelo engine (só FBO 2 é) -> a composição acumula -> smear. Ligamos
       o FBO 0 EXPLICITAMENTE e limpamos antes do DrawFrame (o engine recompõe a cena
       fresca por cima). O clear antigo (sem bind) pegava o FBO errado e não resolvia. */
    if (g_fbclear) {
      extern void glBindFramebuffer(unsigned int, unsigned int);
      extern void glClear(unsigned int);
      glBindFramebuffer(0x8D40 /* GL_FRAMEBUFFER */, 0); /* FBO 0 = tela */
      glClear(0x4100 /* COLOR | DEPTH */);
    }
    if (fox.DrawFrame)   fox.DrawFrame(env, thiz);
    sonic_in_draw_frame = 0;
    if (getenv("SONIC_TESTCLEAR")) {  /* diagnóstico: present/contexto OK? */
      extern void glClearColor(float, float, float, float);
      extern void glClear(unsigned int);
      glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
      glClear(0x4000 /* GL_COLOR_BUFFER_BIT */);
    }
    /* PIXDIAG: localizar onde está o conteúdo (FB0 vs FBO interno). Lê o pixel
       central do framebuffer default (0) e de alguns FBOs antes do present. */
    if (getenv("SONIC_PIXDIAG") && (frame % 60) == 0 && frame > 120) {
      extern void glBindFramebuffer(unsigned, unsigned);
      extern void glReadPixels(int,int,int,int,unsigned,unsigned,void*);
      unsigned char px[4]; int fb;
      for (fb = 0; fb <= 3; fb++) {
        glBindFramebuffer(0x8D40 /*GL_FRAMEBUFFER*/, (unsigned)fb);
        px[0]=px[1]=px[2]=px[3]=0;
        glReadPixels(640, 360, 1, 1, 0x1908 /*GL_RGBA*/, 0x1401 /*UBYTE*/, px);
        fprintf(stderr, "[PIXDIAG f%lu] FB%d center=%02x %02x %02x %02x\n",
                frame, fb, px[0], px[1], px[2], px[3]);
      }
      glBindFramebuffer(0x8D40, 0);
    }
    egl_shim_present();
    /* sinaliza intro-video done nos primeiros segundos */
    if (introCB && frame >= 30 && frame < 120 && (frame % 15) == 0) introCB(env, thiz);
    /* 🔑 dispara o callback do ad intersticial (sem ad Java) p/ destravar a transição
       Start->world map. callbackInterstitialAds(type=0, result=0) invoca o callback que
       showInterstitial ARMAZENOU. ⚠️ NÃO disparar a cada frame: callBackInterestitial NÃO
       limpa o callback após invocar -> re-disparo contínuo RE-INVOCA o "ir pro world map"
       sem parar -> reinicia a criação do world map (createFile/Tex/Mdl/Act) eternamente ->
       nunca completa -> tela azul. Disparo ÚNICO: o jni_shim seta jni_inter_pending no
       INSTANTE em que o engine chama o método Java showInterstitial(I)V (callback já
       armazenado) -> aqui disparamos callbackInterstitialAds 1x (simula ad fechado). */
    {
      extern volatile int jni_inter_pending;
      (void)prev_a; (void)inter_fire_at; (void)interat;
      if (interCB && jni_inter_pending) {
        jni_inter_pending = 0;
        if (sonic_game_started && !env_flag_enabled("SONIC_ALLOW_GAMEPLAY_INTERCB")) {
          if (inter_gameplay_ignored < 8) {
            fprintf(stderr, "=== interCB IGNORE @frame %lu (gameplay showInterstitial) ===\n", frame);
            inter_gameplay_ignored++;
          }
        } else if ((long)frame - inter_last_fire_frame < 30) {
          fprintf(stderr, "=== interCB IGNORE @frame %lu (debounce) ===\n", frame);
        } else {
          inter_last_fire_frame = (long)frame;
          fprintf(stderr, "=== interCB FIRE @frame %lu (showInterstitial->ad closed) ===\n", frame);
          interCB(env, thiz, 0, 0);
        }
      }
    }
    if ((frame % 60) == 0 && env_flag_enabled("SONIC_FRAMELOG"))
      fprintf(stderr, "[frame %lu]\n", frame);
    frame++;
    if (frame_sleep_us > 0) usleep((useconds_t)frame_sleep_us);
  }
  return 0;
}
