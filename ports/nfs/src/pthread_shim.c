/*
 * pthread_shim.c — tradução bionic↔glibc das primitivas de sincronização.
 *
 * 🔑 As structs pthread do BIONIC e da GLIBC têm TAMANHOS/LAYOUTS diferentes:
 *   bionic pthread_mutex_t ~4B, pthread_cond_t ~4B, sem_t ~4B
 *   glibc  pthread_mutex_t 24B, pthread_cond_t 48B, sem_t 16B, rwlock 32B
 * A engine aloca a primitiva no tamanho BIONIC e passa o ponteiro p/ a função
 * da GLIBC → a glibc lê/escreve além da alocação + faz ldrex/ldrd desalinhado →
 * SIGBUS (visto em pthread_cond_wait). NÃO dá p/ usar o storage bionic como
 * struct glibc.
 *
 * Solução: o ENDEREÇO da primitiva bionic é a CHAVE de um side-table que mapeia
 * p/ um objeto GLIBC real (alocado por nós, tamanho/alinhamento corretos).
 * Criação LAZY (cobre tanto init explícito quanto inicializadores estáticos
 * tipo PTHREAD_MUTEX_INITIALIZER). Os mutexes são RECURSIVE (superset seguro;
 * evita self-deadlock comum em engines; cond_wait com lock-count 1 funciona).
 */
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- hashtable uintptr_t(chave bionic) -> void*(objeto glibc) ---- */
struct sentry { uintptr_t key; void *obj; };
static struct sentry *g_tab;
static int g_cap, g_cnt;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; /* mutex GLIBC real (estático) */

static void tab_grow(void) {
  int ncap = g_cap ? g_cap * 2 : 256;
  struct sentry *nt = calloc(ncap, sizeof(struct sentry));
  for (int i = 0; i < g_cap; i++) {
    if (!g_tab[i].key) continue;
    unsigned h = (unsigned)(g_tab[i].key * 2654435761u) & (ncap - 1);
    while (nt[h].key) h = (h + 1) & (ncap - 1);
    nt[h] = g_tab[i];
  }
  free(g_tab); g_tab = nt; g_cap = ncap;
}
/* procura entrada; se !found e create, aloca obj_size zerado e insere. */
static void *tab_lookup(uintptr_t key, size_t obj_size, int create) {
  if (create && (g_cnt + 1) * 4 >= g_cap * 3) tab_grow();
  if (g_cap == 0) return NULL;
  unsigned h = (unsigned)(key * 2654435761u) & (g_cap - 1);
  while (g_tab[h].key) {
    if (g_tab[h].key == key) return g_tab[h].obj;
    h = (h + 1) & (g_cap - 1);
  }
  if (!create) return NULL;
  void *obj = calloc(1, obj_size);
  g_tab[h].key = key; g_tab[h].obj = obj; g_cnt++;
  return obj;
}
static void tab_remove(uintptr_t key) {
  if (g_cap == 0) return;
  unsigned h = (unsigned)(key * 2654435761u) & (g_cap - 1);
  while (g_tab[h].key) {
    if (g_tab[h].key == key) {
      free(g_tab[h].obj); g_tab[h].key = 0; g_tab[h].obj = NULL; g_cnt--;
      /* re-hash do cluster seguinte (open addressing) */
      h = (h + 1) & (g_cap - 1);
      while (g_tab[h].key) {
        uintptr_t k = g_tab[h].key; void *o = g_tab[h].obj;
        g_tab[h].key = 0; g_tab[h].obj = NULL; g_cnt--;
        unsigned nh = (unsigned)(k * 2654435761u) & (g_cap - 1);
        while (g_tab[nh].key) nh = (nh + 1) & (g_cap - 1);
        g_tab[nh].key = k; g_tab[nh].obj = o; g_cnt++;
        h = (h + 1) & (g_cap - 1);
      }
      return;
    }
    h = (h + 1) & (g_cap - 1);
  }
}

