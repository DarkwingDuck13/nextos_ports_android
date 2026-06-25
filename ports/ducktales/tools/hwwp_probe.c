/* Probe ARM HW watchpoint support via ptrace on this device.
   fork a trivial child, PTRACE_GETHBPREGS(reg 0) returns the debug arch info:
   low byte = debug arch version, byte[1] = #breakpoints, byte[2] = #watchpoints. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <errno.h>
#include <string.h>

#ifndef PTRACE_GETHBPREGS
#define PTRACE_GETHBPREGS 29
#define PTRACE_SETHBPREGS 30
#endif

int main(void) {
  pid_t c = fork();
  if (c == 0) {
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    raise(SIGSTOP);
    _exit(0);
  }
  int st; waitpid(c, &st, 0);
  errno = 0;
  long info = ptrace(PTRACE_GETHBPREGS, c, 0, 0);
  if (errno) {
    fprintf(stderr, "PTRACE_GETHBPREGS reg0 failed: %s (errno=%d)\n", strerror(errno), errno);
  } else {
    unsigned v = (unsigned)info;
    fprintf(stderr, "HBPREGS info=0x%08x  debug_arch=%u  #brp=%u  #wrp=%u\n",
            v, v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff);
  }
  /* try to actually SET a watchpoint reg: addr reg = -1 (reg index 1), ctrl reg = -2 */
  unsigned long addr = 0x10000000;
  errno = 0;
  long r = ptrace(PTRACE_SETHBPREGS, c, (void*)-1L, &addr);  /* wrp addr reg #1 */
  fprintf(stderr, "SETHBPREGS wrp-addr#1 ret=%ld errno=%d (%s)\n", r, errno, errno?strerror(errno):"ok");
  unsigned long ctrl = (0xffUL << 5) | (0x7UL << 1) | 1;  /* byte mask, priv, enable */
  errno = 0;
  r = ptrace(PTRACE_SETHBPREGS, c, (void*)-2L, &ctrl);
  fprintf(stderr, "SETHBPREGS wrp-ctrl#1 ret=%ld errno=%d (%s)\n", r, errno, errno?strerror(errno):"ok");
  ptrace(PTRACE_KILL, c, 0, 0);
  waitpid(c, &st, 0);
  return 0;
}
