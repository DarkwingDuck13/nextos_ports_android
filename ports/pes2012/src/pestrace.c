/* pestrace — wrapper ptrace p/ PES 2012: intercepta as syscalls statfs64/
 * fstatfs64 (o exec lê o free-space com struct-layout errado -> "Not enough
 * space" mesmo com GBs livres) e reescreve o struct de saída p/ ~1GB livre com
 * bsize 4096 em TODOS os offsets plausíveis (statfs 32-bit E statfs64). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <elf.h>

/* ARM EABI syscall numbers */
#define SC_statfs 99
#define SC_fstatfs 100
#define SC_statfs64 266
#define SC_fstatfs64 267

/* mais syscalls p/ rastrear onde o jogo procura OBB / mede espaço */
#define SC_open 5
#define SC_access 33
#define SC_openat 322
#define SC_stat64 195
#define SC_lstat64 196
#define SC_unlink 10
#define SC_unlinkat 328
#define SC_rename 38
#define SC_renameat 329

/* ARM: uregs[0..17]; r0=0, r2=2, r7=7, pc=15 */
struct arm_regs { unsigned long uregs[18]; };

/* lê string do tracee em `addr` */
static void peek_str(pid_t pid, unsigned long addr, char *out, int max) {
  int i = 0;
  while (i < max - 4) {
    long w = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + i), 0);
    if (w == -1) break;
    for (int b = 0; b < 4 && i < max - 1; b++, i++) {
      char c = (w >> (b * 8)) & 0xff;
      out[i] = c;
      if (!c) return;
    }
  }
  out[i] = 0;
}

static void poke_word(pid_t pid, unsigned long addr, unsigned long val) {
  ptrace(PTRACE_POKEDATA, pid, (void *)addr, (void *)val);
}

/* reescreve o buffer statfs em `buf`: bsize=4096, ~1GB livre em todos offsets */
static void log_statfs(pid_t pid, unsigned long buf) {
  static int done = 0;
  if (done++ > 1)
    return;
  fprintf(stderr, "[pestrace] ORIG struct @%lx:", buf);
  for (int o = 0; o <= 64; o += 4) {
    long w = ptrace(PTRACE_PEEKDATA, pid, (void *)(buf + o), 0);
    fprintf(stderr, " [%d]=%lu", o, (unsigned long)(w & 0xffffffff));
  }
  fprintf(stderr, "\n");
}

