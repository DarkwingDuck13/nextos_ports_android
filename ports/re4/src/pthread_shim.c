/*
 * pthread_shim.c — tradução de primitivas de sincronização BIONIC <-> GLIBC.
 *
 * O il2cpp/unity (compilados p/ bionic) tem pthread_mutex_t/cond_t/sem_t/rwlock_t
 * com layout e TAMANHO diferentes do glibc. Chamar as funcoes do glibc nessas
 * structs bionic causa misalignment/corrupcao (SIGBUS) OU semaforo/cond que nunca
 * acorda (job system trava: todas as threads em futex_wait).
 *
 * Solucao: para cada ponteiro bionic alocamos a struct glibc REAL (lazy, no 1o uso)
 * num mapa bionic->glibc. As funcoes operam na glibc. Cobre os *_INITIALIZER
 * estaticos (bionic = zeros -> alocamos no 1o uso).
 *
 * Mutexes sao criados RECURSIVOS (evita self-deadlock quando o Unity usa
 * PTHREAD_MUTEX_RECURSIVE via attr, que ignoramos).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>

#define PMAP_SZ 32768
static struct { void * volatile bionic; void * volatile glibc; } g_pmap[PMAP_SZ];
static pthread_mutex_t g_pmap_lock = PTHREAD_MUTEX_INITIALIZER;

enum { K_MUTEX, K_COND, K_SEM, K_RWLOCK };

/* pmap_get e chamado de SIGNAL HANDLERS (o GC do Mono faz sem_post no handler de suspend da
   stop-the-world). pthread_mutex NAO e async-signal-safe -> se a thread interrompida segurava o
   g_pmap_lock, o handler deadlocka -> GC trava (todas as threads esperam em GC_end_blocking).
   FIX: LOOKUP lock-free (mapa append-only; glibc escrito ANTES de bionic c/ barreira -> reader que
   ve bionic==alvo tb ve glibc valido). So o INSERT trava. */
static void *pmap_get(void *bionic, int kind, unsigned sem_val) {
  if (!bionic) return NULL;
  uintptr_t h = ((uintptr_t)bionic >> 4) % PMAP_SZ;
  /* 1) lookup lock-free */
  for (int i = 0; i < PMAP_SZ; i++) {
    int idx = (int)((h + i) % PMAP_SZ);
    void *b = g_pmap[idx].bionic;
    if (b == bionic) { __sync_synchronize(); return g_pmap[idx].glibc; }
    if (b == NULL) break; /* fim da cadeia -> nao existe; vai inserir */
  }
  /* 2) insert (com lock) */
  pthread_mutex_lock(&g_pmap_lock);
  void *ret = NULL;
  for (int i = 0; i < PMAP_SZ; i++) {
    int idx = (int)((h + i) % PMAP_SZ);
    if (g_pmap[idx].bionic == bionic) { ret = g_pmap[idx].glibc; break; }
    if (g_pmap[idx].bionic == NULL) {
      void *g = NULL;
      if (kind == K_MUTEX) {
        g = calloc(1, sizeof(pthread_mutex_t));
        pthread_mutexattr_t a; pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        if (g) pthread_mutex_init((pthread_mutex_t *)g, &a);
        pthread_mutexattr_destroy(&a);
      } else if (kind == K_COND) {
        g = calloc(1, sizeof(pthread_cond_t));
        if (g) pthread_cond_init((pthread_cond_t *)g, NULL);
      } else if (kind == K_SEM) {
        g = calloc(1, sizeof(sem_t));
        if (g) sem_init((sem_t *)g, 0, sem_val);
      } else { /* K_RWLOCK */
        g = calloc(1, sizeof(pthread_rwlock_t));
        if (g) pthread_rwlock_init((pthread_rwlock_t *)g, NULL);
      }
      g_pmap[idx].glibc = g; __sync_synchronize(); /* glibc ANTES de bionic (barreira) */
      g_pmap[idx].bionic = bionic;
      ret = g;
      break;
    }
  }
  pthread_mutex_unlock(&g_pmap_lock);
  return ret;
}
/* true se o bionic->glibc ja existe no mapa (lookup lock-free, append-only) */
static int pmap_has(void *bionic) {
  if (!bionic) return 0;
  uintptr_t h = ((uintptr_t)bionic >> 4) % PMAP_SZ;
  for (int i = 0; i < PMAP_SZ; i++) {
    int idx = (int)((h + i) % PMAP_SZ);
    void *b = g_pmap[idx].bionic;
    if (b == bionic) return 1;
    if (b == NULL) return 0;
  }
  return 0;
}
static void *gmtx(void *b) { return pmap_get(b, K_MUTEX, 0); }
static void *gcnd(void *b) { return pmap_get(b, K_COND, 0); }
static void *grwl(void *b) { return pmap_get(b, K_RWLOCK, 0); }

