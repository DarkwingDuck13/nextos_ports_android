/* CP15 barrier trap-and-emulate for R36S (RK3326, ARMv8 aarch32, CP15BEN=0).
 *
 * The original Sonic 4 EP1 Marmalade engine (libs3e_android.so) was compiled
 * for old ARM and emits deprecated CP15 data/instruction barriers:
 *     mcr p15,0,Rt,c7,c10,5  (DMB)
 *     mcr p15,0,Rt,c7,c10,4  (DSB)
 *     mcr p15,0,Rt,c7,c5,4   (ISB)
 * On the Amlogic kernel these are enabled (CP15BEN=1) and run fine, which is
 * why the proven Mali-450 build works. The RK3326/ArchR kernel leaves CP15BEN=0
 * so the same instruction faults with SIGILL.
 *
 * LD_PRELOAD this .so: a SIGILL handler decodes the faulting CP15 barrier and
 * executes the real ARMv8 equivalent (dmb/dsb/isb), then advances PC past it.
 * Genuine illegal instructions are forwarded to the game's own crash handler.
 *
 * The game installs its own SIGILL handler in main(); we interpose sigaction()
 * (and signal()) so SIGILL stays ours while we remember the game's handler.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <ucontext.h>
#include <pthread.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/syscall.h>

#ifndef __NR_rt_sigaction
#define __NR_rt_sigaction 174   /* ARM EABI */
#endif

typedef void (*sa_sigaction_t)(int, siginfo_t *, void *);

static sa_sigaction_t g_app_sigaction = 0;   /* game's SIGILL handler (SA_SIGINFO) */
static void          (*g_app_handler)(int) = 0; /* game's SIGILL handler (legacy) */
static struct sigaction g_saved_old;
static int g_have_old = 0;
static unsigned long g_emulated = 0;

static int (*real_sigaction)(int, const struct sigaction *, struct sigaction *) = 0;
static void (*(*real_signal)(int, void (*)(int)))(int) = 0;

static void resolve(void)
{
    if (!real_sigaction)
        real_sigaction = dlsym(RTLD_NEXT, "sigaction");
}

static void sigill_handler(int sig, siginfo_t *si, void *uc_void)
{
    ucontext_t *uc = (ucontext_t *)uc_void;
    unsigned long pc = uc->uc_mcontext.arm_pc;
    unsigned int insn = *(volatile unsigned int *)pc;
    unsigned int m = insn & 0xfff0ffffu;      /* mask out Rt (bits 12-15) */

    if (m == 0xee070fbau) {                    /* DMB: mcr p15,0,Rt,c7,c10,5 */
        __asm__ __volatile__("dmb ish" ::: "memory");
        uc->uc_mcontext.arm_pc = pc + 4;
        if (g_emulated < 3) fprintf(stderr, "cp15emul: emulated DMB @%lx (#%lu)\n", pc, g_emulated);
        g_emulated++;
        return;
    }
    if (m == 0xee070f9au) {                    /* DSB: mcr p15,0,Rt,c7,c10,4 */
        __asm__ __volatile__("dsb ish" ::: "memory");
        uc->uc_mcontext.arm_pc = pc + 4;
        g_emulated++;
        return;
    }
    if (m == 0xee070f94u) {                    /* ISB: mcr p15,0,Rt,c7,c5,4 */
        __asm__ __volatile__("isb" ::: "memory");
        uc->uc_mcontext.arm_pc = pc + 4;
        g_emulated++;
        return;
    }

    /* Not a CP15 barrier -> a real illegal instruction. Hand to the game. */
    if (g_app_sigaction) { g_app_sigaction(sig, si, uc_void); return; }
    if (g_app_handler)   { g_app_handler(sig); return; }
    signal(SIGILL, SIG_DFL);
    raise(SIGILL);
}

static void install_ours(void)
{
    struct sigaction sa;
    resolve();
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigill_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (real_sigaction)
        real_sigaction(SIGILL, &sa, &g_saved_old);
}

/* The game installs its own SIGILL handler via a raw rt_sigaction syscall
 * (it does not import sigaction dynamically, so interposing it is not enough).
 * Continuously re-claim SIGILL so our emulator owns it by the time the engine
 * executes its first CP15 barrier (during runNative, seconds after startup). */
