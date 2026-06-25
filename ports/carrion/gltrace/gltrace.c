#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <signal.h>
#include <execinfo.h>
#include <string.h>

static FILE* g_last = 0;
static unsigned long g_count = 0;

// Called by every wrapped GL function. Overwrites lastgl.txt with the call name.

static void segv_handler(int sig, siginfo_t* si, void* uc){
    char buf[256];
    int n = snprintf(buf, sizeof buf, "\n[SEGV] sig=%d tid=%ld addr=%p\n", sig, (long)syscall(SYS_gettid), si->si_addr);
    write(2, buf, n);
    void* bt[32]; int m = backtrace(bt, 32);
    backtrace_symbols_fd(bt, m, 2);
    _exit(139);
}
__attribute__((constructor)) static void install_segv(void){
    if(getenv("CARRION_SEGV")){
        struct sigaction sa; memset(&sa,0,sizeof sa);
        sa.sa_sigaction = segv_handler; sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, 0);
    }
}

void gltrace_log(const char* name, unsigned long* regs){
    g_count++;
    if(!g_last) g_last = fopen("/storage/roms/ports/carrion/lastgl.txt","a");
    if(g_last){
        if(!strcmp(name,"glTexImage2D")){
            fprintf(g_last, "[tid%ld] #%lu %s lvl=%ld ifmt=0x%lx w=%ld h=%ld fmt=0x%lx type=0x%lx data=%p\n",
                (long)syscall(SYS_gettid), g_count, name, (long)regs[1],(unsigned long)regs[2],(long)regs[3],(long)regs[4],(unsigned long)regs[6],(unsigned long)regs[7],(void*)regs[8]);
        } else if(!strcmp(name,"glTexParameteri")){
            fprintf(g_last, "#%lu %s pname=0x%lx param=0x%lx\n", g_count, name, (unsigned long)regs[1],(unsigned long)regs[2]);
        } else {
            fprintf(g_last, "[tid%ld] #%lu %s\n", (long)syscall(SYS_gettid), g_count, name);
        }
        fflush(g_last);
    }
}

// Trampoline template: saves arg regs, calls gltrace_log(name), restores, tail-calls realfn.
extern const unsigned char tramp_start[];
extern const unsigned char tramp_end[];
extern const unsigned char tramp_data[];
__asm__(
".pushsection .text\n"
".globl tramp_start\n.globl tramp_end\n.globl tramp_data\n.hidden tramp_start\n.hidden tramp_end\n.hidden tramp_data\n"
".align 4\n"
"tramp_start:\n"
"   stp x29, x30, [sp, #-160]!\n"
"   stp x0, x1, [sp, #16]\n"
"   stp x2, x3, [sp, #32]\n"
"   stp x4, x5, [sp, #48]\n"
"   stp x6, x7, [sp, #64]\n"
"   str x8, [sp, #80]\n"
"   stp d0, d1, [sp, #88]\n"
"   stp d2, d3, [sp, #104]\n"
"   stp d4, d5, [sp, #120]\n"
"   stp d6, d7, [sp, #136]\n"
"   adr x9, tramp_data\n"
"   ldr x0, [x9]\n"
"   add x1, sp, #16\n"
"   ldr x10, [x9, #16]\n"
"   blr x10\n"
"   ldp d6, d7, [sp, #136]\n"
"   ldp d4, d5, [sp, #120]\n"
"   ldp d2, d3, [sp, #104]\n"
"   ldp d0, d1, [sp, #88]\n"
"   ldr x8, [sp, #80]\n"
"   ldp x6, x7, [sp, #64]\n"
"   ldp x4, x5, [sp, #48]\n"
"   ldp x2, x3, [sp, #32]\n"
"   ldp x0, x1, [sp, #16]\n"
"   adr x9, tramp_data\n"
"   ldr x16, [x9, #8]\n"
"   ldp x29, x30, [sp], #160\n"
"   br x16\n"
".align 4\n"
"tramp_data:\n"
"   .quad 0\n   .quad 0\n   .quad 0\n"
"tramp_end:\n"
".popsection\n"
);

void* gltrace_wrap(const char* name, void* realfn){
    if(!realfn) return realfn;
    size_t clen = (size_t)(tramp_end - tramp_start);
    void* page = mmap(0, clen, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if(page==MAP_FAILED) return realfn;
    memcpy(page, tramp_start, clen);
    uint64_t* data = (uint64_t*)((char*)page + (size_t)(tramp_data - tramp_start));
    data[0] = (uint64_t)(uintptr_t)strdup(name);
    data[1] = (uint64_t)(uintptr_t)realfn;
    data[2] = (uint64_t)(uintptr_t)&gltrace_log;
    __builtin___clear_cache((char*)page, (char*)page+clen);
    return page;
}