/* ---- mutex ---- */
static int sh_mutex_init(void *m, const void *a) { (void)a; (void)gmtx(m); return 0; }
static int sh_mutex_lock(void *m) { return pthread_mutex_lock((pthread_mutex_t *)gmtx(m)); }
static int sh_mutex_unlock(void *m) { return pthread_mutex_unlock((pthread_mutex_t *)gmtx(m)); }
static int sh_mutex_trylock(void *m) { return pthread_mutex_trylock((pthread_mutex_t *)gmtx(m)); }
static int sh_mutex_destroy(void *m) { (void)m; return 0; }

/* ---- cond ---- */
static int sh_cond_init(void *c, const void *a) { (void)a; (void)gcnd(c); return 0; }
extern int g_gameplay, g_gameplay_frame, g_re4_frame; /* main_re4: gatear CONDBREAK so no gameplay carregado */
static int sh_cond_wait(void *c, void *m) {
  pthread_cond_t *cc=(pthread_cond_t *)gcnd(c); pthread_mutex_t *mm=(pthread_mutex_t *)gmtx(m);
  /* QUEBRA-DEADLOCK do job-system no MOVIMENTO: ao mover o Leon o Unity espera num pthread_cond
     que NAO e sinalizado (sinal perdido no ambiente stubado) -> futex_wait eterno (tela congela).
     SO aplicamos no GAMEPLAY JA CARREGADO (g_gameplay + apos o settle) -> NAO toca no load/menu
     (quebrar conds do load congelava). RE4_NO_CONDBREAK=1 desliga; RE4_CONDBREAK_MS ajusta. */
  int gp_ready = g_gameplay && g_re4_frame > g_gameplay_frame + 180;
  if(gp_ready && getenv("RE4_CONDBREAK")){
    const char *bms=getenv("RE4_CONDBREAK_MS");
    long total_ms=(bms&&bms[0])?atol(bms):120; if(total_ms<1)total_ms=120;
    const long step_ms=20; long waited=0; struct timespec ts;
    for(;;){
      clock_gettime(CLOCK_REALTIME,&ts);
      ts.tv_nsec += step_ms*1000000L; if(ts.tv_nsec>=1000000000L){ ts.tv_sec++; ts.tv_nsec-=1000000000L; }
      int r=pthread_cond_timedwait(cc,mm,&ts);
      if(r==0) return 0;                 /* sinalizado de verdade */
      if(r==ETIMEDOUT){ waited+=step_ms;
        if(waited>=total_ms){ static int n=0; if(n++<10) fprintf(stderr,"[CONDBREAK] wake c=%p apos %ldms\n",c,waited); return 0; }
        continue; }
      return r;
    }
  }
  return pthread_cond_wait(cc,mm);
}
static int sh_cond_timedwait(void *c, void *m, const void *ts) {
  return pthread_cond_timedwait((pthread_cond_t *)gcnd(c), (pthread_mutex_t *)gmtx(m),
                                (const struct timespec *)ts);
}
static int sh_cond_signal(void *c) { return pthread_cond_signal((pthread_cond_t *)gcnd(c)); }
static int sh_cond_broadcast(void *c) { return pthread_cond_broadcast((pthread_cond_t *)gcnd(c)); }
static int sh_cond_destroy(void *c) { (void)c; return 0; }