static unsigned long g_reclaims = 0;
static unsigned long g_ticks = 0;
static void *reassert_thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct sigaction cur;
        resolve();
        if (real_sigaction && real_sigaction(SIGILL, 0, &cur) == 0) {
            if (cur.sa_sigaction != sigill_handler) {
                if (cur.sa_flags & SA_SIGINFO) g_app_sigaction = cur.sa_sigaction;
                else if ((void *)cur.sa_handler != (void *)SIG_DFL &&
                         (void *)cur.sa_handler != (void *)SIG_IGN)
                    g_app_handler = cur.sa_handler;
                install_ours();
                g_reclaims++;
                if (g_reclaims <= 8)
                    fprintf(stderr, "cp15emul: RECLAIMED SIGILL from %p (reclaim #%lu)\n",
                            (void *)cur.sa_sigaction, g_reclaims);
            }
        }
        if ((g_ticks++ % 5000) == 0)
            fprintf(stderr, "cp15emul: reassert alive tick=%lu reclaims=%lu emulated=%lu\n",
                    g_ticks, g_reclaims, g_emulated);
        usleep(200);
    }
    return 0;
}

__attribute__((constructor))
static void cp15emul_init(void)
{
    pthread_t t;
    install_ours();
    pthread_create(&t, 0, reassert_thread, 0);
    pthread_detach(t);
    fprintf(stderr, "cp15emul: SIGILL CP15 barrier emulator armed (reassert thread up)\n");
}

/* Interpose sigaction: keep SIGILL ours, remember the game's handler. */
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
    resolve();
    if (signum == SIGILL && act) {
        if (act->sa_flags & SA_SIGINFO) { g_app_sigaction = act->sa_sigaction; g_app_handler = 0; }
        else                            { g_app_handler = act->sa_handler;     g_app_sigaction = 0; }
        if (oldact) {
            if (g_have_old) *oldact = g_saved_old;
            else memset(oldact, 0, sizeof(*oldact));
        }
        /* re-assert our handler in case it was disturbed */
        install_ours();
        g_have_old = 1;
        return 0;
    }
    if (!real_sigaction) return -1;
    return real_sigaction(signum, act, oldact);
}

/* The good binary installs its SIGILL handler with a RAW syscall (it imports
 * syscall@GLIBC_2.4, not sigaction). Interpose syscall(): when it does
 * rt_sigaction on SIGILL, remember the binary's handler but DON'T let it take
 * SIGILL from us — keep our emulator installed. Robust, no race. */
static long (*real_syscall)(long, ...) = 0;

long syscall(long number, ...)
{
    va_list ap;
    va_start(ap, number);
    long a1 = va_arg(ap, long), a2 = va_arg(ap, long), a3 = va_arg(ap, long);
    long a4 = va_arg(ap, long), a5 = va_arg(ap, long), a6 = va_arg(ap, long);
    va_end(ap);

    if (number == __NR_rt_sigaction) {
        static int n; if (n < 12) { fprintf(stderr, "cp15emul: syscall rt_sigaction signum=%ld (intercept=%d)\n", a1, a1==SIGILL); n++; }
    }
    if (number == __NR_rt_sigaction && a1 == SIGILL) {
        /* a2 = act (raw kernel_sigaction), a3 = oldact, a4 = sigsetsize.
         * raw layout (ARM): [0]=handler, [1]=flags, [2]=restorer, then mask. */
        if (a2) {
            unsigned long *act = (unsigned long *)a2;
            unsigned long h = act[0], fl = act[1];
            if (h && h != (unsigned long)SIG_DFL && h != (unsigned long)SIG_IGN) {
                if (fl & SA_SIGINFO) { g_app_sigaction = (sa_sigaction_t)h; g_app_handler = 0; }
                else                 { g_app_handler = (void (*)(int))h;   g_app_sigaction = 0; }
            }
        }
        install_ours();                 /* re-assert OUR handler */
        if (a3) {                       /* report ours as the old disposition */
            unsigned long *old = (unsigned long *)a3;
            old[0] = (unsigned long)sigill_handler;
            old[1] = SA_SIGINFO | SA_RESTART;
        }
        return 0;                       /* binary thinks it succeeded */
    }

    if (!real_syscall) real_syscall = dlsym(RTLD_NEXT, "syscall");
    return real_syscall(number, a1, a2, a3, a4, a5, a6);
}

/* Interpose signal(): swallow SIGILL, pass the rest through. */
void (*signal(int signum, void (*handler)(int)))(int)
{
    if (signum == SIGILL) {
        g_app_handler = handler;
        g_app_sigaction = 0;
        install_ours();
        return SIG_DFL;
    }
    if (!real_signal)
        real_signal = dlsym(RTLD_NEXT, "signal");
    if (real_signal)
        return real_signal(signum, handler);
    return SIG_ERR;
}
