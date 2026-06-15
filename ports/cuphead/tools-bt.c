/* backtrace por ptrace (frame-pointer x29/x30) de uma thread aarch64 travada */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <elf.h>
#include <unistd.h>
struct regs { unsigned long long x[31], sp, pc, pstate; };
static unsigned long pk(int t, unsigned long a){ errno=0; return (unsigned long)ptrace(PTRACE_PEEKDATA,t,(void*)a,0); }
int main(int c, char**v){
  if(c<2){fprintf(stderr,"uso: bt <tid>\n");return 1;}
  int t=atoi(v[1]);
  if(ptrace(PTRACE_SEIZE,t,0,0)<0){perror("seize");return 1;}
  if(ptrace(PTRACE_INTERRUPT,t,0,0)<0){perror("interrupt");return 1;}
  int st; waitpid(t,&st,__WALL);
  struct regs r; struct iovec io={&r,sizeof r};
  if(ptrace(PTRACE_GETREGSET,t,(void*)NT_PRSTATUS,&io)<0){perror("getregs");ptrace(PTRACE_DETACH,t,0,0);return 1;}
  printf("PC=0x%llx LR=0x%llx SP=0x%llx FP=0x%llx\n", r.pc, r.x[30], r.sp, r.x[29]);
  printf("x0=0x%llx x1=0x%llx x2=0x%llx x8=0x%llx x19=0x%llx x20=0x%llx\n", r.x[0],r.x[1],r.x[2],r.x[8],r.x[19],r.x[20]);
  printf("--- bt (fp-chain) ---\n");
  printf("[00] 0x%llx (pc)\n", r.pc);
  printf("[01] 0x%llx (lr)\n", r.x[30]);
  unsigned long fp=r.x[29];
  for(int i=2;i<60 && fp;i++){
    unsigned long nfp=pk(t,fp), lr=pk(t,fp+8);
    if(errno) break;
    printf("[%02d] 0x%lx\n", i, lr);
    if(nfp<=fp) break; fp=nfp;
  }
  ptrace(PTRACE_DETACH,t,0,0);
  return 0;
}