static void fix_statfs(pid_t pid, unsigned long buf, int is64) {
  /* TESTE: bsize=1, free=1GB "em bytes". Cobre o caso do jogo ler bavail SEM
   * multiplicar por bsize (bavail-como-bytes) E o caso bavail*bsize (=1GB, sem
   * overflow pois bsize=1). GB = 0x40000000 = 1073741824 < 2^31. */
  const unsigned long BIG = 0x7F000000UL; /* ~2.1GB, < 2^31 */
  poke_word(pid, buf + 4, 1);         /* f_bsize = 1 */
  poke_word(pid, buf + 8, BIG);       /* f_blocks total */
  poke_word(pid, buf + 12, 0);
  poke_word(pid, buf + 16, BIG);      /* f_bfree ~2GB */
  poke_word(pid, buf + 20, 0);
  poke_word(pid, buf + 24, BIG);      /* f_bavail ~2GB */
  poke_word(pid, buf + 28, 0);
  poke_word(pid, buf + 32, BIG);
  poke_word(pid, buf + 36, 0);
  poke_word(pid, buf + 40, BIG);
  poke_word(pid, buf + 44, 0);
  poke_word(pid, buf + 60, 1);        /* f_frsize = 1 */
  (void)is64;
  static int v = 0;
  if (v++ < 2) {
    long r24 = ptrace(PTRACE_PEEKDATA, pid, (void *)(buf + 24), 0);
    long r16 = ptrace(PTRACE_PEEKDATA, pid, (void *)(buf + 16), 0);
    fprintf(stderr, "[pestrace] após fix: [16]=%lu [24]=%lu\n",
            (unsigned long)(r16 & 0xffffffff), (unsigned long)(r24 & 0xffffffff));
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "uso: pestrace <prog> [args...]\n");
    return 1;
  }
  pid_t child = fork();
  if (child == 0) {
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    execv(argv[1], &argv[1]);
    perror("execv");
    _exit(127);
  }
  int status;
  waitpid(child, &status, 0);
  ptrace(PTRACE_SETOPTIONS, child, 0, (void *)PTRACE_O_TRACESYSGOOD);

  int in_syscall = 0;
  long sc = -1;
  unsigned long buf = 0;
  int is64 = 0;

  while (1) {
    if (ptrace(PTRACE_SYSCALL, child, 0, 0) < 0)
      break;
    if (waitpid(child, &status, 0) < 0)
      break;
    if (WIFEXITED(status) || WIFSIGNALED(status))
      break;
    if (!(WIFSTOPPED(status) && (WSTOPSIG(status) & 0x80)))
      continue; /* não é syscall-stop */

    struct arm_regs regs;
    struct iovec iov = { &regs, sizeof(regs) };
    if (ptrace(PTRACE_GETREGSET, child, (void *)NT_PRSTATUS, &iov) < 0) {
      /* fallback PTRACE_GETREGS */
      if (ptrace(PTRACE_GETREGS, child, 0, &regs) < 0)
        continue;
    }

    /* Sem toggle entry/exit (dessincroniza com sinais): conserta em TODO stop
     * com r7 de statfs. Na entrada o kernel sobrescreve depois; na saída o fix
     * fica. Fixar em ambos garante o valor final correto. */
    sc = regs.uregs[7];
    (void)in_syscall;
    static int verbose = 0;
    if (sc == SC_fstatfs64 || sc == SC_statfs64) {
      buf = regs.uregs[2];
      is64 = 1;
    } else if (sc == SC_fstatfs || sc == SC_statfs) {
      buf = regs.uregs[1];
      is64 = 0;
    } else {
      buf = 0;
    }
    if (buf) {
      log_statfs(child, buf);
      fix_statfs(child, buf, is64);
      if (verbose) {
        char pp[256] = "";
        if (sc == SC_statfs64 || sc == SC_statfs)
          peek_str(child, regs.uregs[0], pp, sizeof(pp)); /* path */
        long b24 = ptrace(PTRACE_PEEKDATA, child, (void *)(buf + 24), 0);
        long b16 = ptrace(PTRACE_PEEKDATA, child, (void *)(buf + 16), 0);
        long b4 = ptrace(PTRACE_PEEKDATA, child, (void *)(buf + 4), 0);
        fprintf(stderr, "[pestrace] VERB statfs sc=%ld path=\"%s\" bsize=%lu "
                "bfree=%lu bavail=%lu\n", sc, pp,
                (unsigned long)(b4 & 0xffffffff),
                (unsigned long)(b16 & 0xffffffff),
                (unsigned long)(b24 & 0xffffffff));
      }
    }

    /* rastreia paths de abertura/checagem (só na ENTRADA: pega o toggle par).
     * Usa um contador por-syscall p/ não duplicar entry/exit. */
    if (sc == SC_unlink || sc == SC_unlinkat || sc == SC_rename ||
        sc == SC_renameat) {
      int pri = (sc == SC_unlinkat || sc == SC_renameat) ? 1 : 0;
      char p[256];
      peek_str(child, regs.uregs[pri], p, sizeof(p));
      /* NEUTRALIZA o unlink de package.dz: o jogo deleta a cópia (rejeita) e
       * depois, no estado 10, tenta MONTÁ-la -> ENOENT -> crash. Trocamos o
       * syscall por getpid (nr 20) só na ENTRADA -> não deleta. */
      if (strstr(p, "package.dz") &&
          (sc == SC_unlink || sc == SC_unlinkat)) {
        /* ARM: p/ trocar o syscall usa-se PTRACE_SET_SYSCALL(23), não r7. */
        ptrace(23, child, 0, (void *)20); /* -> getpid (no-op) */
        fprintf(stderr, "[pestrace] BLOQUEADO unlink \"%s\"\n", p);
      } else
        fprintf(stderr, "[pestrace] DEL/REN sc=%ld \"%s\"\n", sc, p);
    }
    if (verbose) {
      static int vc = 0;
      if (vc++ < 400)
        fprintf(stderr, "[pestrace] SC %ld  r0=%ld r1=%lx r2=%lx\n", sc,
                (long)regs.uregs[0], regs.uregs[1], regs.uregs[2]);
      /* write(1|2, buf, n): mostra o texto (debug com o valor de espaço) */
      if (sc == 4 && (regs.uregs[0] == 1 || regs.uregs[0] == 2)) {
        char w[256];
        peek_str(child, regs.uregs[1], w, sizeof(w));
        fprintf(stderr, "[pestrace] WRITE fd=%ld: %.200s\n", (long)regs.uregs[0], w);
      }
    }
    if (sc == SC_open || sc == SC_access || sc == SC_stat64 ||
        sc == SC_lstat64 || sc == SC_openat) {
      int pri = (sc == SC_openat) ? 1 : 0;
      unsigned long pathreg = regs.uregs[pri];
      char p[256];
      peek_str(child, pathreg, p, sizeof(p));
      /* Scheme Marmalade "raw://" chega LITERAL ao kernel -> ENOENT. Reaponta o
       * ponteiro do path p/ +6 ("raw://" = 6 chars), virando path absoluto real.
       * Faz na ENTRADA (antes do syscall). Idempotente no exit. */
      if (p[0]=='r'&&p[1]=='a'&&p[2]=='w'&&p[3]==':'&&p[4]=='/'&&p[5]=='/') {
        regs.uregs[pri] = pathreg + 6;
        struct iovec wv = { &regs, sizeof(regs) };
        if (ptrace(PTRACE_SETREGSET, child, (void *)NT_PRSTATUS, &wv) < 0)
          ptrace(PTRACE_SETREGS, child, 0, &regs);
        fprintf(stderr, "[pestrace] raw:// -> \"%s\"\n", p + 6);
      } else if (p[0] && !((p[0]=='/' && (p[1]=='u'||p[1]=='l')))) {
        const char *nm = sc == SC_open ? "open" : sc == SC_access ? "access"
                         : sc == SC_openat ? "openat" : "stat";
        /* p/ package.dz mostra r0 (no exit = fd/-errno) */
        if (p[0] && (p[strlen(p)-1]=='z'))
          fprintf(stderr, "[pestrace] %s(\"%s\") r0=%ld\n", nm, p,
                  (long)regs.uregs[0]);
        else
          fprintf(stderr, "[pestrace] %s(\"%s\")\n", nm, p);
      }
    }
  }
  return 0;
}