/* ---- helpers de get-or-create por tipo ---- */
static pthread_mutex_t *get_mutex(void *b) {
  pthread_mutex_lock(&g_lock);
  int existed = tab_lookup((uintptr_t)b, 0, 0) != NULL;
  pthread_mutex_t *m = tab_lookup((uintptr_t)b, sizeof(pthread_mutex_t), 1);
  if (!existed && m) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
  }
  pthread_mutex_unlock(&g_lock);
  return m;
}
static pthread_cond_t *get_cond(void *b) {
  pthread_mutex_lock(&g_lock);
  int existed = tab_lookup((uintptr_t)b, 0, 0) != NULL;
  pthread_cond_t *c = tab_lookup((uintptr_t)b, sizeof(pthread_cond_t), 1);
  if (!existed && c) pthread_cond_init(c, NULL);
  pthread_mutex_unlock(&g_lock);
  return c;
}
static pthread_rwlock_t *get_rwlock(void *b) {
  pthread_mutex_lock(&g_lock);
  int existed = tab_lookup((uintptr_t)b, 0, 0) != NULL;
  pthread_rwlock_t *r = tab_lookup((uintptr_t)b, sizeof(pthread_rwlock_t), 1);
  if (!existed && r) pthread_rwlock_init(r, NULL);
  pthread_mutex_unlock(&g_lock);
  return r;
}

/* ---- MUTEX ---- */
static int m_init(void *m, const void *a)   { (void)a; get_mutex(m); return 0; }
static int m_lock(void *m)                  { return pthread_mutex_lock(get_mutex(m)); }
static int m_trylock(void *m)               { return pthread_mutex_trylock(get_mutex(m)); }
static int m_unlock(void *m)                { return pthread_mutex_unlock(get_mutex(m)); }
static int m_destroy(void *m) {
  pthread_mutex_lock(&g_lock);
  pthread_mutex_t *gm = tab_lookup((uintptr_t)m, 0, 0);
  if (gm) { pthread_mutex_destroy(gm); tab_remove((uintptr_t)m); }
  pthread_mutex_unlock(&g_lock);
  return 0;
}

/* mutexattr bionic ≠ glibc: stub (a engine só seta type; usamos recursive sempre) */
static int ma_init(void *a)            { (void)a; return 0; }
static int ma_destroy(void *a)         { (void)a; return 0; }
static int ma_settype(void *a, int t)  { (void)a; (void)t; return 0; }
static int ma_setpshared(void *a, int p){ (void)a; (void)p; return 0; }

/* ---- COND ---- */
static int c_init(void *c, const void *a)   { (void)a; get_cond(c); return 0; }
static int c_signal(void *c)                { return pthread_cond_signal(get_cond(c)); }
static int c_broadcast(void *c)             { return pthread_cond_broadcast(get_cond(c)); }
static int c_wait(void *c, void *m)         { return pthread_cond_wait(get_cond(c), get_mutex(m)); }
static int c_timedwait(void *c, void *m, const struct timespec *t) {
  return pthread_cond_timedwait(get_cond(c), get_mutex(m), t);
}
static int c_destroy(void *c) {
  pthread_mutex_lock(&g_lock);
  pthread_cond_t *gc = tab_lookup((uintptr_t)c, 0, 0);
  if (gc) { pthread_cond_destroy(gc); tab_remove((uintptr_t)c); }
  pthread_mutex_unlock(&g_lock);
  return 0;
}

/* ---- RWLOCK ---- */
static int rw_init(void *r, const void *a)  { (void)a; get_rwlock(r); return 0; }
static int rw_rdlock(void *r)               { return pthread_rwlock_rdlock(get_rwlock(r)); }
static int rw_wrlock(void *r)               { return pthread_rwlock_wrlock(get_rwlock(r)); }
static int rw_unlock(void *r)               { return pthread_rwlock_unlock(get_rwlock(r)); }
static int rw_destroy(void *r) {
  pthread_mutex_lock(&g_lock);
  pthread_rwlock_t *gr = tab_lookup((uintptr_t)r, 0, 0);
  if (gr) { pthread_rwlock_destroy(gr); tab_remove((uintptr_t)r); }
  pthread_mutex_unlock(&g_lock);
  return 0;
}