/* ---- semaforo (job system do Unity: bionic sem_t=4B vs glibc=32B) ---- */
static int sh_sem_init(void *s, int pshared, unsigned val) {
  (void)pshared;
  /* Na PRIMEIRA vez pmap_get cria o sem glibc ja com `val` (sem_init). Em chamadas seguintes no
     MESMO endereco NAO destruimos/reinicializamos por padrao: o Unity reinicializa os semaforos de
     COMPLETION do job-system (ex: unity+0xf0117dcc/dd4, ~2400x) a cada ciclo; se um worker postou a
     conclusao e nos destruimos+reinicializamos para 0, o POST e PERDIDO -> a main fica eternamente
     no sem_wait (deadlock que so o SEMBREAK quebrava, deixando a cena carregar incompleta).
     RE4_SEM_REINIT=1 restaura o comportamento antigo (destroi+reinit sempre). */
  int existed = pmap_has(s);
  sem_t *g = (sem_t *)pmap_get(s, K_SEM, val);
  if (g && existed) {
    /* POLITICA: so reinicializa se o sem esta DRENADO (valor<=0). Se valor>0 ha um POST
       pendente (ex: worker do job-system sinalizou conclusao) -> PRESERVA (reinit perderia o
       post -> a main travaria eternamente no sem_wait -> cena carrega incompleta). Drenado:
       reinit p/ `val` cobre o reuso de endereco (sem novo com valor inicial != 0). */
    int cur = -1; sem_getvalue(g, &cur);
    if (cur <= 0 && cur != (int)val) { sem_destroy(g); sem_init(g, 0, val); }
    if (getenv("RE4_SEM_REINIT")) { sem_destroy(g); sem_init(g, 0, val); }  /* legado/debug */
  }
  if(getenv("RE4_SEMLOG")){static int n=0;if(n++<5000)fprintf(stderr,"[SEM] init  b=%p val=%u existed=%d tid=%p\n",s,val,existed,(void*)pthread_self());}
  return 0;
}
static int sh_sem_wait(void *s) {
  sem_t *g=(sem_t *)pmap_get(s, K_SEM, 0);
  /* GAMEPLAY ASSENTADO -> espera REAL (sem SEMBREAK). O SEMBREAK (forca-acordar no timeout) e
     necessario no MENU/LOAD (1 semaforo global da libunity nunca e postado), mas no GAMEPLAY ele
     CORROMPE o job-system: forca os workers a acordar sem job real -> a conclusao que a UnityMain
     espera nunca chega -> a main gira em codigo JIT sem renderizar -> tela CONGELA (confirmado por
     gdb: GfxDevice+workers em sh_sem_wait, UnityMain em JIT). No gameplay o engine posta os
     semaforos de verdade, entao a espera real funciona. RE4_SEMBREAK_GP=1 forca o SEMBREAK no
     gameplay (debug). gp_ready: g_gameplay + ja assentado (nao toca no load, que precisa do break). */
  /* DIAG opt-in (RE4_SEMDIAG): no gameplay assentado, espera REAL com log do sem travado/caller.
     NAO e o caminho normal (SEMBREAK abaixo segue ligado p/ o gameplay limpar). */
  int gp_ready = g_gameplay && g_re4_frame > g_gameplay_frame + 180;
  if(gp_ready && getenv("RE4_SEMDIAG")){
    struct timespec ts; int waited=0;
    for(;;){
      clock_gettime(CLOCK_REALTIME,&ts); ts.tv_sec+=1;
      int r=sem_timedwait(g,&ts);
      if(r==0) return 0;
      if(errno==ETIMEDOUT){ waited++;
        if(waited==2||waited==5||(waited%10==0)){ int v=-1; sem_getvalue(g,&v);
          fprintf(stderr,"[GPSTUCK] b=%p glibc=%p val=%d waited=%ds tid=%p caller=%p\n",
            s,(void*)g,v,waited,(void*)pthread_self(),__builtin_return_address(0)); }
        continue; }
      return r;
    }
  }
  if(!getenv("RE4_NO_SEMBREAK")){
    /* timeout total p/ forcar wake. RE4_SEMBREAK_MS (ms) tem prioridade; senao RE4_SEMBREAK (s).
       Default = 150ms: o job-system do Unity (workers+main parados no mesmo wait, ninguem posta)
       progride rapido sem os 3s que deixavam o menu lento. Espera em passos de 25ms p/ pegar
       posts reais na hora. */
    const char *bms=getenv("RE4_SEMBREAK_MS"); const char *bv=getenv("RE4_SEMBREAK");
    long total_ms = (bms&&bms[0]) ? atol(bms) : (bv&&bv[0] ? atol(bv)*1000 : 150);
    if(total_ms<1) total_ms=150;
    const long step_ms=25; long waited_ms=0;
    struct timespec ts;
    for(;;){
      clock_gettime(CLOCK_REALTIME,&ts);
      ts.tv_nsec += step_ms*1000000L; if(ts.tv_nsec>=1000000000L){ ts.tv_sec++; ts.tv_nsec-=1000000000L; }
      int r=sem_timedwait(g,&ts);
      if(r==0) return 0;
      if(errno==ETIMEDOUT){
        waited_ms += step_ms;
        if(waited_ms>=total_ms){ static int n=0; if(n++<10) fprintf(stderr,"[SEMBREAK] forcando wake b=%p apos %ldms\n",s,waited_ms); return 0; }
        continue;
      }
      return r;
    }
  }
  if(getenv("RE4_SEMDIAG")){
    /* detecta a espera que NUNCA acorda (deadlock): timedwait em loop; loga addr+valor se travar */
    struct timespec ts; int waited=0;
    for(;;){
      clock_gettime(CLOCK_REALTIME,&ts); ts.tv_sec+=1;
      int r=sem_timedwait(g,&ts);
      if(r==0) return 0;
      if(errno==ETIMEDOUT){ waited++;
        if(waited==3||waited==8||(waited%20==0)){ int v=-1; sem_getvalue(g,&v);
          fprintf(stderr,"[SEMSTUCK] b=%p glibc=%p val=%d waited=%ds tid=%p\n",s,(void*)g,v,waited,(void*)pthread_self()); }
        continue; }
      return r; /* outro erro */
    }
  }
  return sem_wait(g); }
