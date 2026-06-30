/*
 * pthread_bridge.c — ponte de ABI pthread bionic -> glibc.
 *
 * O reVC e o libc++ foram compilados contra o bionic, cujos objetos opacos
 * (pthread_mutex_t=40B, cond=48B, rwlock=56B no LP64) são MENORES que os do
 * glibc (mutex=48B, cond=48B, rwlock=56B). Passar um mutex bionic pro
 * pthread_mutex_lock do glibc faz o glibc ler/escrever além do storage ->
 * corrupção/deadlock (foi o que travou a cutscene).
 *
 * Solução: tratamos o storage bionic como guardando um PONTEIRO (8B) para o
 * objeto glibc REAL, alocado no heap (lazy na 1ª uso p/ inicializadores
 * estáticos = zero). Só interceptamos os tipos com tamanho divergente; keys,
 * create/join/self/detach (pthread_t compatível 8B) vão direto pro glibc.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "so_util.h"

/* === CANARIO BIONIC (tpidr_el0+0x28) ===
 * A libyoyo (NDK/bionic) le o stack-canary do slot TLS bionic tpidr_el0+0x28
 * (TLS_SLOT_STACK_GUARD). No glibc esse offset NAO e o canary (glibc usa o global
 * __stack_chk_guard) -> o slot guarda lixo que muda entre o prologo (store) e o
 * epilogo (check) -> __stack_chk_fail -> abort (nao-skipavel). Fix: fixamos um valor
 * ESTAVEL nesse slot no inicio de CADA thread que roda codigo da libyoyo (main + as
 * threads criadas via pthread_create). store==check sempre -> passa. */
#define HM_CANARY 0x2b7c91f3a4e6d508ULL
void hm_set_bionic_canary(void) {
  uintptr_t tp;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  *(volatile uint64_t *)(tp + 0x28) = HM_CANARY;
}
/* save/restore do slot: shims que chamam glibc (access/syscall setam errno, que NO GLIBC
 * mora exatamente em tpidr+0x28) precisam preservar o slot p/ o canary do CALLER libyoyo
 * (store no prologo == check no epilogo). Salva na entrada, restaura antes de retornar. */
uint64_t hm_canary_save(void) {
  uintptr_t tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  return *(volatile uint64_t *)(tp + 0x28);
}
void hm_canary_restore(uint64_t v) {
  uintptr_t tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  *(volatile uint64_t *)(tp + 0x28) = v;
}

// lock global (recursivo) p/ serializar lazy-init sem corrida
static pthread_mutex_t g_lock;
__attribute__((constructor)) static void init_glock(void) {
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&g_lock, &a);
  pthread_mutexattr_destroy(&a);
}

// ---------------- mutexattr (bionic = int) ----------------
int b_mutexattr_init(void *a) {
  if (a)
    *(int *)a = 0;
  return 0;
}
int b_mutexattr_destroy(void *a) {
  (void)a;
  return 0;
}
int b_mutexattr_settype(void *a, int type) {
  if (a)
    *(int *)a = type;
  return 0;
}

// ponteiro do heap (grande) vs valor mágico de inicializador estático bionic
// (pequeno: 0, 0x4000 recursivo, 0x8000 errorcheck, etc — todos < 0x10000).
#define IS_HEAP_PTR(v) ((uintptr_t)(v) > 0x10000u)

// cria um glibc mutex RECURSIVO (seguro p/ qualquer uso; evita self-deadlock
// quando o bionic queria recursivo via inicializador estático ou attr).
static pthread_mutex_t *new_recursive_mutex(void) {
  pthread_mutex_t *r = (pthread_mutex_t *)calloc(1, sizeof(pthread_mutex_t));
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(r, &a);
  pthread_mutexattr_destroy(&a);
  return r;
}