/* ---- ONCE (bionic once_t=int; usamos glibc pthread_once_t real via tabela) ---- */
static int o_once(void *oc, void (*fn)(void)) {
  pthread_mutex_lock(&g_lock);
  int existed = tab_lookup((uintptr_t)oc, 0, 0) != NULL;
  pthread_once_t *go = tab_lookup((uintptr_t)oc, sizeof(pthread_once_t), 1);
  if (!existed && go) { pthread_once_t init = PTHREAD_ONCE_INIT; *go = init; }
  pthread_mutex_unlock(&g_lock);
  return pthread_once(go, fn);
}

/* ---- SEMÁFORO (bionic sem_t=4B; glibc=16B → side-table) ---- */
static sem_t *get_sem(void *b, int create) {
  pthread_mutex_lock(&g_lock);
  sem_t *s = tab_lookup((uintptr_t)b, create ? sizeof(sem_t) : 0, create);
  pthread_mutex_unlock(&g_lock);
  return s;
}
static int s_init(void *s, int pshared, unsigned value) {
  sem_t *gs = get_sem(s, 1); return gs ? sem_init(gs, pshared, value) : -1;
}
static int s_wait(void *s)      { sem_t *gs = get_sem(s, 1); return sem_wait(gs); }
static int s_trywait(void *s)   { sem_t *gs = get_sem(s, 1); return sem_trywait(gs); }
static int s_post(void *s)      { sem_t *gs = get_sem(s, 1); return sem_post(gs); }
static int s_getvalue(void *s, int *v) { sem_t *gs = get_sem(s, 1); return sem_getvalue(gs, v); }
static int s_timedwait(void *s, const struct timespec *t) { sem_t *gs = get_sem(s, 1); return sem_timedwait(gs, t); }
static int s_destroy(void *s) {
  pthread_mutex_lock(&g_lock);
  sem_t *gs = tab_lookup((uintptr_t)s, 0, 0);
  if (gs) { sem_destroy(gs); tab_remove((uintptr_t)s); }
  pthread_mutex_unlock(&g_lock);
  return 0;
}

/* ---- tabela exportada p/ imports.c concatenar em nfs_shims[] ---- */
#include "so_util.h"
DynLibFunction nfs_pthread_shims[] = {
  {"pthread_mutex_init", (uintptr_t)m_init},
  {"pthread_mutex_lock", (uintptr_t)m_lock},
  {"pthread_mutex_trylock", (uintptr_t)m_trylock},
  {"pthread_mutex_unlock", (uintptr_t)m_unlock},
  {"pthread_mutex_destroy", (uintptr_t)m_destroy},
  {"pthread_mutexattr_init", (uintptr_t)ma_init},
  {"pthread_mutexattr_destroy", (uintptr_t)ma_destroy},
  {"pthread_mutexattr_settype", (uintptr_t)ma_settype},
  {"pthread_mutexattr_setpshared", (uintptr_t)ma_setpshared},
  {"pthread_cond_init", (uintptr_t)c_init},
  {"pthread_cond_signal", (uintptr_t)c_signal},
  {"pthread_cond_broadcast", (uintptr_t)c_broadcast},
  {"pthread_cond_wait", (uintptr_t)c_wait},
  {"pthread_cond_timedwait", (uintptr_t)c_timedwait},
  {"pthread_cond_destroy", (uintptr_t)c_destroy},
  {"pthread_rwlock_init", (uintptr_t)rw_init},
  {"pthread_rwlock_rdlock", (uintptr_t)rw_rdlock},
  {"pthread_rwlock_wrlock", (uintptr_t)rw_wrlock},
  {"pthread_rwlock_unlock", (uintptr_t)rw_unlock},
  {"pthread_rwlock_destroy", (uintptr_t)rw_destroy},
  {"pthread_once", (uintptr_t)o_once},
  {"sem_init", (uintptr_t)s_init},
  {"sem_wait", (uintptr_t)s_wait},
  {"sem_trywait", (uintptr_t)s_trywait},
  {"sem_post", (uintptr_t)s_post},
  {"sem_getvalue", (uintptr_t)s_getvalue},
  {"sem_timedwait", (uintptr_t)s_timedwait},
  {"sem_destroy", (uintptr_t)s_destroy},
};
int nfs_pthread_shims_count = sizeof(nfs_pthread_shims) / sizeof(nfs_pthread_shims[0]);
