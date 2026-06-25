// LD_PRELOAD: impede SDL/libmali/wayland de roubar os sinais de ativação do GC do CoreCLR.
// O CoreCLR instala handler em SIGRTMIN..+N p/ suspensão de thread do GC; se outra lib
// sobrescreve, o GC quebra -> crash nativo no loading. Gated por CARRION_SIGPROTECT=1.
#define _GNU_SOURCE
#include <signal.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
static int (*real_sigaction)(int, const struct sigaction*, struct sigaction*) = 0;
static int prot = -1;
int sigaction(int sig, const struct sigaction* act, struct sigaction* old){
    if(!real_sigaction) real_sigaction = dlsym(RTLD_NEXT, "sigaction");
    if(prot<0) prot = getenv("CARRION_SIGPROTECT") ? 1 : 0;
    if(prot && act && sig >= SIGRTMIN && sig <= SIGRTMIN+6){
        struct sigaction cur; 
        if(real_sigaction(sig, 0, &cur)==0 && cur.sa_handler!=SIG_DFL && cur.sa_handler!=SIG_IGN){
            // ja tem handler (do CoreCLR, instalado antes) -> nao deixa sobrescrever
            fprintf(stderr,"[sigprotect] bloqueado override de RT signal %d (CoreCLR protegido)\n", sig-SIGRTMIN);
            if(old) *old = cur;
            return 0;
        }
    }
    return real_sigaction(sig, act, old);
}