// ---------------- mutex ----------------
static pthread_mutex_t *mtx_real(void *m) {
  pthread_mutex_t **slot = (pthread_mutex_t **)m;
  if (IS_HEAP_PTR(*slot))
    return *slot; // já é nosso glibc mutex
  pthread_mutex_lock(&g_lock);
  if (!IS_HEAP_PTR(*slot)) // re-check sob lock (descarta mágica bionic estática)
    *slot = new_recursive_mutex();
  pthread_mutex_unlock(&g_lock);
  return *slot;
}
int b_mutex_init(void *m, const void *attr) {
  (void)attr; // sempre recursivo (superset seguro)
  pthread_mutex_t **slot = (pthread_mutex_t **)m;
  pthread_mutex_t *r = new_recursive_mutex();
  pthread_mutex_lock(&g_lock);
  *slot = r;
  pthread_mutex_unlock(&g_lock);
  return 0;
}
int b_mutex_lock(void *m) { return pthread_mutex_lock(mtx_real(m)); }
int b_mutex_unlock(void *m) { return pthread_mutex_unlock(mtx_real(m)); }
int b_mutex_trylock(void *m) { return pthread_mutex_trylock(mtx_real(m)); }
int b_mutex_destroy(void *m) {
  pthread_mutex_t **slot = (pthread_mutex_t **)m;
  pthread_mutex_lock(&g_lock);
  if (*slot) {
    pthread_mutex_destroy(*slot);
    free(*slot);
    *slot = NULL;
  }
  pthread_mutex_unlock(&g_lock);
  return 0;
}

// ---------------- cond (clock MONOTONIC p/ casar o default do bionic) ----------------
static pthread_cond_t *cnd_real(void *c) {
  pthread_cond_t **slot = (pthread_cond_t **)c;
  if (IS_HEAP_PTR(*slot))
    return *slot;
  pthread_mutex_lock(&g_lock);
  if (!IS_HEAP_PTR(*slot)) {
    pthread_cond_t *r = (pthread_cond_t *)calloc(1, sizeof(pthread_cond_t));
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(r, &a);
    pthread_condattr_destroy(&a);
    *slot = r;
  }
  pthread_mutex_unlock(&g_lock);
  return *slot;
}
int b_cond_init(void *c, const void *a) {
  (void)a;
  pthread_cond_t **slot = (pthread_cond_t **)c;
  pthread_mutex_lock(&g_lock);
  *slot = NULL; // força lazy (com clock monotonic) no 1º uso
  pthread_mutex_unlock(&g_lock);
  cnd_real(c);
  return 0;
}
int b_cond_wait(void *c, void *m) {
  return pthread_cond_wait(cnd_real(c), mtx_real(m));
}
int b_cond_timedwait(void *c, void *m, const struct timespec *t) {
  return pthread_cond_timedwait(cnd_real(c), mtx_real(m), t);
}
int b_cond_signal(void *c) { return pthread_cond_signal(cnd_real(c)); }
int b_cond_broadcast(void *c) { return pthread_cond_broadcast(cnd_real(c)); }
int b_cond_destroy(void *c) {
  pthread_cond_t **slot = (pthread_cond_t **)c;
  pthread_mutex_lock(&g_lock);
  if (*slot) {
    pthread_cond_destroy(*slot);
    free(*slot);
    *slot = NULL;
  }
  pthread_mutex_unlock(&g_lock);
  return 0;
}

// ---------------- rwlock ----------------
static pthread_rwlock_t *rw_real(void *r) {
  pthread_rwlock_t **slot = (pthread_rwlock_t **)r;
  if (IS_HEAP_PTR(*slot))
    return *slot;
  pthread_mutex_lock(&g_lock);
  if (!IS_HEAP_PTR(*slot)) {
    pthread_rwlock_t *rr =
        (pthread_rwlock_t *)calloc(1, sizeof(pthread_rwlock_t));
    pthread_rwlock_init(rr, NULL);
    *slot = rr;
  }
  pthread_mutex_unlock(&g_lock);
  return *slot;
}
int b_rwlock_rdlock(void *r) { return pthread_rwlock_rdlock(rw_real(r)); }
int b_rwlock_wrlock(void *r) { return pthread_rwlock_wrlock(rw_real(r)); }
int b_rwlock_unlock(void *r) { return pthread_rwlock_unlock(rw_real(r)); }

