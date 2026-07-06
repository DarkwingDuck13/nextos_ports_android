/*
 * main.c — Cuphead (Unity 2017.4.40f1 IL2CPP) so-loader → NextOS/Mali-450 (arm64, GLES2).
 *
 * Receita Unity baseada no port re4 (Unity 2018 Mono), adaptada p/ arm64 + IL2CPP:
 *   - dlopen libz/libGLESv2/libEGL RTLD_GLOBAL (Unity resolve via dlsym RTLD_DEFAULT)
 *   - so_load libunity.so (engine) -> imports overrides -> init_array
 *   - so_load libil2cpp.so (lógica do jogo C#) + global-metadata.dat   [fase seguinte]
 *   - JNI_OnLoad -> janela GLES2 -> lifecycle (initJni/nativeRender)    [fase seguinte]
 * Alvo GLES2: passar -force-gles20 ao Unity (args via initJni/command line).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sched.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <SDL2/SDL.h>

#include "so_util.h"
#include "imports.h"
#include "jni_shim.h"
#include "opensles_shim.h"
#include "util.h"
#include <link.h>

#define HEAP_MB 160

/* ---- dl_iterate_phdr custom (INTERPÕE o da libc) ----
 * O unwinder C++ da libgcc acha o .eh_frame de cada lib via dl_iterate_phdr. Nossos
 * módulos (libunity/libil2cpp) são mapeados à mão -> invisíveis ao dl_iterate_phdr da
 * libc -> exceção C++ não acha o landing pad -> std::terminate -> abort (asset loading).
 * Como o EXE é -rdynamic e carrega 1º, este símbolo INTERPÕE o da libc: reportamos os
 * módulos do dynamic linker (via o real, RTLD_NEXT) + os NOSSOS (g_so_mods). */
int dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data) {
  static int (*real)(int (*)(struct dl_phdr_info *, size_t, void *), void *);
  if (!real) real = (void *)dlsym(RTLD_NEXT, "dl_iterate_phdr");
  int r = real ? real(cb, data) : 0;
  if (r) return r;
  for (int i = 0; i < g_so_nmods; i++) {
    struct dl_phdr_info info; memset(&info, 0, sizeof info);
    info.dlpi_addr = (ElfW(Addr))g_so_mods[i].base;
    info.dlpi_name = g_so_mods[i].name;
    info.dlpi_phdr = (const ElfW(Phdr) *)g_so_mods[i].ph;
    info.dlpi_phnum = (ElfW(Half))g_so_mods[i].phnum;
    r = cb(&info, sizeof info, data);
    if (r) return r;
  }
  return r;
}

/* canary bionic: libunity lê o stack-guard de tpidr_el0+0x28 (TLS_SLOT_STACK_GUARD
 * do bionic); sob glibc esse offset cai no TLS de outra lib e MUDA em runtime →
 * __stack_chk_fail espúrio (e o "SEGV após neutralizar" era o no-op retornando em
 * código adjacente — noreturn). Pad TLS no exe (1º bloco após o TCB de 16B) cobre
 * offset 16..272 e NUNCA é escrito → slot estável. (causa-raiz achada no Dysmantle) */
/* = {1} → .tdata: fica ANTES das TLS .tbss do egl_shim (link order) no template,
 * senão o pad desliza p/ +0x30 e o slot +0x28 cai fora (visto no device). */
__attribute__((aligned(16))) static _Thread_local char g_bionic_guard_pad[256] = {1};

/* fsync(stderr→debug.log): garante que o log sobrevive a hang/power-cycle */
static void dbg_sync(void) { fsync(2); }

static int env_on(const char *name) {
  const char *v = getenv(name);
  return v && v[0] && strcmp(v, "0") != 0 && strcasecmp(v, "false") != 0 && strcasecmp(v, "no") != 0;
}

static int env_int_default(const char *name, int def) {
  const char *v = getenv(name);
  if (!v || !v[0]) return def;
  char *end = NULL;
  long n = strtol(v, &end, 10);
  return end && end != v ? (int)n : def;
}

static int env_list_has_value(const char *name, const char *value) {
  const char *v = getenv(name);
  if (!v || !v[0] || !value) return 0;
  size_t value_len = strlen(value);
  const char *p = v;
  while (*p) {
    while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t') p++;
    const char *start = p;
    while (*p && *p != ',' && *p != ';' && *p != ' ' && *p != '\t') p++;
    if ((size_t)(p - start) == value_len && !strncmp(start, value, value_len)) return 1;
  }
  return 0;
}

static const char *env_remap_value(const char *name, const char *key, char *out, size_t cap) {
  const char *v = getenv(name);
  if (!v || !v[0] || !key || !out || cap == 0) return NULL;
  size_t key_len = strlen(key);
  const char *p = v;
  while (*p) {
    while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t') p++;
    const char *ks = p;
    while (*p && *p != ':' && *p != '=' && *p != ',' && *p != ';') p++;
    const char *ke = p;
    while (ke > ks && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
    if (*p != ':' && *p != '=') {
      while (*p && *p != ',' && *p != ';') p++;
      continue;
    }
    p++;
    while (*p == ' ' || *p == '\t') p++;
    const char *vs = p;
    while (*p && *p != ',' && *p != ';') p++;
    const char *ve = p;
    while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
    if ((size_t)(ke - ks) == key_len && !strncmp(ks, key, key_len) && ve > vs) {
      size_t n = (size_t)(ve - vs);
      if (n >= cap) n = cap - 1;
      memcpy(out, vs, n);
      out[n] = 0;
      return out;
    }
  }
  return NULL;
}

/* sem_shim: semáforos próprios (bionic sem_t 4B vs glibc 32B) — ver sem_shim.c.
   CAUSA-RAIZ do deadlock no boot: sem_post do glibc não acordava o sem_wait da
   thread pool do Unity. CUP_NOSEMSHIM=1 desliga (volta ao glibc cru). */
extern int sh_sem_init(void *, int, unsigned);
extern int sh_sem_wait(void *);
extern int sh_sem_trywait(void *);
extern int sh_sem_timedwait(void *, const struct timespec *);
extern int sh_sem_post(void *);
extern int sh_sem_getvalue(void *, int *);
extern int sh_sem_destroy(void *);
extern int g_main_tid;
extern void sh_tick_preload(void);
extern void sh_sem_set_poll(int ms);
static void set_import(const char *name, void *fn);
static int patch_got(const char *name, void *fn);
static void install_sem_shim(void) {
  if (getenv("CUP_NOSEMSHIM")) return;
  set_import("sem_init", (void *)sh_sem_init);
  set_import("sem_wait", (void *)sh_sem_wait);
  set_import("sem_trywait", (void *)sh_sem_trywait);
  set_import("sem_timedwait", (void *)sh_sem_timedwait);
  set_import("sem_post", (void *)sh_sem_post);
  set_import("sem_getvalue", (void *)sh_sem_getvalue);
  set_import("sem_destroy", (void *)sh_sem_destroy);
}
static void patch_sem_shim(void) {
  if (getenv("CUP_NOSEMSHIM")) return;
  patch_got("sem_init", (void *)sh_sem_init);
  patch_got("sem_wait", (void *)sh_sem_wait);
  patch_got("sem_trywait", (void *)sh_sem_trywait);
  patch_got("sem_timedwait", (void *)sh_sem_timedwait);
  patch_got("sem_post", (void *)sh_sem_post);
  patch_got("sem_getvalue", (void *)sh_sem_getvalue);
  patch_got("sem_destroy", (void *)sh_sem_destroy);
}

/* pthread mutex/cond/rwlock/attr (bionic) -> objetos glibc reais via ponteiro no slot
   (pthread_fake.c). Em arm64 o struct bionic é >=40B (cabe o ponteiro). SEM isso,
   passthrough -> bionic struct + glibc cond_wait = SIGBUS (ponteiro lixo). Wira o
   conjunto COMPLETO (init/destroy/lock/.../wait) p/ o slot SEMPRE guardar nosso ponteiro. */
#define PT_LIST(X) \
  X("pthread_mutex_init", pthread_mutex_init_fake) X("pthread_mutex_destroy", pthread_mutex_destroy_fake) \
  X("pthread_mutex_lock", pthread_mutex_lock_fake) X("pthread_mutex_unlock", pthread_mutex_unlock_fake) \
  X("pthread_mutex_trylock", pthread_mutex_trylock_fake) \
  X("pthread_cond_init", pthread_cond_init_fake) X("pthread_cond_destroy", pthread_cond_destroy_fake) \
  X("pthread_cond_wait", pthread_cond_wait_fake) X("pthread_cond_timedwait", pthread_cond_timedwait_fake) \
  X("pthread_cond_signal", pthread_cond_signal_fake) X("pthread_cond_broadcast", pthread_cond_broadcast_fake) \
  X("pthread_condattr_init", pthread_condattr_init_fake) X("pthread_condattr_destroy", pthread_condattr_destroy_fake) \
  X("pthread_condattr_setclock", pthread_condattr_setclock_fake) \
  X("pthread_mutexattr_init", pthread_mutexattr_init_fake) X("pthread_mutexattr_destroy", pthread_mutexattr_destroy_fake) \
  X("pthread_mutexattr_settype", pthread_mutexattr_settype_fake) \
  X("pthread_rwlock_init", pthread_rwlock_init_fake) X("pthread_rwlock_destroy", pthread_rwlock_destroy_fake) \
  X("pthread_rwlock_rdlock", pthread_rwlock_rdlock_fake) X("pthread_rwlock_wrlock", pthread_rwlock_wrlock_fake) \
  X("pthread_rwlock_tryrdlock", pthread_rwlock_tryrdlock_fake) X("pthread_rwlock_trywrlock", pthread_rwlock_trywrlock_fake) \
  X("pthread_rwlock_unlock", pthread_rwlock_unlock_fake) \
  X("pthread_sigmask", pthread_sigmask_fake)
#define PT_DECL(n, f) extern int f();
PT_LIST(PT_DECL)
extern int pthread_create_fake(pthread_t *, const void *, void *(*)(void *), void *);
static void install_pthread_shim(void) {
  if (getenv("TER_NOPTSHIM")) return;
#define PT_SET(n, f) set_import(n, (void *)f);
  PT_LIST(PT_SET)
  /* TER_JOBLOG: roteia o pthread_create da ENGINE pelo nosso trampoline p/ logar
     (start_routine, arg=JobQueue) de cada worker — só diagnóstico, opt-in. */
  if (getenv("TER_JOBLOG")) set_import("pthread_create", (void *)pthread_create_fake);
}
static void patch_pthread_shim(void) {
  if (getenv("TER_NOPTSHIM")) return;
#define PT_PATCH(n, f) patch_got(n, (void *)f);
  PT_LIST(PT_PATCH)
  if (getenv("TER_JOBLOG")) patch_got("pthread_create", (void *)pthread_create_fake);
}

/* ---------- crash handler (arm64) ---------- */
static uintptr_t g_unity_base, g_il2cpp_base, g_unity_data;
static uintptr_t g_i2heap_base, g_i2heap_size;
/* exposto p/ pthread_fake.c (TER_JOBLOG: symbolizar start_routine dos workers) */
uintptr_t ter_unity_base(void) { return g_unity_base; }
uintptr_t ter_il2cpp_base(void) { return g_il2cpp_base; }
void *ter_il2cpp_sym(const char *nm);
static void *ter_il2cpp_sym_cached(const char *nm);

/* TER_INLINETASK: FINGE a conclusão do per-object future-task NA MAIN. A main constrói o future
   (0x2f3680), submete o functor a um pool, e espera em 0x2f37a4 que um worker rode o functor e
   chame a conclusão 0x2f3a98 (que seta o nó obj+88 + incrementa o contador GLOBAL c10360 que o
   WaitForJobGroup da frame 3 espera). O dispatch p/ os workers está quebrado no so-loader (eles
   ficam ociosos). Aqui, no topo do loop de espera, a própria main faz o bookkeeping da conclusão:
   seta node->next!=0 (sai da espera) + incrementa c10360 (destrava a frame 3). O TRABALHO de
   serialização em si é pulado (já era tolerado como warning "missing script"). Chamado pelo
   trampolim instalado em TER_INLINETASK. */
static volatile int g_inlinetask_n = 0;
void ter_inline_task(void *obj) {
  if (!obj) return;
  void *node = *(void **)((char *)obj + 88);    /* obj+0x58 = node */
  if (node) *(void **)node = (void *)1;          /* node->next = 1 → satisfaz `cbnz` em 0x2f37b0 */
  if (g_unity_base) {
    uint32_t *cnt = (uint32_t *)(g_unity_base + 0xc10360);
    __atomic_add_fetch(cnt, 1, __ATOMIC_SEQ_CST);
  }
  int n = __atomic_add_fetch(&g_inlinetask_n, 1, __ATOMIC_RELAXED);
  if (n <= 5 || (n % 50) == 0) { fprintf(stderr, "[INLINETASK] #%d obj=%p node=%p c10360++\n", n, obj, node); fsync(2); }
}

/* TER_NUKEKB: patcha métodos il2cpp que lançam exceção TODA FRAME e ABORTAM o ExecuteFrame
   ANTES do Draw (KeyboardInput.Update lê o campo Java 'PressedStates' via reflection que falha
   no nosso JNI fake). Usa a API il2cpp REAL (exportada) p/ achar a classe+método e patchar o
   methodPointer p/ `ret` (no-op). Roda lazy do swap-hook até achar (il2cpp já inicializado). */
static void ter_nuke_methods(void) {
  static int done = 0;
  int want_kb = getenv("TER_NUKEKB") ? 1 : 0;
  int want_nanpart = getenv("TER_FIXNANPART") ? 1 : 0;
  if (done || !g_il2cpp_base || (!want_kb && !want_nanpart)) { if (!want_kb && !want_nanpart) done = 1; return; }
  static int tries = 0; if (tries++ > 600) { done = 1; return; }
  void *(*dom_get)(void) = (void *)(g_il2cpp_base + 0x73c860);
  const void **(*dom_asms)(void *, size_t *) = (void *)(g_il2cpp_base + 0x73c86c);
  void *(*asm_img)(const void *) = (void *)(g_il2cpp_base + 0x73c22c);
  void *(*cls_from_name)(void *, const char *, const char *) = (void *)(g_il2cpp_base + 0x73c264);
  void *(*cls_method)(void *, const char *, int) = (void *)(g_il2cpp_base + 0x73c28c);
  void *domain = dom_get(); if (!domain) return;
  size_t na = 0; const void **asms = dom_asms(domain, &na); if (!asms || !na) return;
  struct nuke_target { const char *env, *ns, *cn, *mn; int argc; };
  static const struct nuke_target targets[] = {
    { "TER_NUKEKB", "", "KeyboardInput", "Update", 0 },
    { "TER_FIXNANPART", "Terraria.Graphics.Renderers", "LittleFlyingCritterParticle", "Update", 1 },
    { "TER_RXD_NUKE_UI_AWAKE", "", "UIManager", "Awake", 0 },
  };
  static unsigned patched_mask;
  int patched = 0;
  for (size_t i = 0; i < na; i++) {
    void *img = asm_img(asms[i]); if (!img) continue;
    for (unsigned t = 0; t < sizeof targets/sizeof targets[0]; t++) {
      if (!getenv(targets[t].env) || (patched_mask & (1u << t))) continue;
      void *cls = cls_from_name(img, targets[t].ns, targets[t].cn); if (!cls) continue;
      void *m = cls_method(cls, targets[t].mn, targets[t].argc); if (!m) continue;
      void *mp = *(void **)m;   /* MethodInfo.methodPointer @ off 0 */
      if (!mp) continue;
      long pgsz = sysconf(_SC_PAGESIZE);
      void *pa = (void *)((uintptr_t)mp & ~((uintptr_t)pgsz - 1));
      mprotect(pa, pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
      *(uint32_t *)mp = 0xD65F03C0u;   /* ret */
      mprotect(pa, pgsz * 2, PROT_READ | PROT_EXEC);
      __builtin___clear_cache((char *)pa, (char *)pa + pgsz * 2);
      patched_mask |= (1u << t);
      fprintf(stderr, "[NUKE] %s%s%s.%s @%p -> ret (asm %zu)\n",
              targets[t].ns, targets[t].ns[0] ? "." : "", targets[t].cn, targets[t].mn, mp, i);
      fsync(2);
      patched++;
    }
  }
  int all = 1;
  for (unsigned t = 0; t < sizeof targets/sizeof targets[0]; t++)
    if (getenv(targets[t].env) && !(patched_mask & (1u << t))) all = 0;
  if (all) done = 1;
  (void)patched;
}

/* 🖤 TER_FIXSP: tela preta ao clicar Single Player. SelectSinglePlayer→Main.LoadPlayers→
 * OldSaveSynchronise.CopyOldSaves→get_OldSaveRoot lança NullReferenceException (migração de
 * saves antigos do Android: path/JNI nulo no so-loader) → a tela de seleção de player não
 * carrega → preto. Neutralizamos CopyOldSaves (-> ret): não há saves antigos pra migrar. */
static void ter_fix_singleplayer(void) {
  static int done = 0; if (done || !g_il2cpp_base || !getenv("TER_FIXSP")) { if (!getenv("TER_FIXSP")) done = 1; return; }
  static int tries = 0; if (tries++ > 400) { done = 1; return; }
  long pgsz0 = sysconf(_SC_PAGESIZE);
  /* 🖤 GUILowDiskSpacePopup.CheckDiskSpace mostra "low on storage" se DiskSpace()<=~50MB.
     DiskSpace() (il2cpp+0xd158ac) usa statfs nativo que retorna pouco no so-loader (espaço real
     = 93GB). Patchamos DiskSpace p/ retornar 1GB (movz x0,#0; movk x0,#0x4000,lsl16; ret). */
  { static int dsdone=0; if(!dsdone){ uint32_t*c=(uint32_t*)(g_il2cpp_base+0xd158ac);
      void*pa=(void*)((uintptr_t)c & ~((uintptr_t)pgsz0-1));
      mprotect(pa,pgsz0*2,PROT_READ|PROT_WRITE|PROT_EXEC);
      c[0]=0xD2800000u; c[1]=0xF2A80000u; c[2]=0xD65F03C0u;   /* return 0x40000000 (1GB) */
      mprotect(pa,pgsz0*2,PROT_READ|PROT_EXEC); __builtin___clear_cache((char*)pa,(char*)pa+12);
      fprintf(stderr,"[FIXSP] GUILowDiskSpacePopup.DiskSpace -> 1GB\n"); fsync(2); dsdone=1; } }
  void *(*dom_get)(void) = (void *)(g_il2cpp_base + 0x73c860);
  const void **(*dom_asms)(void *, size_t *) = (void *)(g_il2cpp_base + 0x73c86c);
  void *(*asm_img)(const void *) = (void *)(g_il2cpp_base + 0x73c22c);
  void *(*cls_from_name)(void *, const char *, const char *) = (void *)(g_il2cpp_base + 0x73c264);
  void *(*cls_method)(void *, const char *, int) = (void *)(g_il2cpp_base + 0x73c28c);
  void *domain = dom_get(); if (!domain) return;
  size_t na = 0; const void **asms = dom_asms(domain, &na); if (!asms || !na) return;
  for (size_t i = 0; i < na; i++) {
    void *img = asm_img(asms[i]); if (!img) continue;
    void *cls = cls_from_name(img, "Terraria.IO", "OldSaveSynchronise"); if (!cls) continue;
    void *m = cls_method(cls, "CopyOldSaves", 0); if (!m) { continue; }
    void *mp = *(void **)m; if (!mp) { done = 1; return; }
    long pgsz = sysconf(_SC_PAGESIZE);
    void *pa = (void *)((uintptr_t)mp & ~((uintptr_t)pgsz - 1));
    mprotect(pa, pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
    *(uint32_t *)mp = 0xD65F03C0u;   /* ret */
    mprotect(pa, pgsz * 2, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)pa, (char *)pa + 8);
    fprintf(stderr, "[FIXSP] OldSaveSynchronise.CopyOldSaves @%p -> ret (asm %zu)\n", mp, i); fsync(2);
    done = 1; return;
  }
}

/* TER_JOBWORKERS0: chama JobsUtility.JobWorkerCount=0 (e ActiveThreadCount=0) via il2cpp_runtime_invoke
   → Unity roda os jobs INLINE na própria thread (dispatch pros worker threads está quebrado no
   so-loader). Fix CORRETO (vs fingir com INLINETASK/SKIPJOBWAIT). Lazy do swap-hook até conseguir. */
static void ter_jobworkers0(void) {
  static int done = 0; if (done || !g_il2cpp_base || !getenv("TER_JOBWORKERS0")) { if (!getenv("TER_JOBWORKERS0")) done = 1; return; }
  static int tries = 0; if (tries++ > 240) { done = 1; return; }
  void *(*dom_get)(void) = (void *)(g_il2cpp_base + 0x73c860);
  const void **(*dom_asms)(void *, size_t *) = (void *)(g_il2cpp_base + 0x73c86c);
  void *(*asm_img)(const void *) = (void *)(g_il2cpp_base + 0x73c22c);
  void *(*cls_from_name)(void *, const char *, const char *) = (void *)(g_il2cpp_base + 0x73c264);
  void *(*cls_method)(void *, const char *, int) = (void *)(g_il2cpp_base + 0x73c28c);
  void *(*rt_invoke)(void *, void *, void **, void **) = (void *)(g_il2cpp_base + 0x73cc7c);
  void *domain = dom_get(); if (!domain) return;
  size_t na = 0; const void **asms = dom_asms(domain, &na); if (!asms || !na) return;
  for (size_t i = 0; i < na; i++) {
    void *img = asm_img(asms[i]); if (!img) continue;
    void *cls = cls_from_name(img, "Unity.Jobs.LowLevel.Unsafe", "JobsUtility"); if (!cls) continue;
    static int enum_once = 0;
    if (getenv("TER_JOBENUM") && !enum_once && ++enum_once) { void (*cls_init)(void *) = (void *)(g_il2cpp_base + 0x73cc80); cls_init(cls);
      fprintf(stderr, "[JOBWORKERS0] JobsUtility achada (asm %zu) — métodos:\n", i);
      void *(*cls_methods)(void *, void **) = (void *)(g_il2cpp_base + 0x73c288);
      const char *(*meth_name)(void *) = (void *)(g_il2cpp_base + 0x73cb9c);
      unsigned (*meth_pc)(void *) = (void *)(g_il2cpp_base + 0x73cbac);
      void *it = NULL, *mm; int cnt = 0;
      while ((mm = cls_methods(cls, &it)) && cnt++ < 60) fprintf(stderr, "   %s/%u\n", meth_name(mm), meth_pc(mm));
      fsync(2);
    }
    int zero = 0; void *params[1] = { &zero }; void *exc = NULL;
    const char *setters[] = { "set_JobWorkerCount", "SetJobQueueMaximumActiveThreadCount", "SetJobQueueMaximumWarpThreadCount" };
    int any = 0;
    for (unsigned s = 0; s < sizeof setters/sizeof setters[0]; s++) {
      void *m = cls_method(cls, setters[s], 1); if (!m) continue;
      exc = NULL; rt_invoke(m, NULL, params, &exc);
      fprintf(stderr, "[JOBWORKERS0] %s(0) invoked exc=%p\n", setters[s], exc); fsync(2); any = 1;
    }
    if (any) { done = 1; return; }
  }
  if (tries > 30) done = 1;   /* desiste do retry (evita spam) se não achou os setters */
}
extern size_t text_size;
/* /proc/self/maps lido UMA vez (sem malloc — open/read/parse manual; fopen não é
 * async-signal-safe e re-faulta no handler). Buffer estático grande o bastante. */
static char g_maps_buf[64 * 1024];
static int g_maps_len;
static void maps_snapshot(void) {
  int fd = open("/proc/self/maps", O_RDONLY);
  g_maps_len = 0;
  if (fd < 0) return;
  int n; char *p = g_maps_buf;
  while (g_maps_len < (int)sizeof(g_maps_buf) - 1 &&
         (n = read(fd, p + g_maps_len, sizeof(g_maps_buf) - 1 - g_maps_len)) > 0)
    g_maps_len += n;
  g_maps_buf[g_maps_len] = 0;
  close(fd);
}
/* acha a linha de maps que contém 'a'; preenche lo/hi/perm; retorna ptr p/ a linha
 * (NUL-terminada temporariamente) ou NULL. Parse manual sobre o snapshot. */
static const char *maps_find(uintptr_t a, uintptr_t *lo_o, uintptr_t *hi_o, char perm_o[5]) {
  const char *s = g_maps_buf;
  while (s < g_maps_buf + g_maps_len) {
    const char *eol = s; while (*eol && *eol != '\n') eol++;
    uintptr_t lo = 0, hi = 0; const char *q = s;
    while (*q && *q != '-') { lo = lo * 16 + (*q <= '9' ? *q - '0' : (*q | 32) - 'a' + 10); q++; }
    if (*q == '-') q++;
    while (*q && *q != ' ') { hi = hi * 16 + (*q <= '9' ? *q - '0' : (*q | 32) - 'a' + 10); q++; }
    if (a >= lo && a < hi) {
      if (lo_o) *lo_o = lo; if (hi_o) *hi_o = hi;
      if (perm_o) { const char *pp = q + 1; for (int i = 0; i < 4; i++) perm_o[i] = pp[i]; perm_o[4] = 0; }
      static char line[256]; int len = (int)(eol - s); if (len > 255) len = 255;
      for (int i = 0; i < len; i++) line[i] = s[i]; line[len] = 0;
      return line;
    }
    s = (*eol == '\n') ? eol + 1 : eol;
  }
  return NULL;
}
static int addr_readable(uintptr_t a) {
  char perm[5]; uintptr_t lo, hi;
  return maps_find(a, &lo, &hi, perm) && perm[0] == 'r';
}
/* imprime a linha de maps que contém 'a' + classifica vs nossas bases */
static void crash_classify(const char *tag, uintptr_t a) {
  fprintf(stderr, "[CR] %s=0x%lx", tag, (unsigned long)a);
  if (g_unity_base && a >= g_unity_base && a < g_unity_base + text_size)
    fprintf(stderr, " (libunity+0x%lx)", a - g_unity_base);
  else if (g_il2cpp_base && a >= g_il2cpp_base && a < g_il2cpp_base + 0x3000000)
    fprintf(stderr, " (libil2cpp+0x%lx)", a - g_il2cpp_base);
  else if (g_i2heap_base && a >= g_i2heap_base && a < g_i2heap_base + g_i2heap_size)
    fprintf(stderr, " (i2heap+0x%lx)", a - g_i2heap_base);
  char perm[5]; uintptr_t lo, hi;
  const char *line = maps_find(a, &lo, &hi, perm);
  if (line) fprintf(stderr, "  | %s", line);
  fprintf(stderr, "\n"); dbg_sync();
}
static void crash_dump_qwords(const char *tag, uintptr_t base, int n) {
  if (!addr_readable(base)) { fprintf(stderr, "[CR] %s @0x%lx ILEGÍVEL\n", tag, (unsigned long)base); dbg_sync(); return; }
  for (int k = 0; k < n; k += 2)
    fprintf(stderr, "[CR] %s +%02x: %016lx %016lx\n", tag, k * 8,
            (unsigned long)((uintptr_t *)base)[k], (unsigned long)((uintptr_t *)base)[k + 1]);
  dbg_sync();
}

static volatile int g_crashing = 0;
#define ARENA_LO 0x7f10000000UL
#define ARENA_HI 0x7f10200000UL
static volatile unsigned long g_skipbad_n = 0;
static int g_skipbad = 0;  /* lido 1× no startup (getenv não é async-signal-safe) */
/* recovery por-frame: sigsetjmp antes de nativeRender; on_crash siglongjmp de volta
   (só se o crash for na THREAD de render — longjmp cross-thread é UB). Pula o frame
   corrompido e continua → renderiza apesar das chamadas de método C# corrompidas. */
#include <setjmp.h>
/* GC stop-the-world: SIGPWR suspende a thread (espera o restart SIGXCPU); SIGXCPU
   é no-op (sua chegada acorda o sigsuspend). Mantém nossas threads vivas durante a
   coleta (sem isso, SIGPWR default mata o processo -> exit 158). */
void gc_suspend_handler(int sig);
void gc_suspend_handler(int sig) {
  (void)sig;
  /* CUP_GCSUSP=wait: protocolo real (suspende até SIGXCPU). Default: RETORNA imediato
     (não suspende) — o stop-the-world do GC está quebrado (handler corrompido) e nunca
     manda o restart, então suspender CONGELA a render. Retornar deixa a thread seguir
     (coleta vira racy, mas a render avança → imagem). */
  if (getenv("CUP_GCSUSP")) {
    sigset_t m; sigfillset(&m);
    sigdelset(&m, SIGXCPU); sigdelset(&m, SIGSEGV); sigdelset(&m, SIGBUS);
    sigsuspend(&m);
  }
}
void gc_restart_handler(int sig);
void gc_restart_handler(int sig) { (void)sig; }
static sigjmp_buf g_render_jmp;
static volatile int g_render_jmp_armed = 0;
static int g_render_tid = 0;
static volatile unsigned long g_recover_n = 0;
static void on_crash(int sig, siginfo_t *si, void *uc_) {
  ucontext_t *uc0 = (ucontext_t *)uc_;
  uintptr_t pc0 = uc0->uc_mcontext.pc, lr0 = uc0->uc_mcontext.regs[30];
  /* 🔎 dump async-signal-safe (write(2) cru, imune a stdio/FILE-lock): garante
     PC/fault/lr mesmo quando o dump rico via fprintf se perde. offsets p/ libunity
     (g_unity_base) e libil2cpp (g_il2cpp_base) p/ casar com objdump no host. */
  {
    char b[256]; int n = 0;
    static const char hx[] = "0123456789abcdef";
    #define _EMIT_S(s) do { const char *p=(s); while(*p&&n<240) b[n++]=*p++; } while(0)
    #define _EMIT_H(v) do { unsigned long _v=(unsigned long)(v); b[n++]='0'; b[n++]='x'; \
        for(int _i=60;_i>=0;_i-=4) b[n++]=hx[(_v>>_i)&0xf]; } while(0)
    _EMIT_S("\n[CR!] sig="); b[n++]=hx[sig&0xf];
    _EMIT_S(" fault="); _EMIT_H((unsigned long)si->si_addr);
    _EMIT_S(" pc="); _EMIT_H(pc0);
    if (g_unity_base && pc0>=g_unity_base && pc0<g_unity_base+0x2000000){ _EMIT_S(" unity+"); _EMIT_H(pc0-g_unity_base); }
    if (g_il2cpp_base && pc0>=g_il2cpp_base && pc0<g_il2cpp_base+0x4000000){ _EMIT_S(" il2cpp+"); _EMIT_H(pc0-g_il2cpp_base); }
    _EMIT_S(" lr="); _EMIT_H(lr0);
    if (g_unity_base && lr0>=g_unity_base && lr0<g_unity_base+0x2000000){ _EMIT_S(" unity+"); _EMIT_H(lr0-g_unity_base); }
    _EMIT_S("\n"); if(n<256){ ssize_t _w=write(2,b,n); (void)_w; }
    #undef _EMIT_S
    #undef _EMIT_H
  }
  /* recovery: crash na thread de render (qualquer fault, não só arena) → volta pro
     loop e pula o frame. Só se armado e na thread certa. */
  if (g_render_jmp_armed && (int)syscall(SYS_gettid) == g_render_tid) {
    g_recover_n++;
    siglongjmp(g_render_jmp, 1);
  }
  /* skipbad: crash em thread NÃO-render (worker/job) → estaciona a thread em vez de
     matar o processo (mantém o jogo vivo p/ a render continuar). */
  if (g_skipbad && sig == SIGSEGV) {
    static volatile unsigned long parked = 0;
    if (parked++ < 40)
      fprintf(stderr, "[PARK] worker tid=%d crashou (pc=0x%lx) — estacionado\n",
              (int)syscall(SYS_gettid), (unsigned long)pc0);
    dbg_sync();
    for (;;) pause();
  }
  /* CUP_SKIPBAD: o ponteiro de método genérico corrompido (→ arena 2MB) é chamado
     em vários sites. Se o pc cai na arena (chamou o lixo), PULA a chamada: retoma
     no lr com retorno null (x0=0). Se as chamadas não forem críticas, o jogo passa
     e renderiza. Hack p/ destravar a imagem (não é fix definitivo). */
  if (g_skipbad && sig == SIGSEGV && pc0 >= ARENA_LO && pc0 < ARENA_HI) {
    if (lr0 && lr0 != pc0) {
      uc0->uc_mcontext.pc = lr0;        /* retoma no retorno */
      uc0->uc_mcontext.regs[0] = 0;     /* valor de retorno = null/0 */
      if (g_skipbad_n++ < 60)
        fprintf(stderr, "[SKIPBAD] #%lu pc=arena -> pula p/ lr=0x%lx\n",
                g_skipbad_n, (unsigned long)lr0);
      if ((g_skipbad_n & 0x3ff) == 0) dbg_sync();
      return;  /* resume */
    }
  }
  /* reentrância: se outra thread já está dumpando (vtable corrompido faz várias
     threads crasharem juntas), esta espera p/ não interleavar/re-faultar o dump. */
  if (__sync_lock_test_and_set(&g_crashing, 1)) {
    fprintf(stderr, "[CR] (2ª thread crashou sig=%d tid=%d — aguardando)\n",
            sig, (int)syscall(SYS_gettid));
    dbg_sync();
    for (;;) pause();
  }
  ucontext_t *uc = (ucontext_t *)uc_;
  uintptr_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
  uintptr_t tb = (uintptr_t)text_base;
  maps_snapshot();   /* sem malloc — antes de qualquer parse */
  fprintf(stderr, "\n=== CRASH sig=%d fault=%p pc=0x%lx", sig, si->si_addr,
          (unsigned long)pc);
  if (pc >= tb && pc < tb + text_size) fprintf(stderr, " (libunity+0x%lx)", pc - tb);
  fprintf(stderr, " lr=0x%lx", (unsigned long)lr);
  if (lr >= tb && lr < tb + text_size) fprintf(stderr, " (lr unity+0x%lx)", lr - tb);
  fprintf(stderr, " ===\n"); dbg_sync();
  for (int i = 0; i < 31; i++) {
    fprintf(stderr, " x%-2d=0x%016lx", i, (unsigned long)uc->uc_mcontext.regs[i]);
    if (i % 3 == 2) fprintf(stderr, "\n");
  }
  dbg_sync();
  /* stack scan limitado à região mapeada da pilha desta thread (evita ler além
     do fim do mapping e re-faultar dentro do handler). */
  fprintf(stderr, "[stack scan]\n");
  uintptr_t sp = uc->uc_mcontext.sp;
  uintptr_t slo = 0, shi = 0; char sperm[5];
  maps_find(sp, &slo, &shi, sperm);
  uintptr_t send = shi ? shi : sp + 400 * 8;
  for (uintptr_t a = sp, hits = 0; a + 8 <= send && hits < 32; a += 8) {
    uintptr_t v = *(uintptr_t *)a;
    if (v >= tb && v < tb + text_size) { fprintf(stderr, "  [sp+0x%lx] libunity+0x%lx\n", a - sp, v - tb); hits++; }
    else if (g_il2cpp_base && v >= g_il2cpp_base && v < g_il2cpp_base + 0x3000000)
      { fprintf(stderr, "  [sp+0x%lx] libil2cpp+0x%lx\n", a - sp, v - g_il2cpp_base); hits++; }
  }
  dbg_sync();
  /* 🔎 unwind por frame-pointer (x29): [x29]=próximo x29, [x29+8]=lr salvo.
     Reconstrói o backtrace REAL mesmo com pc=0/lr=0 (call-site imediato perdido). */
  {
    fprintf(stderr, "[FP] backtrace via x29:\n");
    uintptr_t fp = uc->uc_mcontext.regs[29];
    for (int i = 0; i < 24 && fp; i++) {
      uintptr_t flo=0, fhi=0; char fperm[5]; maps_find(fp, &flo, &fhi, fperm);
      if (!fhi || fp + 16 > fhi) break;
      uintptr_t nfp = *(uintptr_t *)fp, ret = *(uintptr_t *)(fp + 8);
      fprintf(stderr, "[FP]  #%d ret=0x%lx", i, (unsigned long)ret);
      if (ret >= tb && ret < tb + text_size) fprintf(stderr, " libunity+0x%lx", ret - tb);
      else if (g_il2cpp_base && ret >= g_il2cpp_base && ret < g_il2cpp_base + 0x3000000)
        fprintf(stderr, " libil2cpp+0x%lx", ret - g_il2cpp_base);
      fprintf(stderr, "\n");
      if (nfp <= fp || nfp - fp > 0x100000) break;  /* cadeia inválida */
      fp = nfp;
    }
    dbg_sync();
  }

  /* ---- dump rico do crash 0x7f10000004 (vtable/delegate corrompido) ---- */
  uintptr_t fault = (uintptr_t)si->si_addr;
  fprintf(stderr, "[CR] ==== diagnóstico de corrupção ====\n");
  crash_classify("pc", pc);
  crash_classify("fault", fault);
  /* região do ponteiro-lixo (pc=0x7f10000004): o que é 0x7f10000000? */
  crash_classify("pc_region", pc & ~0xFFFUL);
  crash_dump_qwords("pc_target", pc & ~0xFUL, 8);
  /* singleton: *(libunity_data + 0xd18) → método[0] foi p/ o lixo */
  if (g_unity_data) {
    uintptr_t pslot = g_unity_data + 0xd18;
    crash_classify("singleton_slot(d18)", pslot);
    if (addr_readable(pslot)) {
      uintptr_t sgl = *(uintptr_t *)pslot;
      crash_classify("singleton_obj", sgl);
      crash_dump_qwords("singleton", sgl, 16);
    }
  }
  /* dispatcher std::function/delegate: x19 é o objeto; lê [x19+248/256/264] */
  uintptr_t x19 = uc->uc_mcontext.regs[19];
  crash_classify("x19(dispatch_obj)", x19);
  crash_dump_qwords("x19", x19, 40);   /* cobre +0..+312 (inclui 248/256/264) */
  /* x8 = ponteiro de função chamado (= pc no blr x8); x21 = this provável */
  crash_classify("x8", uc->uc_mcontext.regs[8]);
  crash_classify("x21", uc->uc_mcontext.regs[21]);
  /* x20/x22/x23/x24: candidatos a 'this'/objeto pai */
  crash_classify("x20", uc->uc_mcontext.regs[20]);
  crash_classify("x22", uc->uc_mcontext.regs[22]);
  /* SITE DA CHAMADA: lr = retorno após o `blr` que pulou p/ 0x7f10000004.
     Classifica lr e dumpa as 4 instruções em lr-12..lr (acha o blr Xn + o ldr
     que carregou o ponteiro lixo: revela DE ONDE vem 0x7f10000004). */
  crash_classify("lr(call-site)", lr);
  if (addr_readable((lr - 16) & ~0x3UL)) {
    fprintf(stderr, "[CR] insns @lr-16..lr:\n");
    for (uintptr_t a = (lr - 16) & ~0x3UL; a <= lr; a += 4)
      fprintf(stderr, "[CR]   0x%lx: %08x%s\n", (unsigned long)a,
              *(uint32_t *)a, a == lr - 4 ? "  <- blr (chamou o lixo)" : "");
    dbg_sync();
  }
  /* alvo dos ponteiros da singleton (campos = 0x7f..cXX espaçados 4B): o que há lá? */
  if (g_unity_data && addr_readable(g_unity_data + 0xd18)) {
    uintptr_t sgl = *(uintptr_t *)(g_unity_data + 0xd18);
    if (addr_readable(sgl)) {
      uintptr_t tgt = *(uintptr_t *)sgl;       /* singleton[0] = 1º ponteiro */
      crash_classify("singleton[0]_target", tgt);
      crash_dump_qwords("sgl[0]_tgt", tgt & ~0xFUL, 8);
    }
  }
  /* x3/x9/x27: ponteiros 0x7f14.. recorrentes — que região? */
  crash_classify("x3", uc->uc_mcontext.regs[3]);
  crash_classify("x9", uc->uc_mcontext.regs[9]);
  crash_classify("x17", uc->uc_mcontext.regs[17]);
  fprintf(stderr, "[CR] ==== fim ====\n");
  dbg_sync();
  _exit(128 + sig);
}

/* ---------- overrides bionic->glibc (do re4) ---------- */
/* sysconf: Unity lê _SC_* com constantes BIONIC (≠ glibc) → page/nproc/phys errados. */
static long my_sysconf(int name) {
  int ncpu = getenv("CUP_1CORE") ? 1 : 4;
  switch (name) {
    case 39: case 40: return 4096;                 /* _SC_PAGE_SIZE/_SC_PAGESIZE bionic */
    case 6: return 100;                            /* _SC_CLK_TCK */
    case 96: case 97: return ncpu;                 /* _SC_NPROCESSORS_CONF/_ONLN (1 core => Unity desliga MT rendering) */
    case 98: return (512L*1024*1024)/4096;         /* _SC_PHYS_PAGES -> 512MB */
    case 99: return (256L*1024*1024)/4096;         /* _SC_AVPHYS_PAGES -> 256MB */
  }
  long r = sysconf(name);
  if ((name == _SC_PHYS_PAGES || name == _SC_AVPHYS_PAGES) && r <= 0)
    r = (512L*1024*1024)/4096;
  return r;
}
/* TER_JOBINLINE: faz o Unity ver 1 CPU lógica → cria 0 job-workers → o native job system
   roda jobs INLINE na própria thread (sem worker). Resolve o deadlock do boot (a main agenda
   jobs e espera workers que nunca executam: completed-counter 0xc10360 fica 0). hardware_concurrency
   da glibc usa sched_getaffinity → forçamos máscara de 1 CPU. */
static int my_sched_getaffinity(int pid, size_t setsize, void *mask) {
  (void)pid;
  if (mask && setsize >= sizeof(unsigned long)) {
    memset(mask, 0, setsize);
    *(unsigned long *)mask = 1UL;   /* só CPU 0 */
    return 0;
  }
  return -1;
}
/* mmap spy: a arena de 2MB @ 0x7f10000000 (onde os vtables corrompidos apontam)
 * é um mmap de 0x200000. Logamos alocações desse tamanho + o caller (RA→libunity/
 * il2cpp offset) p/ identificar QUAL alocador/subsistema cria a arena. CUP_MMAPLOG. */
static int g_mmaplog;
extern void *mmap(void *, size_t, int, int, int, long);  /* glibc real */
static void *my_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
  void *r = mmap(addr, len, prot, flags, fd, off);
  if (g_mmaplog && (len == 0x200000 || (len >= 0x100000 && len <= 0x400000))) {
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    const char *lib = "?"; uintptr_t off2 = ra;
    if (g_unity_base && ra >= g_unity_base && ra < g_unity_base + text_size) { lib = "libunity"; off2 = ra - g_unity_base; }
    else if (g_il2cpp_base && ra >= g_il2cpp_base && ra < g_il2cpp_base + 0x3000000) { lib = "libil2cpp"; off2 = ra - g_il2cpp_base; }
    fprintf(stderr, "[MMAP] len=0x%zx prot=%d -> %p  caller=%s+0x%lx\n",
            len, prot, r, lib, (unsigned long)off2);
    fsync(2);
  }
  return r;
}
/* /proc/cpuinfo + /sys/.../cpu: Unity conta cores p/ dimensionar job workers. */
static int g_dllog;
static const char *asset_redirect(const char *p, char *buf, size_t bufsz);
static FILE *my_fopen(const char *p, const char *m) {
  if (p && !strcmp(p, "/proc/meminfo")) {
    FILE *t = tmpfile(); if (t) { fputs("MemTotal:      524288 kB\nMemFree:       262144 kB\nMemAvailable:  262144 kB\n", t); rewind(t); return t; }
  }
  if (p && (!strcmp(p, "/sys/devices/system/cpu/possible") || !strcmp(p, "/sys/devices/system/cpu/present") || !strcmp(p, "/sys/devices/system/cpu/online"))) {
    FILE *t = tmpfile(); if (t) { fputs(getenv("CUP_1CORE") ? "0\n" : "0-3\n", t); rewind(t); return t; }
  }
  char rb[512]; const char *r = asset_redirect(p, rb, sizeof rb);
  if (r) {
    if (g_dllog) fprintf(stderr, "[fopen-redir] %s -> %s\n", p, r);
    return fopen(r, m);
  }
  return fopen(p, m);
}
static int my_fclose(FILE *f) {
  static int n;
  if (n++ < 24) {
    fprintf(stderr, "[FCLOSE] skip fclose(%p)\n", (void *)f);
    fsync(2);
  }
  return 0;
}
#define ASSET_BASE_M "/storage/roms/ports/rockmanxdive/"
static const char *rxd_assetpack_file_path(const char *name, char *buf, size_t bufsz) {
  if (!name || !*name) return NULL;
  static const char *fmts[] = {
    ASSET_BASE_M "assetpack/%s",
    ASSET_BASE_M "files/assetpacks/package1/18/18/assets/assetpack/%s",
    ASSET_BASE_M "files/assetpacks/package2/18/18/assets/assetpack/%s",
    ASSET_BASE_M "files/assetpacks/package3/18/18/assets/assetpack/%s",
    ASSET_BASE_M "files/assetpacks/package4/18/18/assets/assetpack/%s",
  };
  for (unsigned i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
    snprintf(buf, bufsz, fmts[i], name);
    if (access(buf, F_OK) == 0) {
      static unsigned n;
      if (i && n++ < 80) {
        fprintf(stderr, "[RXD_ABPACK] %s -> %s\n", name, buf);
        fsync(2);
      }
      return buf;
    }
  }
  return NULL;
}
/* redirect genérico de assets: o engine monta paths de dados com bases erradas
   (APK inexistente, filesdir). Mapeia qualquer tentativa p/ os arquivos REAIS
   deployados em bin/Data (mesma receita do global-metadata.dat, generalizada:
   pega o sufixo após "bin/Data/", senão o basename de arquivos conhecidos do
   engine — globalgamemanagers, level*, sharedassets*, *.assets/.resS/.resource). */
static const char *asset_redirect(const char *p, char *buf, size_t bufsz) {
  if (!p) return NULL;
  /* /data/local/tmp -> /tmp (writable tmpfs). O jogo faz um CASESENSITIVETEST criando
     um arquivo em /data/local/tmp; nosso / é squashfs RO e /data nem existe -> a criação
     falha -> exceção C++ -> (dl_iterate_phdr stubado) std::terminate -> abort. Redireciona
     pro /tmp gravável. SEM access-check (é p/ CRIAR arquivo novo). */
  if (!strncmp(p, "/data/local/tmp", 15)) {
    snprintf(buf, bufsz, "/tmp%s", p + 15);
    return buf;
  }
  /* TER_1CPU: Unity lê /sys/devices/system/cpu/{present,possible,online} p/ contar cores e
     cria (nº cores - 1) Job.Worker threads. O job-system NÃO despacha trabalho pros workers no
     nosso so-loader (eles ficam ociosos; main trava em WaitForJobGroup, counter=0). Reportando
     1 core (string "0"), Unity cria 0 Job.Worker → roda os jobs INLINE na própria thread. */
  if (getenv("TER_1CPU") && !strncmp(p, "/sys/devices/system/cpu/", 24)) {
    const char *leaf = p + 24;
    if (!strcmp(leaf, "present") || !strcmp(leaf, "possible") || !strcmp(leaf, "online")) {
      static const char *fake = "/tmp/ter_cpu0";
      int fd = open(fake, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd >= 0) { if (write(fd, "0\n", 2) < 0) {} close(fd); }
      snprintf(buf, bufsz, "%s", fake);
      return buf;
    }
  }
  /* QUALQUER path .../AssetBundles/<nome> -> /storage/cuphead-sa/AssetBundles/<nome>.
     Resolve o load do DLC (base-path vinha lixo: "Шестигранные врата 1/AssetBundles/..")
     e qualquer base estranha; o path correto redireciona p/ si mesmo (anti-loop). */
  const char *rxdab = strstr(p, "AssetBundles/Android");
  if (rxdab) {
    const char *tail = rxdab + strlen("AssetBundles/Android");
    while (*tail == '/' || *tail == '\\') tail++;
    if (!*tail) {
      snprintf(buf, bufsz, ASSET_BASE_M "Android");
      if (access(buf, F_OK) == 0) return buf;
    } else {
      const char *ap = rxd_assetpack_file_path(tail, buf, bufsz);
      if (ap) return ap;
      snprintf(buf, bufsz, ASSET_BASE_M "%s", tail);
      if (access(buf, F_OK) == 0) return buf;
    }
  }
  const char *ab = strstr(p, "/AssetBundles/");
  /* path RELATIVO "AssetBundles/<nome>" (cutscene do livro monta sem base) */
  if (!ab && !strncmp(p, "AssetBundles/", 13)) ab = p - 1;
  if (ab) {
    const char *sap = getenv("CUP_SAPATH"); if (!sap) sap = "/storage/cuphead-sa";
    snprintf(buf, bufsz, "%s/AssetBundles/%s", sap, ab + 14);
    if (strcmp(buf, p) != 0 && access(buf, F_OK) == 0) return buf;
    return NULL;
  }
  /* anti-loop: só pula o que JÁ aponta pro alvo (bin/Data real); paths de
     userdata/ sob a base ainda precisam de redirect (il2cpp/Metadata) */
  if (!strncmp(p, ASSET_BASE_M "bin/Data/", sizeof(ASSET_BASE_M "bin/Data/") - 1)) return NULL;
  if (!strncmp(p, ASSET_BASE_M "userdata/", sizeof(ASSET_BASE_M "userdata/") - 1) &&
      access(p, F_OK) == 0) {
    return NULL;
  }
  const char *sub = strstr(p, "bin/Data/");
  if (sub) {
    snprintf(buf, bufsz, ASSET_BASE_M "bin/Data/%s", sub + 9);
    if (access(buf, F_OK) == 0) return buf;
  }
  const char *base = strrchr(p, '/'); base = base ? base + 1 : p;
  if (!strcmp(base, "Android") || !strcmp(base, "Android.manifest") || !strcmp(base, "abconfig")) {
    snprintf(buf, bufsz, ASSET_BASE_M "%s", base);
    if (access(buf, F_OK) == 0) return buf;
  }
  if (strlen(base) == 32) {
    int hex = 1;
    for (const char *c = base; *c; c++) {
      if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F'))) {
        hex = 0; break;
      }
    }
    if (hex) {
      const char *ap = rxd_assetpack_file_path(base, buf, bufsz);
      if (ap) return ap;
    }
  }
  if (!strcmp(base, "global-metadata.dat")) {
    snprintf(buf, bufsz, ASSET_BASE_M "bin/Data/Managed/Metadata/global-metadata.dat");
    return buf;
  }
  /* il2cpp procura <userdata>/il2cpp/Resources/*-resources.dat */
  if (strstr(base, "-resources.dat")) {
    snprintf(buf, bufsz, ASSET_BASE_M "bin/Data/Managed/Resources/%s", base);
    if (access(buf, F_OK) == 0) return buf;
  }
  if (!strncmp(base, "level", 5) || !strncmp(base, "sharedassets", 12) ||
      !strncmp(base, "globalgamemanagers", 18) || strstr(base, ".assets") ||
      strstr(base, ".resS") || strstr(base, ".resource") ||
      !strcmp(base, "data.unity3d") || !strcmp(base, "boot.config") ||
      !strcmp(base, "unity default resources") || !strcmp(base, "unity_builtin_extra")) {
    snprintf(buf, bufsz, ASSET_BASE_M "bin/Data/%s", base);
    if (access(buf, F_OK) == 0) return buf;
    snprintf(buf, bufsz, ASSET_BASE_M "bin/Data/Resources/%s", base);
    if (access(buf, F_OK) == 0) return buf;
  }
  return NULL;
}
/* command line do Unity: lido de /proc/<pid>/cmdline (args separados por \0).
   Injeta -force-gfx-st (single-threaded GFX) p/ matar o GfxDeviceWorker e o
   deadlock main<->worker no boot. CUP_GFXARGS sobrescreve. */
static int cmdline_fd(void) {
  const char *extra = getenv("CUP_GFXARGS");
  char buf[256]; int n = 0;
  n += sprintf(buf + n, "rockmanxdive") + 1;
  if (extra && *extra) {
    /* CUP_GFXARGS="-a -b" -> cada token \0-terminado */
    char tmp[200]; strncpy(tmp, extra, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
    for (char *t = strtok(tmp, " "); t; t = strtok(NULL, " ")) n += sprintf(buf + n, "%s", t) + 1;
  } else {
    /* -force-gfx-direct = render DIRETO na main thread (sem GfxDeviceWorker). O nome antigo
       "-force-gfx-st" NÃO é arg real do Unity (era ignorado → worker MT continuava vivo →
       deadlock main<->worker no boot). */
    n += sprintf(buf + n, "-force-gfx-direct") + 1;
    n += sprintf(buf + n, "-force-gles20") + 1;
  }
  FILE *t = tmpfile();
  if (!t) return -1;
  fwrite(buf, 1, n, t); fflush(t);
  int fd = dup(fileno(t)); fclose(t); lseek(fd, 0, SEEK_SET);
  fprintf(stderr, "[CMDLINE] injetado (%d bytes): force-gfx-st\n", n);
  return fd;
}
/* TER_GUIDLOG: rastreia o fd do unity_app_guid p/ ver COMO o engine lê (read/
 * lseek/fstat/mmap/close) — diagnóstico do "guid is empty". */
static int g_guidlog;
static int g_guid_fd = -1;
static int my_open(const char *p, int fl, ...) {
  if (env_on("TER_NO_SYMOPEN") && p &&
      (!strcmp(p, ASSET_BASE_M "rockmanxdive") || strstr(p, "/libc.so") ||
       strstr(p, "/libpthread.so") || strstr(p, "/ld-linux"))) {
    if (g_dllog) fprintf(stderr, "[open-SYM-BLOCK] %s\n", p);
    errno = ENOENT;
    return -1;
  }
  if (p && !strcmp(p, "/proc/cpuinfo")) {
    int nc = getenv("CUP_1CORE") ? 1 : 4;
    FILE *t = tmpfile();
    if (t) { for (int i = 0; i < nc; i++) fprintf(t, "processor\t: %d\nCPU implementer\t: 0x41\nCPU architecture: 8\n\n", i);
      fflush(t); int fd = dup(fileno(t)); fclose(t); lseek(fd, 0, SEEK_SET); return fd; }
  }
  if (p && strstr(p, "cmdline") && !getenv("CUP_NOGFXARGS")) return cmdline_fd();
  char rb[512];
  const char *r = asset_redirect(p, rb, sizeof rb);
  if (r) {
    int rmode = 0;
    if (fl & O_CREAT) { va_list ap; va_start(ap, fl); rmode = va_arg(ap, int); va_end(ap); }
    int fd = open(r, fl, rmode);
    if (g_dllog) fprintf(stderr, "[open-redir%s] %s -> %s\n", fd < 0 ? "-MISS" : "", p, r);
    if (g_guidlog && p && strstr(p, "unity_app_guid")) {
      g_guid_fd = fd;
      struct stat sb; int sr = fstat(fd, &sb);
      fprintf(stderr, "[GUID] open '%s' fl=0x%x -> fd=%d (fstat rc=%d st_size=%lld)\n",
              p, fl, fd, sr, sr == 0 ? (long long)sb.st_size : -1LL);
      fsync(2);
    }
    return fd;
  }
  va_list ap; va_start(ap, fl); int mo = va_arg(ap, int); va_end(ap);
  int fd = open(p, fl, mo);
  if (g_dllog && p) fprintf(stderr, "[open%s] %s\n", fd < 0 ? "-MISS" : "", p);
  if (g_guidlog && p && strstr(p, "unity_app_guid")) {
    g_guid_fd = fd;
    fprintf(stderr, "[GUID] open(noredir) '%s' fl=0x%x -> fd=%d\n", p, fl, fd);
    fsync(2);
  }
  return fd;
}
extern ssize_t read(int, void *, size_t);
extern off64_t lseek64(int, off64_t, int);
extern int fstat64(int, struct stat64 *);
extern void *mmap64(void *, size_t, int, int, int, off64_t);
static ssize_t my_read(int fd, void *buf, size_t n) {
  ssize_t r = read(fd, buf, n);
  if (g_guidlog && fd == g_guid_fd) {
    fprintf(stderr, "[GUID] read(fd=%d, n=%zu) -> %zd  first='%.40s'\n",
            fd, n, r, r > 0 ? (char *)buf : "");
    fsync(2);
  }
  return r;
}
static off64_t my_lseek64(int fd, off64_t off, int wh) {
  off64_t r = lseek64(fd, off, wh);
  if (g_guidlog && fd == g_guid_fd)
    fprintf(stderr, "[GUID] lseek64(fd=%d, off=%lld, wh=%d) -> %lld\n",
            fd, (long long)off, wh, (long long)r), fsync(2);
  return r;
}
static int my_fstat64(int fd, struct stat64 *st) {
  int r = fstat64(fd, st);
  if (g_guidlog && fd == g_guid_fd)
    fprintf(stderr, "[GUID] fstat64(fd=%d) -> rc=%d st_size=%lld\n",
            fd, r, r == 0 ? (long long)st->st_size : -1LL), fsync(2);
  return r;
}
static void *my_mmap64(void *a, size_t len, int prot, int fl, int fd, off64_t off) {
  void *r = mmap64(a, len, prot, fl, fd, off);
  if (g_guidlog && fd == g_guid_fd)
    fprintf(stderr, "[GUID] mmap64(fd=%d, len=%zu, off=%lld) -> %p  first='%.40s'\n",
            fd, len, (long long)off, r,
            (r && r != MAP_FAILED && (prot & PROT_READ)) ? (char *)r : ""), fsync(2);
  return r;
}
static FILE *my_fdopen(int fd, const char *mode) {
  FILE *r = fdopen(fd, mode);
  if (g_guidlog && fd == g_guid_fd)
    fprintf(stderr, "[GUID] fdopen(fd=%d, '%s') -> %p\n", fd, mode ? mode : "?", (void *)r), fsync(2);
  return r;
}
/* stat/lstat/access com o mesmo redirect — o engine checa existência antes de
   abrir ("No GlobalGameManagers file" pode vir de um stat, não do open).
   Layout de struct stat arm64 = kernel em bionic E glibc → pass-through ok. */
static int my_stat(const char *p, void *st) {
  char rb[512]; const char *r = asset_redirect(p, rb, sizeof rb);
  if (r && g_dllog) fprintf(stderr, "[stat-redir] %s -> %s\n", p, r);
  int rc = stat(r ? r : p, (struct stat *)st);
  if (g_dllog && rc < 0 && p) fprintf(stderr, "[stat-MISS] %s\n", p);
  return rc;
}
static int my_lstat(const char *p, void *st) {
  char rb[512]; const char *r = asset_redirect(p, rb, sizeof rb);
  return lstat(r ? r : p, (struct stat *)st);
}
/* 🔑 stat64: libunity importa stat64 (NÃO stat). O leitor de arquivos (ReadAllBytes
   @0x21db60 -> GetFileSize @0x22b7c0) pega o TAMANHO via stat64(path); sem redirect,
   o path "assets/bin/Data/unity_app_guid" não existe em disco -> stat64 falha -> size 0
   -> lê 0 bytes -> guid "is empty" -> re-extract -> "Unable to initialize". O open()
   funcionava (redirecionado) mas o size não. arm64: struct stat == struct stat64. */
static int my_stat64(const char *p, void *st) {
  char rb[512]; const char *r = asset_redirect(p, rb, sizeof rb);
  if (r && g_dllog) fprintf(stderr, "[stat64-redir] %s -> %s\n", p, r);
  int rc = stat64(r ? r : p, (struct stat64 *)st);
  if (g_dllog && rc < 0 && p) fprintf(stderr, "[stat64-MISS] %s\n", p);
  return rc;
}
static int my_lstat64(const char *p, void *st) {
  char rb[512]; const char *r = asset_redirect(p, rb, sizeof rb);
  return lstat64(r ? r : p, (struct stat64 *)st);
}
/* === Enlighten allocator (GI) === FIX do null-deref no HLRTManager/GeoArray.
 * O allocator do Enlighten é um singleton em libunity+0xc886a0. init-A (0x32ea38) instala
 * um allocator VÁLIDO no boot (confirmado: SetMemoryManager(0x7f60...)), MAS algo o ZERA
 * (teardown-B 0x32ec10 = ÚNICO outro writer) antes da criação do HLRTManager (realtime GI da
 * cena 2D). Com singleton NULL, o wrapper de alloc (0x861928) retorna NULL -> ctor do GeoArray
 * faz `str x8,[NULL]` -> SIGSEGV. FIX: substituir o wrapper 0x861928 (52B, cabe trampolim 16B)
 * por my_enl_alloc: usa o allocator REAL quando o singleton é válido (idêntico ao original) e
 * cai p/ posix_memalign quando NULL (evita o crash). */
static int g_enllog;
extern void so_make_text_writable(void), so_make_text_executable(void), so_flush_caches(void);
static void patch_tramp(uintptr_t off, void *fn) {
  uint32_t *p = (uint32_t *)(g_unity_base + off);
  so_make_text_writable();
  p[0] = 0x58000050u;            /* ldr x16, #8  (carrega o .quad abaixo) */
  p[1] = 0xd61f0200u;            /* br  x16      */
  *(uint64_t *)(p + 2) = (uint64_t)fn;  /* .quad fn (ocupa p[2],p[3]) */
  so_make_text_executable(); so_flush_caches();
}
/* assinatura na entrada de 0x861928: (w0=size, w1=align, x2=a2, w3=label, x4=name) -> ptr */
static void *my_enl_alloc(unsigned long size, unsigned long align, void *a2, int label, void *name) {
  void *mm = g_unity_base ? *(void **)(g_unity_base + 0xc886a0) : 0;
  void *r = 0;
  if (mm) {
    /* allocator REAL: vtable[+0x10](this, size, align, a2, label, name) — idêntico ao original */
    void *vt = *(void **)mm;
    void *(*real)(void *, unsigned long, unsigned long, void *, int, void *) =
        *(void *(**)(void *, unsigned long, unsigned long, void *, int, void *))((char *)vt + 0x10);
    r = real(mm, size, align, a2, label, name);
  }
  if (!r) {
    /* singleton NULL OU allocator real devolveu NULL: fallback malloc alinhado (evita o crash) */
    if (align < 8 || (align & (align - 1))) align = 16;
    void *p = NULL;
    if (posix_memalign(&p, align, size ? size : 1) == 0) r = p;
  }
  if (g_enllog) { fprintf(stderr, "[ENL] alloc size=%lu align=%lu label=%d mm=%p -> %p\n", size, align, label, mm, r); fsync(2); }
  return r;
}
static int my_access(const char *p, int m) {
  char rb[512]; const char *r = asset_redirect(p, rb, sizeof rb);
  if (r && g_dllog) fprintf(stderr, "[access-redir] %s -> %s\n", p, r);
  return access(r ? r : p, m);
}
/* statfs64: Unity checa espaço livre via statfs64(path) p/ "instalar resources".
   O path que ele passa pode ser Android (/data/...) inexistente -> erro -> 0 livre ->
   "Not enough storage space". Ignoramos o path e medimos o NOSSO GAMEDIR real (93GB).
   glibc preenche o buffer no layout do kernel statfs64 = o que o bionic espera. */
/* FORTIFY do bionic (__*_chk): glibc não tem esses símbolos -> viram stub (NÃO copiam)
   -> corrupção de heap. Implementações reais (ignoram o arg de bounds-check). */
static void *my_memmove_chk(void *d, const void *s, size_t n, size_t dn) { (void)dn; return memmove(d, s, n); }
static void *my_memcpy_chk(void *d, const void *s, size_t n, size_t dn) { (void)dn; return memcpy(d, s, n); }
static void *my_memset_chk(void *d, int c, size_t n, size_t dn) { (void)dn; return memset(d, c, n); }
static size_t my_strlen_chk(const char *s, size_t mn) { (void)mn; return strlen(s); }
static char *my_strcpy_chk(char *d, const char *s, size_t dn) { (void)dn; return strcpy(d, s); }
static char *my_strcat_chk(char *d, const char *s, size_t dn) { (void)dn; return strcat(d, s); }
static int my_vsnprintf_chk(char *str, size_t sz, int flag, size_t slen, const char *fmt, va_list ap) {
  (void)flag; (void)slen; return vsnprintf(str, sz, fmt, ap); }
static int my_snprintf_chk(char *str, size_t sz, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen; va_list ap; va_start(ap, fmt); int r = vsnprintf(str, sz, fmt, ap); va_end(ap); return r; }
static void my_FD_SET_chk(int fd, fd_set *s, size_t n) { (void)n; if (fd >= 0) FD_SET(fd, s); }
/* strlcpy/strlcat (bionic) — o regex de passthrough não cobre (vira stub que NÃO copia
   -> buffer com lixo -> heap corruption "free(): invalid size"). Implementação real. */
static unsigned long my_strlcpy(char *dst, const char *src, unsigned long sz) {
  unsigned long n = strlen(src);
  if (sz) { unsigned long c = n < sz - 1 ? n : sz - 1; memcpy(dst, src, c); dst[c] = 0; }
  return n;
}
/* 🔑 memalign: estava STUBADO (única fn de alloc não-passthrough) -> retornava NULL.
   libunity E libil2cpp importam memalign; o allocator do Enlighten (GI) usa memalign p/
   memória alinhada -> NULL -> ctor do HLRTManager/GeoArray recebe `this`=NULL -> SIGSEGV.
   Impl real via posix_memalign (memalign do glibc é deprecated). Alinhamento >= sizeof(void*)
   e potência de 2 (exigência do posix_memalign). */
/* 🔑 syscall: estava STUBADO (retornava 0). O job-system do Unity usa `syscall(SYS_futex,
   FUTEX_WAKE)` CRU p/ acordar a main thread quando um job termina; com o stub no-op, o
   futex_wake nunca acontece -> a main (presa em futex_wait via glibc pthread no nativeRender
   do frame 2) DORME P/ SEMPRE e as Job.Worker/Background ficam em busy-spin no stub. arm64:
   números de syscall são IGUAIS em bionic/glibc/kernel -> forward direto é seguro. */
extern long syscall(long, ...);
/* TER_FUTEXPOLL=ms: defesa GERAL contra lost-wakeup no job-system do Unity. As Job.Worker/
   Background usam `syscall(SYS_futex, FUTEX_WAIT)` CRU (não passam pelo nosso sem/cond shim,
   então CUP_SEMPOLL/CONDPOLL não as alcançam). Se a main enfileira trabalho mas perde o
   FUTEX_WAKE, o worker dorme p/ sempre e a main trava esperando o job. Aqui injetamos um
   TIMEOUT curto nas esperas de futex SEM timeout → o waiter acorda periodicamente, re-checa
   seu predicado e re-espera. Cobre TODA a sincronização por futex. */
static long g_futexpoll_ms = 0;
#ifndef SYS_futex
#define SYS_futex 98
#endif
extern void gc_wait_unblock(void *oldp);   /* pthread_fake.c: desbloqueia SIGPWR/SIGXCPU no wait */
extern void gc_wait_restore(void *oldp);
#ifndef SYS_rt_sigprocmask
#define SYS_rt_sigprocmask 135
#endif
static long my_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
  /* 🔑 As threads do GC (Finalizer/Loading, bionic-static) BLOQUEIAM SIGPWR(30)/SIGXCPU(24) via
     rt_sigprocmask CRU (não passam pelos nossos shims pthread). Com SIGPWR bloqueado, o stop-the-world
     do GC nunca consegue suspendê-las → deadlock. Aqui interceptamos o rt_sigprocmask e TIRAMOS
     SIGPWR/SIGXCPU de qualquer BLOCK/SETMASK → toda thread fica suspendível pelo GC. */
  if (!getenv("TER_NORTFILTER") && n == SYS_rt_sigprocmask && a2 &&
      (a1 == 0 /*SIG_BLOCK*/ || a1 == 2 /*SIG_SETMASK*/)) {
    unsigned long m = *(const unsigned long *)a2;
    unsigned long m2 = m & ~((1UL << 29) | (1UL << 23));   /* limpa SIGPWR(bit29)/SIGXCPU(bit23) */
    if (m2 != m) {
      static __thread unsigned long copy;   /* per-thread, sobrevive à chamada */
      copy = m2;
      if (getenv("TER_RTLOG")) { static int rn; if (rn++ < 20) { fprintf(stderr, "[RTMASK] how=%ld 0x%lx->0x%lx\n", a1, m, m2); fsync(2); } }
      return syscall(n, a1, (long)&copy, a3, a4, a5, a6);
    }
  }
  if (n == 123 /*SYS_sched_getaffinity arm64*/ && getenv("TER_JOBINLINE") && a3) {
    long r = syscall(n, a1, a2, a3, a4, a5, a6);
    if (r > 0) { memset((void *)a3, 0, (size_t)a2); *(unsigned long *)a3 = 1UL; }
    return r > 0 ? r : (memset((void *)a3, 0, 8), *(unsigned long *)a3 = 1UL, 8);
  }
  if (n == SYS_futex) {
    int op = (int)a2 & 0x7f;
    if (getenv("TER_FUTEXLOG") && (op == 0 || op == 9 || op == 1 || op == 10)) {
      /* op 0/9=WAIT, 1/10=WAKE. Loga (tid,comm,uaddr,op). WAIT dedup por (tid,uaddr);
         WAKE loga todos (raro, e é o que queremos ver: alguém acorda o uaddr do worker?). */
      int isw = (op == 0 || op == 9);
      int tid = (int)syscall(178 /*arm64 gettid*/);
      int show = 1;
      if (isw) { static struct { int tid; long ua; } seen[200]; static int ns;
        for (int i = 0; i < ns; i++) if (seen[i].tid == tid && seen[i].ua == a1) { show = 0; break; }
        if (show && ns < 200) { seen[ns].tid = tid; seen[ns].ua = a1; ns++; } }
      else { static int wn; if (wn++ > 400) show = 0; }
      if (show) {
        char comm[20] = ""; FILE *f = fopen("/proc/self/comm", "r"); if (f) { if (fgets(comm, sizeof comm, f)) { char *nl = strchr(comm, '\n'); if (nl) *nl = 0; } fclose(f); }
        fprintf(stderr, "[FX] %s tid=%d(%s) uaddr=%p val=%ld\n", isw ? "WAIT" : "WAKE", tid, comm, (void *)a1, a3); fsync(2);
      }
    }
    if (op == 0 || op == 9) {   /* FUTEX_WAIT / FUTEX_WAIT_BITSET: thread vai BLOQUEAR */
      long t4 = a4;
      struct timespec ts;
      if (g_futexpoll_ms && a4 == 0) {  /* injeta timeout (poll anti-lost-wakeup) */
        if (op == 0) { ts.tv_sec = g_futexpoll_ms / 1000; ts.tv_nsec = (g_futexpoll_ms % 1000) * 1000000L; }
        else { clock_gettime(((int)a2 & 256) ? CLOCK_REALTIME : CLOCK_MONOTONIC, &ts);
               ts.tv_sec += g_futexpoll_ms / 1000; ts.tv_nsec += (g_futexpoll_ms % 1000) * 1000000L;
               if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; } }
        t4 = (long)&ts;
      }
      /* 🔑 GC-SAFE: o futex wait é um ponto seguro → desbloqueia SIGPWR/SIGXCPU em volta dele p/ o
         stop-the-world do GC conseguir suspender ESTA thread (que bloqueia SIGPWR) enquanto está
         parada aqui. Sem isso, o GC manda SIGPWR, fica bloqueado, e WaitForThreadsToSuspend trava. */
      char old[128]; gc_wait_unblock(old);
      long r = syscall(n, a1, a2, a3, t4, a5, a6);
      gc_wait_restore(old);
      return r;
    }
  }
  return syscall(n, a1, a2, a3, a4, a5, a6);
}
/* TER_PKLOG: loga pthread_kill (quem o GC sinaliza p/ suspender + qual sinal) — diagnóstico
   do stop-the-world travado (nenhuma thread dá ACK). */
extern int pthread_kill(pthread_t, int);
extern const char *ter_thread_comm(pthread_t t);
static int my_pthread_kill(pthread_t t, int sig) {
  static int n;
  if (getenv("TER_PKLOG") && n++ < 60) { fprintf(stderr, "[PKILL] -> %s sig=%d\n", ter_thread_comm(t), sig); fsync(2); }
  /* TER_NOSUSPEND: ENGOLE os sinais de stop-the-world do GC (SIGPWR=30 suspend / SIGXCPU=24 restart).
     Nenhuma thread é suspensa → o GC (com GCOFF, sem scan) só precisa que os WAITs retornem (NOGCWAIT
     + patch do restart-wait). Neutraliza o STW inteiro sem alcançar as threads bionic-static. */
  if (getenv("TER_NOSUSPEND") && (sig == 30 || sig == 24)) return 0;
  /* 🔑 TER_FAKEACK: a thread bionic-static que o GC quer suspender bloqueia SIGPWR e nunca dá ACK.
     O semáforo de ACK que o WaitForThreadsToSuspend espera é o NOSSO sem_shim em il2cpp+0x31666a0.
     Então POSTAMOS o sem no lugar da thread (fake ACK) + ENGOLIMOS o sinal (a thread não suspende) →
     o GC conta o ACK e segue o fluxo NORMAL (≠ NOGCWAIT). Usar com CUP_GCOFF (sem scan de stack viva). */
  if (getenv("TER_FAKEACK") && (sig == 30 || sig == 24) && g_il2cpp_base) {
    extern int sh_sem_post(void *);
    sh_sem_post((void *)(g_il2cpp_base + 0x31666a0));   /* ACK do suspend (sem do WaitForThreadsToSuspend) */
    return 0;
  }
  return pthread_kill(t, sig);
}
static void *my_memalign(unsigned long alignment, unsigned long size) {
  if (alignment < sizeof(void *)) alignment = sizeof(void *);
  if (alignment & (alignment - 1)) { unsigned long a = sizeof(void *); while (a < alignment) a <<= 1; alignment = a; }
  void *p = NULL;
  if (posix_memalign(&p, alignment, size ? size : 1) != 0) return NULL;
  return p;
}
static unsigned long my_strlcat(char *dst, const char *src, unsigned long sz) {
  unsigned long dl = strnlen(dst, sz), sl = strlen(src);
  if (dl == sz) return sz + sl;
  unsigned long c = (sl < sz - dl - 1) ? sl : sz - dl - 1;
  memcpy(dst + dl, src, c); dst[dl + c] = 0;
  return dl + sl;
}
static int my_statfs64(const char *p, void *buf) {
  static int (*real)(const char *, void *);
  if (!real) { real = (void *)dlsym(RTLD_DEFAULT, "statfs64");
               if (!real) real = (void *)dlsym(RTLD_DEFAULT, "statfs"); }
  int rc = real ? real("/storage/roms/ports/rockmanxdive", buf) : -1;
  if (g_dllog) fprintf(stderr, "[statfs64] path=%s -> GAMEDIR rc=%d\n", p ? p : "?", rc);
  return rc;
}
/* exit() do jogo: loga QUEM chamou (lr) + stack antes de morrer — a morte
   silenciosa pos-FMOD não deixava rastro. */
static void my_exit(int code) {
  fprintf(stderr, "[EXIT] exit(%d) chamado! lr=%p\n", code, __builtin_return_address(0));
  uintptr_t tb = (uintptr_t)g_unity_base;
  uintptr_t lr = (uintptr_t)__builtin_return_address(0);
  if (tb && lr >= tb) fprintf(stderr, "[EXIT] (libunity+0x%lx)\n", lr - tb);
  fsync(2);
  _exit(code);
}
/* __system_property_get: FMOD checa ro.build.version.sdk antes de usar OpenSLES
   (vazio→SDK 0→desiste sem nem dar dlsym; receita Dysmantle: "25"). Resto vazio. */
static int my_sysprop(const char *name, char *value) {
  if (!value) return 0;
  if (name && strstr(name, "version.sdk")) { strcpy(value, "25"); return 2; }
  value[0] = 0; return 0;
}
/* __android_log -> stderr */
static int my_alog_print(int prio, const char *tag, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); fprintf(stderr, "[ALOG:%d %s] ", prio, tag ? tag : "?");
  vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap); return 0;
}
static int my_alog_write(int prio, const char *tag, const char *msg) {
  fprintf(stderr, "[ALOG:%d %s] %s\n", prio, tag ? tag : "?", msg ? msg : ""); return 0;
}
/* __android_log_vprint é o canal do PLAYER LOG do Unity — jamais stubar */
static int my_alog_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  fprintf(stderr, "[ALOG:%d %s] ", prio, tag ? tag : "?");
  vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); return 0;
}
/* ANativeWindow: Unity espera window !=NULL (nativeRecreateGfxState) senão trava p/ sempre.
   Os egl* do libunity são imports PLT que resolvem no libEGL REAL do Mali (dlopen GLOBAL);
   no fbdev a EGLNativeWindowType é só struct {u16 w, u16 h} → entrega uma DE VERDADE e o
   Unity cria a window surface direto no fb0 (sem shim). CUP_SHIMEGL=1 volta pro fake int. */
static struct { unsigned short w, h; } g_fbdev_win = {0, 0};
static int cup_use_kmsdrm(void);  /* fwd: decide fbdev vs kmsdrm (def. abaixo) */
static int g_anw = 0xA11;
/* REGFX: o Unity compara o ptr da ANativeWindow no RecreateGfxState e faz
   early-out se for a MESMA (nao recria a swapchain destruida no f=2). Cada
   geracao > 0 devolve uma janela RECEM-ALOCADA (mesmos w/h) p/ forcar o
   caminho completo ChooseConfig+CreateWindowSurface no fb0. */
static volatile int g_anw_gen = 0;
static void *g_anw_gen_win = NULL;
static int g_anw_gen_done = -1;
static int ter_env_positive_int_main(const char *name) {
  const char *s = getenv(name);
  if (!s || !*s) return 0;
  char *end = NULL;
  long v = strtol(s, &end, 10);
  return (end != s && v > 0 && v < 32768) ? (int)v : 0;
}
static int ter_read_screen_pair_main(const char *path, int *w, int *h) {
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  char buf[128];
  int ok = fgets(buf, sizeof(buf), f) != NULL;
  fclose(f);
  if (!ok) return 0;
  int a = 0, b = 0;
  if (sscanf(buf, "%d,%d", &a, &b) != 2 &&
      sscanf(buf, "%dx%d", &a, &b) != 2 &&
      sscanf(buf, "%*[^0-9]%dx%d", &a, &b) != 2) return 0;
  if (a <= 0 || b <= 0 || a >= 32768 || b >= 32768) return 0;
  *w = a; *h = b;
  return 1;
}
static int ter_native_screen_size_main(int *w, int *h) {
  int sw = ter_env_positive_int_main("TER_SCREEN_W");
  int sh = ter_env_positive_int_main("TER_SCREEN_H");
  if (!sw) sw = ter_env_positive_int_main("TER_SCREEN_WIDTH");
  if (!sh) sh = ter_env_positive_int_main("TER_SCREEN_HEIGHT");
  if (sw > 0 && sh > 0) { *w = sw; *h = sh; return 1; }
  if (ter_read_screen_pair_main("/sys/class/graphics/fb0/mode", &sw, &sh) ||
      ter_read_screen_pair_main("/sys/class/graphics/fb0/modes", &sw, &sh) ||
      ter_read_screen_pair_main("/sys/class/graphics/fb0/virtual_size", &sw, &sh)) {
    *w = sw; *h = sh; return 1;
  }
  return 0;
}
static int ter_window_w(void) {
  int sw = 0, sh = 0;
  if (g_fbdev_win.w > 0) return g_fbdev_win.w;
  return ter_native_screen_size_main(&sw, &sh) ? sw : 0;
}
static int ter_window_h(void) {
  int sw = 0, sh = 0;
  if (g_fbdev_win.h > 0) return g_fbdev_win.h;
  return ter_native_screen_size_main(&sw, &sh) ? sh : 0;
}
static void *my_aw_fromSurface(void *e, void *s) { (void)e; (void)s;
  /* kmsdrm: ANativeWindow fake (egl_shim ignora a window). fbdev: struct {w,h} real. */
  if (cup_use_kmsdrm()) {
    fprintf(stderr, "[ANW] fromSurface kmsdrm surface=%p -> %p\n", s, (void *)&g_anw);
    fsync(2);
    return (void *)&g_anw;
  }
  int sw = ter_window_w();
  int sh = ter_window_h();
  if (sw > 0 && sh > 0) {
    g_fbdev_win.w = (unsigned short)sw;
    g_fbdev_win.h = (unsigned short)sh;
  }
  if (g_anw_gen > 0) {
    if (g_anw_gen_done != g_anw_gen || !g_anw_gen_win) {
      struct { unsigned short w, h; } *nw = malloc(sizeof *nw);
      if (nw) {
        nw->w = g_fbdev_win.w; nw->h = g_fbdev_win.h;
        g_anw_gen_win = nw;
        g_anw_gen_done = g_anw_gen;
      }
    }
    if (g_anw_gen_win) {
      fprintf(stderr, "[ANW] fromSurface surface=%p -> NOVA win gen=%d %p %ux%u\n",
              s, g_anw_gen, g_anw_gen_win, g_fbdev_win.w, g_fbdev_win.h);
      fsync(2);
      return g_anw_gen_win;
    }
  }
  fprintf(stderr, "[ANW] fromSurface surface=%p -> fbdev_win=%p %ux%u fallback=%dx%d\n",
          s, (void *)&g_fbdev_win, g_fbdev_win.w, g_fbdev_win.h, sw, sh);
  fsync(2);
  return (void *)&g_fbdev_win;
}
static int my_aw_setgeom(void *w, int a, int b, int c) {
  (void)c;
  if (a > 0 && b > 0) {
    g_fbdev_win.w = (unsigned short)a;
    g_fbdev_win.h = (unsigned short)b;
    if (w && w == g_anw_gen_win) {
      ((unsigned short *)w)[0] = (unsigned short)a;
      ((unsigned short *)w)[1] = (unsigned short)b;
    }
  }
  fprintf(stderr, "[ANW] setBuffersGeometry win=%p %dx%d fmt=%d -> %ux%u\n",
          w, a, b, c, g_fbdev_win.w, g_fbdev_win.h);
  fsync(2);
  return 0;
}
static int my_aw_getWidth(void *w) {
  int r = ter_window_w();
  static int n; if (n++ < 16) { fprintf(stderr, "[ANW] getWidth win=%p -> %d\n", w, r); fsync(2); }
  return r;
}
static int my_aw_getHeight(void *w) {
  int r = ter_window_h();
  static int n; if (n++ < 16) { fprintf(stderr, "[ANW] getHeight win=%p -> %d\n", w, r); fsync(2); }
  return r;
}
static int my_aw_getFormat(void *w) {
  static int n; if (n++ < 16) { fprintf(stderr, "[ANW] getFormat win=%p -> 1\n", w); fsync(2); }
  return 1;
}
static void my_aw_noop(void *w) {
  static int n; if (n++ < 16) { fprintf(stderr, "[ANW] acquire/release win=%p\n", w); fsync(2); }
}
/* dlopen/dlsym: Unity dlopen libGLESv2/EGL/OpenSLES + dlsym em runtime */
/* ---------- egl_shim (janela GLES2 via SDL2, proven re4) ---------- */
extern void egl_shim_create_window(void);
extern void *egl_shim_GetDisplay(void *);
extern unsigned egl_shim_Initialize(void *, int *, int *);
extern unsigned egl_shim_Terminate(void *);
extern unsigned egl_shim_ChooseConfig(void *, const int *, void **, int, int *);
extern void *egl_shim_CreateWindowSurface(void *, void *, void *, const int *);
extern void *egl_shim_CreatePbufferSurface(void *, void *, const int *);
extern void *egl_shim_CreateContext(void *, void *, void *, const int *);
extern unsigned egl_shim_MakeCurrent(void *, void *, void *, void *);
extern unsigned egl_shim_SwapBuffers(void *, void *);
extern unsigned egl_shim_DestroySurface(void *, void *);
extern unsigned egl_shim_DestroyContext(void *, void *);
extern unsigned egl_shim_QuerySurface(void *, void *, int, int *);
extern unsigned egl_shim_GetConfigAttrib(void *, void *, int, int *);
extern int egl_shim_GetError(void);
extern void *egl_shim_GetProcAddress(const char *);
extern unsigned egl_shim_BindAPI(unsigned);
extern const char *egl_shim_QueryString(void *, int);
extern unsigned egl_shim_SwapInterval(void *, int);
extern void *egl_shim_GetCurrentContext(void);
extern void *egl_shim_GetCurrentSurface(int);
extern unsigned egl_shim_SurfaceAttrib(void *, void *, int, int);
static void *egl_route(const char *nm) {
  struct { const char *n; void *f; } m[] = {
    {"eglGetDisplay", egl_shim_GetDisplay}, {"eglInitialize", egl_shim_Initialize},
    {"eglTerminate", egl_shim_Terminate}, {"eglChooseConfig", egl_shim_ChooseConfig},
    {"eglCreateWindowSurface", egl_shim_CreateWindowSurface},
    {"eglCreatePbufferSurface", egl_shim_CreatePbufferSurface},
    {"eglCreateContext", egl_shim_CreateContext}, {"eglMakeCurrent", egl_shim_MakeCurrent},
    {"eglSwapBuffers", egl_shim_SwapBuffers}, {"eglDestroySurface", egl_shim_DestroySurface},
    {"eglDestroyContext", egl_shim_DestroyContext}, {"eglQuerySurface", egl_shim_QuerySurface},
    {"eglGetConfigAttrib", egl_shim_GetConfigAttrib}, {"eglGetError", egl_shim_GetError},
    {"eglGetProcAddress", egl_shim_GetProcAddress}, {"eglBindAPI", egl_shim_BindAPI},
    {"eglQueryString", egl_shim_QueryString}, {"eglSwapInterval", egl_shim_SwapInterval},
    {"eglGetCurrentContext", egl_shim_GetCurrentContext},
    {"eglGetCurrentSurface", egl_shim_GetCurrentSurface},
    {"eglGetCurrentDisplay", egl_shim_GetDisplay}, {"eglSurfaceAttrib", egl_shim_SurfaceAttrib},
    {0, 0}
  };
  for (int i = 0; m[i].n; i++) if (!strcmp(m[i].n, nm)) return m[i].f;
  return NULL;
}

static int rxd_egl_config_attr_known(int a) {
  switch (a) {
    case 0x3020: case 0x3021: case 0x3022: case 0x3023:
    case 0x3024: case 0x3025: case 0x3026: case 0x3027:
    case 0x3028: case 0x3029: case 0x302A: case 0x302B:
    case 0x302C: case 0x302D: case 0x302E: case 0x302F:
    case 0x3031: case 0x3032: case 0x3033: case 0x3034:
    case 0x3035: case 0x3036: case 0x3037: case 0x3039:
    case 0x303A: case 0x303B: case 0x303C: case 0x303D:
    case 0x303E: case 0x303F: case 0x3040: case 0x3041:
    case 0x3042:
      return 1;
  }
  return 0;
}

static unsigned (*r_eglChooseConfig_real)(void *, const int *, void **, int, int *);
static unsigned my_eglChooseConfig_fbdev(void *dpy, const int *attrs,
                                         void **configs, int config_size,
                                         int *num_config) {
  if (!r_eglChooseConfig_real)
    r_eglChooseConfig_real = dlsym(RTLD_DEFAULT, "eglChooseConfig");
  if (!r_eglChooseConfig_real) return 0;
  if (getenv("TER_RXD_EGL_RAWCHOOSE"))
    return r_eglChooseConfig_real(dpy, attrs, configs, config_size, num_config);

  int out[96];
  int oi = 0, changed = 0, dropped = 0;
  if (attrs) {
    for (int i = 0; i < 90; i += 2) {
      int a = attrs[i];
      if (a == 0x3038) break;  /* EGL_NONE */
      int v = attrs[i + 1];
      if (!rxd_egl_config_attr_known(a)) {
        changed = 1;
        dropped++;
        continue;
      }
      if ((a == 0x3040 || a == 0x3042) && (v & 0x40)) {
        v = (v & ~0x40) | 0x04;  /* GLES3 bit -> GLES2 bit on Mali-450 */
        changed = 1;
      }
      if (oi + 2 >= (int)(sizeof(out) / sizeof(out[0]))) break;
      out[oi++] = a;
      out[oi++] = v;
    }
  }
  out[oi++] = 0x3038;

  const int *use = changed ? out : attrs;
  unsigned ok = r_eglChooseConfig_real(dpy, use, configs, config_size, num_config);
  int n = num_config ? *num_config : -1;
  if ((!ok || n <= 0) && changed) {
    static const int fallback[] = {
      0x3033, 0x0004,  /* EGL_SURFACE_TYPE, EGL_WINDOW_BIT */
      0x3040, 0x0004,  /* EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT */
      0x3024, 8, 0x3023, 8, 0x3022, 8,
      0x3025, 16,
      0x3038
    };
    ok = r_eglChooseConfig_real(dpy, fallback, configs, config_size, num_config);
    n = num_config ? *num_config : -1;
  }
  static int logn;
  if (logn++ < 16) {
    fprintf(stderr, "[EGLFIX] ChooseConfig fbdev changed=%d dropped=%d size=%d ok=%u n=%d\n",
            changed, dropped, config_size, ok, n);
    fsync(2);
  }
  return ok;
}

/* ---------- device-aware video backend (fbdev vs kmsdrm) ----------
 * Amlogic-old (Mali-450 Utgard): EGL REAL do Mali via fbdev (/dev/fb0) — a Unity
 *   cria contexto/surface direto no fb0 (g_fbdev_win). Caminho PROVADO/default.
 * X5M (Amlogic-no, Mali-G310 Valhall): NAO tem EGL fbdev — so KMSDRM. Roteamos o
 *   EGL da Unity pelo egl_shim (SDL2-compat -> SDL3 stock kmsdrm/gbm/Valhall).
 * Decisao (uma vez):
 *   CUP_VIDEO=kmsdrm | fbdev  -> forca.
 *   CUP_SHIMEGL=1 (legado)    -> kmsdrm.
 *   auto: existe /dev/dri/card0 -> kmsdrm; senao fbdev. */
static int cup_use_kmsdrm(void) {
  static int dec = -1;
  if (dec >= 0) return dec;
  const char *v = getenv("CUP_VIDEO");
  if (v && !strcmp(v, "kmsdrm")) { dec = 1; return dec; }
  if (v && !strcmp(v, "fbdev"))  { dec = 0; return dec; }
  if (getenv("CUP_SHIMEGL"))     { dec = 1; return dec; }
  dec = (access("/dev/dri/card0", F_OK) == 0) ? 1 : 0;
  return dec;
}

/* Re-roteia os egl* da libunity (hoje bindados no libEGL REAL pelo so_resolve)
 * para o egl_shim. ELO QUE FALTAVA do caminho kmsdrm: sem isto a janela SDL e'
 * criada mas a Unity continua chamando o libEGL real (sem fbdev no Valhall -> nao
 * renderiza). Chamar com o contexto do libunity ativo (so_use(g_m_unity)). */
static int egl_patch_unity_got(void) {
  static const char *names[] = {
    "eglGetDisplay", "eglInitialize", "eglTerminate", "eglChooseConfig",
    "eglCreateWindowSurface", "eglCreatePbufferSurface", "eglCreateContext",
    "eglMakeCurrent", "eglSwapBuffers", "eglDestroySurface", "eglDestroyContext",
    "eglQuerySurface", "eglGetConfigAttrib", "eglGetError", "eglGetProcAddress",
    "eglBindAPI", "eglQueryString", "eglSwapInterval", "eglGetCurrentContext",
    "eglGetCurrentSurface", "eglGetCurrentDisplay", "eglSurfaceAttrib", NULL };
  int total = 0;
  for (int i = 0; names[i]; i++) {
    void *f = (!strcmp(names[i], "eglGetCurrentDisplay")) ? (void *)egl_shim_GetDisplay
                                                          : egl_route(names[i]);
    if (f) total += so_patch_got(names[i], (uintptr_t)f);
  }
  return total;
}

/* glGetString wrapper (proven re4): o preprocessador de shader do Unity chama
 * glGetString(RENDERER/VERSION/EXT) numa thread sem contexto GL current -> real
 * devolve NULL -> parse char-a-char de NULL estoura o buffer (stack smash em
 * nativeRecreateGfxState). Cache + defaults Mali; NUNCA NULL. */
static const unsigned char *(*r_glGetString)(unsigned) = NULL;
static const unsigned char *g_glcache[5] = {0,0,0,0,0};
static int glstr_idx(unsigned n){ switch(n){case 0x1F00:return 0;case 0x1F01:return 1;case 0x1F02:return 2;case 0x1F03:return 3;case 0x8B8C:return 4;} return -1; }
/* GL_EXTENSIONS curado curto: a string real do Mali-450 é longa e o parser do
 * Unity pode estourar um buffer fixo (stack smash em nativeRecreateGfxState). */
static const char *GL_EXT_SHORT =
  "GL_OES_depth24 GL_OES_element_index_uint GL_OES_texture_npot "
  "GL_OES_rgb8_rgba8 GL_OES_packed_depth_stencil GL_OES_vertex_array_object "
  "GL_EXT_texture_format_BGRA8888 GL_OES_standard_derivatives";
static const unsigned char *my_glGetString(unsigned n){
  if(n==0x1F03) return (const unsigned char*)GL_EXT_SHORT;   /* GL_EXTENSIONS curto */
  if(!r_glGetString) r_glGetString=(const unsigned char*(*)(unsigned))dlsym(RTLD_DEFAULT,"glGetString");
  const unsigned char *s = r_glGetString ? r_glGetString(n) : NULL;
  int i = glstr_idx(n);
  if(s){ if(i>=0 && !g_glcache[i]) g_glcache[i]=(const unsigned char*)strdup((const char*)s); }
  else if(i>=0 && g_glcache[i]) s=g_glcache[i];
  else if(i>=0) s=(const unsigned char*)(n==0x1F00?"ARM":n==0x1F01?"Mali-450 MP":n==0x1F02?"OpenGL ES 2.0":n==0x8B8C?"OpenGL ES GLSL ES 1.00":"");
  return s;
}

/* ---- wrappers GL de shader (diagnóstico: shader falha/trava no Mali?) ---- */
static void (*r_glCompileShader)(unsigned);
static void (*r_glGetShaderiv)(unsigned, unsigned, int *);
static void (*r_glLinkProgram)(unsigned);
static void (*r_glGetProgramiv)(unsigned, unsigned, int *);
static void (*r_glGetShaderInfoLog)(unsigned, int, int *, char *);
static int g_shN, g_prN;
static void my_glCompileShader(unsigned sh) {
  if (!r_glCompileShader) r_glCompileShader = dlsym(RTLD_DEFAULT, "glCompileShader");
  if (!r_glGetShaderiv) r_glGetShaderiv = dlsym(RTLD_DEFAULT, "glGetShaderiv");
  if (!r_glGetShaderInfoLog) r_glGetShaderInfoLog = dlsym(RTLD_DEFAULT, "glGetShaderInfoLog");
  r_glCompileShader(sh);
  int st = -1; if (r_glGetShaderiv) r_glGetShaderiv(sh, 0x8B81, &st); /* COMPILE_STATUS */
  if (st != 1 && g_shN < 100) {
    char log[768] = {0}; if (r_glGetShaderInfoLog) r_glGetShaderInfoLog(sh, sizeof log - 1, NULL, log);
    fprintf(stderr, "[SHADER] compile FALHOU sh=%u status=%d LOG=%s\n", sh, st, log); dbg_sync();
    g_shN++;
  }
}
/* CUP_SHADERDUMP: loga o fonte GLSL na SUBMISSÃO (glGetShaderSource no Mali volta vazio) */
extern volatile int g_render_frame;
static int strstr2_any(const char **string, int count, const char *tok) {
  for (int i = 0; i < count && string[i]; i++)
    if (strstr(string[i], tok)) return 1;
  return 0;
}
static void (*r_glShaderSource)(unsigned, int, const char **, const int *);
static void my_glShaderSource(unsigned sh, int count, const char **string, const int *length) {
  if (!r_glShaderSource) r_glShaderSource = dlsym(RTLD_DEFAULT, "glShaderSource");
  if (getenv("CUP_SHADERDUMP") && string) {
    fprintf(stderr, "[SHSRC] shader=%u count=%d f=%d:\n", sh, count, g_render_frame);
    size_t tot = 0;
    for (int i = 0; i < count && string[i] && tot < 6000; i++) {
      int len = length ? length[i] : (int)strlen(string[i]);
      if (len > (int)(6000 - tot)) len = (int)(6000 - tot);
      fwrite(string[i], 1, len, stderr);
      tot += len;
    }
    fprintf(stderr, "\n[SHSRC] ---fim shader=%u---\n", sh); fsync(2);
  }
  /* CUP_ALPHAFIX: sprites/cenário/chefes INVISÍVEIS — o variant ETC1-split-alpha
   * sampleia _AlphaTex (bound num dummy 4x4) com _EnableExternalAlpha=1:
   *   alpha = mix(_MainTex.a, _AlphaTex.x, _EnableExternalAlpha) -> 0 -> transparente.
   * Os atlases aqui sobem DESCOMPRIMIDOS (RGBA com alpha real no .a — o player prova).
   * Patch: remove a declaração e troca usos de _EnableExternalAlpha por 0.0
   * (força o caminho interno _MainTex.a). */
  if (getenv("CUP_ALPHAFIX") && string &&
      (strstr2_any(string, count, "_EnableExternalAlpha") ||
       strstr2_any(string, count, "_RendererColor"))) {
    /* tokens neutralizados (substituição com fronteira de identificador):
     *   _EnableExternalAlpha -> 0.0       (força alpha interno _MainTex.a)
     *   _RendererColor/_Color -> vec4(1.0) (uniform não-setado = 0 em GLES2 -> cor*0 = invisível) */
    static const struct { const char *tok, *rep; } T[] = {
      {"_EnableExternalAlpha", "0.0"},
      {"_RendererColor", "vec4(1.0)"},
      {"_Color", "vec4(1.0)"},
    };
    size_t tot = 0;
    for (int i = 0; i < count && string[i]; i++)
      tot += (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
    char *buf = tot < 65536 ? malloc(tot + 1) : NULL;
    if (buf) {
      size_t o = 0;
      for (int i = 0; i < count && string[i]; i++) {
        size_t l = (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
        memcpy(buf + o, string[i], l); o += l;
      }
      buf[o] = 0;
      char *out = malloc(o * 2 + 64);
      if (out) {
        size_t w = 0;
        for (char *p = buf; *p; ) {
          char *nl = strchr(p, '\n');
          size_t ll = nl ? (size_t)(nl - p + 1) : strlen(p);
          int drop = 0;
          if (memmem(p, ll, "uniform", 7))
            for (unsigned t = 0; t < sizeof T / sizeof T[0] && !drop; t++)
              if (memmem(p, ll, T[t].tok, strlen(T[t].tok))) drop = 1;  /* corta declaração */
          if (drop) { p += ll; continue; }
          for (size_t k = 0; k < ll; ) {
            int hit = 0;
            for (unsigned t = 0; t < sizeof T / sizeof T[0]; t++) {
              size_t tl = strlen(T[t].tok);
              if (k + tl <= ll && !memcmp(p + k, T[t].tok, tl)) {
                char b = k ? p[k - 1] : ' ', a = (k + tl < ll) ? p[k + tl] : ' ';
                if (!(isalnum(b) || b == '_') && !(isalnum(a) || a == '_')) {  /* fronteira */
                  size_t rl = strlen(T[t].rep);
                  memcpy(out + w, T[t].rep, rl); w += rl; k += tl; hit = 1; break;
                }
              }
            }
            if (!hit) out[w++] = p[k++];
          }
          p += ll;
        }
        out[w] = 0;
        fprintf(stderr, "[ALPHAFIX] shader=%u: ExternalAlpha->0 + _Color/_RendererColor->vec4(1)\n", sh);
        fsync(2);
        r_glShaderSource(sh, 1, (const char **)&out, NULL);
        free(out); free(buf);
        return;
      }
      free(buf);
    }
  }
  r_glShaderSource(sh, count, string, length);
}
static void my_glLinkProgram(unsigned pr) {
  if (!r_glLinkProgram) r_glLinkProgram = dlsym(RTLD_DEFAULT, "glLinkProgram");
  if (!r_glGetProgramiv) r_glGetProgramiv = dlsym(RTLD_DEFAULT, "glGetProgramiv");
  r_glLinkProgram(pr);
  {
    int (*gul)(unsigned, const char *) = dlsym(RTLD_DEFAULT, "glGetUniformLocation");
    if (gul && pr < 4096 && gul(pr, "_AlphaTex") >= 0) {
      extern unsigned char g_extalpha_prog[];
      g_extalpha_prog[pr] = 1;
      fprintf(stderr, "[EXTALPHA] prog=%u marcado (variant _AlphaTex)\n", pr); fsync(2);
    }
  }
  int st = -1; if (r_glGetProgramiv) r_glGetProgramiv(pr, 0x8B82, &st); /* LINK_STATUS */
  if (st != 1 && g_prN < 100) {
    char log[768] = {0}; if (r_glGetShaderInfoLog) { void (*gpil)(unsigned,int,int*,char*) = dlsym(RTLD_DEFAULT,"glGetProgramInfoLog"); if (gpil) gpil(pr, sizeof log-1, NULL, log); }
    fprintf(stderr, "[SHADER] link FALHOU pr=%u status=%d LOG=%s\n", pr, st, log); dbg_sync(); g_prN++;
  }
}

/* ===== CUP_DRAWSPY: ring dos últimos draws p/ achar o que wedga o Utgard =====
 * O bt do wedge mostra a main presa no frame-builder lock do Mali no draw
 * SEGUINTE ao culpado (o GPU não termina o job já submetido) → registramos os
 * últimos DS_RING draws (programa/textura/FBO/count) num ring; um watchdog
 * detecta o stall (seq parado >6s) e dumpa o ring. ⚠️ sem glGetError/glFinish
 * por draw (glFinish satura o Utgard). Bisseção: CUP_SKIPFBO=1 pula draws com
 * FBO!=0 (render-to-texture); CUP_SKIPPROG=a,b,c pula programas específicos. */
static int g_drawspy = 0;       /* roteamento de gl* ligado (TEXHALF e/ou DRAWSPY) */
static int g_drawdiag = 0;      /* DIAGNÓSTICO dos DRAWS (ring + glGetIntegerv/draw) — SÓ com CUP_DRAWSPY.
                                 * ⚠️ ds_enter faz 4 glGetIntegerv POR DRAW = sync CPU↔GPU no Mali =
                                 * mata a performance. NUNCA em produção (TEXHALF sozinho NÃO liga isto). */
volatile int g_render_frame = -1;          /* setado no render loop (F2) */
static void (*ds_r_DrawElements)(unsigned, int, unsigned, const void *);
static void (*ds_r_DrawArrays)(unsigned, int, int);
static void (*ds_r_TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void (*ds_r_CompTexImage2D)(unsigned, int, unsigned, int, int, int, int, const void *);
static void (*ds_r_GetIntegerv)(unsigned, int *);

#define DS_RING 128
typedef struct {
  unsigned seq; int frame; unsigned char kind, in_progress; /* kind 0=elems 1=arrays */
  unsigned mode, type; int count, prog, tex, fbo, unit, texw, texh, tex0, tex1;
} ds_rec;
static ds_rec ds_ring[DS_RING];
static volatile unsigned ds_seq = 0;
static volatile unsigned ds_skipped = 0;

#define DS_MAXTEXID 32768
static unsigned short ds_tw[DS_MAXTEXID], ds_th[DS_MAXTEXID];

static int g_skipfbo = 0;
static int g_skipprog[8], g_nskipprog = 0;

static int ds_geti(unsigned pname) {
  int v = 0;
  if (!ds_r_GetIntegerv) ds_r_GetIntegerv = dlsym(RTLD_DEFAULT, "glGetIntegerv");
  if (ds_r_GetIntegerv) ds_r_GetIntegerv(pname, &v);
  return v;
}
static ds_rec *ds_enter(int kind, unsigned mode, int count, unsigned type) {
  unsigned s = __atomic_fetch_add(&ds_seq, 1, __ATOMIC_RELAXED);
  ds_rec *r = &ds_ring[s % DS_RING];
  r->in_progress = 0; r->seq = s; r->frame = g_render_frame;
  r->kind = (unsigned char)kind; r->mode = mode; r->count = count; r->type = type;
  r->prog = ds_geti(0x8B8D);          /* GL_CURRENT_PROGRAM */
  r->unit = ds_geti(0x84E0) - 0x84C0; /* GL_ACTIVE_TEXTURE - GL_TEXTURE0 */
  r->tex  = ds_geti(0x8069);          /* GL_TEXTURE_BINDING_2D */
  r->fbo  = ds_geti(0x8CA6);          /* GL_FRAMEBUFFER_BINDING */
  r->texw = (r->tex > 0 && r->tex < DS_MAXTEXID) ? ds_tw[r->tex] : 0;
  r->texh = (r->tex > 0 && r->tex < DS_MAXTEXID) ? ds_th[r->tex] : 0;
  r->tex0 = r->tex1 = 0;
  r->in_progress = 1;
  return r;
}
/* probe ARMÁVEL de estado por-draw (depth/blend/attachment do FBO + t0/t1):
 * touch /tmp/dsdump arma N draws via watchdog — pega o quadro completo EM GAMEPLAY */
static volatile int g_probe_arm = 0;
static void ds_probe_state(ds_rec *r) {
  static void (*at)(unsigned);
  static unsigned char (*ise)(unsigned);
  static void (*gfap)(unsigned, unsigned, unsigned, int *);
  if (!at) at = dlsym(RTLD_DEFAULT, "glActiveTexture");
  if (!ise) ise = dlsym(RTLD_DEFAULT, "glIsEnabled");
  if (!gfap) gfap = dlsym(RTLD_DEFAULT, "glGetFramebufferAttachmentParameteriv");
  if (!at || !ise) return;
  at(0x84C0); r->tex0 = ds_geti(0x8069);
  at(0x84C1); r->tex1 = ds_geti(0x8069);
  at(0x84C0 + (r->unit >= 0 && r->unit < 32 ? r->unit : 0));
  int dtest = ise(0x0B71), blend = ise(0x0BE2);
  int dmask = ds_geti(0x0B72), dfunc = ds_geti(0x0B74);
  int datt = -1;
  if (gfap && r->fbo != 0) gfap(0x8D40, 0x8D00, 0x8CD0, &datt);  /* FBO/DEPTH_ATT/OBJ_TYPE */
  /* colorMask (4 bools) + stencil completo — fragments podem estar sendo descartados */
  int cm[4] = {-1, -1, -1, -1};
  static void (*gbv)(unsigned, unsigned char *);
  if (!gbv) gbv = dlsym(RTLD_DEFAULT, "glGetBooleanv");
  if (gbv) { unsigned char b[4] = {9, 9, 9, 9}; gbv(0x0C23, b); cm[0] = b[0]; cm[1] = b[1]; cm[2] = b[2]; cm[3] = b[3]; }
  int stest = ise(0x0B90), sfunc = ds_geti(0x0B92), sref = ds_geti(0x0B97),
      svmask = ds_geti(0x0B93), swmask = ds_geti(0x0B98);
  int sciss = ise(0x0C11);
  int sbox[4] = {0}, vp[4] = {0};
  static void (*giv)(unsigned, int *);
  if (!giv) giv = dlsym(RTLD_DEFAULT, "glGetIntegerv");
  if (giv) { giv(0x0C10, sbox); giv(0x0BA2, vp); }
  fprintf(stderr, "[SPROBE] f=%d prog=%d fbo=%d u%d cnt=%d t0=%d t1=%d(%dx%d) depth{test=%d mask=%d func=0x%X att=0x%X} blend=%d color=%d%d%d%d sten{test=%d func=0x%X ref=%d vm=0x%X wm=0x%X} sciss=%d[%d,%d,%d,%d] vp=[%d,%d,%d,%d]\n",
          r->frame, r->prog, r->fbo, r->unit, r->count, r->tex0,
          r->tex1, r->tex1 > 0 && r->tex1 < DS_MAXTEXID ? ds_tw[r->tex1] : 0,
          r->tex1 > 0 && r->tex1 < DS_MAXTEXID ? ds_th[r->tex1] : 0,
          dtest, dmask, dfunc, datt, blend, cm[0], cm[1], cm[2], cm[3],
          stest, sfunc, sref, svmask, swmask,
          sciss, sbox[0], sbox[1], sbox[2], sbox[3], vp[0], vp[1], vp[2], vp[3]);
  fsync(2);
}
/* programas com _AlphaTex (variant sprite ext-alpha) marcados no link */
unsigned char g_extalpha_prog[4096];
static volatile int g_cur_prog;
static void (*r_glUseProgram)(unsigned);
static void my_glUseProgram(unsigned p) {
  if (!r_glUseProgram) r_glUseProgram = dlsym(RTLD_DEFAULT, "glUseProgram");
  g_cur_prog = (int)p;
  r_glUseProgram(p);
}
/* log de matrizes (translação+escala) enquanto o probe está armado */
static void (*r_glUniformMatrix4fv)(int, int, unsigned char, const float *);
static void my_glUniformMatrix4fv(int loc, int cnt, unsigned char tr, const float *m) {
  if (!r_glUniformMatrix4fv) r_glUniformMatrix4fv = dlsym(RTLD_DEFAULT, "glUniformMatrix4fv");
  if (g_probe_arm > 0 && m) {
    fprintf(stderr, "[MAT] prog=%d loc=%d n=%d diag=(%.3f %.3f %.3f %.3f) trans=(%.2f %.2f %.2f)\n",
            g_cur_prog, loc, cnt, m[0], m[5], m[10], m[15], m[12], m[13], m[14]);
    fsync(2);
  }
  r_glUniformMatrix4fv(loc, cnt, tr, m);
}
/* PROGSPY: na 1ª vez que um programa desenha, loga samplers->unit + fonte GLSL */
static void ds_progspy(int prog) {
  static unsigned char seen[256];
  if (prog <= 0 || prog >= 256 || seen[prog]) return;
  seen[prog] = 1;
  void (*gau)(unsigned, unsigned, int, int *, int *, unsigned *, char *) = dlsym(RTLD_DEFAULT, "glGetActiveUniform");
  int (*gul)(unsigned, const char *) = dlsym(RTLD_DEFAULT, "glGetUniformLocation");
  void (*guiv)(unsigned, int, int *) = dlsym(RTLD_DEFAULT, "glGetUniformiv");
  void (*gas)(unsigned, int, int *, unsigned *) = dlsym(RTLD_DEFAULT, "glGetAttachedShaders");
  void (*gss)(unsigned, int, int *, char *) = dlsym(RTLD_DEFAULT, "glGetShaderSource");
  void (*gpiv)(unsigned, unsigned, int *) = dlsym(RTLD_DEFAULT, "glGetProgramiv");
  if (!gau || !gul || !guiv || !gas || !gss || !gpiv) return;
  int nu = 0; gpiv(prog, 0x8B86, &nu);  /* GL_ACTIVE_UNIFORMS */
  fprintf(stderr, "[PROGSPY] prog=%d uniforms=%d f=%d\n", prog, nu, g_render_frame);
  for (int i = 0; i < nu && i < 64; i++) {
    char nm[128] = {0}; int sz = 0; unsigned ty = 0;
    gau(prog, i, sizeof nm - 1, NULL, &sz, &ty, nm);
    if (ty == 0x8B5E || ty == 0x8B60) {  /* SAMPLER_2D / SAMPLER_CUBE */
      int loc = gul(prog, nm), unit = -1;
      if (loc >= 0) guiv(prog, loc, &unit);
      fprintf(stderr, "[PROGSPY]   sampler %s -> unit %d\n", nm, unit);
    }
  }
  unsigned shs[4] = {0}; int ns = 0;
  gas(prog, 4, &ns, shs);
  for (int i = 0; i < ns; i++) {
    static char src[4096]; int len = 0; src[0] = 0;
    gss(shs[i], sizeof src - 1, &len, src);
    fprintf(stderr, "[PROGSPY] prog=%d shader[%d] len=%d SRC:\n%.2400s\n[PROGSPY] ---fim---\n", prog, i, len, src);
  }
  fsync(2);
}
static int ds_skip(const ds_rec *r) {
  if (g_skipfbo && r->fbo != 0) return 1;
  for (int i = 0; i < g_nskipprog; i++) if (r->prog == g_skipprog[i]) return 1;
  return 0;
}
volatile unsigned long g_frame_draws, g_frame_verts, g_draws_lo;   /* CUP_DRAWCOUNT: carga de desenho/frame */
extern int rs_logical0(void); extern int rs_enabled(void);
static void ds_draw_noscissor(void (*draw)(unsigned, int, unsigned, const void *),
                              unsigned mode, int count, unsigned type, const void *idx) {
  static void (*dis)(unsigned), (*ena)(unsigned);
  static unsigned char (*ise2)(unsigned);
  if (!dis) { dis = dlsym(RTLD_DEFAULT, "glDisable"); ena = dlsym(RTLD_DEFAULT, "glEnable"); ise2 = dlsym(RTLD_DEFAULT, "glIsEnabled"); }
  int was = ise2 ? ise2(0x0C11) : 0;
  if (was && dis) dis(0x0C11);
  draw(mode, count, type, idx);
  if (was && ena) ena(0x0C11);
}
static int g_noscissor = -1;
static void my_glDrawElements(unsigned mode, int count, unsigned type, const void *idx) {
  g_frame_draws++; g_frame_verts += (unsigned)count;
  if (rs_enabled() && rs_logical0()) g_draws_lo++;
  if (g_noscissor < 0) g_noscissor = getenv("CUP_NOSCISSOR") ? 1 : 0;
  if (!g_drawdiag) {  /* fast path: SEM glGetIntegerv (NOSCISSOR só olha o prog atual) */
    if (g_noscissor && g_cur_prog > 0 && g_cur_prog < 4096 && g_extalpha_prog[g_cur_prog]) {
      ds_draw_noscissor(ds_r_DrawElements, mode, count, type, idx);
      return;
    }
    ds_r_DrawElements(mode, count, type, idx);
    return;
  }
  ds_rec *r = ds_enter(0, mode, count, type);
  ds_progspy(r->prog);
  if (g_probe_arm > 0) { g_probe_arm--; ds_probe_state(r); }
  if (r->seq < 8) fprintf(stderr, "[DS] draw#%u f=%d ELM cnt=%d prog=%d fbo=%d tex=%d(%dx%d)\n",
                          r->seq, r->frame, count, r->prog, r->fbo, r->tex, r->texw, r->texh);
  if (ds_skip(r)) { r->in_progress = 0; ds_skipped++; return; }
  /* CUP_NOSCISSOR: testa se o scissor está cortando os sprites ext-alpha */
  if (g_noscissor && r->prog > 0 && r->prog < 4096 && g_extalpha_prog[r->prog]) {
    ds_draw_noscissor(ds_r_DrawElements, mode, count, type, idx);
    r->in_progress = 0;
    return;
  }
  ds_r_DrawElements(mode, count, type, idx);
  r->in_progress = 0;
}
/* CUP_DRAWCOUNT: conta glClear + loga a clear-color (diag de "Draw roda mas 0 draws") */
volatile unsigned long g_clear_count;
static void (*ds_r_Clear)(unsigned);
static void (*ds_r_ClearColor)(float, float, float, float);
static void my_glClear(unsigned mask) {
  g_clear_count++;
  if (ds_r_Clear) ds_r_Clear(mask);
}
static void my_glClearColor(float r, float g, float b, float a) {
  static int n = 0; if (n++ < 8) { fprintf(stderr, "[CLEARCOL] %.3f %.3f %.3f %.3f\n", r, g, b, a); fsync(2); }
  if (ds_r_ClearColor) ds_r_ClearColor(r, g, b, a);
}
static void my_glDrawArrays(unsigned mode, int first, int count) {
  g_frame_draws++; g_frame_verts += (unsigned)count;
  if (!g_drawdiag) { ds_r_DrawArrays(mode, first, count); return; }  /* fast path */
  ds_rec *r = ds_enter(1, mode, count, 0);
  if (ds_skip(r)) { r->in_progress = 0; ds_skipped++; return; }
  ds_r_DrawArrays(mode, first, count);
  r->in_progress = 0;
}
static void ds_rectex(int w, int h, const char *what) {
  int t = ds_geti(0x8069);
  if (t > 0 && t < DS_MAXTEXID) {
    if ((unsigned short)w > ds_tw[t]) ds_tw[t] = (unsigned short)w;
    if ((unsigned short)h > ds_th[t]) ds_th[t] = (unsigned short)h;
  }
  if (w >= 1024 || h >= 1024) { fprintf(stderr, "[DS] BIG TEX %s id=%d %dx%d f=%d\n", what, t, w, h, g_render_frame); fsync(2); }
}
/* CUP_TEXHALF=N: downscale (nearest) das texturas nivel-0 não-comprimidas até
 * caberem no teto N (max(w,h) <= N). N=512 → 2048 vira 512 (1/16 da RAM/VRAM!).
 * Reduz drasticamente p/ o load de assets persistentes caber em 832MB + evita o
 * limite do Utgard. Receita Bully/Castlevania (agressiva). px!=NULL só. */
static int g_texhalf = 0;
static int gl_bpp(unsigned fmt, unsigned type) {
  switch (type) {
    case 0x8033: case 0x8034: case 0x8363: return 2;  /* 4444 / 5551 / 565 */
    case 0x1401:                                       /* UNSIGNED_BYTE */
      switch (fmt) { case 0x1908: return 4;            /* RGBA */
                     case 0x1907: return 3;            /* RGB */
                     case 0x190A: return 2;            /* LUMINANCE_ALPHA */
                     case 0x1909: case 0x1906: return 1; } /* LUMINANCE / ALPHA */
  }
  return 0;  /* desconhecido -> não mexe */
}
static unsigned char ds_shift[DS_MAXTEXID];  /* fator de downscale (log2) por tex id */
static unsigned (*r_glGetError2)(void);
static void my_glTexImage2D(unsigned tgt, int lvl, int ifmt, int w, int h, int b, unsigned fmt, unsigned type, const void *px) {
  if (lvl == 0 && tgt == 0x0DE1) ds_rectex(w, h, "tex");
  if (g_drawdiag && lvl == 0 && tgt == 0x0DE1 && w >= 256) {
    fprintf(stderr, "[TEXFMT] id=%d %dx%d ifmt=0x%X fmt=0x%X type=0x%X px=%c f=%d\n",
            ds_geti(0x8069), w, h, ifmt, fmt, type, px ? 'Y' : 'N', g_render_frame); fsync(2);
  }
  /* CUP_TEXSTAT: o canal alpha dos atlases grandes é real ou zerado? */
  if (getenv("CUP_TEXSTAT") && lvl == 0 && tgt == 0x0DE1 && w >= 1024 && px && fmt == 0x1908 && type == 0x1401) {
    const unsigned char *q = px;
    size_t n = (size_t)w * h, z = 0, ff = 0;
    for (size_t i = 0; i < n; i += 7) {            /* amostra 1/7 dos pixels */
      unsigned char a = q[i * 4 + 3];
      if (a == 0) z++; else if (a == 255) ff++;
    }
    size_t s = (n + 6) / 7;
    fprintf(stderr, "[TEXSTAT] id=%d %dx%d alpha: zero=%zu%% cheio=%zu%% (amostra %zu) f=%d\n",
            ds_geti(0x8069), w, h, z * 100 / s, ff * 100 / s, s, g_render_frame); fsync(2);
  }
  int tid = (g_texhalf && tgt == 0x0DE1) ? ds_geti(0x8069) : 0;
  int shift = 0;
  if (g_texhalf && tgt == 0x0DE1 && px && gl_bpp(fmt, type) > 0) {
    if (lvl == 0) {
      int mw = w, mh = h;
      while ((mw > g_texhalf || mh > g_texhalf) && mw > 1 && mh > 1) { mw >>= 1; mh >>= 1; shift++; }
      if (tid > 0 && tid < DS_MAXTEXID) ds_shift[tid] = (unsigned char)shift;
    } else if (tid > 0 && tid < DS_MAXTEXID) {
      shift = ds_shift[tid];                          /* mesma chain do nivel-0 */
      while (shift > 0 && ((w >> shift) < 1 || (h >> shift) < 1)) shift--;
    }
  }
  if (shift > 0) {
    int bpp = gl_bpp(fmt, type);
    int nw = w >> shift, nh = h >> shift, st = 1 << shift;
    unsigned char *dst = malloc((size_t)nw * nh * bpp);
    if (dst) {
      const unsigned char *src = px;
      for (int y = 0; y < nh; y++) {
        const unsigned char *srow = src + (size_t)(y * st) * w * bpp;
        unsigned char *drow = dst + (size_t)y * nw * bpp;
        for (int x = 0; x < nw; x++)
          memcpy(drow + x * bpp, srow + (size_t)(x * st) * bpp, bpp);
      }
      static int n; if (n++ < 40) { fprintf(stderr, "[TEXHALF] tex=%d %dx%d -> %dx%d (/%d lvl%d)\n", tid, w, h, nw, nh, st, lvl); fsync(2); }
      ds_r_TexImage2D(tgt, lvl, ifmt, nw, nh, b, fmt, type, dst);
      free(dst);
      return;
    }
  }
  ds_r_TexImage2D(tgt, lvl, ifmt, w, h, b, fmt, type, px);
}
static const char *gl_comp_fmt_name(unsigned ifmt) {
  switch (ifmt) {
    case 0x8D64: return "ETC1_RGB8_OES";
    case 0x9274: return "ETC2_RGB8";
    case 0x9275: return "ETC2_SRGB8";
    case 0x9276: return "ETC2_RGB8_PUNCHTHROUGH_ALPHA1";
    case 0x9277: return "ETC2_SRGB8_PUNCHTHROUGH_ALPHA1";
    case 0x9278: return "ETC2_RGBA8_EAC";
    case 0x9279: return "ETC2_SRGB8_ALPHA8_EAC";
    case 0x93B0: return "ASTC_4x4_RGBA";
    case 0x93B7: return "ASTC_8x8_RGBA";
    case 0x83F1: return "DXT1_RGB";
    case 0x83F2: return "DXT1_RGBA";
    case 0x83F3: return "DXT3_RGBA";
    case 0x83F4: return "DXT5_RGBA";
  }
  return "?";
}
static void my_glCompTexImage2D(unsigned tgt, int lvl, unsigned ifmt, int w, int h, int b, int sz, const void *px) {
  if (lvl == 0 && tgt == 0x0DE1) ds_rectex(w, h, "ctex");
  int log = getenv("TER_RXD_CTEXTURE_LOG") != NULL;
  if (log && !r_glGetError2) r_glGetError2 = dlsym(RTLD_DEFAULT, "glGetError");
  if (log && r_glGetError2) r_glGetError2();
  ds_r_CompTexImage2D(tgt, lvl, ifmt, w, h, b, sz, px);
  if (log) {
    unsigned err = r_glGetError2 ? r_glGetError2() : 0;
    static int n = 0;
    if (n++ < env_int_default("TER_RXD_CTEXTURE_LOG_MAX", 240)) {
      fprintf(stderr,
              "[CTEX] id=%d lvl=%d %dx%d ifmt=0x%X(%s) size=%d px=%c err=0x%X f=%d\n",
              ds_geti(0x8069), lvl, w, h, ifmt, gl_comp_fmt_name(ifmt), sz,
              px ? 'Y' : 'N', err, g_render_frame);
      fsync(2);
    }
  }
}
/* ---- renderbuffer/FBO: log + retry de formato de DEPTH não suportado ----
 * Hipótese chefes-invisíveis: Unity pede DEPTH_COMPONENT24/32 (ext OES) que o blob
 * Utgard antigo não tem -> glRenderbufferStorage falha SILENCIOSO -> FBO da cena sem
 * depth -> passe opaco front-to-back sem oclusão -> céu (desenhado depois) soterra
 * os sprites. Retry com DEPTH_COMPONENT16 (sempre suportado em GLES2). */
static void (*r_glRenderbufferStorage)(unsigned, unsigned, int, int);
static unsigned (*r_glCheckFBStatus)(unsigned);
static void my_glRenderbufferStorage(unsigned tgt, unsigned ifmt, int w, int h) {
  if (!r_glRenderbufferStorage) r_glRenderbufferStorage = dlsym(RTLD_DEFAULT, "glRenderbufferStorage");
  if (!r_glGetError2) r_glGetError2 = dlsym(RTLD_DEFAULT, "glGetError");
  if (r_glGetError2) r_glGetError2();                     /* limpa erro pendente */
  r_glRenderbufferStorage(tgt, ifmt, w, h);
  unsigned err = r_glGetError2 ? r_glGetError2() : 0;
  fprintf(stderr, "[RBSTOR] rb=%d ifmt=0x%X %dx%d err=0x%X\n", ds_geti(0x8CA7), ifmt, w, h, err);
  if (err) {
    unsigned fb = 0;
    if (ifmt == 0x81A6 || ifmt == 0x81A7) fb = 0x81A5;          /* DEPTH24/32 -> DEPTH16 */
    else if (ifmt == 0x88F0) fb = 0x81A5;                       /* D24S8 -> DEPTH16 (sem stencil) */
    if (fb) {
      r_glRenderbufferStorage(tgt, fb, w, h);
      unsigned e2 = r_glGetError2 ? r_glGetError2() : 0;
      fprintf(stderr, "[RBSTOR] retry ifmt=0x%X -> err=0x%X %s\n", fb, e2, e2 ? "FALHOU" : "OK");
    }
    fsync(2);
  }
}
/* wiring dos FBOs: qual textura/RB em cada attachment (mapa cena->composição) */
static void (*r_glFBTex2D)(unsigned, unsigned, unsigned, unsigned, int);
static void my_glFramebufferTexture2D(unsigned tgt, unsigned att, unsigned textgt, unsigned tex, int lvl) {
  if (!r_glFBTex2D) r_glFBTex2D = dlsym(RTLD_DEFAULT, "glFramebufferTexture2D");
  fprintf(stderr, "[FBWIRE] fbo=%d att=0x%X tex=%u(%dx%d)\n", ds_geti(0x8CA6), att, tex,
          tex > 0 && tex < DS_MAXTEXID ? ds_tw[tex] : 0, tex > 0 && tex < DS_MAXTEXID ? ds_th[tex] : 0);
  fsync(2);
  r_glFBTex2D(tgt, att, textgt, tex, lvl);
}
static void (*r_glFBRb)(unsigned, unsigned, unsigned, unsigned);
static void my_glFramebufferRenderbuffer(unsigned tgt, unsigned att, unsigned rbtgt, unsigned rb) {
  if (!r_glFBRb) r_glFBRb = dlsym(RTLD_DEFAULT, "glFramebufferRenderbuffer");
  fprintf(stderr, "[FBWIRE] fbo=%d att=0x%X rb=%u\n", ds_geti(0x8CA6), att, rb); fsync(2);
  r_glFBRb(tgt, att, rbtgt, rb);
}
static unsigned my_glCheckFramebufferStatus(unsigned tgt) {
  if (!r_glCheckFBStatus) r_glCheckFBStatus = dlsym(RTLD_DEFAULT, "glCheckFramebufferStatus");
  unsigned st = r_glCheckFBStatus ? r_glCheckFBStatus(tgt) : 0;
  if (st != 0x8CD5) { fprintf(stderr, "[FBSTAT] fbo=%d status=0x%X (INCOMPLETO)\n", ds_geti(0x8CA6), st); fsync(2); }
  return st;
}
static void ds_dump(void) {
  unsigned end = ds_seq;
  unsigned n = end < DS_RING ? end : DS_RING;
  fprintf(stderr, "[DS] ===== DUMP ring (seq=%u frame=%d skipped=%u) =====\n", end, g_render_frame, ds_skipped);
  for (unsigned i = end - n; i != end; i++) {
    ds_rec *r = &ds_ring[i % DS_RING];
    if (r->seq != i) continue;  /* slot já sobrescrito (race benigna) */
    fprintf(stderr, "[DS] #%u f=%d %s mode=%u cnt=%d prog=%d fbo=%d u%d tex=%d(%dx%d) t0=%d t1=%d%s\n",
            r->seq, r->frame, r->kind ? "ARR" : "ELM", r->mode, r->count,
            r->prog, r->fbo, r->unit, r->tex, r->texw, r->texh, r->tex0, r->tex1,
            r->in_progress ? "  <== IN-PROGRESS (bloqueado no driver)" : "");
  }
  fsync(2);
}
static void *ds_watchdog(void *a) {
  (void)a;
  unsigned last = 0; int still = 0, dumped = 0, beat = 0;
  for (;;) {
    sleep(2);
    unsigned s = ds_seq;
    if (++beat % 30 == 0) { fprintf(stderr, "[DS] alive seq=%u f=%d skipped=%u\n", s, g_render_frame, ds_skipped); fsync(2); }
    if (access("/tmp/dsdump", F_OK) == 0) { unlink("/tmp/dsdump"); ds_dump(); g_probe_arm = 60; }
    /* liga/desliga o diag PESADO em runtime (boot/nav leves, diag só na fase) */
    if (access("/tmp/dson", F_OK) == 0) { unlink("/tmp/dson"); g_drawdiag = 1; fprintf(stderr, "[DS] drawdiag ON (runtime)\n"); fsync(2); }
    if (access("/tmp/dsoff", F_OK) == 0) { unlink("/tmp/dsoff"); g_drawdiag = 0; fprintf(stderr, "[DS] drawdiag OFF (runtime)\n"); fsync(2); }
    if (s != last) { last = s; still = 0; dumped = 0; continue; }
    if (s == 0) continue;
    if (++still >= 3 && !dumped) {
      fprintf(stderr, "[DS] STALL: draws parados ha %ds (seq=%u)\n", still * 2, s);
      ds_dump(); dumped = 1;
    }
  }
  return NULL;
}
/* render-scale (renderscale.c): redireciona a tela p/ um FBO lo-res + upscale */
extern void rs_init(void); extern int rs_enabled(void);
extern void rs_BindFramebuffer(unsigned, unsigned);
extern void rs_Viewport(int, int, int, int);
extern void rs_Scissor(int, int, int, int);
extern void rs_present(void);
static unsigned (*r_eglSwapBuffers)(void *, void *);
static void ter_screenshot_write(const char *tag, long n) {
  static void (*p_gi)(unsigned, int*);
  static void (*p_rp)(int,int,int,int,unsigned,unsigned,void*);
  if (!p_gi) p_gi = (void*)dlsym(RTLD_DEFAULT, "glGetIntegerv");
  if (!p_rp) p_rp = (void*)dlsym(RTLD_DEFAULT, "glReadPixels");
  if (!p_gi || !p_rp) { fprintf(stderr, "[SHOT] sem glGetIntegerv/glReadPixels\n"); return; }
  int vp[4] = {0,0,0,0}; p_gi(0x0BA2 /*GL_VIEWPORT*/, vp);
  int w = vp[2], h = vp[3]; if (w <= 0 || h <= 0) { w = g_fbdev_win.w; h = g_fbdev_win.h; }
  if (w <= 0 || h <= 0) { fprintf(stderr, "[SHOT] viewport 0 (%s #%ld)\n", tag ? tag : "?", n); return; }
  unsigned char *buf = malloc((size_t)w*h*4); if (!buf) return;
  p_rp(0,0,w,h,0x1908/*GL_RGBA*/,0x1401/*GL_UNSIGNED_BYTE*/,buf);
  FILE *f = fopen(ASSET_BASE_M "shot.ppm","wb");
  if (f) { fprintf(f,"P6\n%d %d\n255\n", w, h);
    for (int y=h-1;y>=0;y--) for (int x=0;x<w;x++){ unsigned char*p=buf+((size_t)y*w+x)*4; fwrite(p,1,3,f);}
    fclose(f);
    /* conta pixels nao-pretos + draws acumulados p/ diagnostico de tela preta */
    long nb = 0; for (size_t i = 0; i < (size_t)w*h; i++) if (buf[i*4]+buf[i*4+1]+buf[i*4+2] > 24) nb++;
    extern volatile unsigned long g_frame_draws, g_frame_verts, g_clear_count;
    fprintf(stderr,"[SHOT] gravado shot.ppm %dx%d (%s #%ld) nao-pretos=%ld/%d draws_acum=%lu verts=%lu clears=%lu\n",
            w,h, tag ? tag : "?", n, nb, w*h, g_frame_draws, g_frame_verts, g_clear_count);
  } else {
    fprintf(stderr,"[SHOT] fopen shot.ppm falhou errno=%d (%s #%ld)\n", errno, tag ? tag : "?", n);
  }
  free(buf);
}
/* TER_SHOT=N: na N-esima troca de buffer, faz glReadPixels da tela e grava um PPM
 * em /storage/roms/terraria/shot.ppm (verificacao autonoma de IMAGEM no fbdev Mali). */
static void ter_screenshot_maybe(void) {
  static long n = 0; n++;
  const char *s = getenv("TER_SHOT");
  /* TER_SHOTLIVE: captura SOB DEMANDA — quando /tmp/tershot existir, grava shot.ppm e remove o
     gatilho. Permite tirar print a qualquer momento navegando o menu por ssh (depurar Settings). */
  int live = getenv("TER_SHOTLIVE") && access("/tmp/tershot", F_OK) == 0;
  if (live) unlink("/tmp/tershot");
  if (!s && !live) return;
  if (s && !live) { long target = atoi(s); if (target <= 0) target = 1; if (n != target) return; }
  ter_screenshot_write(live ? "live" : "swap", n);
}
static void ter_screenshot_frame_maybe(int frame) {
  const char *s = getenv("TER_SHOT_FRAME");
  static int done = 0;
  if (!s || done) return;
  long target = atol(s);
  if (target <= 0) target = 1;
  if (frame < target) return;
  done = 1;
  ter_screenshot_write("frame", frame);
}
static void ter_nuke_methods(void);
static void ter_jobworkers0(void);
/* chamado por egl_shim_SwapBuffers na thread DONA da window (captura o buffer apresentado) */
void ter_shot_hook(void) { ter_nuke_methods(); ter_jobworkers0(); ter_screenshot_maybe(); }

/* native_pad.c: estado do pad (0=A 1=B 2=X 3=Y 4=LB 5=RB 6=Back 7=Start 8=L3 9=R3 10..13=dpad U D L R) */
extern int np_btn(int b), np_btn_down(int b);
static int g_vkbd_swallow, g_vkbd_sel, g_vkbd_force_frames;
static char g_vkbd_force_text[128];
static void ter_vkbd_update(void);
static void ter_vkbd_draw(void);
static void ter_vkbd_maybe_open(void);
static void ter_name_hooks_install(void);
static void ter_name_force_text(const char *text);
static void ter_name_commit_text(const char *text);
static void ter_force_main_player_name(const char *text);
static void ter_player_name_menu_force_text(const char *text);
static const char *ter_vkbd_effective_name(const char *fallback);
int ter_vkbd_blocking(void) {
  if (getenv("TER_NOVKBD")) return 0;
  return jni_softinput_active() || g_vkbd_swallow > 0;
}
static const char *vk_keys[] = {
  "A","B","C","D","E","F","G","H","I","J",
  "K","L","M","N","O","P","Q","R","S","T",
  "U","V","W","X","Y","Z","0","1","2","3",
  "4","5","6","7","8","9","-","_","DEL","SP",
  "OK","CXL"
};
#define VK_NKEYS ((int)(sizeof(vk_keys)/sizeof(vk_keys[0])))
#define VK_COLS 10

static int vk_glyph(char c, unsigned char r[7]) {
  memset(r, 0, 7);
  #define VG(a,b,c,d,e,f,g) do { unsigned char v[7]={a,b,c,d,e,f,g}; memcpy(r,v,7); return 1; } while(0)
  switch (c) {
    case 'A': VG(14,17,17,31,17,17,17); case 'B': VG(30,17,17,30,17,17,30);
    case 'C': VG(14,17,16,16,16,17,14); case 'D': VG(30,17,17,17,17,17,30);
    case 'E': VG(31,16,16,30,16,16,31); case 'F': VG(31,16,16,30,16,16,16);
    case 'G': VG(14,17,16,23,17,17,14); case 'H': VG(17,17,17,31,17,17,17);
    case 'I': VG(14,4,4,4,4,4,14);      case 'J': VG(7,2,2,2,18,18,12);
    case 'K': VG(17,18,20,24,20,18,17); case 'L': VG(16,16,16,16,16,16,31);
    case 'M': VG(17,27,21,21,17,17,17); case 'N': VG(17,25,21,19,17,17,17);
    case 'O': VG(14,17,17,17,17,17,14); case 'P': VG(30,17,17,30,16,16,16);
    case 'Q': VG(14,17,17,17,21,18,13); case 'R': VG(30,17,17,30,20,18,17);
    case 'S': VG(15,16,16,14,1,1,30);   case 'T': VG(31,4,4,4,4,4,4);
    case 'U': VG(17,17,17,17,17,17,14); case 'V': VG(17,17,17,17,17,10,4);
    case 'W': VG(17,17,17,21,21,21,10); case 'X': VG(17,17,10,4,10,17,17);
    case 'Y': VG(17,17,10,4,4,4,4);     case 'Z': VG(31,1,2,4,8,16,31);
    case '0': VG(14,17,19,21,25,17,14); case '1': VG(4,12,4,4,4,4,14);
    case '2': VG(14,17,1,2,4,8,31);     case '3': VG(30,1,1,14,1,1,30);
    case '4': VG(2,6,10,18,31,2,2);     case '5': VG(31,16,16,30,1,1,30);
    case '6': VG(14,16,16,30,17,17,14); case '7': VG(31,1,2,4,8,8,8);
    case '8': VG(14,17,17,14,17,17,14); case '9': VG(14,17,17,15,1,1,14);
    case '-': VG(0,0,0,31,0,0,0);       case '_': VG(0,0,0,0,0,0,31);
    case ':': VG(0,4,4,0,4,4,0);        case ' ': return 1;
  }
  return 0;
  #undef VG
}

static int vk_gl_begin(int *sw, int *sh, int old_scissor[4], float old_clear[4], int *old_enabled) {
  static void (*p_en)(unsigned), (*p_dis)(unsigned), (*p_sc)(int,int,int,int);
  static void (*p_cc)(float,float,float,float), (*p_cl)(unsigned);
  static void (*p_gi)(unsigned,int*), (*p_gf)(unsigned,float*);
  static unsigned char (*p_ie)(unsigned);
  if (!p_en) {
    p_en=dlsym(RTLD_DEFAULT,"glEnable"); p_dis=dlsym(RTLD_DEFAULT,"glDisable");
    p_sc=dlsym(RTLD_DEFAULT,"glScissor"); p_cc=dlsym(RTLD_DEFAULT,"glClearColor");
    p_cl=dlsym(RTLD_DEFAULT,"glClear"); p_gi=dlsym(RTLD_DEFAULT,"glGetIntegerv");
    p_gf=dlsym(RTLD_DEFAULT,"glGetFloatv"); p_ie=dlsym(RTLD_DEFAULT,"glIsEnabled");
  }
  if (!p_en || !p_dis || !p_sc || !p_cc || !p_cl) return 0;
  *sw = ter_window_w();
  *sh = ter_window_h();
  if (*sw <= 0 || *sh <= 0) return 0;
  if (p_gi) p_gi(0x0C10, old_scissor); else memset(old_scissor, 0, 4*sizeof(int));
  if (p_gf) p_gf(0x0C22, old_clear); else old_clear[0]=old_clear[1]=old_clear[2]=old_clear[3]=0.0f;
  *old_enabled = p_ie ? p_ie(0x0C11) : 0;
  p_en(0x0C11);
  return 1;
}
static void vk_rect(int sw, int sh, int x, int y, int w, int h, float r, float g, float b, float a) {
  static void (*p_sc)(int,int,int,int), (*p_cc)(float,float,float,float), (*p_cl)(unsigned);
  if (!p_sc) { p_sc=dlsym(RTLD_DEFAULT,"glScissor"); p_cc=dlsym(RTLD_DEFAULT,"glClearColor"); p_cl=dlsym(RTLD_DEFAULT,"glClear"); }
  if (!p_sc || !p_cc || !p_cl) return;
  if (w <= 0 || h <= 0 || x >= sw || y >= sh || x + w <= 0 || y + h <= 0) return;
  if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
  if (x + w > sw) w = sw - x; if (y + h > sh) h = sh - y;
  p_sc(x, sh - y - h, w, h); p_cc(r,g,b,a); p_cl(0x00004000);
}
static void vk_gl_end(const int old_scissor[4], const float old_clear[4], int old_enabled) {
  void (*p_sc)(int,int,int,int)=dlsym(RTLD_DEFAULT,"glScissor");
  void (*p_cc)(float,float,float,float)=dlsym(RTLD_DEFAULT,"glClearColor");
  void (*p_en)(unsigned)=dlsym(RTLD_DEFAULT,"glEnable");
  void (*p_dis)(unsigned)=dlsym(RTLD_DEFAULT,"glDisable");
  if (p_sc) p_sc(old_scissor[0], old_scissor[1], old_scissor[2], old_scissor[3]);
  if (p_cc) p_cc(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
  if (old_enabled) { if (p_en) p_en(0x0C11); } else { if (p_dis) p_dis(0x0C11); }
}
static void vk_text(int sw, int sh, int x, int y, const char *s, int scale, float r, float g, float b) {
  for (int n=0; s && s[n]; n++) {
    unsigned char rows[7]; char ch=s[n];
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    vk_glyph(ch, rows);
    for (int yy=0; yy<7; yy++) for (int xx=0; xx<5; xx++)
      if (rows[yy] & (1 << (4-xx))) vk_rect(sw, sh, x+n*6*scale+xx*scale, y+yy*scale, scale, scale, r,g,b,1.0f);
  }
}
static void vk_append_char(char c) {
  char buf[128];
  snprintf(buf, sizeof buf, "%s", jni_softinput_text());
  size_t n = strlen(buf), lim = (size_t)jni_softinput_limit();
  if (lim >= sizeof buf) lim = sizeof buf - 1;
  if (n < lim) {
    buf[n] = c; buf[n+1] = 0;
    jni_softinput_set_text(buf);
    snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", buf);
  }
}
static void vk_backspace(void) {
  char buf[128]; snprintf(buf, sizeof buf, "%s", jni_softinput_text());
  size_t n = strlen(buf); if (n > 0) {
    buf[n-1] = 0;
    jni_softinput_set_text(buf);
    snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", buf);
  }
}
static void vk_commit_text(void) {
  char text[128];
  snprintf(text, sizeof text, "%s", jni_softinput_text());
  snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", text);
  g_vkbd_force_frames = 180;
  jni_softinput_commit(text);
  ter_name_commit_text(text);
  g_vkbd_swallow = 24;
}
static void vk_accept_key(const char *k) {
  if (!strcmp(k, "OK")) { vk_commit_text(); return; }
  if (!strcmp(k, "CXL")) { jni_softinput_cancel(); g_vkbd_swallow = 18; return; }
  if (!strcmp(k, "DEL")) { vk_backspace(); return; }
  if (!strcmp(k, "SP")) { vk_append_char(' '); return; }
  if (k[0] && !k[1]) vk_append_char(k[0]);
}
static void ter_vkbd_maybe_open(void) {
  if (getenv("TER_NOVKBD")) return;
  if (!getenv("TER_OSK") || jni_softinput_active()) return;
  if (access("/tmp/tervkbd", F_OK) == 0) {
    unlink("/tmp/tervkbd");
    jni_softinput_open(getenv("TER_VK_TEXT") ? getenv("TER_VK_TEXT") : "", 32);
    return;
  }
  if ((np_btn_down(3) && np_btn(9)) || (np_btn_down(9) && np_btn(3))) {   /* Y+R3 */
    jni_softinput_open("", 32);
  }
}
static void ter_vkbd_update(void) {
  if (getenv("TER_NOVKBD")) return;
  static int rep, was, auto_frames;
  if (!jni_softinput_active()) { was = 0; return; }
  if (!was) {
    was = 1; g_vkbd_sel = 0; rep = 0; auto_frames = 0;
    snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", jni_softinput_text());
    fprintf(stderr, "[VKBD] teclado ON (A=letra/OK X/B=apaga Y=espaco RB/RT/R3/Start=ok Select=cancela)\n"); fsync(2);
  }
  if (getenv("TER_VK_AUTOTEXT")) {
    const char *t = getenv("TER_VK_AUTOTEXT");
    if (auto_frames == 5) jni_softinput_set_text(t && *t ? t : "PLAYER");
    if (auto_frames++ == 25) { vk_commit_text(); return; }
  }
  int dx = np_btn(13) ? 1 : (np_btn(12) ? -1 : 0);   /* dpad R/L */
  int dy = np_btn(11) ? 1 : (np_btn(10) ? -1 : 0);   /* dpad D/U */
  int edge_move = (np_btn_down(10) || np_btn_down(11) || np_btn_down(12) || np_btn_down(13));
  if ((dx || dy) && (edge_move || rep <= 0)) {
    int row = g_vkbd_sel / VK_COLS, col = g_vkbd_sel % VK_COLS;
    col += dx; row += dy;
    if (col < 0) col = VK_COLS - 1; if (col >= VK_COLS) col = 0;
    int rows = (VK_NKEYS + VK_COLS - 1) / VK_COLS;
    if (row < 0) row = rows - 1; if (row >= rows) row = 0;
    int ns = row * VK_COLS + col;
    while (ns >= VK_NKEYS && ns > 0) ns--;
    g_vkbd_sel = ns; rep = edge_move ? 14 : 8;
  }
  if (rep > 0) rep--;
  if (np_btn_down(0)) vk_accept_key(vk_keys[g_vkbd_sel]);              /* A = letra */
  if (np_btn_down(1) || np_btn_down(2)) vk_backspace();                 /* B/X = apaga */
  if (np_btn_down(3)) vk_append_char(' ');                              /* Y = espaco */
  if (np_btn_down(6)) { jni_softinput_cancel(); g_vkbd_swallow = 18; }  /* Select = cancela */
  if (np_btn_down(7) || np_btn_down(5) || np_btn_down(9)) vk_commit_text(); /* Start/RB/R3 = ok */
}
static void ter_vkbd_draw(void) {
  if (!jni_softinput_active()) return;
  int sw=0, sh=0, osc[4], oen=0; float occ[4];
  if (!vk_gl_begin(&sw, &sh, osc, occ, &oen)) return;
  int margin = sw < 800 ? 18 : 32, gap = sw < 800 ? 3 : 5;
  int keyw = (sw - margin*2 - gap*(VK_COLS-1)) / VK_COLS;
  if (keyw < 34) keyw = 34;
  int keyh = sh < 600 ? 34 : 42;
  int rows = (VK_NKEYS + VK_COLS - 1) / VK_COLS;
  int panel_h = 46 + rows*keyh + (rows-1)*gap + 18;
  int panel_y = sh - panel_h - 16; if (panel_y < 12) panel_y = 12;
  vk_rect(sw, sh, margin-8, panel_y-8, sw-margin*2+16, panel_h+16, 0.02f,0.03f,0.04f,1.0f);
  vk_rect(sw, sh, margin, panel_y, sw-margin*2, 36, 0.12f,0.13f,0.14f,1.0f);
  char line[96]; snprintf(line, sizeof line, "%s", jni_softinput_text());
  vk_text(sw, sh, margin+12, panel_y+10, line[0] ? line : "PLAYER", 3, 0.90f,0.92f,0.95f);
  int selected = g_vkbd_sel;
  for (int i=0; i<VK_NKEYS; i++) {
    int row=i/VK_COLS, col=i%VK_COLS;
    int x=margin+col*(keyw+gap), y=panel_y+46+row*(keyh+gap);
    float br = 0.20f, bg = 0.22f, bb = 0.24f;
    if (i == selected) { br = 0.86f; bg = 0.70f; bb = 0.18f; }
    vk_rect(sw, sh, x, y, keyw, keyh, br,bg,bb,1.0f);
    vk_rect(sw, sh, x+2, y+2, keyw-4, keyh-4, i==selected ? 0.18f : 0.08f, i==selected ? 0.16f : 0.09f, i==selected ? 0.04f : 0.10f, 1.0f);
    int scale = (!strcmp(vk_keys[i],"DEL") || !strcmp(vk_keys[i],"CXL")) ? 2 : 3;
    int tx = x + (keyw - (int)strlen(vk_keys[i])*6*scale) / 2;
    int ty = y + (keyh - 7*scale) / 2;
    vk_text(sw, sh, tx, ty, vk_keys[i], scale, 0.95f,0.96f,0.96f);
  }
  vk_gl_end(osc, occ, oen);
}
/* relocador de ADRP p/ trampolins (corrige o page-relative ao copiar p/ outro endereço) */
static uint32_t ter_reloc_insn(uint32_t insn, uintptr_t opc, uintptr_t npc) {
  if ((insn & 0x9F000000u) == 0x90000000u) {   /* ADRP */
    uint32_t immlo=(insn>>29)&3, immhi=(insn>>5)&0x7FFFF;
    int64_t imm=(int64_t)((immhi<<2)|immlo); if(imm&(1<<20)) imm-=(1<<21);
    uintptr_t target=(opc & ~0xFFFUL)+((uintptr_t)imm<<12);
    int64_t nimm=((int64_t)(target & ~0xFFFUL)-(int64_t)(npc & ~0xFFFUL))>>12;
    uint32_t nlo=nimm&3, nhi=(nimm>>2)&0x7FFFF;
    return (insn & 0x9F00001Fu)|(nlo<<29)|(nhi<<5);
  }
  return insn;  /* (stp/sub/mov etc. são position-independent) */
}
/* hook inline genérico: copia 4 instrs (relocando adrp) p/ um trampolim que segue p/ target+16,
   e patcha a entrada do alvo p/ saltar p/ fn. Retorna o trampolim (=original) em *orig_out. */
int ter_install_hook4(unsigned long off, void* fn, void** orig_out) {
  uintptr_t target=g_il2cpp_base+off; uint32_t*o=(uint32_t*)target; long pgsz=sysconf(_SC_PAGESIZE);
  uint32_t*tr=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(tr==MAP_FAILED) return 0;
  for(int i=0;i<4;i++) tr[i]=ter_reloc_insn(o[i], target+i*4, (uintptr_t)tr+i*4);
  tr[4]=0x58000050u; tr[5]=0xD61F0200u; *(uint64_t*)(tr+6)=(uint64_t)(target+16);
  __builtin___clear_cache((char*)tr,(char*)tr+32);
  *orig_out=(void*)tr;
  void*pa=(void*)(target & ~((uintptr_t)pgsz-1));
  mprotect(pa,pgsz*2,PROT_READ|PROT_WRITE|PROT_EXEC);
  o[0]=0x58000050u; o[1]=0xD61F0200u; *(uint64_t*)(o+2)=(uint64_t)(uintptr_t)fn;
  mprotect(pa,pgsz*2,PROT_READ|PROT_EXEC); __builtin___clear_cache((char*)pa,(char*)pa+16);
  return 1;
}

static int ter_patch_inst32(uintptr_t target, uint32_t insn, const char *tag) {
  if (!target) return 0;
  long pgsz = sysconf(_SC_PAGESIZE);
  void *pa = (void *)(target & ~((uintptr_t)pgsz - 1));
  uint32_t old = *(uint32_t *)target;
  if (mprotect(pa, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    fprintf(stderr, "[PATCH32] %s target=%p mprotect RWX falhou errno=%d\n",
            tag ? tag : "?", (void *)target, errno);
    fsync(2);
    return 0;
  }
  *(uint32_t *)target = insn;
  __builtin___clear_cache((char *)target, (char *)target + 4);
  mprotect(pa, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC);
  __builtin___clear_cache((char *)pa, (char *)pa + pgsz);
  fprintf(stderr, "[PATCH32] %s target=%p old=0x%08x new=0x%08x\n",
          tag ? tag : "?", (void *)target, old, *(uint32_t *)target);
  fsync(2);
  return 1;
}

static int ter_patch_il2cpp_ret(uintptr_t off, const char *tag) {
  return g_il2cpp_base ? ter_patch_inst32(g_il2cpp_base + off, 0xd65f03c0u, tag) : 0;
}
static volatile void *g_player_create_menu_inst, *g_world_create_menu_inst, *g_player_name_menu_inst;
static void ter_invoke0(const char *ns, const char *cn, const char *mn, void *obj);
/* UX "campo = Criar": frame do commit do nome; o Draw do menu de criacao dispara a
   criacao ~15 frames depois (deixa o teclado fechar/estado assentar). -1 = nada. */
static volatile int g_pending_create_player = -1, g_pending_create_world = -1;
/* menuMode do menu de criacao (capturado no Draw dele) p/ o BOUNCE da tela de nome:
   o botao Criar so troca o menuMode p/ a tela "Digite o Nome" (teclado PC morto);
   devolvemos o menuMode na hora + disparamos a criacao = Criar CRIA direto. */
static volatile int g_menumode_create_p = -1, g_menumode_create_w = -1;
static volatile void *g_world_name_menu_inst;
static volatile int g_world_name_menu_frame = -999999;
static volatile void *g_menu_nameedit_inst;
static volatile int g_player_create_menu_frame = -999999, g_world_create_menu_frame = -999999,
                    g_player_name_menu_frame = -999999, g_menu_nameedit_frame = -999999;
static void (*g_orig_player_create_draw)(void*, void*);
static void (*g_orig_player_name_draw)(void*, void*);
static void (*g_orig_world_create_draw)(void*, void*);
static void (*g_orig_nameedit_enable)(void*, void*, void*);
static void (*g_orig_player_create_save)(void*, void*);
static void (*g_orig_player_create_player)(void*, void*);
static void (*g_orig_world_create)(void*, void*);

static void ter_player_create_draw_hook(void *self, void *mi) {
  g_player_create_menu_inst = self;
  g_player_create_menu_frame = g_render_frame;
  { int (*getmm)(void *) = (int (*)(void *))(g_il2cpp_base + 0xF79984);
    g_menumode_create_p = getmm(NULL); }
  if (g_pending_create_player >= 0 && !ter_vkbd_blocking() &&
      g_render_frame - g_pending_create_player > 15) {
    g_pending_create_player = -1;
    fprintf(stderr, "[AUTONAME] nome confirmado no menu -> CreateAndSave (campo = Criar)\n"); fsync(2);
    ter_invoke0("", "GUIPlayerCreateMenu", "CreateAndSave", self);
  }
  if (g_orig_player_create_draw) g_orig_player_create_draw(self, mi);
}
/* 🔑 A tela de nome usa o caminho de texto ESTILO PC: GetInputText(oldString, region, ...)
   @0xFCD98C — le o teclado FNA (morto no so-loader), ZERA Main.inputTextEnter a cada
   chamada (por isso setar a flag de fora nunca funcionou) e devolve o texto digitado
   (por isso o OSK nao aparecia no campo). Hook com call-through: quando o auto-enter
   esta armado (g_name_enter_frames>0, setado pelo pump), devolvemos NOSSO nome e
   ligamos inputTextEnter DEPOIS do original (que acabou de zerar) -> a tela aceita
   nativamente (CreateAndSave/CreateWorld + transicao). Roda na thread do jogo. */
static volatile int g_name_enter_frames;
static void *(*g_orig_getinputtext)(long,long,long,long,long,long,long,long,long,long);
static void *ter_getinputtext_hook(long a0,long a1,long a2,long a3,long a4,
                                   long a5,long a6,long a7,long a8,long a9) {
  void *r = g_orig_getinputtext ? (void *)g_orig_getinputtext(a0,a1,a2,a3,a4,a5,a6,a7,a8,a9)
                                : (void *)a0;
  if (g_name_enter_frames > 0 && g_il2cpp_base) {
    g_name_enter_frames--;
    const char *nm = g_vkbd_force_text[0] ? g_vkbd_force_text :
                     (getenv("TER_VK_DEFAULT") ? getenv("TER_VK_DEFAULT") : "Player");
    void *(*isn)(const char *) = (void *(*)(const char *))(g_il2cpp_base + 0x73cc98);
    /* RVAs CONFERIDOS no dump: set_chatText=0xF74E44, set_inputTextEnter=0xF74EF0
       (0xf74e9c/0xf74f4c eram os GETTERS — a 1a tentativa era no-op). Estamos na
       thread do jogo (chamados pelo Draw) -> thread-static ok. */
    void (*set_chat)(void *, void *) = (void (*)(void *, void *))(g_il2cpp_base + 0xF74E44);
    void (*set_enter)(int, void *) = (void (*)(int, void *))(g_il2cpp_base + 0xF74EF0);
    void *ns = isn(nm);
    set_chat(ns, NULL);
    set_enter(1, NULL);
    static int logged;
    if (logged++ < 8) { fprintf(stderr, "[AUTONAME] GetInputText -> \"%s\" + ENTER\n", nm); fsync(2); }
    return ns;
  }
  return r;
}
/* 🏆 FIX DEFINITIVO da 2a tela de nome ("Digite o Nome do Personagem"): ela e o caminho
   de teclado FISICO do Terraria PC (GetInputText/inputTextEnter) que esta morto no
   so-loader — ja provado que nem o GetInputText dela roda. Em vez de reanimar a tela,
   PULAMOS ela: o botao Criar chama EnterName() -> substituimos por CreateAndSave/
   CreateWorld diretos (mesmo caminho validado do MAKECLEANPLAYER). O nome usado e o
   ja digitado no campo (g_vkbd_force_text, via hook do CreateAndSave/CreateWorld) ou
   TER_VK_DEFAULT. Tela morta nunca mais aparece. */
static void ter_invoke0(const char *ns, const char *cn, const char *mn, void *obj);
static void ter_il2cpp_set_string_field(void *obj, size_t off, void *s);

static void (*g_orig_player_entername)(void*, void*);
static void (*g_orig_world_entername)(void*, void*);
static void ter_player_entername_hook(void *self, void *mi) {
  (void)mi;
  fprintf(stderr, "[AUTONAME] Criar(player): EnterName -> CreateAndSave direto\n"); fsync(2);
  ter_invoke0("", "GUIPlayerCreateMenu", "CreateAndSave", self);
}
static void ter_world_entername_hook(void *self, void *mi) {
  (void)mi;
  fprintf(stderr, "[AUTONAME] Criar(mundo): EnterName -> CreateWorld direto\n"); fsync(2);
  ter_invoke0("", "GUIWorldCreateMenu", "CreateWorld", self);
}
static void (*g_orig_world_name_draw)(void*, void*);
/* RENOMEAR: GUIPlayerSelectMenu (tela "Selecionar Personagem", botao Renomear).
   OpenNameEdit abre o GUIMenuNameEdit; CloseNameEditAndSave@0xD3899C pega
   _editedName e chama PlayerFileData.Rename. O teclado PC esta morto -> escrevemos
   _editedName (0x10) em TEMPO REAL (aparece na tela) e, no OK, disparamos o save. */
static volatile void *g_player_select_inst; static volatile int g_player_select_frame = -999999;
static volatile int g_pending_rename = -1;   /* frame do OK; save dispara no Draw (thread do jogo) */
static void (*g_orig_player_select_draw)(void*, void*);
static void ter_player_select_draw_hook(void *self, void *mi) {
  g_player_select_inst = self;
  g_player_select_frame = g_render_frame;
  /* tempo real: enquanto o OSK esta aberto num rename, reflete o texto no editor */
  if (jni_softinput_active() && g_menu_nameedit_inst &&
      g_render_frame - g_menu_nameedit_frame < 600 && g_il2cpp_base) {
    const char *t = jni_softinput_text();
    if (t && t[0]) {
      void *(*isn)(const char *) = (void *(*)(const char *))(g_il2cpp_base + 0x73cc98);
      ter_il2cpp_set_string_field((void *)g_menu_nameedit_inst, 0x10, isn(t));  /* _editedName */
    }
  }
  if (g_pending_rename >= 0 && !ter_vkbd_blocking() && g_render_frame - g_pending_rename > 8) {
    g_pending_rename = -1;
    fprintf(stderr, "[VKBD] rename: CloseNameEditAndSave (grava .plr)\n"); fsync(2);
    ter_invoke0("", "GUIPlayerSelectMenu", "CloseNameEditAndSave", self);
  }
  if (g_orig_player_select_draw) g_orig_player_select_draw(self, mi);
}
static void ter_world_name_draw_hook(void *self, void *mi) {
  g_world_name_menu_inst = self;
  g_world_name_menu_frame = g_render_frame;
  if (g_menumode_create_w >= 0 && g_render_frame - g_world_create_menu_frame < 300) {
    void (*setmm)(int, void *) = (void (*)(int, void *))(g_il2cpp_base + 0xF799D8);
    setmm(g_menumode_create_w, NULL);
    g_pending_create_world = g_render_frame;
    fprintf(stderr, "[AUTONAME] Criar(mundo): bounce menuMode->%d + criacao agendada\n", g_menumode_create_w); fsync(2);
    return;
  }
  { static int upct; static int last = -999999;
    if (!jni_softinput_active()) upct++; else upct = 0;
    if (upct >= 30 && g_render_frame - last > 300) {
      last = g_render_frame; upct = 0;
      jni_softinput_open(ter_vkbd_effective_name("Mundo"), 20);
      fprintf(stderr, "[AUTONAME] tela de nome do mundo: abrindo teclado na tela\n"); fsync(2);
    } }
  if (g_orig_world_name_draw) g_orig_world_name_draw(self, mi);
}
static void ter_player_name_draw_hook(void *self, void *mi) {
  g_player_name_menu_inst = self;
  g_player_name_menu_frame = g_render_frame;
  /* Criar apertado: BOUNCE imediato de volta pro menu de criacao + criacao agendada
     (CreateAndSave dispara no Draw do menu, ~15 frames). A tela morta nem renderiza. */
  if (g_menumode_create_p >= 0 && g_render_frame - g_player_create_menu_frame < 300) {
    void (*setmm)(int, void *) = (void (*)(int, void *))(g_il2cpp_base + 0xF799D8);
    setmm(g_menumode_create_p, NULL);
    g_pending_create_player = g_render_frame;
    fprintf(stderr, "[AUTONAME] Criar: bounce menuMode->%d + criacao agendada\n", g_menumode_create_p); fsync(2);
    return;
  }
  /* tela de nome do PERSONAGEM: liga o GUARD de edicao ativa (o byte que um TAP no campo
     ligaria) pela MESMA cadeia que o Draw dela le (disasm 0xD360E4: [[base+0x2fc2cb0]+0]
     ->+0xB8->[deref]->+0x2F0->byte+0x18) e arma o auto-enter. Com o guard=1, o Draw chama
     GetInputText (nosso hook devolve o nome + liga inputTextEnter REAL) e a PROPRIA tela
     aceita: PendingPlayer -> CreateAndSave -> set_menuMode. Tudo codigo nativo dela. */
  { static int upct; static int last = -999999;
    static int osk_opened; static int last_seen = -999999;
    if (g_render_frame - last_seen > 30) osk_opened = 0;   /* nova visita da tela */
    last_seen = g_render_frame;
    if (!jni_softinput_active()) upct++; else upct = 0;
    if (upct >= 30 && g_render_frame - last > 300) {
      last = g_render_frame; upct = 0;
      #define NP_SANE(p) ((uintptr_t)(p) > 0x10000 && (uintptr_t)(p) < 0x8000000000UL)
      void **st = (void **)(g_il2cpp_base + 0x2fc2cb0);
      void *o1 = *st;
      void *o2 = NP_SANE(o1) ? *(void **)((char *)o1 + 0xB8) : NULL;
      void *o3 = NP_SANE(o2) ? *(void **)o2 : NULL;
      void *o4 = NP_SANE(o3) ? *(void **)((char *)o3 + 0x2F0) : NULL;
      fprintf(stderr, "[AUTONAME] chain: o1=%p o2=%p o3=%p o4=%p\n", o1, o2, o3, o4); fsync(2);
      if (NP_SANE(o4)) {
        *((unsigned char *)o4 + 0x18) = 1;   /* guard: edicao ativa */
        g_name_enter_frames = 6;             /* GetInputText hook completa */
        fprintf(stderr, "[AUTONAME] guard de edicao LIGADO + auto-enter armado\n"); fsync(2);
      } else if (!osk_opened) {
        /* Criar direto: o editor de nome nem existe (so nasce ao tocar o campo).
           1o passo: abre o NOSSO teclado NESTA tela p/ digitar/confirmar o nome. */
        osk_opened = 1;
        jni_softinput_open(ter_vkbd_effective_name("Player"), 20);
        fprintf(stderr, "[AUTONAME] tela de nome: abrindo teclado na tela\n"); fsync(2);
      } else if (NP_SANE(o3)) {
        /* 2o passo (pos-OK, editor ainda inexistente): CRIA o GUIMenuNameEdit via
           il2cpp, pendura em LocalUser.Active+0x2F0 (write barrier), liga
           _enabled(+0x18) e poe o nome em _editedName(+0x10) -> o Draw entra no
           caminho de input, GetInputText (hookado) da nome+ENTER e a tela aceita
           nativamente (PendingPlayer -> CreateAndSave -> menuMode). */
        void *(*dom_get2)(void) = (void *)(g_il2cpp_base + 0x73c860);
        const void **(*dom_asms2)(void *, size_t *) = (void *)(g_il2cpp_base + 0x73c86c);
        void *(*asm_img2)(const void *) = (void *)(g_il2cpp_base + 0x73c22c);
        void *(*cls_from_name2)(void *, const char *, const char *) = (void *)(g_il2cpp_base + 0x73c264);
        void *(*cls_method2)(void *, const char *, int) = (void *)(g_il2cpp_base + 0x73c28c);
        void *(*obj_new2)(void *) = (void *)(g_il2cpp_base + 0x73cc34);
        void *(*rt_invoke2)(void *, void *, void **, void **) = (void *)(g_il2cpp_base + 0x73cc7c);
        void *(*isn2)(const char *) = (void *)(g_il2cpp_base + 0x73cc98);
        void *cls = NULL; void *dom = dom_get2();
        if (dom) { size_t na = 0; const void **as = dom_asms2(dom, &na);
          for (size_t ai = 0; as && ai < na; ai++) { void *img = asm_img2(as[ai]); if (!img) continue;
            cls = cls_from_name2(img, "", "GUIMenuNameEdit"); if (cls) break; } }
        void *ne = cls ? obj_new2(cls) : NULL;
        if (ne) {
          void *ct = cls_method2(cls, ".ctor", 0);
          if (ct) { void *exc = NULL; rt_invoke2(ct, ne, NULL, &exc); }
          const char *nm2 = g_vkbd_force_text[0] ? g_vkbd_force_text : ter_vkbd_effective_name("Player");
          ter_il2cpp_set_string_field(ne, 0x10, isn2(nm2));  /* _editedName */
          *((unsigned char *)ne + 0x18) = 1;                 /* _enabled */
          void (*wb2)(void *, void **, void *) = (void *)(g_il2cpp_base + 0x73cb34);
          wb2(o3, (void **)((char *)o3 + 0x2F0), ne);        /* LocalUser.Active.nameEdit = ne */
          g_name_enter_frames = 6;
          fprintf(stderr, "[AUTONAME] GUIMenuNameEdit CRIADO (%p) + guard + auto-enter (nome \"%s\")\n", ne, nm2); fsync(2);
        } else {
          fprintf(stderr, "[AUTONAME] falha ao criar GUIMenuNameEdit (cls=%p)\n", cls); fsync(2);
        }
      }
      #undef NP_SANE
    } }
  if (!g_vkbd_force_text[0])
    snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", ter_vkbd_effective_name("Player"));
  ter_player_name_menu_force_text(g_vkbd_force_text);
  if (g_orig_player_name_draw) g_orig_player_name_draw(self, mi);
  ter_player_name_menu_force_text(g_vkbd_force_text);
}
static void ter_world_create_draw_hook(void *self, void *mi) {
  g_world_create_menu_inst = self;
  g_world_create_menu_frame = g_render_frame;
  { int (*getmm)(void *) = (int (*)(void *))(g_il2cpp_base + 0xF79984);
    g_menumode_create_w = getmm(NULL); }
  if (g_pending_create_world >= 0 && !ter_vkbd_blocking() &&
      g_render_frame - g_pending_create_world > 15) {
    g_pending_create_world = -1;
    fprintf(stderr, "[AUTONAME] nome confirmado no menu -> CreateWorld (campo = Criar)\n"); fsync(2);
    ter_invoke0("", "GUIWorldCreateMenu", "CreateWorld", self);
  }
  if (g_orig_world_create_draw) g_orig_world_create_draw(self, mi);
}
static void ter_nameedit_enable_hook(void *self, void *arg, void *mi) {
  g_menu_nameedit_inst = self;
  g_menu_nameedit_frame = g_render_frame;
  if (g_orig_nameedit_enable) g_orig_nameedit_enable(self, arg, mi);
}
static const char *ter_vkbd_effective_name(const char *fallback) {
  if (g_vkbd_force_text[0]) return g_vkbd_force_text;
  const char *env = getenv("TER_VK_DEFAULT");
  return (env && *env) ? env : fallback;
}
static void ter_player_create_save_hook(void *self, void *mi) {
  g_player_create_menu_inst = self;
  g_player_create_menu_frame = g_render_frame;
  if (!g_vkbd_force_text[0])
    snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", ter_vkbd_effective_name("Player"));
  ter_name_force_text(g_vkbd_force_text);
  ter_force_main_player_name(g_vkbd_force_text);
  fprintf(stderr, "[VKBD] CreateAndSave forcou nome \"%s\"\n", g_vkbd_force_text); fsync(2);
  if (g_orig_player_create_save) g_orig_player_create_save(self, mi);
  ter_name_force_text(g_vkbd_force_text);
  ter_force_main_player_name(g_vkbd_force_text);
}
static void ter_player_create_player_hook(void *self, void *mi) {
  g_player_create_menu_inst = self;
  g_player_create_menu_frame = g_render_frame;
  if (!g_vkbd_force_text[0])
    snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", ter_vkbd_effective_name("Player"));
  ter_name_force_text(g_vkbd_force_text);
  ter_force_main_player_name(g_vkbd_force_text);
  fprintf(stderr, "[VKBD] CreatePlayer forcou nome \"%s\"\n", g_vkbd_force_text); fsync(2);
  if (g_orig_player_create_player) g_orig_player_create_player(self, mi);
  ter_name_force_text(g_vkbd_force_text);
  ter_force_main_player_name(g_vkbd_force_text);
}
static void ter_world_create_hook(void *self, void *mi) {
  g_world_create_menu_inst = self;
  g_world_create_menu_frame = g_render_frame;
  if (!g_vkbd_force_text[0])
    snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", ter_vkbd_effective_name("MUNDO"));
  ter_name_force_text(g_vkbd_force_text);
  fprintf(stderr, "[VKBD] CreateWorld forcou nome \"%s\"\n", g_vkbd_force_text); fsync(2);
  if (g_orig_world_create) g_orig_world_create(self, mi);
}
unsigned long ter_method_off(const char *ns, const char *cn, const char *mn, int argc) {
  if (!g_il2cpp_base) return 0;
  void *(*dom_get)(void) = (void *(*)(void))ter_il2cpp_sym_cached("il2cpp_domain_get");
  const void **(*dom_asms)(void*, size_t*) =
      (const void **(*)(void*, size_t*))ter_il2cpp_sym_cached("il2cpp_domain_get_assemblies");
  void *(*asm_img)(const void*) =
      (void *(*)(const void*))ter_il2cpp_sym_cached("il2cpp_assembly_get_image");
  void *(*cls_from_name)(void*, const char*, const char*) =
      (void *(*)(void*, const char*, const char*))ter_il2cpp_sym_cached("il2cpp_class_from_name");
  void *(*cls_method)(void*, const char*, int) =
      (void *(*)(void*, const char*, int))ter_il2cpp_sym_cached("il2cpp_class_get_method_from_name");
  if (!dom_get || !dom_asms || !asm_img || !cls_from_name || !cls_method) return 0;
  void *domain = dom_get(); if (!domain) return 0;
  size_t na=0; const void **as = dom_asms(domain, &na); if (!as) return 0;
  for (size_t i=0; i<na; i++) {
    void *img = asm_img(as[i]); if (!img) continue;
    void *cls = cls_from_name(img, ns, cn); if (!cls) continue;
    void *m = cls_method(cls, mn, argc); if (!m) return 0;
    void *mp = *(void**)m; if (!mp) return 0;
    return (unsigned long)((uintptr_t)mp - g_il2cpp_base);
  }
  return 0;
}
static void ter_name_hooks_install(void) {
  static int done, tries;
  if (done || !g_il2cpp_base || (!getenv("TER_OSK") && !getenv("TER_AUTONAME"))) return;
  if (tries++ > 600) { done = 1; return; }
  unsigned long po = ter_method_off("", "GUIPlayerCreateMenu", "Draw", 0);
  unsigned long pno = ter_method_off("", "GUIPlayerNameMenu", "Draw", 0);
  unsigned long wo = ter_method_off("", "GUIWorldCreateMenu", "Draw", 0);
  unsigned long psd = ter_method_off("", "GUIPlayerSelectMenu", "Draw", 0);
  if (psd && !g_orig_player_select_draw &&
      ter_install_hook4(psd, (void*)ter_player_select_draw_hook, (void**)&g_orig_player_select_draw))
    fprintf(stderr, "[VKBD] GUIPlayerSelectMenu.Draw hookado @0x%lx (rename)\n", psd);
  unsigned long wnn = ter_method_off("", "GUIWorldNameMenu", "Draw", 0);
  if (!g_orig_getinputtext &&
      ter_install_hook4(0xFCD98C, (void*)ter_getinputtext_hook, (void**)&g_orig_getinputtext))
    fprintf(stderr, "[VKBD] GetInputText @0xFCD98C hookado (auto-enter)\n");
  unsigned long pen = ter_method_off("", "GUIPlayerCreateMenu", "EnterName", 0);
  unsigned long wen = ter_method_off("", "GUIWorldCreateMenu", "EnterName", 0);
  if (pen && !g_orig_player_entername &&
      ter_install_hook4(pen, (void*)ter_player_entername_hook, (void**)&g_orig_player_entername))
    fprintf(stderr, "[VKBD] GUIPlayerCreateMenu.EnterName hookado @0x%lx (Criar direto)\n", pen);
  if (wen && !g_orig_world_entername &&
      ter_install_hook4(wen, (void*)ter_world_entername_hook, (void**)&g_orig_world_entername))
    fprintf(stderr, "[VKBD] GUIWorldCreateMenu.EnterName hookado @0x%lx (Criar direto)\n", wen);
  if (wnn && !g_orig_world_name_draw &&
      ter_install_hook4(wnn, (void*)ter_world_name_draw_hook, (void**)&g_orig_world_name_draw))
    fprintf(stderr, "[VKBD] GUIWorldNameMenu.Draw hookado @0x%lx\n", wnn);
  unsigned long no = ter_method_off("", "GUIMenuNameEdit", "Enable", 1);
  unsigned long ps = ter_method_off("", "GUIPlayerCreateMenu", "CreateAndSave", 0);
  unsigned long pc = ter_method_off("", "GUIPlayerCreateMenu", "CreatePlayer", 0);
  unsigned long wc = ter_method_off("", "GUIWorldCreateMenu", "CreateWorld", 0);
  if (po && !g_orig_player_create_draw &&
      ter_install_hook4(po, (void*)ter_player_create_draw_hook, (void**)&g_orig_player_create_draw))
    fprintf(stderr, "[VKBD] GUIPlayerCreateMenu.Draw hookado @0x%lx\n", po);
  if (pno && !g_orig_player_name_draw &&
      ter_install_hook4(pno, (void*)ter_player_name_draw_hook, (void**)&g_orig_player_name_draw))
    fprintf(stderr, "[VKBD] GUIPlayerNameMenu.Draw hookado @0x%lx\n", pno);
  if (wo && !g_orig_world_create_draw &&
      ter_install_hook4(wo, (void*)ter_world_create_draw_hook, (void**)&g_orig_world_create_draw))
    fprintf(stderr, "[VKBD] GUIWorldCreateMenu.Draw hookado @0x%lx\n", wo);
  if (no && !g_orig_nameedit_enable &&
      ter_install_hook4(no, (void*)ter_nameedit_enable_hook, (void**)&g_orig_nameedit_enable))
    fprintf(stderr, "[VKBD] GUIMenuNameEdit.Enable hookado @0x%lx\n", no);
  if (ps && !g_orig_player_create_save &&
      ter_install_hook4(ps, (void*)ter_player_create_save_hook, (void**)&g_orig_player_create_save))
    fprintf(stderr, "[VKBD] GUIPlayerCreateMenu.CreateAndSave hookado @0x%lx\n", ps);
  if (pc && !g_orig_player_create_player &&
      ter_install_hook4(pc, (void*)ter_player_create_player_hook, (void**)&g_orig_player_create_player))
    fprintf(stderr, "[VKBD] GUIPlayerCreateMenu.CreatePlayer hookado @0x%lx\n", pc);
  if (wc && !g_orig_world_create &&
      ter_install_hook4(wc, (void*)ter_world_create_hook, (void**)&g_orig_world_create))
    fprintf(stderr, "[VKBD] GUIWorldCreateMenu.CreateWorld hookado @0x%lx\n", wc);
  if (g_orig_player_create_draw && g_orig_player_name_draw && g_orig_world_create_draw && g_orig_nameedit_enable &&
      g_orig_player_create_save && g_orig_player_create_player && g_orig_world_create) {
    done = 1; fsync(2);
  }
}
static void ter_il2cpp_set_string_field(void *obj, size_t off, void *s) {
  if (!obj || !s) return;
  void **slot = (void **)((char *)obj + off);
  void (*wb)(void *, void **, void *) = (void *)(g_il2cpp_base + 0x73cb34);
  wb(obj, slot, s);
}
static void ter_player_name_menu_force_text(const char *text) {
  if (!g_il2cpp_base || !text || !text[0]) return;
  int fnm = g_render_frame - g_player_name_menu_frame;
  void *inst = (void *)g_player_name_menu_inst;
  if (!inst || fnm < 0 || fnm >= 900) return;
  void *(*isn)(const char *) = (void *(*)(const char *))(g_il2cpp_base + 0x73cc98);
  void *s = isn(text);
  ter_il2cpp_set_string_field(inst, 0x18, s);  /* GUIPlayerNameMenu.editPlayerName */
  ter_force_main_player_name(text);
  if (getenv("TER_VKBDLOG")) {
    fprintf(stderr, "[VKBD] GUIPlayerNameMenu.editPlayerName=\"%s\" inst=%p\n", text, inst);
    fsync(2);
  }
}
void *ter_static_obj(const char *ns, const char *cn, const char *fn) {
  if (!g_il2cpp_base) return NULL;
  void *(*dom_get)(void) = (void *(*)(void))ter_il2cpp_sym_cached("il2cpp_domain_get");
  const void **(*dom_asms)(void*, size_t*) =
      (const void **(*)(void*, size_t*))ter_il2cpp_sym_cached("il2cpp_domain_get_assemblies");
  void *(*asm_img)(const void*) =
      (void *(*)(const void*))ter_il2cpp_sym_cached("il2cpp_assembly_get_image");
  void *(*cls_from_name)(void*, const char*, const char*) =
      (void *(*)(void*, const char*, const char*))ter_il2cpp_sym_cached("il2cpp_class_from_name");
  void *(*getf)(void*, const char*) =
      (void *(*)(void*, const char*))ter_il2cpp_sym_cached("il2cpp_class_get_field_from_name");
  void (*sget)(void*,void*) =
      (void (*)(void*,void*))ter_il2cpp_sym_cached("il2cpp_field_static_get_value");
  if (!dom_get || !dom_asms || !asm_img || !cls_from_name || !getf || !sget) return NULL;
  void *domain = dom_get(); if (!domain) return NULL;
  size_t na=0; const void **as = dom_asms(domain, &na); if (!as) return NULL;
  for (size_t i=0; i<na; i++) {
    void *img = asm_img(as[i]); if (!img) continue;
    void *cls = cls_from_name(img, ns, cn); if (!cls) continue;
    void *fld = getf(cls, fn); if (!fld) return NULL;
    void *obj = NULL; sget(fld, &obj); return obj;
  }
  return NULL;
}
static void ter_force_player_obj_name(void *player, void *s) {
  if (!player || !s) return;
  ter_il2cpp_set_string_field(player, 0xe0, s);  /* Terraria.Player.name */
}
static void ter_force_main_player_name(const char *text) {
  if (!g_il2cpp_base || !text || !text[0]) return;
  void *(*isn)(const char *) = (void *(*)(const char *))(g_il2cpp_base + 0x73cc98);
  void *s = isn(text);
  int hits = 0;
  void *client = ter_static_obj("Terraria", "Main", "clientPlayer");
  if (client) { ter_force_player_obj_name(client, s); hits++; }
  void *arr = ter_static_obj("Terraria", "Main", "player");
  if (arr) {
    size_t len = *(size_t *)((char *)arr + 0x18);
    if (len > 256) len = 256;
    void **vec = (void **)((char *)arr + 0x20);
    for (size_t i=0; i<len; i++) if (vec[i]) {
      ter_force_player_obj_name(vec[i], s);
      hits++;
    }
  }
  if (getenv("TER_VKBDLOG")) {
    fprintf(stderr, "[VKBD] Terraria.Player.name=\"%s\" aplicado em %d instancia(s)\n", text, hits);
    fsync(2);
  }
}
static void ter_name_force_text(const char *text) {
  if (!g_il2cpp_base || !text || !text[0]) return;
  void *(*isn)(const char *) = (void *(*)(const char *))(g_il2cpp_base + 0x73cc98);
  void *s = isn(text);
  int fp = g_render_frame - g_player_create_menu_frame;
  int fw = g_render_frame - g_world_create_menu_frame;
  int fn = g_render_frame - g_menu_nameedit_frame;
  void *pinst = (void *)g_player_create_menu_inst;
  void *winst = (void *)g_world_create_menu_inst;
  void *ninst = (void *)g_menu_nameedit_inst;
  if (ninst && fn >= 0 && fn < 900) ter_il2cpp_set_string_field(ninst, 0x10, s);
  if (pinst && fp >= 0 && fp < 900) {
    ter_il2cpp_set_string_field(pinst, 0x80, s);
    ter_il2cpp_set_string_field(pinst, 0x88, s);
    *(unsigned char *)((char *)pinst + 0x18) = 0;
    *(unsigned char *)((char *)pinst + 0x19) = 0;
  }
  if (winst && fw >= 0 && fw < 900) ter_il2cpp_set_string_field(winst, 0x70, s);
  ter_player_name_menu_force_text(text);
  ter_force_main_player_name(text);
}
static void ter_invoke0(const char *ns, const char *cn, const char *mn, void *obj) {
  if (!g_il2cpp_base || !obj) return;
  void *(*dom_get)(void) = (void*)(g_il2cpp_base + 0x73c860);
  const void **(*dom_asms)(void*, size_t*) = (void*)(g_il2cpp_base + 0x73c86c);
  void *(*asm_img)(const void*) = (void*)(g_il2cpp_base + 0x73c22c);
  void *(*cls_from_name)(void*, const char*, const char*) = (void*)(g_il2cpp_base + 0x73c264);
  void *(*cls_method)(void*, const char*, int) = (void*)(g_il2cpp_base + 0x73c28c);
  void *(*rt_invoke)(void*, void*, void**, void**) = (void*)(g_il2cpp_base + 0x73cc7c);
  void *domain = dom_get(); if (!domain) return;
  size_t na=0; const void **as = dom_asms(domain, &na); if (!as) return;
  for (size_t i=0; i<na; i++) {
    void *img = asm_img(as[i]); if (!img) continue;
    void *cls = cls_from_name(img, ns, cn); if (!cls) continue;
    void *m = cls_method(cls, mn, 0); if (!m) return;
    void *exc = NULL; rt_invoke(m, obj, NULL, &exc);
    if (exc) fprintf(stderr, "[VKBD] %s.%s gerou excecao %p\n", cn, mn, exc);
    return;
  }
}
static void ter_name_commit_text(const char *text) {
  if (!g_il2cpp_base) return;
  if (!text || !text[0]) text = getenv("TER_VK_DEFAULT") ? getenv("TER_VK_DEFAULT") : "PLAYER";
  void *(*isn)(const char *) = (void *(*)(const char *))(g_il2cpp_base + 0x73cc98);
  void *s = isn(text);
  int fp = g_render_frame - g_player_create_menu_frame;
  int fw = g_render_frame - g_world_create_menu_frame;
  int fn = g_render_frame - g_menu_nameedit_frame;
  void *pinst = (void *)g_player_create_menu_inst;
  void *winst = (void *)g_world_create_menu_inst;
  void *ninst = (void *)g_menu_nameedit_inst;
  if (ninst && fn >= 0 && fn < 600) {
    ter_il2cpp_set_string_field(ninst, 0x10, s);  /* GUIMenuNameEdit._editedName */
    fprintf(stderr, "[VKBD] name edit aplicado: \"%s\" inst=%p\n", text, ninst);
    /* RENOMEAR: agenda o save p/ o Draw do menu (thread do jogo). Chamar CloseNameEditAndSave
       aqui (thread de SWAP) crasha — codigo il2cpp so roda na thread do jogo. */
    if (g_player_select_inst && g_render_frame - g_player_select_frame < 120) {
      g_pending_rename = g_render_frame;
      fprintf(stderr, "[VKBD] rename agendado (save no Draw)\n"); fsync(2);
      return;
    }
  }
  if (pinst && fp >= 0 && fp < 3600 && (fp <= fw || fw < 0 || fw >= 3600)) {
    /* o fluxo NATIVO pos-teclado e: CloseNameEdit + (se veio do botao Criar) CreateAndSave.
       O jogo faz isso quando o TouchScreenKeyboard reporta Done — o que nosso teclado fake
       nao convence 100%. Entao NOS completamos: le a flag ANTES do Close (0x19 =
       editPlayerNameForCreate), fecha a edicao (0x18 = editingPlayerName) e invoca o
       CreateAndSave original (mesmo caminho do MAKECLEANPLAYER validado). */
    int for_create = *(unsigned char *)((char *)pinst + 0x19);
    if (ninst && fn >= 0 && fn < 600) ter_il2cpp_set_string_field(ninst, 0x10, s);
    ter_invoke0("", "GUIPlayerCreateMenu", "CloseNameEdit", pinst);
    ter_il2cpp_set_string_field(pinst, 0x80, s);  /* _playerName */
    ter_il2cpp_set_string_field(pinst, 0x88, s);  /* editPlayerName */
    *(unsigned char *)((char *)pinst + 0x18) = 0; /* editingPlayerName = false */
    fprintf(stderr, "[VKBD] player name aplicado: \"%s\" inst=%p forCreate=%d\n", text, pinst, for_create); fsync(2);
    /* 🔑 UX "o campo E o Criar": confirmou o nome no menu de criacao -> CRIA.
       Agendado p/ o Draw do PROPRIO menu (thread do jogo, mesmo contexto do botao). */
    g_pending_create_player = g_render_frame;
    return;
  }
  if (winst && fw >= 0 && fw < 3600) {
    int wfor_create = 0;
    void *wn = (void *)g_world_name_menu_inst;
    if (wn && g_render_frame - g_world_name_menu_frame < 900)
      wfor_create = *(unsigned char *)((char *)wn + 0x15);   /* editWorldNameForCreate */
    if (ninst && fn >= 0 && fn < 600) ter_il2cpp_set_string_field(ninst, 0x10, s);
    ter_invoke0("", "GUIWorldCreateMenu", "CloseNameEdit", winst);
    if (wn) *(unsigned char *)((char *)wn + 0x14) = 0;        /* editingWorldName = false */
    ter_il2cpp_set_string_field(winst, 0x70, s);  /* _worldName */
    g_pending_create_world = g_render_frame;
    fprintf(stderr, "[VKBD] world name aplicado: \"%s\" inst=%p\n", text, winst); fsync(2);
    if (wfor_create) {
      ter_invoke0("", "GUIWorldCreateMenu", "CreateWorld", winst);
      fprintf(stderr, "[VKBD] CreateWorld invocado — criacao do mundo iniciada\n"); fsync(2);
    }
    return;
  }
  fprintf(stderr, "[VKBD] OK sem tela de nome ativa: \"%s\" fp=%d fw=%d fn=%d\n", text, fp, fw, fn); fsync(2);
}
/* pump do autoname/vkbd — ANTES vivia dentro de ter_ctrl_feed (só rodava com TER_CTRL=1);
   agora é chamado incondicionalmente do swap-hook (o caminho NATPAD não seta TER_CTRL). */
static void ter_name_pump(void) {
  if (g_vkbd_force_frames > 0) {
    ter_name_force_text(g_vkbd_force_text);
    g_vkbd_force_frames--;
  }
  ter_vkbd_maybe_open();
  if (jni_softinput_active()) ter_vkbd_update();
  if (!jni_softinput_active() && g_vkbd_swallow > 0) g_vkbd_swallow--;
  /* ⚡ AUTO-ENTER da tela de nome (GUIPlayerNameMenu/GUIWorldNameMenu): essa tela e
     estilo Terraria PC — o Draw dela le Main.chatText (texto) + Main.inputTextEnter
     (flag do ENTER fisico) e, ao ver o Enter, ELA MESMA aceita o nome e chama
     CreateAndSave/CreateWorld + transicao (provado no disassembly de 0xD360E4).
     Sem teclado fisico ninguem seta o Enter -> a tela prendia p/ sempre.
     Fix: quando a tela esta desenhando ha ~30 frames SEM softinput ativo, escrevemos
     Main.chatText = nome ja digitado (ou TER_VK_DEFAULT) e Main.inputTextEnter = true
     por 12 frames seguidos (Update do jogo pode limpar a flag; repetir garante que o
     Draw veja true). Setters estaticos por RVA: set_chatText@0xf74e9c,
     set_inputTextEnter@0xf74f4c. */
  {
    void *pn = (void *)g_player_name_menu_inst;
    int fpn = g_render_frame - g_player_name_menu_frame;
    void *wn = (void *)g_world_name_menu_inst;
    int fwn = g_render_frame - g_world_name_menu_frame;
    int screen_up = (pn && fpn >= 0 && fpn < 5) || (wn && fwn >= 0 && fwn < 5);
    static int upct, last_ac = -999999;
    if (screen_up && !jni_softinput_active()) upct++; else upct = 0;
    if (upct >= 30 && g_render_frame - last_ac > 150) {
      last_ac = g_render_frame; upct = 0; g_name_enter_frames = 6;
      fprintf(stderr, "[AUTONAME] tela de nome sem teclado -> injetando chatText+ENTER (\"%s\")\n",
              g_vkbd_force_text[0] ? g_vkbd_force_text : "Player"); fsync(2);
    }
  }
  /* WATCHDOG anti-trava: se o softinput esta ativo mas a tela de nome (GUIMenuNameEdit)
     ja fechou (_enabled=0 em +0x18) ha >90 frames — usuario saiu sem OK/cancelar — cancela.
     Sem isso o ter_vkbd_blocking fica preso e o NATPAD bloqueia TODOS os controles. */
  if (jni_softinput_active() && g_menu_nameedit_inst) {
    void *ne = (void *)g_menu_nameedit_inst;
    int enabled = *(unsigned char *)((char *)ne + 0x18);
    static int offct = 0;
    if (!enabled) {
      if (++offct >= 90) {
        offct = 0; jni_softinput_cancel();
        fprintf(stderr, "[VKBD] watchdog: tela de nome fechou sem OK -> cancelando teclado (controles liberados)\n"); fsync(2);
      }
    } else offct = 0;
  }
  /* TER_AUTONAME: sem teclado — quando o jogo abre a edicao de nome (GUIMenuNameEdit.Enable),
     espera ~40 frames e preenche o default (TER_VK_DEFAULT, senao PLAYER) fechando nativamente
     (CloseNameEdit via ter_name_commit_text). Re-arma a cada nova abertura. */
  if (getenv("TER_AUTONAME") && g_menu_nameedit_inst) {
    static int seen = -12345, fired = 0;
    int fn = g_menu_nameedit_frame;
    if (fn != seen) { seen = fn; fired = 0; }
    if (jni_softinput_active()) { seen = fn; fired = 1; }   /* teclado aberto: usuario digita, nao interfere */
    /* tela PC de nome ativa: quem completa e o auto-enter do GetInputText; fechar a
       edicao aqui mataria o caminho de input da tela. */
    { int fpn2 = g_render_frame - g_player_name_menu_frame;
      int fwn2 = g_render_frame - g_world_name_menu_frame;
      if ((g_player_name_menu_inst && fpn2 >= 0 && fpn2 < 5) ||
          (g_world_name_menu_inst && fwn2 >= 0 && fwn2 < 5)) fired = 1; }
    if (!fired && g_render_frame - fn >= 40) {
      fired = 1;
      /* replica o fluxo COMPLETO do vk_commit_text: forca o texto nos menus por 180
         frames + jni_softinput_commit (o jogo ve o teclado dar "Done" = CONFIRMA) +
         preenche campos/CloseNameEdit. Sem o softinput_commit o jogo fica esperando
         o teclado terminar e nao confirma o nome. */
      const char *nm = getenv("TER_VK_DEFAULT") ? getenv("TER_VK_DEFAULT") : "Player";
      snprintf(g_vkbd_force_text, sizeof g_vkbd_force_text, "%s", nm);
      g_vkbd_force_frames = 180;
      jni_softinput_commit(nm);   /* SEMPRE: dispara os callbacks de "teclado fechou" do Unity */
      ter_name_commit_text(nm);
      g_vkbd_swallow = 24;
      fprintf(stderr, "[AUTONAME] nome \"%s\" confirmado (softinput_commit + CloseNameEdit)\n", nm); fsync(2);
    }
  }
}
static unsigned my_eglSwapBuffers(void *dpy, void *surf) {
  static unsigned long swaps;
  swaps++;
  if (env_on("TER_SWAPLOG") && (swaps <= 12 || (swaps % 60) == 0)) {
    fprintf(stderr, "[SWAP] #%lu f=%d dpy=%p surf=%p\n", swaps, g_render_frame, dpy, surf);
    fsync(2);
  }
  ter_nuke_methods();   /* TER_NUKEKB: neutraliza KeyboardInput.Update (lazy, até achar) */
  ter_fix_singleplayer(); /* TER_FIXSP: neutraliza OldSaveSynchronise.CopyOldSaves (tela preta SP) */
  ter_jobworkers0();    /* TER_JOBWORKERS0: JobWorkerCount=0 -> jobs inline */
  ter_name_hooks_install(); /* TER_OSK: captura telas de nome p/ gravar texto real */
/* 🎮 NATPAD (native_pad.c): o jogo VE um controle Xbox via InControl attach;
     menu/bolsa/gameplay/glifos 100% nativos. */
  ter_name_pump();  /* autoname/vkbd (independe do sistema de controle) */
  { extern void np_frame(void); np_frame(); }  /* 🎮 NATPAD: controle NATIVO via InControl attach */
  rs_present();   /* upscale do FBO lo-res p/ a tela real ANTES do swap */
  ter_vkbd_draw();
  ter_screenshot_maybe();
  if (!r_eglSwapBuffers) r_eglSwapBuffers = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
  return r_eglSwapBuffers ? r_eglSwapBuffers(dpy, surf) : 1;
}
static void *ds_route(const char *nm, void *real) {
  void *w = real;
  if (!nm || !real) return real;
  /* CUP_RENDERSCALE: intercepta o binding da tela + viewport/scissor (independe de DRAWSPY) */
  if (rs_enabled()) {
    if (!strcmp(nm, "glBindFramebuffer")) return (void *)rs_BindFramebuffer;
    if (!strcmp(nm, "glViewport"))        return (void *)rs_Viewport;
    if (!strcmp(nm, "glScissor"))         return (void *)rs_Scissor;
  }
  /* DRAWS: só envolve quando há contagem/diagnóstico (DRAWSPY=ring+glGetIntegerv,
   * DRAWCOUNT=contador leve). Com TEXHALF SOZINHO os draws passam DIRETO (sem wrapper)
   * — era a SANGRIA de performance (ds_enter fazia 4 glGetIntegerv/draw = sync GPU).
   * O fast-path de my_glDrawElements (g_drawdiag=0) só conta; mesmo assim, sem DRAWCOUNT
   * nem envolvemos. */
  if (g_drawdiag || getenv("CUP_DRAWCOUNT")) {
    if (!strcmp(nm, "glDrawElements")) { ds_r_DrawElements = real; return (void *)my_glDrawElements; }
    if (!strcmp(nm, "glDrawArrays"))   { ds_r_DrawArrays = real;   return (void *)my_glDrawArrays; }
    if (!strcmp(nm, "glClear"))        { ds_r_Clear = real;        return (void *)my_glClear; }
    if (!strcmp(nm, "glClearColor"))   { ds_r_ClearColor = real;   return (void *)my_glClearColor; }
  }
  if (!g_drawspy) return real;
  /* TEXTURAS (TEXHALF) — só estas precisam do roteamento em produção */
  if (!strcmp(nm, "glTexImage2D"))   { ds_r_TexImage2D = real;   w = (void *)my_glTexImage2D; }
  else if (!strcmp(nm, "glCompressedTexImage2D")) { ds_r_CompTexImage2D = real; w = (void *)my_glCompTexImage2D; }
  else if (!strcmp(nm, "glCompileShader")) { r_glCompileShader = real; w = (void *)my_glCompileShader; }
  else if (!strcmp(nm, "glLinkProgram"))   { r_glLinkProgram = real;   w = (void *)my_glLinkProgram; }
  else if (!strcmp(nm, "glShaderSource"))  { r_glShaderSource = real;  w = (void *)my_glShaderSource; }
  else if (!strcmp(nm, "glRenderbufferStorage")) { r_glRenderbufferStorage = real; w = (void *)my_glRenderbufferStorage; }
  else if (!strcmp(nm, "glCheckFramebufferStatus")) { r_glCheckFBStatus = real; w = (void *)my_glCheckFramebufferStatus; }
  else if (!strcmp(nm, "glFramebufferTexture2D")) { r_glFBTex2D = real; w = (void *)my_glFramebufferTexture2D; }
  else if (!strcmp(nm, "glFramebufferRenderbuffer")) { r_glFBRb = real; w = (void *)my_glFramebufferRenderbuffer; }
  else if (!strcmp(nm, "glUseProgram")) { r_glUseProgram = real; w = (void *)my_glUseProgram; }
  else if (!strcmp(nm, "glUniformMatrix4fv")) { r_glUniformMatrix4fv = real; w = (void *)my_glUniformMatrix4fv; }
  if (w != real) { fprintf(stderr, "[DS] route %s (real=%p)\n", nm, real); fsync(2); }
  return w;
}
static void ds_init(void) {
  rs_init();   /* CUP_RENDERSCALE: parseia env (o FBO lo-res cria-se lazy no 1º bind) */
  if (getenv("CUP_TEXHALF")) { g_texhalf = atoi(getenv("CUP_TEXHALF")); if (g_texhalf < 2) g_texhalf = 1024; }
  int drawcount = getenv("CUP_DRAWCOUNT") ? 1 : 0;
  if (!getenv("CUP_DRAWSPY") && !drawcount && !g_texhalf && !rs_enabled()) return;
  g_drawspy = (getenv("CUP_DRAWSPY") || g_texhalf || rs_enabled()) ? 1 : 0;
  g_drawdiag = getenv("CUP_DRAWSPY") ? 1 : 0;  /* ⚠️ ring + glGetIntegerv/draw — só em diag */
  g_skipfbo = getenv("CUP_SKIPFBO") ? 1 : 0;
  const char *sp = getenv("CUP_SKIPPROG");
  if (sp) {
    char buf[128]; strncpy(buf, sp, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (char *t = strtok(buf, ","); t && g_nskipprog < 8; t = strtok(NULL, ","))
      g_skipprog[g_nskipprog++] = atoi(t);
  }
  if (g_drawdiag || getenv("CUP_DRAWCOUNT")) { pthread_t th; pthread_create(&th, NULL, ds_watchdog, NULL); }
  fprintf(stderr, "[DS] roteamento ON (texhalf=%d drawdiag=%d drawcount=%d skipfbo=%d)\n",
          g_texhalf, g_drawdiag, drawcount, g_skipfbo);
}

/* my_eglGetProcAddress: o Unity resolve as funções GL/extensões via
 * eglGetProcAddress (PLT→Mali real). Se uma extensão é ANUNCIADA (glGetString
 * EXTENSIONS) mas a função NÃO resolve (NULL), o Unity guarda um ponteiro
 * inválido e CRASHA ao chamá-lo (fault 0x7f10000004). Loga TODAS as resoluções
 * (com NULL destacado) p/ achar a culpada. CUP_NOVAO força NULL p/ as funções de
 * VAO (testa a hipótese de que GL_OES_vertex_array_object é a culpada). */
static void *(*r_eglGetProcAddress)(const char *);
static unsigned g_egp_n = 0;
static void *my_eglGetProcAddress(const char *nm) {
  /* 🔑 egl*: rotear p/ NOSSOS shims (egl_route). Sem isto, o engine pegava o eglChooseConfig
     REAL do Mali via eglGetProcAddress → o Mali Utgard rejeitava os attribs (GLES3/etc.) com
     EGL_BAD_ATTRIBUTE → o GfxDevice do Unity virava NULL-renderer → 0 chamadas GL → TELA PRETA.
     O nosso egl_shim_ChooseConfig ignora os attribs e devolve a config válida da window SDL. */
  if (nm && !strcmp(nm, "eglSwapBuffers") &&
      (rs_enabled() || getenv("TER_SHOT") || getenv("TER_SHOTLIVE") ||
       getenv("TER_SWAPLOG") || getenv("TER_NUKEKB") || getenv("TER_JOBWORKERS0"))) {
    if (g_egp_n++ < 400) fprintf(stderr, "[EGP] %s -> MY %p\n", nm, (void *)my_eglSwapBuffers);
    return (void *)my_eglSwapBuffers;
  }
  if (nm && nm[0] == 'e' && nm[1] == 'g' && nm[2] == 'l') {
    void *e = egl_route(nm);
    if (e) { if (g_egp_n++ < 400) { fprintf(stderr, "[EGP] %s -> SHIM %p\n", nm, e); } return e; }
  }
  if (!r_eglGetProcAddress) r_eglGetProcAddress = dlsym(RTLD_DEFAULT, "eglGetProcAddress");
  void *p = r_eglGetProcAddress ? r_eglGetProcAddress(nm) : NULL;
  if (nm && getenv("CUP_NOVAO") && strstr(nm, "VertexArray")) p = NULL;  /* (vira no-op no Unity) */
  if (g_egp_n++ < 400)
    fprintf(stderr, "[EGP] %s -> %p%s\n", nm ? nm : "(null)", p, p ? "" : "  <== NULL!");
  return ds_route(nm, p);
}

static char g_dl_self, g_dl_il2cpp, g_dl_cri, g_dl_burst, g_dl_mainlib;
static so_module *g_m_unity = NULL, *g_m_il2cpp = NULL;
static so_module *g_m_cri = NULL, *g_m_burst = NULL, *g_m_mainlib = NULL;
static void *ter_il2cpp_sym_cached(const char *nm) {
  if (!nm || !g_m_il2cpp) return NULL;
  static struct { char name[64]; void *ptr; } cache[96];
  static int ncache;
  for (int i = 0; i < ncache; i++)
    if (!strcmp(cache[i].name, nm)) return cache[i].ptr;
  so_module *cur = so_save();
  so_use(g_m_il2cpp);
  void *p = (void *)so_find_addr_safe(nm);
  so_use(cur);
  free(cur);
  if (!p) {
    fprintf(stderr, "[IL2SYM] %s nao encontrado\n", nm);
    fsync(2);
    return NULL;
  }
  if (ncache < (int)(sizeof cache / sizeof cache[0])) {
    snprintf(cache[ncache].name, sizeof cache[ncache].name, "%s", nm);
    cache[ncache].ptr = p;
    ncache++;
  }
  return p;
}
void *ter_il2cpp_sym(const char *nm) { return ter_il2cpp_sym_cached(nm); }

/* ---- probe MemoryManager do libunity (RE: GetMemoryManager=0x3cbe2c) ----
 * gMemoryManager (bss)  vaddr 0x1292B48; cursor da arena estatica vaddr 0x11EF4D0;
 * data segment vaddr 0x11e6000. Detecta corrupcao do singleton entre fases. */
static void mm_probe(const char *tag) {
  if (!g_unity_data) return;
  void *mm  = *(void **)(g_unity_data + (0x1292B48 - 0x11e6000));
  void *cur = *(void **)(g_unity_data + (0x11EF4D0 - 0x11e6000));
  fprintf(stderr, "[MM:%s] gMemoryManager=%p cursor-arena=%p\n", tag, mm, cur);
}

/* ---- spy na entrada do operator-new tagueado (vaddr 0x3cbf2c) ----
 * Na entrada: x0=mgr x1=size x2=align(0x10) x3=kind x4=flag x5=tag-string.
 * O canario estoura nesta funcao durante RecreateGfxState -> capturar a chamada
 * culpada (size/kind gigante). Loga so' qdo g_in_gfx setado (evita flood).
 * O hook clobbera 4 insns; o tramp re-executa e segue em entry+16. */
uintptr_t g_gfx_cont = 0;            /* entry+16 (usado pelo asm) */
uintptr_t g_alloc_ub = 0, g_alloc_ib = 0;
volatile int g_in_gfx = 0;
static unsigned g_ospy_n = 0;
void onew_spy_log(uintptr_t mgr, uintptr_t size, uintptr_t kind, uintptr_t tag);
void onew_spy_log(uintptr_t mgr, uintptr_t size, uintptr_t kind, uintptr_t tag) {
  if (!g_in_gfx) return;
  const char *t = "?";
  if (g_alloc_ub && tag >= g_alloc_ub && tag < g_alloc_ub + 0x11e6000)
    t = (const char *)tag;
  fprintf(stderr, "[ONEW] #%u mgr=%lx size=%lu kind=%lu tag=%s\n",
          ++g_ospy_n, mgr, size, kind, t);
  fflush(stderr);
}
__asm__(
  ".text\n"
  ".global onew_spy_tramp\n"
  "onew_spy_tramp:\n"
  "  stp x29, x30, [sp, #-112]!\n"
  "  stp x0, x1, [sp, #16]\n"
  "  stp x2, x3, [sp, #32]\n"
  "  stp x4, x5, [sp, #48]\n"
  "  stp x6, x7, [sp, #64]\n"
  "  str x8, [sp, #80]\n"
  "  mov x0, x0\n"               /* mgr */
  "  mov x2, x3\n"               /* kind */
  "  mov x3, x5\n"               /* tag */
  "  bl onew_spy_log\n"          /* (mgr,size,kind,tag) */
  "  ldr x8, [sp, #80]\n"
  "  ldp x6, x7, [sp, #64]\n"
  "  ldp x4, x5, [sp, #48]\n"
  "  ldp x2, x3, [sp, #32]\n"
  "  ldp x0, x1, [sp, #16]\n"
  "  ldp x29, x30, [sp], #112\n"
  /* prologo original clobberado (0x3cbf2c..0x3cbf38) */
  "  stp x28, x27, [sp, #-96]!\n"
  "  stp x26, x25, [sp, #16]\n"
  "  stp x24, x23, [sp, #32]\n"
  "  stp x22, x21, [sp, #48]\n"
  "  adrp x17, g_gfx_cont\n"
  "  add x17, x17, :lo12:g_gfx_cont\n"
  "  ldr x17, [x17]\n"
  "  br x17\n"
);
extern void onew_spy_tramp(void);

/* ===== CUP_WAITGATE: FORCEINTEG cirúrgico (só durante WaitForAll) =====
 * O FORCEINTEG global (NOP em 0x872774) integra ops cedo demais até FORA do
 * WaitForAll → corrompe um delegate → crash 0x7f10000004 ~60 frames depois.
 * Aqui só ignoramos o gate de budget QUANDO a main está dentro de
 * WaitForAllAsyncOperationsToComplete (0x873a90, force-complete legítimo).
 *
 * (1) hook 0x873a90 → my_waitall (C): liga g_in_waitall, chama o original
 *     (waitall_orig_tramp re-executa o prólogo clobberado e segue em +16),
 *     desliga o flag. (2) hook do gate 0x871844 → my_gate: se in_waitall=1
 *     retorna 1; senão replica a lógica original (budget 0x871884 AND
 *     (jobmgr==null OR NOT pending 0x6cdad0)). */
volatile int g_in_waitall = 0;
uintptr_t g_waitall_cont = 0;   /* 0x873a90 + 16 (usado pelo asm) */
/* gate replica — usa as bases já capturadas (g_unity_base/g_unity_data) */
int my_gate(void *op);
static int jobs_pending(void) {
  void *mgr = *(void **)(g_unity_data + 0xd3380);  /* job-scheduler 0x12b9380 */
  if (!mgr) return 0;
  return ((int (*)(void *))(g_unity_base + 0x6cdad0))(mgr);
}
int g_gatewait = 0;   /* CUP_GATEWAIT: gate sempre bypassa budget + spin-wait nos jobs */
int my_gate(void *op) {
  if (g_gatewait) {
    /* SEMPRE ignora budget (time-slice quebrado no so-loader), mas ESPERA os jobs
       do worker terminarem — spin com sched_yield (dá CPU aos workers) até jobmgr
       limpar. Mata a race da integração forçada (objeto malformado -> crash $PC=9). */
    for (int i = 0; i < 200000 && jobs_pending(); i++) sched_yield();
    return 1;
  }
  if (g_in_waitall) {
    if (getenv("CUP_GATEJOBS")) return jobs_pending() ? 0 : 1;
    return 1;
  }
  int budget = ((int (*)(void *))(g_unity_base + 0x871884))((char *)op + 0x98);
  if (!budget) return 0;
  return jobs_pending() ? 0 : 1;
}
/* trampolim que re-executa o prólogo clobberado de 0x873a90 e segue em +16 */
__asm__(
  ".text\n"
  ".global waitall_orig_tramp\n"
  "waitall_orig_tramp:\n"
  "  stp x22, x21, [sp, #-48]!\n"   /* 0x873a90 */
  "  stp x20, x19, [sp, #16]\n"     /* 0x873a94 */
  "  stp x29, x30, [sp, #32]\n"     /* 0x873a98 */
  "  add x29, sp, #0x20\n"          /* 0x873a9c */
  "  adrp x17, g_waitall_cont\n"
  "  add x17, x17, :lo12:g_waitall_cont\n"
  "  ldr x17, [x17]\n"
  "  br x17\n"
);
extern long waitall_orig_tramp(void *thiz, long a1);
long my_waitall(void *thiz, long a1);
long my_waitall(void *thiz, long a1) {
  g_in_waitall++;
  long r = waitall_orig_tramp(thiz, a1);
  g_in_waitall--;
  return r;
}

/* ===== CUP_CLAMPSIG: clampa o count do Semaphore::Signal (0x65850c) =====
 * Signal(x0=sem, w1=count) posta sem(x0+4) `count` vezes (loop do-while w19=w1).
 * O count deriva p/ um valor enorme (storm/livelock ~frame 110). Hookamos a entrada
 * e clampamos w1 a um máximo são (>nº real de threads ~20) → mata o storm.
 * Prólogo clobberado (4 stp em 0x65850c..0x658518); o tramp re-executa e segue +16. */
uintptr_t g_signal_cont = 0;   /* 0x65850c + 16 */
static int g_signal_clamp = 4096;  /* passa counts legítimos (~dezenas/centenas), só pega o storm */
static volatile unsigned g_signal_clamps = 0;
__asm__(
  ".text\n"
  ".global signal_orig_tramp\n"
  "signal_orig_tramp:\n"
  "  stp x26, x25, [sp, #-80]!\n"   /* 0x65850c */
  "  stp x24, x23, [sp, #16]\n"     /* 0x658510 */
  "  stp x22, x21, [sp, #32]\n"     /* 0x658514 */
  "  stp x20, x19, [sp, #48]\n"     /* 0x658518 */
  "  adrp x17, g_signal_cont\n"
  "  add x17, x17, :lo12:g_signal_cont\n"
  "  ldr x17, [x17]\n"
  "  br x17\n"
);
extern long signal_orig_tramp(void *sem, long count);
long my_signal(void *sem, long count);
long my_signal(void *sem, long count) {
  int c = (int)count;
  if (c > g_signal_clamp) {
    if (g_signal_clamps++ < 12) {
      /* caller = quem chamou Signal (job-scheduler?) */
      uintptr_t ra = (uintptr_t)__builtin_return_address(0);
      const char *lib = "?"; uintptr_t off = ra;
      if (g_unity_base && ra >= g_unity_base && ra < g_unity_base + text_size) { lib = "libunity"; off = ra - g_unity_base; }
      else if (g_il2cpp_base && ra >= g_il2cpp_base && ra < g_il2cpp_base + 0x3000000) { lib = "libil2cpp"; off = ra - g_il2cpp_base; }
      fprintf(stderr, "[CLAMPSIG] Signal(sem=%p) count=%d (0x%x) -> %d  caller=%s+0x%lx\n",
              sem, c, (unsigned)c, g_signal_clamp, lib, (unsigned long)off);
      /* vizinhança do sem (objeto Semaphore/fila de jobs): contador interno + campos */
      uintptr_t b = ((uintptr_t)sem - 0x20) & ~0x7UL;
      for (long d = 0; d < 0x40; d += 8)
        fprintf(stderr, "[CLAMPSIG]   sem%+ld: %016lx\n", d - 0x20, *(unsigned long *)(b + d));
      fsync(2);
    }
    count = (long)g_signal_clamp;
  }
  return signal_orig_tramp(sem, count);
}

/* ===== CUP_CRSPY: espião das coroutines de boot do CupheadStartScene =====
 * O boot (disclaimer) é dirigido por start_cr (RVA il2cpp 0x9A58D0, iterator $PC
 * em +0xBC) que encadeia: settings load → fonts → preload atlases/music →
 * WaitForUserInputBeforeContinue (RVA 0x9A619C, $PC em +0x1C) → load do título.
 * Logamos transições de $PC p/ ver exatamente em qual passo o boot estaciona. */
uintptr_t g_cr1_cont = 0, g_cr2_cont = 0;
__asm__(
  ".text\n"
  ".global cr1_tramp\n"
  "cr1_tramp:\n"
  "  stp x24, x23, [sp, #-64]!\n"    /* 0x9A58D0 */
  "  stp x22, x21, [sp, #16]\n"
  "  stp x20, x19, [sp, #32]\n"
  "  stp x29, x30, [sp, #48]\n"
  "  adrp x17, g_cr1_cont\n"
  "  add x17, x17, :lo12:g_cr1_cont\n"
  "  ldr x17, [x17]\n"
  "  br x17\n"
  ".global cr2_tramp\n"
  "cr2_tramp:\n"
  "  stp x22, x21, [sp, #-48]!\n"    /* 0x9A619C */
  "  stp x20, x19, [sp, #16]\n"
  "  stp x29, x30, [sp, #32]\n"
  "  add x29, sp, #0x20\n"
  "  adrp x17, g_cr2_cont\n"
  "  add x17, x17, :lo12:g_cr2_cont\n"
  "  ldr x17, [x17]\n"
  "  br x17\n"
);
extern long cr1_tramp(void *it);
extern long cr2_tramp(void *it);
long my_start_cr(void *it);
static const char *il2cpp_classname(void *obj) {
  /* obj->klass (off 0) -> klass->name (off 0x10 nesta versão il2cpp 2017) */
  if (!obj) return "(null)";
  void *klass = *(void **)obj;
  if (!klass || ((uintptr_t)klass >> 40)) return "(?)";
  const char *nm = *(const char **)((char *)klass + 0x10);
  return (nm && ((uintptr_t)nm >> 40) == 0) ? nm : "(?)";
}
void *volatile g_startcr_it = NULL;  /* iterator do start_cr capturado (CUP_DRIVECR) */
/* CUP_GATERESTORE: FORCEINTEG (NOP no gate budget 0x871854/0x872774) só é necessário
 * p/ o FontLoader ($PC 7->8). Forçar integração GLOBAL integra ops cujo worker job não
 * rodou -> objeto malformado (vtable/offset uninit) que crasha depois (Cuphead.Init $PC=9
 * na desserialização do CupheadCore: fault = fragmento de string = heap uninit). Restaura
 * o gate ORIGINAL assim que o $PC passa de 8, ANTES do Cuphead.Init. */
static void restore_gate_once(void) {
  static int done = 0;
  if (done) return;
  done = 1;
  uintptr_t a1 = g_unity_base + 0x871854, a2 = g_unity_base + 0x872774;
  long pg = sysconf(_SC_PAGESIZE);
  uintptr_t p1 = a1 & ~(uintptr_t)(pg - 1), p2 = a2 & ~(uintptr_t)(pg - 1);
  mprotect((void *)p1, pg, PROT_READ | PROT_WRITE | PROT_EXEC);
  if (p2 != p1) mprotect((void *)p2, pg, PROT_READ | PROT_WRITE | PROT_EXEC);
  *(uint32_t *)a1 = 0x360000c0u;  /* tbz w0,#0,0x87186c (original) */
  *(uint32_t *)a2 = 0x360004e0u;  /* tbz w0,#0,0x872810 (original) */
  __builtin___clear_cache((char *)a1, (char *)a1 + 4);
  __builtin___clear_cache((char *)a2, (char *)a2 + 4);
  mprotect((void *)p1, pg, PROT_READ | PROT_EXEC);
  if (p2 != p1) mprotect((void *)p2, pg, PROT_READ | PROT_EXEC);
  fprintf(stderr, "[GATERESTORE] gate budget restaurado (0x871854/0x872774) apos FontLoader\n");
  fsync(2);
}
long my_start_cr(void *it) {
  static int lastpc = -99; static unsigned n, samepc;
  g_startcr_it = it;
  int pc = *(int *)((char *)it + 0xBC);
  if (pc != lastpc)
    { fprintf(stderr, "[CRSPY] start_cr tick#%u $PC=%d f=%d\n", n, pc, g_render_frame); fsync(2); samepc = 0; }
  else if (++samepc <= 4) {
    void *cur = *(void **)((char *)it + 0xB0);
    fprintf(stderr, "[CRSPY] start_cr RE-ENTER $PC=%d (samepc=%u) $cur=%p cls=%s f=%d\n",
            pc, samepc, cur, il2cpp_classname(cur), g_render_frame); fsync(2);
  }
  else if (samepc % 180 == 0) {
    void *cur = *(void **)((char *)it + 0xB0);  /* $current (objeto yieldado) */
    fprintf(stderr, "[CRSPY] start_cr SPIN $PC=%d x%u $current=%p cls=%s f=%d\n",
            pc, samepc, cur, il2cpp_classname(cur), g_render_frame); fsync(2);
  }
  lastpc = pc; n++;
  long r = cr1_tramp(it);
  int pc2 = *(int *)((char *)it + 0xBC);
  void *cur = *(void **)((char *)it + 0xB0);
  if (pc2 != pc) {
    fprintf(stderr, "[CRSPY] start_cr $PC %d -> %d (ret=%ld $cur=%p cls=%s f=%d)\n",
            pc, pc2, r, cur, il2cpp_classname(cur), g_render_frame);
    fsync(2); lastpc = pc2;
    if (pc2 >= 9 && getenv("CUP_GATERESTORE")) restore_gate_once();
  } else if (samepc <= 4) {
    fprintf(stderr, "[CRSPY] start_cr POST $PC=%d ret=%ld $cur=%p cls=%s f=%d\n",
            pc, r, cur, il2cpp_classname(cur), g_render_frame); fsync(2);
  }
  return r;
}
long my_inputwait_cr(void *it);
long my_inputwait_cr(void *it) {
  static int lastpc = -99; static unsigned n;
  int pc = *(int *)((char *)it + 0x1C);
  if (pc != lastpc)
    { fprintf(stderr, "[CRSPY] inputwait tick#%u $PC=%d f=%d\n", n, pc, g_render_frame); fsync(2); }
  lastpc = pc; n++;
  long r = cr2_tramp(it);
  int pc2 = *(int *)((char *)it + 0x1C);
  if (pc2 != pc) {
    fprintf(stderr, "[CRSPY] inputwait $PC %d -> %d (ret=%ld f=%d)\n", pc, pc2, r, g_render_frame);
    fsync(2); lastpc = pc2;
  }
  return r;
}

/* ===== CUP_BOOTSPY: log de entrada nas funções da cadeia de boot (il2cpp) =====
 * Hooks de log genéricos: trampolim runtime copia as 4 insns clobberadas pelo
 * hook_arm64; stp/add/mov copiam direto, adrp é recomputado (ldr-literal com o
 * endereço absoluto da página). Mostra qual elo da cadeia
 * Start→LoadFromCloud→LoadCloudData→OnLoaded→OnSettingsDataLoaded→start_cr morre. */
static uint32_t *bs_page = NULL; static int bs_used = 0;
#define RXD_TRAMP_WORDS (65536 / (int)sizeof(uint32_t))
static void *g_rxd_ui_mgr_good;
static void *g_rxd_ui_mgr_good_go;
static int rxd_ui_has_core(void *obj);
static int rxd_unity_alive(void *obj);
static void *rxd_comp_go(void *comp);
static int rxd_go_active_self(void *go);
static int rxd_go_active_hier(void *go);
static void rxd_go_set_active(void *go, int active);
static uint32_t rxd_gc_pin(void *obj, const char *tag);
static void rxd_dontdestroy_obj(void *obj, const char *tag);
static const char *rxd_str(void *s, char *buf, size_t cap);
static void *mk_tramp(uintptr_t target, const char *name) {
  if (!bs_page) {
    bs_page = mmap(NULL, RXD_TRAMP_WORDS * sizeof(uint32_t),
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bs_page == MAP_FAILED) { bs_page = NULL; return NULL; }
  }
  if (bs_used + 64 >= RXD_TRAMP_WORDS) {
    fprintf(stderr, "[BOOTSPY] %s: tramp arena cheia used=%d words\n",
            name ? name : "?", bs_used);
    fsync(2);
    return NULL;
  }
  uint32_t *st = bs_page + bs_used;
  uint32_t *p = st;
  const uint32_t *src = (const uint32_t *)target;
  for (int i = 0; i < 4; i++) {
    uint32_t in = src[i];
    if ((in & 0x9F000000u) == 0x90000000u) {            /* adrp rd, page */
      int rd = in & 31;
      long immlo = (in >> 29) & 3, immhi = (in >> 5) & 0x7FFFF;
      long imm = (immhi << 2) | immlo;
      if (imm & (1L << 20)) imm -= (1L << 21);          /* sign extend 21 bits */
      uint64_t page = ((target + i * 4) & ~0xFFFUL) + (imm << 12);
      *p++ = 0x58000040u | rd;                          /* ldr rd, +8 */
      *p++ = 0x14000003u;                               /* b +12 */
      *(uint64_t *)p = page; p += 2;
    } else if ((in & 0x7C000000u) == 0x14000000u || (in & 0xFF000000u) == 0x58000000u ||
               (in & 0x7C000000u) == 0x94000000u || (in & 0xFE000000u) == 0x54000000u) {
      fprintf(stderr, "[BOOTSPY] %s: insn %d não-relocável (%08x) — hook ABORTADO\n", name, i, in);
      return NULL;
    } else {
      *p++ = in;                                        /* stp/add/mov etc: PI, copia */
    }
  }
  *p++ = 0x58000051u;                                   /* ldr x17, #8 */
  *p++ = 0xd61f0220u;                                   /* br x17 */
  *(uint64_t *)p = target + 16; p += 2;
  bs_used += (int)(p - st) + (4 - ((p - st) & 3));      /* avança alinhado */
  __builtin___clear_cache((char *)st, (char *)p);
  return st;
}

static void (*rxd_ui_awake_orig)(void *, void *);
static void (*rxd_ui_update_orig)(void *, void *);
static void (*rxd_ui_res_orig)(void *, void *);
static void (*rxd_ui_fading_orig)(void *, int, void *, void *);
static void (*rxd_ui_loading_block_orig)(void *, void *);
static void (*rxd_ui_open_loading_orig)(void *, void *, int, float, void *);
static void (*rxd_ui_close_loading_orig)(void *, void *, float, void *);
static void (*rxd_ui_close_all_orig)(void *, void *, void *);
static void rxd_invoke_callback0(void *cb, const char *tag) {
  if (!cb || !g_il2cpp_base) return;
  void (*invoke)(void *, void *) = (void (*)(void *, void *))(g_il2cpp_base + 0x2DC1638);
  fprintf(stderr, "[RXD_CB0] %s invoke cb=%p f=%d\n", tag ? tag : "Callback", cb, g_render_frame);
  fsync(2);
  invoke(cb, NULL);
  fprintf(stderr, "[RXD_CB0] %s done cb=%p f=%d\n", tag ? tag : "Callback", cb, g_render_frame);
  fsync(2);
}

static void rxd_ui_hide_comp_go(void *comp, const char *tag) {
  void *go = rxd_comp_go(comp);
  if (!go) return;
  int self_active = rxd_go_active_self(go);
  int hier_active = rxd_go_active_hier(go);
  fprintf(stderr,
          "[RXD_UI_LOADING] hide %s comp=%p go=%p self=%d hier=%d f=%d\n",
          tag ? tag : "ui", comp, go, self_active, hier_active, g_render_frame);
  fsync(2);
  rxd_go_set_active(go, 0);
}

static void rxd_ui_force_hide_loading(void *self, const char *reason) {
  if (!rxd_ui_has_core(self)) return;
  void *loading = *(void **)((char *)self + 0x70);
  void *connecting = *(void **)((char *)self + 0x78);
  void *translucent = *(void **)((char *)self + 0x80);
  void *block = *(void **)((char *)self + 0x88);
  fprintf(stderr,
          "[RXD_UI_LOADING] force-hide reason=%s self=%p loading=%p connecting=%p translucent=%p block=%p f=%d\n",
          reason ? reason : "?", self, loading, connecting, translucent, block, g_render_frame);
  fsync(2);
  rxd_ui_hide_comp_go(loading, "loading");
  rxd_ui_hide_comp_go(connecting, "connecting");
  rxd_ui_hide_comp_go(translucent, "translucent");
  rxd_ui_hide_comp_go(block, "block");
}

static void rxd_ui_awake_hook(void *self, void *method) {
  void *joy = self ? *(void **)((char *)self + 0x60) : NULL;
  void *ui_parent = self ? *(void **)((char *)self + 0x68) : NULL;
  void *translucent = self ? *(void **)((char *)self + 0x80) : NULL;
  void *block = self ? *(void **)((char *)self + 0x88) : NULL;
  void *cam = self ? *(void **)((char *)self + 0xA0) : NULL;
  int has_core = joy && ui_parent && translucent && block;
  static unsigned n;
  if (n++ < 32 || !has_core) {
    fprintf(stderr,
            "[RXD_UI_AWAKE] self=%p has=%d joy=%p uiParent=%p translucent=%p block=%p cam=%p f=%d\n",
            self, has_core, joy, ui_parent, translucent, block, cam, g_render_frame);
    fsync(2);
  }
  if (!has_core) return;
  rxd_ui_awake_orig(self, method);
  void *mgr_go = rxd_comp_go(self);
  if (mgr_go) {
    g_rxd_ui_mgr_good_go = mgr_go;
    rxd_gc_pin(mgr_go, "UIManager.go");
  }
  rxd_gc_pin(self, "UIManager");
  if (getenv("TER_RXD_UI_DDOL")) {
    void *loading = self ? *(void **)((char *)self + 0x70) : NULL;
    void *connecting = self ? *(void **)((char *)self + 0x78) : NULL;
    void *confirm = self ? *(void **)((char *)self + 0x98) : NULL;
    rxd_dontdestroy_obj(self, "UIManager.component");
    rxd_dontdestroy_obj(mgr_go, "UIManager.go");
    rxd_dontdestroy_obj(rxd_comp_go(joy), "UIManager.joy.go");
    rxd_dontdestroy_obj(rxd_comp_go(ui_parent), "UIManager.parent.go");
    rxd_dontdestroy_obj(rxd_comp_go(loading), "UIManager.loading.go");
    rxd_dontdestroy_obj(rxd_comp_go(connecting), "UIManager.connecting.go");
    rxd_dontdestroy_obj(rxd_comp_go(translucent), "UIManager.translucent.go");
    rxd_dontdestroy_obj(rxd_comp_go(block), "UIManager.block.go");
    rxd_dontdestroy_obj(rxd_comp_go(confirm), "UIManager.confirm.go");
    rxd_dontdestroy_obj(rxd_comp_go(cam), "UIManager.cam.go");
  }
  if (rxd_ui_has_core(self) && g_rxd_ui_mgr_good != self) {
    g_rxd_ui_mgr_good = self;
    fprintf(stderr, "[RXD_UI_AWAKE] UIManager bom pos-Awake=%p f=%d\n", self, g_render_frame);
    fsync(2);
  }
}

static void rxd_ui_update_hook(void *self, void *method) {
  if (!rxd_ui_has_core(self)) {
    static unsigned n;
    if (n++ < 32 || (g_render_frame % 120) == 0) {
      fprintf(stderr, "[RXD_UI_UPDATE] skip UIManager vazio self=%p f=%d\n", self, g_render_frame);
      fsync(2);
    }
    return;
  }
  rxd_ui_update_orig(self, method);
}

static void rxd_ui_res_hook(void *self, void *method) {
  if (!rxd_ui_has_core(self) || env_on("TER_RXD_UI_SKIP_RES") ||
      (env_on("TER_RXD_NUKE_UI_AWAKE") && !env_on("TER_RXD_UI_ALLOW_RES"))) {
    static unsigned n;
    if (n++ < 32 || (g_render_frame % 120) == 0) {
      fprintf(stderr, "[RXD_UI_RES] skip UpdateResolutionData self=%p has=%d f=%d\n",
              self, rxd_ui_has_core(self), g_render_frame);
      fsync(2);
    }
    return;
  }
  rxd_ui_res_orig(self, method);
}

static void rxd_ui_loading_block_hook(void *self, void *method) {
  if (!rxd_ui_has_core(self)) {
    static unsigned n;
    if (n++ < 48 || (g_render_frame % 120) == 0) {
      fprintf(stderr, "[RXD_UI_BLOCK] skip UpdateLoadingBlock self=%p has=0 f=%d\n",
              self, g_render_frame);
      fsync(2);
    }
    return;
  }
  rxd_ui_loading_block_orig(self, method);
}

static void rxd_ui_fading_hook(void *self, int show, void *text, void *method) {
  void *connecting = self ? *(void **)((char *)self + 0x78) : NULL;
  char b[96];
  static unsigned n;
  if (n++ < 48 || (g_render_frame % 120) == 0) {
    fprintf(stderr,
            "[RXD_UI_FADE] self=%p has=%d connecting=%p show=%d text=%s skip=%d f=%d\n",
            self, rxd_ui_has_core(self), connecting, show, rxd_str(text, b, sizeof b),
            env_on("TER_RXD_NO_CONNECTING") || !rxd_ui_has_core(self) || !connecting,
            g_render_frame);
    fsync(2);
  }
  if (env_on("TER_RXD_NO_CONNECTING") || !rxd_ui_has_core(self) || !connecting) return;
  rxd_ui_fading_orig(self, show, text, method);
}

static void rxd_ui_open_loading_hook(void *self, void *cb, int type, float fade, void *method) {
  int has = rxd_ui_has_core(self);
  int skip = env_on("TER_RXD_NO_LOADINGUI") || !has || !rxd_ui_open_loading_orig;
  fprintf(stderr, "[RXD_UI_LOADING] OpenLoadingUI self=%p cb=%p type=%d fade=%g has=%d orig=%p skip=%d f=%d\n",
          self, cb, type, (double)fade, has, rxd_ui_open_loading_orig, skip, g_render_frame);
  fsync(2);
  if (!skip) {
    rxd_ui_open_loading_orig(self, cb, type, fade, method);
    return;
  }
  if (has && !rxd_ui_open_loading_orig) {
    fprintf(stderr, "[RXD_UI_LOADING] OpenLoadingUI sem trampoline: callback fallback f=%d\n", g_render_frame);
    fsync(2);
  }
  rxd_invoke_callback0(cb, "OpenLoadingUI");
}

static void rxd_ui_close_loading_hook(void *self, void *cb, float fade, void *method) {
  int has = rxd_ui_has_core(self);
  int skip = env_on("TER_RXD_NO_LOADINGUI") || !has || !rxd_ui_close_loading_orig;
  fprintf(stderr, "[RXD_UI_LOADING] CloseLoadingUI self=%p cb=%p fade=%g has=%d orig=%p skip=%d f=%d\n",
          self, cb, (double)fade, has, rxd_ui_close_loading_orig, skip, g_render_frame);
  fsync(2);
  if (!skip) {
    rxd_ui_close_loading_orig(self, cb, fade, method);
    return;
  }
  if (has && !rxd_ui_close_loading_orig) {
    fprintf(stderr, "[RXD_UI_LOADING] CloseLoadingUI sem trampoline: callback fallback f=%d\n", g_render_frame);
    fsync(2);
  }
  if (has) rxd_ui_force_hide_loading(self, "CloseLoadingUI-fallback");
  rxd_invoke_callback0(cb, "CloseLoadingUI");
}

static void rxd_ui_close_all_hook(void *self, void *cb, void *method) {
  int has = rxd_ui_has_core(self);
  int skip = env_on("TER_RXD_NO_LOADINGUI") || !has || !rxd_ui_close_all_orig;
  fprintf(stderr, "[RXD_UI_LOADING] CloseAllUI self=%p cb=%p has=%d orig=%p skip=%d f=%d\n",
          self, cb, has, rxd_ui_close_all_orig, skip, g_render_frame);
  fsync(2);
  if (!skip) {
    rxd_ui_close_all_orig(self, cb, method);
    return;
  }
  if (has && !rxd_ui_close_all_orig) {
    fprintf(stderr, "[RXD_UI_LOADING] CloseAllUI sem trampoline: callback fallback f=%d\n", g_render_frame);
    fsync(2);
  }
  if (has) rxd_ui_force_hide_loading(self, "CloseAllUI-fallback");
  rxd_invoke_callback0(cb, "CloseAllUI");
}

static void rxd_install_ui_awake_hook(uintptr_t base) {
  static int done = 0;
  if (done || !(env_on("TER_RXD_NUKE_UI_AWAKE") || env_on("TER_RXD_UI_GUARD"))) return;
  int ui_guard = env_on("TER_RXD_UI_GUARD");
  int no_loading = env_on("TER_RXD_NO_LOADINGUI");
  rxd_ui_awake_orig = (void (*)(void *, void *))mk_tramp(base + 0x11648D8, "UIManager.Awake");
  rxd_ui_update_orig = (void (*)(void *, void *))mk_tramp(base + 0x1164BCC, "UIManager.Update");
  rxd_ui_res_orig = (void (*)(void *, void *))mk_tramp(base + 0x1164EAC, "UIManager.UpdateResolutionData");
  rxd_ui_fading_orig = (void (*)(void *, int, void *, void *))mk_tramp(base + 0x11634C0, "UIManager.FadingText");
  rxd_ui_loading_block_orig = (void (*)(void *, void *))mk_tramp(base + 0x11667C8, "UIManager.UpdateLoadingBlock");
  rxd_ui_close_all_orig = (void (*)(void *, void *, void *))mk_tramp(base + 0x1165AF4, "UIManager.CloseAllUI");
  rxd_ui_open_loading_orig = (void (*)(void *, void *, int, float, void *))mk_tramp(base + 0x1165C2C, "UIManager.OpenLoadingUI");
  rxd_ui_close_loading_orig = (void (*)(void *, void *, float, void *))mk_tramp(base + 0x1166540, "UIManager.CloseLoadingUI");
  if (!rxd_ui_awake_orig || !rxd_ui_update_orig || !rxd_ui_res_orig ||
      !rxd_ui_fading_orig || !rxd_ui_loading_block_orig) {
    fprintf(stderr, "[RXD_UI_AWAKE] hook abortado: trampoline basico falhou\n");
    fsync(2);
    done = 1;
    return;
  }
  hook_arm64(base + 0x11648D8, (uintptr_t)rxd_ui_awake_hook);
  hook_arm64(base + 0x1164BCC, (uintptr_t)rxd_ui_update_hook);
  hook_arm64(base + 0x1164EAC, (uintptr_t)rxd_ui_res_hook);
  hook_arm64(base + 0x11634C0, (uintptr_t)rxd_ui_fading_hook);
  hook_arm64(base + 0x11667C8, (uintptr_t)rxd_ui_loading_block_hook);
  if (rxd_ui_close_all_orig || no_loading) {
    hook_arm64(base + 0x1165AF4, (uintptr_t)rxd_ui_close_all_hook);
  } else {
    fprintf(stderr, "[RXD_UI_AWAKE] CloseAllUI sem trampoline: hook opcional desligado\n");
  }
  if (rxd_ui_open_loading_orig || no_loading) {
    hook_arm64(base + 0x1165C2C, (uintptr_t)rxd_ui_open_loading_hook);
  } else {
    fprintf(stderr, "[RXD_UI_AWAKE] OpenLoadingUI sem trampoline: hook opcional desligado\n");
  }
  if (rxd_ui_close_loading_orig || ui_guard || no_loading) {
    hook_arm64(base + 0x1166540, (uintptr_t)rxd_ui_close_loading_hook);
  } else {
    fprintf(stderr, "[RXD_UI_AWAKE] CloseLoadingUI sem trampoline: hook opcional desligado\n");
  }
  fprintf(stderr,
          "[RXD_UI_AWAKE] hooks UIManager basicos instalados all=%p open=%p close=%p guard=%d no_loading=%d\n",
          rxd_ui_close_all_orig, rxd_ui_open_loading_orig, rxd_ui_close_loading_orig,
          ui_guard, no_loading);
  fsync(2);
  done = 1;
}

#define BS_WRAP(idx, label) \
  static long (*bs_orig_##idx)(long, long, long, long, long, long, long, long); \
  static long bs_hook_##idx(long a, long b, long c, long d, long e, long f, long g, long h) { \
    static unsigned n; \
    if (n++ < 24) { fprintf(stderr, "[BOOTSPY] %s (#%u) x0=%lx x1=%lx f=%d\n", label, n, a, b, g_render_frame); fsync(2); } \
    return bs_orig_##idx(a, b, c, d, e, f, g, h); \
  }
BS_WRAP(0, "CupheadStartScene.Start")
BS_WRAP(1, "CupheadStartScene.OnSettingsDataLoaded")
BS_WRAP(2, "SettingsData.LoadFromCloud")
BS_WRAP(3, "OnlineInterfaceSteam.LoadCloudData")
BS_WRAP(4, "OnlineManager.Init")
BS_WRAP(5, "SettingsData.Save")
BS_WRAP(6, "CupheadStartScene.start_cr(factory)")
BS_WRAP(7, "SettingsData.OnLoadedCloudData")
/* ===== CUP_MASKGUARD (s12): 2º crash do load do mapa =====
 * libunity 0x8f9914 monta arrays de índice (SpriteMask/Tilemap mesh): recebe a
 * CONTAGEM em w0 e escreve em [obj+128][w0-1]. Na cena do mapa, w0 vem LIXO
 * (~0x10000000) -> escrita fora dos limites -> SIGSEGV em 0x8f9b1c. Mesma raiz do
 * SCENEGUARD (objeto da cena do mapa mal-inicializado no so-loader). Clampa a
 * contagem insana p/ 0 (mask vazia) em vez de estourar. */
/* ===== CUP_SCENESKIP (s12): RAIZ da cadeia de crashes do mapa =====
 * A função 0x541c9c processa o tilemap/mesh de um GameObject; resolve a scene via
 * helper 0x8f7c48 = ldp x8,x1,[arg0,#56] (scene = *(void**)(arg0+56)). No mapa, vários
 * GameObjects têm scene NULL (não registrados na cena pelo so-loader) -> a função
 * deref nulls em cascata (0x541cdc, 0x8f9b1c via 0x8f9914, 0x8f9b88, 0x541e54...).
 * Em vez de remendar cada deref (whack-a-mole), PULA a função inteira quando a scene
 * é NULL: o GameObject não monta o mesh (não renderiza), mas nada crasha. Epílogo é
 * void (caller 0x541c2c ignora o retorno). Substitui a abordagem fake-scene do island. */
static long (*scene541_orig)(long, long, long, long, long, long, long, long);
static volatile uint32_t g_sceneskip_hits, g_sceneadopt_hits;
static void *volatile g_map_scene;   /* último scene handle VÁLIDO visto (p/ adoção) */
static long scene541_hook(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
  void *scene = a0 ? *(void **)((char *)a0 + 56) : NULL;
  if (scene) {
    g_map_scene = scene;   /* captura: objeto bem-registrado da cena do mapa */
    return scene541_orig(a0, a1, a2, a3, a4, a5, a6, a7);
  }
  /* scene==NULL: o objeto (player/rig de câmera) está numa cena que o so-loader NÃO
   * registrou. CUP_SCENEADOPT (opt-IN; default OFF=skip): tentou ADOTAR o objeto na cena
   * válida (escreve scene real em [a0+56]) — mas FALHOU: o objeto está meio-construído
   * (OUTROS campos null tb: idx/tilemap) -> deref selvagem em 0x541cdc (fault wild). Igual
   * à fake-scene antiga. Raiz = integração async da cena aditiva nunca completa, não só o
   * scene-link. Mantido GATED p/ referência; default = SKIP (mapa renderiza sem o player). */
  if (a0 && g_map_scene && getenv("CUP_SCENEADOPT")) {
    *(void **)((char *)a0 + 56) = g_map_scene;
    if (g_sceneadopt_hits < 12)
      fprintf(stderr, "[SCENEADOPT] 0x541c9c scene=NULL -> adotado na cena do mapa (%p, f=%d)\n",
              g_map_scene, g_render_frame);
    g_sceneadopt_hits++;
    return scene541_orig(a0, a1, a2, a3, a4, a5, a6, a7);
  }
  /* ===== CUP_HIERFIX (s14, default ON; CUP_NOHIERFIX desliga) — FIX REAL =====
   * Ground truth s14: a0 é o TRANSFORM do prefab de ORIGEM do Instantiate (0x541c9c roda
   * no source, início do clone worker 0x5424b0) e {P,idx}={NULL,-1} = o prefab carregado
   * dos assets NUNCA foi inserido numa TransformHierarchy (o passo do load que a
   * integração forçada pula; caller normal 0x459230 no caminho de load). Sem P o clone
   * produz NADA -> CloneObject retorna NULL (os DESERGUARD pareados) -> Instantiate=null
   * -> player/câmera/UI do mapa nem EXISTEM.
   * Fix: libunity 0x901164(transform) = rebuild da hierarchy da árvore inteira: sobe à
   * raiz ([T+0x90]), conta a sub-árvore (0x90110c, só anda em filhos [T+0x70/0x80] —
   * null-safe), cria hierarchy (0x8f9914), insere recursivo (0x9012b8, escreve {P,idx}
   * em cada nó), registra no manager global ([0x12c0398]) e destrói a antiga (0x8f9d80,
   * null-safe). Depois disso o clone segue o caminho NORMAL do engine. */
  if (a0 && !getenv("CUP_NOHIERFIX")) {
    static volatile uint32_t hf_n;
    /* raiz da árvore (p/ log; o rebuild já sobe sozinho) */
    void *root = (void *)a0;
    while (*(void **)((char *)root + 0x90)) root = *(void **)((char *)root + 0x90);
    ((void (*)(void *))(g_unity_base + 0x901164))((void *)a0);
    void *P = *(void **)((char *)a0 + 56);
    long idx = *(long *)((char *)a0 + 64);
    if (hf_n < 16)
      fprintf(stderr, "[HIERFIX] 0x901164(rebuild) t=%p root=%p -> P=%p idx=%ld (f=%d)\n",
              (void *)a0, root, P, idx, g_render_frame);
    hf_n++;
    if (P) return scene541_orig(a0, a1, a2, a3, a4, a5, a6, a7);
    /* rebuild não populou — cai no skip de segurança */
  }
  if (g_sceneskip_hits < 8) {
    /* s14: a0 = Transform (RE 0x541b90: Component -> [obj+0x30]=GameObject -> cast).
     * {P=hierarchy, idx} em [a0+0x38/0x40]; +0x30 = GameObject do Component. */
    fprintf(stderr, "[SCENESKIP] 0x541c9c scene=NULL -> skip GO (f=%d) obj=%p go=%p idx=%ld\n",
            g_render_frame, (void *)a0,
            a0 ? *(void **)((char *)a0 + 0x30) : NULL,
            a0 ? *(long *)((char *)a0 + 0x40) : -1);
  }
  g_sceneskip_hits++;
  return 0;
}
/* ===== CUP_NULLGUARD (s12): 3º crash do load do mapa =====
 * libunity 0x8f9b88 (função de tilemap/mesh, chamada de 0x541dcc) faz
 * `ldr x14,[x0,#24]` SEM null-check; no mapa x0 (arg0) vem NULL (deriva da
 * fake-scene do SCENEGUARD) -> SIGSEGV fault=0x18. Skip quando arg0==NULL. */
static long (*nullfn_orig)(long, long, long, long, long, long, long, long);
static volatile uint32_t g_nullguard_hits;
static long nullfn_hook(long a, long b, long c, long d, long e, long f, long g, long h) {
  if (a == 0) {
    if (g_nullguard_hits < 8) fprintf(stderr, "[NULLGUARD] 0x8f9b88 arg0=NULL -> skip (f=%d)\n", g_render_frame);
    g_nullguard_hits++;
    return 0;
  }
  return nullfn_orig(a, b, c, d, e, f, g, h);
}
static long (*maskfn_orig)(long, long, long, long, long, long, long, long);
static volatile uint32_t g_maskguard_hits;
static long maskfn_hook(long a, long b, long c, long d, long e, long f, long g, long h) {
  uint32_t n = (uint32_t)a;
  /* A função faz `w10 = count-1` e escreve array[count-1] SEM checar count>0:
   *   count==0 -> w10 = 0xffffffff -> store OOB gigante -> SIGSEGV em 0x8f9b1c.
   *   count enorme (lixo) -> idem. No mapa do Cuphead aparece count==0 (mesh/tilemap
   *   vazio no so-loader). Clampa p/ [1, 0x40000]: count=1 -> w10=0 -> array[0]
   *   (slot que a função JÁ escreve incondicionalmente em 0x8f9b14, logo existe). */
  if (n == 0 || n > 0x40000u) {
    if (g_maskguard_hits < 8)
      fprintf(stderr, "[MASKGUARD] count=%u (0x%x) -> 1 (f=%d)\n", n, n, g_render_frame);
    g_maskguard_hits++;
    a = 1;
  }
  return maskfn_orig(a, b, c, d, e, f, g, h);
}
/* ===== CUP_DESERGUARD (s13): crash #5 do load do mapa =====
 * libunity 0x54220c (cluster de desserialização da cena; recebe arg0=ptr p/ um par
 * {objeto, ...} na stack) faz `ldr x8,[arg0]` (objeto) e logo `ldr w9,[x8,#0xc]`
 * (lê a classe/type do objeto) SEM null-check. Na cena do MAPA várias referências de
 * objeto não resolvem (PPtr -> NULL) -> *arg0 == NULL -> x8=0 -> SIGSEGV fault=0xc em
 * 0x542258. Pula a função quando *arg0==NULL (o objeto null não é processado; mesmo
 * espírito do SCENESKIP). Caller (0x542474) usa o retorno como ponteiro/flag -> 0 é seguro
 * (= "sem tipo/sem objeto"). */
static long (*deser542_orig)(long, long, long, long, long, long, long, long);
static volatile uint32_t g_deserguard_hits;
static long deser542_hook(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
  if (a0 == 0 || *(void **)a0 == NULL) {
    if (g_deserguard_hits < 8)
      fprintf(stderr, "[DESERGUARD] 0x54220c *arg0=NULL -> skip (f=%d)\n", g_render_frame);
    g_deserguard_hits++;
    return 0;
  }
  return deser542_orig(a0, a1, a2, a3, a4, a5, a6, a7);
}
/* ===== CUP_SCENESPY / CUP_SETACTIVE (s14): SceneManager nativo =====
 * RE (icall table libunity): INTERNAL_CALL_GetActiveScene=0x1bb414, SetActiveScene=
 * 0x1bb44c -> setter real 0x875dc4(mgr, scene); MoveGameObjectToScene=0x1bbc68.
 * Singleton SceneManager = [libunity+0x12bc850]. Cena ativa = [mgr+0x48]; fallback
 * do GetActiveScene = ÚLTIMA da lista (array de ptrs [mgr+0x50], count [mgr+0x60]).
 * UnityScene: nome std::string SSO (+0x38 ptr de dados; ==NULL -> inline em +0x40),
 * estado +0x9c (2 = loaded; SetActiveScene exige ==2).
 * Hipótese do player-fantasma do mapa: Object.Instantiate (Map.Awake/CreateUI) dá a
 * cena ATIVA aos clones; se o mgr não tem cena registrada/ativa no so-loader, o
 * Transform do clone fica com {hierarchy P, idx} = NULL em [+0x38/+0x40] -> SCENESKIP
 * o esconde -> player invisível. SCENESPY mede; SETACTIVE conserta ([mgr+0x48]==NULL
 * com cena loaded na lista -> chama o setter real). */
static void scenespy_dump(const char *tag) {
  if (!g_unity_base) return;
  void *mgr = *(void **)(g_unity_base + 0x12bc850);
  if (!mgr) { fprintf(stderr, "[SCENESPY:%s] mgr=NULL\n", tag); fsync(2); return; }
  void *active = *(void **)((char *)mgr + 0x48);
  void **arr = *(void ***)((char *)mgr + 0x50);
  long cnt = *(long *)((char *)mgr + 0x60);
  fprintf(stderr, "[SCENESPY:%s] mgr=%p active=%p count=%ld f=%d\n", tag, mgr, active, cnt, g_render_frame);
  for (long i = 0; i < cnt && i < 8 && arr; i++) {
    char *sc = (char *)arr[i];
    if (!sc) { fprintf(stderr, "[SCENESPY:%s]  cena[%ld]=NULL\n", tag, i); continue; }
    char *nm = *(char **)(sc + 0x38); if (!nm) nm = sc + 0x40;
    fprintf(stderr, "[SCENESPY:%s]  cena[%ld]=%p state=%d nome=%.48s%s\n", tag, i, sc,
            *(int *)(sc + 0x9c), nm, sc == active ? " (ATIVA)" : "");
  }
  fsync(2);
}
static volatile uint32_t g_setactive_n;
static void setactive_fix(void) {
  if (!g_unity_base) return;
  void *mgr = *(void **)(g_unity_base + 0x12bc850);
  if (!mgr) return;
  void *active = *(void **)((char *)mgr + 0x48);
  void **arr = *(void ***)((char *)mgr + 0x50);
  long cnt = *(long *)((char *)mgr + 0x60);
  if (active || !arr || cnt <= 0) return;
  /* última cena loaded (state==2) — mesma escolha do fallback do GetActiveScene */
  for (long i = cnt - 1; i >= 0; i--) {
    char *sc = (char *)arr[i];
    if (!sc || *(int *)(sc + 0x9c) != 2) continue;
    int ok = ((int (*)(void *, void *))(g_unity_base + 0x875dc4))(mgr, sc);
    char *nm = *(char **)(sc + 0x38); if (!nm) nm = sc + 0x40;
    fprintf(stderr, "[SETACTIVE] cena[%ld]=%p (%.48s) -> SetActiveScene ret=%d (#%u f=%d)\n",
            i, sc, nm, ok, ++g_setactive_n, g_render_frame);
    fsync(2);
    return;
  }
}
static void bootspy_install(uintptr_t base) {
  struct { uintptr_t rva; void *hook; void **orig; const char *nm; } T[] = {
    {0x9A55CC, (void *)bs_hook_0, (void **)&bs_orig_0, "Start"},
    {0x9A5828, (void *)bs_hook_1, (void **)&bs_orig_1, "OnSettingsDataLoaded"},
    {0xB73C60, (void *)bs_hook_2, (void **)&bs_orig_2, "LoadFromCloud"},
    {0xB2398C, (void *)bs_hook_3, (void **)&bs_orig_3, "LoadCloudData"},
    {0xB23EF0, (void *)bs_hook_4, (void **)&bs_orig_4, "OnlineMgr.Init"},
    {0xB73798, (void *)bs_hook_5, (void **)&bs_orig_5, "Settings.Save"},
    {0x9A5750, (void *)bs_hook_6, (void **)&bs_orig_6, "start_cr fac"},
    {0xB7422C, (void *)bs_hook_7, (void **)&bs_orig_7, "OnLoadedCloud"},
  };
  for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
    void *tr = mk_tramp(base + T[i].rva, T[i].nm);
    if (!tr) continue;
    *T[i].orig = tr;
    hook_arm64(base + T[i].rva, (uintptr_t)T[i].hook);
  }
  fprintf(stderr, "[BOOTSPY] %u hooks de boot instalados\n", (unsigned)(sizeof T / sizeof T[0]));
}

/* ===== CUP_MENUSPY: espião do SlotSelectScreen (menu principal pós-título) =====
 * O Update (0xAB4FF0) despacha pelo state(+0x50): se state==0 (InitializeStorage)
 * NÃO faz NADA (ret) — o menu renderiza mas ignora input até o save/storage init
 * completar: OnPlayerDataInitialized(success=true) seta dataStatus(+0x1C8)=1
 * (Received) -> Update inicia allDataLoaded_cr -> SetState(1=MainMenu). Logamos
 * cada elo p/ ver onde a cadeia para no so-loader. */
static long (*ms_orig_update)(void *);
static long ms_hook_update(void *self) {
  static int ls = -1, ld = -1;
  int st = *(int *)((char *)self + 0x50), ds = *(int *)((char *)self + 0x1C8);
  if (st != ls || ds != ld) {
    fprintf(stderr, "[MENUSPY] SlotSelect state=%d dataStatus=%d f=%d\n", st, ds, g_render_frame);
    fsync(2); ls = st; ld = ds;
  }
  return ms_orig_update(self);
}
static long (*ms_orig_setstate)(void *, int);
static long ms_hook_setstate(void *self, int v) {
  fprintf(stderr, "[MENUSPY] SetState(%d) f=%d\n", v, g_render_frame); fsync(2);
  return ms_orig_setstate(self, v);
}
static long (*ms_orig_pdata)(void *, int);
static long ms_hook_pdata(void *self, int ok) {
  fprintf(stderr, "[MENUSPY] OnPlayerDataInitialized(success=%d) f=%d\n", ok & 1, g_render_frame); fsync(2);
  return ms_orig_pdata(self, ok);
}
static long (*ms_orig_sdata)(void *, int);
static long ms_hook_sdata(void *self, int ok) {
  fprintf(stderr, "[MENUSPY] OnSettingsDataLoaded(success=%d) f=%d\n", ok & 1, g_render_frame); fsync(2);
  return ms_orig_sdata(self, ok);
}
static long (*ms_orig_awake)(void *);
static long ms_hook_awake(void *self) {
  fprintf(stderr, "[MENUSPY] SlotSelectScreen.Awake f=%d\n", g_render_frame); fsync(2);
  return ms_orig_awake(self);
}
static void menuspy_install(uintptr_t base) {
  struct { uintptr_t rva; void *hook; void **orig; const char *nm; } T[] = {
    {0xAB4FF0, (void *)ms_hook_update,   (void **)&ms_orig_update,   "SlotSelect.Update"},
    {0xAB670C, (void *)ms_hook_setstate, (void **)&ms_orig_setstate, "SlotSelect.SetState"},
    {0xAB8868, (void *)ms_hook_pdata,    (void **)&ms_orig_pdata,    "OnPlayerDataInitialized"},
    {0xAB89A0, (void *)ms_hook_sdata,    (void **)&ms_orig_sdata,    "OnSettingsDataLoaded"},
    {0xAB4BA4, (void *)ms_hook_awake,    (void **)&ms_orig_awake,    "SlotSelect.Awake"},
  };
  for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
    void *tr = mk_tramp(base + T[i].rva, T[i].nm);
    if (!tr) continue;
    *T[i].orig = tr;
    hook_arm64(base + T[i].rva, (uintptr_t)T[i].hook);
  }
  fprintf(stderr, "[MENUSPY] hooks SlotSelectScreen instalados\n"); fsync(2);
}

/* ===== CUP_STAGESPY (s14c): por que o conteúdo da fase (boss/cenário) não aparece? =====
 * Fase: player+chão+céu renderizam, boss+cenário FALTAM; só 29 draws/frame; 1 HIERFIX
 * (≠ problema do mapa). atlas_veggieslevel deployado. Pergunta-chave: os sprites do boss
 * estão sendo ATRIBUÍDOS aos renderers (=> problema é render/Mali) ou NÃO (=> load async
 * da fase não completa)? Hook decisivo: SpriteRenderer.set_sprite (il2cpp 0x178EB3C) —
 * conta atribuições e quantas com sprite NULL. + AssetBundle.LoadAssetAsync (0x17C893C) —
 * loga o que a fase pede async (se nunca completa, o sprite nunca é setado). */
static long (*ss_setsprite_orig)(void *, void *);
static volatile uint32_t g_ss_set, g_ss_null;
static long ss_setsprite_hook(void *self, void *sprite) {
  g_ss_set++;
  if (!sprite) g_ss_null++;
  return ss_setsprite_orig(self, sprite);
}
static long (*ss_loadasync_orig)(void *, void *, void *);
static volatile uint32_t g_ss_async;
static long ss_loadasync_hook(void *self, void *name, void *type) {
  g_ss_async++;
  if (g_ss_async < 60 && name) {
    /* il2cpp String: len@+0x10 (int), chars utf16@+0x14 */
    int len = *(int *)((char *)name + 0x10);
    unsigned short *u = (unsigned short *)((char *)name + 0x14);
    char buf[128]; int n = 0;
    for (int i = 0; i < len && n < (int)sizeof buf - 1; i++)
      buf[n++] = (u[i] < 128) ? (char)u[i] : '?';
    buf[n] = 0;
    fprintf(stderr, "[STAGESPY] LoadAssetAsync(\"%s\") #%u f=%d\n", buf, g_ss_async, g_render_frame);
    fsync(2);
  }
  return ss_loadasync_orig(self, name, type);
}
static void stagespy_install(uintptr_t base) {
  struct { uintptr_t rva; void *hook; void **orig; const char *nm; } T[] = {
    {0x178EB3C, (void *)ss_setsprite_hook, (void **)&ss_setsprite_orig, "SpriteRenderer.set_sprite"},
    {0x17C893C, (void *)ss_loadasync_hook, (void **)&ss_loadasync_orig, "AssetBundle.LoadAssetAsync"},
  };
  for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
    void *tr = mk_tramp(base + T[i].rva, T[i].nm);
    if (!tr) { fprintf(stderr, "[STAGESPY] tramp %s falhou\n", T[i].nm); continue; }
    *T[i].orig = tr;
    hook_arm64(base + T[i].rva, (uintptr_t)T[i].hook);
  }
  fprintf(stderr, "[STAGESPY] hooks instalados (set_sprite + LoadAssetAsync)\n"); fsync(2);
}

/* ===== TER_RXD_ABSPY: diagnostico do boot/assets do Rockman X DiVE ===== */
static const char *rxd_str(void *s, char *buf, size_t cap) {
  if (!s) return "(null)";
  if (cap == 0) return "";
  int len = *(int *)((char *)s + 0x10);
  if (len < 0 || len > 4096) { snprintf(buf, cap, "(badstr:%p)", s); return buf; }
  unsigned short *u = (unsigned short *)((char *)s + 0x14);
  int n = 0;
  for (int i = 0; i < len && n < (int)cap - 1; i++) {
    unsigned short ch = u[i];
    buf[n++] = (ch >= 32 && ch < 127) ? (char)ch : '?';
  }
  buf[n] = 0;
  return buf;
}

static void rxd_log_array(const char *tag, void *arr) {
  if (!arr) { fprintf(stderr, "%s arr=NULL\n", tag); return; }
  size_t len = *(size_t *)((char *)arr + 0x18);
  if (len > 64) len = 64;
  void **vec = (void **)((char *)arr + 0x20);
  fprintf(stderr, "%s len=%zu", tag, len);
  for (size_t i = 0; i < len && i < 8; i++) {
    char b[128];
    fprintf(stderr, " [%zu]=%s", i, rxd_str(vec[i], b, sizeof b));
  }
  if (len > 8) fprintf(stderr, " ...");
  fprintf(stderr, "\n");
}

static void rxd_enum_scene_methods_once(void) {
  static int done;
  if (done || !env_on("TER_RXD_SCENE_ENUM") || !g_il2cpp_base) return;
  done = 1;
  void *(*dom_get)(void) = (void *(*)(void))ter_il2cpp_sym_cached("il2cpp_domain_get");
  const void **(*dom_asms)(void *, size_t *) =
      (const void **(*)(void *, size_t *))ter_il2cpp_sym_cached("il2cpp_domain_get_assemblies");
  void *(*asm_img)(const void *) =
      (void *(*)(const void *))ter_il2cpp_sym_cached("il2cpp_assembly_get_image");
  void *(*cls_from_name)(void *, const char *, const char *) =
      (void *(*)(void *, const char *, const char *))ter_il2cpp_sym_cached("il2cpp_class_from_name");
  void *(*cls_methods)(void *, void **) =
      (void *(*)(void *, void **))ter_il2cpp_sym_cached("il2cpp_class_get_methods");
  const char *(*meth_name)(void *) =
      (const char *(*)(void *))ter_il2cpp_sym_cached("il2cpp_method_get_name");
  unsigned (*meth_pcount)(void *) =
      (unsigned (*)(void *))ter_il2cpp_sym_cached("il2cpp_method_get_param_count");
  void *(*meth_param)(void *, unsigned) =
      (void *(*)(void *, unsigned))ter_il2cpp_sym_cached("il2cpp_method_get_param");
  const char *(*meth_pname)(void *, unsigned) =
      (const char *(*)(void *, unsigned))ter_il2cpp_sym_cached("il2cpp_method_get_param_name");
  void *(*meth_ret)(void *) =
      (void *(*)(void *))ter_il2cpp_sym_cached("il2cpp_method_get_return_type");
  char *(*type_name)(void *) =
      (char *(*)(void *))ter_il2cpp_sym_cached("il2cpp_type_get_name");
  if (!dom_get || !dom_asms || !asm_img || !cls_from_name || !cls_methods ||
      !meth_name || !meth_pcount || !meth_param || !meth_pname || !meth_ret || !type_name) {
    fprintf(stderr, "[RXD_ENUM] APIs IL2CPP incompletas\n");
    fsync(2);
    return;
  }
  void *domain = dom_get();
  size_t na = 0;
  const void **asms = domain ? dom_asms(domain, &na) : NULL;
  static const char *namespaces[] = { "RXD", "", NULL };
  for (size_t i = 0; asms && i < na; i++) {
    void *img = asm_img(asms[i]);
    if (!img) continue;
    for (int ni = 0; namespaces[ni]; ni++) {
      void *cls = cls_from_name(img, namespaces[ni], "OrangeSceneManager");
      if (!cls) continue;
      fprintf(stderr, "[RXD_ENUM] OrangeSceneManager ns=%s cls=%p\n",
              namespaces[ni][0] ? namespaces[ni] : "(empty)", cls);
      void *it = NULL;
      for (;;) {
        void *m = cls_methods(cls, &it);
        if (!m) break;
        const char *mn = meth_name(m);
        if (!mn || (!strstr(mn, "Scene") && !strstr(mn, "Change"))) continue;
        unsigned pc = meth_pcount(m);
        char *rt = type_name(meth_ret(m));
        fprintf(stderr, "[RXD_ENUM]   %s argc=%u ret=%s method=%p", mn, pc, rt ? rt : "?", m);
        for (unsigned p = 0; p < pc; p++) {
          char *tn = type_name(meth_param(m, p));
          const char *pn = meth_pname(m, p);
          fprintf(stderr, " | p%u %s %s", p, tn ? tn : "?", pn ? pn : "?");
        }
        fprintf(stderr, "\n");
      }
      fsync(2);
    }
  }
}

static void rxd_log_asyncop(const char *tag, void *op) {
  if (!op) { fprintf(stderr, "%s op=NULL\n", tag); return; }
  void *ptr = *(void **)((char *)op + 0x10);
  int nstate = ptr ? *(int *)((char *)ptr + 0x48) : -1;
  int nmode = ptr ? *(int *)((char *)ptr + 0x3A0) : -1;
  int nallow = ptr ? *(unsigned char *)((char *)ptr + 0x3A4) : -1;
  int nready = ptr ? *(unsigned char *)((char *)ptr + 0x3A5) : -1;
  int nflag = ptr ? *(unsigned char *)((char *)ptr + 0x3A6) : -1;
  int done = -1;
  float prog = -1.0f;
  int allow = -1;
  void *ab = NULL;
  if (g_il2cpp_base) {
    int (*get_done)(void *, void *) = (void *)(g_il2cpp_base + 0x1EBE394);
    float (*get_prog)(void *, void *) = (void *)(g_il2cpp_base + 0x1EBE3D4);
    if (get_done) done = get_done(op, NULL);
    if (get_prog) prog = get_prog(op, NULL);
    if (env_on("TER_RXD_ASYNC_ICALL")) {
      void *(*resolve_icall)(const char *) =
          (void *(*)(const char *))ter_il2cpp_sym_cached("il2cpp_resolve_icall");
      static void *get_allow_icall, *set_allow_icall, *get_done_icall, *get_prog_icall;
      static int once;
      if (resolve_icall && !once) {
        once = 1;
        get_allow_icall = resolve_icall("UnityEngine.AsyncOperation::get_allowSceneActivation()");
        set_allow_icall = resolve_icall("UnityEngine.AsyncOperation::set_allowSceneActivation(System.Boolean)");
        get_done_icall = resolve_icall("UnityEngine.AsyncOperation::get_isDone()");
        get_prog_icall = resolve_icall("UnityEngine.AsyncOperation::get_progress()");
        fprintf(stderr,
                "[RXD_ASYNC_ICALL] get_allow=%p set_allow=%p get_done=%p get_progress=%p\n",
                get_allow_icall, set_allow_icall, get_done_icall, get_prog_icall);
        fsync(2);
      }
      if (get_allow_icall) allow = ((int (*)(void *))get_allow_icall)(op);
    }
    if (env_on("TER_RXD_ASYNC_AB")) {
      void *(*get_ab)(void *, void *) = (void *)(g_il2cpp_base + 0x2E2C5CC);
      if (get_ab) ab = get_ab(op, NULL);
    }
  }
  fprintf(stderr,
          "%s op=%p m_Ptr=%p done=%d progress=%.3f allow=%d "
          "state=%d mode=%d native_allow=%d ready=%d flag=%d assetBundle=%p f=%d\n",
          tag, op, ptr, done, prog, allow, nstate, nmode, nallow, nready,
          nflag, ab, g_render_frame);
  if (ptr && env_on("TER_RXD_ASYNC_DUMP")) {
    uintptr_t *p = (uintptr_t *)ptr;
    uintptr_t vt = p[0];
    uintptr_t f80 = vt ? *(uintptr_t *)(vt + 0x50) : 0;
    uintptr_t f88 = vt ? *(uintptr_t *)(vt + 0x58) : 0;
    fprintf(stderr,
            "[RXD_ASYNC_DUMP] ptr=%p +00=%016lx +08=%016lx +10=%016lx +18=%016lx "
            "+20=%016lx +28=%016lx +30=%016lx +38=%016lx f=%d\n",
            ptr, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], g_render_frame);
    fprintf(stderr,
            "[RXD_ASYNC_DUMP] vt=%p f80=%p f88=%p state48=%d progress6c=%.3f "
            "mode3a0=%d allow3a4=%d ready3a5=%d flag3a6=%d sub98=%016lx suba0=%016lx f=%d\n",
            (void *)vt, (void *)f80, (void *)f88, nstate,
            *(float *)((char *)ptr + 0x6C), nmode, nallow, nready, nflag,
            *(uintptr_t *)((char *)ptr + 0x98),
            *(uintptr_t *)((char *)ptr + 0xA0), g_render_frame);
  }
  fsync(2);
}

static int rxd_byte_array_len(void *arr);
static void rxd_log_abid(const char *tag, void *abid);

static void rxd_log_cr(const char *tag, void *it, int after, long ret) {
  if (!it) return;
  char s0[192], s1[192];
  int st = *(int *)((char *)it + 0x10);
  void *cur = *(void **)((char *)it + 0x18);
  void *self = *(void **)((char *)it + 0x20);
  if (!strcmp(tag, "Manifest")) {
    void *path = *(void **)((char *)it + 0x28);
    void *req = *(void **)((char *)it + 0x30);
    fprintf(stderr, "[RXD_CR] %s %s state=%d ret=%ld self=%p cur=%p(%s) path=%s req=%p f=%d\n",
            tag, after ? "post" : "pre", st, ret, self, cur, il2cpp_classname(cur),
            rxd_str(path, s0, sizeof s0), req, g_render_frame);
    if (req) rxd_log_asyncop("[RXD_CR] Manifest request", req);
  } else if (!strcmp(tag, "Assets")) {
    int len = *(int *)((char *)it + 0x4C);
    int idx = *(int *)((char *)it + 0x50);
    int go = *(unsigned char *)((char *)it + 0x48);
    fprintf(stderr, "[RXD_CR] %s %s state=%d ret=%ld self=%p cur=%p(%s) go=%d i=%d len=%d f=%d\n",
            tag, after ? "post" : "pre", st, ret, self, cur, il2cpp_classname(cur),
            go, idx, len, g_render_frame);
  } else {
    int idx = *(int *)((char *)it + 0x50);
    int loaded = *(unsigned char *)((char *)it + 0x40);
    int nokeep = *(unsigned char *)((char *)it + 0x41);
    void *bundle = *(void **)((char *)it + 0x30);
    void *dc = *(void **)((char *)it + 0x38);
    void *abid = dc ? *(void **)((char *)dc + 0x10) : NULL;
    void *bytes = dc ? *(void **)((char *)dc + 0x30) : NULL;
    void *loc = dc ? *(void **)((char *)dc + 0x38) : NULL;
    int go_next = dc ? *(unsigned char *)((char *)dc + 0x40) : -1;
    char s2[192];
    fprintf(stderr, "[RXD_CR] %s %s state=%d ret=%ld self=%p cur=%p(%s) bundle=%s loaded=%d nokeep=%d i=%d dc=%p abid=%p bytes=%p len=%d loc=%s goNext=%d f=%d\n",
            tag, after ? "post" : "pre", st, ret, self, cur, il2cpp_classname(cur),
            rxd_str(bundle, s1, sizeof s1), loaded, nokeep, idx, dc, abid, bytes,
            rxd_byte_array_len(bytes), rxd_str(loc, s2, sizeof s2), go_next, g_render_frame);
    if (abid && env_on("TER_RXD_SINGLE_ABID")) {
      fprintf(stderr, "[RXD_CR] %s abid-detail ", tag);
      rxd_log_abid("", abid);
    }
  }
  fsync(2);
}

static void *volatile g_rxd_manifest_it;
static void *volatile g_rxd_assets_it;
static void *volatile g_rxd_single_it;
static volatile int g_rxd_manifest_done;
static volatile int g_rxd_assets_done;
static volatile int g_rxd_single_done;
#define RXD_SINGLE_MAX 64
static void *volatile g_rxd_single_its[RXD_SINGLE_MAX];
static unsigned char g_rxd_single_dones[RXD_SINGLE_MAX];
static int g_rxd_single_n;
#define RXD_ASYNCASSET_MAX 64
static void *volatile g_rxd_asyncasset_its[RXD_ASYNCASSET_MAX];
static unsigned char g_rxd_asyncasset_dones[RXD_ASYNCASSET_MAX];
static int g_rxd_asyncasset_n;
static void *volatile g_rxd_audio_mgr;
static void *volatile g_rxd_console_mgr;
static void *volatile g_rxd_localization_mgr;

static void rxd_track_single_it(void *it) {
  if (!it) return;
  for (int i = 0; i < g_rxd_single_n; i++) {
    if (g_rxd_single_its[i] == it) {
      g_rxd_single_dones[i] = 0;
      return;
    }
  }
  int slot = g_rxd_single_n;
  if (slot >= RXD_SINGLE_MAX) slot = RXD_SINGLE_MAX - 1;
  else g_rxd_single_n++;
  g_rxd_single_its[slot] = it;
  g_rxd_single_dones[slot] = 0;
}

static void rxd_mark_single_done(void *it) {
  if (!it) return;
  for (int i = 0; i < g_rxd_single_n; i++) {
    if (g_rxd_single_its[i] == it) {
      g_rxd_single_dones[i] = 1;
      return;
    }
  }
}

static int rxd_single_is_done(void *it) {
  if (!it) return 1;
  for (int i = 0; i < g_rxd_single_n; i++) {
    if (g_rxd_single_its[i] == it) return g_rxd_single_dones[i] != 0;
  }
  return *(int *)((char *)it + 0x10) == -1;
}

static void rxd_track_asyncasset_it(void *it) {
  if (!it) return;
  for (int i = 0; i < g_rxd_asyncasset_n; i++) {
    if (g_rxd_asyncasset_its[i] == it) {
      g_rxd_asyncasset_dones[i] = 0;
      return;
    }
  }
  int slot = g_rxd_asyncasset_n;
  if (slot >= RXD_ASYNCASSET_MAX) slot = RXD_ASYNCASSET_MAX - 1;
  else g_rxd_asyncasset_n++;
  g_rxd_asyncasset_its[slot] = it;
  g_rxd_asyncasset_dones[slot] = 0;
}

static void rxd_log_asyncasset_it(const char *tag, void *it, int after, long ret) {
  if (!it) return;
  char b[192], a[192];
  int st = *(int *)((char *)it + 0x10);
  void *cur = *(void **)((char *)it + 0x18);
  void *self = *(void **)((char *)it + 0x20);
  void *bundle = *(void **)((char *)it + 0x28);
  int keep = *(int *)((char *)it + 0x30);
  void *asset = *(void **)((char *)it + 0x38);
  void *cb = *(void **)((char *)it + 0x40);
  fprintf(stderr,
          "[RXD_AACR] %s state=%d ret=%ld self=%p cur=%p(%s) bundle=%s asset=%s cb=%p keep=%d childDone=%d f=%d\n",
          after ? "post" : "pre", st, ret, self, cur, il2cpp_classname(cur),
          rxd_str(bundle, b, sizeof b), rxd_str(asset, a, sizeof a),
          cb, keep, rxd_single_is_done(cur), g_render_frame);
  (void)tag;
  fsync(2);
}

static long rxd_asyncasset_movenext_runtime(void *it) {
  if (!it || !g_il2cpp_base) return 0;
  void *(*obj_cls)(void *) = (void *(*)(void *))ter_il2cpp_sym_cached("il2cpp_object_get_class");
  void *(*cls_method)(void *, const char *, int) =
      (void *(*)(void *, const char *, int))ter_il2cpp_sym_cached("il2cpp_class_get_method_from_name");
  void *(*rt_invoke)(void *, void *, void **, void **) =
      (void *(*)(void *, void *, void **, void **))ter_il2cpp_sym_cached("il2cpp_runtime_invoke");
  if (!obj_cls || !cls_method || !rt_invoke) return 0;
  void *cls = obj_cls(it);
  void *m = cls ? cls_method(cls, "MoveNext", 0) : NULL;
  if (!m) {
    fprintf(stderr, "[RXD_AACR] MoveNext nao encontrado it=%p cls=%p(%s) f=%d\n",
            it, cls, il2cpp_classname(it), g_render_frame);
    fsync(2);
    return 0;
  }
  rxd_log_asyncasset_it("AsyncAsset", it, 0, -1);
  void *exc = NULL;
  void *ret = rt_invoke(m, it, NULL, &exc);
  long ok = ret ? *(unsigned char *)((char *)ret + 0x10) : 0;
  if (exc) {
    fprintf(stderr, "[RXD_AACR] MoveNext exception=%p(%s) it=%p f=%d\n",
            exc, il2cpp_classname(exc), it, g_render_frame);
    fsync(2);
    ok = 0;
  }
  rxd_log_asyncasset_it("AsyncAsset", it, 1, ok);
  return ok;
}

static void rxd_audio_log(const char *tag, void *self) {
  if (!self) { fprintf(stderr, "[RXD_AUDIO] %s self=NULL f=%d\n", tag, g_render_frame); return; }
  fprintf(stderr, "[RXD_AUDIO] %s self=%p IsInitAll=%d IsInitSystemSE=%d acf=%p cfg=%p preload=%p f=%d\n",
          tag, self, *(unsigned char *)((char *)self + 0x21),
          *(unsigned char *)((char *)self + 0x22),
          *(void **)((char *)self + 0x90), *(void **)((char *)self + 0x98),
          *(void **)((char *)self + 0xA0), g_render_frame);
  fsync(2);
}

static void *rxd_get_audio_mgr_instance(void) {
  return (void *)g_rxd_audio_mgr;
}

static void rxd_console_log(const char *tag, void *self) {
  if (!self) { fprintf(stderr, "[RXD_CONSOLE] %s self=NULL f=%d\n", tag, g_render_frame); return; }
  fprintf(stderr, "[RXD_CONSOLE] %s self=%p b20=%d b21=%d b22=%d f=%d\n",
          tag, self, *(unsigned char *)((char *)self + 0x20),
          *(unsigned char *)((char *)self + 0x21),
          *(unsigned char *)((char *)self + 0x22), g_render_frame);
  fsync(2);
}

static void *rxd_get_console_mgr_instance(void) {
  return (void *)g_rxd_console_mgr;
}

static void rxd_localization_log(const char *tag, void *self) {
  if (!self) { fprintf(stderr, "[RXD_LOCALE] %s self=NULL f=%d\n", tag, g_render_frame); return; }
  fprintf(stderr, "[RXD_LOCALE] %s self=%p b18=%d b20=%d b21=%d b22=%d f=%d\n",
          tag, self,
          *(unsigned char *)((char *)self + 0x18),
          *(unsigned char *)((char *)self + 0x20),
          *(unsigned char *)((char *)self + 0x21),
          *(unsigned char *)((char *)self + 0x22), g_render_frame);
  fsync(2);
}

static void *rxd_get_localization_mgr_instance(void) {
  return (void *)g_rxd_localization_mgr;
}

static int rxd_ui_has_core(void *obj) {
  return obj &&
         *(void **)((char *)obj + 0x60) &&
         *(void **)((char *)obj + 0x68) &&
         *(void **)((char *)obj + 0x80) &&
         *(void **)((char *)obj + 0x88);
}

static int rxd_ui_logic_ok(void *obj) {
  return rxd_ui_has_core(obj) &&
         (getenv("TER_RXD_UI_KEEP_DEAD_SINGLETON") || rxd_unity_alive(obj));
}

static void rxd_managed_store_ref(void *obj, void **slot, void *value) {
  void (*wb)(void *, void **, void *) =
      (void (*)(void *, void **, void *))ter_il2cpp_sym_cached("il2cpp_gc_wbarrier_set_field");
  if (wb) wb(obj, slot, value);
  else *slot = value;
}

static void *rxd_class_any(const char *ns, const char *cn) {
  void *(*dom_get)(void) = (void *(*)(void))ter_il2cpp_sym_cached("il2cpp_domain_get");
  const void **(*dom_asms)(void *, size_t *) =
      (const void **(*)(void *, size_t *))ter_il2cpp_sym_cached("il2cpp_domain_get_assemblies");
  void *(*asm_img)(const void *) =
      (void *(*)(const void *))ter_il2cpp_sym_cached("il2cpp_assembly_get_image");
  void *(*cls_from_name)(void *, const char *, const char *) =
      (void *(*)(void *, const char *, const char *))ter_il2cpp_sym_cached("il2cpp_class_from_name");
  if (!dom_get || !dom_asms || !asm_img || !cls_from_name) return NULL;
  void *domain = dom_get();
  size_t na = 0;
  const void **as = domain ? dom_asms(domain, &na) : NULL;
  if (!as) return NULL;
  for (size_t i = 0; i < na; i++) {
    void *img = asm_img(as[i]);
    void *cls = img ? cls_from_name(img, ns ? ns : "", cn) : NULL;
    if (cls) return cls;
  }
  return NULL;
}

static void *rxd_type_object_any(const char *ns, const char *cn) {
  void *cls = rxd_class_any(ns, cn);
  void *(*class_get_type)(void *) =
      (void *(*)(void *))ter_il2cpp_sym_cached("il2cpp_class_get_type");
  void *(*type_get_object)(void *) =
      (void *(*)(void *))ter_il2cpp_sym_cached("il2cpp_type_get_object");
  void *ty = (cls && class_get_type) ? class_get_type(cls) : NULL;
  return (ty && type_get_object) ? type_get_object(ty) : NULL;
}

static void *rxd_class_scan_any(const char *ns, const char *cn) {
  void *cls = rxd_class_any(ns, cn);
  if (cls) return cls;
  void *(*dom_get)(void) = (void *(*)(void))ter_il2cpp_sym_cached("il2cpp_domain_get");
  const void **(*dom_asms)(void *, size_t *) =
      (const void **(*)(void *, size_t *))ter_il2cpp_sym_cached("il2cpp_domain_get_assemblies");
  void *(*asm_img)(const void *) =
      (void *(*)(const void *))ter_il2cpp_sym_cached("il2cpp_assembly_get_image");
  size_t (*img_count)(void *) =
      (size_t (*)(void *))ter_il2cpp_sym_cached("il2cpp_image_get_class_count");
  void *(*img_class)(void *, size_t) =
      (void *(*)(void *, size_t))ter_il2cpp_sym_cached("il2cpp_image_get_class");
  const char *(*cls_name)(void *) =
      (const char *(*)(void *))ter_il2cpp_sym_cached("il2cpp_class_get_name");
  const char *(*cls_ns)(void *) =
      (const char *(*)(void *))ter_il2cpp_sym_cached("il2cpp_class_get_namespace");
  if (!dom_get || !dom_asms || !asm_img || !img_count || !img_class ||
      !cls_name || !cls_ns) return NULL;
  void *domain = dom_get();
  size_t na = 0;
  const void **as = domain ? dom_asms(domain, &na) : NULL;
  for (size_t ai = 0; as && ai < na; ai++) {
    void *img = asm_img(as[ai]);
    size_t nc = img ? img_count(img) : 0;
    for (size_t ci = 0; ci < nc; ci++) {
      void *c = img_class(img, ci);
      const char *n = c ? cls_name(c) : NULL;
      const char *x = c ? cls_ns(c) : NULL;
      if (n && !strcmp(n, cn) && (!ns || !strcmp(x ? x : "", ns))) return c;
    }
  }
  return NULL;
}

static void *rxd_method_any(void *cls, const char *mn, int argc, const char *label) {
  void *(*cls_method)(void *, const char *, int) =
      (void *(*)(void *, const char *, int))ter_il2cpp_sym_cached("il2cpp_class_get_method_from_name");
  void *(*cls_methods)(void *, void **) =
      (void *(*)(void *, void **))ter_il2cpp_sym_cached("il2cpp_class_get_methods");
  const char *(*meth_name)(void *) =
      (const char *(*)(void *))ter_il2cpp_sym_cached("il2cpp_method_get_name");
  unsigned (*meth_pcount)(void *) =
      (unsigned (*)(void *))ter_il2cpp_sym_cached("il2cpp_method_get_param_count");
  if (!cls) return NULL;
  void *m = (argc >= 0 && cls_method) ? cls_method(cls, mn, argc) : NULL;
  if (m) return m;
  if (!cls_methods || !meth_name || !meth_pcount) return NULL;
  void *it = NULL;
  while ((m = cls_methods(cls, &it))) {
    const char *n = meth_name(m);
    unsigned pc = meth_pcount(m);
    if (n && !strcmp(n, mn) && (argc < 0 || pc == (unsigned)argc)) return m;
  }
  if (label) {
    fprintf(stderr, "[RXD_SPLASHSPY] metodo nao achado %s.%s argc=%d\n", label, mn, argc);
    fsync(2);
  }
  return NULL;
}

static int rxd_hook_method_any(const char *ns, const char *cn, const char *mn,
                               int argc, void *hook, void **orig, const char *label) {
  void *cls = rxd_class_scan_any(ns, cn);
  void *m = rxd_method_any(cls, mn, argc, label);
  void *target = m ? *(void **)m : NULL;
  uintptr_t rva = (target && g_il2cpp_base &&
                   (uintptr_t)target >= g_il2cpp_base) ?
      (uintptr_t)target - g_il2cpp_base : 0;
  fprintf(stderr,
          "[RXD_SPLASHSPY] hook lookup %s cls=%p method=%p target=%p rva=0x%lx f=%d\n",
          label ? label : "?", cls, m, target, (unsigned long)rva, g_render_frame);
  fsync(2);
  if (!target || !orig) return 0;
  void *tr = mk_tramp((uintptr_t)target, label ? label : "RXD_SPLASHSPY");
  if (!tr) {
    fprintf(stderr, "[RXD_SPLASHSPY] tramp falhou %s target=%p\n",
            label ? label : "?", target);
    fsync(2);
    return 0;
  }
  *orig = tr;
  hook_arm64((uintptr_t)target, (uintptr_t)hook);
  return 1;
}

static void *g_rxd_last_title_mgr;
static void *g_rxd_last_title_ui;
static void *g_rxd_last_title_clone;

static int rxd_list_size(void *list) {
  return list ? *(int *)((char *)list + 0x18) : -1;
}

static void *rxd_obj_cached(void *obj) {
  return obj ? *(void **)((char *)obj + 0x10) : NULL;
}

static uint32_t rxd_gc_pin(void *obj, const char *tag) {
  if (!obj) return 0;
  uint32_t (*gch_new)(void *, int) =
      (uint32_t (*)(void *, int))ter_il2cpp_sym_cached("il2cpp_gchandle_new");
  if (!gch_new) return 0;
  uint32_t h = gch_new(obj, 0);
  if (h) {
    fprintf(stderr, "[RXD_GC] pin %s obj=%p(%s) handle=%u f=%d\n",
            tag ? tag : "obj", obj, il2cpp_classname(obj), h, g_render_frame);
    fsync(2);
  }
  return h;
}

static int rxd_unity_alive(void *obj) {
  if (!obj || !g_il2cpp_base) return 0;
  int (*op_implicit)(void *, void *) =
      (int (*)(void *, void *))(g_il2cpp_base + 0x18E853C);
  return op_implicit ? op_implicit(obj, NULL) : (rxd_obj_cached(obj) != NULL);
}

static void rxd_dontdestroy_obj(void *obj, const char *tag) {
  if (!obj || !g_il2cpp_base || !rxd_unity_alive(obj)) return;
  void (*ddol)(void *, void *) =
      (void (*)(void *, void *))(g_il2cpp_base + 0x18E91CC);
  if (!ddol) return;
  fprintf(stderr, "[RXD_DDOL] %s obj=%p(%s) cp=%p f=%d\n",
          tag ? tag : "obj", obj, il2cpp_classname(obj), rxd_obj_cached(obj),
          g_render_frame);
  fsync(2);
  ddol(obj, NULL);
}

static void *rxd_comp_go(void *comp) {
  if (!comp || !g_il2cpp_base || !rxd_unity_alive(comp)) return NULL;
  void *(*get_go)(void *, void *) =
      (void *(*)(void *, void *))(g_il2cpp_base + 0x1EC4938);
  return get_go ? get_go(comp, NULL) : NULL;
}

static void *rxd_reacquire_uimanager(void) {
  if (rxd_ui_logic_ok(g_rxd_ui_mgr_good)) {
    return g_rxd_ui_mgr_good;
  }
  if (!g_rxd_ui_mgr_good_go || !rxd_unity_alive(g_rxd_ui_mgr_good_go) ||
      !g_il2cpp_base) {
    return g_rxd_ui_mgr_good;
  }
  void *ty = rxd_type_object_any("", "UIManager");
  void *(*go_get_component)(void *, void *, void *) =
      (void *(*)(void *, void *, void *))(g_il2cpp_base + 0x1EC49FC);
  void *mgr = (ty && go_get_component) ?
      go_get_component(g_rxd_ui_mgr_good_go, ty, NULL) : NULL;
  fprintf(stderr,
          "[RXD_UI_RECOVER] go=%p cp=%p ty=%p -> mgr=%p cp=%p has=%d alive=%d f=%d\n",
          g_rxd_ui_mgr_good_go, rxd_obj_cached(g_rxd_ui_mgr_good_go), ty, mgr,
          rxd_obj_cached(mgr), rxd_ui_has_core(mgr), rxd_unity_alive(mgr),
          g_render_frame);
  fsync(2);
  if (rxd_ui_has_core(mgr) && rxd_unity_alive(mgr)) {
    g_rxd_ui_mgr_good = mgr;
    rxd_gc_pin(mgr, "UIManager.recovered");
    return mgr;
  }
  return g_rxd_ui_mgr_good;
}

static void *rxd_comp_tr(void *comp) {
  if (!comp || !g_il2cpp_base || !rxd_unity_alive(comp)) return NULL;
  void *(*get_tr)(void *, void *) =
      (void *(*)(void *, void *))(g_il2cpp_base + 0x1EC48F8);
  return get_tr ? get_tr(comp, NULL) : NULL;
}

static void *rxd_go_tr(void *go) {
  if (!go || !g_il2cpp_base || !rxd_unity_alive(go)) return NULL;
  void *(*get_tr)(void *, void *) =
      (void *(*)(void *, void *))(g_il2cpp_base + 0x1EC92A4);
  return get_tr ? get_tr(go, NULL) : NULL;
}

static int rxd_go_active_self(void *go) {
  if (!go || !g_il2cpp_base || !rxd_unity_alive(go)) return -1;
  int (*fn)(void *, void *) = (int (*)(void *, void *))(g_il2cpp_base + 0x1EC93C4);
  return fn ? fn(go, NULL) : -1;
}

static int rxd_go_active_hier(void *go) {
  if (!go || !g_il2cpp_base || !rxd_unity_alive(go)) return -1;
  int (*fn)(void *, void *) = (int (*)(void *, void *))(g_il2cpp_base + 0x1EC9404);
  return fn ? fn(go, NULL) : -1;
}

static int rxd_go_layer(void *go) {
  if (!go || !g_il2cpp_base || !rxd_unity_alive(go)) return -999;
  int (*fn)(void *, void *) = (int (*)(void *, void *))(g_il2cpp_base + 0x1EC92E4);
  return fn ? fn(go, NULL) : -999;
}

static void rxd_go_set_layer(void *go, int layer) {
  if (!go || !g_il2cpp_base || !rxd_unity_alive(go)) return;
  void (*fn)(void *, int, void *) =
      (void (*)(void *, int, void *))(g_il2cpp_base + 0x1EC9324);
  if (fn) fn(go, layer, NULL);
}

static void rxd_go_set_active(void *go, int active) {
  if (!go || !g_il2cpp_base || !rxd_unity_alive(go)) return;
  void (*fn)(void *, int, void *) =
      (void (*)(void *, int, void *))(g_il2cpp_base + 0x1EC9374);
  if (fn) fn(go, active, NULL);
}

static void *rxd_tr_parent(void *tr) {
  if (!tr || !g_il2cpp_base || !rxd_unity_alive(tr)) return NULL;
  void *(*fn)(void *, void *) =
      (void *(*)(void *, void *))(g_il2cpp_base + 0x28D70A4);
  return fn ? fn(tr, NULL) : NULL;
}

static int rxd_tr_child_count(void *tr) {
  if (!tr || !g_il2cpp_base || !rxd_unity_alive(tr)) return -1;
  int (*fn)(void *, void *) = (int (*)(void *, void *))(g_il2cpp_base + 0x28D80B8);
  return fn ? fn(tr, NULL) : -1;
}

static void rxd_tr_set_parent(void *tr, void *parent, int world_stays) {
  if (!tr || !parent || !g_il2cpp_base || !rxd_unity_alive(tr) ||
      !rxd_unity_alive(parent)) return;
  void (*fn)(void *, void *, int, void *) =
      (void (*)(void *, void *, int, void *))(g_il2cpp_base + 0x28D72EC);
  if (fn) fn(tr, parent, world_stays, NULL);
}

static int rxd_beh_enabled(void *beh) {
  if (!beh || !g_il2cpp_base || !rxd_unity_alive(beh)) return -1;
  int (*fn)(void *, void *) = (int (*)(void *, void *))(g_il2cpp_base + 0x1EBF404);
  return fn ? fn(beh, NULL) : -1;
}

static int rxd_beh_active_enabled(void *beh) {
  if (!beh || !g_il2cpp_base || !rxd_unity_alive(beh)) return -1;
  int (*fn)(void *, void *) = (int (*)(void *, void *))(g_il2cpp_base + 0x1EBF494);
  return fn ? fn(beh, NULL) : -1;
}

static void rxd_beh_set_enabled(void *beh, int enabled) {
  if (!beh || !g_il2cpp_base || !rxd_unity_alive(beh)) return;
  void (*fn)(void *, int, void *) =
      (void (*)(void *, int, void *))(g_il2cpp_base + 0x1EBF444);
  if (fn) fn(beh, enabled, NULL);
}

static int rxd_canvas_mode(void *canvas) {
  if (!canvas || !g_il2cpp_base || !rxd_unity_alive(canvas)) return -1;
  int (*fn)(void *, void *) = (int (*)(void *, void *))(g_il2cpp_base + 0x2E1D398);
  return fn ? fn(canvas, NULL) : -1;
}

static int rxd_canvas_order(void *canvas) {
  if (!canvas || !g_il2cpp_base || !rxd_unity_alive(canvas)) return -999;
  int (*fn)(void *, void *) = (int (*)(void *, void *))(g_il2cpp_base + 0x2E1D698);
  return fn ? fn(canvas, NULL) : -999;
}

static void rxd_canvas_set_mode(void *canvas, int mode) {
  if (!canvas || !g_il2cpp_base || !rxd_unity_alive(canvas)) return;
  void (*fn)(void *, int, void *) =
      (void (*)(void *, int, void *))(g_il2cpp_base + 0x2E1D3D8);
  if (fn) fn(canvas, mode, NULL);
}

static void rxd_canvas_set_override_sorting(void *canvas, int value) {
  if (!canvas || !g_il2cpp_base || !rxd_unity_alive(canvas)) return;
  void (*fn)(void *, int, void *) =
      (void (*)(void *, int, void *))(g_il2cpp_base + 0x2E1D648);
  if (fn) fn(canvas, value, NULL);
}

static void rxd_canvas_set_order(void *canvas, int order) {
  if (!canvas || !g_il2cpp_base || !rxd_unity_alive(canvas)) return;
  void (*fn)(void *, int, void *) =
      (void (*)(void *, int, void *))(g_il2cpp_base + 0x2E1D6D8);
  if (fn) fn(canvas, order, NULL);
}

static void rxd_canvas_set_scale(void *canvas, float scale, float refppu) {
  if (!canvas || !g_il2cpp_base || !rxd_unity_alive(canvas)) return;
  void (*set_scale)(void *, float, void *) =
      (void (*)(void *, float, void *))(g_il2cpp_base + 0x2E1D4A8);
  void (*set_ref)(void *, float, void *) =
      (void (*)(void *, float, void *))(g_il2cpp_base + 0x2E1D538);
  if (set_scale) set_scale(canvas, scale, NULL);
  if (set_ref) set_ref(canvas, refppu, NULL);
}

typedef struct { float x, y; } rxd_v2;
typedef struct { float x, y, z; } rxd_v3;

static void rxd_recttransform_fullscreen(void *rt) {
  if (!rt || !g_il2cpp_base || !rxd_unity_alive(rt)) return;
  void (*set_anchor_min)(void *, rxd_v2, void *) =
      (void (*)(void *, rxd_v2, void *))(g_il2cpp_base + 0x18F4700);
  void (*set_anchor_max)(void *, rxd_v2, void *) =
      (void (*)(void *, rxd_v2, void *))(g_il2cpp_base + 0x18F484C);
  void (*set_anchored)(void *, rxd_v2, void *) =
      (void (*)(void *, rxd_v2, void *))(g_il2cpp_base + 0x18F4998);
  void (*set_size)(void *, rxd_v2, void *) =
      (void (*)(void *, rxd_v2, void *))(g_il2cpp_base + 0x18F4AE4);
  void (*set_offset_min)(void *, rxd_v2, void *) =
      (void (*)(void *, rxd_v2, void *))(g_il2cpp_base + 0x18F4DBC);
  void (*set_offset_max)(void *, rxd_v2, void *) =
      (void (*)(void *, rxd_v2, void *))(g_il2cpp_base + 0x18F4F10);
  void (*force_rect)(void *, void *) =
      (void (*)(void *, void *))(g_il2cpp_base + 0x18F4FEC);
  void (*set_scale)(void *, rxd_v3, void *) =
      (void (*)(void *, rxd_v3, void *))(g_il2cpp_base + 0x28D6FFC);
  rxd_v2 z = {0.0f, 0.0f};
  rxd_v2 one = {1.0f, 1.0f};
  rxd_v3 s = {1.0f, 1.0f, 1.0f};
  if (set_anchor_min) set_anchor_min(rt, z, NULL);
  if (set_anchor_max) set_anchor_max(rt, one, NULL);
  if (set_anchored) set_anchored(rt, z, NULL);
  if (set_size) set_size(rt, z, NULL);
  if (set_offset_min) set_offset_min(rt, z, NULL);
  if (set_offset_max) set_offset_max(rt, z, NULL);
  if (set_scale) set_scale(rt, s, NULL);
  if (force_rect) force_rect(rt, NULL);
}

static float rxd_cg_alpha(void *cg) {
  if (!cg || !g_il2cpp_base || !rxd_unity_alive(cg)) return -1.0f;
  float (*fn)(void *, void *) = (float (*)(void *, void *))(g_il2cpp_base + 0x2E1DCF8);
  return fn ? fn(cg, NULL) : -1.0f;
}

static void rxd_cg_force_visible(void *cg) {
  if (!cg || !g_il2cpp_base || !rxd_unity_alive(cg)) return;
  void (*set_alpha)(void *, float, void *) =
      (void (*)(void *, float, void *))(g_il2cpp_base + 0x2E1DD38);
  void (*set_interactable)(void *, int, void *) =
      (void (*)(void *, int, void *))(g_il2cpp_base + 0x2E1DDC8);
  void (*set_blocks)(void *, int, void *) =
      (void (*)(void *, int, void *))(g_il2cpp_base + 0x2E1DE58);
  if (set_alpha) set_alpha(cg, 1.0f, NULL);
  if (set_interactable) set_interactable(cg, 1, NULL);
  if (set_blocks) set_blocks(cg, 1, NULL);
}

static void rxd_orange_ui_set_canvas(void *ui, int enable) {
  if (!ui || !g_il2cpp_base) return;
  void (*fn)(void *, int, void *) =
      (void (*)(void *, int, void *))(g_il2cpp_base + 0x1070948);
  if (fn) fn(ui, enable, NULL);
}

static void *rxd_uim_get_uicamera(void *mgr) {
  if (!mgr || !g_il2cpp_base) return NULL;
  void *(*fn)(void *, void *) =
      (void *(*)(void *, void *))(g_il2cpp_base + 0x1164898);
  return fn ? fn(mgr, NULL) : NULL;
}

static void rxd_canvas_set_camera(void *canvas, void *cam) {
  if (!canvas || !cam || !g_il2cpp_base || !rxd_unity_alive(canvas) ||
      !rxd_unity_alive(cam)) return;
  void (*fn)(void *, void *, void *) =
      (void (*)(void *, void *, void *))(g_il2cpp_base + 0x2E1D960);
  if (fn) fn(canvas, cam, NULL);
}

static void rxd_canvas_force_update(void) {
  if (!g_il2cpp_base) return;
  void (*fn)(void *) = (void (*)(void *))(g_il2cpp_base + 0x2E1DA18);
  if (fn) fn(NULL);
}

static void rxd_log_uistate(const char *tag, void *mgr, void *ui, void *clone) {
  void *list = mgr ? *(void **)((char *)mgr + 0x50) : NULL;
  void *ui_parent = mgr ? *(void **)((char *)mgr + 0x68) : NULL;
  void *trans = mgr ? *(void **)((char *)mgr + 0x80) : NULL;
  void *block = mgr ? *(void **)((char *)mgr + 0x88) : NULL;
  void *mgr_cam_field = mgr ? *(void **)((char *)mgr + 0xA0) : NULL;
  void *uicam = rxd_uim_get_uicamera(mgr);
  void *ui_go = rxd_comp_go(ui);
  void *ui_tr = rxd_comp_tr(ui);
  void *clone_tr = rxd_go_tr(clone);
  void *parent_tr = rxd_tr_parent(clone_tr);
  void *canvas = ui ? *(void **)((char *)ui + 0x58) : NULL;
  void *cg = ui ? *(void **)((char *)ui + 0x68) : NULL;
  void *rt = ui ? *(void **)((char *)ui + 0x70) : NULL;
  void *parent_go = rxd_comp_go(ui_parent);
  fprintf(stderr,
          "[RXD_UISTATE] %s f=%d mgr=%p cp=%p has=%d list=%p size=%d parent=%p parentGo=%p pAct=%d/%d pChild=%d trans=%p block=%p camField=%p uicam=%p clone=%p cp=%p act=%d/%d layer=%d tr=%p par=%p parIsUi=%d ui=%p(%s) cp=%p go=%p act=%d/%d layer=%d tr=%p canvas=%p en=%d ae=%d mode=%d order=%d cg=%p alpha=%.3f rt=%p visible=%d lock=%d\n",
          tag ? tag : "?", g_render_frame, mgr, rxd_obj_cached(mgr),
          rxd_ui_has_core(mgr), list, rxd_list_size(list), ui_parent, parent_go,
          rxd_go_active_self(parent_go), rxd_go_active_hier(parent_go),
          rxd_tr_child_count(ui_parent), trans, block, mgr_cam_field, uicam,
          clone, rxd_obj_cached(clone), rxd_go_active_self(clone),
          rxd_go_active_hier(clone), rxd_go_layer(clone), clone_tr, parent_tr,
          parent_tr && parent_tr == ui_parent, ui, il2cpp_classname(ui),
          rxd_obj_cached(ui), ui_go, rxd_go_active_self(ui_go),
          rxd_go_active_hier(ui_go), rxd_go_layer(ui_go), ui_tr, canvas,
          rxd_beh_enabled(canvas), rxd_beh_active_enabled(canvas),
          rxd_canvas_mode(canvas), rxd_canvas_order(canvas), cg, rxd_cg_alpha(cg),
          rt, ui ? *(unsigned char *)((char *)ui + 0x2A) : -1,
          ui ? *(unsigned char *)((char *)ui + 0x78) : -1);
  fsync(2);
}

static void rxd_force_ui_visible(void *mgr, void *ui, void *clone) {
  if (!g_il2cpp_base) return;
  void *ui_parent = mgr ? *(void **)((char *)mgr + 0x68) : NULL;
  void *parent_go = rxd_comp_go(ui_parent);
  void *clone_tr = rxd_go_tr(clone);
  void *ui_go = rxd_comp_go(ui);
  void *canvas = ui ? *(void **)((char *)ui + 0x58) : NULL;
  void *raycaster = ui ? *(void **)((char *)ui + 0x60) : NULL;
  void *cg = ui ? *(void **)((char *)ui + 0x68) : NULL;
  void *uicam = rxd_uim_get_uicamera(mgr);
  int parent_layer = rxd_go_layer(parent_go);
  if (clone_tr && ui_parent) rxd_tr_set_parent(clone_tr, ui_parent, 0);
  if (parent_layer >= 0) {
    rxd_go_set_layer(clone, parent_layer);
    rxd_go_set_layer(ui_go, parent_layer);
  }
  rxd_go_set_active(parent_go, 1);
  rxd_go_set_active(clone, 1);
  rxd_go_set_active(ui_go, 1);
  if (ui) {
    *(unsigned char *)((char *)ui + 0x2A) = 1;  /* OrangeUIBase.IsVisible */
    *(unsigned char *)((char *)ui + 0x78) = 0;  /* OrangeUIBase.IsLock */
    rxd_orange_ui_set_canvas(ui, 1);
  }
  rxd_beh_set_enabled(canvas, 1);
  rxd_beh_set_enabled(raycaster, 1);
  int canvas_mode = env_int_default("TER_RXD_UILB_CANVAS_MODE", 0);
  const char *canvas_mode_env = getenv("TER_RXD_UILB_CANVAS_MODE");
  if (canvas && uicam && !env_on("TER_RXD_UILB_CANVAS_NOCAM")) {
    rxd_canvas_set_camera(canvas, uicam);
  }
  rxd_canvas_set_mode(canvas, canvas_mode);
  rxd_canvas_set_override_sorting(canvas, 1);
  rxd_canvas_set_order(canvas, env_int_default("TER_RXD_UILB_CANVAS_ORDER", 5000));
  rxd_canvas_set_scale(canvas, 1.0f, 100.0f);
  rxd_recttransform_fullscreen(ui ? *(void **)((char *)ui + 0x70) : NULL);
  rxd_cg_force_visible(cg);
  if (canvas && uicam && !env_on("TER_RXD_UILB_CANVAS_NOCAM")) {
    rxd_canvas_set_camera(canvas, uicam);
    rxd_canvas_set_mode(canvas, canvas_mode);
  }
  rxd_canvas_force_update();
  static unsigned force_log_n;
  if (force_log_n++ < 40 || (g_render_frame % 60) == 0) {
    fprintf(stderr,
            "[RXD_UIFORCE] mode_env=%s desired=%d actual=%d canvas=%p uicam=%p nocam=%d f=%d\n",
            canvas_mode_env ? canvas_mode_env : "(null)", canvas_mode,
            rxd_canvas_mode(canvas), canvas, uicam,
            env_on("TER_RXD_UILB_CANVAS_NOCAM"), g_render_frame);
    fsync(2);
  }
}

static int rxd_uim_call_activeui(void *method, void *mgr, void *ui, void *cb) {
  if (!method || !mgr || !ui || !g_il2cpp_base) return 0;
  void *rgctx0 = *(void **)((char *)method + 0x18);
  void *rgctx1 = rgctx0 ? *(void **)((char *)rgctx0 + 0xC0) : NULL;
  void *active_method = rgctx1 ? *(void **)((char *)rgctx1 + 0x08) : NULL;
  void *active_func = active_method ? *(void **)active_method : NULL;
  fprintf(stderr,
          "[RXD_UILB] ActiveUI ctx method=%p rgctx0=%p rgctx1=%p activeMethod=%p func=%p mgr=%p ui=%p(%s) cb=%p f=%d\n",
          method, rgctx0, rgctx1, active_method, active_func, mgr, ui,
          il2cpp_classname(ui), cb, g_render_frame);
  fsync(2);
  if (!active_func) return 0;
  void (*fn)(void *, void *, void *, int, void *) =
      (void (*)(void *, void *, void *, int, void *))active_func;
  fn(mgr, ui, cb, 0, active_method);
  fprintf(stderr, "[RXD_UILB] ActiveUI done mgr=%p ui=%p f=%d\n",
          mgr, ui, g_render_frame);
  fsync(2);
  return 1;
}

static void rxd_uim_loadui_manual(void *self, void *asset, void *method) {
  if (!g_il2cpp_base) return;
  void *mgr = self ? *(void **)((char *)self + 0x10) : NULL;
  if (!rxd_ui_logic_ok(mgr)) {
    void *good_mgr = rxd_reacquire_uimanager();
    if (rxd_ui_logic_ok(good_mgr)) mgr = good_mgr;
  }
  void *p_name = self ? *(void **)((char *)self + 0x18) : NULL;
  void *p_cb = self ? *(void **)((char *)self + 0x20) : NULL;
  void *ui_parent = mgr ? *(void **)((char *)mgr + 0x68) : NULL;
  char nbuf[160];

  void *title_ty = rxd_type_object_any("", "TitleNewUI");
  void *orange_ty = rxd_type_object_any("", "OrangeUIBase");
  void *(*instantiate_parent)(void *, void *, int, void *) =
      (void *(*)(void *, void *, int, void *))(g_il2cpp_base + 0x18E8E10);
  void (*set_name)(void *, void *, void *) =
      (void (*)(void *, void *, void *))(g_il2cpp_base + 0x18E8644);
  void *(*go_get_component)(void *, void *, void *) =
      (void *(*)(void *, void *, void *))(g_il2cpp_base + 0x1EC49FC);
  void *(*go_get_child)(void *, void *, int, void *) =
      (void *(*)(void *, void *, int, void *))(g_il2cpp_base + 0x1EC4B30);
  void (*delegate_invoke)(void *, void *, void *) =
      (void (*)(void *, void *, void *))(g_il2cpp_base + 0x2648548);

  fprintf(stderr,
          "[RXD_UILB] manual start name=%s mgr=%p has=%d parent=%p asset=%p(%s) cb=%p titleTy=%p orangeTy=%p f=%d\n",
          rxd_str(p_name, nbuf, sizeof nbuf), mgr, rxd_ui_has_core(mgr),
          ui_parent, asset, il2cpp_classname(asset), p_cb, title_ty, orange_ty,
          g_render_frame);
  fsync(2);

  void *clone = (asset && ui_parent && instantiate_parent) ?
      instantiate_parent(asset, ui_parent, 0, NULL) : NULL;
  fprintf(stderr, "[RXD_UILB] manual instantiate -> %p(%s) f=%d\n",
          clone, il2cpp_classname(clone), g_render_frame);
  fsync(2);
  if (!clone) return;

  if (set_name && p_name) set_name(clone, p_name, NULL);
  rxd_tr_set_parent(rxd_go_tr(clone), ui_parent, 0);
  void *title = (title_ty && go_get_component) ? go_get_component(clone, title_ty, NULL) : NULL;
  void *orange = (orange_ty && go_get_component) ? go_get_component(clone, orange_ty, NULL) : NULL;
  void *title_child = (title_ty && go_get_child) ? go_get_child(clone, title_ty, 1, NULL) : NULL;
  void *orange_child = (orange_ty && go_get_child) ? go_get_child(clone, orange_ty, 1, NULL) : NULL;
  fprintf(stderr,
          "[RXD_UILB] manual comps title=%p(%s) orange=%p(%s) titleChild=%p(%s) orangeChild=%p(%s) f=%d\n",
          title, il2cpp_classname(title), orange, il2cpp_classname(orange),
          title_child, il2cpp_classname(title_child), orange_child,
          il2cpp_classname(orange_child), g_render_frame);
  fsync(2);

  rxd_go_set_active(clone, 1);
  void *ui = title ? title : title_child;
  if (!ui) ui = orange ? orange : orange_child;
  g_rxd_last_title_mgr = mgr;
  g_rxd_last_title_ui = ui;
  g_rxd_last_title_clone = clone;
  rxd_log_uistate("manual-pre", mgr, ui, clone);
  if (env_on("TER_RXD_UILB_FORCEVISIBLE")) {
    rxd_force_ui_visible(mgr, ui, clone);
    rxd_log_uistate("manual-force", mgr, ui, clone);
  }
  if (ui && p_cb && env_on("TER_RXD_UILB_ACTIVEUI") &&
      rxd_uim_call_activeui(method, mgr, ui, p_cb)) {
    rxd_log_uistate("manual-activeui", mgr, ui, clone);
    if (env_on("TER_RXD_UILB_FORCEVISIBLE")) {
      rxd_force_ui_visible(mgr, ui, clone);
      rxd_log_uistate("manual-activeui-force", mgr, ui, clone);
    }
    return;
  }
  if (ui && p_cb && delegate_invoke) {
    fprintf(stderr, "[RXD_UILB] manual invoke cb=%p ui=%p(%s) f=%d\n",
            p_cb, ui, il2cpp_classname(ui), g_render_frame);
    fsync(2);
    delegate_invoke(p_cb, ui, NULL);
    fprintf(stderr, "[RXD_UILB] manual invoke done cb=%p ui=%p f=%d\n",
            p_cb, ui, g_render_frame);
    fsync(2);
    rxd_log_uistate("manual-cb", mgr, ui, clone);
  }
}

static void (*rxd_uim_loadui_b0_orig)(void *, void *, void *);
static void rxd_uim_loadui_b0_hook(void *self, void *asset, void *method) {
  void **mgr_slot = self ? (void **)((char *)self + 0x10) : NULL;
  void *mgr = mgr_slot ? *mgr_slot : NULL;
  void *good_mgr = rxd_reacquire_uimanager();
  void *p_name = self ? *(void **)((char *)self + 0x18) : NULL;
  void *p_cb = self ? *(void **)((char *)self + 0x20) : NULL;
  char nbuf[160];
  fprintf(stderr,
          "[RXD_UILB] LoadUI.b__0 pre self=%p mgr=%p has=%d alive=%d good=%p goodAlive=%d name=%s cb=%p asset=%p(%s) f=%d\n",
          self, mgr, rxd_ui_has_core(mgr), rxd_unity_alive(mgr), good_mgr,
          rxd_unity_alive(good_mgr),
          rxd_str(p_name, nbuf, sizeof nbuf), p_cb, asset, il2cpp_classname(asset),
          g_render_frame);
  fsync(2);
  if (mgr_slot && !rxd_ui_logic_ok(mgr) && rxd_ui_logic_ok(good_mgr)) {
    rxd_managed_store_ref(self, mgr_slot, good_mgr);
    mgr = good_mgr;
    fprintf(stderr, "[RXD_UILB] LoadUI.b__0 corrigiu mgr vazio -> %p f=%d\n",
            mgr, g_render_frame);
    fsync(2);
  }
  if (env_on("TER_RXD_UILB_MANUAL")) {
    rxd_uim_loadui_manual(self, asset, method);
    return;
  }
  rxd_uim_loadui_b0_orig(self, asset, method);
  fprintf(stderr, "[RXD_UILB] LoadUI.b__0 post self=%p mgr=%p has=%d asset=%p f=%d\n",
          self, mgr, rxd_ui_has_core(mgr), asset, g_render_frame);
  fsync(2);
}

static void rxd_patch_ui_awake_from_instance(void *obj) {
  static unsigned n = 0;
  if (!obj || !getenv("TER_RXD_NUKE_UI_AWAKE")) return;
  if (n++ >= 24 && !getenv("TER_RXD_UI_ENUM")) return;
  fprintf(stderr,
          "[RXD_UI_INST] self=%p joy=%p uiParent=%p loading=%p connecting=%p translucent=%p block=%p cam=%p active=%p bgdict=%p f=%d\n",
          obj,
          *(void **)((char *)obj + 0x60),
          *(void **)((char *)obj + 0x68),
          *(void **)((char *)obj + 0x70),
          *(void **)((char *)obj + 0x78),
          *(void **)((char *)obj + 0x80),
          *(void **)((char *)obj + 0x88),
          *(void **)((char *)obj + 0xA0),
          *(void **)((char *)obj + 0x50),
          *(void **)((char *)obj + 0x58),
          g_render_frame);
  fsync(2);
}

static int rxd_ui_sub_window_active(void) {
  int from = env_int_default("TER_RXD_UI_SUB_DEAD_FROM", -1);
  int until = env_int_default("TER_RXD_UI_SUB_DEAD_UNTIL", -1);
  return from >= 0 && g_render_frame >= from &&
         (until < 0 || g_render_frame <= until);
}

static int rxd_ui_copy_core_refs(void *dst, void *src, const char *why) {
  static const unsigned offs[] = {
      0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88, 0x98, 0xA0};
  if (!dst || !src || !rxd_ui_has_core(src)) return 0;
  for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
    void **d = (void **)((char *)dst + offs[i]);
    void *s = *(void **)((char *)src + offs[i]);
    if (s) rxd_managed_store_ref(dst, d, s);
  }
  fprintf(stderr,
          "[RXD_UI_INST] copiou refs UIManager %s dst=%p src=%p dst_has=%d dst_alive=%d src_alive=%d f=%d\n",
          why ? why : "fallback", dst, src, rxd_ui_has_core(dst),
          rxd_unity_alive(dst), rxd_unity_alive(src), g_render_frame);
  fsync(2);
  return rxd_ui_has_core(dst);
}

static void *rxd_ui_try_splash_singleton_fallback(void *ret) {
  if (!rxd_ui_sub_window_active() || rxd_ui_has_core(ret) ||
      !g_rxd_ui_mgr_good || !rxd_ui_has_core(g_rxd_ui_mgr_good)) {
    return ret;
  }

  if (env_on("TER_RXD_UI_SUB_COPY") && rxd_ui_copy_core_refs(ret, g_rxd_ui_mgr_good, "splash")) {
    return ret;
  }

  fprintf(stderr,
          "[RXD_UI_INST] substituindo UIManager vazio por antigo completo ret=%p antigo=%p antigo_alive=%d f=%d\n",
          ret, g_rxd_ui_mgr_good, rxd_unity_alive(g_rxd_ui_mgr_good),
          g_render_frame);
  fsync(2);
  return g_rxd_ui_mgr_good;
}

static void *(*rxd_mbs_get_instance_orig)(void *);
static void *rxd_mbs_get_instance_hook(void *method) {
  void *ret = rxd_mbs_get_instance_orig(method);
  if (ret) {
    const char *cn = il2cpp_classname(ret);
    static unsigned instlog_n;
    if (getenv("TER_RXD_INSTLOG") && instlog_n++ < 400) {
      fprintf(stderr, "[RXD_INST] ret=%p class=%s b20=%d b21=%d b22=%d method=%p f=%d\n",
              ret, cn ? cn : "(null)",
              *(unsigned char *)((char *)ret + 0x20),
              *(unsigned char *)((char *)ret + 0x21),
              *(unsigned char *)((char *)ret + 0x22),
              method, g_render_frame);
      fsync(2);
    }
    if (cn && !strcmp(cn, "AudioManager")) {
      if (g_rxd_audio_mgr != ret) {
        g_rxd_audio_mgr = ret;
        rxd_audio_log("AudioManager.Instance capturado", ret);
      }
    } else if (cn && !strcmp(cn, "ConsolePackageManager")) {
      if (g_rxd_console_mgr != ret) {
        g_rxd_console_mgr = ret;
        rxd_console_log("ConsolePackageManager.Instance capturado", ret);
      }
    } else if (cn && !strcmp(cn, "LocalizationManager")) {
      if (g_rxd_localization_mgr != ret) {
        g_rxd_localization_mgr = ret;
        rxd_localization_log("LocalizationManager.Instance capturado", ret);
      }
    } else if (cn && !strcmp(cn, "UIManager")) {
      rxd_patch_ui_awake_from_instance(ret);
      if (rxd_ui_logic_ok(ret)) {
        if (g_rxd_ui_mgr_good != ret) {
          g_rxd_ui_mgr_good = ret;
          if (rxd_unity_alive(ret)) g_rxd_ui_mgr_good_go = rxd_comp_go(ret);
          rxd_gc_pin(ret, "UIManager.instance");
          if (g_rxd_ui_mgr_good_go) rxd_gc_pin(g_rxd_ui_mgr_good_go, "UIManager.instance.go");
          fprintf(stderr, "[RXD_UI_INST] UIManager bom capturado=%p go=%p f=%d\n",
                  ret, g_rxd_ui_mgr_good_go, g_render_frame);
          fsync(2);
        }
      } else {
        void *good = rxd_reacquire_uimanager();
        if (rxd_ui_logic_ok(good) && good != ret) {
          fprintf(stderr, "[RXD_UI_INST] substituindo UIManager vazio %p -> bom %p f=%d\n",
                  ret, good, g_render_frame);
          fsync(2);
          ret = good;
        } else {
          void *fallback = rxd_ui_try_splash_singleton_fallback(ret);
          if (fallback != ret) {
            ret = fallback;
          } else if (g_rxd_ui_mgr_good && !rxd_unity_alive(g_rxd_ui_mgr_good)) {
            fprintf(stderr, "[RXD_UI_INST] bom antigo morto=%p; mantendo ret=%p has=%d alive=%d f=%d\n",
                    g_rxd_ui_mgr_good, ret, rxd_ui_has_core(ret),
                    rxd_unity_alive(ret), g_render_frame);
            fsync(2);
          }
        }
      }
    }
  }
  return ret;
}

static void (*rxd_abm_awake_orig)(void *, void *);
static void *volatile g_rxd_abm_good;
static int rxd_abm_has_manifest(void *self) {
  return self && *(void **)((char *)self + 0x30) &&
         *(void **)((char *)self + 0x40);
}

static void rxd_abm_note_good(void *self, const char *why) {
  if (!self) return;
  if (g_rxd_abm_good != self) {
    g_rxd_abm_good = self;
    fprintf(stderr,
            "[RXD_ABM] manager bom capturado=%p why=%s isLoad=%d manifest=%p dictID=%p dataPaths=%p f=%d\n",
            self, why ? why : "?",
            *(unsigned char *)((char *)self + 0x58),
            *(void **)((char *)self + 0x30),
            *(void **)((char *)self + 0x40),
            *(void **)((char *)self + 0x48), g_render_frame);
    fsync(2);
  }
}

static void rxd_abm_awake_hook(void *self, void *method) {
  fprintf(stderr, "[RXD_ABM] Awake self=%p f=%d\n", self, g_render_frame); fsync(2);
  rxd_abm_awake_orig(self, method);
  fprintf(stderr, "[RXD_ABM] Awake done self=%p isLoad=%d manifest=%p dictID=%p dataPaths=%p f=%d\n",
          self, self ? *(unsigned char *)((char *)self + 0x58) : -1,
          self ? *(void **)((char *)self + 0x30) : NULL,
          self ? *(void **)((char *)self + 0x40) : NULL,
          self ? *(void **)((char *)self + 0x48) : NULL, g_render_frame);
  if (rxd_abm_has_manifest(self)) rxd_abm_note_good(self, "Awake");
  fsync(2);
}
static void (*rxd_abm_init_orig)(void *, void *);
static void rxd_abm_init_hook(void *self, void *method) {
  fprintf(stderr, "[RXD_ABM] Init self=%p isLoad=%d f=%d\n",
          self, self ? *(unsigned char *)((char *)self + 0x58) : -1, g_render_frame); fsync(2);
  rxd_abm_init_orig(self, method);
}

static int rxd_ascii_endswith(const char *s, const char *suffix) {
  if (!s || !suffix) return 0;
  size_t sl = strlen(s), tl = strlen(suffix);
  if (tl > sl) return 0;
  return strcasecmp(s + sl - tl, suffix) == 0;
}

static struct {
  char hash[33];
  char name[120];
  int file_url;
} g_rxd_hash_modes[128];
static int g_rxd_hash_modes_n;

static void rxd_remember_hash_mode(const char *hash, const char *name, int file_url) {
  if (!hash || strlen(hash) != 32) return;
  for (int i = 0; i < g_rxd_hash_modes_n; i++) {
    if (!strcmp(g_rxd_hash_modes[i].hash, hash)) {
      g_rxd_hash_modes[i].file_url = file_url;
      return;
    }
  }
  if (g_rxd_hash_modes_n >= (int)(sizeof g_rxd_hash_modes / sizeof g_rxd_hash_modes[0])) return;
  int i = g_rxd_hash_modes_n++;
  snprintf(g_rxd_hash_modes[i].hash, sizeof g_rxd_hash_modes[i].hash, "%s", hash);
  snprintf(g_rxd_hash_modes[i].name, sizeof g_rxd_hash_modes[i].name, "%s", name ? name : "");
  g_rxd_hash_modes[i].file_url = file_url;
}

static int rxd_hash_needs_file_url(const char *hash, char *name, size_t name_cap) {
  if (!hash) return 0;
  for (int i = 0; i < g_rxd_hash_modes_n; i++) {
    if (!strcmp(g_rxd_hash_modes[i].hash, hash)) {
      if (name && name_cap) snprintf(name, name_cap, "%s", g_rxd_hash_modes[i].name);
      return g_rxd_hash_modes[i].file_url;
    }
  }
  return 0;
}

static void *(*rxd_abm_getpath_orig)(void *, int, void *, void *);
static void *rxd_abm_getpath_hook(void *self, int dp, void *file, void *method) {
  char fbuf[192], rbuf[256];
  void *ret = rxd_abm_getpath_orig(self, dp, file, method);
  const char *fstr = rxd_str(file, fbuf, sizeof fbuf);
  const char *rstr = rxd_str(ret, rbuf, sizeof rbuf);
  char hname[128];
  if (dp == 4 && ret && rxd_hash_needs_file_url(fstr, hname, sizeof hname) && strncmp(rstr, "file://", 7)) {
    void *(*isn)(const char *) = (void *(*)(const char *))ter_il2cpp_sym_cached("il2cpp_string_new");
    if (isn) {
      char url[640];
      snprintf(url, sizeof url, "file://%s", rstr);
      void *s = isn(url);
      if (s) {
        fprintf(stderr, "[RXD_ABM] GetPath file-url dp=%d file=%s name=%s -> %s f=%d\n",
                dp, fstr, hname, url, g_render_frame);
        fsync(2);
        ret = s;
        rstr = url;
      }
    }
  }
  static unsigned n;
  if (n++ < 240) {
    fprintf(stderr, "[RXD_ABM] GetPath dp=%d file=%s -> %s f=%d\n",
            dp, fstr, rstr, g_render_frame);
    fsync(2);
  }
  return ret;
}

static int rxd_byte_array_len(void *arr) {
  if (!arr) return -1;
  size_t len = *(size_t *)((char *)arr + 0x18);
  return len > 0x7fffffffU ? -2 : (int)len;
}

static int rxd_decrypt_abid_to_userdata(void *abid, const char *why);

static void rxd_log_abid(const char *tag, void *abid) {
  if (!abid) {
    fprintf(stderr, "%s abid=NULL\n", tag);
    return;
  }
  char n[160], h[96], p[96];
  fprintf(stderr, "%s abid=%p name=%s hash=%s crc=%u size=%lld package=%s\n",
          tag, abid,
          rxd_str(*(void **)((char *)abid + 0x10), n, sizeof n),
          rxd_str(*(void **)((char *)abid + 0x18), h, sizeof h),
          *(unsigned *)((char *)abid + 0x20),
          (long long)*(int64_t *)((char *)abid + 0x28),
          rxd_str(*(void **)((char *)abid + 0x30), p, sizeof p));
}

static void *(*rxd_abm_getbundleid_orig)(void *, void *, void *);
static void *rxd_abm_getbundleid_hook(void *self, void *bundle, void *method) {
  char b[192];
  void *ret_owner = self;
  void *ret = rxd_abm_getbundleid_orig(self, bundle, method);
  if (!ret && env_on("TER_RXD_ABM_GOOD_FALLBACK") && g_rxd_abm_good &&
      g_rxd_abm_good != self) {
    void *retry = rxd_abm_getbundleid_orig((void *)g_rxd_abm_good, bundle, method);
    if (retry) {
      fprintf(stderr, "[RXD_ABM] GetBundleID retry manager bom self=%p good=%p bundle=%s -> %p f=%d\n",
              self, (void *)g_rxd_abm_good, rxd_str(bundle, b, sizeof b),
              retry, g_render_frame);
      ret = retry;
      ret_owner = (void *)g_rxd_abm_good;
    }
  }
  static unsigned n;
  if (n++ < 180) {
    fprintf(stderr, "[RXD_ABM] GetBundleID bundle=%s -> %p f=%d\n",
            rxd_str(bundle, b, sizeof b), ret, g_render_frame);
    if (ret) rxd_log_abid("[RXD_ABM]   id", ret);
    fsync(2);
  }
  if (ret) rxd_abm_note_good(ret_owner, "GetBundleID");
  if (ret) rxd_decrypt_abid_to_userdata(ret, "GetBundleID");
  return ret;
}

static void (*rxd_owrl_load_orig)(void *, int, void *, int, void *, void *, void *);
static void rxd_owrl_load_hook(void *self, int type, void *fname, int flags, void *cb, void *server_crc, void *method) {
  char f[192], c[96];
  static unsigned n;
  if (n++ < 160) {
    fprintf(stderr, "[RXD_OWRL] Load self=%p type=%d file=%s flags=%d cb=%p crc=%s f=%d\n",
            self, type, rxd_str(fname, f, sizeof f), flags, cb,
            rxd_str(server_crc, c, sizeof c), g_render_frame);
    fsync(2);
  }
  rxd_owrl_load_orig(self, type, fname, flags, cb, server_crc, method);
}

static void (*rxd_owrl_load_b1_orig)(void *, void *, void *, void *);
static void rxd_owrl_load_b1_hook(void *self, void *bytes, void *loc, void *method) {
  char l[192];
  fprintf(stderr, "[RXD_OWRL] <Load>b__1 self=%p bytes=%p len=%d loc=%s p_cb=%p f=%d\n",
          self, bytes, rxd_byte_array_len(bytes), rxd_str(loc, l, sizeof l),
          self ? *(void **)((char *)self + 0x10) : NULL, g_render_frame);
  fsync(2);
  rxd_owrl_load_b1_orig(self, bytes, loc, method);
}

static void (*rxd_owrl_load_b0_orig)(void *, void *, void *, void *);
static void rxd_owrl_load_b0_hook(void *self, void *bytes, void *loc, void *method) {
  char l[192], lp[192];
  fprintf(stderr, "[RXD_OWRL] <Load>b__0 self=%p bytes=%p len=%d loc=%s localpath=%s parent=%p f=%d\n",
          self, bytes, rxd_byte_array_len(bytes), rxd_str(loc, l, sizeof l),
          self ? rxd_str(*(void **)((char *)self + 0x10), lp, sizeof lp) : "(null)",
          self ? *(void **)((char *)self + 0x18) : NULL, g_render_frame);
  fsync(2);
  rxd_owrl_load_b0_orig(self, bytes, loc, method);
}

static void (*rxd_design_cb_orig)(void *, void *, void *, void *);
static void rxd_design_cb_hook(void *self, void *bytes, void *loc, void *method) {
  char d[160], l[192];
  void *locals = self ? *(void **)((char *)self + 0x18) : NULL;
  int before = locals ? *(int *)((char *)locals + 0x18) : -1;
  fprintf(stderr, "[RXD_DESIGNCB] pre self=%p data=%s bytes=%p len=%d loc=%s loadCount=%d f=%d\n",
          self, self ? rxd_str(*(void **)((char *)self + 0x10), d, sizeof d) : "(null)",
          bytes, rxd_byte_array_len(bytes), rxd_str(loc, l, sizeof l), before, g_render_frame);
  fsync(2);
  rxd_design_cb_orig(self, bytes, loc, method);
  locals = self ? *(void **)((char *)self + 0x18) : NULL;
  int after = locals ? *(int *)((char *)locals + 0x18) : -1;
  fprintf(stderr, "[RXD_DESIGNCB] post self=%p loadCount=%d f=%d\n", self, after, g_render_frame);
  fsync(2);
}

static void *rxd_byte_array_from_file(const char *path, size_t *out_len) {
  if (out_len) *out_len = 0;
  if (!path || !g_il2cpp_base) return NULL;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size < 0 || st.st_size > 256 * 1024 * 1024) {
    close(fd);
    return NULL;
  }
  void *(*get_corlib)(void) = (void *(*)(void))ter_il2cpp_sym_cached("il2cpp_get_corlib");
  void *(*class_from_name)(void *, const char *, const char *) =
      (void *(*)(void *, const char *, const char *))ter_il2cpp_sym_cached("il2cpp_class_from_name");
  void *(*array_new)(void *, uintptr_t) =
      (void *(*)(void *, uintptr_t))ter_il2cpp_sym_cached("il2cpp_array_new");
  if (!get_corlib || !class_from_name || !array_new) {
    close(fd);
    return NULL;
  }
  static void *byte_class;
  if (!byte_class) byte_class = class_from_name(get_corlib(), "System", "Byte");
  if (!byte_class) {
    close(fd);
    return NULL;
  }
  size_t len = (size_t)st.st_size;
  void *arr = array_new(byte_class, len);
  if (!arr) {
    close(fd);
    return NULL;
  }
  char *dst = (char *)arr + 0x20;
  size_t got = 0;
  while (got < len) {
    ssize_t r = read(fd, dst + got, len - got);
    if (r <= 0) break;
    got += (size_t)r;
  }
  close(fd);
  if (got != len) return NULL;
  if (out_len) *out_len = len;
  return arr;
}

static int rxd_write_userdata_cache(const char *hash, void *bytes, const char *why) {
  if (!hash || !bytes) return 0;
  int outlen = rxd_byte_array_len(bytes);
  if (outlen <= 0 || outlen > 256 * 1024 * 1024) return 0;
  char out[512];
  snprintf(out, sizeof out, ASSET_BASE_M "userdata/%s", hash);
  mkdir(ASSET_BASE_M "userdata", 0755);
  int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fprintf(stderr, "[RXD_CACHE] falhou criar %s why=%s errno=%d\n", out, why ? why : "?", errno);
    fsync(2);
    return 0;
  }
  char *src = (char *)bytes + 0x20;
  int off = 0;
  while (off < outlen) {
    ssize_t w = write(fd, src + off, (size_t)(outlen - off));
    if (w <= 0) break;
    off += (int)w;
  }
  close(fd);
  if (off != outlen) {
    unlink(out);
    fprintf(stderr, "[RXD_CACHE] write parcial %s %d/%d why=%s\n", out, off, outlen, why ? why : "?");
    fsync(2);
    return 0;
  }
  fprintf(stderr, "[RXD_CACHE] userdata hash=%s len=%d why=%s -> %s\n", hash, outlen, why ? why : "?", out);
  fsync(2);
  return 1;
}

static int rxd_decrypt_abid_to_userdata(void *abid, const char *why) {
  if (!abid || !g_il2cpp_base) return 0;
  char namebuf[192], hashbuf[96], path[512];
  const char *name = rxd_str(*(void **)((char *)abid + 0x10), namebuf, sizeof namebuf);
  const char *hash = rxd_str(*(void **)((char *)abid + 0x18), hashbuf, sizeof hashbuf);
  if (strncmp(name, "criware/", 8) != 0) return 0;
  int direct_cri_file = rxd_ascii_endswith(name, ".acf") ||
                        rxd_ascii_endswith(name, ".acb") ||
                        rxd_ascii_endswith(name, ".awb");
  rxd_remember_hash_mode(hash, name, !direct_cri_file);
  if (!hash || strlen(hash) != 32 || !rxd_assetpack_file_path(hash, path, sizeof path)) {
    fprintf(stderr, "[RXD_CACHE] sem assetpack name=%s hash=%s why=%s\n", name, hash ? hash : "(null)", why ? why : "?");
    fsync(2);
    return 0;
  }
  char out[512];
  snprintf(out, sizeof out, ASSET_BASE_M "userdata/%s", hash);
  size_t raw_len = 0;
  void *bytes = rxd_byte_array_from_file(path, &raw_len);
  if (!bytes) {
    fprintf(stderr, "[RXD_CACHE] read fail name=%s hash=%s path=%s why=%s\n", name, hash, path, why ? why : "?");
    fsync(2);
    return 0;
  }
  void *keys = NULL;
  void *(*get_keys)(void *, void *) = (void *(*)(void *, void *))(g_il2cpp_base + 0x10ECCDC);
  void (*read_bytes)(void *, void **, void *) = (void (*)(void *, void **, void *))(g_il2cpp_base + 0x10EC938);
  keys = get_keys ? get_keys(abid, NULL) : NULL;
  if (keys && read_bytes) read_bytes(keys, &bytes, NULL);
  fprintf(stderr, "[RXD_CACHE] decrypt name=%s hash=%s raw=%zu keys=%p outlen=%d why=%s\n",
          name, hash, raw_len, keys, rxd_byte_array_len(bytes), why ? why : "?");
  fsync(2);
  return rxd_write_userdata_cache(hash, bytes, why);
}

static int (*rxd_cpm_assetsload_orig)(void *, void *, void *, void *);
static int rxd_cpm_assetsload_hook(void *self, void *abid, void *pb, void *method) {
  char n[160], h[96], p[96], path[512];
  const char *hash = abid ? rxd_str(*(void **)((char *)abid + 0x18), h, sizeof h) : "(null)";
  const char *pack = abid ? rxd_str(*(void **)((char *)abid + 0x30), p, sizeof p) : "(null)";
  fprintf(stderr, "[RXD_CPM] AssetsLoad self=%p abid=%p name=%s hash=%s package=%s pb=%p f=%d\n",
          self, abid,
          abid ? rxd_str(*(void **)((char *)abid + 0x10), n, sizeof n) : "(null)",
          hash, pack, pb, g_render_frame);
  fsync(2);
  if (abid && pb && hash && strcmp(hash, "(null)") != 0 && rxd_assetpack_file_path(hash, path, sizeof path)) {
    size_t len = 0;
    void *bytes = rxd_byte_array_from_file(path, &len);
    void *(*isn)(const char *) = (void *(*)(const char *))ter_il2cpp_sym_cached("il2cpp_string_new");
    void *loc = isn ? isn(path) : NULL;
    if (bytes) {
      void *keys = NULL;
      if (abid && g_il2cpp_base) {
        void *(*get_keys)(void *, void *) = (void *(*)(void *, void *))(g_il2cpp_base + 0x10ECCDC);
        void (*read_bytes)(void *, void **, void *) = (void (*)(void *, void **, void *))(g_il2cpp_base + 0x10EC938);
        keys = get_keys(abid, NULL);
        if (keys) read_bytes(keys, &bytes, NULL);
      }
      fprintf(stderr, "[RXD_CPM] local hit hash=%s len=%zu keys=%p outlen=%d path=%s -> invoke %p\n",
              hash, len, keys, rxd_byte_array_len(bytes), path, pb);
      fsync(2);
      rxd_decrypt_abid_to_userdata(abid, "AssetsLoad");
      void (*invoke)(void *, void *, void *, void *) =
          (void (*)(void *, void *, void *, void *))(g_il2cpp_base + 0x1995DE8);
      invoke(pb, bytes, loc, NULL);
      return 1;
    }
    fprintf(stderr, "[RXD_CPM] local read failed hash=%s path=%s\n", hash, path);
    fsync(2);
  }
  int ret = rxd_cpm_assetsload_orig(self, abid, pb, method);
  fprintf(stderr, "[RXD_CPM] AssetsLoad orig -> %d f=%d\n", ret, g_render_frame);
  fsync(2);
  return ret;
}

static void (*rxd_abm_olsa_cb_orig)(void *, void *, void *, void *);
static void rxd_abm_olsa_cb_hook(void *self, void *bytes, void *loc, void *method) {
  char b[192], l[256];
  int before = self ? *(unsigned char *)((char *)self + 0x40) : -1;
  void *old_bytes = self ? *(void **)((char *)self + 0x30) : NULL;
  fprintf(stderr,
          "[RXD_ABMCB] SingleAsset.cb pre self=%p bundle=%s bytes=%p len=%d loc=%s oldBytes=%p oldLen=%d goNext=%d f=%d\n",
          self,
          self ? rxd_str(*(void **)((char *)self + 0x28), b, sizeof b) : "(null)",
          bytes, rxd_byte_array_len(bytes), rxd_str(loc, l, sizeof l),
          old_bytes, rxd_byte_array_len(old_bytes), before, g_render_frame);
  fsync(2);
  rxd_abm_olsa_cb_orig(self, bytes, loc, method);
  int after = self ? *(unsigned char *)((char *)self + 0x40) : -1;
  void *new_bytes = self ? *(void **)((char *)self + 0x30) : NULL;
  void *new_loc = self ? *(void **)((char *)self + 0x38) : NULL;
  fprintf(stderr,
          "[RXD_ABMCB] SingleAsset.cb post self=%p bytes=%p len=%d loc=%s goNext=%d f=%d\n",
          self, new_bytes, rxd_byte_array_len(new_bytes), rxd_str(new_loc, l, sizeof l),
          after, g_render_frame);
  fsync(2);
}

static void *(*rxd_abm_olm_orig)(void *, void *);
static void *rxd_abm_olm_hook(void *self, void *method) {
  void *ret = rxd_abm_olm_orig(self, method);
  g_rxd_manifest_it = ret;
  g_rxd_manifest_done = 0;
  fprintf(stderr, "[RXD_ABM] OnStartLoadManifest self=%p -> it=%p f=%d\n", self, ret, g_render_frame);
  fsync(2);
  return ret;
}
static void (*rxd_abm_loadassets_orig)(void *, void *, void *, int, int, void *);
static void rxd_abm_loadassets_hook(void *self, void *arr, void *cb, int keep, int check, void *method) {
  fprintf(stderr, "[RXD_ABM] LoadAssets self=%p cb=%p keep=%d check=%d f=%d\n", self, cb, keep, check, g_render_frame);
  rxd_log_array("[RXD_ABM] LoadAssets names", arr); fsync(2);
  rxd_abm_loadassets_orig(self, arr, cb, keep, check, method);
}
static void *(*rxd_abm_ola_orig)(void *, void *, void *, int, int, void *);
static void *rxd_abm_ola_hook(void *self, void *arr, void *cb, int keep, int check, void *method) {
  void *ret = rxd_abm_ola_orig(self, arr, cb, keep, check, method);
  g_rxd_assets_it = ret;
  g_rxd_assets_done = 0;
  fprintf(stderr, "[RXD_ABM] OnStartLoadAssets self=%p -> it=%p keep=%d check=%d f=%d\n", self, ret, keep, check, g_render_frame);
  rxd_log_array("[RXD_ABM] OnStartLoadAssets names", arr); fsync(2);
  return ret;
}
static void *(*rxd_abm_olsa_orig)(void *, void *, int, void *);
static void *rxd_abm_olsa_hook(void *self, void *bundle, int keep, void *method) {
  char b[192];
  void *ret = rxd_abm_olsa_orig(self, bundle, keep, method);
  g_rxd_single_it = ret;
  g_rxd_single_done = 0;
  rxd_track_single_it(ret);
  fprintf(stderr, "[RXD_ABM] OnStartLoadSingleAsset bundle=%s keep=%d -> it=%p f=%d\n",
          rxd_str(bundle, b, sizeof b), keep, ret, g_render_frame); fsync(2);
  return ret;
}
static void *(*rxd_abm_gaal_orig)(void *, void *, void *, void *, int, void *);
static int rxd_is_titlepolicy_request(const char *s) {
  return s && (!strcasecmp(s, "UI_TitlePolicy") ||
               !strcasecmp(s, "ui/UI_TitlePolicy") ||
               !strcasecmp(s, "ui/ui_titlepolicy"));
}

static void rxd_abm_gaal_hook(void *self, void *bundle, void *asset, void *cb, int keep, void *method) {
  char b[160], a[160];
  const char *bs = rxd_str(bundle, b, sizeof b);
  const char *as = rxd_str(asset, a, sizeof a);
  fprintf(stderr,
          "[RXD_ABM] GetAssetAndAsyncLoad self=%p good=%p has=%d bundle=%s asset=%s cb=%p keep=%d f=%d\n",
          self, (void *)g_rxd_abm_good, rxd_abm_has_manifest(self),
          bs, as, cb, keep, g_render_frame);
  fsync(2);
  if (env_on("TER_RXD_GAAL_TITLE_NORM") &&
      (rxd_is_titlepolicy_request(bs) || rxd_is_titlepolicy_request(as))) {
    void *mgr = self;
    if (env_on("TER_RXD_ABM_GOOD_FALLBACK") && g_rxd_abm_good &&
        g_rxd_abm_good != self && !rxd_abm_has_manifest(self)) {
      fprintf(stderr,
              "[RXD_ABM] TitlePolicy usando manager bom self=%p good=%p f=%d\n",
              self, (void *)g_rxd_abm_good, g_render_frame);
      fsync(2);
      mgr = (void *)g_rxd_abm_good;
    }
    void *(*isn)(const char *) =
        (void *(*)(const char *))ter_il2cpp_sym_cached("il2cpp_string_new");
    static void *norm_bundle;
    static void *norm_asset;
    if (isn && !norm_bundle) norm_bundle = isn("ui/ui_titlepolicy");
    if (isn && !norm_asset) norm_asset = isn("UI_TitlePolicy");
    if (norm_bundle) {
      void *use_asset = asset ? asset : norm_asset;
      void *abid = rxd_abm_getbundleid_orig ?
          rxd_abm_getbundleid_orig(mgr, norm_bundle, NULL) : NULL;
      fprintf(stderr,
              "[RXD_ABM] TitlePolicy norm mgr=%p bundle=%s/%s -> ui/ui_titlepolicy asset=%s abid=%p f=%d\n",
              mgr, bs, as, use_asset ? "ok" : "NULL", abid, g_render_frame);
      if (abid) rxd_log_abid("[RXD_ABM]   title id", abid);
      fsync(2);
      rxd_abm_gaal_orig(mgr, norm_bundle, use_asset, cb, keep, method);
      return;
    }
  }
  if (env_on("TER_RXD_ABM_GOOD_FALLBACK") && g_rxd_abm_good &&
      g_rxd_abm_good != self && !rxd_abm_has_manifest(self)) {
    fprintf(stderr,
            "[RXD_ABM] GetAssetAndAsyncLoad usando manager bom self=%p good=%p bundle=%s f=%d\n",
            self, (void *)g_rxd_abm_good, bs, g_render_frame);
    fsync(2);
    rxd_abm_gaal_orig((void *)g_rxd_abm_good, bundle, asset, cb, keep, method);
    return;
  }
  rxd_abm_gaal_orig(self, bundle, asset, cb, keep, method);
}
static void *(*rxd_abm_asyncasset_orig)(void *, void *, void *, void *, int, void *);
static void *rxd_abm_asyncasset_hook(void *self, void *bundle, void *asset, void *cb, int keep, void *method) {
  char b[160], a[160];
  void *ret = rxd_abm_asyncasset_orig(self, bundle, asset, cb, keep, method);
  rxd_track_asyncasset_it(ret);
  fprintf(stderr, "[RXD_ABM] AsyncLoadAsset bundle=%s asset=%s -> it=%p keep=%d f=%d\n",
          rxd_str(bundle, b, sizeof b), rxd_str(asset, a, sizeof a), ret, keep, g_render_frame);
  fsync(2);
  return ret;
}
static long (*rxd_cr_manifest_orig)(void *, void *);
static long rxd_cr_manifest_hook(void *it, void *method) {
  g_rxd_manifest_it = it;
  static int last = -999; int st = it ? *(int *)((char *)it + 0x10) : -999;
  if (st != last) { rxd_log_cr("Manifest", it, 0, -1); last = st; }
  long ret = rxd_cr_manifest_orig(it, method);
  int st2 = it ? *(int *)((char *)it + 0x10) : -999;
  if (st2 != st || !ret) { rxd_log_cr("Manifest", it, 1, ret); last = st2; }
  if (!ret) g_rxd_manifest_done = 1;
  return ret;
}
static long (*rxd_cr_assets_orig)(void *, void *);
static long rxd_cr_assets_hook(void *it, void *method) {
  g_rxd_assets_it = it;
  static int last = -999, last_i = -999; int st = it ? *(int *)((char *)it + 0x10) : -999;
  int idx = it ? *(int *)((char *)it + 0x50) : -999;
  if (st != last || idx != last_i) { rxd_log_cr("Assets", it, 0, -1); last = st; last_i = idx; }
  long ret = rxd_cr_assets_orig(it, method);
  int st2 = it ? *(int *)((char *)it + 0x10) : -999;
  int idx2 = it ? *(int *)((char *)it + 0x50) : -999;
  if (st2 != st || idx2 != idx || !ret) { rxd_log_cr("Assets", it, 1, ret); last = st2; last_i = idx2; }
  if (!ret) g_rxd_assets_done = 1;
  return ret;
}
static long (*rxd_cr_single_orig)(void *, void *);
static long rxd_cr_single_hook(void *it, void *method) {
  g_rxd_single_it = it;
  rxd_track_single_it(it);
  static int last = -999, last_i = -999; int st = it ? *(int *)((char *)it + 0x10) : -999;
  int idx = it ? *(int *)((char *)it + 0x50) : -999;
  if (st != last || idx != last_i) { rxd_log_cr("Single", it, 0, -1); last = st; last_i = idx; }
  long ret = rxd_cr_single_orig(it, method);
  int st2 = it ? *(int *)((char *)it + 0x10) : -999;
  int idx2 = it ? *(int *)((char *)it + 0x50) : -999;
  if (st2 != st || idx2 != idx || !ret) { rxd_log_cr("Single", it, 1, ret); last = st2; last_i = idx2; }
  if (!ret) {
    g_rxd_single_done = 1;
    rxd_mark_single_done(it);
  }
  return ret;
}
static void *(*rxd_ab_lffa_orig)(void *, uint32_t, uint64_t, void *);
static void *rxd_ab_strip_file_url(void *path, char *orig, size_t orig_cap,
                                   char *call, size_t call_cap, const char *tag) {
  const char *p = rxd_str(path, orig, orig_cap);
  if (call && call_cap) snprintf(call, call_cap, "%s", p ? p : "(null)");
  if (!p || strncmp(p, "file://", 7) != 0) return path;
  void *(*isn)(const char *) = (void *(*)(const char *))ter_il2cpp_sym_cached("il2cpp_string_new");
  if (!isn) return path;
  const char *local = p + 7;
  void *np = isn(local);
  if (np) {
    if (call && call_cap) snprintf(call, call_cap, "%s", local);
    fprintf(stderr, "[RXD_AB] %s strip file-url %s -> %s f=%d\n",
            tag ? tag : "LoadFromFile", p, local, g_render_frame);
    fsync(2);
    return np;
  }
  return path;
}
static void *rxd_ab_lffa_hook(void *path, uint32_t crc, uint64_t off, void *method) {
  char p[256], call[256];
  void *path2 = rxd_ab_strip_file_url(path, p, sizeof p, call, sizeof call, "LoadFromFileAsync_Internal");
  void *ret = rxd_ab_lffa_orig(path2, crc, off, method);
  static unsigned n; if (n++ < 240) {
    fprintf(stderr, "[RXD_AB] LoadFromFileAsync_Internal path=%s call=%s crc=%u off=%llu -> %p f=%d\n",
            p, call, crc, (unsigned long long)off, ret, g_render_frame); fsync(2);
  }
  return ret;
}
static void *(*rxd_ab_lffa_pub_orig)(void *, void *);
static void *rxd_ab_lffa_pub_hook(void *path, void *method) {
  char p[256], call[256];
  void *path2 = rxd_ab_strip_file_url(path, p, sizeof p, call, sizeof call, "LoadFromFileAsync");
  void *ret = rxd_ab_lffa_pub_orig(path2, method);
  static unsigned n; if (n++ < 120) {
    fprintf(stderr, "[RXD_AB] LoadFromFileAsync path=%s call=%s -> %p f=%d\n",
            p, call, ret, g_render_frame); fsync(2);
  }
  return ret;
}
static void *(*rxd_ab_lffa_pub3_orig)(void *, uint32_t, uint64_t, void *);
static void *rxd_ab_lffa_pub3_hook(void *path, uint32_t crc, uint64_t off, void *method) {
  char p[256], call[256];
  void *path2 = rxd_ab_strip_file_url(path, p, sizeof p, call, sizeof call, "LoadFromFileAsync3");
  void *ret = rxd_ab_lffa_pub3_orig(path2, crc, off, method);
  static unsigned n; if (n++ < 120) {
    fprintf(stderr, "[RXD_AB] LoadFromFileAsync3 path=%s call=%s crc=%u off=%llu -> %p f=%d\n",
            p, call, crc, (unsigned long long)off, ret, g_render_frame); fsync(2);
  }
  return ret;
}
static void *(*rxd_ab_lff_orig)(void *, uint32_t, uint64_t, void *);
static void *rxd_ab_lff_hook(void *path, uint32_t crc, uint64_t off, void *method) {
  char p[256], call[256];
  void *path2 = rxd_ab_strip_file_url(path, p, sizeof p, call, sizeof call, "LoadFromFile_Internal");
  void *ret = rxd_ab_lff_orig(path2, crc, off, method);
  static unsigned n; if (n++ < 240) {
    fprintf(stderr, "[RXD_AB] LoadFromFile_Internal path=%s call=%s crc=%u off=%llu -> %p f=%d\n",
            p, call, crc, (unsigned long long)off, ret, g_render_frame); fsync(2);
  }
  return ret;
}
static void *(*rxd_ab_lfma_orig)(void *, uint32_t, void *);
static void *(*rxd_ab_lfm_orig)(void *, uint32_t, void *);
enum { RXD_ABREQ_MAX = 128 };
static void *g_rxd_abreq_req[RXD_ABREQ_MAX];
static void *g_rxd_abreq_bundle[RXD_ABREQ_MAX];
static int g_rxd_abreq_n;
enum { RXD_SCENEOP_MAX = 8 };
static void *g_rxd_sceneop_req[RXD_SCENEOP_MAX];
static int g_rxd_sceneop_frame[RXD_SCENEOP_MAX];
static int g_rxd_sceneop_n;
static void *volatile g_rxd_manual_scene_op;
static int g_rxd_manual_scene_frame = -1;
static void *volatile g_rxd_manual_scene_final_op;
static int g_rxd_manual_scene_final_done;
static volatile int g_rxd_scene_complete_seen;
static int g_rxd_scene_complete_frame = -1;
static int (*g_rxd_sceneop_integ_orig)(void *);
static void (*g_rxd_sceneop_final_orig)(void *);
static uintptr_t g_rxd_sceneop_guard_vt;

static void rxd_sceneop_dump_native(const char *tag, void *ptr) {
  if (!ptr || !env_on("TER_RXD_SCENEVT_DUMP")) return;
  static unsigned n;
  unsigned max = (unsigned)env_int_default("TER_RXD_SCENEVT_DUMP_MAX", 12);
  if (n++ >= max) return;
  uintptr_t *p = (uintptr_t *)ptr;
  uintptr_t vt = p[0];
  uintptr_t f50 = vt ? *(uintptr_t *)(vt + 0x50) : 0;
  uintptr_t f58 = vt ? *(uintptr_t *)(vt + 0x58) : 0;
  uintptr_t f60 = vt ? *(uintptr_t *)(vt + 0x60) : 0;
  uintptr_t f70 = vt ? *(uintptr_t *)(vt + 0x70) : 0;
  fprintf(stderr,
          "[RXD_SCENEVT_DUMP] %s ptr=%p vt=%p f50=%p f58=%p f60=%p f70=%p "
          "state48=%d progress6c=%.3f sub98=%p queueA0=%p mode3a0=%d "
          "allow3a4=%d ready3a5=%d flag3a6=%d f=%d\n",
          tag ? tag : "?", ptr, (void *)vt, (void *)f50, (void *)f58,
          (void *)f60, (void *)f70, *(int *)((char *)ptr + 0x48),
          *(float *)((char *)ptr + 0x6C), *(void **)((char *)ptr + 0x98),
          *(void **)((char *)ptr + 0xA0), *(int *)((char *)ptr + 0x3A0),
          *(unsigned char *)((char *)ptr + 0x3A4),
          *(unsigned char *)((char *)ptr + 0x3A5),
          *(unsigned char *)((char *)ptr + 0x3A6), g_render_frame);
  for (int k = 0; k < 24; k += 4) {
    fprintf(stderr,
            "[RXD_SCENEVT_DUMP]   +%02x: %016lx %016lx %016lx %016lx\n",
            k * 8, p[k], p[k + 1], p[k + 2], p[k + 3]);
  }
  fsync(2);
}

static void rxd_sceneop_apply_env(void *ptr) {
  if (!ptr) return;
  int v = env_int_default("TER_RXD_SCENEVT_FORCE_STATE", -999);
  if (v != -999) *(int *)((char *)ptr + 0x48) = v;
  v = env_int_default("TER_RXD_SCENEVT_FORCE_PROGRESS_BITS", -1);
  if (v >= 0) *(int *)((char *)ptr + 0x6C) = v;
  v = env_int_default("TER_RXD_SCENEVT_FORCE_MODE", -999);
  if (v != -999) *(int *)((char *)ptr + 0x3A0) = v;
  v = env_int_default("TER_RXD_SCENEVT_FORCE_ALLOW", -1);
  if (v >= 0) *(unsigned char *)((char *)ptr + 0x3A4) = (unsigned char)v;
  v = env_int_default("TER_RXD_SCENEVT_FORCE_READY", -1);
  if (v >= 0) *(unsigned char *)((char *)ptr + 0x3A5) = (unsigned char)v;
  v = env_int_default("TER_RXD_SCENEVT_FORCE_FLAG", -1);
  if (v >= 0) *(unsigned char *)((char *)ptr + 0x3A6) = (unsigned char)v;
}

static void *rxd_sceneop_current_target_ptr(void) {
  void *op = (void *)g_rxd_manual_scene_op;
  return op ? *(void **)((char *)op + 0x10) : NULL;
}

static int rxd_sceneop_is_target_ptr(void *ptr) {
  if (!ptr) return 0;
  if (env_on("TER_RXD_SCENEVT_GLOBAL")) return 1;
  if (g_rxd_scene_complete_seen && !env_on("TER_RXD_SCENEVT_AFTER_COMPLETE")) return 0;
  if (!env_on("TER_RXD_SCENEVT_TARGET_ONLY") && g_rxd_manual_scene_op) return 1;
  void *target = rxd_sceneop_current_target_ptr();
  if (target && ptr == target) return 1;
  return g_rxd_manual_scene_final_op && ptr == (void *)g_rxd_manual_scene_final_op;
}

static int rxd_sceneop_integ_guard(void *ptr) {
  if (!ptr) return 0;
  if (!rxd_sceneop_is_target_ptr(ptr))
    return g_rxd_sceneop_integ_orig ? g_rxd_sceneop_integ_orig(ptr) : 0;
  int state = *(int *)((char *)ptr + 0x48);
  void *queue = *(void **)((char *)ptr + 0xA0);
  if (state == 2 || !queue) {
    static unsigned n;
    if (n++ < 80) {
      fprintf(stderr, "[RXD_SCENEVT] integ guard ptr=%p state=%d queue=%p f=%d\n",
              ptr, state, queue, g_render_frame);
      fsync(2);
    }
    rxd_sceneop_dump_native("integ-pre", ptr);
    rxd_sceneop_apply_env(ptr);
    if (env_on("TER_RXD_SCENEVT_CALL_ORIG") && g_rxd_sceneop_integ_orig) {
      int ret = g_rxd_sceneop_integ_orig(ptr);
      if (n <= 80) {
        fprintf(stderr, "[RXD_SCENEVT] integ orig ret=%d state=%d queue=%p ready=%d f=%d\n",
                ret, *(int *)((char *)ptr + 0x48),
                *(void **)((char *)ptr + 0xA0),
                *(unsigned char *)((char *)ptr + 0x3A5), g_render_frame);
        fsync(2);
      }
      rxd_sceneop_dump_native("integ-post", ptr);
      if (env_on("TER_RXD_SCENEVT_FINAL_AFTER_ORIG") && g_rxd_sceneop_final_orig) {
        g_rxd_sceneop_final_orig(ptr);
        fprintf(stderr, "[RXD_SCENEVT] final-after-orig state=%d queue=%p ready=%d f=%d\n",
                *(int *)((char *)ptr + 0x48),
                *(void **)((char *)ptr + 0xA0),
                *(unsigned char *)((char *)ptr + 0x3A5), g_render_frame);
        fsync(2);
      }
      int over = env_int_default("TER_RXD_SCENEVT_RET_OVERRIDE", -999);
      return over != -999 ? over : ret;
    }
    if (env_on("TER_RXD_SCENEVT_FINAL_ON_GUARD") && g_rxd_sceneop_final_orig) {
      g_rxd_sceneop_final_orig(ptr);
      fprintf(stderr, "[RXD_SCENEVT] final-on-guard state=%d queue=%p ready=%d f=%d\n",
              *(int *)((char *)ptr + 0x48),
              *(void **)((char *)ptr + 0xA0),
              *(unsigned char *)((char *)ptr + 0x3A5), g_render_frame);
      fsync(2);
    }
    int over = env_int_default("TER_RXD_SCENEVT_RET_OVERRIDE", -999);
    return over != -999 ? over : env_int_default("TER_RXD_SCENEVT_GUARD_RET", 0);
  }
  return g_rxd_sceneop_integ_orig ? g_rxd_sceneop_integ_orig(ptr) : 0;
}

static void rxd_sceneop_final_guard(void *ptr) {
  if (!ptr) return;
  if (!rxd_sceneop_is_target_ptr(ptr)) {
    if (g_rxd_sceneop_final_orig) g_rxd_sceneop_final_orig(ptr);
    return;
  }
  int state = *(int *)((char *)ptr + 0x48);
  void *queue = *(void **)((char *)ptr + 0xA0);
  int call_noqueue = env_on("TER_RXD_SCENEVT_FINAL_CALL_ORIG_NOQUEUE") ||
      env_on("TER_RXD_SCENEVT_FINAL_CALL_ORIG");
  if (state == 2 || (!queue && !call_noqueue)) {
    static unsigned n;
    if (n++ < 80) {
      fprintf(stderr, "[RXD_SCENEVT] final guard bloqueado ptr=%p state=%d queue=%p f=%d\n",
              ptr, state, queue, g_render_frame);
      fsync(2);
    }
    rxd_sceneop_dump_native("final-block", ptr);
    return;
  }
  static unsigned n;
  if (n++ < 80) {
    fprintf(stderr, "[RXD_SCENEVT] final orig pre ptr=%p state=%d queue=%p progress=%.3f ready=%d f=%d\n",
            ptr, state, queue, *(float *)((char *)ptr + 0x6C),
            *(unsigned char *)((char *)ptr + 0x3A5), g_render_frame);
    fsync(2);
  }
  rxd_sceneop_dump_native("final-pre", ptr);
  rxd_sceneop_apply_env(ptr);
  if (g_rxd_sceneop_final_orig) g_rxd_sceneop_final_orig(ptr);
  if (n <= 80) {
    fprintf(stderr, "[RXD_SCENEVT] final orig post ptr=%p state=%d queue=%p progress=%.3f ready=%d f=%d\n",
            ptr, *(int *)((char *)ptr + 0x48),
            *(void **)((char *)ptr + 0xA0),
            *(float *)((char *)ptr + 0x6C),
            *(unsigned char *)((char *)ptr + 0x3A5), g_render_frame);
    fsync(2);
  }
  rxd_sceneop_dump_native("final-post", ptr);
}

static void rxd_sceneop_patch_vtable(uintptr_t vt) {
  if (!vt || g_rxd_sceneop_guard_vt == vt) return;
  long pgsz = sysconf(_SC_PAGESIZE);
  uintptr_t p0 = (vt + 0x58) & ~(uintptr_t)(pgsz - 1);
  uintptr_t p1 = (vt + 0x60) & ~(uintptr_t)(pgsz - 1);
  mprotect((void *)p0, pgsz, PROT_READ | PROT_WRITE);
  if (p1 != p0) mprotect((void *)p1, pgsz, PROT_READ | PROT_WRITE);
  void **integ = (void **)(vt + 0x58);
  void **final = (void **)(vt + 0x60);
  g_rxd_sceneop_integ_orig = (int (*)(void *))*integ;
  g_rxd_sceneop_final_orig = (void (*)(void *))*final;
  *integ = (void *)rxd_sceneop_integ_guard;
  *final = (void *)rxd_sceneop_final_guard;
  mprotect((void *)p0, pgsz, PROT_READ);
  if (p1 != p0) mprotect((void *)p1, pgsz, PROT_READ);
  __builtin___clear_cache((char *)(vt + 0x58), (char *)(vt + 0x68));
  g_rxd_sceneop_guard_vt = vt;
  fprintf(stderr, "[RXD_SCENEVT] vtable guard vt=%p integ=%p final=%p f=%d\n",
          (void *)vt, (void *)g_rxd_sceneop_integ_orig,
          (void *)g_rxd_sceneop_final_orig, g_render_frame);
  fsync(2);
}

static int rxd_abreq_find(void *req) {
  if (!req) return -1;
  for (int i = 0; i < g_rxd_abreq_n; i++)
    if (g_rxd_abreq_req[i] == req) return i;
  return -1;
}

static int rxd_sceneop_find(void *req) {
  if (!req) return -1;
  for (int i = 0; i < g_rxd_sceneop_n; i++)
    if (g_rxd_sceneop_req[i] == req) return i;
  return -1;
}

static void rxd_sceneop_store(void *req) {
  if (!req || !env_on("TER_RXD_SCENEOP_FORCE")) return;
  int i = rxd_sceneop_find(req);
  if (i < 0) {
    if (g_rxd_sceneop_n >= RXD_SCENEOP_MAX) return;
    i = g_rxd_sceneop_n++;
    g_rxd_sceneop_req[i] = req;
  }
  g_rxd_sceneop_frame[i] = g_render_frame;
  fprintf(stderr, "[RXD_SCENEOP] track op=%p start_f=%d total=%d\n",
          req, g_render_frame, g_rxd_sceneop_n);
  fsync(2);
}

static void rxd_abreq_store(void *req, void *bundle, const char *tag) {
  if (!req) return;
  int i = rxd_abreq_find(req);
  if (i < 0) {
    if (g_rxd_abreq_n >= RXD_ABREQ_MAX) return;
    i = g_rxd_abreq_n++;
    g_rxd_abreq_req[i] = req;
  }
  g_rxd_abreq_bundle[i] = bundle;
  fprintf(stderr, "[RXD_ABREQ] %s req=%p -> bundle=%p total=%d f=%d\n",
          tag ? tag : "store", req, bundle, g_rxd_abreq_n, g_render_frame);
  fsync(2);
}

static void *rxd_ab_sync_from_memory(void *bytes, uint32_t crc, const char *tag) {
  int len = bytes ? *(int *)((char *)bytes + 0x18) : -1;
  if (!bytes) return NULL;
  void *ret = NULL;
  if (rxd_ab_lfm_orig) ret = rxd_ab_lfm_orig(bytes, crc, NULL);
  else if (g_il2cpp_base) {
    void *(*sync_load)(void *, uint32_t, void *) =
        (void *(*)(void *, uint32_t, void *))(g_il2cpp_base + 0x2E2C118);
    ret = sync_load(bytes, crc, NULL);
  }
  fprintf(stderr, "[RXD_ABREQ] sync LoadFromMemory tag=%s bytes=%p len=%d crc=%u -> %p f=%d\n",
          tag ? tag : "?", bytes, len, crc, ret, g_render_frame);
  fsync(2);
  return ret;
}

static void (*rxd_async_set_allow_orig)(void *, int, void *);
static void rxd_async_set_allow_hook(void *op, int value, void *method) {
  fprintf(stderr, "[RXD_ASYNC] set_allowSceneActivation op=%p value=%d f=%d\n",
          op, value, g_render_frame);
  fsync(2);
  rxd_async_set_allow_orig(op, value, method);
  if (env_on("TER_RXD_ASYNC_ICALL")) rxd_log_asyncop("[RXD_ASYNC] after set_allow", op);
}

static int (*rxd_async_done_orig)(void *, void *);
static int rxd_async_done_hook(void *op, void *method) {
  if (rxd_abreq_find(op) >= 0) return 1;
  int si = rxd_sceneop_find(op);
  if (si >= 0) {
    int age = g_render_frame - g_rxd_sceneop_frame[si];
    int done_from = env_int_default("TER_RXD_SCENEOP_DONE_FROM", 90);
    if (age >= done_from) {
      static unsigned n;
      if (n++ < 40) {
        fprintf(stderr, "[RXD_SCENEOP] isDone force op=%p age=%d from=%d f=%d\n",
                op, age, done_from, g_render_frame);
        fsync(2);
      }
      return 1;
    }
  }
  return rxd_async_done_orig(op, method);
}

static float (*rxd_async_progress_orig)(void *, void *);
static float rxd_async_progress_hook(void *op, void *method) {
  if (rxd_abreq_find(op) >= 0) return 1.0f;
  int si = rxd_sceneop_find(op);
  if (si >= 0) {
    int age = g_render_frame - g_rxd_sceneop_frame[si];
    int prog_from = env_int_default("TER_RXD_SCENEOP_PROGRESS_FROM", 20);
    if (age >= prog_from) {
      static unsigned n;
      if (n++ < 40) {
        fprintf(stderr, "[RXD_SCENEOP] progress force op=%p age=%d from=%d f=%d\n",
                op, age, prog_from, g_render_frame);
        fsync(2);
      }
      return 1.0f;
    }
  }
  return rxd_async_progress_orig(op, method);
}

static void *(*rxd_ab_getbundle_orig)(void *, void *);
static void *rxd_ab_getbundle_hook(void *req, void *method) {
  int i = rxd_abreq_find(req);
  if (i >= 0) {
    static unsigned n;
    if (!g_rxd_abreq_bundle[i]) {
      void *ret = rxd_ab_getbundle_orig(req, method);
      if (ret) g_rxd_abreq_bundle[i] = ret;
    }
    if (n++ < 80) {
      fprintf(stderr, "[RXD_ABREQ] get_assetBundle req=%p -> %p f=%d\n",
              req, g_rxd_abreq_bundle[i], g_render_frame);
      fsync(2);
    }
    return g_rxd_abreq_bundle[i];
  }
  return rxd_ab_getbundle_orig(req, method);
}

static void *rxd_ab_lfma_hook(void *bytes, uint32_t crc, void *method) {
  int len = bytes ? *(int *)((char *)bytes + 0x18) : -1;
  void *ret = rxd_ab_lfma_orig(bytes, crc, method);
  void *bundle = env_on("TER_RXD_ABREQ_SYNC") ?
      rxd_ab_sync_from_memory(bytes, crc, "LoadFromMemoryAsync_Internal") : NULL;
  rxd_abreq_store(ret, bundle, "LoadFromMemoryAsync_Internal");
  static unsigned n; if (n++ < 240) {
    fprintf(stderr, "[RXD_AB] LoadFromMemoryAsync_Internal bytes=%p len=%d crc=%u -> %p f=%d\n",
            bytes, len, crc, ret, g_render_frame);
    if (ret) rxd_log_asyncop("[RXD_AB] LoadFromMemoryAsync request", ret);
    fsync(2);
  }
  return ret;
}
static void *(*rxd_ab_lfma_pub_orig)(void *, void *);
static void *rxd_ab_lfma_pub_hook(void *bytes, void *method) {
  int len = bytes ? *(int *)((char *)bytes + 0x18) : -1;
  void *ret = rxd_ab_lfma_pub_orig(bytes, method);
  void *bundle = env_on("TER_RXD_ABREQ_SYNC") ?
      rxd_ab_sync_from_memory(bytes, 0, "LoadFromMemoryAsync") : NULL;
  rxd_abreq_store(ret, bundle, "LoadFromMemoryAsync");
  static unsigned n; if (n++ < 160) {
    fprintf(stderr, "[RXD_AB] LoadFromMemoryAsync bytes=%p len=%d -> %p f=%d\n",
            bytes, len, ret, g_render_frame);
    if (ret) rxd_log_asyncop("[RXD_AB] LoadFromMemoryAsync request", ret);
    fsync(2);
  }
  return ret;
}
static void *rxd_ab_lfm_hook(void *bytes, uint32_t crc, void *method) {
  int len = bytes ? *(int *)((char *)bytes + 0x18) : -1;
  void *ret = rxd_ab_lfm_orig(bytes, crc, method);
  static unsigned n; if (n++ < 120) {
    fprintf(stderr, "[RXD_AB] LoadFromMemory_Internal bytes=%p len=%d crc=%u -> %p f=%d\n",
            bytes, len, crc, ret, g_render_frame);
    fsync(2);
  }
  return ret;
}
static void *(*rxd_ab_laa_orig)(void *, void *, void *, void *);
static void *rxd_ab_laa_hook(void *self, void *name, void *type, void *method) {
  char b[192]; void *ret = rxd_ab_laa_orig(self, name, type, method);
  static unsigned n; if (n++ < 240) {
    fprintf(stderr, "[RXD_AB] LoadAssetAsync_Internal ab=%p name=%s type=%p -> %p f=%d\n",
            self, rxd_str(name, b, sizeof b), type, ret, g_render_frame); fsync(2);
  }
  return ret;
}
static void *(*rxd_ab_loadasset_orig)(void *, void *, void *);
static void *rxd_ab_loadasset_hook(void *self, void *name, void *method) {
  char b[192]; void *ret = rxd_ab_loadasset_orig(self, name, method);
  static unsigned n; if (n++ < 120) {
    fprintf(stderr, "[RXD_AB] LoadAsset<T> ab=%p name=%s -> %p(%s) f=%d\n",
            self, rxd_str(name, b, sizeof b), ret, il2cpp_classname(ret), g_render_frame);
    fsync(2);
  }
  return ret;
}

static void *(*rxd_boot_start_orig)(void *, void *);
static void *volatile g_rxd_boot_start_it;
static void *volatile g_rxd_boot_warm_it;
static void *volatile g_rxd_design_it;
static volatile int g_rxd_boot_start_done;
static volatile int g_rxd_boot_warm_done;
static volatile int g_rxd_design_done;
static void *rxd_boot_start_hook(void *self, void *method) {
  void *ret = rxd_boot_start_orig(self, method);
  g_rxd_boot_start_it = ret;
  g_rxd_boot_start_done = 0;
  fprintf(stderr, "[RXD_BOOT] OrangeBootup.Start self=%p -> it=%p f=%d\n", self, ret, g_render_frame);
  fsync(2);
  return ret;
}
static void (*rxd_boot_setup_orig)(void *, void *);
static void rxd_boot_setup_hook(void *self, void *method) {
  fprintf(stderr, "[RXD_BOOT] SetupEnvironment self=%p f=%d\n", self, g_render_frame); fsync(2);
  rxd_boot_setup_orig(self, method);
  fprintf(stderr, "[RXD_BOOT] SetupEnvironment done self=%p f=%d\n", self, g_render_frame); fsync(2);
}
static void *(*rxd_boot_warm_orig)(void *, void *, void *);
static void *rxd_boot_warm_hook(void *self, void *obj, void *method) {
  void *ret = rxd_boot_warm_orig(self, obj, method);
  g_rxd_boot_warm_it = ret;
  g_rxd_boot_warm_done = 0;
  fprintf(stderr, "[RXD_BOOT] WarmUpSystemNeed self=%p obj=%p(%s) -> it=%p f=%d\n",
          self, obj, il2cpp_classname(obj), ret, g_render_frame); fsync(2);
  return ret;
}
static void (*rxd_boot_cb_orig)(void *, void *, void *);
static void rxd_boot_cb_hook(void *self, void *obj, void *method) {
  fprintf(stderr, "[RXD_BOOT] Start_b__1_0 self=%p obj=%p(%s) f=%d\n",
          self, obj, il2cpp_classname(obj), g_render_frame); fsync(2);
  rxd_boot_cb_orig(self, obj, method);
}

static void rxd_log_boot_it(const char *tag, void *it, int after, long ret) {
  if (!it) return;
  int st = *(int *)((char *)it + 0x10);
  void *cur = *(void **)((char *)it + 0x18);
  void *a = *(void **)((char *)it + 0x20);
  void *b = *(void **)((char *)it + 0x28);
  fprintf(stderr, "[RXD_BOOTCR] %s %s state=%d ret=%ld a=%p(%s) b=%p(%s) cur=%p(%s) f=%d\n",
          tag, after ? "post" : "pre", st, ret, a, il2cpp_classname(a),
          b, il2cpp_classname(b), cur, il2cpp_classname(cur), g_render_frame);
  fsync(2);
}
static long (*rxd_boot_start_cr_orig)(void *, void *);
static long rxd_boot_start_cr_hook(void *it, void *method) {
  g_rxd_boot_start_it = it;
  static int last = -999; int st = it ? *(int *)((char *)it + 0x10) : -999;
  if (st != last) { rxd_log_boot_it("Start", it, 0, -1); last = st; }
  long ret = rxd_boot_start_cr_orig(it, method);
  int st2 = it ? *(int *)((char *)it + 0x10) : -999;
  if (st2 != st || !ret) { rxd_log_boot_it("Start", it, 1, ret); last = st2; }
  if (!ret) g_rxd_boot_start_done = 1;
  return ret;
}
static long (*rxd_boot_warm_cr_orig)(void *, void *);
static long rxd_boot_warm_cr_hook(void *it, void *method) {
  g_rxd_boot_warm_it = it;
  static int last = -999; int st = it ? *(int *)((char *)it + 0x10) : -999;
  if (st != last) { rxd_log_boot_it("WarmUp", it, 0, -1); last = st; }
  long ret = rxd_boot_warm_cr_orig(it, method);
  int st2 = it ? *(int *)((char *)it + 0x10) : -999;
  if (st2 != st || !ret) { rxd_log_boot_it("WarmUp", it, 1, ret); last = st2; }
  if (!ret) g_rxd_boot_warm_done = 1;
  return ret;
}

static int rxd_is_design_cr(void *it) {
  const char *cn = il2cpp_classname(it);
  return cn && strstr(cn, "LoadDesignsData");
}

static void *rxd_boot_current(void) {
  void *it = (void *)g_rxd_boot_start_it;
  return it ? *(void **)((char *)it + 0x18) : NULL;
}

static void rxd_log_design_it(const char *tag, void *it, int after, long ret) {
  if (!it) return;
  int st = *(int *)((char *)it + 0x10);
  void *cur = *(void **)((char *)it + 0x18);
  void *self = *(void **)((char *)it + 0x20);
  void *locals = *(void **)((char *)it + 0x28);
  fprintf(stderr, "[RXD_DESIGNCR] %s %s state=%d ret=%ld self=%p(%s) locals=%p cur=%p(%s) f=%d\n",
          tag, after ? "post" : "pre", st, ret, self, il2cpp_classname(self),
          locals, cur, il2cpp_classname(cur), g_render_frame);
  fsync(2);
}

static long (*rxd_design_cr_orig)(void *, void *);
static long rxd_design_cr_hook(void *it, void *method) {
  g_rxd_design_it = it;
  static int last = -999; int st = it ? *(int *)((char *)it + 0x10) : -999;
  if (st != last) { rxd_log_design_it("LoadDesignsData", it, 0, -1); last = st; }
  long ret = rxd_design_cr_orig(it, method);
  int st2 = it ? *(int *)((char *)it + 0x10) : -999;
  if (st2 != st || !ret) { rxd_log_design_it("LoadDesignsData", it, 1, ret); last = st2; }
  if (!ret) g_rxd_design_done = 1;
  return ret;
}

static void (*rxd_audio_awake_orig)(void *, void *);
static void rxd_audio_awake_hook(void *self, void *method) {
  g_rxd_audio_mgr = self;
  rxd_audio_log("Awake pre", self);
  rxd_audio_awake_orig(self, method);
  rxd_audio_log("Awake post", self);
}
static void (*rxd_audio_onenable_orig)(void *, void *);
static void rxd_audio_onenable_hook(void *self, void *method) {
  g_rxd_audio_mgr = self;
  rxd_audio_log("OnEnable pre", self);
  rxd_audio_onenable_orig(self, method);
  rxd_audio_log("OnEnable post", self);
}
static void (*rxd_audio_init_orig)(void *, void *, void *);
static void rxd_audio_init_hook(void *self, void *cb, void *method) {
  g_rxd_audio_mgr = self;
  fprintf(stderr, "[RXD_AUDIO] Init self=%p cb=%p f=%d\n", self, cb, g_render_frame);
  rxd_audio_log("Init pre", self);
  if (env_on("TER_RXD_AUDIO_BOOT_FAKE")) {
    if (self) {
      *(unsigned char *)((char *)self + 0x21) = 1;
      *(unsigned char *)((char *)self + 0x22) = 1;
    }
    fprintf(stderr, "[RXD_AUDIO] Init fake: IsInitAll/IsInitSystemSE=1, pula LoadAcf/Preload f=%d\n",
            g_render_frame);
    fsync(2);
    return;
  }
  rxd_audio_init_orig(self, cb, method);
  rxd_audio_log("Init post", self);
}
static void (*rxd_audio_loadacf_orig)(void *, void *, void *);
static void rxd_audio_loadacf_hook(void *self, void *cb, void *method) {
  g_rxd_audio_mgr = self;
  fprintf(stderr, "[RXD_AUDIO] LoadAcf self=%p cb=%p f=%d\n", self, cb, g_render_frame);
  rxd_audio_loadacf_orig(self, cb, method);
  rxd_audio_log("LoadAcf post", self);
}
static void (*rxd_audio_preload_orig)(void *, void *);
static void rxd_audio_preload_hook(void *self, void *method) {
  g_rxd_audio_mgr = self;
  rxd_audio_log("PreloadInitAudio pre", self);
  if (env_on("TER_RXD_AUDIO_BOOT_FAKE")) {
    if (self) {
      *(unsigned char *)((char *)self + 0x21) = 1;
      *(unsigned char *)((char *)self + 0x22) = 1;
    }
    fprintf(stderr, "[RXD_AUDIO] PreloadInitAudio fake: pula ACB/AWB boot f=%d\n", g_render_frame);
    fsync(2);
    return;
  }
  rxd_audio_preload_orig(self, method);
  rxd_audio_log("PreloadInitAudio post", self);
}
static void (*rxd_audio_preload_cb_orig)(void *, void *);
static void rxd_audio_preload_cb_hook(void *self, void *method) {
  g_rxd_audio_mgr = self;
  rxd_audio_log("PreloadInitAudio_cb pre", self);
  if (env_on("TER_RXD_AUDIO_BOOT_FAKE")) {
    fprintf(stderr, "[RXD_AUDIO] PreloadInitAudio_cb fake skip f=%d\n", g_render_frame);
    fsync(2);
    return;
  }
  rxd_audio_preload_cb_orig(self, method);
  rxd_audio_log("PreloadInitAudio_cb post", self);
}

static void (*rxd_scene_awake_orig)(void *, void *);
static void *volatile g_rxd_scene_mgr;
static void *volatile g_rxd_scene_it;
static volatile int g_rxd_scene_done;
static volatile int g_rxd_scene_change_seen;
static volatile int g_rxd_scene_change_frame = -1;
static void rxd_log_scene_it(const char *tag, void *it, int after, long ret) {
  if (!it) return;
  int st = *(int *)((char *)it + 0x10);
  void *cur = *(void **)((char *)it + 0x18);
  void *self = *(void **)((char *)it + 0x20);
  fprintf(stderr, "[RXD_SCENECR] %s %s state=%d ret=%ld self=%p(%s) cur=%p(%s) f=%d\n",
          tag, after ? "post" : "pre", st, ret, self, il2cpp_classname(self),
          cur, il2cpp_classname(cur), g_render_frame);
  if (env_on("TER_RXD_SCENE_OPLOG") && st >= 3) {
    void *op = *(void **)((char *)it + 0x30);
    if (op) rxd_log_asyncop("[RXD_SCENECR] scene op", op);
  }
  if (env_on("TER_RXD_SCENE_DUMP")) {
    uintptr_t *w = (uintptr_t *)it;
    fprintf(stderr, "[RXD_SCENECR] %s raw +20..+88:", tag);
    for (int off = 0x20; off <= 0x88; off += 8)
      fprintf(stderr, " +%02x=%016lx", off, (unsigned long)w[off / 8]);
    fprintf(stderr, "\n");
  }
  fsync(2);
}
static long rxd_runtime_movenext(void *it, const char *tag) {
  if (!it || !g_il2cpp_base) return 0;
  void *(*obj_cls)(void *) = (void *(*)(void *))ter_il2cpp_sym_cached("il2cpp_object_get_class");
  void *(*cls_method)(void *, const char *, int) =
      (void *(*)(void *, const char *, int))ter_il2cpp_sym_cached("il2cpp_class_get_method_from_name");
  void *(*rt_invoke)(void *, void *, void **, void **) =
      (void *(*)(void *, void *, void **, void **))ter_il2cpp_sym_cached("il2cpp_runtime_invoke");
  if (!obj_cls || !cls_method || !rt_invoke) return 0;
  void *cls = obj_cls(it);
  void *m = cls ? cls_method(cls, "MoveNext", 0) : NULL;
  if (!m) {
    fprintf(stderr, "[RXD_SCENECR] %s MoveNext nao encontrado it=%p cls=%p(%s)\n",
            tag, it, cls, il2cpp_classname(it));
    fsync(2);
    return 0;
  }
  rxd_log_scene_it(tag, it, 0, -1);
  void *exc = NULL;
  void *ret = rt_invoke(m, it, NULL, &exc);
  long ok = ret ? *(unsigned char *)((char *)ret + 0x10) : 0;
  if (exc) {
    void (*fmt_exc)(void *, char *, int) =
        (void (*)(void *, char *, int))ter_il2cpp_sym_cached("il2cpp_format_exception");
    void (*fmt_stack)(void *, char *, int) =
        (void (*)(void *, char *, int))ter_il2cpp_sym_cached("il2cpp_format_stack_trace");
    char msg[1024] = {0};
    char stack[2048] = {0};
    if (fmt_exc) fmt_exc(exc, msg, sizeof msg);
    if (fmt_stack) fmt_stack(exc, stack, sizeof stack);
    fprintf(stderr, "[RXD_SCENECR] %s MoveNext exception=%p(%s) msg=%s stack=%s\n",
            tag, exc, il2cpp_classname(exc), msg[0] ? msg : "?",
            stack[0] ? stack : "?");
    fsync(2);
    ok = 0;
  }
  rxd_log_scene_it(tag, it, 1, ok);
  return ok;
}
static void rxd_scene_awake_hook(void *self, void *method) {
  g_rxd_scene_mgr = self;
  fprintf(stderr, "[RXD_SCENE] OrangeSceneManager.Awake self=%p now=%p loading=%d f=%d\n",
          self, self ? *(void **)((char *)self + 0x18) : NULL,
          self ? *(unsigned char *)((char *)self + 0x30) : -1, g_render_frame);
  fsync(2);
  rxd_scene_awake_orig(self, method);
}
static void (*rxd_scene_change_orig)(void *, void *, int, void *, int, int, void *);
static void rxd_scene_change_hook(void *self, void *scene, int type, void *cb, int clear_se, int skip_same, void *method) {
  char s[128];
  const char *scene_name = rxd_str(scene, s, sizeof s);
  fprintf(stderr, "[RXD_SCENE] ChangeScene scene=%s type=%d cb=%p clear=%d skip=%d self=%p f=%d\n",
          scene_name, type, cb, clear_se, skip_same, self, g_render_frame); fsync(2);
  g_rxd_scene_complete_seen = 0;
  g_rxd_scene_complete_frame = -1;
  if (env_list_has_value("TER_RXD_SCENE_SKIP", scene_name)) {
    fprintf(stderr, "[RXD_SCENE] ChangeScene SKIP scene=%s self=%p f=%d\n",
            scene_name, self, g_render_frame);
    fsync(2);
    return;
  }
  char remap[128];
  const char *new_name = env_remap_value("TER_RXD_SCENE_REMAP", scene_name, remap, sizeof remap);
  if (new_name) {
    void *(*isn)(const char *) = (void *(*)(const char *))ter_il2cpp_sym_cached("il2cpp_string_new");
    void *new_scene = isn ? isn(new_name) : NULL;
    fprintf(stderr, "[RXD_SCENE] ChangeScene REMAP %s -> %s str=%p f=%d\n",
            scene_name, new_name, new_scene, g_render_frame);
    fsync(2);
    if (new_scene) {
      scene = new_scene;
      scene_name = new_name;
    }
  }
  rxd_scene_change_orig(self, scene, type, cb, clear_se, skip_same, method);
  if (self) g_rxd_scene_mgr = self;
  g_rxd_scene_change_seen = 1;
  g_rxd_scene_change_frame = g_render_frame;
  char n[128];
  fprintf(stderr, "[RXD_SCENE] ChangeScene post now=%s loading=%d self=%p f=%d\n",
          self ? rxd_str(*(void **)((char *)self + 0x18), n, sizeof n) : "(null)",
          self ? *(unsigned char *)((char *)self + 0x30) : -1, self, g_render_frame);
  fsync(2);
}
static void *(*rxd_scene_onchange_orig)(void *, void *);
static void *rxd_scene_onchange_hook(void *self, void *method) {
  void *ret = rxd_scene_onchange_orig(self, method);
  g_rxd_scene_it = ret;
  g_rxd_scene_done = 0;
  g_rxd_scene_complete_seen = 0;
  g_rxd_scene_complete_frame = -1;
  char n[128];
  fprintf(stderr, "[RXD_SCENE] OnStartChangeScene self=%p now=%s -> it=%p f=%d\n",
          self, self ? rxd_str(*(void **)((char *)self + 0x18), n, sizeof n) : "(null)",
          ret, g_render_frame); fsync(2);
  return ret;
}
static void rxd_set_active_scene_managed_tick(int f);
static void rxd_dump_component_array(const char *tag, const char *ns, const char *cn);
static void (*rxd_scene_complete_orig)(void *, void *);
static void rxd_scene_complete_hook(void *self, void *method) {
  char n[128];
  fprintf(stderr, "[RXD_SCENE] ChangeSceneComplete self=%p now=%s f=%d\n",
          self, self ? rxd_str(*(void **)((char *)self + 0x18), n, sizeof n) : "(null)", g_render_frame);
  fsync(2);
  if (env_on("TER_RXD_NO_LOADINGUI")) {
    void *cb = self ? *(void **)((char *)self + 0x48) : NULL;
    if (self) {
      *(unsigned char *)((char *)self + 0x30) = 0;  /* IsLoading */
      *(void **)((char *)self + 0x48) = NULL;       /* ChangeSceneCallback */
    }
    fprintf(stderr, "[RXD_SCENE] ChangeSceneComplete minimal loading=0 cb=%p f=%d\n",
            cb, g_render_frame);
    fsync(2);
    rxd_invoke_callback0(cb, "ChangeSceneComplete");
    if (env_on("TER_RXD_SET_ACTIVE_ON_COMPLETE"))
      rxd_set_active_scene_managed_tick(g_render_frame);
    if (env_on("TER_RXD_DUMP_ON_COMPLETE")) {
      rxd_dump_component_array("Camera", "UnityEngine", "Camera");
      rxd_dump_component_array("Canvas", "UnityEngine", "Canvas");
    }
    g_rxd_scene_complete_seen = 1;
    g_rxd_scene_complete_frame = g_render_frame;
    g_rxd_manual_scene_final_done = 1;
    return;
  }
  rxd_scene_complete_orig(self, method);
  if (env_on("TER_RXD_SET_ACTIVE_ON_COMPLETE"))
    rxd_set_active_scene_managed_tick(g_render_frame);
  if (env_on("TER_RXD_DUMP_ON_COMPLETE")) {
    rxd_dump_component_array("Camera", "UnityEngine", "Camera");
    rxd_dump_component_array("Canvas", "UnityEngine", "Canvas");
  }
  g_rxd_scene_complete_seen = 1;
  g_rxd_scene_complete_frame = g_render_frame;
  g_rxd_manual_scene_final_done = 1;
}
static void (*rxd_scene_additive_orig)(void *, void *, void *, void *, void *);
static void rxd_scene_additive_hook(void *self, void *scene, void *cb, void *extra, void *method) {
  char s[128];
  fprintf(stderr, "[RXD_SCENE] AdditiveScene scene=%s cb=%p extra=%p self=%p f=%d\n",
          rxd_str(scene, s, sizeof s), cb, extra, self, g_render_frame); fsync(2);
  rxd_scene_additive_orig(self, scene, cb, extra, method);
}
static void *(*rxd_usm_loadasync_orig)(void *, int, uint64_t, int, void *);
static int rxd_scene_index_from_name(const char *s) {
  if (!s || !*s || !strcmp(s, "(null)")) return -1;
  const char *base = strrchr(s, '/');
  base = base ? base + 1 : s;
  char b[96];
  snprintf(b, sizeof b, "%s", base);
  char *dot = strrchr(b, '.');
  if (dot && !strcasecmp(dot, ".unity")) *dot = 0;
  struct { const char *name; int idx; } map[] = {
    { "bootup", 0 }, { "notice", 1 }, { "splash", 2 }, { "switch", 3 },
    { "title", 4 }, { "hometop", 5 }, { "StageTest", 6 }, { "WorldView", 7 },
    { "EndingStage", 8 }, { "OpeningStage", 9 },
  };
  for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
    if (!strcasecmp(b, map[i].name)) return map[i].idx;
  return -1;
}
static void *rxd_usm_loadasync_hook(void *scene, int build, uint64_t parms, int must, void *method) {
  char s[128];
  const char *name = rxd_str(scene, s, sizeof s);
  int mapped = (build < 0) ? rxd_scene_index_from_name(name) : -1;
  int no_map = env_on("TER_RXD_NO_SCENE_MAP");
  int do_map = mapped >= 0 && !no_map;
  void *call_scene = scene;
  int call_build = build;
  int call_must = must;
  if (do_map) {
    call_scene = env_on("TER_RXD_SCENE_MAP_KEEP_SCENE") ? scene : NULL;
    call_build = mapped;
    if (env_on("TER_RXD_SCENE_MAP_MUST")) call_must = 1;
  }
  void *ret = rxd_usm_loadasync_orig(call_scene, call_build, parms, call_must, method);
  fprintf(stderr, "[RXD_USCENE] LoadSceneAsyncNameIndexInternal scene=%s build=%d mapped=%d do_map=%d call_scene=%p call_build=%d parms=0x%llx must=%d->%d -> %p f=%d\n",
          name, build, mapped, do_map, call_scene, call_build,
          (unsigned long long)parms, must, call_must, ret, g_render_frame);
  if (ret) {
    g_rxd_scene_complete_seen = 0;
    g_rxd_scene_complete_frame = -1;
  }
  if (ret) rxd_sceneop_store(ret);
  if (ret && !g_rxd_manual_scene_op &&
      (env_on("TER_RXD_NATIVE_SCENE_PUMP") ||
       env_on("TER_RXD_LOADSCENE_PUMP") || env_on("TER_RXD_LOADSCENE_PUMP_BG") ||
       env_on("TER_RXD_LOADSCENE_PUMP_INTEG") || env_on("TER_RXD_LOADSCENE_PUMP_FINAL"))) {
    g_rxd_manual_scene_op = ret;
    g_rxd_manual_scene_frame = g_render_frame;
    g_rxd_manual_scene_final_op = NULL;
    g_rxd_manual_scene_final_done = 0;
    fprintf(stderr, "[RXD_SCENEOP] native async adotado para pump op=%p f=%d\n",
            ret, g_render_frame);
  }
  if (ret) rxd_log_asyncop("[RXD_USCENE] async", ret);
  fsync(2);
  return ret;
}
static void (*rxd_usm_loadscene_orig)(void *, int, void *);
static void rxd_usm_loadscene_hook(void *scene, int mode, void *method) {
  char s[128];
  fprintf(stderr, "[RXD_USCENE] LoadScene scene=%s mode=%d f=%d\n", rxd_str(scene, s, sizeof s), mode, g_render_frame);
  fsync(2);
  rxd_usm_loadscene_orig(scene, mode, method);
}
static void *(*rxd_usm_first_orig)(int, void *);
static void *rxd_usm_first_hook(int async, void *method) {
  int skip_from = env_int_default("TER_RXD_SKIP_FIRSTSCENE_FROM", 0);
  if (env_on("TER_RXD_SKIP_FIRSTSCENE") && g_render_frame >= skip_from) {
    fprintf(stderr, "[RXD_USCENE] LoadFirstScene_Internal async=%d SKIP f=%d from=%d\n",
            async, g_render_frame, skip_from);
    fsync(2);
    return NULL;
  }
  void *ret = rxd_usm_first_orig(async, method);
  fprintf(stderr, "[RXD_USCENE] LoadFirstScene_Internal async=%d -> %p f=%d\n", async, ret, g_render_frame);
  fsync(2);
  return ret;
}

typedef struct { int handle; } rxd_scene_value;

static const char *rxd_scene_name(rxd_scene_value sc, char *buf, size_t cap) {
  if (!g_il2cpp_base || !sc.handle) return "(null)";
  void *(*get_name)(int, void *) =
      (void *(*)(int, void *))(g_il2cpp_base + 0x18FA7C8);
  return rxd_str(get_name(sc.handle, NULL), buf, cap);
}

static void rxd_log_managed_scene(const char *tag, rxd_scene_value sc) {
  int valid = 0, loaded = 0, build = -1, roots = -1;
  char name[128];
  if (g_il2cpp_base && sc.handle) {
    int (*is_valid)(int, void *) =
        (int (*)(int, void *))(g_il2cpp_base + 0x18FA788);
    int (*is_loaded)(int, void *) =
        (int (*)(int, void *))(g_il2cpp_base + 0x18FA808);
    int (*get_build)(int, void *) =
        (int (*)(int, void *))(g_il2cpp_base + 0x18FA848);
    int (*get_roots)(int, void *) =
        (int (*)(int, void *))(g_il2cpp_base + 0x18FA888);
    valid = is_valid(sc.handle, NULL);
    if (valid) {
      loaded = is_loaded(sc.handle, NULL);
      build = get_build(sc.handle, NULL);
      roots = get_roots(sc.handle, NULL);
    }
  }
  fprintf(stderr,
          "[RXD_MSCENE] %s handle=%d valid=%d loaded=%d build=%d roots=%d name=%s f=%d\n",
          tag, sc.handle, valid, loaded, build, roots,
          rxd_scene_name(sc, name, sizeof name), g_render_frame);
  fsync(2);
}

static void rxd_set_active_scene_managed_tick(int f) {
  if (!g_il2cpp_base) return;
  int (*get_count)(void *) = (int (*)(void *))(g_il2cpp_base + 0x18FAE18);
  void (*get_active)(rxd_scene_value *, void *) =
      (void (*)(rxd_scene_value *, void *))(g_il2cpp_base + 0x18FAED4);
  void (*get_at)(int, rxd_scene_value *, void *) =
      (void (*)(int, rxd_scene_value *, void *))(g_il2cpp_base + 0x18FB15C);
  int (*set_active)(rxd_scene_value *, void *) =
      (int (*)(rxd_scene_value *, void *))(g_il2cpp_base + 0x18FAF9C);
  int count = get_count(NULL);
  int idx = env_int_default("TER_RXD_SET_ACTIVE_INDEX", -1);
  if (idx < 0) idx = count - 1;
  if (count <= 0 || idx < 0 || idx >= count) {
    static int bad_n;
    if (bad_n++ < 24 || (f % 60) == 0) {
      fprintf(stderr, "[RXD_MSCENE] setactive skip count=%d idx=%d f=%d\n",
              count, idx, f);
      fsync(2);
    }
    return;
  }
  rxd_scene_value active = {0}, target = {0};
  get_active(&active, NULL);
  get_at(idx, &target, NULL);
  int ret = set_active(&target, NULL);
  static int n;
  if (n++ < 48 || ret || (f % 60) == 0) {
    fprintf(stderr, "[RXD_MSCENE] SetActiveScene idx=%d/%d ret=%d f=%d\n",
            idx, count, ret, f);
    rxd_log_managed_scene("active-before", active);
    rxd_log_managed_scene("target", target);
    rxd_scene_value after = {0};
    get_active(&after, NULL);
    rxd_log_managed_scene("active-after", after);
  }
}

static int rxd_array_len(void *arr) {
  if (!arr) return -1;
  size_t len = *(size_t *)((char *)arr + 0x18);
  return len > 100000 ? -2 : (int)len;
}

static void *rxd_array_obj(void *arr, int idx) {
  if (!arr || idx < 0) return NULL;
  int len = rxd_array_len(arr);
  if (len < 0 || idx >= len) return NULL;
  return *(void **)((char *)arr + 0x20 + (size_t)idx * sizeof(void *));
}

static const char *rxd_obj_name(void *obj, char *buf, size_t cap) {
  if (!obj || !g_il2cpp_base || !rxd_unity_alive(obj)) return "(null)";
  void *(*get_name)(void *, void *) =
      (void *(*)(void *, void *))(g_il2cpp_base + 0x18E639C);
  return get_name ? rxd_str(get_name(obj, NULL), buf, cap) : "(noname)";
}

static void rxd_dump_component_array(const char *tag, const char *ns, const char *cn) {
  if (!g_il2cpp_base) return;
  void *ty = rxd_type_object_any(ns, cn);
  void *(*find_objects)(void *, int, void *) =
      (void *(*)(void *, int, void *))(g_il2cpp_base + 0x18E917C);
  void *arr = (ty && find_objects) ? find_objects(ty, 1, NULL) : NULL;
  int len = rxd_array_len(arr);
  fprintf(stderr, "[RXD_SCENEDUMP] %s type=%s.%s ty=%p arr=%p len=%d f=%d\n",
          tag, ns ? ns : "", cn ? cn : "", ty, arr, len, g_render_frame);
  int max = len > 12 ? 12 : len;
  for (int i = 0; i < max; i++) {
    void *comp = rxd_array_obj(arr, i);
    void *go = rxd_comp_go(comp);
    char n[128];
    fprintf(stderr,
            "[RXD_SCENEDUMP] %s[%d] comp=%p(%s) enabled=%d activeEnabled=%d go=%p name=%s self=%d hier=%d layer=%d cp=%p\n",
            tag, i, comp, il2cpp_classname(comp), rxd_beh_enabled(comp),
            rxd_beh_active_enabled(comp), go, rxd_obj_name(go, n, sizeof n),
            rxd_go_active_self(go), rxd_go_active_hier(go), rxd_go_layer(go),
            rxd_obj_cached(go));
  }
  fsync(2);
}

static void rxd_dump_scene_objects_tick(int f) {
  if (!g_il2cpp_base) return;
  void (*get_active)(rxd_scene_value *, void *) =
      (void (*)(rxd_scene_value *, void *))(g_il2cpp_base + 0x18FAED4);
  rxd_scene_value active = {0};
  get_active(&active, NULL);
  rxd_log_managed_scene("dump-active", active);
  void *roots = NULL;
  if (env_on("TER_RXD_SCENEDUMP_ROOTS_UNSAFE")) {
    void *(*get_roots_unsafe)(int, void *) =
        (void *(*)(int, void *))(g_il2cpp_base + 0x18FAA60);
    roots = (active.handle && get_roots_unsafe) ?
        get_roots_unsafe(active.handle, NULL) : NULL;
  }
  int len = rxd_array_len(roots);
  void *cam_ty = rxd_type_object_any("UnityEngine", "Camera");
  void *canvas_ty = rxd_type_object_any("UnityEngine", "Canvas");
  void *(*go_get_component)(void *, void *, void *) =
      (void *(*)(void *, void *, void *))(g_il2cpp_base + 0x1EC49FC);
  fprintf(stderr,
          "[RXD_SCENEDUMP] roots arr=%p len=%d unsafe=%d camTy=%p canvasTy=%p f=%d\n",
          roots, len, env_on("TER_RXD_SCENEDUMP_ROOTS_UNSAFE"), cam_ty, canvas_ty, f);
  int max = len > 16 ? 16 : len;
  for (int i = 0; i < max; i++) {
    void *go = rxd_array_obj(roots, i);
    char n[128];
    void *cam = (go && cam_ty && go_get_component) ?
        go_get_component(go, cam_ty, NULL) : NULL;
    void *canvas = (go && canvas_ty && go_get_component) ?
        go_get_component(go, canvas_ty, NULL) : NULL;
    fprintf(stderr,
            "[RXD_SCENEDUMP] root[%d]=%p(%s) name=%s self=%d hier=%d layer=%d cp=%p cam=%p en=%d canvas=%p en=%d mode=%d order=%d\n",
            i, go, il2cpp_classname(go), rxd_obj_name(go, n, sizeof n),
            rxd_go_active_self(go), rxd_go_active_hier(go), rxd_go_layer(go),
            rxd_obj_cached(go), cam, rxd_beh_enabled(cam), canvas,
            rxd_beh_enabled(canvas), rxd_canvas_mode(canvas), rxd_canvas_order(canvas));
  }
  rxd_dump_component_array("Camera", "UnityEngine", "Camera");
  rxd_dump_component_array("Canvas", "UnityEngine", "Canvas");
  fsync(2);
}

static void rxd_force_camera_render_tick(int f) {
  if (!g_il2cpp_base) return;
  void *ty = rxd_type_object_any("UnityEngine", "Camera");
  void *(*find_objects)(void *, int, void *) =
      (void *(*)(void *, int, void *))(g_il2cpp_base + 0x18E917C);
  void (*camera_render)(void *, void *) =
      (void (*)(void *, void *))(g_il2cpp_base + 0x1EC20FC);
  void *arr = (ty && find_objects) ? find_objects(ty, 1, NULL) : NULL;
  int len = rxd_array_len(arr);
  int rendered = 0;
  int render_disabled = env_on("TER_RXD_CAMERA_RENDER_DISABLED");
  int max = len > 8 ? 8 : len;
  for (int i = 0; i < max; i++) {
    void *cam = rxd_array_obj(arr, i);
    if (!cam || !camera_render) continue;
    int en = rxd_beh_enabled(cam);
    int aen = rxd_beh_active_enabled(cam);
    if (!render_disabled && (!en || !aen)) continue;
    void *go = rxd_comp_go(cam);
    char n[128];
    static int log_n;
    if (log_n++ < 48 || (f % 60) == 0) {
      fprintf(stderr,
              "[RXD_CAMRENDER] call cam[%d]=%p go=%p name=%s en=%d active=%d f=%d\n",
              i, cam, go, rxd_obj_name(go, n, sizeof n), en, aen, f);
      fsync(2);
    }
    camera_render(cam, NULL);
    rendered++;
  }
  static int n;
  if (n++ < 48 || (f % 60) == 0) {
    fprintf(stderr, "[RXD_CAMRENDER] done len=%d rendered=%d f=%d\n",
            len, rendered, f);
    fsync(2);
  }
}

static void (*rxd_title_awake_orig)(void *, void *);
static void (*rxd_title_disable_orig)(void *, void *);
static void (*rxd_title_sceneinit_orig)(void *, void *);
static void (*rxd_title_start_orig)(void *, void *);
static void (*rxd_title_update_orig)(void *, void *);
static void (*rxd_title_openui_orig)(void *, void *);
static void (*rxd_title_closeui_orig)(void *, void *);
static void *volatile g_rxd_title_inst;
static int g_rxd_title_awake_frame = -1;
static void rxd_title_awake_hook(void *self, void *method) {
  g_rxd_title_inst = self;
  g_rxd_title_awake_frame = g_render_frame;
  fprintf(stderr, "[RXD_TITLE] TitleSceneController.Awake self=%p f=%d\n", self, g_render_frame);
  fsync(2);
  rxd_title_awake_orig(self, method);
  fprintf(stderr, "[RXD_TITLE] TitleSceneController.Awake done self=%p f=%d\n", self, g_render_frame);
  fsync(2);
}
static void rxd_title_disable_hook(void *self, void *method) {
  fprintf(stderr, "[RXD_TITLE] TitleSceneController.OnDisable self=%p f=%d\n", self, g_render_frame);
  fsync(2);
  rxd_title_disable_orig(self, method);
}
static void rxd_title_sceneinit_hook(void *self, void *method) {
  fprintf(stderr, "[RXD_TITLE] TitleSceneController.SceneInit self=%p f=%d\n", self, g_render_frame);
  fsync(2);
  rxd_title_sceneinit_orig(self, method);
  fprintf(stderr, "[RXD_TITLE] TitleSceneController.SceneInit done self=%p f=%d\n", self, g_render_frame);
  fsync(2);
}
static void rxd_title_start_hook(void *self, void *method) {
  fprintf(stderr, "[RXD_TITLE] TitleSceneController.Start self=%p f=%d\n", self, g_render_frame);
  fsync(2);
  rxd_title_start_orig(self, method);
}
static void rxd_title_update_hook(void *self, void *method) {
  static int n;
  if (n++ < 16 || (g_render_frame % 120) == 0) {
    fprintf(stderr, "[RXD_TITLE] TitleSceneController.Update self=%p f=%d\n", self, g_render_frame);
    fsync(2);
  }
  rxd_title_update_orig(self, method);
}
static void rxd_title_openui_hook(void *self, void *method) {
  fprintf(stderr, "[RXD_TITLE] TitleSceneController.TitleOpenUI self=%p f=%d\n", self, g_render_frame);
  fsync(2);
  rxd_title_openui_orig(self, method);
  fprintf(stderr, "[RXD_TITLE] TitleSceneController.TitleOpenUI done self=%p f=%d\n", self, g_render_frame);
  fsync(2);
}
static void rxd_title_closeui_hook(void *self, void *method) {
  fprintf(stderr, "[RXD_TITLE] TitleSceneController.TitleCloseUI self=%p f=%d\n", self, g_render_frame);
  fsync(2);
  rxd_title_closeui_orig(self, method);
}

static void (*rxd_tnui_awake_orig)(void *, void *);
static void (*rxd_tnui_enable_orig)(void *, void *);
static void (*rxd_tnui_disable_orig)(void *, void *);
static void (*rxd_tnui_setup_orig)(void *, void *);
static void (*rxd_tnui_click_orig)(void *, void *);
static void rxd_tnui_awake_hook(void *self, void *method) {
  fprintf(stderr, "[RXD_TNUI] TitleNewUI.Awake self=%p btnStart=%p btnExit=%p textVersion=%p f=%d\n",
          self, self ? *(void **)((char *)self + 0xD8) : NULL,
          self ? *(void **)((char *)self + 0xE0) : NULL,
          self ? *(void **)((char *)self + 0xE8) : NULL, g_render_frame);
  fsync(2);
  rxd_tnui_awake_orig(self, method);
  fprintf(stderr, "[RXD_TNUI] TitleNewUI.Awake done self=%p f=%d\n", self, g_render_frame);
  fsync(2);
}
static void rxd_tnui_enable_hook(void *self, void *method) {
  fprintf(stderr, "[RXD_TNUI] TitleNewUI.OnEnable self=%p loading=%d f=%d\n",
          self, self ? *(unsigned char *)((char *)self + 0xF0) : -1, g_render_frame);
  fsync(2);
  rxd_tnui_enable_orig(self, method);
}
static void rxd_tnui_disable_hook(void *self, void *method) {
  fprintf(stderr, "[RXD_TNUI] TitleNewUI.OnDisable self=%p f=%d\n", self, g_render_frame);
  fsync(2);
  rxd_tnui_disable_orig(self, method);
}
static void rxd_tnui_setup_hook(void *self, void *method) {
  fprintf(stderr, "[RXD_TNUI] TitleNewUI.Setup self=%p loading=%d f=%d\n",
          self, self ? *(unsigned char *)((char *)self + 0xF0) : -1, g_render_frame);
  fsync(2);
  rxd_tnui_setup_orig(self, method);
  fprintf(stderr, "[RXD_TNUI] TitleNewUI.Setup done self=%p loading=%d f=%d\n",
          self, self ? *(unsigned char *)((char *)self + 0xF0) : -1, g_render_frame);
  fsync(2);
}
static void rxd_tnui_click_hook(void *self, void *method) {
  fprintf(stderr, "[RXD_TNUI] TitleNewUI.OnClickLogin self=%p loading=%d f=%d\n",
          self, self ? *(unsigned char *)((char *)self + 0xF0) : -1, g_render_frame);
  fsync(2);
  rxd_tnui_click_orig(self, method);
}

static void (*rxd_title_scene_cb_orig)(void *, void *, void *);
static void rxd_title_scene_cb_hook(void *self, void *ui, void *method) {
  fprintf(stderr,
          "[RXD_TITLECB] SceneInit.b__6_0 self=%p ui=%p(%s) btnStart=%p btnExit=%p textVersion=%p loading=%d f=%d\n",
          self, ui, il2cpp_classname(ui),
          ui ? *(void **)((char *)ui + 0xD8) : NULL,
          ui ? *(void **)((char *)ui + 0xE0) : NULL,
          ui ? *(void **)((char *)ui + 0xE8) : NULL,
          ui ? *(unsigned char *)((char *)ui + 0xF0) : -1,
          g_render_frame);
  fsync(2);
  rxd_title_scene_cb_orig(self, ui, method);
  fprintf(stderr, "[RXD_TITLECB] SceneInit.b__6_0 done self=%p ui=%p f=%d\n",
          self, ui, g_render_frame);
  fsync(2);
}

static void *volatile g_rxd_splash_start_it;
static void *volatile g_rxd_splash_cri_it;
static void *volatile g_rxd_switcher_inst;
static void *volatile g_rxd_capcom_inst;
static int g_rxd_switcher_awake_frame = -1;
static int g_rxd_capcom_awake_frame = -1;
static uint32_t g_rxd_switcher_gch;
static uint32_t g_rxd_capcom_gch;

static void rxd_splash_log_obj(const char *tag, void *self) {
  void *go = rxd_comp_go(self);
  fprintf(stderr,
          "[RXD_SPLASH] %s self=%p(%s) go=%p act=%d/%d layer=%d f=%d\n",
          tag ? tag : "?", self, il2cpp_classname(self), go,
          rxd_go_active_self(go), rxd_go_active_hier(go), rxd_go_layer(go),
          g_render_frame);
  fsync(2);
}

static void (*rxd_splash_awake_orig)(void *, void *);
static void rxd_splash_awake_hook(void *self, void *method) {
  rxd_splash_log_obj("OrangeSplash.Awake pre", self);
  rxd_splash_awake_orig(self, method);
  const char *cn = il2cpp_classname(self);
  if (cn && strstr(cn, "OrangeSplashCapcomLogo")) {
    g_rxd_capcom_inst = self;
    g_rxd_capcom_awake_frame = g_render_frame;
    if (!g_rxd_capcom_gch) g_rxd_capcom_gch = rxd_gc_pin(self, "OrangeSplashCapcomLogo");
  }
  rxd_splash_log_obj("OrangeSplash.Awake post", self);
}

static void *(*rxd_splash_onstart_orig)(void *, void *);
static void *rxd_splash_onstart_hook(void *self, void *method) {
  rxd_splash_log_obj("OrangeSplash.OnStartSplash pre", self);
  void *ret = rxd_splash_onstart_orig(self, method);
  g_rxd_splash_start_it = ret;
  fprintf(stderr, "[RXD_SPLASH] OrangeSplash.OnStartSplash -> it=%p(%s) f=%d\n",
          ret, il2cpp_classname(ret), g_render_frame);
  fsync(2);
  return ret;
}

static void *(*rxd_splash_criinit_orig)(void *, void *);
static void *rxd_splash_criinit_hook(void *self, void *method) {
  rxd_splash_log_obj("OrangeSplash.OnCriWareInitializer pre", self);
  void *ret = rxd_splash_criinit_orig(self, method);
  g_rxd_splash_cri_it = ret;
  fprintf(stderr, "[RXD_SPLASH] OrangeSplash.OnCriWareInitializer -> it=%p(%s) f=%d\n",
          ret, il2cpp_classname(ret), g_render_frame);
  fsync(2);
  return ret;
}

static void (*rxd_splash_click_orig)(void *, void *);
static void rxd_splash_click_hook(void *self, void *method) {
  rxd_splash_log_obj("OrangeSplash.OnClickGoNextSplash pre", self);
  rxd_splash_click_orig(self, method);
  rxd_splash_log_obj("OrangeSplash.OnClickGoNextSplash post", self);
}

static void (*rxd_splash_b0_orig)(void *, void *);
static void rxd_splash_b0_hook(void *self, void *method) {
  rxd_splash_log_obj("OrangeSplash.<OnStartSplash>b__11_0 pre", self);
  rxd_splash_b0_orig(self, method);
  rxd_splash_log_obj("OrangeSplash.<OnStartSplash>b__11_0 post", self);
}

static void (*rxd_splash_b1_orig)(void *, void *);
static void rxd_splash_b1_hook(void *self, void *method) {
  rxd_splash_log_obj("OrangeSplash.<OnStartSplash>b__11_1 pre", self);
  rxd_splash_b1_orig(self, method);
  rxd_splash_log_obj("OrangeSplash.<OnStartSplash>b__11_1 post", self);
}

static void *(*rxd_switcher_onstart_orig)(void *, void *);
static void *rxd_switcher_onstart_hook(void *self, void *method) {
  rxd_splash_log_obj("OrangeSplashSwitcher.OnStartSplash pre", self);
  void *ret = rxd_switcher_onstart_orig(self, method);
  g_rxd_splash_start_it = ret;
  fprintf(stderr, "[RXD_SPLASH] OrangeSplashSwitcher.OnStartSplash -> it=%p(%s) f=%d\n",
          ret, il2cpp_classname(ret), g_render_frame);
  fsync(2);
  return ret;
}

static void (*rxd_switcher_awake_orig)(void *, void *);
static void rxd_switcher_awake_hook(void *self, void *method) {
  g_rxd_switcher_inst = self;
  g_rxd_switcher_awake_frame = g_render_frame;
  rxd_splash_log_obj("OrangeSplashSwitcher.Awake pre", self);
  rxd_switcher_awake_orig(self, method);
  if (!g_rxd_switcher_gch) g_rxd_switcher_gch = rxd_gc_pin(self, "OrangeSplashSwitcher");
  rxd_splash_log_obj("OrangeSplashSwitcher.Awake post", self);
}

static void (*rxd_switcher_start_orig)(void *, void *);
static void rxd_switcher_start_hook(void *self, void *method) {
  rxd_splash_log_obj("OrangeSplashSwitcher.Start pre", self);
  rxd_switcher_start_orig(self, method);
  rxd_splash_log_obj("OrangeSplashSwitcher.Start post", self);
}

static void *(*rxd_splash_active_orig)(void *, void *);
static void *rxd_splash_active_hook(void *self, void *method) {
  rxd_splash_log_obj("OrangeSplash.Active pre", self);
  void *ret = rxd_splash_active_orig(self, method);
  fprintf(stderr, "[RXD_SPLASH] OrangeSplash.Active -> it=%p(%s) f=%d\n",
          ret, il2cpp_classname(ret), g_render_frame);
  fsync(2);
  return ret;
}

static void *(*rxd_switcher_criinit_orig)(void *, void *);
static void *rxd_switcher_criinit_hook(void *self, void *method) {
  rxd_splash_log_obj("OrangeSplashSwitcher.OnCriWareInitializer pre", self);
  void *ret = rxd_switcher_criinit_orig(self, method);
  g_rxd_splash_cri_it = ret;
  fprintf(stderr, "[RXD_SPLASH] OrangeSplashSwitcher.OnCriWareInitializer -> it=%p(%s) f=%d\n",
          ret, il2cpp_classname(ret), g_render_frame);
  fsync(2);
  return ret;
}

static void (*rxd_switcher_click_orig)(void *, void *);
static void rxd_switcher_click_hook(void *self, void *method) {
  rxd_splash_log_obj("OrangeSplashSwitcher.OnClickGoNextSplash pre", self);
  rxd_switcher_click_orig(self, method);
  rxd_splash_log_obj("OrangeSplashSwitcher.OnClickGoNextSplash post", self);
}

static void *(*rxd_switcher_wait_orig)(void *, void *);
static void *rxd_switcher_wait_hook(void *self, void *method) {
  rxd_splash_log_obj("OrangeSplashSwitcher.WaitForWhile pre", self);
  void *ret = rxd_switcher_wait_orig(self, method);
  fprintf(stderr, "[RXD_SPLASH] OrangeSplashSwitcher.WaitForWhile -> it=%p(%s) f=%d\n",
          ret, il2cpp_classname(ret), g_render_frame);
  fsync(2);
  return ret;
}

static void (*rxd_switcher_playaudio_orig)(void *, void *, void *);
static void rxd_switcher_playaudio_hook(void *self, void *name, void *method) {
  char buf[128];
  rxd_splash_log_obj("OrangeSplashSwitcher.PlayAudio pre", self);
  fprintf(stderr, "[RXD_SPLASH] OrangeSplashSwitcher.PlayAudio name=%s f=%d\n",
          rxd_str(name, buf, sizeof buf), g_render_frame);
  fsync(2);
  rxd_switcher_playaudio_orig(self, name, method);
  rxd_splash_log_obj("OrangeSplashSwitcher.PlayAudio post", self);
}

static void (*rxd_switcher_b0_orig)(void *, void *, void *);
static void rxd_switcher_b0_hook(void *self, void *arg, void *method) {
  fprintf(stderr, "[RXD_SPLASH] OrangeSplashSwitcher.<OnStartSplash>b__11_0 self=%p(%s) arg=%p(%s) f=%d\n",
          self, il2cpp_classname(self), arg, il2cpp_classname(arg), g_render_frame);
  fsync(2);
  rxd_switcher_b0_orig(self, arg, method);
}

static void (*rxd_switcher_b1_orig)(void *, void *);
static void rxd_switcher_b1_hook(void *self, void *method) {
  rxd_splash_log_obj("OrangeSplashSwitcher.<OnStartSplash>b__11_1 pre", self);
  rxd_switcher_b1_orig(self, method);
  rxd_splash_log_obj("OrangeSplashSwitcher.<OnStartSplash>b__11_1 post", self);
}

static void (*rxd_capcom_update_orig)(void *, void *);
static void rxd_capcom_update_hook(void *self, void *method) {
  g_rxd_capcom_inst = self;
  static unsigned n;
  if (n++ < 18 || (g_render_frame % 60) == 0)
    rxd_splash_log_obj("OrangeSplashCapcomLogo.Update", self);
  rxd_capcom_update_orig(self, method);
}

static long (*rxd_splash_start_cr_orig)(void *, void *);
static long rxd_splash_start_cr_hook(void *it, void *method) {
  int st = it ? *(int *)((char *)it + 0x10) : -999;
  void *cur = it ? *(void **)((char *)it + 0x18) : NULL;
  void *self = it ? *(void **)((char *)it + 0x20) : NULL;
  static int last = -9999;
  if (st != last || env_on("TER_RXD_SPLASHCR_VERBOSE")) {
    fprintf(stderr, "[RXD_SPLASHCR] OnStartSplash pre it=%p state=%d cur=%p(%s) self=%p(%s) f=%d\n",
            it, st, cur, il2cpp_classname(cur), self, il2cpp_classname(self),
            g_render_frame);
    fsync(2);
    last = st;
  }
  long ret = rxd_splash_start_cr_orig(it, method);
  int st2 = it ? *(int *)((char *)it + 0x10) : -999;
  void *cur2 = it ? *(void **)((char *)it + 0x18) : NULL;
  if (st2 != st || !ret || env_on("TER_RXD_SPLASHCR_VERBOSE")) {
    fprintf(stderr, "[RXD_SPLASHCR] OnStartSplash post it=%p state=%d->%d ret=%ld cur=%p(%s) f=%d\n",
            it, st, st2, ret, cur2, il2cpp_classname(cur2), g_render_frame);
    fsync(2);
    last = st2;
  }
  return ret;
}

static long (*rxd_splash_cri_cr_orig)(void *, void *);
static long rxd_splash_cri_cr_hook(void *it, void *method) {
  int st = it ? *(int *)((char *)it + 0x10) : -999;
  void *cur = it ? *(void **)((char *)it + 0x18) : NULL;
  void *self = it ? *(void **)((char *)it + 0x20) : NULL;
  static int last = -9999;
  if (st != last || env_on("TER_RXD_SPLASHCR_VERBOSE")) {
    fprintf(stderr, "[RXD_SPLASHCR] OnCriWareInitializer pre it=%p state=%d cur=%p(%s) self=%p(%s) f=%d\n",
            it, st, cur, il2cpp_classname(cur), self, il2cpp_classname(self),
            g_render_frame);
    fsync(2);
    last = st;
  }
  long ret = rxd_splash_cri_cr_orig(it, method);
  int st2 = it ? *(int *)((char *)it + 0x10) : -999;
  void *cur2 = it ? *(void **)((char *)it + 0x18) : NULL;
  if (st2 != st || !ret || env_on("TER_RXD_SPLASHCR_VERBOSE")) {
    fprintf(stderr, "[RXD_SPLASHCR] OnCriWareInitializer post it=%p state=%d->%d ret=%ld cur=%p(%s) f=%d\n",
            it, st, st2, ret, cur2, il2cpp_classname(cur2), g_render_frame);
    fsync(2);
    last = st2;
  }
  return ret;
}

static void rxd_splashdrive_tick(int f, int from, int period) {
  if (!env_on("TER_RXD_SPLASHDRIVE") || f < from) return;
  if (period <= 0) period = 1;
  void *sw = (void *)g_rxd_switcher_inst;
  static int wait_n, start_done, onstart_done, start_cr_done, cri_cr_done;
  static int update_n, click_done;
  if (!sw || !rxd_unity_alive(sw)) {
    if (wait_n++ < 24 || (f % 60) == 0) {
      fprintf(stderr, "[RXD_SPLASHDRIVE] aguardando switcher sw=%p alive=%d f=%d\n",
              sw, rxd_unity_alive(sw), f);
      fsync(2);
    }
    return;
  }
  if (!start_done && !env_on("TER_RXD_SPLASHDRIVE_NO_START") && rxd_switcher_start_orig) {
    fprintf(stderr, "[RXD_SPLASHDRIVE] Start sw=%p age=%d f=%d\n",
            sw, g_rxd_switcher_awake_frame >= 0 ? f - g_rxd_switcher_awake_frame : -1, f);
    fsync(2);
    rxd_switcher_start_orig(sw, NULL);
    fprintf(stderr, "[RXD_SPLASHDRIVE] Start done sw=%p it=%p f=%d\n",
            sw, (void *)g_rxd_splash_start_it, f);
    fsync(2);
    start_done = 1;
  }
  if (!g_rxd_splash_start_it && !onstart_done &&
      !env_on("TER_RXD_SPLASHDRIVE_NO_ONSTART") && rxd_switcher_onstart_orig) {
    fprintf(stderr, "[RXD_SPLASHDRIVE] OnStartSplash direto sw=%p f=%d\n", sw, f);
    fsync(2);
    void *it = rxd_switcher_onstart_orig(sw, NULL);
    g_rxd_splash_start_it = it;
    fprintf(stderr, "[RXD_SPLASHDRIVE] OnStartSplash -> it=%p(%s) f=%d\n",
            it, il2cpp_classname(it), f);
    fsync(2);
    onstart_done = 1;
  }
  if (env_on("TER_RXD_SPLASHDRIVE_UPDATE") && rxd_capcom_update_orig &&
      g_rxd_capcom_inst && ((f - from) % period) == 0) {
    if (update_n++ < 24 || (f % 60) == 0) {
      fprintf(stderr, "[RXD_SPLASHDRIVE] CapcomLogo.Update manual cap=%p age=%d f=%d\n",
              (void *)g_rxd_capcom_inst,
              g_rxd_capcom_awake_frame >= 0 ? f - g_rxd_capcom_awake_frame : -1, f);
      fsync(2);
    }
    rxd_capcom_update_orig((void *)g_rxd_capcom_inst, NULL);
  }
  if (g_rxd_splash_start_it && !start_cr_done && ((f - from) % period) == 0) {
    long r = rxd_runtime_movenext((void *)g_rxd_splash_start_it, "Splash.OnStartSplash");
    fprintf(stderr, "[RXD_SPLASHDRIVE] OnStartSplash MoveNext ret=%ld it=%p f=%d\n",
            r, (void *)g_rxd_splash_start_it, f);
    fsync(2);
    if (!r) start_cr_done = 1;
  }
  if (g_rxd_splash_cri_it && !cri_cr_done && ((f - from) % period) == 0) {
    long r = rxd_runtime_movenext((void *)g_rxd_splash_cri_it, "Splash.OnCriWareInitializer");
    fprintf(stderr, "[RXD_SPLASHDRIVE] OnCriWareInitializer MoveNext ret=%ld it=%p f=%d\n",
            r, (void *)g_rxd_splash_cri_it, f);
    fsync(2);
    if (!r) cri_cr_done = 1;
  }
  int click_from = env_int_default("TER_RXD_SPLASHDRIVE_CLICK_FROM", -1);
  if (!click_done && click_from >= 0 && f >= click_from && rxd_switcher_click_orig) {
    fprintf(stderr, "[RXD_SPLASHDRIVE] OnClickGoNextSplash sw=%p f=%d\n", sw, f);
    fsync(2);
    rxd_switcher_click_orig(sw, NULL);
    click_done = 1;
  }
}

static void rxd_splash_enum_class(const char *ns, const char *cn) {
  void *cls = rxd_class_scan_any(ns, cn);
  void *(*cls_methods)(void *, void **) =
      (void *(*)(void *, void **))ter_il2cpp_sym_cached("il2cpp_class_get_methods");
  const char *(*meth_name)(void *) =
      (const char *(*)(void *))ter_il2cpp_sym_cached("il2cpp_method_get_name");
  unsigned (*meth_pcount)(void *) =
      (unsigned (*)(void *))ter_il2cpp_sym_cached("il2cpp_method_get_param_count");
  void *(*meth_ret)(void *) =
      (void *(*)(void *))ter_il2cpp_sym_cached("il2cpp_method_get_return_type");
  char *(*type_name)(void *) =
      (char *(*)(void *))ter_il2cpp_sym_cached("il2cpp_type_get_name");
  fprintf(stderr, "[RXD_SPLASHSPY] enum class %s.%s cls=%p\n",
          ns && ns[0] ? ns : "(empty)", cn, cls);
  if (!cls || !cls_methods || !meth_name || !meth_pcount) {
    fsync(2);
    return;
  }
  void *it = NULL;
  while (1) {
    void *m = cls_methods(cls, &it);
    if (!m) break;
    const char *mn = meth_name(m);
    if (!mn) continue;
    void *target = *(void **)m;
    uintptr_t rva = (target && g_il2cpp_base &&
                     (uintptr_t)target >= g_il2cpp_base) ?
        (uintptr_t)target - g_il2cpp_base : 0;
    char *rt = (meth_ret && type_name) ? type_name(meth_ret(m)) : NULL;
    fprintf(stderr, "[RXD_SPLASHSPY]   %s argc=%u ret=%s method=%p target=%p rva=0x%lx\n",
            mn, meth_pcount(m), rt ? rt : "?", m, target, (unsigned long)rva);
  }
  fsync(2);
}

static void rxd_splashspy_install_once(void) {
  static int done;
  if (done || (!env_on("TER_RXD_SPLASHSPY") && !env_on("TER_RXD_SPLASHDRIVE")) ||
      !g_il2cpp_base) return;
  void *probe = rxd_class_scan_any("", "OrangeSplash");
  if (!probe) {
    static unsigned waits;
    if (waits++ < 12 || (waits % 60) == 0) {
      fprintf(stderr, "[RXD_SPLASHSPY] OrangeSplash ainda indisponivel f=%d try=%u\n",
              g_render_frame, waits);
      fsync(2);
    }
    return;
  }
  done = 1;
  rxd_splash_enum_class("", "OrangeSplash");
  rxd_splash_enum_class("", "OrangeSplashCapcomLogo");
  rxd_splash_enum_class("", "OrangeSplashSwitcher");
  rxd_splash_enum_class("", "<OnStartSplash>d__11");
  rxd_splash_enum_class("", "<OnCriWareInitializer>d__13");
  if (env_on("TER_RXD_SPLASHSPY_ENUM_ONLY")) {
    fprintf(stderr, "[RXD_SPLASHSPY] enum-only: hooks pulados f=%d\n", g_render_frame);
    fsync(2);
    return;
  }
  int ok = 0, total = 0;
#define RXD_HOOK_SPL(ns, cn, mn, argc, hook, orig, label) do { \
    total++; ok += rxd_hook_method_any((ns), (cn), (mn), (argc), \
                                       (void *)(hook), (void **)&(orig), (label)); \
  } while (0)
  RXD_HOOK_SPL("", "OrangeSplash", "Awake", 0,
               rxd_splash_awake_hook, rxd_splash_awake_orig,
               "OrangeSplash.Awake");
  RXD_HOOK_SPL("", "OrangeSplash", "Active", 0,
               rxd_splash_active_hook, rxd_splash_active_orig,
               "OrangeSplash.Active");
  RXD_HOOK_SPL("", "OrangeSplash", "OnStartSplash", 0,
               rxd_splash_onstart_hook, rxd_splash_onstart_orig,
               "OrangeSplash.OnStartSplash");
  RXD_HOOK_SPL("", "OrangeSplash", "OnCriWareInitializer", 0,
               rxd_splash_criinit_hook, rxd_splash_criinit_orig,
               "OrangeSplash.OnCriWareInitializer");
  RXD_HOOK_SPL("", "OrangeSplash", "OnClickGoNextSplash", 0,
               rxd_splash_click_hook, rxd_splash_click_orig,
               "OrangeSplash.OnClickGoNextSplash");
  RXD_HOOK_SPL("", "OrangeSplash", "<OnStartSplash>b__11_0", 0,
               rxd_splash_b0_hook, rxd_splash_b0_orig,
               "OrangeSplash.<OnStartSplash>b__11_0");
  RXD_HOOK_SPL("", "OrangeSplash", "<OnStartSplash>b__11_1", 0,
               rxd_splash_b1_hook, rxd_splash_b1_orig,
               "OrangeSplash.<OnStartSplash>b__11_1");
  RXD_HOOK_SPL("", "OrangeSplashCapcomLogo", "Update", 0,
               rxd_capcom_update_hook, rxd_capcom_update_orig,
               "OrangeSplashCapcomLogo.Update");
  RXD_HOOK_SPL("", "OrangeSplashSwitcher", "Awake", 0,
               rxd_switcher_awake_hook, rxd_switcher_awake_orig,
               "OrangeSplashSwitcher.Awake");
  RXD_HOOK_SPL("", "OrangeSplashSwitcher", "Start", 0,
               rxd_switcher_start_hook, rxd_switcher_start_orig,
               "OrangeSplashSwitcher.Start");
  RXD_HOOK_SPL("", "OrangeSplashSwitcher", "OnStartSplash", 0,
               rxd_switcher_onstart_hook, rxd_switcher_onstart_orig,
               "OrangeSplashSwitcher.OnStartSplash");
  RXD_HOOK_SPL("", "OrangeSplashSwitcher", "OnCriWareInitializer", 0,
               rxd_switcher_criinit_hook, rxd_switcher_criinit_orig,
               "OrangeSplashSwitcher.OnCriWareInitializer");
  RXD_HOOK_SPL("", "OrangeSplashSwitcher", "OnClickGoNextSplash", 0,
               rxd_switcher_click_hook, rxd_switcher_click_orig,
               "OrangeSplashSwitcher.OnClickGoNextSplash");
  RXD_HOOK_SPL("", "OrangeSplashSwitcher", "WaitForWhile", 0,
               rxd_switcher_wait_hook, rxd_switcher_wait_orig,
               "OrangeSplashSwitcher.WaitForWhile");
  RXD_HOOK_SPL("", "OrangeSplashSwitcher", "PlayAudio", 1,
               rxd_switcher_playaudio_hook, rxd_switcher_playaudio_orig,
               "OrangeSplashSwitcher.PlayAudio");
  RXD_HOOK_SPL("", "OrangeSplashSwitcher", "<OnStartSplash>b__11_0", 1,
               rxd_switcher_b0_hook, rxd_switcher_b0_orig,
               "OrangeSplashSwitcher.<OnStartSplash>b__11_0");
  RXD_HOOK_SPL("", "OrangeSplashSwitcher", "<OnStartSplash>b__11_1", 0,
               rxd_switcher_b1_hook, rxd_switcher_b1_orig,
               "OrangeSplashSwitcher.<OnStartSplash>b__11_1");
  RXD_HOOK_SPL("", "<OnStartSplash>d__11", "MoveNext", 0,
               rxd_splash_start_cr_hook, rxd_splash_start_cr_orig,
               "OrangeSplash.<OnStartSplash>.MoveNext");
  RXD_HOOK_SPL("", "<OnCriWareInitializer>d__13", "MoveNext", 0,
               rxd_splash_cri_cr_hook, rxd_splash_cri_cr_orig,
               "OrangeSplash.<OnCriWareInitializer>.MoveNext");
  RXD_HOOK_SPL("UnityEngine.SceneManagement", "SceneManager", "LoadFirstScene_Internal", 1,
               rxd_usm_first_hook, rxd_usm_first_orig,
               "UnityEngine.SceneManager.LoadFirstScene_Internal");
#undef RXD_HOOK_SPL
  fprintf(stderr, "[RXD_SPLASHSPY] hooks instalados %d/%d\n", ok, total);
  fsync(2);
}

static void rxd_install_abspy(uintptr_t base) {
  struct { uintptr_t rva; void *hook; void **orig; const char *nm; } T[] = {
    {0x10ED11C, (void *)rxd_abm_awake_hook,      (void **)&rxd_abm_awake_orig,      "RXD.AssetsBundleManager.Awake"},
    {0x10EDE80, (void *)rxd_abm_init_hook,       (void **)&rxd_abm_init_orig,       "RXD.AssetsBundleManager.Init"},
    {0x10ECE64, (void *)rxd_abm_getpath_hook,    (void **)&rxd_abm_getpath_orig,    "RXD.AssetsBundleManager.GetPath"},
    {0x10ED0A0, (void *)rxd_abm_getbundleid_hook,(void **)&rxd_abm_getbundleid_orig,"RXD.AssetsBundleManager.GetBundleID"},
    {0x10EDEB0, (void *)rxd_abm_olm_hook,        (void **)&rxd_abm_olm_orig,        "RXD.AssetsBundleManager.OnStartLoadManifest"},
    {0x10EDF28, (void *)rxd_abm_loadassets_hook, (void **)&rxd_abm_loadassets_orig, "RXD.AssetsBundleManager.LoadAssets"},
    {0x10EE064, (void *)rxd_abm_ola_hook,        (void **)&rxd_abm_ola_orig,        "RXD.AssetsBundleManager.OnStartLoadAssets"},
    {0x10EE128, (void *)rxd_abm_olsa_hook,       (void **)&rxd_abm_olsa_orig,       "RXD.AssetsBundleManager.OnStartLoadSingleAsset"},
    {0x16A4AF8, (void *)rxd_abm_gaal_hook,       (void **)&rxd_abm_gaal_orig,       "RXD.AssetsBundleManager.GetAssetAndAsyncLoad<T>"},
    {0x16A4A28, (void *)rxd_abm_asyncasset_hook, (void **)&rxd_abm_asyncasset_orig, "RXD.AssetsBundleManager.AsyncLoadAsset<T>"},
    {0x1143D94, (void *)rxd_cr_manifest_hook,    (void **)&rxd_cr_manifest_orig,    "RXD.<OnStartLoadManifest>.MoveNext"},
    {0x11438E0, (void *)rxd_cr_assets_hook,      (void **)&rxd_cr_assets_orig,      "RXD.<OnStartLoadAssets>.MoveNext"},
    {0x1143450, (void *)rxd_abm_olsa_cb_hook,    (void **)&rxd_abm_olsa_cb_orig,    "RXD.<OnStartLoadSingleAsset>.b__0"},
    {0x1144410, (void *)rxd_cr_single_hook,      (void **)&rxd_cr_single_orig,      "RXD.<OnStartLoadSingleAsset>.MoveNext"},
    {0x2E2BEEC, (void *)rxd_ab_lffa_hook,        (void **)&rxd_ab_lffa_orig,        "RXD.AssetBundle.LoadFromFileAsync_Internal"},
    {0x2E2BF44, (void *)rxd_ab_lffa_pub_hook,    (void **)&rxd_ab_lffa_pub_orig,    "RXD.AssetBundle.LoadFromFileAsync"},
    {0x2E2BF8C, (void *)rxd_ab_lffa_pub3_hook,   (void **)&rxd_ab_lffa_pub3_orig,   "RXD.AssetBundle.LoadFromFileAsync3"},
    {0x2E2BFE4, (void *)rxd_ab_lff_hook,         (void **)&rxd_ab_lff_orig,         "RXD.AssetBundle.LoadFromFile_Internal"},
    {0x2E2C084, (void *)rxd_ab_lfma_hook,        (void **)&rxd_ab_lfma_orig,        "RXD.AssetBundle.LoadFromMemoryAsync_Internal"},
    {0x2E2C0D4, (void *)rxd_ab_lfma_pub_hook,    (void **)&rxd_ab_lfma_pub_orig,    "RXD.AssetBundle.LoadFromMemoryAsync"},
    {0x2E2C118, (void *)rxd_ab_lfm_hook,         (void **)&rxd_ab_lfm_orig,         "RXD.AssetBundle.LoadFromMemory_Internal"},
    {0x1EBE464, (void *)rxd_async_set_allow_hook,(void **)&rxd_async_set_allow_orig,"RXD.AsyncOperation.set_allowSceneActivation"},
    {0x1EBE394, (void *)rxd_async_done_hook,     (void **)&rxd_async_done_orig,     "RXD.AsyncOperation.get_isDone"},
    {0x1EBE3D4, (void *)rxd_async_progress_hook, (void **)&rxd_async_progress_orig, "RXD.AsyncOperation.get_progress"},
    {0x2E2C5CC, (void *)rxd_ab_getbundle_hook,   (void **)&rxd_ab_getbundle_orig,   "RXD.AssetBundleCreateRequest.get_assetBundle"},
    {0x2E2C4E4, (void *)rxd_ab_laa_hook,         (void **)&rxd_ab_laa_orig,         "RXD.AssetBundle.LoadAssetAsync_Internal"},
    {0x16A494C, (void *)rxd_ab_loadasset_hook,   (void **)&rxd_ab_loadasset_orig,   "RXD.AssetBundle.LoadAsset<T>"},
    {0x0CF9E30, (void *)rxd_boot_start_hook,     (void **)&rxd_boot_start_orig,     "RXD.OrangeBootup.Start"},
    {0x0CF9EA8, (void *)rxd_boot_setup_hook,     (void **)&rxd_boot_setup_orig,     "RXD.OrangeBootup.SetupEnvironment"},
    {0x0CFA530, (void *)rxd_boot_warm_hook,      (void **)&rxd_boot_warm_orig,      "RXD.OrangeBootup.WarmUpSystemNeed"},
    {0x0CFA604, (void *)rxd_boot_cb_hook,        (void **)&rxd_boot_cb_orig,        "RXD.OrangeBootup.Start_b__1_0"},
    {0x0DB57C8, (void *)rxd_boot_start_cr_hook,  (void **)&rxd_boot_start_cr_orig,  "RXD.OrangeBootup.<Start>.MoveNext"},
    {0x0DB6DF8, (void *)rxd_boot_warm_cr_hook,   (void **)&rxd_boot_warm_cr_orig,   "RXD.OrangeBootup.<WarmUp>.MoveNext"},
    {0x0DBA640, (void *)rxd_design_cr_hook,      (void **)&rxd_design_cr_orig,      "RXD.OrangeDataReader.<LoadDesignsData>.MoveNext"},
    {0x0DBA458, (void *)rxd_design_cb_hook,      (void **)&rxd_design_cb_orig,      "RXD.OrangeDataReader.<LoadDesignsData>b__0"},
    {0x1071410, (void *)rxd_owrl_load_hook,      (void **)&rxd_owrl_load_orig,      "RXD.OrangeWebRequestLoad.Load"},
    {0x0DBFE84, (void *)rxd_owrl_load_b1_hook,   (void **)&rxd_owrl_load_b1_orig,   "RXD.OrangeWebRequestLoad.<Load>b__1"},
    {0x0DBFEF0, (void *)rxd_owrl_load_b0_hook,   (void **)&rxd_owrl_load_b0_orig,   "RXD.OrangeWebRequestLoad.<Load>b__0"},
    {0x1336BBC, (void *)rxd_cpm_assetsload_hook, (void **)&rxd_cpm_assetsload_orig, "RXD.ConsolePackageManager.AssetsLoad"},
    {0x2649F8C, (void *)rxd_mbs_get_instance_hook, (void **)&rxd_mbs_get_instance_orig, "RXD.MonoBehaviourSingleton<T>.get_Instance"},
    {0x10B0B28, (void *)rxd_audio_init_hook,     (void **)&rxd_audio_init_orig,     "RXD.AudioManager.Init"},
    {0x10B10A4, (void *)rxd_audio_loadacf_hook,  (void **)&rxd_audio_loadacf_orig,  "RXD.AudioManager.LoadAcf"},
    {0x10B11D4, (void *)rxd_audio_preload_hook,  (void **)&rxd_audio_preload_orig,  "RXD.AudioManager.PreloadInitAudio"},
    {0x10B648C, (void *)rxd_audio_preload_cb_hook, (void **)&rxd_audio_preload_cb_orig, "RXD.AudioManager.<PreloadInitAudio>b__68_0"},
    {0x1065568, (void *)rxd_scene_awake_hook,    (void **)&rxd_scene_awake_orig,    "RXD.OrangeSceneManager.Awake"},
    {0x10655C8, (void *)rxd_scene_change_hook,   (void **)&rxd_scene_change_orig,   "RXD.OrangeSceneManager.ChangeScene"},
    {0x1065EC4, (void *)rxd_scene_onchange_hook, (void **)&rxd_scene_onchange_orig, "RXD.OrangeSceneManager.OnStartChangeScene"},
    {0x1065F3C, (void *)rxd_scene_complete_hook, (void **)&rxd_scene_complete_orig, "RXD.OrangeSceneManager.ChangeSceneComplete"},
    {0x10661B0, (void *)rxd_scene_additive_hook, (void **)&rxd_scene_additive_orig, "RXD.OrangeSceneManager.AdditiveScene"},
    {0x18FB290, (void *)rxd_usm_loadasync_hook,  (void **)&rxd_usm_loadasync_orig,  "RXD.SceneManager.LoadSceneAsyncNameIndexInternal"},
    {0x18FB888, (void *)rxd_usm_loadscene_hook,  (void **)&rxd_usm_loadscene_orig,  "RXD.SceneManager.LoadScene"},
    {0x1435B04, (void *)rxd_uim_loadui_b0_hook,  (void **)&rxd_uim_loadui_b0_orig,  "RXD.UIManager.LoadUI.b__0"},
    {0x0FCB7B4, (void *)rxd_title_awake_hook,     (void **)&rxd_title_awake_orig,    "RXD.TitleSceneController.Awake"},
    {0x0FCB904, (void *)rxd_title_disable_hook,   (void **)&rxd_title_disable_orig,  "RXD.TitleSceneController.OnDisable"},
    {0x0FCBA54, (void *)rxd_title_sceneinit_hook, (void **)&rxd_title_sceneinit_orig,"RXD.TitleSceneController.SceneInit"},
    {0x0FCBC48, (void *)rxd_title_start_hook,     (void **)&rxd_title_start_orig,    "RXD.TitleSceneController.Start"},
    {0x0FCBC74, (void *)rxd_title_update_hook,    (void **)&rxd_title_update_orig,   "RXD.TitleSceneController.Update"},
    {0x0FCBE28, (void *)rxd_title_openui_hook,    (void **)&rxd_title_openui_orig,   "RXD.TitleSceneController.TitleOpenUI"},
    {0x0FCBE30, (void *)rxd_title_closeui_hook,   (void **)&rxd_title_closeui_orig,  "RXD.TitleSceneController.TitleCloseUI"},
    {0x0FCA8C8, (void *)rxd_tnui_awake_hook,      (void **)&rxd_tnui_awake_orig,     "RXD.TitleNewUI.Awake"},
    {0x0FCA8F4, (void *)rxd_tnui_enable_hook,     (void **)&rxd_tnui_enable_orig,    "RXD.TitleNewUI.OnEnable"},
    {0x0FCA8F8, (void *)rxd_tnui_disable_hook,    (void **)&rxd_tnui_disable_orig,   "RXD.TitleNewUI.OnDisable"},
    {0x0FCA8FC, (void *)rxd_tnui_setup_hook,      (void **)&rxd_tnui_setup_orig,     "RXD.TitleNewUI.Setup"},
    {0x0FCA9E0, (void *)rxd_tnui_click_hook,      (void **)&rxd_tnui_click_orig,     "RXD.TitleNewUI.OnClickLogin"},
    {0x0E7A5B0, (void *)rxd_title_scene_cb_hook,  (void **)&rxd_title_scene_cb_orig, "RXD.TitleScene.SceneInit_b__6_0"},
  };
  unsigned ok = 0;
  for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
    void *tr = mk_tramp(base + T[i].rva, T[i].nm);
    if (!tr) { fprintf(stderr, "[RXD_ABSPY] tramp falhou: %s\n", T[i].nm); continue; }
    *T[i].orig = tr;
    hook_arm64(base + T[i].rva, (uintptr_t)T[i].hook);
    ok++;
  }
  fprintf(stderr, "[RXD_ABSPY] %u/%u hooks instalados\n", ok, (unsigned)(sizeof T / sizeof T[0]));
  fsync(2);
}

/* ===== CUP_TAPINPUT: pulsa AnyPlayerInput.GetAnyButtonDown (il2cpp 0xCC2854) =====
 * A coroutine WaitForUserInputBeforeContinue do disclaimer espera
 * WaitUntil(() => AnyPlayerInput.GetAnyButtonDown()). Sem plumbing real de input,
 * fica preso. Hookamos o método: retorna TRUE em janelas periódicas (~3 frames a
 * cada CUP_TAPPERIOD frames) — equivale a um "toque" que destrava o disclaimer e
 * confirma menus, mas sem ficar true p/ sempre (evita auto-navegar descontrolado).
 * CUP_TAPSTART=frame inicial (default 200, dá tempo do disclaimer subir). */
static int g_tap_period = 240, g_tap_start = 200, g_tap_width = 3;
uintptr_t g_tapinput_cont = 0;
__asm__(
  ".text\n"
  ".global tapinput_tramp\n"
  "tapinput_tramp:\n"
  "  stp x28, x27, [sp, #-96]!\n"    /* 0xCC2854 */
  "  stp x26, x25, [sp, #16]\n"      /* 0xCC2858 */
  "  stp x24, x23, [sp, #32]\n"      /* 0xCC285C */
  "  stp x22, x21, [sp, #48]\n"      /* 0xCC2860 */
  "  adrp x17, g_tapinput_cont\n"
  "  add x17, x17, :lo12:g_tapinput_cont\n"
  "  ldr x17, [x17]\n"
  "  br x17\n"
);
extern int tapinput_tramp(void *self);
int my_getanybuttondown(void *self);
int my_getanybuttondown(void *self) {
  int f = g_render_frame;
  if (f >= g_tap_start) {
    int ph = (f - g_tap_start) % g_tap_period;
    if (ph < g_tap_width) {
      static int lastf = -1;
      if (f != lastf) { fprintf(stderr, "[TAPINPUT] pulse f=%d\n", f); fsync(2); lastf = f; }
      return 1;
    }
  }
  return tapinput_tramp(self);
}

/* ===== CUP_SAPATH: override Application.get_streamingAssetsPath (il2cpp 0x17C7C1C) =====
 * No so-loader o getter retorna "jar:file://!" (caminho do APK vazio) -> os
 * AssetBundles do título (AssetBundle.LoadFromFile(streamingAssetsPath+"/AssetBundles/"+n))
 * falham com "Unable to open archive file" -> NullReferenceException mata a coroutine
 * de boot. Apontamos p/ um diretório REAL do filesystem (CUP_SAPATH=/storage/cuphead-sa)
 * onde deployamos os bundles -> LoadFromFile abre o arquivo de verdade.
 * Cria a string il2cpp 1× via il2cpp_string_new (não chama o original). */
static void *g_sa_string = NULL;
void *my_streamingAssetsPath(void);
void *my_streamingAssetsPath(void) {
  if (!g_sa_string && g_il2cpp_base) {
    void *(*isn)(const char *) = (void *(*)(const char *))(g_il2cpp_base + 0x1b62c38); /* il2cpp_string_new */
    const char *p = getenv("CUP_SAPATH"); if (!p) p = "/storage/cuphead-sa";
    g_sa_string = isn(p);
    fprintf(stderr, "[SAPATH] streamingAssetsPath -> \"%s\" (il2cpp str=%p)\n", p, g_sa_string);
    fsync(2);
  }
  return g_sa_string;
}
/* AssetBundleLoader.getBasePath(location) (il2cpp 0x1031C8C): p/ StreamingAssets usa
 * streamingAssetsPath (já overridado), mas p/ DLC (location=1) usa OUTRA fonte que no
 * so-loader retorna string-lixo ("Шестигранные врата 1") → o load de DLC persistente
 * falha. Override: retorna SEMPRE o nosso path real (ignora location). */
void *my_getbasepath(int location);
void *my_getbasepath(int location) {
  (void)location;
  return my_streamingAssetsPath();  /* mesmo dir; loader anexa "/AssetBundles/"+nome */
}

/* RXD: o asset de config marca o caminho debug/local, mas no so-loader o campo
 * serializado chega vazio e AssetBundleScriptableObject.GetDebugLocalPath()
 * chama DirectoryInfo("") antes do manager montar os paths. Retornamos o
 * diretório real dos bundles externos extraídos do APK. */
static void *g_rxd_abpath_string = NULL;
void *rxd_get_debug_local_path(void *self);
void *rxd_get_debug_local_path(void *self) {
  (void)self;
  if (!g_rxd_abpath_string && g_il2cpp_base) {
    void *(*isn)(const char *) = (void *(*)(const char *))ter_il2cpp_sym_cached("il2cpp_string_new");
    const char *p = getenv("TER_RXD_ABPATH");
    if (!p || !*p) p = "/storage/roms/ports/rockmanxdive/assetpack";
    g_rxd_abpath_string = isn ? isn(p) : NULL;
    fprintf(stderr, "[RXD_ABPATH] GetDebugLocalPath -> \"%s\" (il2cpp str=%p)\n", p, g_rxd_abpath_string);
    fsync(2);
  }
  return g_rxd_abpath_string;
}

static char g_dl_sl; /* sentinela do handle de libOpenSLES (FMOD → opensles_shim) */

static long rxd_cri_stub_ret0(void) { return 0; }
static long rxd_cri_stub_ret1(void) { return 1; }
static long rxd_cri_stub_ret2(void) { return 2; }
static uintptr_t rxd_cri_stub_handle(void) {
  static uintptr_t h = 0x4352490000000000ull; /* "CRI" high bits; fake non-NULL handles */
  return ++h;
}
static void rxd_cri_stub_void(void) {}
static void rxd_cri_unity_set_graphics_device(void *dev, int type, int event) {
  (void)dev; (void)type; (void)event;
}
static void rxd_cri_unity_render_event(int event_id) { (void)event_id; }
static void rxd_cri_unity_plugin_load(void *interfaces) {
  (void)interfaces;
  static int logged;
  if (!logged++) fprintf(stderr, "[RXD_CRI_FAKE] UnityPluginLoad no-op f=%d\n", g_render_frame);
}
static void rxd_cri_unity_plugin_unload(void) {}
static void *rxd_cri_get_render_event_func(void) { return (void *)rxd_cri_unity_render_event; }
static void *rxd_cri_alloc_cb(void *obj, size_t size, size_t align) {
  (void)obj; (void)align;
  return calloc(1, size ? size : 1);
}
static void rxd_cri_dealloc_cb(void *obj, void *p) {
  (void)obj;
  free(p);
}
static void *rxd_cri_get_allocate_func(void) { return (void *)rxd_cri_alloc_cb; }
static void *rxd_cri_get_deallocate_func(void) { return (void *)rxd_cri_dealloc_cb; }
static void *rxd_cri_get_error_callback_func(void) { return NULL; }

static void *rxd_cri_fake_dlsym(void *h, const char *nm) {
  if (!env_on("TER_RXD_CRI_FAKE") || !nm) return NULL;
  int cri_handle = (h == &g_dl_cri) || (h == &g_dl_self) || h == RTLD_DEFAULT || h == RTLD_NEXT;
  int cri_sym = !strncmp(nm, "cri", 3) || !strncmp(nm, "CRIWARE", 7);
  int unity_sym = !strncmp(nm, "Unity", 5);
  if (!cri_handle || (!cri_sym && !unity_sym)) return NULL;

  void *p = NULL;
  if (!strcmp(nm, "UnitySetGraphicsDevice")) p = (void *)rxd_cri_unity_set_graphics_device;
  else if (!strcmp(nm, "UnityRenderEvent")) p = (void *)rxd_cri_unity_render_event;
  else if (!strcmp(nm, "UnityPluginLoad")) p = (void *)rxd_cri_unity_plugin_load;
  else if (!strcmp(nm, "UnityPluginUnload")) p = (void *)rxd_cri_unity_plugin_unload;
  else if (!strcmp(nm, "UnityGetAudioEffectDefinitions") ||
           !strcmp(nm, "UnityRenderingExtEvent") ||
           !strcmp(nm, "UnityRenderingExtQuery") ||
           !strcmp(nm, "UnityShaderCompilerExtEvent") ||
           !strcmp(nm, "UnitySetEventQueue")) p = NULL;
  else if (!strcmp(nm, "criWareUnity_GetRenderEventFunc")) p = (void *)rxd_cri_get_render_event_func;
  else if (!strcmp(nm, "criWareUnity_GetAllocateFunc")) p = (void *)rxd_cri_get_allocate_func;
  else if (!strcmp(nm, "criWareUnity_GetDeallocateFunc")) p = (void *)rxd_cri_get_deallocate_func;
  else if (!strcmp(nm, "criWareUnity_GetPluginErrorCallbackFunc")) p = (void *)rxd_cri_get_error_callback_func;
  else if (strstr(nm, "GetStatus") || strstr(nm, "WaitForCompletion")) p = (void *)rxd_cri_stub_ret2;
  else if (strstr(nm, "Create") || strstr(nm, "Load") || strstr(nm, "Allocate") ||
           strstr(nm, "Bind") || strstr(nm, "Attach") || strstr(nm, "RegisterAcf") ||
           strstr(nm, "Start")) p = (void *)rxd_cri_stub_handle;
  else if (strstr(nm, "Initialize") || strstr(nm, "Is") || strstr(nm, "Exists") ||
           strstr(nm, "Available") || strstr(nm, "Supported")) p = (void *)rxd_cri_stub_ret1;
  else if (cri_sym) p = (void *)rxd_cri_stub_ret0;

  static int logs;
  if (p || unity_sym || cri_sym) {
    if (logs < 96) {
      fprintf(stderr, "[RXD_CRI_FAKE] dlsym %s -> %p\n", nm, p);
      if (++logs == 96) fprintf(stderr, "[RXD_CRI_FAKE] dlsym log limit\n");
    }
    return p;
  }
  return NULL;
}

static void *my_dlopen(const char *nm, int flag) {
  if (g_dllog) fprintf(stderr, "[dlopen] \"%s\"\n", nm ? nm : "(null)");
  /* il2cpp: nosso modulo ja' carregado (F1). Casa "il2cpp" em qualquer forma. */
  if (nm && strstr(nm, "il2cpp")) { fprintf(stderr, "[DLOPEN] %s -> il2cpp module\n", nm); return &g_dl_il2cpp; }
  if (nm && strstr(nm, "cri_ware_unity")) {
    fprintf(stderr, "[DLOPEN] %s -> criware module (%s)\n", nm, g_m_cri ? "OK" : "missing");
    return g_m_cri ? &g_dl_cri : &g_dl_self;
  }
  if (nm && strstr(nm, "burst_generated")) {
    fprintf(stderr, "[DLOPEN] %s -> burst module (%s)\n", nm, g_m_burst ? "OK" : "missing");
    return g_m_burst ? &g_dl_burst : &g_dl_self;
  }
  if (nm && strstr(nm, "libmain")) {
    fprintf(stderr, "[DLOPEN] %s -> libmain module (%s)\n", nm, g_m_mainlib ? "OK" : "self");
    return g_m_mainlib ? &g_dl_mainlib : &g_dl_self;
  }
  /* FMOD (audio do Unity) faz dlopen("libOpenSLES.so") em runtime. CUP_NOSL=1
     desliga o shim (volta ao estado imagem-OK: FMOD cai no null output). */
  if (nm && strstr(nm, "OpenSLES") && !getenv("CUP_NOSL")) {
    fprintf(stderr, "[DLOPEN] %s -> opensles_shim\n", nm); return &g_dl_sl; }
  if (!nm || !nm[0] || strstr(nm, "libc") || strstr(nm, "libunity") || strstr(nm, "libmain"))
    return &g_dl_self;
  void *h = dlopen(nm, flag); return h ? h : &g_dl_self;
}
static void *my_dlsym(void *h, const char *nm) {
  if (!nm) return NULL;
  if (g_dllog) fprintf(stderr, "[dlsym] h=%p \"%s\"\n", h, nm);
  if (!strcmp(nm, "glGetString")) return (void *)my_glGetString;
  if ((g_drawspy || getenv("CUP_DRAWCOUNT")) && nm[0] == 'g' && nm[1] == 'l') {   /* cobre resolução de gl* via dlsym tb */
    void *p = dlsym(RTLD_DEFAULT, nm);
    void *w = ds_route(nm, p);
    if (w != p) return w;
  }
  if (getenv("CUP_SHLOG")) {
    if (!strcmp(nm, "glCompileShader")) return (void *)my_glCompileShader;
    if (!strcmp(nm, "glLinkProgram")) return (void *)my_glLinkProgram;
  }
  if (nm[0] == 'e' && nm[1] == 'g' && nm[2] == 'l') { void *p = egl_route(nm); if (p) return p; }
  /* AUDIO: dlsym do handle de libOpenSLES -> opensles_shim (slCreateEngine + SL_IID_*
     com as identidades DO SHIM — ele compara ponteiro, receita re4/Dysmantle) */
  if (h == &g_dl_sl) {
    fprintf(stderr, "[DLSYM:SL] pede \"%s\"\n", nm);
    if (!strcmp(nm, "slCreateEngine")) return (void *)slCreateEngine_shim;
    if (!strcmp(nm, "SL_IID_ENGINE")) return (void *)&sl_IID_ENGINE;
    if (!strcmp(nm, "SL_IID_PLAY")) return (void *)&sl_IID_PLAY;
    if (!strcmp(nm, "SL_IID_VOLUME")) return (void *)&sl_IID_VOLUME;
    if (!strcmp(nm, "SL_IID_BUFFERQUEUE") || !strcmp(nm, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"))
      return (void *)&sl_IID_BUFFERQUEUE;
    if (!strcmp(nm, "SL_IID_EFFECTSEND")) return (void *)&sl_IID_EFFECTSEND;
    if (!strcmp(nm, "SL_IID_ANDROIDCONFIGURATION")) return (void *)&sl_IID_ANDROIDCONFIGURATION;
    if (!strcmp(nm, "SL_IID_ENGINECAPABILITIES")) return (void *)&sl_IID_ENGINECAPABILITIES;
    if (!strcmp(nm, "SL_IID_ENVIRONMENTALREVERB")) return (void *)&sl_IID_ENVIRONMENTALREVERB;
    /* s14: o FMOD resolve TODOS os SL_IID_* de antemão e ABORTA o init se qualquer
       um vier NULL (RECORD, MIDI...). Identidade genérica única por nome — os
       GetInterface dos objetos do shim devolvem stub-success p/ IID desconhecido. */
    if (!strncmp(nm, "SL_IID_", 7)) {
      static struct { char name[48]; void *id; } gen[24];
      static void *slots[24];
      for (int i = 0; i < 24; i++) {
        if (gen[i].name[0] && !strcmp(gen[i].name, nm)) return (void *)&gen[i].id;
        if (!gen[i].name[0]) {
          snprintf(gen[i].name, sizeof gen[i].name, "%s", nm);
          gen[i].id = &slots[i];
          fprintf(stderr, "[DLSYM:SL] %s -> identidade generica\n", nm);
          return (void *)&gen[i].id;
        }
      }
    }
    fprintf(stderr, "[DLSYM:SL] %s -> NULL\n", nm);
    return NULL;
  }
  {
    void *p = rxd_cri_fake_dlsym(h, nm);
    if (p || (env_on("TER_RXD_CRI_FAKE") &&
              (!strncmp(nm, "cri", 3) || !strncmp(nm, "CRIWARE", 7) || !strncmp(nm, "Unity", 5))))
      return p;
  }
  /* qualquer simbolo il2cpp_* resolve no modulo il2cpp (qualquer handle) */
  if (!strncmp(nm, "il2cpp", 6) && g_m_il2cpp) {
    so_module *c = so_save(); so_use(g_m_il2cpp);
    void *p = (void *)so_find_addr_safe(nm);
    so_use(c); free(c);
    fprintf(stderr, "[DLSYM:il2cpp*] %s -> %p\n", nm, p);
    return p;
  }
  if (h == &g_dl_il2cpp && g_m_il2cpp) {
    so_module *c = so_save(); so_use(g_m_il2cpp);
    void *p = (void *)so_find_addr_safe(nm);
    so_use(c); free(c);
    fprintf(stderr, "[DLSYM:il2cpp] %s -> %p\n", nm, p);
    return p;
  }
  if (h == &g_dl_cri && g_m_cri) {
    so_module *c = so_save(); so_use(g_m_cri);
    void *p = (void *)so_find_addr_safe(nm);
    so_use(c); free(c);
    fprintf(stderr, "[DLSYM:cri] %s -> %p\n", nm, p);
    return p;
  }
  if (h == &g_dl_burst && g_m_burst) {
    so_module *c = so_save(); so_use(g_m_burst);
    void *p = (void *)so_find_addr_safe(nm);
    so_use(c); free(c);
    fprintf(stderr, "[DLSYM:burst] %s -> %p\n", nm, p);
    return p;
  }
  if (h == &g_dl_mainlib && g_m_mainlib) {
    so_module *c = so_save(); so_use(g_m_mainlib);
    void *p = (void *)so_find_addr_safe(nm);
    so_use(c); free(c);
    fprintf(stderr, "[DLSYM:main] %s -> %p\n", nm, p);
    return p;
  }
  if (g_m_cri && (!strncmp(nm, "cri", 3) || !strncmp(nm, "CRIWARE", 7) ||
                  !strcmp(nm, "UnityRenderEvent"))) {
    so_module *c = so_save(); so_use(g_m_cri);
    void *p = (void *)so_find_addr_safe(nm);
    so_use(c); free(c);
    if (p) { fprintf(stderr, "[DLSYM:cri*] %s -> %p\n", nm, p); return p; }
  }
  if (h == &g_dl_self) {
    void *p = (void *)so_find_addr_safe(nm);
    if (!p && g_m_il2cpp) { so_module *c = so_save(); so_use(g_m_il2cpp); p = (void *)so_find_addr_safe(nm); so_use(c); free(c); }
    if (!p && g_m_cri) { so_module *c = so_save(); so_use(g_m_cri); p = (void *)so_find_addr_safe(nm); so_use(c); free(c); }
    if (!p && g_m_burst) { so_module *c = so_save(); so_use(g_m_burst); p = (void *)so_find_addr_safe(nm); so_use(c); free(c); }
    if (!p && g_m_mainlib) { so_module *c = so_save(); so_use(g_m_mainlib); p = (void *)so_find_addr_safe(nm); so_use(c); free(c); }
    if (!p) p = dlsym(RTLD_DEFAULT, nm);
    return p;
  }
  return dlsym(h, nm);
}
static const char *my_dlerror(void) { return NULL; }
static int my_dladdr(const void *a, void *i) { (void)a; (void)i; return 0; }
static int my_dlclose(void *h) { (void)h; return 0; }

/* ---------- TLS bridge (bionic keys -> slots nossos; 1 glibc key) ---------- */
#define NSLOT 1024
static pthread_key_t g_tls_base; static int g_tls_init = 0;
static int g_slot_next = 1; static pthread_mutex_t g_slot_mtx = PTHREAD_MUTEX_INITIALIZER;
static void tls_dtor(void *p) { free(p); }
static void **tls_slots(void) {
  if (!g_tls_init) { pthread_key_create(&g_tls_base, tls_dtor); g_tls_init = 1; }
  void **s = (void **)pthread_getspecific(g_tls_base);
  if (!s) { s = (void **)calloc(NSLOT, sizeof(void *)); pthread_setspecific(g_tls_base, s); }
  return s;
}
static int sh_key_create(unsigned *k, void (*d)(void *)) { (void)d; pthread_mutex_lock(&g_slot_mtx);
  int n = g_slot_next++; pthread_mutex_unlock(&g_slot_mtx); if (n >= NSLOT) return 11; *k = (unsigned)n; return 0; }
static int sh_key_delete(unsigned k) { (void)k; return 0; }
static void *sh_getspecific(unsigned k) { if ((int)k <= 0 || (int)k >= NSLOT) return NULL; return tls_slots()[(int)k]; }
static int sh_setspecific(unsigned k, const void *v) { if ((int)k <= 0 || (int)k >= NSLOT) return 22; tls_slots()[(int)k] = (void *)v; return 0; }

/* ---------- abort/raise/tgkill: loga o CALLER (achar a origem do fatal) ---------- */
static void map_caller(const char *tag, uintptr_t ra) {
  if (g_unity_base && ra >= g_unity_base && ra < g_unity_base + 0x2000000)
    fprintf(stderr, "%s caller=libunity+0x%lx\n", tag, ra - g_unity_base);
  else if (g_il2cpp_base && ra >= g_il2cpp_base && ra < g_il2cpp_base + 0x3000000)
    fprintf(stderr, "%s caller=libil2cpp+0x%lx\n", tag, ra - g_il2cpp_base);
  else fprintf(stderr, "%s caller=0x%lx (?)\n", tag, ra);
  fflush(stderr);
}
static int my_raise(int sig) { map_caller("[RAISE]", (uintptr_t)__builtin_return_address(0));
  fprintf(stderr, "[RAISE] sig=%d\n", sig); if (getenv("CUP_NORAISE")) return 0; return raise(sig); }
static void my_abort(void) { map_caller("[ABORT]", (uintptr_t)__builtin_return_address(0));
  if (getenv("CUP_NORAISE")) return; abort(); }
static int my_tgkill(int tgid, int tid, int sig) { map_caller("[TGKILL]", (uintptr_t)__builtin_return_address(0));
  fprintf(stderr, "[TGKILL] sig=%d\n", sig); if (getenv("CUP_NORAISE")) return 0; return syscall(__NR_tgkill, tgid, tid, sig); }

/* __stack_chk_fail: o operator-new tagueado (0x3cbf2c) tem canario; numa chamada
 * do RecreateGfxState ele falha -> abort. Neutraliza p/ diagnosticar (loga caller). */
static int g_scf_n = 0;
static void my_stack_chk_fail(void) {
  uintptr_t ra = (uintptr_t)__builtin_return_address(0);
  if (g_scf_n++ == 0) {
    fprintf(stderr, "\n[SCF] __stack_chk_fail caller=%lx", ra);
    if (g_alloc_ub && ra >= g_alloc_ub && ra < g_alloc_ub + 0x11e6000)
      fprintf(stderr, " (libunity+0x%lx)", ra - g_alloc_ub);
    fprintf(stderr, "\n[SCF] stack scan (callers unity FORA do operator-new):\n");
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    for (int k = 0, hits = 0; k < 800 && hits < 30; k++) {
      uintptr_t v = *(uintptr_t *)(sp + k * 8);
      if (g_alloc_ub && v >= g_alloc_ub && v < g_alloc_ub + 0x11e6000) {
        uintptr_t off = v - g_alloc_ub;
        const char *tag = (off >= 0x3cbe90 && off <= 0x3cc1a0) ? " [op-new]" : " <==";
        fprintf(stderr, "  sp+0x%04x libunity+0x%lx%s\n", k * 8, off, tag);
        hits++;
      } else if (g_alloc_ib && v >= g_alloc_ib && v < g_alloc_ib + 0x2325000) {
        fprintf(stderr, "  sp+0x%04x libil2cpp+0x%lx\n", k * 8, v - g_alloc_ib);
        hits++;
      }
    }
    fflush(stderr);
  }
  /* retorna em vez de abort */
}

/* ---------- _ctype_ (tabela BSD/bionic de classes de caractere) ----------
 * libunity (bionic) importa `_ctype_` — uma `const char*` que aponta p/ uma tabela
 * de 257 bytes; isalpha/isdigit/tolower fazem `_ctype_[(int)c+1] & BITS`. O glibc
 * NÃO exporta `_ctype_` → ficava UNRESOLVED (NULL) → o processamento de string do
 * engine (nomes de asset etc.) fazia `ldr [_ctype_]; ldr [x0]` = NULL deref (crash
 * libunity+0xe449d4 no asset loading). Provemos a tabela (preenchida via glibc) e
 * resolvemos o símbolo. Bits bionic: _U=1 _L=2 _N=4 _S=8 _P=0x10 _C=0x20 _X=0x40 _B=0x80. */
#include <ctype.h>
static unsigned char g_ctype_table[257];
static const unsigned char *g_ctype_ptr = g_ctype_table;  /* _ctype_ aponta p/ a base; idx [c+1] */
static unsigned char g_tolower_table[257], g_toupper_table[257];
static const unsigned char *g_tolower_ptr = g_tolower_table, *g_toupper_ptr = g_toupper_table;
static void ctype_init(void) {
  g_ctype_table[0] = 0;                 /* slot do EOF (c=-1) */
  g_tolower_table[0] = 0; g_toupper_table[0] = 0;
  for (int c = 0; c < 256; c++) {
    unsigned char b = 0;
    if (isupper(c)) b |= 0x01;          /* _U */
    if (islower(c)) b |= 0x02;          /* _L */
    if (isdigit(c)) b |= 0x04;          /* _N */
    if (isspace(c)) b |= 0x08;          /* _S */
    if (ispunct(c)) b |= 0x10;          /* _P */
    if (iscntrl(c)) b |= 0x20;          /* _C */
    if (isxdigit(c) && !isdigit(c)) b |= 0x40; /* _X (só hex-letra; dígitos já têm _N) */
    if (c == ' ')  b |= 0x80;           /* _B (blank imprimível) */
    g_ctype_table[c + 1] = b;
    g_tolower_table[c + 1] = (unsigned char)tolower(c);
    g_toupper_table[c + 1] = (unsigned char)toupper(c);
  }
}
/* resolve _ctype_/_tolower_tab_/_toupper_tab_ na GOT do módulo ATIVO. A GOT guarda
 * o ENDEREÇO da variável-ponteiro (code: ldr [got]→ptr_var; ldr [ptr_var]→tabela). */
static void ctype_resolve(void) {
  so_patch_got("_ctype_", (uintptr_t)&g_ctype_ptr);
  so_patch_got("_tolower_tab_", (uintptr_t)&g_tolower_ptr);
  so_patch_got("_toupper_tab_", (uintptr_t)&g_toupper_ptr);
}

/* ---------- helper: override import na tabela ---------- */
static void set_import(const char *name, void *fn) {
  for (size_t i = 0; i < dynlib_numfunctions; i++)
    if (!strcmp(dynlib_functions[i].symbol, name)) { dynlib_functions[i].func = (uintptr_t)fn; return; }
}

/* patch_got: sobrescreve o slot da GOT DIRETO (apos so_resolve). Necessario p/
 * simbolos que NAO estao em dynlib_functions (NDK: ANativeWindow_*, __android_log_*,
 * ASensor*, ...) — p/ esses set_import e' no-op e ficam UNRESOLVED com GOT lixo. */
static int patch_got(const char *name, void *fn) {
  int n = so_patch_got(name, (uintptr_t)fn);
  if (!n) fprintf(stderr, "[GOT] %s: 0 slots (nao achado)\n", name);
  return n;
}

static char rxd_sl_iid_record_tag;
static const SLInterfaceID rxd_sl_iid_record = &rxd_sl_iid_record_tag;

static void set_android_import_overrides(void) {
  set_import("AAssetManager_fromJava", (void *)jni_AAssetManager_fromJava);
  set_import("AAssetManager_open", (void *)jni_AAssetManager_open);
  set_import("AAsset_getLength64", (void *)jni_AAsset_getLength64);
  set_import("AAsset_read", (void *)jni_AAsset_read);
  set_import("AAsset_seek", (void *)jni_AAsset_seek);
  set_import("AAsset_openFileDescriptor64", (void *)jni_AAsset_openFileDescriptor64);
  set_import("AAsset_close", (void *)jni_AAsset_close);
  set_import("slCreateEngine", (void *)slCreateEngine_shim);
  set_import("SL_IID_ENGINE", (void *)&sl_IID_ENGINE);
  set_import("SL_IID_PLAY", (void *)&sl_IID_PLAY);
  set_import("SL_IID_VOLUME", (void *)&sl_IID_VOLUME);
  set_import("SL_IID_BUFFERQUEUE", (void *)&sl_IID_BUFFERQUEUE);
  set_import("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (void *)&sl_IID_BUFFERQUEUE);
  set_import("SL_IID_RECORD", (void *)&rxd_sl_iid_record);
  set_import("SL_IID_ANDROIDCONFIGURATION", (void *)&sl_IID_ANDROIDCONFIGURATION);
  set_import("SL_IID_EFFECTSEND", (void *)&sl_IID_EFFECTSEND);
  set_import("SL_IID_ENGINECAPABILITIES", (void *)&sl_IID_ENGINECAPABILITIES);
  set_import("SL_IID_ENVIRONMENTALREVERB", (void *)&sl_IID_ENVIRONMENTALREVERB);
}

static void patch_android_import_overrides(void) {
  patch_got("AAssetManager_fromJava", (void *)jni_AAssetManager_fromJava);
  patch_got("AAssetManager_open", (void *)jni_AAssetManager_open);
  patch_got("AAsset_getLength64", (void *)jni_AAsset_getLength64);
  patch_got("AAsset_read", (void *)jni_AAsset_read);
  patch_got("AAsset_seek", (void *)jni_AAsset_seek);
  patch_got("AAsset_openFileDescriptor64", (void *)jni_AAsset_openFileDescriptor64);
  patch_got("AAsset_close", (void *)jni_AAsset_close);
  patch_got("slCreateEngine", (void *)slCreateEngine_shim);
  patch_got("SL_IID_ENGINE", (void *)&sl_IID_ENGINE);
  patch_got("SL_IID_PLAY", (void *)&sl_IID_PLAY);
  patch_got("SL_IID_VOLUME", (void *)&sl_IID_VOLUME);
  patch_got("SL_IID_BUFFERQUEUE", (void *)&sl_IID_BUFFERQUEUE);
  patch_got("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (void *)&sl_IID_BUFFERQUEUE);
  patch_got("SL_IID_RECORD", (void *)&rxd_sl_iid_record);
  patch_got("SL_IID_ANDROIDCONFIGURATION", (void *)&sl_IID_ANDROIDCONFIGURATION);
  patch_got("SL_IID_EFFECTSEND", (void *)&sl_IID_EFFECTSEND);
  patch_got("SL_IID_ENGINECAPABILITIES", (void *)&sl_IID_ENGINECAPABILITIES);
  patch_got("SL_IID_ENVIRONMENTALREVERB", (void *)&sl_IID_ENVIRONMENTALREVERB);
  patch_got("__android_log_print", (void *)my_alog_print);
  patch_got("__android_log_write", (void *)my_alog_write);
  patch_got("__android_log_vprint", (void *)my_alog_vprint);
  patch_got("dlopen", (void *)my_dlopen);
  patch_got("dlsym", (void *)my_dlsym);
  patch_got("dlerror", (void *)my_dlerror);
  patch_got("dlclose", (void *)my_dlclose);
  patch_got("dladdr", (void *)my_dladdr);
  patch_got("abort", (void *)my_abort);
  patch_got("raise", (void *)my_raise);
  patch_got("exit", (void *)my_exit);
  patch_got("_exit", (void *)my_exit);
  patch_got("glGetString", (void *)my_glGetString);
}

static so_module *load_android_plugin_so(const char *soname, size_t heap_mb,
                                         const char *tag, void *vm) {
  if (access(soname, R_OK) != 0) {
    fprintf(stderr, "[%s] %s ausente, pulando\n", tag, soname);
    return NULL;
  }
  so_module *prev = so_save();
  size_t sz = heap_mb * 1024UL * 1024UL;
  void *heap = mmap(NULL, sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) {
    fprintf(stderr, "[%s] mmap %zuMB falhou\n", tag, heap_mb);
    so_use(prev); free(prev);
    return NULL;
  }
  fprintf(stderr, "[%s] carregando %s em heap %zuMB @ %p\n", tag, soname, heap_mb, heap);
  if (so_load(soname, heap, sz) < 0) {
    fprintf(stderr, "[%s] so_load falhou\n", tag);
    munmap(heap, sz);
    so_use(prev); free(prev);
    return NULL;
  }
  if (so_relocate() < 0) fprintf(stderr, "[%s] relocate reportou falha\n", tag);
  set_android_import_overrides();
  { extern void recon_fill_passthrough(void); recon_fill_passthrough(); }
  so_resolve(dynlib_functions, dynlib_numfunctions, 0);
  ctype_resolve();
  patch_android_import_overrides();
  patch_sem_shim();
  patch_pthread_shim();
  so_record_phdr(soname);
  so_finalize(); so_flush_caches();
  so_execute_init_array();
  uintptr_t onload = so_find_addr_safe("JNI_OnLoad");
  if (onload && vm) {
    int ver = ((int (*)(void *, void *))onload)(vm, NULL);
    fprintf(stderr, "[%s] JNI_OnLoad = 0x%x\n", tag, ver);
  }
  so_module *m = so_save();
  fprintf(stderr, "[%s] %s carregado OK\n", tag, soname);
  so_use(prev); free(prev);
  return m;
}

static void *volatile g_preload_mgr;  /* PreloadManager capturado pelo spy (CUP_PSPY) */

/* ===== CUP_SCENEGUARD (s12): crash casa→mapa =====
 * Instantiate na cena do mapa chega em libunity 0x541c9c (resolve {scene,idx}
 * do GameObject via helper 0x8f7c48 = ldp x8,x1,[GO+0x38]) e deref [scene+24]
 * SEM null-check. GO destruído/fora-de-cena tem scene NULL → SIGSEGV fault=0x18
 * (dump s11: x0=0 x1=0xffffffffff). Guard = ilha de código substituindo o bl;
 * hits contados aqui e logados no render loop. */
static volatile uint32_t g_sceneguard_hits;
static uint64_t sg_fake_scene[8];   /* [3] (+24) -> sg_fake_arr (handle w27=0) */
static uint32_t sg_fake_arr[16];

/* stubs NDK no-op (sensores/looper/profiler google) — devolvem 0/NULL */
static long ndk_stub0(void) { return 0; }

extern int my_sigaction();  /* bionic_shims.c (ABI sigset bionic/glibc) */

/* thread de áudio do FMOD (output AudioTrack Java): o Cuphead registra
   org.fmod.FMODAudioDevice.fmodProcess(ByteBuffer) e espera uma thread Java
   chamá-la em loop p/ o mixer avançar. Sem JVM, replicamos em C: assim que
   fmodProcess existir, chamamos com nosso ByteBuffer a cada ~10ms. Isso destrava
   o boot (a main fica presa no nativeRender esperando o áudio progredir). */
static void *g_fmod_env;
static volatile int g_fmod_run = 1;

/* ---- CUP_PSPY: espião do PreloadManager (muro frame 111) ----
 * RE (libunity, offsets confirmados no disasm):
 *   [mgr+208/+224] = fila de PRELOAD  (array/count) — consumida pela thread
 *                    UnityPreload (entry 0x8736cc): roda op->vt[+80] em bg e
 *                    seta op[72]=1; move a op p/ fila de integração (push 0x873790).
 *   [mgr+240/+256] = fila de INTEGRAÇÃO (array/count) — consumida pela main em
 *                    UpdatePreloadingSingleStep (0x8733a8): chama op->vt[+88]
 *                    (timesliced); o POP ([+256]-- em 0x873500) SÓ acontece se
 *                    vt[+88] retornar true E op[72]==1.
 *   HasPendingOperations (0x8739c4) = lock(mgr+0x78); [+224]||[+256]; unlock.
 *   WaitForAllAsyncOperationsToComplete (0x873a90) gira enquanto pendente.
 * O spy substitui 0x8739c4 por uma réplica C (mesma semântica, mesmos locks
 * 0x8f5d0c/0x8f5d14) que captura o ponteiro do mgr; uma thread despeja as filas
 * (op, vtable, flags, alvos de vt[+80/+88/+112]) p/ identificar a op presa. */
#define UN_HASPEND   0x8739c4
#define UN_MTX_LOCK  0x8f5d0c
#define UN_MTX_UNLK  0x8f5d14
static volatile unsigned long g_haspend_calls;   /* quantas vezes a loading-screen consultou */
static volatile int g_haspend_stalled;            /* filas presas (mesmas ops) — gate da bg thread */
static int my_haspending(void *mgr) {
  g_preload_mgr = mgr;
  g_haspend_calls++;
  void (*lk)(void *) = (void (*)(void *))(g_unity_base + UN_MTX_LOCK);
  void (*ul)(void *) = (void (*)(void *))(g_unity_base + UN_MTX_UNLK);
  lk((char *)mgr + 0x78);
  uintptr_t pq = *(uintptr_t *)((char *)mgr + 224);
  uintptr_t iq = *(uintptr_t *)((char *)mgr + 256);
  uintptr_t pq_a = *(uintptr_t *)((char *)mgr + 208);
  uintptr_t iq_a = *(uintptr_t *)((char *)mgr + 240);
  uintptr_t pop = (pq && pq_a) ? ((uintptr_t *)pq_a)[0] : 0;
  uintptr_t iop = (iq && iq_a) ? ((uintptr_t *)iq_a)[0] : 0;
  ul((char *)mgr + 0x78);
  int pending = (pq || iq) ? 1 : 0;
  /* CUP_HASPEND_STALE=N: se as filas ficam IDÊNTICAS (mesmas ops, mesma contagem)
   * por N consultas seguidas, a(s) op(s) estão PRESAS — done72 nunca vira 1 (a thread
   * UnityPreload não processa a op no so-loader; ver 0x873900). O BOOT tolera essa op
   * persistente (não espera HasPendingOperations==0), mas a tela de loading do MAPA
   * espera -> trava eterna. Reportamos "sem pendências" p/ destravar a loading.
   * Só dispara em fila ESTÁVEL (sem progresso): se as ops mudam, é load real -> verdade. */
  static long stale_thr = -1, skip_thr = -1;
  if (stale_thr == -1) {
    /* detecção do stall: gate da bg thread (CUP_PRELOAD_BG). Limiar baixo p/ a bg
       kickar logo que o mapa trava; no boot/gameplay as filas FLUEM (ops entram/saem)
       -> nunca fica idêntico por tanto tempo -> bg fica ociosa, sem corromper o boot. */
    stale_thr = getenv("CUP_BG_STALL") ? atol(getenv("CUP_BG_STALL")) : 24;
    /* return-0 (pula a espera) — só se CUP_HASPEND_STALE setado (fallback bruto;
       deixa objetos meio-construídos -> use PRELOAD_BG preferencialmente) */
    skip_thr = getenv("CUP_HASPEND_STALE") ? atol(getenv("CUP_HASPEND_STALE")) : 0;
  }
  if (pending) {
    static uintptr_t last_pq, last_iq, last_pop, last_iop;
    static long stall;
    static int logged;
    if (pq == last_pq && iq == last_iq && pop == last_pop && iop == last_iop) {
      stall++;
      if (stall >= stale_thr && !g_haspend_stalled) {
        g_haspend_stalled = 1;
        fprintf(stderr, "[HASPEND] filas PRESAS (pq=%lu iq=%lu pop=%lx iop=%lx, %ld "
                "consultas) -> bg thread liberada\n", pq, iq, pop, iop, stall);
        dbg_sync();
      }
      if (skip_thr > 0 && stall >= skip_thr) {
        if (!logged) { logged = 1;
          fprintf(stderr, "[HASPEND] -> reportando SEM pendencias (CUP_HASPEND_STALE)\n");
          dbg_sync(); }
        return 0;
      }
    } else {
      stall = 0; logged = 0; g_haspend_stalled = 0;
      last_pq = pq; last_iq = iq; last_pop = pop; last_iop = iop;
    }
  } else {
    g_haspend_stalled = 0;
  }
  return pending;
}
static void pspy_dump_op(const char *qn, unsigned i, uintptr_t op) {
  uintptr_t vt = *(uintptr_t *)op;
  uintptr_t vt_off = (vt >= g_unity_base) ? vt - g_unity_base : vt;
  uintptr_t f80 = *(uintptr_t *)(vt + 80), f88 = *(uintptr_t *)(vt + 88);
  uintptr_t f112 = *(uintptr_t *)(vt + 112);
  fprintf(stderr,
          "[PSPY]  %s[%u] op=%lx vt=u+0x%lx done72=%d w64=%d w68=%d "
          "bg(+80)=u+0x%lx integ(+88)=u+0x%lx q(+112)=u+0x%lx\n",
          qn, i, op, vt_off, *(int *)(op + 72), *(int *)(op + 64),
          *(int *)(op + 68), f80 - g_unity_base, f88 - g_unity_base,
          f112 - g_unity_base);
  /* primeiros 0xC0 bytes da op (estado interno: progress/flags/ponteiros) */
  for (int k = 0; k < 24; k += 4)
    fprintf(stderr, "[PSPY]   +%02x: %016lx %016lx %016lx %016lx\n", k * 8,
            ((uintptr_t *)op)[k], ((uintptr_t *)op)[k + 1],
            ((uintptr_t *)op)[k + 2], ((uintptr_t *)op)[k + 3]);
}
/* CUP_PRELOAD_BG: a thread UnityPreload (entry 0x8736cc) processa o BACKGROUND das
 * ops de preload — 0x873900 faz: pop op da fila, chama op->vt[10] e op->vt[14], e
 * seta done72=1. No so-loader essa thread NÃO bombeia (ops do load da cena do mapa
 * ficam com done72=0 -> objetos meio-desserializados, campos null -> crashes
 * 0x541cdc/0x8f9b1c/0x542258; E a integração na main BLOQUEIA esperando o bg da PQ op,
 * por isso CUP_DRAINPRELOAD pendurava). Imitamos a thread faltante: chamamos
 * 0x873900(mgr) em loop. A função trava internamente mgr+0x78 (thread-safe, igual à
 * original); roda numa thread SEPARADA p/ não pendurar o render se uma op bloquear. */
static void *preload_bg_thread(void *arg) {
  (void)arg;
  int (*bg)(void *) = (int (*)(void *))(g_unity_base + 0x873900);
  fprintf(stderr, "[PRELOAD_BG] thread ativa (imita UnityPreload 0x873900)\n");
  unsigned long n = 0;
  /* só processa quando a loading-screen ESTÁ presa (g_haspend_stalled) — no boot
   * o engine bombeia as ops normalmente e processar em paralelo CORROMPE/trava
   * (render congelava em 1860). A detecção de stall só dispara no load do mapa
   * (filas idênticas por N consultas), nunca no boot (ops fluem).
   * CUP_BG_AFTER=N: gate ALTERNATIVO por frame — processa tb se g_render_frame>N
   * (p/ testar drenar a cena aditiva do mapa que NÃO gera stall detectável). */
  long bg_after = getenv("CUP_BG_AFTER") ? atol(getenv("CUP_BG_AFTER")) : 0;
  while (g_fmod_run) {
    void *m = g_preload_mgr;
    int active = g_haspend_stalled || (bg_after > 0 && g_render_frame > bg_after);
    if (m && active) {
      int did = bg(m);   /* processa 1 op de background; !=0 = fez trabalho */
      if (did) {
        if ((n++ % 16) == 0)
          fprintf(stderr, "[PRELOAD_BG] processou op presa (#%lu, f=%d)\n", n, g_render_frame);
        continue;        /* mais ops pendentes? drena sem dormir */
      }
    }
    usleep(2000);
  }
  return NULL;
}
static void *preload_spy_thread(void *arg) {
  (void)arg;
  fprintf(stderr, "[PSPY] thread ativa (2s)\n");
  while (g_fmod_run) {
    sleep(2);
    char *m = (char *)g_preload_mgr;
    if (!m) continue;
    /* leitura SEM lock (racy, mas nunca bloqueia/deadlocka o diagnóstico) */
    uintptr_t pq_a = *(uintptr_t *)(m + 208), pq_n = *(uintptr_t *)(m + 224);
    uintptr_t iq_a = *(uintptr_t *)(m + 240), iq_n = *(uintptr_t *)(m + 256);
    /* job-scheduler global (libunity bss 0x12b9380; o gate da integração só passa
       quando estes 3 contadores estão <=0 — 0x6cdad0). +0x70=jobs / +0x168 / +0x16c */
    void *jm = *(void **)(g_unity_data + 0xd3380);
    if (jm) fprintf(stderr, "[PSPY] jobmgr=%p +70=%d +168=%d +16c=%d\n", jm,
                    *(int *)((char *)jm + 0x70), *(int *)((char *)jm + 0x168),
                    *(int *)((char *)jm + 0x16c));
    fprintf(stderr, "[PSPY] mgr=%p preloadQ=%lu integQ=%lu\n", m, pq_n, iq_n);
    for (unsigned i = 0; i < pq_n && i < 2; i++)
      if (((uintptr_t *)pq_a)[i]) pspy_dump_op("PQ", i, ((uintptr_t *)pq_a)[i]);
    for (unsigned i = 0; i < iq_n && i < 2; i++)
      if (((uintptr_t *)iq_a)[i]) pspy_dump_op("IQ", i, ((uintptr_t *)iq_a)[i]);
    dbg_sync();
  }
  return NULL;
}
/* TESTE CUP_PRELOAD_TICK: posta periodicamente os sems em que threads não-main
   bloqueiam (acorda a UnityPreload p/ processar o item pendente da fila). */
static void *preload_tick_thread(void *arg) {
  (void)arg;
  fprintf(stderr, "[TICK] thread de preload-tick ativa (16ms)\n");
  while (g_fmod_run) { sh_tick_preload(); usleep(16000); }
  return NULL;
}

/* RXD/Unity2020: depois que a troca nativa para `splash` completa, o render chama
 * pequenas rotinas de fila da libunity com fila NULL no nosso host. No Android real
 * essas filas existem; aqui o NULL derruba `Loading.Preload` antes de qualquer draw.
 * Guard opt-in: quando a fila esta ausente, devolve fila vazia; quando esta valida,
 * chama a Unity original sem mudar o fluxo de scene/load. */
static void *(*rxd_preloadq_direct4_orig)(void *, uint64_t *);
static void *(*rxd_preloadq_slot24_orig)(void *, uint64_t *);
static void *(*rxd_preloadq_slot4_orig)(void *, uint64_t *);

static int rxd_preloadq_bad_ptr(const void *p) {
  return ((uintptr_t)p) < 0x10000UL;
}

static void rxd_preloadq_zero_count(uint64_t *want) {
  if (!rxd_preloadq_bad_ptr(want)) *want = 0;
}

static void rxd_preloadq_log(const char *tag, void *a0, void *want, void *inner) {
  static unsigned n;
  if (n++ >= 64 && (g_render_frame % 120) != 0) return;
  uintptr_t ra = (uintptr_t)__builtin_return_address(0);
  const char *lib = "?";
  uintptr_t off = ra;
  if (g_unity_base && ra >= g_unity_base && ra < g_unity_base + text_size) {
    lib = "libunity";
    off = ra - g_unity_base;
  } else if (g_il2cpp_base && ra >= g_il2cpp_base && ra < g_il2cpp_base + 0x3000000) {
    lib = "libil2cpp";
    off = ra - g_il2cpp_base;
  }
  fprintf(stderr, "[RXD_PRELOAD_NULL] %s fila vazia a0=%p want=%p inner=%p caller=%s+0x%lx f=%d\n",
          tag, a0, want, inner, lib, (unsigned long)off, g_render_frame);
  fsync(2);
}

static void *rxd_preloadq_direct4_hook(void *queue, uint64_t *want) {
  if (rxd_preloadq_bad_ptr(queue) || rxd_preloadq_bad_ptr(want)) {
    rxd_preloadq_zero_count(want);
    rxd_preloadq_log("direct4", queue, want, NULL);
    return NULL;
  }
  void *ring = *(void **)((char *)queue + 0x8);
  if (rxd_preloadq_bad_ptr(ring)) {
    rxd_preloadq_zero_count(want);
    rxd_preloadq_log("direct4", queue, want, ring);
    return NULL;
  }
  return rxd_preloadq_direct4_orig ? rxd_preloadq_direct4_orig(queue, want) : NULL;
}

static void *rxd_preloadq_slot24_hook(void *slot, uint64_t *want) {
  if (rxd_preloadq_bad_ptr(slot) || rxd_preloadq_bad_ptr(want)) {
    rxd_preloadq_zero_count(want);
    rxd_preloadq_log("slot24", slot, want, NULL);
    return NULL;
  }
  void *queue = *(void **)slot;
  if (rxd_preloadq_bad_ptr(queue)) {
    rxd_preloadq_zero_count(want);
    rxd_preloadq_log("slot24", slot, want, queue);
    return NULL;
  }
  return rxd_preloadq_slot24_orig ? rxd_preloadq_slot24_orig(slot, want) : NULL;
}

static void *rxd_preloadq_slot4_hook(void *slot, uint64_t *want) {
  if (rxd_preloadq_bad_ptr(slot) || rxd_preloadq_bad_ptr(want)) {
    rxd_preloadq_zero_count(want);
    rxd_preloadq_log("slot4", slot, want, NULL);
    return NULL;
  }
  void *queue = *(void **)slot;
  if (rxd_preloadq_bad_ptr(queue)) {
    rxd_preloadq_zero_count(want);
    rxd_preloadq_log("slot4", slot, want, queue);
    return NULL;
  }
  return rxd_preloadq_slot4_orig ? rxd_preloadq_slot4_orig(slot, want) : NULL;
}

static void rxd_install_preload_null_guard(uintptr_t base) {
  static int done;
  if (done || !env_on("TER_RXD_PRELOAD_NULL_GUARD")) return;
  done = 1;
  struct {
    uintptr_t rva;
    void *hook;
    void **orig;
    const char *name;
  } T[] = {
    {0x556468, (void *)rxd_preloadq_direct4_hook, (void **)&rxd_preloadq_direct4_orig, "RXD.preloadq.direct4"},
    {0x556774, (void *)rxd_preloadq_slot24_hook,  (void **)&rxd_preloadq_slot24_orig,  "RXD.preloadq.slot24"},
    {0x5568e8, (void *)rxd_preloadq_slot4_hook,   (void **)&rxd_preloadq_slot4_orig,   "RXD.preloadq.slot4"},
  };
  int ok = 0;
  for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
    void *tr = mk_tramp(base + T[i].rva, T[i].name);
    if (!tr) {
      fprintf(stderr, "[RXD_PRELOAD_NULL] tramp falhou %s @0x%lx\n",
              T[i].name, (unsigned long)T[i].rva);
      fsync(2);
      continue;
    }
    *T[i].orig = tr;
  }
  extern void so_make_text_writable(void), so_make_text_executable(void);
  so_make_text_writable();
  for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
    if (!*T[i].orig) continue;
    hook_arm64(base + T[i].rva, (uintptr_t)T[i].hook);
    ok++;
  }
  so_make_text_executable();
  so_flush_caches();
  fprintf(stderr, "[RXD_PRELOAD_NULL] guards instalados=%d\n", ok);
  fsync(2);
}
/* driver do Choreographer: anexa ao il2cpp e dispara doFrame(nanos) ~60Hz. */
static void *g_choreo_env;
extern int g_choreo_log;
typedef void (*native_choreo_fn)(void *, void *, long);
static native_choreo_fn rxd_find_native_choreo(void) {
  native_choreo_fn native_choreo = NULL;
  if (g_m_unity) {
    so_module *cur = so_save();
    so_use(g_m_unity);
    native_choreo = (native_choreo_fn)so_find_addr_safe("Java_com_google_androidgamesdk_ChoreographerCallback_nOnChoreographer");
    so_use(cur);
    free(cur);
  }
  return native_choreo;
}

static void *choreo_driver_thread(void *arg) {
  (void)arg;
  g_choreo_log = env_on("TER_CHOREOLOG") ? 1 : 0;
  extern int jni_choreo_doframe(void *env, long nanos);
  extern int jni_choreo_captured(void);
  extern long jni_choreo_handle(void);
  int wait_ms = env_int_default("TER_CHOREO_WAIT_MS", 15000);
  int step_us = env_int_default("TER_CHOREO_US", 16000);
  int max_frames = env_int_default("TER_CHOREO_MAX", 0);
  int log_every = env_int_default("TER_CHOREO_LOG_EVERY", 60);
  if (step_us <= 0) step_us = 16000;
  if (log_every < 0) log_every = 0;
  fprintf(stderr, "[CHOREO] thread ativa wait_ms=%d step_us=%d max=%d attach=%d\n",
          wait_ms, step_us, max_frames, env_on("TER_CHOREO_ATTACH"));
  fsync(2);
  /* ESPERA o FrameCallback ser capturado (frame 2) ANTES de anexar — anexar cedo (durante a
     init do il2cpp no frame 0) crashava o thread_attach (il2cpp ainda não pronto). */
  int waited_ms = 0;
  while (!jni_choreo_captured()) {
    if (wait_ms > 0 && waited_ms >= wait_ms) {
      fprintf(stderr, "[CHOREO] timeout esperando FrameCallback (%dms)\n", waited_ms);
      fsync(2);
      return NULL;
    }
    usleep(20000);
    waited_ms += 20;
  }
  fprintf(stderr, "[CHOREO] FrameCallback capturado apos %dms\n", waited_ms);
  fsync(2);
  if (g_il2cpp_base && env_on("TER_CHOREO_ATTACH")) {
    void *(*dom_get)(void) = (void *(*)(void))ter_il2cpp_sym_cached("il2cpp_domain_get");
    void *(*thr_attach)(void *) = (void *(*)(void *))ter_il2cpp_sym_cached("il2cpp_thread_attach");
    void *th = (dom_get && thr_attach) ? thr_attach(dom_get()) : NULL;
    fprintf(stderr, "[CHOREO] FrameCallback pronto; il2cpp_thread_attach -> %p\n", th); fsync(2);
  } else if (g_il2cpp_base) {
    fprintf(stderr, "[CHOREO] il2cpp_thread_attach desativado (TER_CHOREO_ATTACH=0)\n");
    fsync(2);
  } else {
    fprintf(stderr, "[CHOREO] g_il2cpp_base=0 (sem attach)\n");
    fsync(2);
  }
  native_choreo_fn native_choreo = env_on("TER_CHOREO_NATIVE") ? rxd_find_native_choreo() : NULL;
  int native_mode = native_choreo != NULL;
  int both_paths = env_on("TER_CHOREO_BOTH");
  fprintf(stderr, "[CHOREO] native=%p mode=%d both=%d handle=0x%lx\n",
          (void *)native_choreo, native_mode, both_paths, (unsigned long)jni_choreo_handle());
  fsync(2);
  int started = 0;
  int sent = 0;
  for (;;) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long nanos = (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
    int r = 0;
    if (native_mode) {
      long h = jni_choreo_handle();
      if (h) {
        native_choreo(g_choreo_env, NULL, h);
        r = 1;
      } else if (!started) {
        fprintf(stderr, "[CHOREO] native sem handle ainda\n");
        fsync(2);
      }
    }
    if (!native_mode || both_paths) {
      int jr = jni_choreo_doframe(g_choreo_env, nanos);
      r = r || jr;
    }
    if (r && !started) { started = 1; fprintf(stderr, "[CHOREO] doFrame começou a disparar\n"); fsync(2); }
    if (r) {
      sent++;
      if (log_every > 0 && (sent % log_every) == 0) {
        fprintf(stderr, "[CHOREO] doFrame #%d\n", sent);
        fsync(2);
      }
      if (max_frames > 0 && sent >= max_frames) {
        fprintf(stderr, "[CHOREO] limite atingido (%d doFrame); encerrando thread\n", sent);
        fsync(2);
        return NULL;
      }
    }
    usleep((useconds_t)step_us);  /* ~60Hz por padrao */
  }
  return NULL;
}

static void rxd_choreo_inline_tick(void *env) {
  extern int jni_choreo_doframe(void *env, long nanos);
  extern int jni_choreo_captured(void);
  extern long jni_choreo_handle(void);
  static int init, started;
  static native_choreo_fn native_choreo;
  static int native_mode, both_paths, log_every, sent;
  if (!init) {
    init = 1;
    g_choreo_log = env_on("TER_CHOREOLOG") ? 1 : 0;
    native_choreo = env_on("TER_CHOREO_NATIVE") ? rxd_find_native_choreo() : NULL;
    native_mode = native_choreo != NULL;
    both_paths = env_on("TER_CHOREO_BOTH");
    log_every = env_int_default("TER_CHOREO_LOG_EVERY", 60);
    if (log_every < 0) log_every = 0;
    fprintf(stderr, "[CHOREO_INLINE] active native=%p mode=%d both=%d\n",
            (void *)native_choreo, native_mode, both_paths);
    fsync(2);
  }
  if (!jni_choreo_captured()) return;
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  long nanos = (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
  int r = 0;
  if (native_mode) {
    long h = jni_choreo_handle();
    if (h) {
      native_choreo(env, NULL, h);
      r = 1;
    }
  }
  if (!native_mode || both_paths) {
    int jr = jni_choreo_doframe(env, nanos);
    r = r || jr;
  }
  if (r && !started) {
    started = 1;
    fprintf(stderr, "[CHOREO_INLINE] doFrame comecou na thread de render handle=0x%lx\n",
            (unsigned long)jni_choreo_handle());
    fsync(2);
  }
  if (r) {
    sent++;
    if (log_every > 0 && (sent % log_every) == 0) {
      fprintf(stderr, "[CHOREO_INLINE] doFrame #%d\n", sent);
      fsync(2);
    }
  }
}

static void rxd_log_render_state(const char *tag, int f) {
  if (!g_unity_base || !env_on("TER_RENDERSTATE")) return;
  unsigned char *b = (unsigned char *)(g_unity_base + 0x182a000);
  unsigned char *b2 = (unsigned char *)(g_unity_base + 0x182b000);
  void *app = NULL;
  int app340 = -1, app341 = -1, app344 = -1;
  void *(*get_app)(void) = (void *(*)(void))(g_unity_base + 0x430d5c);
  if (get_app) {
    app = get_app();
    if (app) {
      app340 = *(unsigned char *)((char *)app + 340);
      app341 = *(unsigned char *)((char *)app + 341);
      app344 = *(int *)((char *)app + 344);
    }
  }
  fprintf(stderr, "[RSTATE] %s f=%d b2992=%u b3032=%u b3036=%u b3040=%u b3041=%u b3048=%u p3056=%p b3064=%u b3068=%u b3069=%u b2_80=%u app=%p a340=%d a341=%d a344=%d\n",
          tag, f,
          b[2992], b[3032], b[3036], b[3040], b[3041], b[3048],
          *(void **)(b + 3056), b[3064], b[3068], b[3069], b2[128],
          app, app340, app341, app344);
  fsync(2);
}

static void rxd_keep_rendering_tick(int f) {
  if (!g_unity_base || !env_on("TER_RXD_KEEP_RENDERING")) return;
  int from = env_int_default("TER_RXD_KEEP_RENDERING_FROM", 0);
  if (f < from) return;
  void *(*get_app)(void) = (void *(*)(void))(g_unity_base + 0x430d5c);
  void *app = get_app ? get_app() : NULL;
  if (!app) return;
  unsigned char *a340 = (unsigned char *)app + 340;
  unsigned char *a341 = (unsigned char *)app + 341;
  int dirty_from = env_int_default("TER_RXD_KEEP_RENDERING_DIRTY_FROM", 0);
  if (env_on("TER_RXD_KEEP_RENDERING_DIRTY") && f >= dirty_from && !*a340) {
    static int dn;
    if (dn++ < 24 || (f % 60) == 0) {
      fprintf(stderr, "[RKEEP] app+340 %u->1 f=%d app=%p a341=%u a344=%d\n",
              *a340, f, app, *a341, *(int *)((char *)app + 344));
      fsync(2);
    }
    *a340 = 1;
  }
  if (*a341) {
    static int n;
    if (n++ < 24 || (f % 60) == 0) {
      fprintf(stderr, "[RKEEP] app+341 %u->0 f=%d app=%p a340=%u a344=%d\n",
              *a341, f, app, *a340, *(int *)((char *)app + 344));
      fsync(2);
    }
    *a341 = 0;
  }
}

static void rxd_force_render_gate_tick(int f) {
  if (!g_unity_base || !env_on("TER_RXD_RENDER_GATE")) return;
  static int once_done;
  int from = env_int_default("TER_RXD_RENDER_GATE_FROM", 0);
  if (f < from) return;
  int until = env_int_default("TER_RXD_RENDER_GATE_UNTIL", -1);
  if (until >= 0 && f > until) return;
  int period = env_int_default("TER_RXD_RENDER_GATE_PERIOD", 0);
  if (period > 0 && ((f - from) % period) != 0) return;
  if (env_on("TER_RXD_RENDER_GATE_ONCE")) {
    if (once_done) return;
    once_done = 1;
  }

  unsigned char *b = (unsigned char *)(g_unity_base + 0x182a000);
  int old2992 = b[2992], old3036 = b[3036], old3040 = b[3040];
  int old3048 = b[3048], old3069 = b[3069];
  int changed = 0;

  if (!env_on("TER_RXD_RENDER_GATE_KEEP3048") && b[3048]) {
    b[3048] = 0;
    changed = 1;
  }
  if (env_on("TER_RXD_RENDER_GATE_SET2992") && !b[2992]) {
    b[2992] = 1;
    changed = 1;
  }
  if (env_on("TER_RXD_RENDER_GATE_SET3036") && !b[3036]) {
    b[3036] = 1;
    changed = 1;
  }
  if (env_on("TER_RXD_RENDER_GATE_CLEAR3040") && b[3040]) {
    b[3040] = 0;
    changed = 1;
  }
  if (env_on("TER_RXD_RENDER_GATE_CLEAR3069") && b[3069]) {
    b[3069] = 0;
    changed = 1;
  }

  void *app = NULL;
  int old340 = -1, old341 = -1, old344 = -1;
  if (env_on("TER_RXD_RENDER_GATE_APP340") || env_on("TER_RXD_RENDER_GATE_APP341_CLEAR")) {
    void *(*get_app)(void) = (void *(*)(void))(g_unity_base + 0x430d5c);
    app = get_app ? get_app() : NULL;
    if (app) {
      unsigned char *a340 = (unsigned char *)app + 340;
      unsigned char *a341 = (unsigned char *)app + 341;
      old340 = *a340;
      old341 = *a341;
      old344 = *(int *)((char *)app + 344);
      if (env_on("TER_RXD_RENDER_GATE_APP340") && !*a340) {
        *a340 = 1;
        changed = 1;
      }
      if (env_on("TER_RXD_RENDER_GATE_APP341_CLEAR") && *a341) {
        *a341 = 0;
        changed = 1;
      }
    }
  }

  static int n;
  if (changed || n < 24 || (f % 60) == 0) {
    fprintf(stderr,
            "[RGATE] f=%d b2992=%d->%u b3036=%d->%u b3040=%d->%u b3048=%d->%u b3069=%d->%u app=%p a340=%d a341=%d a344=%d\n",
            f, old2992, b[2992], old3036, b[3036], old3040, b[3040],
            old3048, b[3048], old3069, b[3069], app, old340, old341, old344);
    fsync(2);
  }
  n++;
}
/* CUP_FORCESL: substitui a decisão de backend de áudio do FMOD (0x350298) */
static long forcesl_hook(void) { return 2; }   /* 2 = OpenSL */
/* CUP_FMODSPY (s14): loga o retorno das etapas do init do FMOD p/ achar QUAL
 * subsistema falha (output OpenSL passa: engine+player+8 enqueues+start ok, e o
 * 3->1 é teardown). 0xa6281c=System::init; 0xa6dbe0=output_opensl init;
 * 0xa6e270=output start (SetPlayState PLAYING). */
static long (*fmod_init_orig)(long, long, long, long, long, long, long, long);
static long (*fmod_oinit_orig)(long, long, long, long, long, long, long, long);
static long (*fmod_ostart_orig)(long, long, long, long, long, long, long, long);
static long fmod_init_hook(long a, long b, long c, long d, long e, long f, long g, long h) {
  long r = fmod_init_orig(a, b, c, d, e, f, g, h);
  fprintf(stderr, "[FMODSPY] System::init(maxch=%ld flags=0x%lx extra=%lx) -> %ld\n", b, c, d, r);
  fsync(2);
  return r;
}
static long fmod_oinit_hook(long a, long b, long c, long d, long e, long f, long g, long h) {
  long r = fmod_oinit_orig(a, b, c, d, e, f, g, h);
  fprintf(stderr, "[FMODSPY] output_opensl.init -> %ld\n", r); fsync(2);
  return r;
}
static long fmod_ostart_hook(long a, long b, long c, long d, long e, long f, long g, long h) {
  long r = fmod_ostart_orig(a, b, c, d, e, f, g, h);
  fprintf(stderr, "[FMODSPY] output_opensl.start -> %ld\n", r); fsync(2);
  return r;
}
/* ===== CUP_FMODALLOCGUARD (s14, default ON com FORCESL) — SOM =====
 * O alocador do FMOD (0xa66e6c / 0xa66b74: pool, size w1, file x2, line w3) recebe
 * um pedido INSANO de ~4.29GB do **DSP de flange** (fmod_dsp_flange.cpp:172): o
 * cálculo do buffer de delay (samplerate*40ms / blocksize * canais * 2) estoura no
 * so-loader (um dos campos do mixer vem corrompido) -> o wrapper loga "System out of
 * memory (MemoryLabel: FMOD)" e ABORTA o engine (fatal, SIGTRAP no boot). O próprio
 * flange tem caminho de falha LIMPO: se a alocação retorna NULL (0xa47570 cbz ->
 * 0xa47658), ele retorna FMOD_ERR_MEMORY e o FMOD SEGUE sem o efeito de flange
 * (irrelevante p/ o jogo). Guard: pedidos > 100MB do FMOD -> NULL (sem chamar o
 * wrapper fatal). Os allocs normais (KB/poucos MB) passam direto. */
#define FMOD_ALLOC_SANE 0x6400000UL  /* 100 MB */
static long (*fmod_alloc_orig)(long, long, long, long, long, long, long, long);
static long fmod_alloc_hook(long a, long b, long c, long d, long e, long f, long g, long h) {
  if ((unsigned long)b > FMOD_ALLOC_SANE) {
    static int n; if (n++ < 6)
      fprintf(stderr, "[FMODGUARD] alloc insana %lu (file=%s line=%ld) -> NULL (flange skip)\n",
              (unsigned long)b, (c && *(char *)c) ? (char *)c : "?", d);
    fsync(2);
    return 0;  /* NULL: o caller (flange create) trata como ERR_MEMORY e segue */
  }
  return fmod_alloc_orig(a, b, c, d, e, f, g, h);
}
/* SIGUSR1: dump leve do backtrace da thread que recebe (acha endereços libunity/
 * il2cpp na pilha) e RETORNA (não mata). Diagnóstico de hang: manda SIGUSR1 e vê a
 * call chain do wait. Gateado por nada — só dispara quando o sinal chega. */
static void diag_handler(int sig, siginfo_t *si, void *uc_) {
  (void)sig; (void)si;
  ucontext_t *uc = (ucontext_t *)uc_;
  uintptr_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30], sp = uc->uc_mcontext.sp;
  uintptr_t ub = g_unity_base, ib = g_il2cpp_base;
  fprintf(stderr, "[DIAG] tid=%d pc=0x%lx lr=0x%lx", (int)syscall(SYS_gettid),
          (unsigned long)pc, (unsigned long)lr);
  if (ub && lr >= ub && lr < ub + text_size) fprintf(stderr, " lr=libunity+0x%lx", lr - ub);
  fprintf(stderr, "\n");
  int hits = 0;
  for (uintptr_t a = sp; a + 8 <= sp + 16384UL * 8 && hits < 60; a += 8) {
    if (!addr_readable(a)) break;
    uintptr_t v = *(uintptr_t *)a;
    if (ub && v >= ub && v < ub + text_size) { fprintf(stderr, "[DIAG]  libunity+0x%lx\n", v - ub); hits++; }
    else if (ib && v >= ib && v < ib + 0x3000000) { fprintf(stderr, "[DIAG]  libil2cpp+0x%lx\n", v - ib); hits++; }
  }
  fprintf(stderr, "[DIAG] --- fim (%d hits) ---\n", hits); fsync(2);
}
static long (*fmod_alloc2_orig)(long, long, long, long, long, long, long, long);
static long fmod_alloc2_hook(long a, long b, long c, long d, long e, long f, long g, long h) {
  if ((unsigned long)b > FMOD_ALLOC_SANE) {
    static int n; if (n++ < 6)
      fprintf(stderr, "[FMODGUARD] alloc2 insana %lu (file=%s line=%ld) -> NULL (flange skip)\n",
              (unsigned long)b, (c && *(char *)c) ? (char *)c : "?", d);
    fsync(2);
    return 0;
  }
  return fmod_alloc2_orig(a, b, c, d, e, f, g, h);
}
/* ---- TER_AUDIOSPY: espião do createSound do Unity-FMOD (wrapper @0x806cb4) ----
 * RE: AudioClip::CreateFMODSound (0x3cf...) chama System::createSound(sys, data, mode, exinfo,
 * &sound) = 0x806cb4 -> impl real 0x7bcf98. O log "Cannot create FMOD::Sound ... INTERNAL"
 * (code 33) vem daqui. Spy loga mode/exinfo/result p/ ver QUAIS sons falham (SFX sample vs
 * Music stream) e o código de erro exato. Gated; default OFF. */
static long (*cs_orig)(void *, void *, int, void *, void *);
/* TER_AUDIOSPY: hook do MIXER (0x805a94) — ground-truth de count(x2) + formato real do
   system ([output+0x60]). Loga 3× e segue. */
static long (*mix_orig)(void *, void *, int);
static long mix_hook(void *output, void *buf, int count) {
  static int n;
  if (n++ < 3) {
    void *sys = (output && addr_readable((uintptr_t)output + 0x60)) ? *(void **)((char *)output + 0x60) : NULL;
    if (sys && addr_readable((uintptr_t)sys + 0x800))
      fprintf(stderr, "[MIXSPY] count(x2)=%d output=%p sys=%p | 7c4=%u 7c8=%u 7d4=%u 7f4=%u 7f8=%u 97e8=%u\n",
              count, output, sys,
              *(uint32_t *)((char *)sys + 0x7c4), *(uint32_t *)((char *)sys + 0x7c8),
              *(uint32_t *)((char *)sys + 0x7d4), *(uint32_t *)((char *)sys + 0x7f4),
              *(uint32_t *)((char *)sys + 0x7f8), *(uint32_t *)((char *)sys + 0x97e8));
    dbg_sync();
  }
  return mix_orig(output, buf, count);
}
static int g_stream_fallback;
static long cs_hook(void *sys, void *data, int mode, void *exinfo, void *out) {
  long r = cs_orig(sys, data, mode, exinfo, out);
  static int nfail, nok;
  int openmem = (mode & 0x800) || (mode & 0x10000000);
  /* TER_STREAMFALLBACK: o open de STREAM (0x864f78) falha INTERNAL(33) no so-loader
     (maquinaria de stream/stream-thread). Refaz como SAMPLE não-streamado (mesma fonte
     via file-callback do Unity) -> a música carrega inteira na memória e toca. SFX (sem
     bit 0x80) não passam por aqui. */
  if (r != 0 && (mode & 0x80) && g_stream_fallback) {
    int m2 = mode & ~0x80;               /* tira CREATESTREAM */
    long r2 = cs_orig(sys, data, m2, exinfo, out);
    static int nf; if (nf++ < 8)
      fprintf(stderr, "[CSSPY] STREAM falhou(%ld) -> retry sample mode=0x%x -> %ld\n", r, (unsigned)m2, r2);
    dbg_sync();
    if (r2 == 0) return 0;
    r = r2;
  }
  if (r != 0) {  /* FALHA: dump detalhado do data source */
    if (nfail++ < 40) {
      char nm[80]; nm[0] = 0;
      if (data && !openmem && addr_readable((uintptr_t)data)) {
        const char *s = (const char *)data; int ok = 1;
        for (int i = 0; i < 70; i++) { char c = s[i]; if (!c) break; if (c < 9 || (unsigned char)c > 126) { ok = 0; break; } }
        if (ok) snprintf(nm, sizeof nm, "\"%.70s\"", s);
      }
      unsigned long d0 = (data && addr_readable((uintptr_t)data)) ? *(unsigned long *)data : 0;
      /* exinfo dump: cbsize@0 length@4 numchannels@? — campos crus p/ diagnóstico */
      fprintf(stderr, "[CSSPY] FAIL createSound mode=0x%x stream=%d openmem=%d data=%p name=%s d[0]=0x%lx exinfo=%p -> %ld\n",
              (unsigned)mode, (mode >> 7) & 1, openmem, data, nm, d0, exinfo, r);
      if (exinfo && addr_readable((uintptr_t)exinfo + 0x40))
        fprintf(stderr, "[CSSPY]   exinfo: cb=%u len=%u +8=%u +c=%u +10=%u +14=%u +18=%u +1c=0x%x +20=0x%lx +28=0x%lx\n",
                *(uint32_t *)((char *)exinfo + 0), *(uint32_t *)((char *)exinfo + 4),
                *(uint32_t *)((char *)exinfo + 8), *(uint32_t *)((char *)exinfo + 0xc),
                *(uint32_t *)((char *)exinfo + 0x10), *(uint32_t *)((char *)exinfo + 0x14),
                *(uint32_t *)((char *)exinfo + 0x18), *(uint32_t *)((char *)exinfo + 0x1c),
                *(unsigned long *)((char *)exinfo + 0x20), *(unsigned long *)((char *)exinfo + 0x28));
      dbg_sync();
    }
  } else if (nok++ < 4) {
    fprintf(stderr, "[CSSPY] ok createSound mode=0x%x stream=%d -> 0\n", (unsigned)mode, (mode >> 7) & 1);
  }
  return r;
}
/* ---- output do FMOD (AudioTrack Java) bombeado em C -> SDL -> Pulse/PipeWire/ALSA ----
 * RAIZ (RE libunity, fmodProcess @0x811378): a thread Java chamaria fmodProcess(ByteBuffer)
 * e escreveria no AudioTrack. fmodProcess: GetDirectBufferAddress -> mixa `blockSize` frames
 * (via 0x805a94) no buffer -> RETORNA 0 em SUCESSO (-1 se o output *0xc7c2f0 ainda é NULL).
 * BUG anterior: o pump só enfileirava se r>0; como fmodProcess SEMPRE retorna 0 no sucesso,
 * NUNCA mandávamos PCM (silêncio total mesmo com o mixer rodando). Fix: enfileirar quando
 * r==0; o nº de bytes preenchidos = blockSize*canais*2 (PCM interleaved s16), lido do struct
 * do FMOD System em runtime ([*(unity+0xc7c2f0)]+0x60 = system; +0x7f4=block, +0x7c4/+0x7c8=
 * rate/canais). Back-pressure (mantém ~6 blocos na fila do SDL) -> ritmo = consumo real-time;
 * fmodProcess É o clock do mixer (mixa só quando chamado). Backend SDL = auto (pulse/pipewire/
 * alsa) via device NULL — portável a qualquer device. */
static unsigned g_fmod_blk, g_fmod_rate, g_fmod_ch;
static int fmod_read_format(unsigned *blk, unsigned *rate, unsigned *ch, void **sysout) {
  uintptr_t outp = g_unity_base + 0xc7c2f0;
  if (!addr_readable(outp)) return 0;
  void *out = *(void **)outp;
  if (!out || !addr_readable((uintptr_t)out + 0x60)) return 0;
  void *sys = *(void **)((char *)out + 0x60);
  if (!sys || !addr_readable((uintptr_t)sys + 0x7f8)) return 0;
  if (blk)  *blk  = *(uint32_t *)((char *)sys + 0x7f4);
  /* RAIZ do "acelerado": a taxa do PCM entregue ao AudioTrack é [system+0x7d4]
     (= o que fmodGetInfo @0x8112b0 reporta como samplerate), NÃO [+0x7c4]. O +0x7c4
     é a taxa do device de saída; o mixer entrega no rate +0x7d4 (mobile costuma usar
     24000). Tocar 24000 como 44100 = ~1.8x rápido/agudo. */
  if (rate) *rate = *(uint32_t *)((char *)sys + 0x7d4);
  if (ch)   *ch   = *(uint32_t *)((char *)sys + 0x7c8);
  if (sysout) *sysout = sys;
  return 1;
}
static void *fmod_audio_thread(void *arg) {
  (void)arg;
  void *fp = NULL;
  while (g_fmod_run && !(fp = jni_find_native("fmodProcess"))) usleep(20000);
  if (!fp) return NULL;
  static long fdev = 0xFAD;            /* this (FMODAudioDevice) fake */
  void *bb = jni_fmod_bytebuffer();
  void *pcm = jni_fmod_pcm();
  int pcmcap = jni_fmod_pcm_size();
  /* 🔑 MEDIR EMPIRICAMENTE quantos bytes o fmodProcess ESCREVE por chamada. O offset da struct do
     System (0xc7c2f0) é não-confiável (fmod_read_format sempre falhava). Mas fmodProcess MIXA um
     nº FIXO de bytes (blockSize*ch*2) no buffer, independente da capacidade. Medimos: pré-enchemos
     o buffer com sentinela 0xAB, chamamos fmodProcess (silêncio = zeros ≠ 0xAB → detectável) e
     achamos o último byte escrito. Enfileirar EXATAMENTE esse nº = clock do mixer casa com o
     playback (senão acelera/atrasa). Tomamos o MÁX sobre várias chamadas (robusto a 0xAB no tail). */
  unsigned rate = 44100, ch = 2, blk = 0, measured = 0;
  unsigned char *pcmb = (unsigned char *)pcm;
  int got = 0;
  for (int t = 0; g_fmod_run && t < 4000; t++) {
    memset(pcmb, 0xAB, pcmcap);
    int r = ((int (*)(void *, void *, void *))fp)(g_fmod_env, &fdev, bb);
    if (r == 0) {
      int last = -1;
      for (int i = pcmcap - 1; i >= 0; i--) if (pcmb[i] != 0xAB) { last = i; break; }
      if (last >= 0) { unsigned wb = (unsigned)(last + 1); if (wb > measured) measured = wb; got++; }
      if (got >= 60) break;   /* ~60 amostras p/ um máximo estável */
    }
    usleep(2000);
  }
  fprintf(stderr, "[AUDIO] MEDIDO: fmodProcess escreve ~%u bytes/chamada (amostras=%d, cap=%d)\n", measured, got, pcmcap);
  /* 🔑🔑 GROUND TRUTH do formato: fmodGetInfo(env,thiz,infoType) @libunity+0x8112b0 é a fn que o FMOD
     expõe p/ o Java montar o AudioTrack. Tipos: 0=SAMPLERATE, 1=blockSize(frames), 4=CHANNELS.
     RAIZ DO ÁUDIO RÁPIDO: o FMOD mixa a **24000 Hz** (mobile), mas o SDL estava a 44100 →
     44100/24000 = 1.84× acelerado. FIX = abrir o SDL na taxa/canais REAIS do fmodGetInfo. */
  { int (*fgi)(void*, void*, int) = (void *)(g_unity_base + 0x8112b0);
    for (int it = 0; it < 5; it++)
      fprintf(stderr, "[AUDIO] fmodGetInfo(%d) = %d\n", it, fgi(g_fmod_env, &fdev, it));
    int r0 = fgi(g_fmod_env, &fdev, 0), c4 = fgi(g_fmod_env, &fdev, 4);
    if (r0 >= 8000 && r0 <= 192000) rate = (unsigned)r0;     /* taxa real do mixer FMOD */
    if (c4 == 1 || c4 == 2) ch = (unsigned)c4;               /* canais reais */
  }
  if (getenv("TER_AUDIO_RATE")) rate = atoi(getenv("TER_AUDIO_RATE"));
  if (getenv("TER_AUDIO_CH"))   ch   = atoi(getenv("TER_AUDIO_CH"));
  if (rate < 8000 || rate > 192000) rate = 44100;
  if (ch != 1 && ch != 2) ch = 2;
  unsigned bytes;
  if (getenv("TER_AUDIO_FRAMES")) bytes = (unsigned)atoi(getenv("TER_AUDIO_FRAMES")) * ch * 2;
  else if (measured >= 256) bytes = (measured / (ch * 2)) * (ch * 2);   /* alinha ao frame */
  else bytes = 4096;   /* fallback se a medição falhar */
  if ((int)bytes > pcmcap) bytes = pcmcap;
  blk = bytes / (ch * 2);
  g_fmod_rate = rate; g_fmod_ch = ch; g_fmod_blk = blk;
  SDL_AudioSpec want, have; memset(&want, 0, sizeof want);
  want.freq = rate; want.format = AUDIO_S16SYS; want.channels = ch; want.samples = 1024;
  if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
  SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  /* back-pressure: colchão p/ absorver jitter de scheduling (o jogo ocupa os 4 núcleos do Mali). 6
     blocos default = continuidade SEM engasgo, latência ~256ms (validado ~perfeito). Tunável
     TER_AUDIO_BP. PRÉ-ENCHE a fila antes de despausar (sem corte no arranque). */
  unsigned bpblocks = getenv("TER_AUDIO_BP") ? (unsigned)atoi(getenv("TER_AUDIO_BP")) : 6;
  if (bpblocks < 2) bpblocks = 2; if (bpblocks > 64) bpblocks = 64;
  Uint32 target = bytes * bpblocks;
  unsigned long n = 0, fed = 0;
  for (unsigned i = 0; dev && i < bpblocks && g_fmod_run; i++) {   /* pré-enche */
    int r = ((int (*)(void *, void *, void *))fp)(g_fmod_env, &fdev, bb);
    if (r == 0) { SDL_QueueAudio(dev, pcm, bytes); fed += bytes; } else { usleep(3000); }
  }
  if (dev) SDL_PauseAudioDevice(dev, 0);
  fprintf(stderr, "[AUDIO] SDL dev=%d rate=%u ch=%u blk=%u bytes=%u bp=%u (have f=%d c=%d) err=%s\n",
          dev, rate, ch, blk, bytes, bpblocks, have.freq, have.channels, dev ? "" : SDL_GetError());
  while (g_fmod_run) {
    if (dev && SDL_GetQueuedAudioSize(dev) > target) { usleep(1000); continue; }
    int r = ((int (*)(void *, void *, void *))fp)(g_fmod_env, &fdev, bb);
    if (r == 0 && dev) { SDL_QueueAudio(dev, pcm, bytes); fed += bytes; }
    else if (r < 0) usleep(5000);      /* output ainda não pronto */
    if (n < 5 || n % 2000 == 0) {
      fprintf(stderr, "[AUDIO] fmodProcess #%lu -> %d (fed=%lu q=%u)\n",
              n, r, fed, dev ? SDL_GetQueuedAudioSize(dev) : 0); dbg_sync(); }
    n++;
  }
  return NULL;
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
  g_main_tid = (int)syscall(SYS_gettid);  /* p/ o sem_shim distinguir main de workers */
  if (getenv("CUP_SEMPOLL")) sh_sem_set_poll(atoi(getenv("CUP_SEMPOLL")));  /* polling do sem_wait */
  if (getenv("TER_FUTEXPOLL")) { g_futexpoll_ms = atoi(getenv("TER_FUTEXPOLL")); fprintf(stderr, "[FUTEXPOLL] %ldms (timeout em FUTEX_WAIT sem timeout)\n", g_futexpoll_ms); }
  { extern void cond_set_poll(int);  /* polling do pthread_cond_wait (lost-wakeup futex) */
    if (getenv("CUP_CONDPOLL")) cond_set_poll(atoi(getenv("CUP_CONDPOLL"))); }

  /* log persistente: stderr -> debug.log (unbuffered + fsync nos marcos =
     sobrevive a hang/power-cycle do device). CUP_NOLOGFILE=1 desativa. */
  if (!getenv("CUP_NOLOGFILE")) {
    int lf = open(ASSET_BASE_M "debug.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (lf >= 0) {
      printf("stderr -> " ASSET_BASE_M "debug.log\n");
      dup2(lf, 2); if (lf != 2) close(lf);
    }
  }

  /* valida que tpidr_el0+0x28 (canary bionic) caiu DENTRO do nosso pad TLS */
  { uintptr_t tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    uintptr_t slot = tp + 0x28, lo = (uintptr_t)g_bionic_guard_pad;
    fprintf(stderr, "TLS guard: tpidr=0x%lx slot+0x28=0x%lx pad=[0x%lx..0x%lx] %s (val=0x%lx)\n",
            (unsigned long)tp, (unsigned long)slot, (unsigned long)lo,
            (unsigned long)(lo + sizeof(g_bionic_guard_pad)),
            (slot >= lo && slot + 8 <= lo + sizeof(g_bionic_guard_pad))
                ? "DENTRO ok" : "FORA (canary instavel!)",
            *(unsigned long *)slot);
  }
  /* sigaltstack: p/ o handler reportar STACK OVERFLOW (SIGSEGV na guard page →
     sem espaço na pilha normal p/ rodar o handler → morte silenciosa). */
  g_skipbad = getenv("CUP_SKIPBAD") ? 1 : 0;
  /* CUP_GCSIG: Boehm GC suspende threads via SIGPWR(30)/restart SIGXCPU(24) p/
     stop-the-world. Nossas threads (criadas via pthread_create_fake, fora do
     registro do GC) recebem SIGPWR com ação DEFAULT = mata o processo (exit 158).
     Instalamos handlers que implementam o protocolo (suspende+espera restart) p/
     a thread não morrer. (my_sigaction bloqueia o engine de sobrescrever.) */
  if (getenv("CUP_GCSIG")) {
    extern void gc_suspend_handler(int), gc_restart_handler(int);
    struct sigaction sp; memset(&sp, 0, sizeof sp);
    sp.sa_handler = gc_suspend_handler; sigfillset(&sp.sa_mask);
    sigdelset(&sp.sa_mask, SIGXCPU); sigdelset(&sp.sa_mask, SIGSEGV);
    sigaction(SIGPWR, &sp, 0);
    struct sigaction sr; memset(&sr, 0, sizeof sr);
    sr.sa_handler = gc_restart_handler; sigemptyset(&sr.sa_mask);
    sigaction(SIGXCPU, &sr, 0);
    fprintf(stderr, "[GCSIG] handlers SIGPWR(suspend)+SIGXCPU(restart) instalados\n");
  }
  { static char altstk[256 * 1024]; stack_t ss = {0};
    ss.ss_sp = altstk; ss.ss_size = sizeof altstk; ss.ss_flags = 0;
    sigaltstack(&ss, NULL); }
  struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_crash; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGABRT, &sa, 0);
  sigaction(SIGILL, &sa, 0); sigaction(SIGFPE, &sa, 0);
  sigaction(SIGTRAP, &sa, 0); sigaction(SIGSYS, &sa, 0);  /* BRK/seccomp matam calado */
  { struct sigaction sd; memset(&sd, 0, sizeof sd); sd.sa_sigaction = diag_handler;
    sd.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART; sigaction(SIGUSR1, &sd, 0); }

  fprintf(stderr, "=== ROCKMAN X DiVE Offline Unity 2020.3 IL2CPP (arm64 GLES2) so-loader ===\n");

  /* GL/EGL/z visíveis p/ dlsym(RTLD_DEFAULT) do Unity */
  dlopen("libz.so.1", RTLD_NOW | RTLD_GLOBAL);
  void *g = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL); if (!g) dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
  void *e = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL); if (!e) dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
  fprintf(stderr, "[libs] z/GLESv2/EGL dlopen (glClear=%p)\n", dlsym(RTLD_DEFAULT, "glClear"));

  /* ---- F0: carrega libunity.so ---- */
  size_t hs = (size_t)HEAP_MB * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) { perror("mmap"); return 1; }
  fprintf(stderr, "[F0] heap %dMB @ %p, carregando libunity.so...\n", HEAP_MB, heap);
  if (so_load("libunity.so", heap, hs) < 0) { fprintf(stderr, "so_load libunity FALHOU\n"); return 1; }
  fprintf(stderr, "[F0] libunity: text=%p+%zu data=%p+%zu\n", text_base, text_size, data_base, data_size);
  g_unity_data = (uintptr_t)data_base;
  if (so_relocate() < 0) { fprintf(stderr, "relocate FALHOU\n"); return 1; }

  /* overrides */
  g_unity_base = (uintptr_t)text_base;
  set_import("abort", (void *)my_abort);
  set_import("raise", (void *)my_raise);
  set_import("tgkill", (void *)my_tgkill);
  set_import("exit", (void *)my_exit);
  set_import("_exit", (void *)my_exit);
  /* __stack_chk_fail nao esta na tabela de imports -> patch direto na GOT (apos resolve) */
  set_import("glGetString", (void *)my_glGetString);
  ds_init();  /* CUP_DRAWSPY: ring de draws + watchdog (intercepta via eglGetProcAddress) */
  if (getenv("CUP_EGPLOG") || getenv("CUP_NOVAO") || g_drawspy || getenv("CUP_DRAWCOUNT"))
    set_import("eglGetProcAddress", (void *)my_eglGetProcAddress);
  set_import("sysconf", (void *)my_sysconf);
  /* 🔑 dl_iterate_phdr: libil2cpp importa e o resolvia p/ o STUB (retorna 0) → o unwinder C++ da
     libgcc não acha o .eh_frame de libunity/libil2cpp → exceção C++ real (ex.: shader/currentActivity)
     vira `std::terminate`→abort em vez de ser capturada. Wira o REAL (itera g_so_mods). */
  { extern int dl_iterate_phdr(int (*)(struct dl_phdr_info *, size_t, void *), void *);
    set_import("dl_iterate_phdr", (void *)dl_iterate_phdr); }
  if (getenv("TER_JOBINLINE")) {
    set_import("sched_getaffinity", (void *)my_sched_getaffinity);
    fprintf(stderr, "[JOBINLINE] sched_getaffinity -> 1 CPU (job system roda inline)\n");
  }
  g_mmaplog = getenv("CUP_MMAPLOG") ? 1 : 0;
  g_guidlog = getenv("TER_GUIDLOG") ? 1 : 0;
  set_import("mmap", (void *)my_mmap);
  set_import("mmap64", (void *)my_mmap);
  if (g_guidlog) {
    set_import("read", (void *)my_read);
    set_import("lseek64", (void *)my_lseek64);
    set_import("fstat64", (void *)my_fstat64);
    set_import("mmap64", (void *)my_mmap64);
    set_import("fdopen", (void *)my_fdopen);
  }
  set_import("fopen", (void *)my_fopen);
  if (getenv("TER_NO_FCLOSE")) set_import("fclose", (void *)my_fclose);
  set_import("open", (void *)my_open);
  set_import("stat", (void *)my_stat);
  set_import("lstat", (void *)my_lstat);
  set_import("stat64", (void *)my_stat64);
  set_import("lstat64", (void *)my_lstat64);
  /* 🔑 fstat64: glibc≥2.30 NÃO exporta `fstat64` p/ dlsym → passthrough falha → slot
     GOT NULL. libunity chama fstat64 no RecreateGfxState (kmsdrm/G31) → salta p/ NULL
     (pc=0). my_fstat64 chama o fstat64 REAL (linka na buster glibc 2.28). Incondicional
     (antes só com TER_GUIDLOG). Mesma classe do stat/lstat da lição DYSMANTLE. */
  set_import("fstat64", (void *)my_fstat64);
  set_import("access", (void *)my_access);
  set_import("statfs64", (void *)my_statfs64);
  set_import("statfs", (void *)my_statfs64);
  set_import("strlcpy", (void *)my_strlcpy);
  set_import("strlcat", (void *)my_strlcat);
  set_import("memalign", (void *)my_memalign);
  set_import("syscall", (void *)my_syscall);
  set_import("pthread_kill", (void *)my_pthread_kill);
  set_import("__memmove_chk", (void *)my_memmove_chk);
  set_import("__memcpy_chk", (void *)my_memcpy_chk);
  set_import("__memset_chk", (void *)my_memset_chk);
  set_import("__strlen_chk", (void *)my_strlen_chk);
  set_import("__strcpy_chk", (void *)my_strcpy_chk);
  set_import("__strcat_chk", (void *)my_strcat_chk);
  set_import("__vsnprintf_chk", (void *)my_vsnprintf_chk);
  set_import("__snprintf_chk", (void *)my_snprintf_chk);
  set_import("__FD_SET_chk", (void *)my_FD_SET_chk);
  set_import("__system_property_get", (void *)my_sysprop);
  set_import("__android_log_print", (void *)my_alog_print);
  set_import("__android_log_write", (void *)my_alog_write);
  set_import("sigaction", (void *)my_sigaction);
  set_import("dlopen", (void *)my_dlopen);
  set_import("dlsym", (void *)my_dlsym);
  set_import("dlerror", (void *)my_dlerror);
  set_import("dladdr", (void *)my_dladdr);
  set_import("dlclose", (void *)my_dlclose);
  set_import("pthread_key_create", (void *)sh_key_create);
  set_import("pthread_key_delete", (void *)sh_key_delete);
  set_import("pthread_getspecific", (void *)sh_getspecific);
  set_import("pthread_setspecific", (void *)sh_setspecific);
  set_import("ANativeWindow_fromSurface", (void *)my_aw_fromSurface);
  set_import("ANativeWindow_setBuffersGeometry", (void *)my_aw_setgeom);
  set_import("ANativeWindow_getWidth", (void *)my_aw_getWidth);
  set_import("ANativeWindow_getHeight", (void *)my_aw_getHeight);
  set_import("ANativeWindow_getFormat", (void *)my_aw_getFormat);
  set_import("ANativeWindow_acquire", (void *)my_aw_noop);
  set_import("ANativeWindow_release", (void *)my_aw_noop);
  if (!cup_use_kmsdrm())
    set_import("eglChooseConfig", (void *)my_eglChooseConfig_fbdev);
  set_android_import_overrides();

  /* alias bionic->glibc: __errno (bionic) = __errno_location (glibc) */
  { void *el = dlsym(RTLD_DEFAULT, "__errno_location");
    if (el) set_import("__errno", el); }

  install_sem_shim();  /* semáforos próprios bionic→glibc (fix deadlock boot) */
  install_pthread_shim();  /* mutex/cond/rwlock bionic->glibc (fix SIGBUS cond_wait) */

  fprintf(stderr, "[F0] resolvendo %zu imports...\n", dynlib_numfunctions);
  { extern void recon_fill_passthrough(void); recon_fill_passthrough(); }  /* preenche passthrough via dlsym (tabela gerada) */
  if (so_resolve(dynlib_functions, dynlib_numfunctions, 0) < 0) { fprintf(stderr, "resolve FALHOU\n"); return 1; }
  ctype_init(); ctype_resolve();   /* _ctype_/_tolower_tab_/_toupper_tab_ (bionic) p/ libunity */
  so_record_phdr("libunity.so");   /* p/ o dl_iterate_phdr custom (unwind de exceções C++) */
  if (so_register_eh_frame() == 0) fprintf(stderr, "[EH] .eh_frame libunity registrado (exceções C++)\n");
  /* PATCH-GOT: os imports NDK nao estao em dynlib_functions -> set_import foi
   * no-op e ficaram UNRESOLVED (GOT lixo). Sobrescreve os slots DIRETO. */
  patch_got("ANativeWindow_fromSurface", (void *)my_aw_fromSurface);
  patch_got("ANativeWindow_setBuffersGeometry", (void *)my_aw_setgeom);
  patch_got("ANativeWindow_getWidth", (void *)my_aw_getWidth);
  patch_got("ANativeWindow_getHeight", (void *)my_aw_getHeight);
  patch_got("ANativeWindow_getFormat", (void *)my_aw_getFormat);
  patch_got("ANativeWindow_acquire", (void *)my_aw_noop);
  patch_got("ANativeWindow_release", (void *)my_aw_noop);
  if (!cup_use_kmsdrm())
    patch_got("eglChooseConfig", (void *)my_eglChooseConfig_fbdev);
  patch_got("__android_log_print", (void *)my_alog_print);
  patch_got("__android_log_write", (void *)my_alog_write);
  patch_got("__android_log_vprint", (void *)my_alog_vprint);
  if (getenv("CUP_EGPLOG") || getenv("CUP_NOVAO") || g_drawspy || getenv("CUP_DRAWCOUNT"))
    patch_got("eglGetProcAddress", (void *)my_eglGetProcAddress);
  /* CUP_RENDERSCALE: interpõe eglSwapBuffers p/ dar upscale do FBO lo-res antes do swap */
  if (rs_enabled() || getenv("TER_SHOT") || getenv("TER_SHOTLIVE") || getenv("TER_SWAPLOG") ||
      getenv("TER_NUKEKB") || getenv("TER_JOBWORKERS0"))
    patch_got("eglSwapBuffers", (void *)my_eglSwapBuffers);
  /* dl* estavam COMENTADOS em imports.gen.c -> set_import foi no-op e o dlopen@plt
     caiu no glibc REAL (falha ao carregar .so Android). Sem isso o il2cpp nao carrega. */
  patch_got("dlopen", (void *)my_dlopen);
  patch_got("dlsym", (void *)my_dlsym);
  patch_got("dlerror", (void *)my_dlerror);
  patch_got("dlclose", (void *)my_dlclose);
  patch_got("dladdr", (void *)my_dladdr);
  /* engine checa existência dos arquivos de dados antes de abrir */
  patch_got("open", (void *)my_open);
  patch_got("fopen", (void *)my_fopen);
  if (getenv("TER_NO_FCLOSE")) patch_got("fclose", (void *)my_fclose);
  patch_got("stat", (void *)my_stat);
  patch_got("lstat", (void *)my_lstat);
  patch_got("stat64", (void *)my_stat64);
  patch_got("lstat64", (void *)my_lstat64);
  patch_got("fstat64", (void *)my_fstat64);  /* incondicional: dlsym falha na glibc≥2.30 */
  patch_got("access", (void *)my_access);
  patch_got("statfs64", (void *)my_statfs64);
  patch_got("statfs", (void *)my_statfs64);
  patch_got("strlcpy", (void *)my_strlcpy);
  patch_got("strlcat", (void *)my_strlcat);
  patch_got("memalign", (void *)my_memalign);
  patch_got("syscall", (void *)my_syscall);
  patch_got("pthread_kill", (void *)my_pthread_kill);
  patch_got("memalign", (void *)my_memalign);
  patch_got("syscall", (void *)my_syscall);
  patch_got("pthread_kill", (void *)my_pthread_kill);
  patch_got("__memmove_chk", (void *)my_memmove_chk);
  patch_got("__memcpy_chk", (void *)my_memcpy_chk);
  patch_got("__memset_chk", (void *)my_memset_chk);
  patch_got("__strlen_chk", (void *)my_strlen_chk);
  patch_got("__strcpy_chk", (void *)my_strcpy_chk);
  patch_got("__strcat_chk", (void *)my_strcat_chk);
  patch_got("__vsnprintf_chk", (void *)my_vsnprintf_chk);
  patch_got("__snprintf_chk", (void *)my_snprintf_chk);
  patch_got("__FD_SET_chk", (void *)my_FD_SET_chk);
  patch_got("exit", (void *)my_exit);
  patch_got("_exit", (void *)my_exit);
  if (g_guidlog) {
    patch_got("read", (void *)my_read);
    patch_got("lseek64", (void *)my_lseek64);
    patch_got("fstat64", (void *)my_fstat64);
    patch_got("mmap64", (void *)my_mmap64);
    patch_got("fdopen", (void *)my_fdopen);
  }
  patch_sem_shim();  /* sem_* nos slots GOT do libunity */
  patch_pthread_shim();
  /* sensores/looper/profiler google: stub no-op (nao usados no path do gfx) */
  const char *ndk_noop[] = {
    "ALooper_forThread","ALooper_prepare","ASensorManager_getInstance",
    "ASensorManager_createEventQueue","ASensorManager_getSensorList",
    "ASensorManager_getDefaultSensor","ASensorManager_destroyEventQueue",
    "ASensorEventQueue_hasEvents","ASensorEventQueue_getEvents",
    "ASensorEventQueue_enableSensor","ASensorEventQueue_disableSensor",
    "ASensorEventQueue_setEventRate","ASensor_getType","ASensor_getResolution",
    "ASensor_getMinDelay","ASensor_getName","ASensor_getVendor",
    "__google_potentially_blocking_region_begin",
    "__google_potentially_blocking_region_end", NULL };
  for (int i = 0; ndk_noop[i]; i++) patch_got(ndk_noop[i], (void *)ndk_stub0);
  patch_android_import_overrides();

  /* TER: bypass do "Not enough storage space to install required resources".
   * RE (libunity): em 0x2d8fac `tbz w0,#0, 0x2d9068` — se a checagem de espaço/resources
   * (0x22b7e0) retorna falso, pula pro bloco que monta o AlertDialog (string 0x9288ef).
   * Esse bloco SÓ é alcançável por esse branch. NOP -> sempre segue o caminho de sucesso
   * (dados já estão em bin/Data, lidos via AssetManager). */
  if (getenv("RXD_STORAGEPATCH")) {
    extern void so_make_text_writable(void), so_make_text_executable(void);
    so_make_text_writable();
    *(uint32_t *)((uintptr_t)text_base + 0x2d8fac) = 0xd503201fu; /* NOP */
    so_make_text_executable(); so_flush_caches();
    fprintf(stderr, "[RXD] storage-check patch opt-in aplicado em libunity+0x2d8fac\n");
  }

  /* O FIX REAL do null-deref do Enlighten é o `memalign` (acima, deixou de ser stub).
     Este patch do wrapper 0x861928 -> my_enl_alloc é uma REDE DE SEGURANÇA opcional
     (fallback malloc se o allocator real devolver NULL por qualquer motivo). OPT-IN via
     TER_ENLFIX (default OFF — memalign sozinho já resolve). TER_ENLLOG liga log por-alloc. */
  g_enllog = getenv("TER_ENLLOG") ? 1 : 0;
  if (getenv("TER_ENLFIX")) {
    patch_tramp(0x861928, (void *)my_enl_alloc);
    fprintf(stderr, "[ENL] alloc-wrapper 0x861928 -> my_enl_alloc (rede de segurança)\n");
  }

  /* CUP_FORCEIL2: o helper "load library by name" do Unity (0x357938) faz o
     System.load do il2cpp via JNI -> falha no nosso ambiente ("Failed to load
     Il2CPP"). Mas NOS ja' carregamos libil2cpp.so no F1. Forca retorno 1 (sucesso):
       mov w0,#1 ; ret  */
  if (getenv("CUP_FORCEIL2")) {
    *(uint32_t *)((uintptr_t)text_base + 0x357938) = 0x52800020u; /* mov w0,#1 */
    *(uint32_t *)((uintptr_t)text_base + 0x35793c) = 0xd65f03c0u; /* ret */
    fprintf(stderr, "[FORCEIL2] 0x357938 -> mov w0,#1; ret\n");
  }
  /* CUP_NOEXTRACT: a extracao de recursos do APK (0x94184c) copia de um VFS source
     (o APK) que nao temos -> falha ("Failed to extract resources"). Mas os assets
     JA estao deployados em bin/Data/. Forca a extracao reportar sucesso. */
  if (getenv("CUP_NOEXTRACT")) {
    *(uint32_t *)((uintptr_t)text_base + 0x94184c) = 0x52800020u; /* mov w0,#1 */
    *(uint32_t *)((uintptr_t)text_base + 0x941850) = 0xd65f03c0u; /* ret */
    fprintf(stderr, "[NOEXTRACT] 0x94184c -> mov w0,#1; ret\n");
  }

  so_finalize(); so_flush_caches();
  g_alloc_ub = (uintptr_t)text_base;
  if (env_on("CUP_DLLOG")) g_dllog = 1;

  /* __stack_chk_fail nao esta na tabela -> patch direto no slot da GOT */
  if (getenv("CUP_NOSCF")) {
    extern uintptr_t so_find_rel_addr_safe(const char *);
    uintptr_t got = so_find_rel_addr_safe("__stack_chk_fail");
    if (got) { *(uintptr_t *)got = (uintptr_t)my_stack_chk_fail;
      fprintf(stderr, "[SCF] GOT __stack_chk_fail @ 0x%lx patcheado\n", got); }
    else fprintf(stderr, "[SCF] __stack_chk_fail nao achado na GOT\n");
  }

  /* DIAGNÓSTICO: a main fica presa num loop (libunity 0x873a90) esperando a
     função 0x8739c4 (fila do GfxDevice [+224]/[+256]) zerar — deadlock do
     threaded rendering no Mali. CUP_NOGFXWAIT patcha 0x8739c4 p/ retornar 0
     (não espera) e ver se o jogo avança. */
  if (getenv("CUP_NOGFXWAIT")) {
    extern void so_make_text_writable(void), so_make_text_executable(void);
    so_make_text_writable();
    *(uint32_t *)((uintptr_t)text_base + 0x8739c4) = 0x52800000u; /* mov w0,#0 */
    *(uint32_t *)((uintptr_t)text_base + 0x8739c8) = 0xd65f03c0u; /* ret */
    so_make_text_executable(); so_flush_caches();
    fprintf(stderr, "[NOGFXWAIT] 0x8739c4 -> mov w0,#0; ret\n");
  }

  /* CUP_PSPY: substitui HasPendingOperations (0x8739c4) pela réplica C que
     captura o ponteiro do PreloadManager (diagnóstico do muro frame 111).
     CUP_GCEVERY também precisa do mgr (gate de ociosidade da limpeza). */
  if (getenv("CUP_PSPY") || getenv("CUP_GCEVERY")) {
    extern void so_make_text_writable(void), so_make_text_executable(void);
    so_make_text_writable();
    hook_arm64((uintptr_t)text_base + UN_HASPEND, (uintptr_t)my_haspending);
    so_make_text_executable(); so_flush_caches();
    fprintf(stderr, "[PSPY] hook HasPendingOperations (0x%x) instalado\n", UN_HASPEND);
  }
  rxd_install_preload_null_guard((uintptr_t)text_base);
  /* CUP_FORCEINTEG: dentro de IntegrateOp (0x872758), o gate (0x871844, budget
     time-slice) é checado em 0x872774 `tbz w0,#0, 872810` → se budget recusa,
     integração aborta retornando 0. Em WaitForAll (force-complete) o budget
     DEVERIA ser ignorado. NOP nesse branch: o gate ainda RODA (efeitos colaterais
     do predictor preservados), só o VEREDITO é ignorado → integração prossegue.
     Cirúrgico (só o path de integração; não mexe no gate compartilhado). */
  if (getenv("CUP_FORCEINTEG")) {
    extern void so_make_text_writable(void), so_make_text_executable(void);
    so_make_text_writable();
    /* ⚠️ 0x872774 (IntegrateOp) bypassa o VEREDITO INTEIRO do gate, incl. jobs-pending
       -> integra com job do worker pendente = RACE = objeto malformado (crash $PC=9 na
       desserializacao do CupheadCore). CUP_NO872774 pula este (mantem so o 0x871854 que
       bypassa SO o budget e PRESERVA a espera do job). */
    if (!getenv("CUP_NO872774"))
      *(uint32_t *)((uintptr_t)text_base + 0x872774) = 0xd503201fu; /* NOP (IntegrateOp 0x872758) */
    /* + ops cujo integrate É o gate 0x871844 DIRETO (materiais/shaders): NOP no branch
       0x871854 `tbz w0,#0, 87186c` que aborta no veredito do budget -> cai no check de
       jobs (passa qdo jobmgr=0). Cobre as 52 ops de shader/material presas (sessão 8). */
    if (!getenv("CUP_NOGATE854"))
      *(uint32_t *)((uintptr_t)text_base + 0x871854) = 0xd503201fu; /* NOP */
    so_make_text_executable(); so_flush_caches();
    fprintf(stderr, "[FORCEINTEG] 0x872774 + 0x871854 (gate budget-fail branches) -> NOP\n");
  }
  /* ===== CUP_SCENESKIP (default ON; CUP_NOSCENESKIP desliga) — RAIZ =====
   * Hook na ENTRADA de 0x541c9c: se a scene do GameObject ([arg0+56]) for NULL,
   * pula a função INTEIRA (não monta o mesh) em vez de cascatear nulls. Substitui
   * o antigo island fake-scene (que vazava e crashava downstream em 0x8f9b1c/b88/541e54). */
  if (0 /* RE Cuphead 2017.4 — offset inexistente no Terraria 2021.3 */) {
    void *trs = mk_tramp((uintptr_t)text_base + 0x541c9c, "scene541");
    if (trs) {
      scene541_orig = (long (*)(long, long, long, long, long, long, long, long))trs;
      extern void so_make_text_writable(void), so_make_text_executable(void);
      so_make_text_writable();
      hook_arm64((uintptr_t)text_base + 0x541c9c, (uintptr_t)scene541_hook);
      so_make_text_executable(); so_flush_caches();
      fprintf(stderr, "[SCENESKIP] hook 0x541c9c (skip GO se scene[arg0+56]==NULL)\n");
    } else {
      fprintf(stderr, "[SCENESKIP] mk_tramp falhou — guard OFF\n");
    }
  }
  /* CUP_MASKGUARD (default ON): clampa contagem insana em 0x8f9914 (mesh de
     SpriteMask/Tilemap do mapa) — 2º crash do load do mapa (0x8f9b1c). */
  if (0 /* RE Cuphead 2017.4 — offsets inexistentes no Terraria 2021.3 */) {
    void *tr = mk_tramp((uintptr_t)text_base + 0x8f9914, "maskfn");
    if (tr) {
      maskfn_orig = (long (*)(long, long, long, long, long, long, long, long))tr;
      extern void so_make_text_writable(void), so_make_text_executable(void);
      so_make_text_writable();
      hook_arm64((uintptr_t)text_base + 0x8f9914, (uintptr_t)maskfn_hook);
      so_make_text_executable(); so_flush_caches();
      fprintf(stderr, "[MASKGUARD] hook 0x8f9914 (clamp count [1,0x40000])\n");
    } else {
      fprintf(stderr, "[MASKGUARD] mk_tramp falhou — guard OFF\n");
    }
    /* NULLGUARD: 0x8f9b88 (tilemap, arg0 NULL no mapa) */
    void *trn = mk_tramp((uintptr_t)text_base + 0x8f9b88, "nullfn");
    if (trn) {
      nullfn_orig = (long (*)(long, long, long, long, long, long, long, long))trn;
      extern void so_make_text_writable(void), so_make_text_executable(void);
      so_make_text_writable();
      hook_arm64((uintptr_t)text_base + 0x8f9b88, (uintptr_t)nullfn_hook);
      so_make_text_executable(); so_flush_caches();
      fprintf(stderr, "[NULLGUARD] hook 0x8f9b88 (skip se arg0==NULL)\n");
    }
    /* DESERGUARD: 0x54220c (desserialização, *arg0 NULL no mapa) — crash #5 */
    void *trd = mk_tramp((uintptr_t)text_base + 0x54220c, "deser542");
    if (trd) {
      deser542_orig = (long (*)(long, long, long, long, long, long, long, long))trd;
      extern void so_make_text_writable(void), so_make_text_executable(void);
      so_make_text_writable();
      hook_arm64((uintptr_t)text_base + 0x54220c, (uintptr_t)deser542_hook);
      so_make_text_executable(); so_flush_caches();
      fprintf(stderr, "[DESERGUARD] hook 0x54220c (skip se *arg0==NULL)\n");
    }
  }
  /* TER_AUDIOSPY/TER_STREAMFALLBACK: hook do createSound (libunity 0x806cb4).
     SPY loga result de cada som; STREAMFALLBACK refaz streams falhos como sample.
     Instalado aqui (contexto libunity, text_base=libunity, ANTES do F1/il2cpp). */
  if (getenv("TER_AUDIOSPY") || getenv("TER_STREAMFALLBACK")) {
    g_stream_fallback = getenv("TER_STREAMFALLBACK") ? 1 : 0;
    void *tr = mk_tramp((uintptr_t)text_base + 0x806cb4, "createSound");
    if (tr) {
      cs_orig = (long (*)(void *, void *, int, void *, void *))tr;
      extern void so_make_text_writable(void), so_make_text_executable(void);
      so_make_text_writable();
      hook_arm64((uintptr_t)text_base + 0x806cb4, (uintptr_t)cs_hook);
      so_make_text_executable(); so_flush_caches();
      fprintf(stderr, "[CSSPY] hook createSound(0x806cb4) instalado (fallback=%d)\n", g_stream_fallback);
    } else fprintf(stderr, "[CSSPY] mk_tramp falhou\n");
    /* MIXSPY: ground-truth do mixer (count real + formato) p/ achar a causa do áudio rápido */
    if (getenv("TER_AUDIOSPY")) {
      void *trm = mk_tramp((uintptr_t)text_base + 0x805a94, "mixer");
      if (trm) {
        mix_orig = (long (*)(void *, void *, int))trm;
        extern void so_make_text_writable(void), so_make_text_executable(void);
        so_make_text_writable();
        hook_arm64((uintptr_t)text_base + 0x805a94, (uintptr_t)mix_hook);
        so_make_text_executable(); so_flush_caches();
        fprintf(stderr, "[MIXSPY] hook mixer(0x805a94) instalado\n");
      } else fprintf(stderr, "[MIXSPY] mk_tramp falhou\n");
    }
  }
  /* CUP_WAITGATE: FORCEINTEG cirúrgico — ignora o gate de budget SÓ dentro do
     WaitForAll (0x873a90). Hook do WaitForAll (flag in_waitall) + hook do gate
     (0x871844 → my_gate). NÃO combinar com CUP_FORCEINTEG. */
  if (getenv("CUP_WAITGATE")) {
    extern void so_make_text_writable(void), so_make_text_executable(void);
    g_waitall_cont = (uintptr_t)text_base + 0x873a90 + 16;
    so_make_text_writable();
    hook_arm64((uintptr_t)text_base + 0x873a90, (uintptr_t)my_waitall);
    hook_arm64((uintptr_t)text_base + 0x871844, (uintptr_t)my_gate);
    so_make_text_executable(); so_flush_caches();
    fprintf(stderr, "[WAITGATE] hook WaitForAll(0x873a90)+gate(0x871844); cont=0x%lx\n",
            (unsigned long)g_waitall_cont);
  }
  /* CUP_GATEWAIT: hook do gate (0x871844) SEMPRE-ativo — bypassa o budget (quebrado no
     so-loader) MAS faz spin-wait nos jobs do worker (sched_yield) antes de liberar a
     integração. Mata a race da integração forçada SEM os NOPs (0x872774/0x871854).
     NÃO combinar com CUP_FORCEINTEG. */
  if (getenv("CUP_GATEWAIT")) {
    extern void so_make_text_writable(void), so_make_text_executable(void);
    g_gatewait = 1;
    so_make_text_writable();
    hook_arm64((uintptr_t)text_base + 0x871844, (uintptr_t)my_gate);
    so_make_text_executable(); so_flush_caches();
    fprintf(stderr, "[GATEWAIT] hook gate(0x871844) sempre: bypass budget + spin-wait jobs\n");
  }
  /* ===== CUP_FORCESL (s14, default ON; CUP_NOFORCESL desliga) — SOM =====
   * O FMOD escolhe o output Android em 0x350298: retorna 2=OpenSL (setOutput 22) ou
   * 1=AudioTrack-Java (setOutput 21). Exige SDK>16 (0x3506b8 lê Build.VERSION.SDK_INT
   * via JNI, cache [0x128dcc0] — nosso shim devolve 0) + checks de low-latency → cai
   * SEMPRE no AudioTrack; sem JVM real o init falha → "null output" → SEM SOM (o
   * dlopen(libOpenSLES) era só probe; slCreateEngine nunca rodava). Forçamos retorno
   * 2 → init entra no slCreateEngine → opensles_shim → SDL2 (receita DYSMANTLE sdk=25). */
  if (0 /* RE Cuphead 2017.4 FMOD — offsets inexistentes no Terraria 2021.3 */) {
    extern void so_make_text_writable(void), so_make_text_executable(void);
    so_make_text_writable();
    hook_arm64((uintptr_t)text_base + 0x350298, (uintptr_t)forcesl_hook);
    /* guard do alocador do FMOD (default ON; CUP_NOFMODGUARD desliga) — mata o OOM
       fatal do flange. Dentro da janela WRITABLE (hook escreve no .text). */
    if (!getenv("CUP_NOFMODGUARD")) {
      struct { uintptr_t rva; void *hook; void **orig; const char *nm; } G[] = {
        {0xa66e6c, (void *)fmod_alloc_hook,  (void **)&fmod_alloc_orig,  "fmod.alloc"},
        {0xa66b74, (void *)fmod_alloc2_hook, (void **)&fmod_alloc2_orig, "fmod.alloc2"},
      };
      for (unsigned i = 0; i < sizeof G / sizeof G[0]; i++) {
        void *tr = mk_tramp((uintptr_t)text_base + G[i].rva, G[i].nm);
        if (!tr) { fprintf(stderr, "[FMODGUARD] tramp %s falhou\n", G[i].nm); continue; }
        *G[i].orig = tr;
        hook_arm64((uintptr_t)text_base + G[i].rva, (uintptr_t)G[i].hook);
      }
      fprintf(stderr, "[FMODGUARD] guard do alocador instalado\n");
    }
    if (getenv("CUP_FMODSPY")) {   /* dentro da janela WRITABLE (hook escreve no .text) */
      struct { uintptr_t rva; void *hook; void **orig; const char *nm; } T[] = {
        {0xa6281c, (void *)fmod_init_hook,   (void **)&fmod_init_orig,   "Sys::init"},
        {0xa6dbe0, (void *)fmod_oinit_hook,  (void **)&fmod_oinit_orig,  "osl.init"},
        {0xa6e270, (void *)fmod_ostart_hook, (void **)&fmod_ostart_orig, "osl.start"},
      };
      for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        void *tr = mk_tramp((uintptr_t)text_base + T[i].rva, T[i].nm);
        if (!tr) { fprintf(stderr, "[FMODSPY] tramp %s falhou\n", T[i].nm); continue; }
        *T[i].orig = tr;
        hook_arm64((uintptr_t)text_base + T[i].rva, (uintptr_t)T[i].hook);
      }
      fprintf(stderr, "[FMODSPY] hooks init/oinit/ostart instalados\n");
    }
    so_make_text_executable(); so_flush_caches();
    fprintf(stderr, "[FORCESL] hook 0x350298 -> 2 (FMOD output = OpenSL/shim)\n");
  }
  /* CUP_CLAMPSIG: clampa o count de Semaphore::Signal (0x65850c) p/ matar o storm
     (count deriva p/ enorme ~frame 110 → posta bilhões de vezes = livelock). */
  if (getenv("CUP_CLAMPSIG")) {
    extern void so_make_text_writable(void), so_make_text_executable(void);
    if (getenv("CUP_SIGCLAMP")) g_signal_clamp = atoi(getenv("CUP_SIGCLAMP"));
    g_signal_cont = (uintptr_t)text_base + 0x65850c + 16;
    so_make_text_writable();
    hook_arm64((uintptr_t)text_base + 0x65850c, (uintptr_t)my_signal);
    so_make_text_executable(); so_flush_caches();
    fprintf(stderr, "[CLAMPSIG] hook Signal(0x65850c) clamp=%d; cont=0x%lx\n",
            g_signal_clamp, (unsigned long)g_signal_cont);
  }

  fprintf(stderr, "[F0] init_array...\n");
  so_execute_init_array();
  fprintf(stderr, "[F0] libunity init OK\n");
  mm_probe("pos-init_array-unity");

  /* ---- JNI_OnLoad da libunity ---- */
  jni_shim_set_package("jp.co.capcom.rxdoff", 0);
  void *vm = NULL, *env = NULL; jni_shim_init(&vm, &env);
  uintptr_t onload = so_find_addr_safe("JNI_OnLoad");
  if (onload) {
    int ver = ((int (*)(void *, void *))onload)(vm, NULL);
    fprintf(stderr, "[F0] JNI_OnLoad = 0x%x\n", ver);
  } else {
    fprintf(stderr, "[F0] JNI_OnLoad não encontrado em libunity\n");
  }
  fprintf(stderr, "[F0] === libunity OK ===\n");
  mm_probe("pos-JNI_OnLoad");
  dbg_sync();

  /* ---- F1: carrega libil2cpp.so (2º módulo, lógica C# do jogo) ---- */
  g_m_unity = so_save();
  size_t i2s = 192UL * 1024 * 1024;
  void *i2heap = mmap(NULL, i2s, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  g_i2heap_base = (uintptr_t)i2heap; g_i2heap_size = i2s;
  if (i2heap != MAP_FAILED && so_load("libil2cpp.so", i2heap, i2s) >= 0) {
    g_il2cpp_base = (uintptr_t)text_base;
    g_alloc_ib = g_il2cpp_base;
    fprintf(stderr, "[F1] libil2cpp: text=%p+%zu\n", text_base, text_size);
    so_relocate();
    { extern void recon_fill_passthrough(void); recon_fill_passthrough(); }
    so_resolve(dynlib_functions, dynlib_numfunctions, 0);
    ctype_resolve();   /* _ctype_/_tolower_tab_/_toupper_tab_ p/ libil2cpp tb */
    so_record_phdr("libil2cpp.so");   /* p/ o dl_iterate_phdr custom (unwind) */
    if (so_register_eh_frame() == 0) fprintf(stderr, "[EH] .eh_frame libil2cpp registrado (exceções C++)\n");
    /* il2cpp abre o global-metadata.dat via open() -> intercepta p/ redirecionar.
       patch_got opera no modulo ATIVO (=il2cpp agora). Tb dlopen/dlsym/log. */
    patch_got("open", (void *)my_open);
    patch_got("mmap", (void *)my_mmap);
    patch_got("mmap64", (void *)my_mmap);
    /* sigaction do libil2cpp (o GC instala SIGPWR/SIGXCPU por aqui). Sem patch, o GC
       instalava um handler CORROMPIDO (0x7f10000004) p/ SIGPWR -> stop-the-world
       crashava. Com my_sigaction + CUP_GCSIG, bloqueamos -> nosso handler válido fica. */
    { extern int my_sigaction(); patch_got("sigaction", (void *)my_sigaction); }
    patch_got("fopen", (void *)my_fopen);
    if (getenv("TER_NO_FCLOSE")) patch_got("fclose", (void *)my_fclose);
    patch_got("stat", (void *)my_stat);
    patch_got("lstat", (void *)my_lstat);
    patch_got("stat64", (void *)my_stat64);
    patch_got("lstat64", (void *)my_lstat64);
    patch_got("access", (void *)my_access);
  patch_got("statfs64", (void *)my_statfs64);
  patch_got("statfs", (void *)my_statfs64);
  patch_got("strlcpy", (void *)my_strlcpy);
  patch_got("strlcat", (void *)my_strlcat);
  patch_got("__memmove_chk", (void *)my_memmove_chk);
  patch_got("__memcpy_chk", (void *)my_memcpy_chk);
  patch_got("__memset_chk", (void *)my_memset_chk);
  patch_got("__strlen_chk", (void *)my_strlen_chk);
  patch_got("__strcpy_chk", (void *)my_strcpy_chk);
  patch_got("__strcat_chk", (void *)my_strcat_chk);
  patch_got("__vsnprintf_chk", (void *)my_vsnprintf_chk);
  patch_got("__snprintf_chk", (void *)my_snprintf_chk);
  patch_got("__FD_SET_chk", (void *)my_FD_SET_chk);
    patch_got("dlopen", (void *)my_dlopen);
    patch_got("dlsym", (void *)my_dlsym);
    patch_got("exit", (void *)my_exit);
    patch_got("_exit", (void *)my_exit);
    patch_got("__android_log_print", (void *)my_alog_print);
    patch_got("__android_log_write", (void *)my_alog_write);
    patch_got("__android_log_vprint", (void *)my_alog_vprint);
    patch_android_import_overrides();
    patch_sem_shim();  /* sem_* nos slots GOT do libil2cpp */
    patch_pthread_shim();
    /* CUP_CRSPY: hooks nos MoveNext das coroutines de boot (antes do flush de caches) */
    if (getenv("CUP_CRSPY")) {
      g_cr1_cont = g_il2cpp_base + 0x9A58D0 + 16;
      g_cr2_cont = g_il2cpp_base + 0x9A619C + 16;
      hook_arm64(g_il2cpp_base + 0x9A58D0, (uintptr_t)my_start_cr);
      hook_arm64(g_il2cpp_base + 0x9A619C, (uintptr_t)my_inputwait_cr);
      fprintf(stderr, "[CRSPY] hooks start_cr(0x9A58D0 $PC+0xBC) + inputwait(0x9A619C $PC+0x1C)\n");
    }
    if (getenv("CUP_BOOTSPY")) bootspy_install(g_il2cpp_base);
    if (getenv("CUP_MENUSPY")) menuspy_install(g_il2cpp_base);
    /* CUP_FORCESTARTCR: CupheadStartScene.Start (0x9A55CC) faz 3 checks
     * op_Inequality em Application.version/productName/identifier e DÁ EARLY-RETURN
     * (0x9A56F8) antes de StartCoroutine(start_cr) se algum não bate. No so-loader
     * esses getters não retornam o esperado -> start_cr NUNCA roda -> disclaimer
     * congela. Forçamos o caminho de prosseguir: NOP nos 2 tbnz-return + B p/ o
     * bloco-proceed no 3º branch. (start_cr dirige disclaimer->preload->título.) */
    if (getenv("CUP_FORCESTARTCR")) {
      uint32_t *t = (uint32_t *)(g_il2cpp_base + 0x9A567C);
      t[0] = 0xd503201fu;                              /* 0x9A567C tbnz -> NOP (cai = prossegue) */
      *(uint32_t *)(g_il2cpp_base + 0x9A56B8) = 0xd503201fu; /* 0x9A56B8 tbnz -> NOP */
      *(uint32_t *)(g_il2cpp_base + 0x9A56F4) = 0x14000006u; /* 0x9A56F4 tbz -> B 0x9A570C (proceed) */
      __builtin___clear_cache((char *)(g_il2cpp_base + 0x9A567C), (char *)(g_il2cpp_base + 0x9A56F8));
      fprintf(stderr, "[FORCESTARTCR] Start() early-returns NOPados -> StartCoroutine(start_cr) forçado\n");
    }
    /* CUP_NOREFRESHDLC: case 9 do start_cr chama DLCManager.RefreshDLC (0xC91C44) que
       no so-loader crasha (blr p/ delegate de plataforma NULL = método il2cpp não-init)
       -> coroutine de boot quebra no $PC=9. NOP a função inteira (ret) p/ pular. */
    if (getenv("CUP_NOREFRESHDLC")) {
      *(uint32_t *)(g_il2cpp_base + 0xC91C44) = 0xd65f03c0u; /* ret */
      __builtin___clear_cache((char *)(g_il2cpp_base + 0xC91C44), (char *)(g_il2cpp_base + 0xC91C48));
      fprintf(stderr, "[NOREFRESHDLC] DLCManager.RefreshDLC(0xC91C44) -> ret\n");
    }
    if (getenv("CUP_SAPATH") || getenv("CUP_SAPATH_ON")) {
      hook_arm64(g_il2cpp_base + 0x17C7C1C, (uintptr_t)my_streamingAssetsPath);
      /* NÃO hookar getBasePath 0x1031C8C: é stub de 3 insns que JÁ tail-calleia
         get_streamingAssetsPath (hookado); o hook de 16B estoura na função seguinte. */
      fprintf(stderr, "[SAPATH] hook get_streamingAssetsPath(0x17C7C1C)\n");
    }
    if (!getenv("TER_RXD_NOABPATH")) {
      hook_arm64(g_il2cpp_base + 0x10EAF1C, (uintptr_t)rxd_get_debug_local_path);
      fprintf(stderr, "[RXD_ABPATH] hook AssetBundleScriptableObject.GetDebugLocalPath(0x10EAF1C)\n");
    }
    if (getenv("TER_RXD_FORCECONSOLEREADY")) {
      ter_patch_il2cpp_ret(0x1336ACC, "RXD ConsolePackageManager.StartDownloadAll");
      fprintf(stderr, "[RXD_CONSOLE] ConsolePackageManager.StartDownloadAll(0x1336ACC) -> ret\n");
    }
    if (getenv("TER_RXD_AUDIO_BOOT_FAKE")) {
      static const struct { uintptr_t off; const char *name; } audio_nops[] = {
        { 0x10B1B24, "AudioManager.PlayBGM(string,string)" },
        { 0x10B1E78, "AudioManager.PlayBGM(string,int)" },
        { 0x10B20E8, "AudioManager.PlaySystemSE(SystemSE)" },
        { 0x10B2144, "AudioManager.PlaySystemSE(SystemSE02)" },
        { 0x10B2410, "AudioManager.PlaySystemSE(string)" },
        { 0x10B26A8, "AudioManager.StopBGM" },
        { 0x10B2740, "AudioManager.StopAllExceptSE" },
        { 0x10B28E8, "AudioManager.StopAllSE" },
        { 0x10B2CF4, "AudioManager.StopAllVoice" },
        { 0x10B2EA8, "AudioManager.Stop(AudioChannelType)" },
        { 0x10B2ED0, "AudioManager.Stop(string)" },
        { 0x10B0200, "AudioManager.PauseAll" },
        { 0x10B039C, "AudioManager.PauseBGM" },
        { 0x10B303C, "AudioManager.PauseSE" },
        { 0x10B342C, "AudioManager.PauseVOICE" },
        { 0x10B371C, "AudioManager.Pause(AudioChannelType)" },
        { 0x10B374C, "AudioManager.Resume(AudioChannelType)" },
        { 0x10B01B8, "AudioManager.ResumeAll" },
      };
      for (unsigned ai = 0; ai < sizeof audio_nops / sizeof audio_nops[0]; ai++) {
        char tag[96];
        snprintf(tag, sizeof tag, "RXD %s", audio_nops[ai].name);
        ter_patch_il2cpp_ret(audio_nops[ai].off, tag);
      }
      fprintf(stderr, "[RXD_AUDIO] AudioManager play/stop -> ret (fake audio)\n");
    }
    if (getenv("TER_RXD_NO_LOADINGBLOCK")) {
      ter_patch_il2cpp_ret(0x11667C8, "RXD UIManager.UpdateLoadingBlock");
      fprintf(stderr, "[RXD_UI_BLOCK] UIManager.UpdateLoadingBlock(0x11667C8) -> ret\n");
    }
    /* TER_RXD_NO_UNLOAD: em f=2 um script da boot scene (CRI incompat?) chama
       Application.Unload() -> o player destroi a swapchain (p3056=nil), seta
       b2992/b3040 e entra no ciclo pauseJavaAndCallUnloadCallback = tela preta
       eterna com o managed rodando. Neutraliza o ICALL (RET) antes do 1o frame. */
    if (env_on("TER_RXD_NO_UNLOAD")) {
      void *(*resolve)(const char *) =
          (void *(*)(const char *))ter_il2cpp_sym_cached("il2cpp_resolve_icall");
      static const char *kill_icalls[] = {
        "UnityEngine.Application::Unload()",
        "UnityEngine.Application::Unload",
        "UnityEngine.Application::Quit()",
        "UnityEngine.Application::Quit(System.Int32)",
        NULL };
      long pgsz = sysconf(_SC_PAGESIZE);
      for (int ki = 0; kill_icalls[ki]; ki++) {
        void *ic = resolve ? resolve(kill_icalls[ki]) : NULL;
        if (!ic) continue;
        uintptr_t p0 = (uintptr_t)ic & ~(uintptr_t)(pgsz - 1);
        mprotect((void *)p0, (size_t)pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
        *(uint32_t *)ic = 0xd65f03c0u;  /* RET */
        __builtin___clear_cache((char *)ic, (char *)ic + 4);
        fprintf(stderr, "[RXD_NOUNLOAD] icall %s @ %p -> RET\n", kill_icalls[ki], ic);
        fsync(2);
      }
    }
    if (getenv("TER_RXD_ABSPY")) rxd_install_abspy(g_il2cpp_base);
    rxd_install_ui_awake_hook(g_il2cpp_base);
    if (getenv("CUP_TAPINPUT")) {
      if (getenv("CUP_TAPSTART")) g_tap_start = atoi(getenv("CUP_TAPSTART"));
      if (getenv("CUP_TAPPERIOD")) g_tap_period = atoi(getenv("CUP_TAPPERIOD"));
      g_tapinput_cont = g_il2cpp_base + 0xCC2854 + 16;
      hook_arm64(g_il2cpp_base + 0xCC2854, (uintptr_t)my_getanybuttondown);
      fprintf(stderr, "[TAPINPUT] hook GetAnyButtonDown(0xCC2854) start=%d period=%d\n", g_tap_start, g_tap_period);
    }
    /* CUP_GAMEPAD: controle REAL via USB Gamepad (js0). Substitui Rewired.Player
       .GetButton/Down/Up/GetAxis(string) lendo o estado do js0. (gamepad.c) */
    if (getenv("CUP_GAMEPAD")) {
      extern void gp_init(uintptr_t);
      gp_init(g_il2cpp_base);
    }
    if (getenv("CUP_STAGESPY")) stagespy_install(g_il2cpp_base);
    so_finalize(); so_flush_caches();
    fprintf(stderr, "[F1] libil2cpp init_array...\n");
    so_execute_init_array();
    g_m_il2cpp = so_save();
    fprintf(stderr, "[F1] libil2cpp carregado OK\n");
    mm_probe("pos-init_array-il2cpp");
    dbg_sync();
    g_m_mainlib = load_android_plugin_so("libmain.so", 4, "F1-main", vm);
    g_m_burst = load_android_plugin_so("lib_burst_generated.so", 16, "F1-burst", vm);
    g_m_cri = load_android_plugin_so("libcri_ware_unity.so", 32, "F1-cri", vm);
  } else {
    fprintf(stderr, "[F1] FALHOU carregar libil2cpp (heap=%p)\n", i2heap);
  }
  /* informa o sem_shim das bases p/ o detector de livelock mapear callers */
  { extern void sh_set_bases(unsigned long, unsigned long, unsigned long, unsigned long);
    sh_set_bases(g_unity_base, 0x2000000, g_il2cpp_base, 0x3000000); }
  { extern void pt_set_bases(unsigned long, unsigned long); pt_set_bases(g_unity_base, g_il2cpp_base); }

  so_use(g_m_unity);  /* volta o contexto p/ libunity */

  /* lista os métodos nativos registrados (achar initJni/nativeRender) */
  extern void jni_dump_natives(void);
  extern void *jni_find_native(const char *);
  jni_dump_natives();

  /* ---- F2: janela GLES2 + lifecycle Unity ----
     fbdev (Amlogic-old): EGL REAL do Mali (Unity cria contexto/surface no fb0).
     kmsdrm (X5M/Valhall): janela SDL3-kmsdrm + re-rota os egl* da Unity p/ egl_shim. */
  if (cup_use_kmsdrm()) {
    extern int egl_shim_ensure_current(void);
    /* SDL3 stock do X5M tem driver kmsdrm (+wayland). Default kmsdrm; o launcher
       pode sobrescrever via SDL_VIDEODRIVER (ex "wayland" sob sway, ou lista). */
    if (!getenv("SDL_VIDEODRIVER")) setenv("SDL_VIDEODRIVER", "kmsdrm", 1);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) fprintf(stderr, "[F2] SDL_Init(VIDEO|AUDIO): %s\n", SDL_GetError());
    fprintf(stderr, "[F2] kmsdrm: SDL video driver = %s\n",
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(null)");
    egl_shim_create_window();
    /* ELO: re-rota os egl* da libunity (so_resolve bindou no libEGL real) -> egl_shim.
       Contexto libunity ja' ativo aqui (so_use(g_m_unity) acima). */
    int np = egl_patch_unity_got();
    fprintf(stderr, "[F2] kmsdrm: %d slots egl* da libunity -> egl_shim\n", np);
    egl_shim_ensure_current();   /* deixa o contexto GL current na thread do jogo */
    fprintf(stderr, "[F2] janela GLES2 criada (egl_shim/SDL3 kmsdrm)\n");
  } else {
    /* áudio (opensles_shim usa SDL_OpenAudioDevice) */
    if (SDL_Init(SDL_INIT_AUDIO) != 0) fprintf(stderr, "[F2] SDL_Init(AUDIO): %s\n", SDL_GetError());
    fprintf(stderr, "[F2] EGL REAL Mali fbdev (fbdev_window %ux%u)\n",
            g_fbdev_win.w, g_fbdev_win.h);
  }

  static long thiz = 0xA1, ctx = 0xC0, surf = 0x5F;
  void *fn;
  if ((fn = jni_find_native("initJni"))) {
    fprintf(stderr, "[F2] initJni...\n");
    ((void (*)(void *, void *, void *))fn)(env, &thiz, &ctx);
    fprintf(stderr, "[F2] initJni OK\n");
    mm_probe("pos-initJni");
    dbg_sync();
  }
  if ((fn = jni_find_native("nativeRecreateGfxState"))) {
    mm_probe("pre-RecreateGfxState");
    /* 🔎 RE-ARMA on_crash: SDL_Init(VIDEO) do kmsdrm e/ou o blob Mali reinstalam
       o SIGSEGV default -> nosso dump nunca roda. Reinstala aqui, colado no ponto
       de crash, p/ o [CR!] async-safe sair. */
    {
      static char rearm_stk[256 * 1024]; stack_t rs = {0};
      rs.ss_sp = rearm_stk; rs.ss_size = sizeof rearm_stk; rs.ss_flags = 0;
      sigaltstack(&rs, NULL);
      struct sigaction sc; memset(&sc, 0, sizeof sc);
      sc.sa_sigaction = on_crash; sc.sa_flags = SA_SIGINFO | SA_ONSTACK;
      sigaction(SIGSEGV, &sc, 0); sigaction(SIGBUS, &sc, 0); sigaction(SIGILL, &sc, 0);
      fprintf(stderr, "[F2] on_crash re-armado pre-RecreateGfxState\n");
    }
    /* 🔎 dump de maps p/ correlacionar o pc do dmesg com a lib (blob Mali/libc/unity). */
    if (getenv("TER_DUMPMAPS")) {
      int mfd = open("/proc/self/maps", O_RDONLY);
      if (mfd >= 0) { char mb[4096]; ssize_t r;
        fprintf(stderr, "[MAPS] ==== inicio ====\n"); dbg_sync();
        while ((r = read(mfd, mb, sizeof mb)) > 0) { ssize_t w=write(2, mb, r); (void)w; }
        close(mfd); fprintf(stderr, "\n[MAPS] ==== fim ====\n"); dbg_sync();
      }
    }
    /* TESTE: anula o instalador de signal-handlers do Unity (0x360af8) com RET.
       Esse caminho (sigaction QUERY -> map RB-tree de old-handlers via operator-new)
       e' onde o canario estoura. Nao precisamos dos handlers do Unity (temos on_crash). */
    if (getenv("CUP_NOSIGINST")) {
      extern void so_make_text_writable(void), so_make_text_executable(void);
      so_make_text_writable();
      *(uint32_t *)(g_alloc_ub + 0x360af8) = 0xd65f03c0u;  /* RET */
      so_make_text_executable();
      so_flush_caches();
      fprintf(stderr, "[NOSIGINST] 0x360af8 (install handlers) -> RET\n");
    }
    /* spy: hook na entrada do operator-new (0x3cbf2c) p/ capturar args da
       chamada que estoura o canario. Instala AQUI p/ pegar so' o gfx path. */
    if (getenv("CUP_ASPY")) {
      extern void so_make_text_writable(void), so_make_text_executable(void);
      g_gfx_cont = g_alloc_ub + 0x3cbf2c + 16;
      so_make_text_writable();
      hook_arm64(g_alloc_ub + 0x3cbf2c, (uintptr_t)onew_spy_tramp);
      so_make_text_executable();
      so_flush_caches();
      g_in_gfx = 1;
      fprintf(stderr, "[ONEW] hook em operator-new (0x3cbf2c) instalado\n");
    }
    fprintf(stderr, "[F2] nativeRecreateGfxState...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, &thiz, 0, &surf);
    fprintf(stderr, "[F2] nativeRecreateGfxState OK\n");
    mm_probe("pos-RecreateGfxState");
    dbg_sync();
  }
  if ((fn = jni_find_native("nativeSendSurfaceChangedEvent"))) {
    fprintf(stderr, "[F2] nativeSendSurfaceChangedEvent...\n");
    ((void (*)(void *, void *))fn)(env, &thiz);
    fprintf(stderr, "[F2] nativeSendSurfaceChangedEvent OK\n");
    dbg_sync();
  }
  if ((fn = jni_find_native("nativeResume"))) {
    fprintf(stderr, "[F2] nativeResume...\n");
    ((void (*)(void *, void *))fn)(env, &thiz);
    fprintf(stderr, "[F2] nativeResume OK\n");
    dbg_sync();
  }
  if ((fn = jni_find_native("nativeFocusChanged"))) {
    fprintf(stderr, "[F2] nativeFocusChanged(1)...\n");
    ((void (*)(void *, void *, int))fn)(env, &thiz, 1);
    fprintf(stderr, "[F2] nativeFocusChanged OK\n");
    dbg_sync();
  }

  /* dispara a thread de áudio do FMOD (alimenta fmodProcess em paralelo ao
     render — destrava o boot que espera o mixer). CUP_NOAUDIOTHREAD=1 desliga.
     s14: a própria thread checa opensles_shim_engine_active() e se desliga
     quando o OpenSL (FORCESL) assumiu o mixer. */
  if (getenv("TER_AUDIO")) {  /* experimento de som — gated, OFF no run.sh (não quebra o build OK) */
    g_fmod_env = env;
    pthread_t at; pthread_create(&at, NULL, fmod_audio_thread, NULL);
    pthread_detach(at);
    fprintf(stderr, "[AUDIO] thread de áudio FMOD criada\n");
  }
  if (getenv("CUP_PRELOAD_TICK")) {
    pthread_t tt; pthread_create(&tt, NULL, preload_tick_thread, NULL);
    pthread_detach(tt);
  }
  /* Choreographer driver: dispara FrameCallback.doFrame(nanos) ~60Hz numa thread propria
     p/ destravar o nativeRender do frame 2 que espera o vsync/doFrame que nosso Looper
     fake nunca entrega. Opt-in enquanto o ROCKMAN valida esse caminho: TER_CHOREO=1. */
  g_choreo_env = env;
  if (env_on("TER_CHOREO") && env_on("TER_CHOREO_INLINE")) {
    fprintf(stderr, "[CHOREO_INLINE] driver inline habilitado (sem thread separada)\n");
    fsync(2);
  } else if (env_on("TER_CHOREO") && !env_on("TER_NOCHOREO")) {
    pthread_t ct; pthread_create(&ct, NULL, choreo_driver_thread, NULL);
    pthread_detach(ct);
    fprintf(stderr, "[CHOREO] driver-thread de doFrame criada (~60Hz)\n");
  }
  if (getenv("CUP_PSPY")) {
    pthread_t st; pthread_create(&st, NULL, preload_spy_thread, NULL);
    pthread_detach(st);
  }
  if (getenv("CUP_PRELOAD_BG")) {
    pthread_t bt; pthread_create(&bt, NULL, preload_bg_thread, NULL);
    pthread_detach(bt);
  }
  void *render = jni_find_native("nativeRender");
  void *rxd_native_surface_event = jni_find_native("nativeSendSurfaceChangedEvent");
  void *rxd_native_resume = jni_find_native("nativeResume");
  void *rxd_native_focus_changed = jni_find_native("nativeFocusChanged");
  fprintf(stderr, "[F2] nativeRender=%p -> loop\n", render);
  int max_f = getenv("CUP_FRAMES") ? atoi(getenv("CUP_FRAMES")) : 600;
  void *fpump = jni_find_native("nativePause");  /* só p/ existência */ (void)fpump;
  g_render_tid = (int)syscall(SYS_gettid);   /* p/ recovery longjmp (CUP_SKIPBAD) */
  /* CUP_AUTOTAP: o disclaimer/menu espera "toque/botão pra continuar". Injeta
     periodicamente um botão de confirmação via nativeInjectEvent (KeyEvent) p/
     avançar. CUP_AUTOTAP=keycode (default 66=ENTER; 96=BUTTON_A, 23=DPAD_CENTER). */
  extern struct hk_inject_s { int action, keycode, source, deviceId, metaState, repeat,
                              scancode, flags, unicode; long eventTime, downTime; } g_hk_inject;
  extern void *hk_keyevent_object(void);
  void *inject = jni_find_native("nativeInjectEvent");
  int tapkey = getenv("CUP_AUTOTAP") ? atoi(getenv("CUP_AUTOTAP")) : 0;
  if (tapkey && inject) fprintf(stderr, "[AUTOTAP] keycode=%d via nativeInjectEvent=%p\n", tapkey, inject);
  /* CUP_DRAINPRELOAD=N: os ops de preload do título completam o background (jobmgr=0)
   * mas ficam presos na fila de INTEGRAÇÃO (integQ) pq a integração per-frame não roda
   * fora do WaitForAll. Dirigimos nós: N× UpdatePreloadingSingleStep(mgr,2,0x10) por frame
   * (limitado=não pendura; +FORCEINTEG p/ passar o gate de budget). mgr vem do PSPY. */
  int drainN = getenv("CUP_DRAINPRELOAD") ? atoi(getenv("CUP_DRAINPRELOAD")) : 0;
  void (*preload_step)(void *, int, int) = drainN ? (void (*)(void *, int, int))(g_unity_base + 0x8733a8) : NULL;
  if (drainN) fprintf(stderr, "[DRAINPRELOAD] %d steps/frame (UpdatePreloadingSingleStep=0x8733a8)\n", drainN);
  /* CUP_DRAINWAIT: chama WaitForAllAsyncOperationsToComplete(mgr) (0x873a90) 1×/frame.
   * Diferente do step cru, o WaitForAll roda o loop completo + a fase de "process"
   * (0x738a98) que DISPARA OS CALLBACKS de conclusão das async ops -> o FontLoader
   * vê as fontes como done e avança. ⚠️ pode pendurar se HasPendingOps nunca zerar. */
  int drainWait = getenv("CUP_DRAINWAIT") ? 1 : 0;
  void (*wait_all)(void *) = drainWait ? (void (*)(void *))(g_unity_base + 0x873a90) : NULL;
  if (drainWait) fprintf(stderr, "[DRAINWAIT] WaitForAll(mgr)=0x873a90 1x/frame\n");
  /* CUP_DRIVECR: o pump de coroutine do engine PARA de resumir o start_cr no $PC=9
   * (Cuphead.Init/RefreshDLC) — render voa mas $PC fica preso. Dirigimos o MoveNext
   * nós mesmos a cada N frames (cr1_tramp = MoveNext real). Só age a partir do
   * CUP_DRIVECR_FROM (default 200, dá tempo do boot normal rodar) p/ não atropelar
   * o pump do engine nas fases iniciais. */
  extern long cr1_tramp(void *it); extern void *volatile g_startcr_it;
  int drivecr = getenv("CUP_DRIVECR") ? 1 : 0;
  int drivecr_from = getenv("CUP_DRIVECR_FROM") ? atoi(getenv("CUP_DRIVECR_FROM")) : 200;
  if (drivecr) fprintf(stderr, "[DRIVECR] dirige start_cr MoveNext a partir do frame %d\n", drivecr_from);
  /* TER_RXD_DRIVEBOOT: no Rockman X DiVE o OrangeBootup.<Start> para no primeiro
   * WaitForSeconds. Quando o pump de coroutine do Unity nao resume, dirigimos os
   * MoveNext do boot em baixa frequencia para atravessar o warmup e chegar no load
   * de assets/cenas. */
  int rxd_driveboot = getenv("TER_RXD_DRIVEBOOT") ? 1 : 0;
  int rxd_driveboot_from = env_int_default("TER_RXD_DRIVEBOOT_FROM", 30);
  int rxd_driveboot_period = env_int_default("TER_RXD_DRIVEBOOT_PERIOD", 30);
  int rxd_drivenested_period = env_int_default("TER_RXD_DRIVENESTED_PERIOD", 10);
  if (rxd_driveboot_period <= 0) rxd_driveboot_period = 1;
  if (rxd_drivenested_period <= 0) rxd_drivenested_period = 1;
  if (rxd_driveboot) {
    fprintf(stderr, "[RXD_DRIVEBOOT] dirige OrangeBootup MoveNext a partir do frame %d, periodo=%d nested=%d\n",
            rxd_driveboot_from, rxd_driveboot_period, rxd_drivenested_period);
  }
  int rxd_driveab = getenv("TER_RXD_DRIVEAB") ? 1 : rxd_driveboot;
  int rxd_driveab_from = env_int_default("TER_RXD_DRIVEAB_FROM", rxd_driveboot_from + 10);
  int rxd_driveab_period = env_int_default("TER_RXD_DRIVEAB_PERIOD", 10);
  if (rxd_driveab_period <= 0) rxd_driveab_period = 1;
  if (rxd_driveab) {
    fprintf(stderr, "[RXD_DRIVEAB] dirige asset coroutines a partir do frame %d, periodo=%d\n",
            rxd_driveab_from, rxd_driveab_period);
  }
  int rxd_forceaudio = getenv("TER_RXD_FORCEAUDIOREADY") ? 1 : 0;
  int rxd_forceaudio_from = env_int_default("TER_RXD_FORCEAUDIOREADY_FROM", 55);
  if (rxd_forceaudio) {
    fprintf(stderr, "[RXD_AUDIO] FORCE IsInitAll a partir do frame %d\n", rxd_forceaudio_from);
  }
  int rxd_forceconsole = getenv("TER_RXD_FORCECONSOLEREADY") ? 1 : 0;
  int rxd_forceconsole_from = env_int_default("TER_RXD_FORCECONSOLEREADY_FROM", 65);
  if (rxd_forceconsole) {
    fprintf(stderr, "[RXD_CONSOLE] FORCE ready a partir do frame %d\n", rxd_forceconsole_from);
  }
  int rxd_forcelocale = getenv("TER_RXD_FORCELOCALEREADY") ? 1 : 0;
  int rxd_forcelocale_from = env_int_default("TER_RXD_FORCELOCALEREADY_FROM", 150);
  if (rxd_forcelocale) {
    fprintf(stderr, "[RXD_LOCALE] FORCE ready a partir do frame %d\n", rxd_forcelocale_from);
  }
  int rxd_loadscene = getenv("TER_RXD_LOADSCENE_INDEX") ? 1 : 0;
  int rxd_loadscene_index = env_int_default("TER_RXD_LOADSCENE_INDEX", 0);
  int rxd_loadscene_from = env_int_default("TER_RXD_LOADSCENE_FROM", 300);
  int rxd_loadscene_must = env_int_default("TER_RXD_LOADSCENE_MUST", 0);
  int rxd_loadscene_log_period = env_int_default("TER_RXD_LOADSCENE_LOG_PERIOD", 60);
  int rxd_loadscene_activate = env_on("TER_RXD_LOADSCENE_ACTIVATE");
  int rxd_loadscene_pump = env_on("TER_RXD_LOADSCENE_PUMP");
  int rxd_loadscene_pump_bg = env_on("TER_RXD_LOADSCENE_PUMP_BG");
  int rxd_loadscene_pump_integ = env_on("TER_RXD_LOADSCENE_PUMP_INTEG") || rxd_loadscene_pump;
  int rxd_loadscene_pump_final = env_on("TER_RXD_LOADSCENE_PUMP_FINAL");
  int rxd_loadscene_force_state2 = env_on("TER_RXD_LOADSCENE_FORCE_STATE2");
  int rxd_scenevt_guard = env_on("TER_RXD_SCENEVT_GUARD") || rxd_loadscene_pump_final;
  int rxd_loadscene_pump_from = env_int_default("TER_RXD_LOADSCENE_PUMP_FROM", rxd_loadscene_from + 40);
  int rxd_loadscene_pump_period = env_int_default("TER_RXD_LOADSCENE_PUMP_PERIOD", 30);
  int rxd_loadscene_pump_n = env_int_default("TER_RXD_LOADSCENE_PUMP_N", 1);
  int rxd_loadscene_final_delay = env_int_default("TER_RXD_LOADSCENE_FINAL_DELAY", 20);
  int rxd_loadscene_final_require_allow = !env_on("TER_RXD_LOADSCENE_FINAL_NO_REQUIRE_ALLOW");
  int rxd_loadscene_final_require_queue = !env_on("TER_RXD_LOADSCENE_FINAL_NO_REQUIRE_QUEUE");
  if (rxd_loadscene_pump_period <= 0) rxd_loadscene_pump_period = 1;
  if (rxd_loadscene_pump_n <= 0) rxd_loadscene_pump_n = 1;
  if (rxd_loadscene_final_delay < 0) rxd_loadscene_final_delay = 0;
  int rxd_loadscene_done = 0;
  if (rxd_loadscene) {
    fprintf(stderr, "[RXD_LOADSCENE] vai chamar LoadSceneAsyncNameIndexInternal index=%d must=%d activate=%d log_period=%d pump=%d bg=%d integ=%d final=%d force_state2=%d vtguard=%d pump_from=%d period=%d n=%d final_delay=%d req_allow=%d req_queue=%d a partir do frame %d\n",
            rxd_loadscene_index, rxd_loadscene_must, rxd_loadscene_activate,
            rxd_loadscene_log_period, rxd_loadscene_pump, rxd_loadscene_pump_bg,
            rxd_loadscene_pump_integ, rxd_loadscene_pump_final,
            rxd_loadscene_force_state2, rxd_scenevt_guard, rxd_loadscene_pump_from,
            rxd_loadscene_pump_period, rxd_loadscene_pump_n,
            rxd_loadscene_final_delay, rxd_loadscene_final_require_allow,
            rxd_loadscene_final_require_queue, rxd_loadscene_from);
  }
  int rxd_titledrive = env_on("TER_RXD_TITLE_DRIVE");
  int rxd_titledrive_from = env_int_default("TER_RXD_TITLE_DRIVE_FROM", 390);
  int rxd_titledrive_update_period = env_int_default("TER_RXD_TITLE_UPDATE_PERIOD", 30);
  int rxd_titledrive_sceneinit = !env_on("TER_RXD_TITLE_NO_SCENEINIT");
  int rxd_titledrive_start = !env_on("TER_RXD_TITLE_NO_START");
  int rxd_titledrive_openui = !env_on("TER_RXD_TITLE_NO_OPENUI");
  int rxd_titledrive_update = env_on("TER_RXD_TITLE_UPDATE");
  int rxd_titledrive_sceneinit_done = 0;
  int rxd_titledrive_start_done = 0;
  int rxd_titledrive_openui_done = 0;
  if (rxd_titledrive) {
    if (rxd_titledrive_update_period <= 0) rxd_titledrive_update_period = 1;
    fprintf(stderr, "[RXD_TITLEDRIVE] from=%d sceneinit=%d start=%d openui=%d update=%d period=%d\n",
            rxd_titledrive_from, rxd_titledrive_sceneinit, rxd_titledrive_start,
            rxd_titledrive_openui, rxd_titledrive_update, rxd_titledrive_update_period);
  }
  int rxd_setactive_managed = env_on("TER_RXD_SET_ACTIVE_MANAGED");
  int rxd_setactive_managed_from = env_int_default("TER_RXD_SET_ACTIVE_MANAGED_FROM",
                                                   rxd_loadscene_pump_from + 20);
  int rxd_setactive_managed_period = env_int_default("TER_RXD_SET_ACTIVE_MANAGED_PERIOD", 30);
  if (rxd_setactive_managed_period <= 0) rxd_setactive_managed_period = 1;
  if (rxd_setactive_managed) {
    fprintf(stderr, "[RXD_MSCENE] managed SetActiveScene from=%d period=%d index=%d\n",
            rxd_setactive_managed_from, rxd_setactive_managed_period,
            env_int_default("TER_RXD_SET_ACTIVE_INDEX", -1));
  }
  /* TER_RXD_REGFX: o Unity 2020.3 DESTROI a swapchain interna (unity+0x182a000
     p3056) no f=2 em reacao ao setRequestedOrientation do jogo e fica esperando a
     Activity Java entregar uma surface nova (que nunca vem) -> nativeRender
     early-out (ret=0) p/ sempre = tela preta com o managed rodando. A UNICA
     rota que entrega janela e' nativeRecreateGfxState(0, Surface); rechamamos
     com a MESMA surface fake do F2 enquanto o render nao volta (ret==0). */
  int rxd_regfx = env_on("TER_RXD_REGFX");
  int rxd_regfx_from = env_int_default("TER_RXD_REGFX_FROM", 8);
  int rxd_regfx_period = env_int_default("TER_RXD_REGFX_PERIOD", 30);
  int rxd_regfx_max = env_int_default("TER_RXD_REGFX_MAX", 40);
  int rxd_regfx_n = 0;
  unsigned char rxd_regfx_last_ret = 1;
  if (rxd_regfx_period <= 0) rxd_regfx_period = 1;
  if (rxd_regfx) {
    fprintf(stderr, "[RXD_REGFX] re-entrega de surface armada from=%d period=%d max=%d\n",
            rxd_regfx_from, rxd_regfx_period, rxd_regfx_max);
  }
  int rxd_lifecycle_tick = env_on("TER_RXD_LIFECYCLE_TICK");
  int rxd_lifecycle_from = env_int_default("TER_RXD_LIFECYCLE_FROM",
                                           rxd_loadscene_pump_from + 20);
  int rxd_lifecycle_period = env_int_default("TER_RXD_LIFECYCLE_PERIOD", 30);
  int rxd_lifecycle_surface = !env_on("TER_RXD_LIFECYCLE_NO_SURFACE");
  int rxd_lifecycle_resume = env_on("TER_RXD_LIFECYCLE_RESUME");
  int rxd_lifecycle_focus = env_on("TER_RXD_LIFECYCLE_FOCUS");
  if (rxd_lifecycle_period <= 0) rxd_lifecycle_period = 1;
  if (rxd_lifecycle_tick) {
    fprintf(stderr,
            "[RXD_LIFE] tick from=%d period=%d surface=%d resume=%d focus=%d fns surface=%p resume=%p focus=%p\n",
            rxd_lifecycle_from, rxd_lifecycle_period, rxd_lifecycle_surface,
            rxd_lifecycle_resume, rxd_lifecycle_focus, rxd_native_surface_event,
            rxd_native_resume, rxd_native_focus_changed);
  }
  int rxd_scenedump = env_on("TER_RXD_SCENEDUMP");
  int rxd_scenedump_from = env_int_default("TER_RXD_SCENEDUMP_FROM",
                                           rxd_loadscene_pump_from + 20);
  int rxd_scenedump_period = env_int_default("TER_RXD_SCENEDUMP_PERIOD", 60);
  if (rxd_scenedump_period <= 0) rxd_scenedump_period = 1;
  if (rxd_scenedump) {
    fprintf(stderr, "[RXD_SCENEDUMP] from=%d period=%d\n",
            rxd_scenedump_from, rxd_scenedump_period);
  }
  int rxd_camera_render = env_on("TER_RXD_CAMERA_RENDER");
  int rxd_camera_render_from = env_int_default("TER_RXD_CAMERA_RENDER_FROM",
                                               rxd_loadscene_pump_from + 80);
  int rxd_camera_render_period = env_int_default("TER_RXD_CAMERA_RENDER_PERIOD", 1);
  if (rxd_camera_render_period <= 0) rxd_camera_render_period = 1;
  if (rxd_camera_render) {
    fprintf(stderr, "[RXD_CAMRENDER] from=%d period=%d disabled=%d\n",
            rxd_camera_render_from, rxd_camera_render_period,
            env_on("TER_RXD_CAMERA_RENDER_DISABLED"));
  }
  int rxd_splashdrive = env_on("TER_RXD_SPLASHDRIVE");
  int rxd_splashdrive_from = env_int_default("TER_RXD_SPLASHDRIVE_FROM",
                                             rxd_loadscene_pump_from + 90);
  int rxd_splashdrive_period = env_int_default("TER_RXD_SPLASHDRIVE_PERIOD", 10);
  if (rxd_splashdrive_period <= 0) rxd_splashdrive_period = 1;
  if (rxd_splashdrive) {
    fprintf(stderr, "[RXD_SPLASHDRIVE] from=%d period=%d start=%d onstart=%d update=%d click_from=%d\n",
            rxd_splashdrive_from, rxd_splashdrive_period,
            !env_on("TER_RXD_SPLASHDRIVE_NO_START"),
            !env_on("TER_RXD_SPLASHDRIVE_NO_ONSTART"),
            env_on("TER_RXD_SPLASHDRIVE_UPDATE"),
            env_int_default("TER_RXD_SPLASHDRIVE_CLICK_FROM", -1));
    fsync(2);
  }
  const char *rxd_change_scene = getenv("TER_RXD_CHANGE_SCENE");
  int rxd_change = (rxd_change_scene && rxd_change_scene[0]) ? 1 : 0;
  int rxd_change_from = env_int_default("TER_RXD_CHANGE_FROM", 300);
  int rxd_change_type = env_int_default("TER_RXD_CHANGE_TYPE", 0);
  int rxd_change_clear = env_int_default("TER_RXD_CHANGE_CLEAR", 1);
  int rxd_change_skip = env_int_default("TER_RXD_CHANGE_SKIP", 0);
  int rxd_change_done = 0;
  if (rxd_change) {
    fprintf(stderr, "[RXD_CHANGE] vai chamar OrangeSceneManager.ChangeScene scene=%s type=%d clear=%d skip=%d a partir do frame %d\n",
            rxd_change_scene, rxd_change_type, rxd_change_clear, rxd_change_skip, rxd_change_from);
  }
  int rxd_force_onstart = env_on("TER_RXD_FORCE_ONSTART");
  int rxd_drivescene = env_on("TER_RXD_DRIVESCENE") || rxd_force_onstart;
  int rxd_drivescene_from = env_int_default("TER_RXD_DRIVESCENE_FROM", rxd_change_from + 1);
  int rxd_drivescene_period = env_int_default("TER_RXD_DRIVESCENE_PERIOD", 10);
  int rxd_force_onstart_delay = env_int_default("TER_RXD_FORCE_ONSTART_DELAY", 1);
  if (rxd_drivescene_period <= 0) rxd_drivescene_period = 1;
  if (rxd_force_onstart_delay < 0) rxd_force_onstart_delay = 0;
  if (rxd_drivescene) {
    fprintf(stderr, "[RXD_SCENECR] driver OnStartChangeScene from=%d period=%d force_onstart=%d delay=%d\n",
            rxd_drivescene_from, rxd_drivescene_period, rxd_force_onstart,
            rxd_force_onstart_delay);
  }
  /* CUP_GCOFF: desabilita o GC do il2cpp durante o boot. Hipótese: o crash flaky do
     $PC=9 é use-after-free — o Boehm GC coleta um objeto que a desserialização do
     CupheadCore ainda referencia (a integração forçada cria objeto que o GC não
     rastreia). Sem GC no boot, nada é coletado -> sem UAF. (heap cresce; re-habilitar
     depois do título se preciso.) */
  void (*il2_gc_disable)(void) = (void (*)(void))ter_il2cpp_sym_cached("il2cpp_gc_disable");
  void (*il2_gc_enable)(void) = (void (*)(void))ter_il2cpp_sym_cached("il2cpp_gc_enable");
  void (*il2_gc_collect)(int) = (void (*)(int))ter_il2cpp_sym_cached("il2cpp_gc_collect");
  long (*il2_gc_heap)(void) = (long (*)(void))ter_il2cpp_sym_cached("il2cpp_gc_get_heap_size");
  long (*il2_gc_used)(void) = (long (*)(void))ter_il2cpp_sym_cached("il2cpp_gc_get_used_size");
  int gcoff = (getenv("CUP_GCOFF") && g_il2cpp_base && il2_gc_disable) ? 1 : 0;
  /* religa o GC no frame CUP_GCON_F (default 350, bem depois do boot ~frame 200) p/ o
     heap NÃO crescer indefinido (parado no disclaimer = OOM/thrash). 0 = nunca religa. */
  int gcon_f = getenv("CUP_GCON_F") ? atoi(getenv("CUP_GCON_F")) : 350;
  if (gcoff) {
    il2_gc_disable();
    fprintf(stderr, "[GCOFF] il2cpp_gc_disable() no boot; religa GC no frame %d\n", gcon_f);
  }
  /* TER_NOGCWAIT: o muro do frame 2 = il2cpp GC stop-the-world (WaitForThreadsToSuspend
     @libil2cpp+0x74f260) que ESPERA p/ sempre uma thread cooperativa (que bloqueia SIGPWR)
     dar ACK do suspend e ela nunca chega num safepoint. EXPERIMENTO: patcha a fn p/ retornar
     0 (=todas suspensas) imediatamente, deixando o GC seguir. ⚠️ pode scan stack de thread
     viva (com GC desligado o scan é mínimo). */
  if (getenv("TER_NOGCWAIT") && g_il2cpp_base) {
    uintptr_t a = g_il2cpp_base + 0x74f260;
    long pgsz = sysconf(_SC_PAGESIZE);
    void *pa = (void *)(a & ~((uintptr_t)pgsz - 1));
    mprotect(pa, pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
    *(uint32_t *)a = 0x52800000u;        /* mov w0, #0 */
    *(uint32_t *)(a + 4) = 0xd65f03c0u;  /* ret */
    mprotect(pa, pgsz * 2, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)pa, (char *)pa + pgsz * 2);
    fprintf(stderr, "[NOGCWAIT] WaitForThreadsToSuspend 0x74f260 -> return 0\n");
  }
  /* RXD/Unity2020: frame 3 fica em pthread_cond_wait no loop libunity+0x6b15a8
     esperando [obj+0x58] ficar pronto. No so-loader o sinal/producer não chega;
     para diagnóstico e destravamento, sai do loop e segue para o unlock/ret. */
  if (getenv("TER_RXD_SKIPWAIT") && g_unity_base) {
    uintptr_t a = g_unity_base + 0x6b15a8;
    long pgsz = sysconf(_SC_PAGESIZE);
    void *pa = (void *)(a & ~((uintptr_t)pgsz - 1));
    mprotect(pa, pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
    *(uint32_t *)a = 0x14000004u;   /* b 0x6b15b8: pula cond_wait, faz unlock e retorna */
    mprotect(pa, pgsz * 2, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)pa, (char *)pa + 4);
    fprintf(stderr, "[RXD_SKIPWAIT] libunity+0x6b15a8 -> b +0x10\n");
  }
  /* TER_SKIPTASKWAIT: PLANO B — pula a wait do job-queue em libunity+0x2f37b0 (a main constrói
     uma future-task no init de serialização e BLOQUEIA p/ sempre pq o worker nunca produz). A
     saída (0x2f37c4) só faz mutex_unlock+ret (NÃO deref o item), então pular é razoável p/ ver o
     PRÓXIMO muro. Patcha `cbnz x8, 0x2f37c4` -> `b 0x2f37c4` (sai sem esperar). */
  if (getenv("TER_SKIPTASKWAIT") && g_unity_base) {
    uintptr_t a = g_unity_base + 0x2f37b0;
    long pgsz = sysconf(_SC_PAGESIZE);
    void *pa = (void *)(a & ~((uintptr_t)pgsz - 1));
    mprotect(pa, pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
    *(uint32_t *)a = 0x14000005u;   /* b 0x2f37c4 (+0x14) — sempre sai do loop de espera */
    mprotect(pa, pgsz * 2, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)pa, (char *)pa + pgsz * 2);
    fprintf(stderr, "[SKIPTASKWAIT] libunity+0x2f37b0 cbnz->b (pula a wait do job-queue)\n");
  }
  /* TER_INLINETASK: instala um trampolim no TOPO do loop de espera do per-object task (0x2f37a4)
     que chama ter_inline_task(obj) (finge a conclusão: seta o nó + incrementa c10360) e então
     executa a instrução original + volta. Destrava per-object task (frame 2) E WaitForJobGroup
     (frame 3) sem depender do dispatch p/ workers. (Substitui o SKIPTASKWAIT — NÃO usar ambos.) */
  if (getenv("TER_INLINETASK") && g_unity_base) {
    extern void ter_inline_task(void *);
    long pgsz = sysconf(_SC_PAGESIZE);
    uintptr_t patch = g_unity_base + 0x2f37a4;
    /* trampolim numa página RWX PERTO da libunity (b tem alcance ±128MB) */
    uintptr_t hint = (g_unity_base + 0x2000000) & ~((uintptr_t)pgsz - 1);
    void *tp = mmap((void *)hint, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tp == MAP_FAILED) tp = mmap(NULL, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long d = (long)((uintptr_t)tp) - (long)patch;
    if (tp != MAP_FAILED && d > -0x7000000L && d < 0x7000000L) {
      uint32_t *t = (uint32_t *)tp;
      t[0] = 0xF81F0FFEu;          /* str x30,[sp,#-16]!  */
      t[1] = 0xAA1303E0u;          /* mov x0, x19 (obj)   */
      t[2] = 0x580000D0u;          /* ldr x16,[pc+0x18] -> fn@T+0x20 */
      t[3] = 0xD63F0200u;          /* blr x16             */
      t[4] = 0xF84107FEu;          /* ldr x30,[sp],#16    */
      t[5] = 0xF9402E68u;          /* ldr x8,[x19,#88] (instr original) */
      t[6] = 0x58000090u;          /* ldr x16,[pc+0x10] -> dst@T+0x28 */
      t[7] = 0xD61F0200u;          /* br x16              */
      *(uint64_t *)((char *)tp + 0x20) = (uint64_t)(uintptr_t)ter_inline_task;
      *(uint64_t *)((char *)tp + 0x28) = (uint64_t)(g_unity_base + 0x2f37a8);
      __builtin___clear_cache((char *)tp, (char *)tp + pgsz);
      /* patch 0x2f37a4 -> b trampolim */
      void *pp = (void *)(patch & ~((uintptr_t)pgsz - 1));
      mprotect(pp, pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
      *(uint32_t *)patch = 0x14000000u | (uint32_t)(((d) >> 2) & 0x03FFFFFF);
      mprotect(pp, pgsz * 2, PROT_READ | PROT_EXEC);
      __builtin___clear_cache((char *)pp, (char *)pp + pgsz * 2);
      fprintf(stderr, "[INLINETASK] trampolim @%p (d=0x%lx) patch 0x2f37a4->b\n", tp, d);
    } else {
      fprintf(stderr, "[INLINETASK] FALHOU mmap/alcance (tp=%p d=0x%lx)\n", tp, d);
    }
  }
  /* TER_SKIPJOBWAIT: pula TAMBÉM o WaitForJobGroup (0x2f1d1c): `while([0xc10360]<target) cond_wait`.
     ⚠️ causa abort (job results incompletos são necessários) — só p/ diagnóstico. */
  if (getenv("TER_SKIPJOBWAIT") && g_unity_base) {
    uintptr_t b = g_unity_base + 0x2f1d48;
    long pgsz = sysconf(_SC_PAGESIZE);
    void *pb = (void *)(b & ~((uintptr_t)pgsz - 1));
    mprotect(pb, pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
    *(uint32_t *)b = 0x14000005u;   /* b 0x2f1d5c (+0x14) */
    mprotect(pb, pgsz * 2, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)pb, (char *)pb + pgsz * 2);
    fprintf(stderr, "[SKIPJOBWAIT] libunity+0x2f1d48 WaitForJobGroup -> sai imediato\n");
  }
  /* TER_FORCETHREADED: o flag "threaded" do job-system (libunity+0xc0da20) fica 0 no nosso env
     (a capability/boot.config retorna 0) → o scheduler NUNCA despacha p/ os worker threads (que
     EXISTEM e estão parked) → roda "inline" mas o inline não incrementa o contador (0xc10360) →
     WaitForJobGroup trava p/ sempre (gdb: flag=0, counter=0, main em 0x2f1d58 cond_wait).
     FIX: (1) patcha o `cset w20,ne` (0x2eaacc) que computa o flag → `mov w20,#1` (sempre threaded);
     (2) escreve 1 direto no byte (caso o init já tenha rodado antes deste patch). */
  if (getenv("TER_FORCETHREADED") && g_unity_base) {
    long pgsz = sysconf(_SC_PAGESIZE);
    uintptr_t c = g_unity_base + 0x2eaacc;            /* cset w20, ne */
    void *pc = (void *)(c & ~((uintptr_t)pgsz - 1));
    mprotect(pc, pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC);
    *(uint32_t *)c = 0x52800034u;                     /* mov w20, #1 */
    mprotect(pc, pgsz * 2, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)pc, (char *)pc + pgsz * 2);
    volatile uint8_t *flag = (uint8_t *)(g_unity_base + 0xc0da20);
    *flag = 1;
    fprintf(stderr, "[FORCETHREADED] flag c0da20 -> 1 (cset->mov#1 @0x2eaacc + write byte)\n");
  }
  /* TER_JOBSPY: lê os contadores globais do job system (0xc10350..0xc10370) periódico p/
     diagnosticar agendado-vs-completado. */
  int jobspy = getenv("TER_JOBSPY") ? 1 : 0;
  int gamepad_on = getenv("CUP_GAMEPAD") ? 1 : 0;
  extern void gp_poll(void); extern void gp_frame_end(void);
  /* CUP_LOADYIELD=us: durante o boot/load, cede CPU aos WORKER threads (sched_yield+usleep)
     ANTES de cada frame integrar, p/ os jobs async COMPLETAREM antes da integração forçada
     (o check jobs-pending 0x6cdad0 é não-confiável aqui -> race -> objeto malformado ->
     crash $PC=9). Só nos primeiros LOADYIELD_F frames (fase de load). */
  int loadyield = getenv("CUP_LOADYIELD") ? atoi(getenv("CUP_LOADYIELD")) : 0;
  int loadyield_f = getenv("CUP_LOADYIELD_F") ? atoi(getenv("CUP_LOADYIELD_F")) : 350;
  if (loadyield) fprintf(stderr, "[LOADYIELD] %dus/frame nos 1ºs %d frames (CPU p/ workers)\n", loadyield, loadyield_f);
  int render_sleep_us = env_int_default("TER_RENDER_SLEEP_US", 0);
  int render_sleep0_us = env_int_default("TER_RENDER_SLEEP0_US", 0);
  if (render_sleep_us > 0 || render_sleep0_us > 0) {
    fprintf(stderr, "[RENDERPACE] sleep_us=%d sleep0_us=%d\n", render_sleep_us, render_sleep0_us);
    fsync(2);
  }
  rxd_enum_scene_methods_once();
  rxd_splashspy_install_once();
  /* CUP_MEMLOG: telemetria de memória a cada ~10s (600 frames) — p/ achar a curva
     do vazamento que mata o device ~6-7min de render (thrash->sshd starved->freeze) */
  int memlog = getenv("CUP_MEMLOG") ? 1 : 0;
  /* CUP_SCENESPY: dump periódico do SceneManager nativo; CUP_SETACTIVE: conserta
     cena ativa NULL (raiz provável do player-fantasma do mapa, s14) */
  int scenespy = getenv("CUP_SCENESPY") ? 1 : 0;
  int setactive = getenv("CUP_SETACTIVE") ? 1 : 0;
  /* CUP_GCEVERY=N: força il2cpp_gc_collect a cada N frames (contém heap Boehm) */
  int gcevery = getenv("CUP_GCEVERY") ? atoi(getenv("CUP_GCEVERY")) : 0;
  int gc_pending = 0, gc_idle = 0;
  for (int f = 0; render && (max_f <= 0 || f < max_f); f++) {
    g_render_frame = f;  /* CUP_DRAWSPY: amarra os draws ao frame */
    if ((env_on("TER_RXD_SPLASHSPY") || env_on("TER_RXD_SPLASHDRIVE")) &&
        f % 30 == 0) rxd_splashspy_install_once();
    if (memlog && f % 150 == 0) {
      static long last_swfree = -1; static int verbose_until = 0;
      long avail = -1, swfree = -1, rss = -1; char ln[128];
      FILE *mi = fopen("/proc/meminfo", "r");
      if (mi) { while (fgets(ln, sizeof ln, mi)) {
          sscanf(ln, "MemAvailable: %ld", &avail); sscanf(ln, "SwapFree: %ld", &swfree); }
        fclose(mi); }
      /* burst de swap (>8MB desde a última amostra) -> amostragem densa por 1200f */
      if (last_swfree >= 0 && last_swfree - swfree > 8192) verbose_until = f + 1200;
      last_swfree = swfree;
      if (f % 600 == 0 || f < verbose_until) {
        FILE *st = fopen("/proc/self/status", "r");
        if (st) { while (fgets(ln, sizeof ln, st)) sscanf(ln, "VmRSS: %ld", &rss); fclose(st); }
        long gch = il2_gc_heap ? il2_gc_heap() : -1;
        long gcu = il2_gc_used ? il2_gc_used() : -1;
        fprintf(stderr, "[MEM] f=%d avail=%ldMB swfree=%ldMB rss=%ldMB gcheap=%ldMB gcused=%ldMB\n",
                f, avail / 1024, swfree / 1024, rss / 1024, gch >> 20, gcu >> 20);
        fsync(2);
      }
    }
    if (getenv("CUP_STAGESPY") && f % 300 == 0) {
      fprintf(stderr, "[STAGESPY] f=%d set_sprite=%u (null=%u) loadAsync=%u\n",
              f, g_ss_set, g_ss_null, g_ss_async); fsync(2);
    }
    if (scenespy && f % 600 == 0) scenespy_dump("tick");
    if (setactive && f % 30 == 0) setactive_fix();
    if (gcevery && f > gcon_f && f % gcevery == 0) gc_pending = 1;
    if (gc_pending) {
      /* limpeza de transição (ideia do usuário): solta assets da cena anterior +
         coleta o heap — sem isso o load da cena nova SOMA com a velha -> burst
         de ~150MB -> swap storm -> device asfixia.
         ⚠️ s12: SÓ com o PreloadManager OCIOSO (preloadQ[+224]==0 e integQ[+256]==0
         por 90 frames seguidos). O tick cego da s11 caiu NO MEIO do load assíncrono
         da cena do mapa (f=10800, rss subindo) e varreu objetos ainda não
         enraizados -> GameObject com scene NULL -> crash 0x541cdc. */
      char *m = (char *)g_preload_mgr;
      int idle = !m || (*(volatile uintptr_t *)(m + 224) == 0 &&
                        *(volatile uintptr_t *)(m + 256) == 0);
      gc_idle = idle ? gc_idle + 1 : 0;
      if (gc_idle >= (m ? 90 : 1200)) {   /* sem mgr capturado: espera ~20s */
        gc_pending = 0; gc_idle = 0;
        fprintf(stderr, "[GCEVERY] limpeza f=%d (mgr %s)\n", f, m ? "ocioso" : "n/d");
        ((void *(*)(void))(g_il2cpp_base + 0x178BAAC))(); /* Resources.UnloadUnusedAssets */
        if (il2_gc_collect) il2_gc_collect(-1);
      }
    }
    { /* log de hits do SCENESKIP + MASKGUARD + NULLGUARD */
      static uint32_t sg_last, mg_last;
      if (g_sceneskip_hits != sg_last) {
        fprintf(stderr, "[SCENESKIP] GO sem scene pulado (%u hits, f=%d) — sobrevivido\n",
                g_sceneskip_hits, f); fsync(2);
        sg_last = g_sceneskip_hits;
        if (scenespy) scenespy_dump("skip");   /* estado do mgr NO momento do problema */
      }
      if (g_maskguard_hits != mg_last) {
        fprintf(stderr, "[MASKGUARD] count insana clampada (%u hits, f=%d) — sobrevivido\n",
                g_maskguard_hits, f); fsync(2);
        mg_last = g_maskguard_hits;
      }
      static uint32_t ng_last;
      if (g_nullguard_hits != ng_last) {
        fprintf(stderr, "[NULLGUARD] arg0 NULL skipado (%u hits, f=%d) — sobrevivido\n",
                g_nullguard_hits, f); fsync(2);
        ng_last = g_nullguard_hits;
      }
    }
    if (gcoff && gcon_f > 0 && f == gcon_f) {
      if (il2_gc_enable) il2_gc_enable();
      if (il2_gc_collect) il2_gc_collect(-1);
      fprintf(stderr, "[GCOFF] GC RELIGADO + collect no frame %d (boot ja passou)\n", f);
      fflush(stderr);
    }
    if (loadyield && f < loadyield_f) { for (int y = 0; y < 4; y++) sched_yield(); usleep(loadyield); }
    if (gamepad_on) gp_poll();   /* drena eventos do js0 ANTES do frame ler input */
    if (drivecr && f >= drivecr_from && g_startcr_it) cr1_tramp(g_startcr_it);
    if (rxd_driveboot && f >= rxd_driveboot_from &&
        ((f - rxd_driveboot_from) % rxd_drivenested_period) == 0) {
      void *cur = rxd_boot_current();
      if (cur && rxd_is_design_cr(cur)) {
        if (g_rxd_design_it != cur) {
          g_rxd_design_it = cur;
          g_rxd_design_done = 0;
        }
        if (!g_rxd_design_done && rxd_design_cr_orig) {
          long r = rxd_design_cr_hook(cur, NULL);
          fprintf(stderr, "[RXD_DRIVENEST] LoadDesignsData MoveNext ret=%ld it=%p f=%d\n",
                  r, cur, f);
          if (!r) g_rxd_design_done = 1;
          fsync(2);
        }
      }
    }
    if (rxd_driveboot && f >= rxd_driveboot_from &&
        ((f - rxd_driveboot_from) % rxd_driveboot_period) == 0) {
      if (g_rxd_boot_start_it && !g_rxd_boot_start_done && rxd_boot_start_cr_orig) {
        void *cur = rxd_boot_current();
        if (cur && rxd_is_design_cr(cur) && !g_rxd_design_done) {
          fprintf(stderr, "[RXD_DRIVEBOOT] Start aguardando nested %p(%s) f=%d\n",
                  cur, il2cpp_classname(cur), f);
        } else {
          long r = rxd_boot_start_cr_hook((void *)g_rxd_boot_start_it, NULL);
          fprintf(stderr, "[RXD_DRIVEBOOT] Start MoveNext ret=%ld it=%p f=%d\n",
                  r, (void *)g_rxd_boot_start_it, f);
          if (!r) g_rxd_boot_start_done = 1;
        }
        fsync(2);
      }
      if (g_rxd_boot_warm_it && !g_rxd_boot_warm_done && rxd_boot_warm_cr_orig) {
        long r = rxd_boot_warm_cr_hook((void *)g_rxd_boot_warm_it, NULL);
        fprintf(stderr, "[RXD_DRIVEBOOT] WarmUp MoveNext ret=%ld it=%p f=%d\n",
                r, (void *)g_rxd_boot_warm_it, f);
        if (!r) g_rxd_boot_warm_done = 1;
        fsync(2);
      }
    }
    if (rxd_driveab && f >= rxd_driveab_from &&
        ((f - rxd_driveab_from) % rxd_driveab_period) == 0) {
      if (g_rxd_manifest_it && !g_rxd_manifest_done && rxd_cr_manifest_orig) {
        long r = rxd_cr_manifest_hook((void *)g_rxd_manifest_it, NULL);
        fprintf(stderr, "[RXD_DRIVEAB] Manifest MoveNext ret=%ld it=%p f=%d\n",
                r, (void *)g_rxd_manifest_it, f);
        if (!r) g_rxd_manifest_done = 1;
        fsync(2);
      }
      if (g_rxd_assets_it && !g_rxd_assets_done && rxd_cr_assets_orig) {
        long r = rxd_cr_assets_hook((void *)g_rxd_assets_it, NULL);
        fprintf(stderr, "[RXD_DRIVEAB] Assets MoveNext ret=%ld it=%p f=%d\n",
                r, (void *)g_rxd_assets_it, f);
        if (!r) g_rxd_assets_done = 1;
        fsync(2);
      }
      {
        int n = g_rxd_asyncasset_n;
        if (n > RXD_ASYNCASSET_MAX) n = RXD_ASYNCASSET_MAX;
        for (int ai = n - 1; ai >= 0; ai--) {
          void *it = (void *)g_rxd_asyncasset_its[ai];
          if (!it || g_rxd_asyncasset_dones[ai]) continue;
          int st = *(int *)((char *)it + 0x10);
          void *cur = *(void **)((char *)it + 0x18);
          if (st == 1 && !rxd_single_is_done(cur)) {
            static unsigned wait_log;
            if (wait_log++ < 120 || (f % 60) == 0) {
              fprintf(stderr, "[RXD_DRIVEAB] AsyncAsset[%d] aguardando child=%p(%s) state=%d f=%d\n",
                      ai, cur, il2cpp_classname(cur),
                      cur ? *(int *)((char *)cur + 0x10) : -999, f);
              fsync(2);
            }
            continue;
          }
          long r = rxd_asyncasset_movenext_runtime(it);
          fprintf(stderr, "[RXD_DRIVEAB] AsyncAsset[%d] MoveNext ret=%ld it=%p f=%d\n",
                  ai, r, it, f);
          if (!r) g_rxd_asyncasset_dones[ai] = 1;
          fsync(2);
        }
      }
      if (rxd_cr_single_orig) {
        int n = g_rxd_single_n;
        if (n > RXD_SINGLE_MAX) n = RXD_SINGLE_MAX;
        for (int si = n - 1; si >= 0; si--) {
          void *it = (void *)g_rxd_single_its[si];
          if (!it || g_rxd_single_dones[si]) continue;
          long r = rxd_cr_single_hook(it, NULL);
          fprintf(stderr, "[RXD_DRIVEAB] Single[%d] MoveNext ret=%ld it=%p f=%d\n",
                  si, r, it, f);
          if (!r) g_rxd_single_dones[si] = 1;
          fsync(2);
        }
      }
    }
    if (rxd_forceaudio && f >= rxd_forceaudio_from) {
      static int forceaudio_logged;
      void *am = rxd_get_audio_mgr_instance();
      if (am) {
        unsigned char *is_all = (unsigned char *)am + 0x21;
        unsigned char *is_sys = (unsigned char *)am + 0x22;
        if (!*is_all || !*is_sys || !forceaudio_logged) {
          fprintf(stderr, "[RXD_AUDIO] FORCE ready self=%p IsInitAll %d->1 IsInitSystemSE %d->1 f=%d\n",
                  am, *is_all, *is_sys, f);
          *is_all = 1;
          *is_sys = 1;
          forceaudio_logged = 1;
          fsync(2);
        }
      } else if (!forceaudio_logged) {
        fprintf(stderr, "[RXD_AUDIO] FORCE aguardando AudioManager.Instance f=%d\n", f);
        forceaudio_logged = 1;
        fsync(2);
      }
    }
    if (rxd_forceconsole && f >= rxd_forceconsole_from) {
      static int forceconsole_logged;
      void *cm = rxd_get_console_mgr_instance();
      if (cm) {
        unsigned char *initialized = (unsigned char *)cm + 0x20;
        unsigned char *ready = (unsigned char *)cm + 0x21;
        if (!*initialized || !*ready || !forceconsole_logged || (f % 120) == 0) {
          fprintf(stderr, "[RXD_CONSOLE] FORCE ready self=%p b20=%d->1 b21=%d->1 b22=%d f=%d\n",
                  cm, *initialized, *ready,
                  *(unsigned char *)((char *)cm + 0x22), f);
          forceconsole_logged = 1;
          fsync(2);
        }
        *initialized = 1;
        *ready = 1;
      } else if (!forceconsole_logged) {
        fprintf(stderr, "[RXD_CONSOLE] FORCE aguardando ConsolePackageManager.Instance f=%d\n", f);
        forceconsole_logged = 1;
        fsync(2);
      }
    }
    if (rxd_forcelocale && f >= rxd_forcelocale_from) {
      static int forcelocale_logged;
      void *lm = rxd_get_localization_mgr_instance();
      if (lm) {
        unsigned char *ready = (unsigned char *)lm + 0x18;
        if (!*ready || !forcelocale_logged || (f % 120) == 0) {
          fprintf(stderr, "[RXD_LOCALE] FORCE ready self=%p b18=%d->1 b20=%d b21=%d b22=%d f=%d\n",
                  lm, *ready,
                  *(unsigned char *)((char *)lm + 0x20),
                  *(unsigned char *)((char *)lm + 0x21),
                  *(unsigned char *)((char *)lm + 0x22), f);
          forcelocale_logged = 1;
          fsync(2);
        }
        *ready = 1;
      } else if (!forcelocale_logged) {
        fprintf(stderr, "[RXD_LOCALE] FORCE aguardando LocalizationManager.Instance f=%d\n", f);
        forcelocale_logged = 1;
        fsync(2);
      }
    }
    if (rxd_loadscene && !rxd_loadscene_done && f >= rxd_loadscene_from &&
        g_rxd_boot_start_done && rxd_usm_loadasync_orig) {
      uint64_t parms = 0;  /* LoadSceneMode.Single + LocalPhysicsMode.None */
      void *op = rxd_usm_loadasync_hook(NULL, rxd_loadscene_index, parms, rxd_loadscene_must, NULL);
      g_rxd_manual_scene_op = op;
      g_rxd_manual_scene_frame = f;
      g_rxd_manual_scene_final_op = NULL;
      g_rxd_manual_scene_final_done = 0;
      if (op && rxd_loadscene_activate && g_il2cpp_base) {
        void (*set_act)(void *, int, void *) = (void *)(g_il2cpp_base + 0x1EBE464);
        set_act(op, 1, NULL);
        fprintf(stderr, "[RXD_LOADSCENE] allowSceneActivation(true) op=%p f=%d\n", op, f);
        if (env_on("TER_RXD_LOADSCENE_ACTIVATE_ICALL")) {
          void *(*resolve_icall)(const char *) =
              (void *(*)(const char *))ter_il2cpp_sym_cached("il2cpp_resolve_icall");
          void (*set_act_icall)(void *, int) = resolve_icall ?
              (void (*)(void *, int))resolve_icall("UnityEngine.AsyncOperation::set_allowSceneActivation(System.Boolean)") : NULL;
          if (set_act_icall) {
            set_act_icall(op, 1);
            fprintf(stderr, "[RXD_LOADSCENE] allowSceneActivation icall(true) op=%p target=%p f=%d\n",
                    op, (void *)set_act_icall, f);
          } else {
            fprintf(stderr, "[RXD_LOADSCENE] allowSceneActivation icall nao resolvido f=%d\n", f);
          }
        }
      }
      fprintf(stderr, "[RXD_LOADSCENE] manual index=%d must=%d -> %p f=%d\n",
              rxd_loadscene_index, rxd_loadscene_must, op, f);
      fsync(2);
      rxd_loadscene_done = 1;
    }
    if (rxd_loadscene_done && g_rxd_manual_scene_op && rxd_loadscene_log_period > 0 &&
        f > g_rxd_manual_scene_frame &&
        ((f - g_rxd_manual_scene_frame) % rxd_loadscene_log_period) == 0) {
      rxd_log_asyncop("[RXD_LOADSCENE] manual op", (void *)g_rxd_manual_scene_op);
      if (scenespy) scenespy_dump("manual-load");
    }
    if (g_rxd_scene_complete_seen && g_rxd_manual_scene_op && !g_rxd_manual_scene_final_done) {
      fprintf(stderr,
              "[RXD_LOADSCENE] stop pump apos ChangeSceneComplete op=%p complete_f=%d f=%d\n",
              (void *)g_rxd_manual_scene_op, g_rxd_scene_complete_frame, f);
      fsync(2);
      g_rxd_manual_scene_final_op = rxd_sceneop_current_target_ptr();
      g_rxd_manual_scene_final_done = 1;
    }
    if ((rxd_loadscene_pump || rxd_loadscene_pump_bg || rxd_loadscene_pump_integ ||
         rxd_loadscene_pump_final || rxd_loadscene_force_state2) &&
        g_rxd_manual_scene_op && !g_rxd_scene_complete_seen && f >= rxd_loadscene_pump_from &&
        ((f - rxd_loadscene_pump_from) % rxd_loadscene_pump_period) == 0) {
      void *op = (void *)g_rxd_manual_scene_op;
      void *ptr = op ? *(void **)((char *)op + 0x10) : NULL;
      uintptr_t vt = ptr ? *(uintptr_t *)ptr : 0;
      if (rxd_scenevt_guard) rxd_sceneop_patch_vtable(vt);
      int state0 = ptr ? *(int *)((char *)ptr + 0x48) : -1;
      int (*bg_fn)(void *) = (vt && rxd_loadscene_pump_bg) ? *(int (**)(void *))(vt + 0x50) : NULL;
      int (*it_fn)(void *) = (vt && rxd_loadscene_pump_integ) ? *(int (**)(void *))(vt + 0x58) : NULL;
      int final_done = (g_rxd_manual_scene_final_done && g_rxd_manual_scene_final_op == ptr);
      void (*fn_fn)(void *) = (vt && rxd_loadscene_pump_final && !final_done && state0 != 2) ?
          *(void (**)(void *))(vt + 0x60) : NULL;
      for (int k = 0; ptr && k < rxd_loadscene_pump_n; k++) {
        int rb = -999, ri = -999, rf = -999;
        int final_called = 0;
        rxd_sceneop_apply_env(ptr);
        if (bg_fn) rb = bg_fn(ptr);
        if (it_fn) ri = it_fn(ptr);
        if (fn_fn && (ri == 1 || !it_fn)) {
          int final_age = (g_rxd_manual_scene_frame >= 0) ? (f - g_rxd_manual_scene_frame) : 999999;
          int native_allow = *(unsigned char *)((char *)ptr + 0x3A4);
          void *queue = *(void **)((char *)ptr + 0xA0);
          int final_ready =
              final_age >= rxd_loadscene_final_delay &&
              (!rxd_loadscene_final_require_allow || native_allow) &&
              (!rxd_loadscene_final_require_queue || queue);
          if (!final_ready) {
            static unsigned final_delay_logs;
            if (final_delay_logs++ < 120 || (f % 60) == 0) {
              fprintf(stderr,
                      "[RXD_LOADSCENE] final adiado ptr=%p fn=%p age=%d/%d allow=%d req_allow=%d queue=%p req_queue=%d ri=%d f=%d\n",
                      ptr, (void *)fn_fn, final_age, rxd_loadscene_final_delay,
                      native_allow, rxd_loadscene_final_require_allow, queue,
                      rxd_loadscene_final_require_queue, ri, f);
              fsync(2);
            }
            fn_fn = NULL;
            rf = -998;
          } else {
          int sf0 = *(int *)((char *)ptr + 0x48);
          float pf0 = *(float *)((char *)ptr + 0x6C);
          int rf0 = *(unsigned char *)((char *)ptr + 0x3A5);
          int af0 = *(unsigned char *)((char *)ptr + 0x3A4);
          int mf0 = *(int *)((char *)ptr + 0x3A0);
          int ff0 = *(unsigned char *)((char *)ptr + 0x3A6);
          void *qf0 = *(void **)((char *)ptr + 0xA0);
          fprintf(stderr,
                  "[RXD_LOADSCENE] final-call pre ptr=%p fn=%p state=%d progress=%.3f mode=%d allow=%d ready=%d flag=%d queue=%p age=%d f=%d\n",
                  ptr, (void *)fn_fn, sf0, pf0, mf0, af0, rf0, ff0, qf0, final_age, f);
          fsync(2);
          fn_fn(ptr);
          final_called = 1;
          int done_after = -1;
          if (g_il2cpp_base) {
            int (*get_done)(void *, void *) = (void *)(g_il2cpp_base + 0x1EBE394);
            if (get_done) done_after = get_done(op, NULL);
          }
          int sf1 = *(int *)((char *)ptr + 0x48);
          float pf1 = *(float *)((char *)ptr + 0x6C);
          int mf1 = *(int *)((char *)ptr + 0x3A0);
          int af1 = *(unsigned char *)((char *)ptr + 0x3A4);
          int rf1 = *(unsigned char *)((char *)ptr + 0x3A5);
          int ff1 = *(unsigned char *)((char *)ptr + 0x3A6);
          void *qf1 = *(void **)((char *)ptr + 0xA0);
          rf = done_after;
          fprintf(stderr,
                  "[RXD_LOADSCENE] final-call post ptr=%p fn=%p done=%d state=%d progress=%.3f mode=%d allow=%d ready=%d flag=%d queue=%p f=%d\n",
                  ptr, (void *)fn_fn, done_after, sf1, pf1, mf1, af1, rf1, ff1, qf1, f);
          fsync(2);
          g_rxd_manual_scene_final_op = ptr;
          g_rxd_manual_scene_final_done = (done_after == 1 || sf1 == 2);
          if (g_rxd_manual_scene_final_done) fn_fn = NULL;
          }
        }
        if (rxd_loadscene_force_state2 && (ri == 1 || final_called || !it_fn)) {
          *(int *)((char *)ptr + 0x48) = 2;
          *(float *)((char *)ptr + 0x6C) = 1.0f;
        }
        fprintf(stderr,
                "[RXD_LOADSCENE] pump k=%d ptr=%p vt=%p bg=%p->%d integ=%p->%d final=%p->%d "
                "state=%d progress=%.3f mode=%d allow=%d ready=%d flag=%d f=%d\n",
                k, ptr, (void *)vt, (void *)bg_fn, rb, (void *)it_fn, ri,
                (void *)fn_fn, rf,
                *(int *)((char *)ptr + 0x48),
                *(float *)((char *)ptr + 0x6C),
                *(int *)((char *)ptr + 0x3A0),
                *(unsigned char *)((char *)ptr + 0x3A4),
                *(unsigned char *)((char *)ptr + 0x3A5),
                *(unsigned char *)((char *)ptr + 0x3A6), f);
        fsync(2);
      }
      rxd_log_asyncop("[RXD_LOADSCENE] after pump", op);
    }
    if (rxd_setactive_managed && f >= rxd_setactive_managed_from &&
        ((f - rxd_setactive_managed_from) % rxd_setactive_managed_period) == 0) {
      rxd_set_active_scene_managed_tick(f);
    }
    if (rxd_regfx && f >= rxd_regfx_from &&
        (!rxd_regfx_last_ret ||
         (g_unity_base && !*(void **)(g_unity_base + 0x182a000 + 3056))) &&
        rxd_regfx_n < rxd_regfx_max &&
        ((f - rxd_regfx_from) % rxd_regfx_period) == 0) {
      void *fnr = jni_find_native("nativeRecreateGfxState");
      if (fnr) {
        rxd_regfx_n++;
        g_anw_gen++;   /* fromSurface devolve janela NOVA -> Unity recria a surface EGL */
        fprintf(stderr, "[RXD_REGFX] re-entrega surface #%d gen=%d f=%d\n",
                rxd_regfx_n, g_anw_gen, f);
        fsync(2);
        ((void (*)(void *, void *, int, void *))fnr)(env, &thiz, 0, &surf);
        if (rxd_native_surface_event)
          ((void (*)(void *, void *))rxd_native_surface_event)(env, &thiz);
        if (rxd_native_resume)
          ((void (*)(void *, void *))rxd_native_resume)(env, &thiz);
        if (rxd_native_focus_changed)
          ((void (*)(void *, void *, int))rxd_native_focus_changed)(env, &thiz, 1);
        fprintf(stderr, "[RXD_REGFX] done #%d f=%d\n", rxd_regfx_n, f);
        fsync(2);
      }
    }
    if (rxd_lifecycle_tick && f >= rxd_lifecycle_from &&
        ((f - rxd_lifecycle_from) % rxd_lifecycle_period) == 0) {
      fprintf(stderr, "[RXD_LIFE] tick f=%d\n", f);
      if (rxd_lifecycle_surface && rxd_native_surface_event) {
        ((void (*)(void *, void *))rxd_native_surface_event)(env, &thiz);
        fprintf(stderr, "[RXD_LIFE] nativeSendSurfaceChangedEvent done f=%d\n", f);
      }
      if (rxd_lifecycle_resume && rxd_native_resume) {
        ((void (*)(void *, void *))rxd_native_resume)(env, &thiz);
        fprintf(stderr, "[RXD_LIFE] nativeResume done f=%d\n", f);
      }
      if (rxd_lifecycle_focus && rxd_native_focus_changed) {
        ((void (*)(void *, void *, int))rxd_native_focus_changed)(env, &thiz, 1);
        fprintf(stderr, "[RXD_LIFE] nativeFocusChanged(1) done f=%d\n", f);
      }
      fsync(2);
    }
    if (rxd_scenedump && f >= rxd_scenedump_from &&
        ((f - rxd_scenedump_from) % rxd_scenedump_period) == 0) {
      rxd_dump_scene_objects_tick(f);
    }
    if (rxd_splashdrive) {
      rxd_splashdrive_tick(f, rxd_splashdrive_from, rxd_splashdrive_period);
    }
    if (rxd_titledrive && g_rxd_title_inst && f >= rxd_titledrive_from) {
      void *title = (void *)g_rxd_title_inst;
      if (rxd_titledrive_sceneinit && !rxd_titledrive_sceneinit_done && rxd_title_sceneinit_orig) {
        fprintf(stderr, "[RXD_TITLEDRIVE] SceneInit title=%p age=%d f=%d\n",
                title, g_rxd_title_awake_frame >= 0 ? f - g_rxd_title_awake_frame : -1, f);
        fsync(2);
        rxd_title_sceneinit_orig(title, NULL);
        fprintf(stderr, "[RXD_TITLEDRIVE] SceneInit done title=%p f=%d\n", title, f);
        fsync(2);
        rxd_titledrive_sceneinit_done = 1;
      }
      if (rxd_titledrive_start && !rxd_titledrive_start_done && f >= rxd_titledrive_from + 1 &&
          rxd_title_start_orig) {
        fprintf(stderr, "[RXD_TITLEDRIVE] Start title=%p age=%d f=%d\n",
                title, g_rxd_title_awake_frame >= 0 ? f - g_rxd_title_awake_frame : -1, f);
        fsync(2);
        rxd_title_start_orig(title, NULL);
        fprintf(stderr, "[RXD_TITLEDRIVE] Start done title=%p f=%d\n", title, f);
        fsync(2);
        rxd_titledrive_start_done = 1;
      }
      if (rxd_titledrive_openui && !rxd_titledrive_openui_done && f >= rxd_titledrive_from + 2 &&
          rxd_title_openui_orig) {
        fprintf(stderr, "[RXD_TITLEDRIVE] TitleOpenUI title=%p age=%d f=%d\n",
                title, g_rxd_title_awake_frame >= 0 ? f - g_rxd_title_awake_frame : -1, f);
        fsync(2);
        rxd_title_openui_orig(title, NULL);
        fprintf(stderr, "[RXD_TITLEDRIVE] TitleOpenUI done title=%p f=%d\n", title, f);
        fsync(2);
        rxd_titledrive_openui_done = 1;
      }
      if (rxd_titledrive_update && rxd_title_update_orig &&
          ((f - rxd_titledrive_from) % rxd_titledrive_update_period) == 0) {
        fprintf(stderr, "[RXD_TITLEDRIVE] Update title=%p age=%d f=%d\n",
                title, g_rxd_title_awake_frame >= 0 ? f - g_rxd_title_awake_frame : -1, f);
        fsync(2);
        rxd_title_update_orig(title, NULL);
      }
    }
    if (rxd_change && !rxd_change_done && f >= rxd_change_from &&
        g_rxd_boot_start_done && rxd_scene_change_orig) {
      void *mgr = (void *)g_rxd_scene_mgr;
      void *(*isn)(const char *) = (void *(*)(const char *))ter_il2cpp_sym_cached("il2cpp_string_new");
      void *scene = (isn && rxd_change_scene) ? isn(rxd_change_scene) : NULL;
      if (mgr && scene) {
        fprintf(stderr, "[RXD_CHANGE] manual scene=%s mgr=%p str=%p f=%d\n",
                rxd_change_scene, mgr, scene, f);
        fsync(2);
        rxd_scene_change_hook(mgr, scene, rxd_change_type, NULL,
                              rxd_change_clear, rxd_change_skip, NULL);
        rxd_change_done = 1;
      } else {
        fprintf(stderr, "[RXD_CHANGE] aguardando mgr/str mgr=%p str=%p f=%d\n", mgr, scene, f);
        fsync(2);
      }
    }
    int rxd_native_change_ready = g_rxd_scene_change_seen &&
        g_rxd_scene_change_frame >= 0 &&
        f >= g_rxd_scene_change_frame + rxd_force_onstart_delay;
    if (rxd_force_onstart && (rxd_change_done || rxd_native_change_ready) && !g_rxd_scene_it &&
        f >= rxd_drivescene_from && g_rxd_scene_mgr && rxd_scene_onchange_orig) {
      fprintf(stderr, "[RXD_SCENECR] force OnStartChangeScene mgr=%p change_done=%d native_seen=%d native_frame=%d f=%d\n",
              (void *)g_rxd_scene_mgr, rxd_change_done, g_rxd_scene_change_seen,
              g_rxd_scene_change_frame, f);
      fsync(2);
      rxd_scene_onchange_hook((void *)g_rxd_scene_mgr, NULL);
    }
    if (rxd_drivescene && g_rxd_scene_it && !g_rxd_scene_done &&
        f >= rxd_drivescene_from && ((f - rxd_drivescene_from) % rxd_drivescene_period) == 0) {
      long r = rxd_runtime_movenext((void *)g_rxd_scene_it, "OnStartChangeScene");
      fprintf(stderr, "[RXD_SCENECR] driver MoveNext ret=%ld it=%p f=%d\n",
              r, (void *)g_rxd_scene_it, f);
      if (!r) g_rxd_scene_done = 1;
      fsync(2);
    }
    if (drainN && g_preload_mgr) {
      for (int k = 0; k < drainN; k++) preload_step(g_preload_mgr, 2, 0x10);
    }
    if (drainWait && g_preload_mgr) {
      /* zera [mgr+0xE0] (flag GfxDevice +224) p/ WaitForAll só checar o integQ [+256]
       * e NÃO pendurar no loop (com MT-off o flag +224 fica setado p/ sempre). */
      if (getenv("CUP_DRAINWAIT_GFX")) *(volatile int *)((char *)g_preload_mgr + 0xE0) = 0;
      wait_all(g_preload_mgr);
    }
    if (f < 200) { fprintf(stderr, "[r%d>\n", f); dbg_sync(); }  /* ENTRA no render */
    if (env_on("TER_CHOREO") && env_on("TER_CHOREO_INLINE")) rxd_choreo_inline_tick(env);
    ter_nuke_methods();
    rxd_keep_rendering_tick(f);
    if (!env_on("TER_RXD_RENDER_GATE_POST")) rxd_force_render_gate_tick(f);
    if (env_on("TER_RXD_UILB_MONITOR") && g_rxd_last_title_ui &&
        (f < 700 || (f % 120) == 0) && (f % 30) == 0) {
      rxd_log_uistate("tick", (void *)g_rxd_last_title_mgr,
                      (void *)g_rxd_last_title_ui,
                      (void *)g_rxd_last_title_clone);
      if (env_on("TER_RXD_UILB_FORCEVISIBLE_EACH")) {
        rxd_force_ui_visible((void *)g_rxd_last_title_mgr,
                             (void *)g_rxd_last_title_ui,
                             (void *)g_rxd_last_title_clone);
      }
    }
    if (env_on("TER_RENDERSTATE") && (f < 24 || (f % 60) == 0)) rxd_log_render_state("pre", f);
    unsigned char render_ret = 0;
    if (g_skipbad) {
      /* arma o recovery: se nativeRender crashar nesta thread, volta aqui e pula o frame */
      if (sigsetjmp(g_render_jmp, 1) == 0) {
        g_render_jmp_armed = 1;
        render_ret = ((unsigned char (*)(void *, void *))render)(env, &thiz);
      } else {
        if (g_recover_n < 80 || (g_recover_n % 60) == 0)
          fprintf(stderr, "[RECOVER] frame %d pulado (crash #%lu na render)\n", f, g_recover_n);
      }
      g_render_jmp_armed = 0;
    } else {
      render_ret = ((unsigned char (*)(void *, void *))render)(env, &thiz);
    }
    rxd_regfx_last_ret = render_ret;
    if (env_on("TER_RXD_RENDER_GATE_POST")) rxd_force_render_gate_tick(f);
    if (rxd_camera_render && f >= rxd_camera_render_from &&
        ((f - rxd_camera_render_from) % rxd_camera_render_period) == 0) {
      rxd_force_camera_render_tick(f);
    }
    if (env_on("TER_GL_TESTRECT")) {
      int sw = 0, sh = 0, old_scissor[4] = {0};
      float old_clear[4] = {0};
      int old_enabled = 0;
      if (vk_gl_begin(&sw, &sh, old_scissor, old_clear, &old_enabled)) {
        vk_rect(sw, sh, 32, 32, sw > 96 ? sw / 5 : sw, sh > 96 ? sh / 5 : sh,
                1.0f, 0.0f, 0.0f, 1.0f);
        vk_gl_end(old_scissor, old_clear, old_enabled);
      }
    }
    ter_screenshot_frame_maybe(f);
    if (env_on("TER_RENDERRET") && (f < 20 || (f % 60) == 0)) {
      fprintf(stderr, "[RENDERRET] f=%d ret=%u draws=%lu clears=%lu\n",
              f, render_ret, g_frame_draws, g_clear_count);
      fsync(2);
    }
    if (env_on("TER_RENDERSTATE") && (f < 24 || (f % 60) == 0)) rxd_log_render_state("post", f);
    if (render_sleep_us > 0) usleep((useconds_t)render_sleep_us);
    else if (!render_ret && render_sleep0_us > 0) usleep((useconds_t)render_sleep0_us);
    if (f < 200) { fprintf(stderr, "<r%d]\n", f); dbg_sync(); }  /* SAIU do render */
    opensles_shim_pump_callbacks();
    /* bombeia eventos SDL (foco/janela) p/ o input do Unity não esfomear */
    SDL_Event ev; while (SDL_PollEvent(&ev)) {}
    /* AUTOTAP: a cada ~90 frames, manda DOWN; ~3 frames depois, UP (1 "toque") */
    if (tapkey && inject && f > 120) {
      int phase = f % 90;
      if (phase == 0 || phase == 3) {
        g_hk_inject.action = (phase == 0) ? 0 : 1;   /* 0=DOWN 1=UP */
        g_hk_inject.keycode = tapkey;
        g_hk_inject.source = 0x501;                  /* gamepad|keyboard */
        g_hk_inject.deviceId = 0; g_hk_inject.repeat = 0; g_hk_inject.flags = 0;
        g_hk_inject.metaState = 0; g_hk_inject.scancode = 0; g_hk_inject.unicode = 0;
        int ir = ((int (*)(void *, void *, void *))inject)(env, &thiz, hk_keyevent_object());
        if (f < 600) fprintf(stderr, "[AUTOTAP] %s key=%d (f=%d) ret=%d\n", phase ? "UP" : "DOWN", tapkey, f, ir);
      }
    }
    if (gamepad_on) gp_frame_end();  /* snapshot p/ edge-detect do GetButtonDown/Up */
    if (f % 60 == 0) { fprintf(stderr, "[render %d]\n", f); dbg_sync(); }
    { /* FPS médio por janela de 600 frames (mede lag do mapa/fases p/ tuning) */
      static struct timespec t0; static int f0 = -1;
      if (f % 600 == 0) {
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        if (f0 >= 0) {
          double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
          if (dt > 0.5)
            fprintf(stderr, "[FPS] f=%d media=%.1f draws/f=%lu kverts/f=%lu lo/f=%lu (janela %d frames / %.1fs)\n",
                    f, (f - f0) / dt,
                    (f > f0) ? g_frame_draws / (unsigned)(f - f0) : 0,
                    (f > f0) ? (g_frame_verts / (unsigned)(f - f0)) / 1000 : 0,
                    (f > f0) ? g_draws_lo / (unsigned)(f - f0) : 0,
                    f - f0, dt);
        }
        t0 = t1; f0 = f; g_frame_draws = 0; g_frame_verts = 0; g_draws_lo = 0;
      }
    }
  }
  fprintf(stderr, "[F2] === render loop terminou ===\n");
  fflush(stderr); dbg_sync();
  _exit(0);  /* hard exit — destrutores do .so crasham no teardown normal */
}