static int sh_sem_trywait(void *s) { return sem_trywait((sem_t *)pmap_get(s, K_SEM, 0)); }
static int sh_sem_post(void *s) {
  if(getenv("RE4_SEMLOG")){static int n=0;if(n++<8000)fprintf(stderr,"[SEM] post  b=%p tid=%p\n",s,(void*)pthread_self());}
  if(getenv("RE4_SEMDIAG") && g_gameplay && g_re4_frame > g_gameplay_frame + 180){
    static int n=0; if(n++<400) fprintf(stderr,"[GPPOST] b=%p tid=%p caller=%p f=%d\n",
      s,(void*)pthread_self(),__builtin_return_address(0),g_re4_frame);
  }
  return sem_post((sem_t *)pmap_get(s, K_SEM, 0)); }
static int sh_sem_timedwait(void *s, const void *ts) {
  return sem_timedwait((sem_t *)pmap_get(s, K_SEM, 0), (const struct timespec *)ts);
}
static int sh_sem_getvalue(void *s, int *v) { return sem_getvalue((sem_t *)pmap_get(s, K_SEM, 0), v); }
static int sh_sem_destroy(void *s) { (void)s; return 0; }

/* ---- rwlock ---- */
static int sh_rwl_init(void *l, const void *a) { (void)a; (void)grwl(l); return 0; }
static int sh_rwl_rdlock(void *l) { return pthread_rwlock_rdlock((pthread_rwlock_t *)grwl(l)); }
static int sh_rwl_wrlock(void *l) { return pthread_rwlock_wrlock((pthread_rwlock_t *)grwl(l)); }
static int sh_rwl_unlock(void *l) { return pthread_rwlock_unlock((pthread_rwlock_t *)grwl(l)); }
static int sh_rwl_destroy(void *l) { (void)l; return 0; }

/* ---- pthread_create: Unity passa pthread_attr_t BIONIC (layout != glibc).
   bionic attr: flags@0, stack_base@8, stack_size@16, guard_size@24, policy@32, prio@36.
   Traduzimos so o stack_size (resto = default glibc). ---- */