// ---------------- once (bionic = int) ----------------
int b_once(void *once_ctl, void (*init)(void)) {
  // bionic once_control = int. 0=não feito, 1=em progresso, 2=feito.
  // NÃO segura g_lock durante init() (init pode esperar outra thread).
  volatile int *st = (volatile int *)once_ctl;
  pthread_mutex_lock(&g_lock);
  while (*st == 1) { // outra thread inicializando: espera
    pthread_mutex_unlock(&g_lock);
    usleep(200);
    pthread_mutex_lock(&g_lock);
  }
  if (*st == 0) {
    *st = 1;
    pthread_mutex_unlock(&g_lock);
    init();
    pthread_mutex_lock(&g_lock);
    *st = 2;
  }
  pthread_mutex_unlock(&g_lock);
  return 0;
}

// ---------------- pthread_create: seta o canario bionic no inicio da thread ----------------
struct hm_thread_arg { void *(*fn)(void *); void *arg; };
static void *hm_thread_tramp(void *a) {
  struct hm_thread_arg *t = (struct hm_thread_arg *)a;
  void *(*fn)(void *) = t->fn; void *arg = t->arg; free(t);
  hm_set_bionic_canary();   /* canario estavel ANTES de qualquer codigo libyoyo */
  return fn(arg);
}
int b_pthread_create(pthread_t *th, const pthread_attr_t *attr,
                     void *(*fn)(void *), void *arg) {
  struct hm_thread_arg *t = (struct hm_thread_arg *)malloc(sizeof(*t));
  if (!t) return pthread_create(th, attr, fn, arg);
  t->fn = fn; t->arg = arg;
  return pthread_create(th, attr, hm_thread_tramp, t);
}

DynLibFunction revc_pthread_table[] = {
    {"pthread_create", (uintptr_t)&b_pthread_create},
    {"pthread_mutexattr_init", (uintptr_t)&b_mutexattr_init},
    {"pthread_mutexattr_destroy", (uintptr_t)&b_mutexattr_destroy},
    {"pthread_mutexattr_settype", (uintptr_t)&b_mutexattr_settype},
    {"pthread_mutex_init", (uintptr_t)&b_mutex_init},
    {"pthread_mutex_lock", (uintptr_t)&b_mutex_lock},
    {"pthread_mutex_unlock", (uintptr_t)&b_mutex_unlock},
    {"pthread_mutex_trylock", (uintptr_t)&b_mutex_trylock},
    {"pthread_mutex_destroy", (uintptr_t)&b_mutex_destroy},
    {"pthread_cond_init", (uintptr_t)&b_cond_init},
    {"pthread_cond_wait", (uintptr_t)&b_cond_wait},
    {"pthread_cond_timedwait", (uintptr_t)&b_cond_timedwait},
    {"pthread_cond_signal", (uintptr_t)&b_cond_signal},
    {"pthread_cond_broadcast", (uintptr_t)&b_cond_broadcast},
    {"pthread_cond_destroy", (uintptr_t)&b_cond_destroy},
    {"pthread_rwlock_rdlock", (uintptr_t)&b_rwlock_rdlock},
    {"pthread_rwlock_wrlock", (uintptr_t)&b_rwlock_wrlock},
    {"pthread_rwlock_unlock", (uintptr_t)&b_rwlock_unlock},
    {"pthread_once", (uintptr_t)&b_once},
};
const int revc_pthread_count =
    sizeof(revc_pthread_table) / sizeof(revc_pthread_table[0]);