/* Wrapper: DESBLOQUEIA os sinais do Boehm GC (SIGPWR=30 suspend, SIGXCPU=24 restart) na thread
   nova. O GC tenta desbloquear via pthread_sigmask com sigset_t BIONIC -> glibc le/escreve 128B
   (ABI != bionic 8B) -> unblock falha -> SIGPWR fica BLOQUEADO -> a thread nao suspende no
   stop-the-world -> GC trava esperando ack. Fazemos o unblock com sigset glibc CORRETO aqui. */
struct sh_thunk { void *(*fn)(void *); void *arg; };
static void sh_unblock_gc_signals(void){
  sigset_t s; sigemptyset(&s);
  sigaddset(&s, 30); sigaddset(&s, 24);   /* SIGPWR, SIGXCPU (Boehm) */
  sigaddset(&s, SIGPWR); sigaddset(&s, SIGXCPU);
  pthread_sigmask(SIG_UNBLOCK, &s, NULL);
}
static void *sh_thread_start(void *p){ struct sh_thunk t = *(struct sh_thunk *)p; free(p);
  sh_unblock_gc_signals(); return t.fn(t.arg); }
static int sh_create(void *thr, const void *battr, void *(*fn)(void *), void *arg) {
  pthread_attr_t ga; pthread_attr_init(&ga);
  if (battr) {
    size_t ss = *(const size_t *)((const unsigned char *)battr + 8);
    if (ss >= 32768 && ss <= (256UL << 20)) pthread_attr_setstacksize(&ga, ss);
  }
  struct sh_thunk *th = (struct sh_thunk *)malloc(sizeof *th); th->fn = fn; th->arg = arg;
  fprintf(stderr,"[THREAD] sh_create fn=%p\n",fn); int r = pthread_create((pthread_t *)thr, &ga, sh_thread_start, th);
  pthread_attr_destroy(&ga);
  if (r) { fprintf(stderr, "[pthread] create FALHOU r=%d\n", r); free(th); }
  return r;
}
static int sh_attr_init(void *a) { if (a) memset(a, 0, 24); return 0; } /* bionic attr=24B (nao 56!) */
static int sh_attr_destroy(void *a) { (void)a; return 0; }
static int sh_attr_setstacksize(void *a, size_t s) {
  if (a) *(size_t *)((unsigned char *)a + 8) = s; return 0;  /* bionic 32-bit stack_size@8 */
}
static int sh_attr_setdetachstate(void *a, int s) {
  if (a) *(int *)((unsigned char *)a + 0) = s ? 1 : 0; return 0;
}
static int sh_ok0(void) { return 0; }

/* ---- pthread_getattr_np + pthread_attr_getstack: o GC (BDWGC) do il2cpp pega os
   limites da pilha de cada thread p/ escanear roots. Passar direto pro glibc deixa
   o GC (bionic) ler os campos no offset ERRADO -> bounds gigantes (59MB) -> GC
   varre lixo 0x0101.. fora da pilha -> deref -> SIGSEGV. Fix: pegamos os bounds
   REAIS do glibc e gravamos no attr bionic (base@8, size@16); getstack le de la. ---- */
static int sh_getattr_np(void *thread_, void *battr) {
  pthread_attr_t ga;
  int r = pthread_getattr_np((pthread_t)thread_, &ga);
  void *base = NULL; size_t size = 0;
  if (r == 0) pthread_attr_getstack(&ga, &base, &size);
  pthread_attr_destroy(&ga);
  if (battr) {
    memset(battr, 0, 24); /* bionic attr=24B; memset 56 ESTOURAVA o buffer do caller -> regs salvos zerados */
    /* bionic pthread_attr_t 32-bit (armeabi-v7a): flags@0, stack_base@4, stack_size@8 */
    *(void **)((char *)battr + 4) = base;
    *(size_t *)((char *)battr + 8) = size;
  }
  fprintf(stderr, "[STACK] getattr_np base=%p size=%zu\n", base, size);
  return r;
}
static int sh_attr_getstack(void *battr, void **base, size_t *size) {
  /* RAIZ do crash do GC: o attr foi preenchido pelo glibc pthread_getattr_np (layout glibc)
     mas aqui liamos offsets bionic -> base=0 -> GC aborta "Bad GET_MEM arg".
     Fix: ignora o attr e pega os bounds REAIS da thread atual via glibc (consistente). */
  (void)battr;
  void *b = NULL; size_t s = 0;
  pthread_attr_t ga;
  if (pthread_getattr_np(pthread_self(), &ga) == 0) {
    pthread_attr_getstack(&ga, &b, &s);
    pthread_attr_destroy(&ga);
  }
  if (base) *base = b;
  if (size) *size = s;
  fprintf(stderr, "[STACK] attr_getstack -> base=%p size=%zu\n", b, s);
  return 0;
}

void recon_wire_pthread(void (*set_import)(const char *, void *)) {
  set_import("pthread_mutex_init", (void *)sh_mutex_init);
  set_import("pthread_mutex_lock", (void *)sh_mutex_lock);
  set_import("pthread_mutex_unlock", (void *)sh_mutex_unlock);
  set_import("pthread_mutex_trylock", (void *)sh_mutex_trylock);
  set_import("pthread_mutex_destroy", (void *)sh_mutex_destroy);
  set_import("pthread_cond_init", (void *)sh_cond_init);
  set_import("pthread_cond_wait", (void *)sh_cond_wait);
  set_import("pthread_cond_timedwait", (void *)sh_cond_timedwait);
  set_import("pthread_cond_signal", (void *)sh_cond_signal);
  set_import("pthread_cond_broadcast", (void *)sh_cond_broadcast);
  set_import("pthread_cond_destroy", (void *)sh_cond_destroy);
  /* semaforos — CHAVE p/ destravar o job system */
  set_import("sem_init", (void *)sh_sem_init);
  set_import("sem_wait", (void *)sh_sem_wait);
  set_import("sem_trywait", (void *)sh_sem_trywait);
  set_import("sem_post", (void *)sh_sem_post);
  set_import("sem_timedwait", (void *)sh_sem_timedwait);
  set_import("sem_getvalue", (void *)sh_sem_getvalue);
  set_import("sem_destroy", (void *)sh_sem_destroy);
  /* rwlock */
  set_import("pthread_rwlock_init", (void *)sh_rwl_init);
  set_import("pthread_rwlock_rdlock", (void *)sh_rwl_rdlock);
  set_import("pthread_rwlock_wrlock", (void *)sh_rwl_wrlock);
  set_import("pthread_rwlock_unlock", (void *)sh_rwl_unlock);
  set_import("pthread_rwlock_destroy", (void *)sh_rwl_destroy);
  /* create + attrs */
  set_import("pthread_create", (void *)sh_create);
  set_import("pthread_attr_init", (void *)sh_attr_init);
  set_import("pthread_attr_destroy", (void *)sh_attr_destroy);
  set_import("pthread_attr_setstacksize", (void *)sh_attr_setstacksize);
  set_import("pthread_attr_setdetachstate", (void *)sh_attr_setdetachstate);
  set_import("pthread_attr_setschedparam", (void *)sh_ok0);
  set_import("pthread_attr_setschedpolicy", (void *)sh_ok0);
  set_import("pthread_attr_setguardsize", (void *)sh_ok0);
  /* stack bounds corretos p/ o GC (senao escaneia lixo -> crash) */
  set_import("pthread_getattr_np", (void *)sh_getattr_np);
  set_import("pthread_attr_getstack", (void *)sh_attr_getstack);
  /* attrs de mutex/cond: ignorados (mutex sempre recursivo, cond default) */
  set_import("pthread_mutexattr_init", (void *)sh_ok0);
  set_import("pthread_mutexattr_destroy", (void *)sh_ok0);
  set_import("pthread_mutexattr_settype", (void *)sh_ok0);
  set_import("pthread_condattr_init", (void *)sh_ok0);
  set_import("pthread_condattr_destroy", (void *)sh_ok0);
  set_import("pthread_condattr_setclock", (void *)sh_ok0);
  fprintf(stderr, "[pthread] shim wired: mutex+cond+SEM+rwlock+create (bionic<->glibc)\n");
}
